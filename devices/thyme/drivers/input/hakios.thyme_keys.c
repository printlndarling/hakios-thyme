/*
 * Xiaomi Mi 10 (UMI) side and volume keys. The PM8150 PON block reports
 * power and volume-down; PM8150 GPIO6 reports volume-up. This is based on
 * the working UMI implementation in ios-sm8250.
 *
 * PMIC access is read-only: the only MMIO write issues an extended read on
 * the PMIC arbiter's observer channel. The driver's only effects are button
 * records posted to /dev/hakios-touch.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern void hakios_prng_mix(const void *, size_t) __attribute__((weak));

SYSCTL_DECL(_kern_hakios);

#define UMI_ARB_CORE_PA       0x0c440000UL
#define UMI_ARB_CNFG_PA       0x0c40a000UL
#define UMI_ARB_OBSERVER_PA   0x0e600000UL
#define UMI_PON_PPID          0x0008u
#define UMI_RTC_PPID          0x0060u
#define UMI_GPIO6_PPID        0x00c5u
#define UMI_PON_RT_STATUS     0x0810u
#define UMI_RTC_TYPE          0x6004u
#define UMI_RTC_SUBTYPE       0x6005u
#define UMI_RTC_CTRL          0x6046u
#define UMI_RTC_VAL0          0x6048u
#define UMI_GPIO_RT_STATUS    0xc110u
#define UMI_GPIO_MODE         0xc140u
#define UMI_GPIO_ENABLE       0xc146u
#define UMI_GPIO_SUBTYPE      0xc105u
#define UMI_KEY_POLL_MS       20
#define UMI_NO_APID           0xffffu

#define UMI_GCC_PA            0x00100000UL
#define UMI_GCC_PRNG_CBCR     0x34004u
#define UMI_GCC_PRNG_VOTE     0x52004u
#define UMI_PRNG_PA           0x00793000UL
#define UMI_PRNG_DATA_OUT     0x000u
#define UMI_PRNG_STATUS       0x004u

#define UMI_TLMM_SOUTH_PA     0x0f500000UL
#define UMI_TLMM_SOUTH_SIZE   0x00060000UL
#define UMI_TLMM_NORTH_PA     0x0f900000UL
#define UMI_TLMM_NORTH_SIZE   0x00010000UL
#define UMI_HALL_GPIO_PIN     14u
#define UMI_QUP1_SDA_PIN      4u
#define UMI_QUP1_SCL_PIN      5u
#define UMI_QUP15_SDA_PIN     44u
#define UMI_QUP15_SCL_PIN     45u
#define UMI_QUP16_SDA_PIN     48u
#define UMI_QUP16_SCL_PIN     49u

#define UMI_WDT_PA            0x17c10000UL
#define UMI_WDT_CTL           0x08u
#define UMI_WDT_STS           0x0cu
#define UMI_WDT_BARK          0x10u
#define UMI_WDT_BITE          0x14u

#define ARB_VERSION           0x000
#define ARB_FEATURES          0x004
#define ARB_CHANNEL(n)        (0x900u + 4u * (n))
#define ARB_OWNER(n)          (0x700u + 4u * (n))
#define OBS_COMMAND           0x00
#define OBS_STATUS            0x08
#define OBS_DATA0             0x18
#define OBS_DONE              0x1u
#define OBS_DROPPED           0x4u
#define OBS_ERRORS            0x0eu
#define OBS_READ_LONG         (1u << 27)

static vm_offset_t arb_core, arb_cnfg, arb_observer, south_tlmm, north_tlmm, hall_gpio;
static uint16_t pon_apid = UMI_NO_APID, gpio_apid = UMI_NO_APID, rtc_apid = UMI_NO_APID;
static bool keys_started, rtc_ready;
static int keys_sleep_chan, qrng_ready, hall_closed;
static int wdt_ctl, wdt_sts;
static int fastchg_part, fastchg_vbus_mv, fastchg_vbat_mv, fastchg_ibus_ma, wlc_chip_id, nfc_present;
static uint32_t rtc_boot_seconds;
static uint32_t press_ms = 150, press_gap = 10, press_left;
static thread_call_t press_call;

SYSCTL_INT(_kern_hakios, OID_AUTO, qrng_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &qrng_ready, 0,
    "SM8250 hardware TRNG seeded XNU mc_prng (1=ready)");
SYSCTL_INT(_kern_hakios, OID_AUTO, hall_closed,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &hall_closed, 0,
    "Xiaomi Mi 10S Hall sensor state (0=open, 1=closed)");
SYSCTL_INT(_kern_hakios, OID_AUTO, wdt_ctl,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &wdt_ctl, 0,
    "SM8250 hardware watchdog CTL register (0x17c10008)");
SYSCTL_INT(_kern_hakios, OID_AUTO, wdt_sts,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &wdt_sts, 0,
    "SM8250 hardware watchdog STS register (0x17c1000c)");
SYSCTL_INT(_kern_hakios, OID_AUTO, fastchg_part,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &fastchg_part, 0,
    "Wired 33W charger IC bq25970 part register (0x13)");
SYSCTL_INT(_kern_hakios, OID_AUTO, fastchg_vbus_mv,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &fastchg_vbus_mv, 0,
    "Wired 33W charger IC bq25970 VBUS ADC (mV)");
SYSCTL_INT(_kern_hakios, OID_AUTO, fastchg_vbat_mv,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &fastchg_vbat_mv, 0,
    "Wired 33W charger IC bq25970 VBAT ADC (mV)");
SYSCTL_INT(_kern_hakios, OID_AUTO, fastchg_ibus_ma,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &fastchg_ibus_ma, 0,
    "Wired 33W charger IC bq25970 IBUS ADC (mA)");
SYSCTL_INT(_kern_hakios, OID_AUTO, wlc_chip_id,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &wlc_chip_id, 0,
    "Wireless 30W charger IC idtp9415 chip ID (0x9415)");
SYSCTL_INT(_kern_hakios, OID_AUTO, nfc_present,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &nfc_present, 0,
    "NFC controller nq-nci@0x28 I2C ACK state (1=present)");

static uint32_t
keys_reg(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
keys_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint16_t
keys_find_apid(uint16_t ppid, uint32_t channels)
{
	uint16_t found = UMI_NO_APID;

	for (uint32_t channel = 0; channel < channels; channel++) {
		uint32_t map = keys_reg(arb_core, ARB_CHANNEL(channel));
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		uint32_t owner = keys_reg(arb_cnfg, ARB_OWNER(channel)) & 7u;
		if (found == UMI_NO_APID || owner == 0) {
			found = (uint16_t)channel;
		}
	}
	return found;
}

/* Read one byte through this execution environment's observer channel. */
static bool
keys_read(uint16_t apid, uint16_t address, uint8_t *value)
{
	if (apid == UMI_NO_APID) {
		return false;
	}
	uint32_t offset = 0x80u * apid; /* EE 0 observer, one 0x80-byte window per APID */
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint64_t deadline, now;
		clock_interval_to_deadline(2, NSEC_PER_MSEC, &deadline);
		hakios_spmi_lock();
		__asm__ volatile("dmb oshst" ::: "memory");
		keys_write(arb_observer, offset + OBS_COMMAND,
		    OBS_READ_LONG | ((uint32_t)(address & 0xffu) << 4));
		bool dropped = false;
		do {
			uint32_t status = keys_reg(arb_observer, offset + OBS_STATUS);
			if (status & OBS_DONE) {
				bool okay = !(status & OBS_ERRORS);
				if (okay) {
					*value = (uint8_t)keys_reg(arb_observer, offset + OBS_DATA0);
					hakios_spmi_unlock();
					return true;
				}
				dropped = (status & OBS_DROPPED) != 0;
				hakios_spmi_unlock();
				if (!dropped) {
					return false;
				}
				break;
			}
			now = mach_absolute_time();
		} while (now < deadline);
		if (!dropped) {
			hakios_spmi_unlock();
			return false;
		}
		clock_interval_to_deadline(50, NSEC_PER_USEC, &deadline);
		while (mach_absolute_time() < deadline) {
		}
	}
	return false;
}

static bool
umi_rtc_read(uint32_t *seconds_out)
{
	uint32_t first = 0, second = 0;

	if (rtc_apid == UMI_NO_APID) {
		return false;
	}
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint8_t b[4] = { 0 };

		for (unsigned i = 0; i < 4; i++) {
			if (!keys_read(rtc_apid, (uint16_t)(UMI_RTC_VAL0 + i), &b[i])) {
				return false;
			}
		}
		first = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
		for (unsigned i = 0; i < 4; i++) {
			if (!keys_read(rtc_apid, (uint16_t)(UMI_RTC_VAL0 + i), &b[i])) {
				return false;
			}
		}
		second = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
		if (first == second) {
			*seconds_out = second;
			return true;
		}
	}
	*seconds_out = second;
	return true;
}

static int
sysctl_umi_rtc_seconds(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	uint32_t secs = 0;
	int value;

	if (!rtc_ready || !umi_rtc_read(&secs)) {
		return ENXIO;
	}
	value = (int)secs;
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, umi_rtc_seconds,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_umi_rtc_seconds, "I", "PM8150 SPMI hardware RTC seconds (thyme)");

static void
thyme_qrng_init(void)
{
	vm_offset_t gcc = ml_io_map(UMI_GCC_PA, 0x60000UL);
	vm_offset_t qrng = 0;
	uint32_t words[16];
	uint32_t vote_before, cbcr, distinct = 0;
	bool all_zero = true, all_ones = true;

	if (gcc == 0) {
		hakios_tl("UMI qrng: GCC map failed\n");
		return;
	}
	vote_before = keys_reg(gcc, UMI_GCC_PRNG_VOTE);
	cbcr = keys_reg(gcc, UMI_GCC_PRNG_CBCR);
	keys_write(gcc, UMI_GCC_PRNG_VOTE, vote_before | (1u << 13));
	uint64_t deadline, now;
	clock_interval_to_deadline(2000, NSEC_PER_USEC, &deadline);
	do {
		cbcr = keys_reg(gcc, UMI_GCC_PRNG_CBCR);
		if ((cbcr & 0x80000000u) == 0) {
			break;
		}
		now = mach_absolute_time();
	} while (now < deadline);
	if ((cbcr & 0x80000000u) != 0) {
		keys_write(gcc, UMI_GCC_PRNG_VOTE, vote_before);
		hakios_tl("UMI qrng: GCC_PRNG_AHB_CLK remained halted (%#x)\n", cbcr);
		return;
	}
	qrng = ml_io_map(UMI_PRNG_PA, 0x1000UL);
	if (qrng == 0) {
		keys_write(gcc, UMI_GCC_PRNG_VOTE, vote_before);
		hakios_tl("UMI qrng: PRNG MMIO map failed\n");
		return;
	}
	unsigned collected = 0;
	for (unsigned i = 0; i < 16; i++) {
		clock_interval_to_deadline(2000, NSEC_PER_USEC, &deadline);
		bool avail = false;
		do {
			if (keys_reg(qrng, UMI_PRNG_STATUS) & 1u) {
				avail = true;
				break;
			}
			now = mach_absolute_time();
		} while (now < deadline);
		if (!avail) {
			break;
		}
		words[i] = keys_reg(qrng, UMI_PRNG_DATA_OUT);
		if (words[i] != 0) {
			all_zero = false;
		}
		if (words[i] != 0xffffffffu) {
			all_ones = false;
		}
		bool dup = false;
		for (unsigned j = 0; j < i; j++) {
			if (words[j] == words[i]) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			distinct++;
		}
		collected++;
	}
	keys_write(gcc, UMI_GCC_PRNG_VOTE, vote_before);
	if (collected == 16 && !all_zero && !all_ones && distinct >= 12) {
		if (hakios_prng_mix != NULL) {
			hakios_prng_mix(words, sizeof(words));
		}
		qrng_ready = 1;
		hakios_tl("UMI qrng: SM8250 hardware TRNG seeded XNU mc_prng (64 bytes, 16/16 words, distinct=%u)\n",
		    distinct);
	} else {
		hakios_tl("UMI qrng: insufficient entropy (words=%u distinct=%u)\n", collected, distinct);
	}
	memset(words, 0, sizeof(words));
}

static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(UMI_KEY_POLL_MS, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_post(unsigned key, bool down)
{
	static const uint32_t button[] = {
	    HAKIOS_BUTTON_LOCK, HAKIOS_BUTTON_VOLUME_UP, HAKIOS_BUTTON_VOLUME_DOWN
	};
	hakios_tl("UMI keys: %s %s\n", key == 0 ? "power" : key == 1 ? "volume-up" : "volume-down",
	    down ? "pressed" : "released");
	hakios_touch_post(down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, button[key], 0, 0);
}

static void
keys_thread(__unused void *arg, __unused wait_result_t result)
{
	/* Unknown initial state is observed silently; only subsequent transitions are events. */
	int state[3] = { -1, -1, -1 };
	bool live[3] = { false, false, false };
	unsigned errors = 0, ticks = 0, hall_debounce = 0;
	int hall_candidate = hall_closed;
	bool rtc_tick_checked = !rtc_ready;

	for (;;) {
		uint8_t pon = 0, gpio = 0;
		bool pon_ok = keys_read(pon_apid, UMI_PON_RT_STATUS, &pon);
		bool gpio_ok = keys_read(gpio_apid, UMI_GPIO_RT_STATUS, &gpio);
		int current[3] = { !!(pon & 1u), !(gpio & 1u), !!(pon & 2u) };
		if (!pon_ok || !gpio_ok) {
			if ((errors++ % 250u) == 0) {
				hakios_tl("UMI keys: PMIC read error (PON=%d GPIO6=%d)\n", pon_ok, gpio_ok);
			}
		} else {
			errors = 0;
		}
		for (unsigned key = 0; key < 3; key++) {
			bool okay = key == 1 ? gpio_ok : pon_ok;
			if (!okay) {
				continue;
			}
			if (state[key] < 0) {
				state[key] = current[key];
				live[key] = current[key] == 0;
			} else if (!live[key]) {
				state[key] = current[key];
				if (!current[key]) {
					live[key] = true;
				}
			} else if (state[key] != current[key]) {
				state[key] = current[key];
				keys_post(key, current[key]);
			}
		}
		if (hall_gpio != 0) {
			int cur_closed = (keys_reg(hall_gpio, 0x04) & 1u) == 0 ? 1 : 0;

			if (cur_closed != hall_closed) {
				if (cur_closed == hall_candidate) {
					if (++hall_debounce >= 3) {
						hall_closed = cur_closed;
						hall_debounce = 0;
						hakios_tl("UMI hall: magnetic cover %s\n",
						    hall_closed ? "closed" : "open");
					}
				} else {
					hall_candidate = cur_closed;
					hall_debounce = 1;
				}
			} else {
				hall_debounce = 0;
			}
		}
		if (!rtc_tick_checked && ++ticks >= 100) {
			uint32_t now_secs = 0;

			rtc_tick_checked = true;
			if (umi_rtc_read(&now_secs)) {
				hakios_tl("UMI rtc: tick check t0=%u t1=%u delta=%u s\n",
				    rtc_boot_seconds, now_secs, now_secs - rtc_boot_seconds);
			}
		}
		keys_wait();
	}
}

static void
press_step(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static bool down;
	uint64_t deadline;

	down = !down;
	hakios_tl("UMI keys: press_power: power %s (by the test)\n", down ? "pressed" : "released");
	hakios_touch_post(down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0);
	if (down) {
		clock_interval_to_deadline(press_ms, NSEC_PER_MSEC, &deadline);
	} else if (--press_left > 0) {
		clock_interval_to_deadline(press_gap, NSEC_PER_SEC, &deadline);
	} else {
		return;
	}
	thread_call_enter_delayed(press_call, deadline);
}

static void
press_power_init(void)
{
	uint32_t seconds = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("press_power", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	PE_parse_boot_argn("press_power_ms", &press_ms, sizeof(press_ms));
	PE_parse_boot_argn("press_power_gap", &press_gap, sizeof(press_gap));
	press_left = 2;
	press_call = thread_call_allocate(press_step, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(press_call, deadline);
	hakios_tl("UMI keys: press_power: power will be pressed for %u ms at %u s and %u s\n",
	    press_ms, seconds, seconds + press_gap);
}

static void
keys_spin_us(uint32_t us)
{
	uint64_t deadline;

	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	while (mach_absolute_time() < deadline) {
	}
}

static inline uint32_t
tlmm_cfg_rd(vm_offset_t tlmm, unsigned pin)
{
	return keys_reg(tlmm, pin * 0x1000u);
}

static inline uint32_t
tlmm_io_rd(vm_offset_t tlmm, unsigned pin)
{
	return keys_reg(tlmm, pin * 0x1000u + 4u);
}

static inline void
tlmm_cfg_wr(vm_offset_t tlmm, unsigned pin, uint32_t val)
{
	keys_write(tlmm, pin * 0x1000u, val);
}

static inline void
tlmm_io_wr(vm_offset_t tlmm, unsigned pin, uint32_t val)
{
	keys_write(tlmm, pin * 0x1000u + 4u, val);
}

static void
i2c_line(vm_offset_t tlmm, unsigned pin, bool high)
{
	tlmm_io_wr(tlmm, pin, 0);
	tlmm_cfg_wr(tlmm, pin, 3u | (high ? 0u : 0x200u));
}

static bool
i2c_level(vm_offset_t tlmm, unsigned pin)
{
	return (tlmm_io_rd(tlmm, pin) & 1u) != 0;
}

static void
i2c_half_bit(void)
{
	keys_spin_us(5);
}

/* Take open-drain control of a TLMM I2C pair only if both lines are mapped and idle-high. */
static bool
i2c_bus_take(vm_offset_t tlmm, unsigned bus_id, unsigned sda, unsigned scl,
    uint32_t saved_cfg[2], uint32_t saved_io[2])
{
	if (tlmm == 0) {
		return false;
	}
	uint32_t cfg_sda = tlmm_cfg_rd(tlmm, sda);
	uint32_t io_sda = tlmm_io_rd(tlmm, sda);
	uint32_t cfg_scl = tlmm_cfg_rd(tlmm, scl);
	uint32_t io_scl = tlmm_io_rd(tlmm, scl);

	hakios_tl("UMI i2c: bus%u gpio%u/%u cfg=%#x/%#x io=%#x/%#x\n",
	    bus_id, sda, scl, cfg_sda, cfg_scl, io_sda, io_scl);
	if ((cfg_sda == 0 && io_sda == 0) || (cfg_scl == 0 && io_scl == 0)) {
		hakios_tl("UMI i2c: bus%u gpio%u/%u RAZ/XPU guarded; skipped safely\n", bus_id, sda, scl);
		return false;
	}
	if ((io_sda & 1u) == 0 || (io_scl & 1u) == 0) {
		hakios_tl("UMI i2c: bus%u gpio%u/%u not idle-high; skipped safely\n", bus_id, sda, scl);
		return false;
	}
	saved_cfg[0] = cfg_sda;
	saved_cfg[1] = cfg_scl;
	saved_io[0] = io_sda;
	saved_io[1] = io_scl;
	i2c_line(tlmm, sda, true);
	i2c_line(tlmm, scl, true);
	i2c_half_bit();
	if (!i2c_level(tlmm, sda) || !i2c_level(tlmm, scl)) {
		tlmm_io_wr(tlmm, sda, saved_io[0] & 2u);
		tlmm_io_wr(tlmm, scl, saved_io[1] & 2u);
		tlmm_cfg_wr(tlmm, sda, saved_cfg[0]);
		tlmm_cfg_wr(tlmm, scl, saved_cfg[1]);
		hakios_tl("UMI i2c: bus%u stuck low after release; restored\n", bus_id);
		return false;
	}
	return true;
}

static void
i2c_bus_release(vm_offset_t tlmm, unsigned sda, unsigned scl,
    const uint32_t saved_cfg[2], const uint32_t saved_io[2])
{
	tlmm_io_wr(tlmm, sda, saved_io[0] & 2u);
	tlmm_io_wr(tlmm, scl, saved_io[1] & 2u);
	tlmm_cfg_wr(tlmm, sda, saved_cfg[0]);
	tlmm_cfg_wr(tlmm, scl, saved_cfg[1]);
}

static void
i2c_start(vm_offset_t tlmm, unsigned sda, unsigned scl)
{
	i2c_line(tlmm, sda, true);
	i2c_line(tlmm, scl, true);
	i2c_half_bit();
	i2c_line(tlmm, sda, false);
	i2c_half_bit();
	i2c_line(tlmm, scl, false);
	i2c_half_bit();
}

static void
i2c_stop(vm_offset_t tlmm, unsigned sda, unsigned scl)
{
	i2c_line(tlmm, sda, false);
	i2c_half_bit();
	i2c_line(tlmm, scl, true);
	i2c_half_bit();
	i2c_line(tlmm, sda, true);
	i2c_half_bit();
}

static bool
i2c_bit(vm_offset_t tlmm, unsigned sda, unsigned scl, bool out)
{
	bool in;

	i2c_line(tlmm, sda, out);
	i2c_half_bit();
	i2c_line(tlmm, scl, true);
	for (unsigned stretched = 0; !i2c_level(tlmm, scl) && stretched < 100; stretched++) {
		i2c_half_bit();
	}
	i2c_half_bit();
	in = i2c_level(tlmm, sda);
	i2c_line(tlmm, scl, false);
	return in;
}

static bool
i2c_write_byte(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		(void)i2c_bit(tlmm, sda, scl, ((byte >> bit) & 1u) != 0);
	}
	return !i2c_bit(tlmm, sda, scl, true);
}

static uint8_t
i2c_read_byte(vm_offset_t tlmm, unsigned sda, unsigned scl, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)((byte << 1) | (i2c_bit(tlmm, sda, scl, true) ? 1u : 0u));
	}
	(void)i2c_bit(tlmm, sda, scl, !more);
	return byte;
}

/* Strictly read-only I2C register block read (8-bit or 16-bit BE register address). */
static bool
i2c_read_regs(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t addr7, uint16_t reg, bool reg16_be,
    uint8_t *buf, unsigned count)
{
	bool ok;

	i2c_start(tlmm, sda, scl);
	ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)(addr7 << 1));
	if (ok && reg16_be) {
		ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)(reg >> 8));
	}
	if (ok) {
		ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)(reg & 0xffu));
	}
	if (ok) {
		i2c_start(tlmm, sda, scl);
		ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u));
	}
	for (unsigned i = 0; ok && i < count; i++) {
		buf[i] = i2c_read_byte(tlmm, sda, scl, i + 1 < count);
	}
	i2c_stop(tlmm, sda, scl);
	return ok;
}

/* Non-destructive I2C address presence check via read address + 1-byte NACK + STOP. */
static bool
i2c_probe_addr(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t addr7)
{
	bool ok;

	i2c_start(tlmm, sda, scl);
	ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u));
	if (ok) {
		(void)i2c_read_byte(tlmm, sda, scl, false);
	}
	i2c_stop(tlmm, sda, scl);
	return ok;
}

/* Read-only SM8250 watchdog status probe (qcom,wdt@17c10000). */
static void
thyme_wdt_probe(void)
{
	vm_offset_t wdt = ml_io_map(UMI_WDT_PA, 0x1000UL);

	if (wdt == 0) {
		hakios_tl("UMI wdt: map failed\n");
		return;
	}
	uint32_t ctl = keys_reg(wdt, UMI_WDT_CTL);
	uint32_t sts = keys_reg(wdt, UMI_WDT_STS);
	uint32_t bark = keys_reg(wdt, UMI_WDT_BARK);
	uint32_t bite = keys_reg(wdt, UMI_WDT_BITE);

	wdt_ctl = (int)ctl;
	wdt_sts = (int)sts;
	hakios_tl("UMI wdt: 0x17c10000 ctl=%#x sts=%#x bark=%u bite=%u\n", ctl, sts, bark, bite);
}

/* Strictly read-only I2C telemetry probe for wired 33W (bq25970), wireless 30W (idtp9415/ln8282) & NFC (nq-nci). */
static void
thyme_charger_probe(void)
{
	uint32_t saved_cfg[2], saved_io[2];

	if (i2c_bus_take(south_tlmm, 15, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, saved_cfg, saved_io)) {
		uint8_t part = 0, adc[10] = { 0 }, fsa_id = 0;

		if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x66u, 0x13u, false, &part, 1)) {
			fastchg_part = (int)part;
			if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x66u, 0x16u, false, adc, sizeof(adc))) {
				int ibus = (int)(((uint16_t)adc[0] << 8) | adc[1]);
				int vbus = (int)(((uint16_t)adc[2] << 8) | adc[3]);
				int vac  = (int)(((uint16_t)adc[4] << 8) | adc[5]);
				int vbat = (int)(((uint16_t)adc[8] << 8) | adc[9]);

				fastchg_ibus_ma = ibus;
				fastchg_vbus_mv = vbus;
				fastchg_vbat_mv = vbat;
				hakios_tl("UMI fastchg: bq25970@0x66 part=%#x vbus=%d mV vac=%d mV vbat=%d mV ibus=%d mA\n",
				    part, vbus, vac, vbat, ibus);
			} else {
				hakios_tl("UMI fastchg: bq25970@0x66 part=%#x (ADC read failed)\n", part);
			}
		} else {
			hakios_tl("UMI fastchg: bq25970@0x66 NACK\n");
		}
		if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x00u, false, &fsa_id, 1)) {
			hakios_tl("UMI usbc_audio: fsa4480@0x42 id=%#x\n", fsa_id);
		}
		i2c_bus_release(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, saved_cfg, saved_io);
	}

	if (i2c_bus_take(south_tlmm, 16, UMI_QUP16_SDA_PIN, UMI_QUP16_SCL_PIN, saved_cfg, saved_io)) {
		uint8_t idt[4] = { 0 }, ln_id = 0;

		if (i2c_read_regs(south_tlmm, UMI_QUP16_SDA_PIN, UMI_QUP16_SCL_PIN, 0x3bu, 0x0000u, true, idt, sizeof(idt))) {
			uint16_t chip = (uint16_t)idt[0] | ((uint16_t)idt[1] << 8);

			wlc_chip_id = (int)chip;
			hakios_tl("UMI wlc: idtp9415@0x3b chip=%#04x rev=%#x cust=%#x\n",
			    chip, idt[2], idt[3]);
		} else {
			hakios_tl("UMI wlc: idtp9415@0x3b NACK\n");
		}
		if (i2c_read_regs(south_tlmm, UMI_QUP16_SDA_PIN, UMI_QUP16_SCL_PIN, 0x55u, 0x00u, false, &ln_id, 1)) {
			hakios_tl("UMI wlc: ln8282@0x55 id=%#x\n", ln_id);
		}
		i2c_bus_release(south_tlmm, UMI_QUP16_SDA_PIN, UMI_QUP16_SCL_PIN, saved_cfg, saved_io);
	}

	north_tlmm = ml_io_map(UMI_TLMM_NORTH_PA, UMI_TLMM_NORTH_SIZE);
	if (i2c_bus_take(north_tlmm, 1, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io)) {
		bool ok = i2c_probe_addr(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, 0x28u);

		nfc_present = ok ? 1 : 0;
		hakios_tl("UMI nfc: nq-nci@0x28 %s\n", ok ? "ACK" : "NACK");
		i2c_bus_release(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io);
	}
}

void
umi_keys_init(void)
{
	uint32_t enabled = 0;
	DTEntry chosen;
	const void *board;
	unsigned size;
	thread_t thread;

	press_power_init();

    if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
        /* umi_keys=1: enable the PM8150 power and volume keys when the generic buttons arg is absent */
        (void)PE_parse_boot_argn("umi_keys", &enabled, sizeof(enabled));
    }
    if (keys_started || enabled != 1) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-board", &board, &size) != kSuccess ||
	    size != 6 || memcmp(board, "thyme", 5) != 0) {
		hakios_tl("UMI keys: board mismatch; driver not started\n");
		return;
	}
	keys_started = true;
	arb_core = ml_io_map(UMI_ARB_CORE_PA, 0x2000);
	arb_cnfg = ml_io_map(UMI_ARB_CNFG_PA, 0x1000);
	arb_observer = ml_io_map(UMI_ARB_OBSERVER_PA, 0x10000);
	if (!arb_core || !arb_cnfg || !arb_observer) {
		hakios_tl("UMI keys: PMIC arbiter mapping failed\n");
		return;
	}
	uint32_t version = keys_reg(arb_core, ARB_VERSION);
	uint32_t channels = keys_reg(arb_core, ARB_FEATURES) & 0x7ffu;
	if (version < 0x50000000u || version >= 0x70000000u || channels == 0 || channels > 512) {
		hakios_tl("UMI keys: unsupported PMIC arbiter version=%#x channels=%u\n", version, channels);
		return;
	}
	pon_apid = keys_find_apid(UMI_PON_PPID, channels);
	rtc_apid = keys_find_apid(UMI_RTC_PPID, channels);
	gpio_apid = keys_find_apid(UMI_GPIO6_PPID, channels);
	uint8_t mode = 0, enable = 0, subtype = 0;
	bool mode_ok = keys_read(gpio_apid, UMI_GPIO_MODE, &mode);
	bool enable_ok = keys_read(gpio_apid, UMI_GPIO_ENABLE, &enable);
	bool subtype_ok = keys_read(gpio_apid, UMI_GPIO_SUBTYPE, &subtype);
	unsigned direction = subtype >= 0x10u && subtype <= 0x13u ? mode & 3u : (mode >> 4) & 7u;
	if (!mode_ok || !enable_ok || !subtype_ok || subtype < 0x10u || subtype > 0x13u ||
	    !(enable & 0x80u) || (direction != 0 && direction != 2)) {
		hakios_tl("UMI keys: GPIO6 is not confirmed enabled as input; volume-up withheld\n");
		gpio_apid = UMI_NO_APID;
	}
	hakios_tl("UMI keys: arbiter=%#x channels=%u PON-APID=%u GPIO6-APID=%u\n",
	    version, channels, pon_apid, gpio_apid);
	thyme_qrng_init();
	{
		static const uint16_t rtc_candidates[] = { 0x0060u, 0x0061u, 0x0062u };

		for (unsigned c = 0; c < sizeof(rtc_candidates) / sizeof(rtc_candidates[0]); c++) {
			uint16_t ap = keys_find_apid(rtc_candidates[c], channels);
			uint8_t rtype = 0, rsub = 0, rctrl = 0;

			if (ap == UMI_NO_APID) {
				continue;
			}
			if (keys_read(ap, UMI_RTC_TYPE, &rtype) &&
			    keys_read(ap, UMI_RTC_SUBTYPE, &rsub) &&
			    keys_read(ap, UMI_RTC_CTRL, &rctrl)) {
				hakios_tl("UMI rtc: candidate ppid=%#x apid=%u type=%#x subtype=%#x ctrl=%#x\n",
				    rtc_candidates[c], ap, rtype, rsub, rctrl);
				if ((rtype == 0x07u || rtype == 0x06u) && rsub == 0x01u && (rctrl & 0x80u) != 0) {
					rtc_apid = ap;
					if (umi_rtc_read(&rtc_boot_seconds)) {
						rtc_ready = true;
						hakios_tl("UMI rtc: PM8150 RTC ready ppid=%#x apid=%u seconds=%u\n",
						    rtc_candidates[c], rtc_apid, rtc_boot_seconds);
						break;
					}
				}
			}
		}
	}
	south_tlmm = ml_io_map(UMI_TLMM_SOUTH_PA, UMI_TLMM_SOUTH_SIZE);
	hall_gpio = south_tlmm != 0 ? (south_tlmm + (UMI_HALL_GPIO_PIN * 0x1000UL)) : 0;
	if (hall_gpio != 0) {
		uint32_t cfg0 = keys_reg(hall_gpio, 0x00);
		uint32_t io = keys_reg(hall_gpio, 0x04);

		if (cfg0 == 0 && io == 0) {
			/* cfg=0/io=0 on GPIO14 (RAZ/unconfigured); disable polling rather than false-closed. */
			hakios_tl("UMI hall: TLMM SOUTH GPIO14 cfg=0 io=0 (unconfigured/RAZ; polling disabled)\n");
			hall_gpio = 0;
		} else {
			hall_closed = (io & 1u) == 0 ? 1 : 0;
			hakios_tl("UMI hall: TLMM SOUTH GPIO14 cfg=%#x io=%#x state=%s\n",
			    cfg0, io, hall_closed ? "closed" : "open");
		}
	}
	thyme_wdt_probe();
	thyme_charger_probe();
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		hakios_tl("UMI keys: kernel_thread_start failed %d\n", kr);
	}
}

/* The common input core calls this hook when CONFIG_HAKIOS_THYME_INPUT replaces BUTTONS. */
void
hakios_buttons_init(void)
{
	umi_keys_init();
}
