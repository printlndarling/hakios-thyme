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
#include <kern/kalloc.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern void hakios_prng_mix(const void *, size_t) __attribute__((weak));
extern int hakios_ufs_read_partition(const char *, uint64_t, uint8_t *, uint64_t, uint64_t *) __attribute__((weak));

extern bool hakios_thyme_cdsp_smp2p(uint32_t *) __attribute__((weak));
extern bool hakios_thyme_cdsp_publish(void) __attribute__((weak));
extern void hakios_thyme_cdsp_kick(void) __attribute__((weak));
extern void hakios_thyme_cdsp_edge_summary(void) __attribute__((weak));

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
#define UMI_TLMM_NORTH_SIZE   0x00080000UL
#define UMI_HALL_GPIO_PIN     14u
#define UMI_QUP1_SDA_PIN      4u
#define UMI_QUP1_SCL_PIN      5u
#define UMI_QUP15_SDA_PIN     44u
#define UMI_QUP15_SCL_PIN     45u
#define UMI_QUP16_SDA_PIN     48u
#define UMI_QUP16_SCL_PIN     49u
#define UMI_NFC_VEN_PIN       83u
#define UMI_NFC_FIRM_PIN      110u
#define UMI_NFC_IRQ_PIN       111u
#define UMI_IR_MOSI_PIN       116u
#define UMI_FOD_PWR_PIN       10u
#define UMI_FOD_IRQ_PIN       23u
#define UMI_FOD_RST_PIN       24u

#define UMI_WDT_PA            0x17c10000UL
#define UMI_WDT_RST           0x04u
#define UMI_WDT_CTL           0x08u
#define UMI_WDT_STS           0x0cu
#define UMI_WDT_BARK          0x10u
#define UMI_WDT_BITE          0x14u
#define UMI_USB3_DWC3_PA      0x0a60c000UL

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

static vm_offset_t arb_core, arb_cnfg, arb_observer, south_tlmm, north_tlmm, hall_gpio, wdt_base;
static uint16_t pon_apid = UMI_NO_APID, gpio_apid = UMI_NO_APID, rtc_apid = UMI_NO_APID;
static bool keys_started, rtc_ready;
static int keys_sleep_chan, qrng_ready, hall_closed;
static int wdt_ctl, wdt_sts, wdt_armed, wdt_pets;
static int venus_fw_size, cdsp_fw_size, venus_ready, venus_segments, cdsp_ready, cdsp_segments;
static int cdsp_smp2p_state = -1, cdsp_smp2p_fresh_ready;
static unsigned cdsp_probe_ticks;
static bool cdsp_saw_not_ready;

#define CDSP_SMP2P_FATAL (1u << 0)
#define CDSP_SMP2P_READY (1u << 1)
#define CDSP_SMP2P_POLL_TICKS (2000u / UMI_KEY_POLL_MS)
static int fastchg_part, fastchg_vbus_mv, fastchg_vbat_mv, fastchg_ibus_ma, wlc_chip_id;
static int nfc_present, nfc_nci_ver, nfc_dev_ready, usbc_audio_id, usbc_switch_status, ir_ready, ir_tx_count;
static int usb_dwc3_id, usb_dwc3_dsts;
static uint32_t nfc_saved_ven_cfg, nfc_saved_ven_io, nfc_saved_firm_cfg, nfc_saved_firm_io;
static bool nfc_saved_valid;
static uint32_t rtc_boot_seconds;
static int64_t umi_rtc_epoch_offset;
static bool umi_rtc_synced;
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
SYSCTL_INT(_kern_hakios, OID_AUTO, wdt_armed,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &wdt_armed, 0,
    "SM8250 hardware watchdog active armed state (1=armed)");
SYSCTL_INT(_kern_hakios, OID_AUTO, wdt_pets,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &wdt_pets, 0,
    "SM8250 hardware watchdog periodic pet count");
SYSCTL_INT(_kern_hakios, OID_AUTO, venus_fw_size,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &venus_fw_size, 0,
    "Size in bytes of venus.mdt/mbn found in UFS modem_a/image");
SYSCTL_INT(_kern_hakios, OID_AUTO, cdsp_fw_size,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &cdsp_fw_size, 0,
    "Size in bytes of cdsp.mdt/mbn found in UFS modem_a/image");
SYSCTL_INT(_kern_hakios, OID_AUTO, venus_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &venus_ready, 0,
    "Venus VPU TrustZone PIL-TZ authenticated state (1=up)");
SYSCTL_INT(_kern_hakios, OID_AUTO, venus_segments,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &venus_segments, 0,
    "Number of loadable ELF segments loaded into pil_video_region");
SYSCTL_INT(_kern_hakios, OID_AUTO, cdsp_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &cdsp_ready, 0,
    "CDSP TrustZone PAS authentication and reset returned success");
SYSCTL_INT(_kern_hakios, OID_AUTO, cdsp_smp2p_state,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &cdsp_smp2p_state, 0,
    "Last CDSP slave-kernel SMP2P state (-1 if unavailable)");
SYSCTL_INT(_kern_hakios, OID_AUTO, cdsp_smp2p_fresh_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &cdsp_smp2p_fresh_ready, 0,
    "Observed a fresh CDSP SMP2P ready indication after PAS authentication");
SYSCTL_INT(_kern_hakios, OID_AUTO, cdsp_segments,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &cdsp_segments, 0,
    "Number of loadable ELF segments loaded into pil_cdsp_region");
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
SYSCTL_INT(_kern_hakios, OID_AUTO, nfc_nci_ver,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &nfc_nci_ver, 0,
    "NFC controller NCI protocol version from CORE_RESET");
SYSCTL_INT(_kern_hakios, OID_AUTO, nfc_dev_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &nfc_dev_ready, 0,
    "NFC NCI character device /dev/hakios-nfc ready state");
SYSCTL_INT(_kern_hakios, OID_AUTO, usb_dwc3_id,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &usb_dwc3_id, 0,
    "Synopsys DWC3 GSNPSID register");
SYSCTL_INT(_kern_hakios, OID_AUTO, usb_dwc3_dsts,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &usb_dwc3_dsts, 0,
    "Synopsys DWC3 DSTS device status register");
SYSCTL_INT(_kern_hakios, OID_AUTO, usbc_audio_id,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &usbc_audio_id, 0,
    "USB-C audio switch fsa4480@0x42 chip ID register (0x00)");
SYSCTL_INT(_kern_hakios, OID_AUTO, usbc_switch_status,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &usbc_switch_status, 0,
    "USB-C audio switch fsa4480@0x42 status registers (st1<<8 | st0)");
SYSCTL_INT(_kern_hakios, OID_AUTO, ir_ready,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &ir_ready, 0,
    "IR blaster gpio116 NEC TX /dev/hakios-ir status (1=ready)");
SYSCTL_INT(_kern_hakios, OID_AUTO, ir_tx_count,
    CTLFLAG_RD | CTLFLAG_ANYBODY, &ir_tx_count, 0,
    "Number of IR NEC frames transmitted via /dev/hakios-ir");

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

static int
sysctl_umi_rtc_unix_time(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	uint32_t secs = 0;
	uint64_t value;

	if (!rtc_ready || !umi_rtc_synced || !umi_rtc_read(&secs)) {
		return ENXIO;
	}
	value = (uint64_t)((int64_t)secs + umi_rtc_epoch_offset);
	return SYSCTL_OUT(req, &value, sizeof(value));
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, umi_rtc_unix_time,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_umi_rtc_unix_time, "Q", "PM8150 SPMI RTC synchronized Unix epoch seconds (thyme)");

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
	hakios_tl("UMI qce: gcc_ce1 cbcr=%#x/%#x/%#x/%#x\n",
	    keys_reg(gcc, 0x41004u), keys_reg(gcc, 0x41008u),
	    keys_reg(gcc, 0x4100cu), keys_reg(gcc, 0x41010u));
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
thyme_cdsp_poll(void)
{
	uint32_t state;

	if (cdsp_probe_ticks == 0) {
		return;
	}
	cdsp_probe_ticks--;
	if (hakios_thyme_cdsp_smp2p(&state)) {
		if ((uint32_t)cdsp_smp2p_state != state) {
			cdsp_smp2p_state = (int)state;
			hakios_tl("UMI cdsp: SMP2P state=%#x\n", state);
		}
		if ((state & CDSP_SMP2P_FATAL) != 0) {
			cdsp_probe_ticks = 0;
			hakios_tl("UMI cdsp: SMP2P reports fatal\n");
			return;
		}
		if ((state & CDSP_SMP2P_READY) == 0) {
			cdsp_saw_not_ready = true;
		} else if (cdsp_saw_not_ready) {
			cdsp_probe_ticks = 0;
			cdsp_smp2p_fresh_ready = 1;
			hakios_tl("UMI cdsp: SMP2P ready after PAS, state=%#x\n", state);
			if (hakios_thyme_cdsp_edge_summary != NULL) {
				hakios_thyme_cdsp_edge_summary();
			}
			return;
		}
	}
	if (cdsp_probe_ticks == 0) {
		hakios_tl("UMI cdsp: SMP2P fresh ready timed out (last=%#x)\n", (uint32_t)cdsp_smp2p_state);
		if (hakios_thyme_cdsp_edge_summary != NULL) {
			hakios_thyme_cdsp_edge_summary();
		}
	}
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

static void thyme_qup15_poll(bool initial);
static void thyme_modem_fat_scan(void);
static void thyme_venus_pil_boot(void);
int hakios_modem_fat_read(const char *image_file, uint8_t *dst, uint32_t max_len, uint32_t *out_len);

static void
keys_thread(__unused void *arg, __unused wait_result_t result)
{
	/* Unknown initial state is observed silently; only subsequent transitions are events. */
	int state[3] = { -1, -1, -1 };
	bool live[3] = { false, false, false };
	unsigned errors = 0, ticks = 0, wdt_ticks = 0, chg_ticks = 0, hall_debounce = 0;
	int hall_candidate = hall_closed;
	bool rtc_tick_checked = !rtc_ready, ufs_fw_checked = false;

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
		if (!ufs_fw_checked && ticks >= 100u) {
			ufs_fw_checked = true;
			thyme_modem_fat_scan();
			thyme_venus_pil_boot();
		}
		thyme_cdsp_poll();
		if (wdt_armed && wdt_base != 0 && ++wdt_ticks >= 200u) {
			wdt_ticks = 0;
			uint32_t sts_before = keys_reg(wdt_base, UMI_WDT_STS);

			keys_write(wdt_base, UMI_WDT_RST, 1u);
			uint32_t sts_after = keys_reg(wdt_base, UMI_WDT_STS);

			wdt_sts = (int)sts_after;
			wdt_pets++;
			if (wdt_pets <= 3) {
				hakios_tl("UMI wdt: pet #%d sts %#x -> %#x\n", wdt_pets, sts_before, sts_after);
			}
		}
		if (south_tlmm != 0 && fastchg_part != 0 && ++chg_ticks >= 500u) {
			chg_ticks = 0;
			thyme_qup15_poll(false);
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

	if (bus_id != 0) {
		hakios_tl("UMI i2c: bus%u gpio%u/%u cfg=%#x/%#x io=%#x/%#x\n",
		    bus_id, sda, scl, cfg_sda, cfg_scl, io_sda, io_scl);
	}
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

static bool
i2c_write_raw(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t addr7,
    const uint8_t *buf, unsigned count)
{
	bool ok;

	i2c_start(tlmm, sda, scl);
	ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)(addr7 << 1));
	for (unsigned i = 0; ok && i < count; i++) {
		ok = i2c_write_byte(tlmm, sda, scl, buf[i]);
	}
	i2c_stop(tlmm, sda, scl);
	return ok;
}

static bool
i2c_probe_write_addr(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t addr7)
{
	bool ok;

	i2c_start(tlmm, sda, scl);
	ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)(addr7 << 1));
	i2c_stop(tlmm, sda, scl);
	return ok;
}

static bool
i2c_read_raw(vm_offset_t tlmm, unsigned sda, unsigned scl, uint8_t addr7,
    uint8_t *buf, unsigned count)
{
	bool ok;

	i2c_start(tlmm, sda, scl);
	ok = i2c_write_byte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u));
	for (unsigned i = 0; ok && i < count; i++) {
		buf[i] = i2c_read_byte(tlmm, sda, scl, i + 1 < count);
	}
	i2c_stop(tlmm, sda, scl);
	return ok;
}

static bool
nfc_read_packet(uint8_t *buf, unsigned max_len, unsigned *out_len)
{
	unsigned plen;

	if (north_tlmm == 0 || max_len < 3u) {
		return false;
	}
	if (!i2c_read_raw(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, 0x28u, buf, 3u)) {
		return false;
	}
	plen = buf[2];
	if (plen > max_len - 3u) {
		plen = max_len - 3u;
	}
	if (plen > 0 &&
	    !i2c_read_raw(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, 0x28u, buf + 3, plen)) {
		return false;
	}
	if (out_len != NULL) {
		*out_len = 3u + plen;
	}
	return true;
}

static int
nfc_dev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	if (north_tlmm == 0 || !nfc_present) {
		return ENODEV;
	}
	if (nfc_saved_valid) {
		tlmm_io_wr(north_tlmm, UMI_NFC_FIRM_PIN, 0);
		tlmm_cfg_wr(north_tlmm, UMI_NFC_FIRM_PIN, (nfc_saved_firm_cfg & ~0x3cu) | 0x200u);
		tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, 0);
		tlmm_cfg_wr(north_tlmm, UMI_NFC_VEN_PIN, (nfc_saved_ven_cfg & ~0x3cu) | 0x200u);
		delay_for_interval(10, NSEC_PER_MSEC);
		tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, 2u);
		delay_for_interval(25, NSEC_PER_MSEC);
	}
	return 0;
}

static int
nfc_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	if (north_tlmm != 0 && nfc_saved_valid) {
		tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, nfc_saved_ven_io & 2u);
		tlmm_cfg_wr(north_tlmm, UMI_NFC_VEN_PIN, nfc_saved_ven_cfg);
		tlmm_io_wr(north_tlmm, UMI_NFC_FIRM_PIN, nfc_saved_firm_io & 2u);
		tlmm_cfg_wr(north_tlmm, UMI_NFC_FIRM_PIN, nfc_saved_firm_cfg);
	}
	return 0;
}

static int
nfc_dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t buf[260];
	uint32_t saved_cfg[2], saved_io[2];
	user_ssize_t resid = uio_resid(uio);
	unsigned len;
	int err;
	bool ok;

	if (north_tlmm == 0 || !nfc_present) {
		return ENODEV;
	}
	if (resid <= 0 || resid > 258) {
		return EINVAL;
	}
	len = (unsigned)resid;
	err = uiomove((caddr_t)buf, (int)len, uio);
	if (err != 0) {
		return err;
	}
	if (!i2c_bus_take(north_tlmm, 1, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io)) {
		return EBUSY;
	}
	ok = i2c_write_raw(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, 0x28u, buf, len);
	i2c_bus_release(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io);
	return ok ? 0 : EIO;
}

static int
nfc_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t buf[260];
	uint32_t saved_cfg[2], saved_io[2];
	user_ssize_t resid = uio_resid(uio);
	unsigned pkt_len = 0, copy_len;
	bool ok;

	if (north_tlmm == 0 || !nfc_present) {
		return ENODEV;
	}
	if (resid < 3) {
		return EINVAL;
	}
	for (unsigned w = 0; w < 200 && (tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) == 0; w++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if ((tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) == 0) {
		return EAGAIN;
	}
	if (!i2c_bus_take(north_tlmm, 1, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io)) {
		return EBUSY;
	}
	ok = nfc_read_packet(buf, sizeof(buf), &pkt_len);
	i2c_bus_release(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io);
	if (!ok || pkt_len == 0) {
		return EIO;
	}
	copy_len = pkt_len;
	if ((user_ssize_t)copy_len > resid) {
		copy_len = (unsigned)resid;
	}
	return uiomove((caddr_t)buf, (int)copy_len, uio);
}

static const struct cdevsw nfc_cdevsw = {
	.d_open = nfc_dev_open, .d_close = nfc_dev_close, .d_read = nfc_dev_read, .d_write = nfc_dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0,
};

static void
ir_carrier_us(unsigned us)
{
	unsigned cycles = us / 26u;

	for (unsigned c = 0; c < cycles; c++) {
		tlmm_io_wr(north_tlmm, UMI_IR_MOSI_PIN, 2u);
		keys_spin_us(9);
		tlmm_io_wr(north_tlmm, UMI_IR_MOSI_PIN, 0u);
		keys_spin_us(17);
	}
}

static void
ir_send_nec(uint32_t code)
{
	uint32_t saved_cfg, saved_io;

	if (north_tlmm == 0 || !ir_ready) {
		return;
	}
	saved_cfg = tlmm_cfg_rd(north_tlmm, UMI_IR_MOSI_PIN);
	saved_io = tlmm_io_rd(north_tlmm, UMI_IR_MOSI_PIN);
	tlmm_io_wr(north_tlmm, UMI_IR_MOSI_PIN, 0u);
	tlmm_cfg_wr(north_tlmm, UMI_IR_MOSI_PIN, (saved_cfg & ~0x3cu) | 0x200u);

	ir_carrier_us(9000);
	keys_spin_us(4500);
	for (unsigned bit = 0; bit < 32; bit++) {
		ir_carrier_us(562);
		keys_spin_us(((code >> bit) & 1u) ? 1687u : 562u);
	}
	ir_carrier_us(562);

	tlmm_io_wr(north_tlmm, UMI_IR_MOSI_PIN, saved_io & 2u);
	tlmm_cfg_wr(north_tlmm, UMI_IR_MOSI_PIN, saved_cfg);
	ir_tx_count++;
}

static int
ir_dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t code = 0;
	int err;

	if (uio_resid(uio) < (user_ssize_t)sizeof(code)) {
		return EINVAL;
	}
	err = uiomove((caddr_t)&code, sizeof(code), uio);
	if (err == 0) {
		ir_send_nec(code);
	}
	return err;
}

static int
ir_dev_nop(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw ir_cdevsw = {
	.d_open = ir_dev_nop, .d_close = ir_dev_nop, .d_read = eno_rdwrt, .d_write = ir_dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0,
};

static uint8_t modem_fat_secbuf[4096];

static uint16_t
fat_u16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
fat_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char
fat_up(uint8_t c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
}

static bool
fat_eq_83_ci(const uint8_t d[11], const char want[11])
{
	for (unsigned i = 0; i < 11; i++) {
		if (fat_up(d[i]) != fat_up((uint8_t)want[i])) {
			return false;
		}
	}
	return true;
}

static bool
fat_str_eq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		if (fat_up((uint8_t)*a++) != fat_up((uint8_t)*b++)) {
			return false;
		}
	}
	return *a == 0 && *b == 0;
}

static bool
fat_str_pfx_ci(const char *s, const char *pfx)
{
	while (*pfx) {
		if (fat_up((uint8_t)*s++) != fat_up((uint8_t)*pfx++)) {
			return false;
		}
	}
	return true;
}

static bool
fat_str_sfx_ci(const char *s, const char *sfx)
{
	size_t sl = strlen(s), xl = strlen(sfx);
	return sl >= xl && fat_str_eq_ci(s + (sl - xl), sfx);
}

static void
fat_lfn_feed(const uint8_t d[32], char lfn[160], bool *valid)
{
	static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	unsigned seq = d[0] & 0x1fu;

	if ((d[0] & 0x40u) != 0) {
		memset(lfn, 0, 160);
		*valid = true;
	}
	if (!*valid || seq < 1u || seq > 12u) {
		*valid = false;
		return;
	}
	unsigned base = (seq - 1u) * 13u;
	for (unsigned i = 0; i < 13u && base + i < 159u; i++) {
		uint16_t ch = fat_u16(d + offs[i]);
		if (ch == 0x0000u || ch == 0xffffu) {
			break;
		}
		lfn[base + i] = (ch < 0x80u) ? (char)ch : '_';
	}
}

static void
fat_format_83(const uint8_t d[11], char out[16])
{
	unsigned p = 0, end8 = 8, end3 = 11;

	while (end8 > 0 && d[end8 - 1] == ' ') end8--;
	for (unsigned i = 0; i < end8; i++) out[p++] = (char)d[i];
	while (end3 > 8 && d[end3 - 1] == ' ') end3--;
	if (end3 > 8) {
		out[p++] = '.';
		for (unsigned i = 8; i < end3; i++) out[p++] = (char)d[i];
	}
	out[p] = 0;
}

struct fat_geom {
	uint64_t part_bytes;
	uint32_t bps;
	uint32_t spc;
	uint32_t rsvd_sec;
	uint32_t num_fats;
	uint32_t root_ent;
	uint32_t fat_sz;
	uint32_t root_clus;
	uint32_t first_data_sec;
	uint32_t root_dir_secs;
	uint32_t tot_sec;
	bool is_fat32;
};

static bool
fat_init_geom(struct fat_geom *g, uint8_t secbuf[4096])
{
	uint64_t part_bytes = 0;

	if (hakios_ufs_read_partition == NULL ||
	    hakios_ufs_read_partition("modem_a", 0, secbuf, 512, &part_bytes) != 0) {
		return false;
	}
	if (secbuf[510] != 0x55u || secbuf[511] != 0xAAu) {
		return false;
	}
	uint32_t bps = fat_u16(secbuf + 11);
	uint32_t spc = secbuf[13];
	uint32_t rsvd = fat_u16(secbuf + 14);
	uint32_t nfats = secbuf[16];
	uint32_t rent = fat_u16(secbuf + 17);
	uint32_t t16 = fat_u16(secbuf + 19);
	uint32_t f16 = fat_u16(secbuf + 22);
	uint32_t t32 = fat_u32(secbuf + 32);
	uint32_t f32 = fat_u32(secbuf + 36);
	uint32_t rclus = fat_u32(secbuf + 44);

	if (bps < 512u || bps > 4096u || (bps & (bps - 1u)) != 0 || spc == 0 || nfats == 0) {
		return false;
	}
	uint32_t tot_sec = t16 != 0 ? t16 : t32;
	uint32_t fat_sz = f16 != 0 ? f16 : f32;
	uint32_t rdir_secs = ((rent * 32u) + (bps - 1u)) / bps;
	uint32_t fdata = rsvd + nfats * fat_sz + rdir_secs;
	if (fat_sz == 0 || fdata >= tot_sec) {
		return false;
	}
	uint32_t data_clus = (tot_sec - fdata) / spc;
	g->part_bytes = part_bytes;
	g->bps = bps;
	g->spc = spc;
	g->rsvd_sec = rsvd;
	g->num_fats = nfats;
	g->root_ent = rent;
	g->fat_sz = fat_sz;
	g->root_clus = rclus;
	g->first_data_sec = fdata;
	g->root_dir_secs = rdir_secs;
	g->tot_sec = tot_sec;
	g->is_fat32 = (rent == 0 && data_clus >= 65525u);
	return true;
}

static uint32_t
fat_next_clus(const struct fat_geom *g, uint32_t clus)
{
	uint8_t ent[4] = { 0 };
	uint64_t off = (uint64_t)g->rsvd_sec * g->bps + (uint64_t)clus * (g->is_fat32 ? 4u : 2u);

	if (off + 4u > g->part_bytes ||
	    hakios_ufs_read_partition("modem_a", off, ent, g->is_fat32 ? 4u : 2u, NULL) != 0) {
		return 0x0fffffffu;
	}
	if (g->is_fat32) {
		uint32_t v = fat_u32(ent) & 0x0fffffffu;
		return v >= 0x0ffffff8u ? 0x0fffffffu : v;
	}
	uint16_t v = fat_u16(ent);
	return v >= 0xfff8u ? 0x0fffffffu : (uint32_t)v;
}

static uint64_t
fat_clus_off(const struct fat_geom *g, uint32_t clus)
{
	uint64_t sec = (uint64_t)g->first_data_sec + (uint64_t)(clus - 2u) * g->spc;
	return sec * g->bps;
}

static bool
fat_find_image_dir(const struct fat_geom *g, uint8_t secbuf[4096], uint32_t *out_clus, bool log_root)
{
	unsigned logged = 0;
	uint32_t first_dir_clus = 0;
	char lfn[160], sname[16];
	bool lfn_valid = false;

	lfn[0] = 0;
	if (!g->is_fat32 && g->root_dir_secs > 0) {
		uint64_t base = (uint64_t)(g->rsvd_sec + g->num_fats * g->fat_sz) * g->bps;
		for (uint32_t s = 0; s < g->root_dir_secs; s++) {
			int err = hakios_ufs_read_partition("modem_a", base + (uint64_t)s * g->bps, secbuf, g->bps, NULL);
			if (err != 0) {
				hakios_tl("UMI ufs_fw: rdir read sec=%u off=%llu err=%d\n",
				    s, base + (uint64_t)s * g->bps, err);
				return false;
			}
			for (uint32_t o = 0; o + 32u <= g->bps; o += 32u) {
				const uint8_t *d = secbuf + o;
				if (d[0] == 0x00 || d[0] == 0xe5) { lfn_valid = false; continue; }
				if ((d[11] & 0x3fu) == 0x0fu) { fat_lfn_feed(d, lfn, &lfn_valid); continue; }
				fat_format_83(d, sname);
				const char *fname = (lfn_valid && lfn[0] != 0) ? lfn : sname;
				lfn_valid = false;
				uint32_t fclus = ((uint32_t)fat_u16(d + 20) << 16) | fat_u16(d + 26);
				uint32_t fsize = fat_u32(d + 28);
				if (log_root && logged < 8u) {
					hakios_tl("UMI ufs_fw: root[%u] '%s' attr=%#x clus=%u size=%u\n",
					    logged, fname, d[11], fclus, fsize);
					logged++;
				}
				if ((d[11] & 0x10) != 0 && fclus >= 2u) {
					if (first_dir_clus == 0) {
						first_dir_clus = fclus;
					}
					if (fat_str_eq_ci(fname, "image")) {
						*out_clus = fclus;
						return true;
					}
				}
			}
		}
	} else {
		uint32_t clus = g->root_clus;
		for (unsigned iter = 0; iter < 64u && clus >= 2u && clus < 0x0ffffff8u; iter++) {
			uint64_t base = fat_clus_off(g, clus);
			for (uint32_t s = 0; s < g->spc; s++) {
				int err = hakios_ufs_read_partition("modem_a", base + (uint64_t)s * g->bps, secbuf, g->bps, NULL);
				if (err != 0) {
					hakios_tl("UMI ufs_fw: rdir32 read clus=%u err=%d\n", clus, err);
					return false;
				}
				for (uint32_t o = 0; o + 32u <= g->bps; o += 32u) {
					const uint8_t *d = secbuf + o;
					if (d[0] == 0x00 || d[0] == 0xe5) { lfn_valid = false; continue; }
					if ((d[11] & 0x3fu) == 0x0fu) { fat_lfn_feed(d, lfn, &lfn_valid); continue; }
					fat_format_83(d, sname);
					const char *fname = (lfn_valid && lfn[0] != 0) ? lfn : sname;
					lfn_valid = false;
					uint32_t fclus = ((uint32_t)fat_u16(d + 20) << 16) | fat_u16(d + 26);
					uint32_t fsize = fat_u32(d + 28);
					if (log_root && logged < 8u) {
						hakios_tl("UMI ufs_fw: root[%u] '%s' attr=%#x clus=%u size=%u\n",
						    logged, fname, d[11], fclus, fsize);
						logged++;
					}
					if ((d[11] & 0x10) != 0 && fclus >= 2u) {
						if (first_dir_clus == 0) {
							first_dir_clus = fclus;
						}
						if (fat_str_eq_ci(fname, "image")) {
							*out_clus = fclus;
							return true;
						}
					}
				}
			}
			clus = fat_next_clus(g, clus);
		}
	}
	if (first_dir_clus < 2u) {
		return false;
	}
	uint32_t sub_clus = first_dir_clus;
	unsigned sub_logged = 0;
	lfn_valid = false;
	for (unsigned iter = 0; iter < 16u && sub_clus >= 2u && sub_clus < 0x0ffffff8u; iter++) {
		uint64_t base = fat_clus_off(g, sub_clus);
		for (uint32_t s = 0; s < g->spc; s++) {
			if (hakios_ufs_read_partition("modem_a", base + (uint64_t)s * g->bps, secbuf, g->bps, NULL) != 0) {
				*out_clus = first_dir_clus;
				return true;
			}
			for (uint32_t o = 0; o + 32u <= g->bps; o += 32u) {
				const uint8_t *d = secbuf + o;
				if (d[0] == 0x00 || d[0] == 0xe5) { lfn_valid = false; continue; }
				if ((d[11] & 0x3fu) == 0x0fu) { fat_lfn_feed(d, lfn, &lfn_valid); continue; }
				fat_format_83(d, sname);
				const char *fname = (lfn_valid && lfn[0] != 0) ? lfn : sname;
				lfn_valid = false;
				uint32_t fclus = ((uint32_t)fat_u16(d + 20) << 16) | fat_u16(d + 26);
				uint32_t fsize = fat_u32(d + 28);
				if (log_root && sub_logged < 8u) {
					hakios_tl("UMI ufs_fw: sub[%u] '%s' attr=%#x clus=%u size=%u\n",
					    sub_logged, fname, d[11], fclus, fsize);
					sub_logged++;
				}
				if ((d[11] & 0x10) != 0 && fclus >= 2u && fat_str_eq_ci(fname, "image")) {
					*out_clus = fclus;
					return true;
				}
			}
		}
		sub_clus = fat_next_clus(g, sub_clus);
	}
	*out_clus = first_dir_clus;
	return true;
}

int
hakios_modem_fat_read(const char *image_file, uint8_t *dst, uint32_t max_len, uint32_t *out_len)
{
	struct fat_geom g;
	uint32_t img_clus = 0;
	char lfn[160], sname[16];
	bool lfn_valid = false;

	lfn[0] = 0;
	if (!fat_init_geom(&g, modem_fat_secbuf) || !fat_find_image_dir(&g, modem_fat_secbuf, &img_clus, false)) {
		return ENOENT;
	}
	uint32_t clus = img_clus, fclus = 0, fsize = 0;
	bool found = false;
	for (unsigned iter = 0; iter < 256u && clus >= 2u && clus < 0x0ffffff8u && !found; iter++) {
		uint64_t base = fat_clus_off(&g, clus);
		for (uint32_t s = 0; s < g.spc && !found; s++) {
			if (hakios_ufs_read_partition("modem_a", base + (uint64_t)s * g.bps, modem_fat_secbuf, g.bps, NULL) != 0) {
				return EIO;
			}
			for (uint32_t o = 0; o + 32u <= g.bps; o += 32u) {
				const uint8_t *d = modem_fat_secbuf + o;
				if (d[0] == 0x00) { lfn_valid = false; break; }
				if (d[0] == 0xe5) { lfn_valid = false; continue; }
				if ((d[11] & 0x3fu) == 0x0fu) { fat_lfn_feed(d, lfn, &lfn_valid); continue; }
				fat_format_83(d, sname);
				const char *fname = (lfn_valid && lfn[0] != 0) ? lfn : sname;
				lfn_valid = false;
				if ((d[11] & 0x18) != 0) continue;
				if (fat_str_eq_ci(fname, image_file)) {
					fclus = ((uint32_t)fat_u16(d + 20) << 16) | fat_u16(d + 26);
					fsize = fat_u32(d + 28);
					found = true;
					break;
				}
			}
		}
		clus = fat_next_clus(&g, clus);
	}
	if (!found) {
		return ENOENT;
	}
	if (out_len != NULL) {
		*out_len = fsize;
	}
	if (dst == NULL || max_len == 0) {
		return 0;
	}
	uint32_t want = fsize < max_len ? fsize : max_len;
	uint32_t copied = 0, c = fclus;
	uint32_t clus_bytes = g.bps * g.spc;
	while (copied < want && c >= 2u && c < 0x0ffffff8u) {
		uint32_t chunk = want - copied;
		if (chunk > clus_bytes) chunk = clus_bytes;
		if (hakios_ufs_read_partition("modem_a", fat_clus_off(&g, c), dst + copied, chunk, NULL) != 0) {
			return EIO;
		}
		copied += chunk;
		c = fat_next_clus(&g, c);
	}
	return copied == want ? 0 : EIO;
}

static void
thyme_modem_fat_scan(void)
{
	struct fat_geom g;
	uint32_t img_clus = 0;
	char lfn[160], sname[16];
	bool lfn_valid = false;

	lfn[0] = 0;
	if (!fat_init_geom(&g, modem_fat_secbuf)) {
		hakios_tl("UMI ufs_fw: modem_a FAT init failed\n");
		return;
	}
	hakios_tl("UMI ufs_fw: modem_a FAT%u bps=%u spc=%u nfats=%u rsvd=%u fat_sz=%u root_ent=%u fdata=%u\n",
	    g.is_fat32 ? 32u : 16u, g.bps, g.spc, g.num_fats, g.rsvd_sec, g.fat_sz, g.root_ent, g.first_data_sec);
	if (!fat_find_image_dir(&g, modem_fat_secbuf, &img_clus, true)) {
		hakios_tl("UMI ufs_fw: modem_a/IMAGE not found\n");
		return;
	}
	uint32_t clus = img_clus, logged = 0, total = 0, sample_logged = 0;
	uint32_t venus_clus = 0, cdsp_clus = 0;
	bool done = false;
	for (unsigned iter = 0; iter < 256u && clus >= 2u && clus < 0x0ffffff8u && !done; iter++) {
		uint64_t base = fat_clus_off(&g, clus);
		for (uint32_t s = 0; s < g.spc && !done; s++) {
			if (hakios_ufs_read_partition("modem_a", base + (uint64_t)s * g.bps, modem_fat_secbuf, g.bps, NULL) != 0) {
				done = true;
				break;
			}
			for (uint32_t o = 0; o + 32u <= g.bps; o += 32u) {
				const uint8_t *d = modem_fat_secbuf + o;
				if (d[0] == 0x00) { lfn_valid = false; done = true; break; }
				if (d[0] == 0xe5) { lfn_valid = false; continue; }
				if ((d[11] & 0x3fu) == 0x0fu) { fat_lfn_feed(d, lfn, &lfn_valid); continue; }
				fat_format_83(d, sname);
				const char *fname = (lfn_valid && lfn[0] != 0) ? lfn : sname;
				lfn_valid = false;
				if ((d[11] & 0x18) != 0) continue;
				total++;
				bool is_venus = fat_str_pfx_ci(fname, "venus");
				bool is_cdsp = fat_str_pfx_ci(fname, "cdsp");
				if (is_venus || is_cdsp) {
					uint32_t fsize = fat_u32(d + 28);
					uint32_t fclus = ((uint32_t)fat_u16(d + 20) << 16) | fat_u16(d + 26);
					if (logged < 20u) {
						logged++;
						hakios_tl("UMI ufs_fw: image/%s size=%u clus=%u\n",
						    fname, fsize, fclus);
					}
					if (is_venus && (fat_str_sfx_ci(fname, ".mdt") || fat_str_sfx_ci(fname, ".mbn"))) {
						venus_fw_size = (int)fsize;
						venus_clus = fclus;
					}
					if (is_cdsp && (fat_str_sfx_ci(fname, ".mdt") || fat_str_sfx_ci(fname, ".mbn"))) {
						cdsp_fw_size = (int)fsize;
						cdsp_clus = fclus;
					}
				} else if (sample_logged < 8u) {
					sample_logged++;
					hakios_tl("UMI ufs_fw: sample[%u] %s size=%u\n",
					    sample_logged, fname, fat_u32(d + 28));
				}
			}
		}
		clus = fat_next_clus(&g, clus);
	}
	uint8_t hdr[16] = { 0 };
	if (venus_clus >= 2u &&
	    hakios_ufs_read_partition("modem_a", fat_clus_off(&g, venus_clus), hdr, sizeof(hdr), NULL) == 0) {
		hakios_tl("UMI ufs_fw: VENUS header (%d B): %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    venus_fw_size, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]);
	}
	if (cdsp_clus >= 2u &&
	    hakios_ufs_read_partition("modem_a", fat_clus_off(&g, cdsp_clus), hdr, sizeof(hdr), NULL) == 0) {
		hakios_tl("UMI ufs_fw: CDSP header (%d B): %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    cdsp_fw_size, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]);
	}
	hakios_tl("UMI ufs_fw: scanned %u files in IMAGE (venus=%d cdsp=%d)\n",
	    total, venus_fw_size, cdsp_fw_size);
	{
		uint8_t sb[64] = { 0 };
		if (hakios_ufs_read_partition("dsp_a", 0x400u, sb, sizeof(sb), NULL) == 0) {
			hakios_tl("UMI ufs_fw: dsp_a magic=%#x blocks=%u\n",
			    fat_u16(sb + 0x38u), fat_u32(sb + 4u));
		}
	}
}

#define UMI_SCM_64              (1ULL << 30)
#define UMI_SCM_SIP             (2ULL << 24)
#define UMI_SCM_PAS_INIT_IMAGE  ((2ULL << 8) | 1ULL)
#define UMI_SCM_PAS_MEM_SETUP   ((2ULL << 8) | 2ULL)
#define UMI_SCM_PAS_AUTH_RESET  ((2ULL << 8) | 5ULL)
#define UMI_SCM_PAS_SHUTDOWN    ((2ULL << 8) | 6ULL)
#define UMI_VENUS_PA            0x8ca00000ULL
#define UMI_VENUS_SIZE          0x00500000UL
#define UMI_VENUS_PAS_ID        9ULL
#define UMI_CDSP_PA             0x8d400000ULL
#define UMI_CDSP_SIZE           0x01400000UL
#define UMI_CDSP_PAS_ID         0x12ULL

static int32_t
thyme_scm_call(uint64_t func, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *ans)
{
	uint64_t session = 0;

	func |= UMI_SCM_64 | UMI_SCM_SIP;
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = (i == 0) ? func : 1ULL;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = a0;
		register uint64_t x3 __asm__("x3") = a1;
		register uint64_t x4 __asm__("x4") = a2;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
		    : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != 1ULL) {
			if (ans != NULL) {
				*ans = x1;
			}
			return (int32_t)x0;
		}
	}
	return -1;
}

static void
thyme_pil_boot_one(const char *tag, const char *mdt_name, const char *seg_fmt,
    uint64_t pas_id, uint64_t carve_pa, vm_size_t carve_sz,
    bool do_auth_reset, int *out_ready, int *out_segs)
{
	vm_offset_t meta_va = 0;
	uint32_t mdt_size = 0;

	if (kmem_alloc(kernel_map, &meta_va, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || meta_va == 0) {
		hakios_tl("UMI %s: meta alloc failed\n", tag);
		return;
	}
	if (hakios_modem_fat_read(mdt_name, (uint8_t *)meta_va, (uint32_t)PAGE_SIZE, &mdt_size) != 0 ||
	    mdt_size < 52u || mdt_size > (uint32_t)PAGE_SIZE) {
		hakios_tl("UMI %s: %s read failed (size=%u)\n", tag, mdt_name, mdt_size);
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}
	const uint8_t *hdr = (const uint8_t *)meta_va;
	if (memcmp(hdr, "\177ELF\1\1\1", 7) != 0) {
		hakios_tl("UMI %s: bad ELF magic\n", tag);
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}
	uint32_t phoff = fat_u32(hdr + 28u);
	uint32_t phentsize = fat_u16(hdr + 42u);
	uint32_t phnum = fat_u16(hdr + 44u);
	if (phoff < 52u || phoff > mdt_size || phentsize != 32u ||
	    phnum < 2u || phnum > 64u || phnum > (mdt_size - phoff) / phentsize) {
		hakios_tl("UMI %s: bad ELF phoff=%u phentsize=%u phnum=%u size=%u\n",
		    tag, phoff, phentsize, phnum, mdt_size);
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}
	uint32_t lowest = 0xffffffffu, loadable = 0;
	uint64_t highest = 0;
	bool reloc = false;

	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = hdr + phoff + phentsize * i;
		uint32_t p_type = fat_u32(ph + 0u);
		uint32_t p_paddr = fat_u32(ph + 12u);
		uint32_t p_filesz = fat_u32(ph + 16u);
		uint32_t p_memsz = fat_u32(ph + 20u);
		uint32_t p_flags = fat_u32(ph + 24u);

		if (p_type != 1u || ((p_flags & (7u << 24)) == (2u << 24))) {
			continue;
		}
		if (p_filesz > p_memsz) {
			hakios_tl("UMI %s: seg %u filesz=%u exceeds memsz=%u\n", tag, i, p_filesz, p_memsz);
			kmem_free(kernel_map, meta_va, PAGE_SIZE);
			return;
		}
		if (p_memsz == 0) {
			continue;
		}
		if ((p_flags & 0x08000000u) != 0) {
			reloc = true;
		}
		if (p_paddr < lowest) {
			lowest = p_paddr;
		}
		uint64_t end = (uint64_t)p_paddr + p_memsz;
		if (end > highest) {
			highest = end;
		}
		loadable++;
	}
	uint32_t base = reloc ? lowest : (uint32_t)carve_pa;
	uint64_t aligned_end = (highest + 0xfffULL) & ~0xfffULL;
	uint64_t span = aligned_end > base ? aligned_end - base : 0;
	hakios_tl("UMI %s: ELF segments=%u loadable=%u reloc=%d span=%#llx (base=%#x)\n",
	    tag, phnum, loadable, reloc ? 1 : 0, span, base);
	if (loadable == 0 || lowest < base || span == 0 || (span & 7u) != 0 || span > carve_sz) {
		hakios_tl("UMI %s: invalid ELF span %#llx (max=%#lx)\n", tag, span, (unsigned long)carve_sz);
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}

	for (vm_offset_t line = meta_va; line < meta_va + PAGE_SIZE; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy; isb" ::: "memory");

	uint64_t ans = ~0ULL;
	int32_t ret = thyme_scm_call(UMI_SCM_PAS_INIT_IMAGE, 2u | (2u << 6),
	    pas_id, kvtophys(meta_va), 0, &ans);
	hakios_tl("UMI %s: TZ INIT_IMAGE ret=%d ans=%#llx\n", tag, ret, ans);
	if (ret != 0 || ans != 0) {
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}

	/* PAS establishes the relocation window before the AP writes the segments. */
	if (reloc) {
		ret = thyme_scm_call(UMI_SCM_PAS_MEM_SETUP, 3u, pas_id, carve_pa, span, &ans);
		hakios_tl("UMI %s: TZ MEM_SETUP ret=%d ans=%#llx\n", tag, ret, ans);
		if (ret != 0 || ans != 0) {
			kmem_free(kernel_map, meta_va, PAGE_SIZE);
			return;
		}
	}

	vm_offset_t carve = ml_io_map_unmappable((vm_offset_t)carve_pa, carve_sz, VM_WIMG_WCOMB);
	if (carve == 0) {
		hakios_tl("UMI %s: carveout map failed\n", tag);
		kmem_free(kernel_map, meta_va, PAGE_SIZE);
		return;
	}
	hakios_tl("UMI %s: loading %u segments into %#llx (span=%#llx)\n", tag, loadable, carve_pa, span);
	volatile uint8_t *dst = (volatile uint8_t *)carve;
	for (uint64_t z = 0; z < span; z += 8u) {
		*(volatile uint64_t *)(dst + z) = 0;
	}
	uint32_t total_loaded = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = hdr + phoff + phentsize * i;
		uint32_t p_type = fat_u32(ph + 0u);
		uint32_t p_paddr = fat_u32(ph + 12u);
		uint32_t p_filesz = fat_u32(ph + 16u);
		uint32_t p_memsz = fat_u32(ph + 20u);
		uint32_t p_flags = fat_u32(ph + 24u);

		if (p_type != 1u || ((p_flags & (7u << 24)) == (2u << 24)) || p_memsz == 0 || p_filesz == 0) {
			continue;
		}
		if (p_paddr < base || (uint64_t)(p_paddr - base) + p_memsz > span) {
			hakios_tl("UMI %s: seg %u out of bounds\n", tag, i);
			ml_io_unmap(carve, carve_sz);
			kmem_free(kernel_map, meta_va, PAGE_SIZE);
			return;
		}
		char bname[24];
		snprintf(bname, sizeof(bname), seg_fmt, i);
		vm_size_t bufsz = round_page((vm_size_t)p_filesz);
		vm_offset_t seg_va = 0;
		if (kmem_alloc(kernel_map, &seg_va, bufsz, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
		    seg_va == 0) {
			hakios_tl("UMI %s: seg %u alloc %u failed\n", tag, i, p_filesz);
			ml_io_unmap(carve, carve_sz);
			kmem_free(kernel_map, meta_va, PAGE_SIZE);
			return;
		}
		uint32_t got_sz = 0;
		int rerr = hakios_modem_fat_read(bname, (uint8_t *)seg_va, p_filesz, &got_sz);
		if (rerr != 0 || got_sz != p_filesz) {
			hakios_tl("UMI %s: read %s err=%d got=%u want=%u\n", tag, bname, rerr, got_sz, p_filesz);
			kmem_free(kernel_map, seg_va, bufsz);
			ml_io_unmap(carve, carve_sz);
			kmem_free(kernel_map, meta_va, PAGE_SIZE);
			return;
		}
		uint32_t at = p_paddr - base, k = 0;
		const uint8_t *src_b = (const uint8_t *)seg_va;
		while (k < p_filesz && ((at + k) & 7u) != 0) {
			dst[at + k] = src_b[k];
			k++;
		}
		for (; k + 8u <= p_filesz; k += 8u) {
			uint64_t w;
			memcpy(&w, src_b + k, sizeof(w));
			*(volatile uint64_t *)(dst + at + k) = w;
		}
		for (; k < p_filesz; k++) {
			dst[at + k] = src_b[k];
		}
		total_loaded += p_filesz;
		kmem_free(kernel_map, seg_va, bufsz);
	}
	__asm__ volatile("dsb sy; isb" ::: "memory");
	ml_io_unmap(carve, carve_sz);
	kmem_free(kernel_map, meta_va, PAGE_SIZE);
	hakios_tl("UMI %s: segments loaded (%u bytes total)\n", tag, total_loaded);

	*out_segs = (int)loadable;

	if (!do_auth_reset) {
		return;
	}
	ret = thyme_scm_call(UMI_SCM_PAS_AUTH_RESET, 1u, pas_id, 0, 0, &ans);
	hakios_tl("UMI %s: TZ AUTH_AND_RESET ret=%d ans=%#llx\n", tag, ret, ans);
	if (ret == 0 && ans == 0) {
		*out_ready = 1;
	}
}

#define VENUS_GCC_VIDEO_BASE    0x0010b000UL
#define VENUS_GCC_AHB_CBCR      0x004u
#define VENUS_GCC_XO_CBCR       0x03cu
#define VENUS_VIDEO_CC_BASE     0x0abf0000UL
#define VENUS_MVS0_RCG_CMD      0xb94u
#define VENUS_MVS0_RCG_CFG      0xb98u
#define VENUS_MVS0C_DIV         0xc54u
#define VENUS_VIDEO_AHB_CBCR    0xe58u
#define VENUS_VIDEO_XO_CBCR     0xeecu
#define VENUS_RCG_UPDATE        1u
#define VENUS_RCG_ROOT_OFF      (1u << 31)
#define VENUS_RCG_RATE_MASK    0x371fu
#define VENUS_RCG_XO_DIV1      1u
#define VENUS_CORE_RESET       (1u << 2)
#define VENUS_MVS0C_GDSCR       0xbf8u
#define VENUS_MVS0C_CBCR        0xc34u
#define VENUS_CLK_ENABLE        1u
#define VENUS_CLK_OFF           (1u << 31)
#define VENUS_SW_COLLAPSE       1u
#define VENUS_PWR_ON            (1u << 31)

static bool
thyme_venus_wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t expected)
{
	for (unsigned attempt = 0; attempt < 200; attempt++) {
		if ((keys_reg(base, offset) & mask) == expected) {
			return true;
		}
		keys_spin_us(10);
	}
	return (keys_reg(base, offset) & mask) == expected;
}

static bool
thyme_venus_set_branch(vm_offset_t base, uint32_t offset, uint32_t target)
{
	uint32_t reg = keys_reg(base, offset);
	uint32_t enable = target & VENUS_CLK_ENABLE;

	if ((reg & VENUS_CLK_ENABLE) != enable) {
		keys_write(base, offset, (reg & ~VENUS_CLK_ENABLE) | enable);
	}
	const uint32_t mask = VENUS_CLK_ENABLE | VENUS_CLK_OFF;
	return thyme_venus_wait_reg(base, offset, mask, target & mask);
}

static bool
thyme_venus_set_rcg(vm_offset_t base, uint32_t cfg)
{
	keys_write(base, VENUS_MVS0_RCG_CFG, cfg);
	keys_write(base, VENUS_MVS0_RCG_CMD, keys_reg(base, VENUS_MVS0_RCG_CMD) | VENUS_RCG_UPDATE);
	return thyme_venus_wait_reg(base, VENUS_MVS0_RCG_CMD, VENUS_RCG_UPDATE, 0);
}

static void
thyme_venus_core_probe(vm_offset_t vcc)
{
	uint32_t core = keys_reg(vcc, VENUS_MVS0C_CBCR);
	uint32_t cfg = keys_reg(vcc, VENUS_MVS0_RCG_CFG);
	uint32_t cmd = keys_reg(vcc, VENUS_MVS0_RCG_CMD);
	uint32_t ahb = keys_reg(vcc, VENUS_VIDEO_AHB_CBCR);
	uint32_t xo = keys_reg(vcc, VENUS_VIDEO_XO_CBCR);
	uint32_t div = (keys_reg(vcc, VENUS_MVS0C_DIV) & 3u) + 1u;

	hakios_tl("UMI venus: core clock before cbcr=%#x cmd=%#x cfg=%#x div=%u ahb=%#x xo=%#x\n",
	    core, cmd, cfg, div, ahb, xo);
	if ((core & (VENUS_CLK_ENABLE | VENUS_CORE_RESET | VENUS_CLK_OFF)) != VENUS_CLK_OFF ||
	    (cmd & (VENUS_RCG_ROOT_OFF | VENUS_RCG_UPDATE)) != VENUS_RCG_ROOT_OFF) {
		hakios_tl("UMI venus: core clock probe skipped (branch or RCG in use)\n");
		return;
	}

	/* The AHB/XO branches can report running with no local enable vote. Keep inherited votes. */
	bool vote_xo = (xo & VENUS_CLK_OFF) != 0;
	bool xo_on = !vote_xo || thyme_venus_set_branch(vcc, VENUS_VIDEO_XO_CBCR, VENUS_CLK_ENABLE);
	bool vote_ahb = xo_on && (ahb & VENUS_CLK_OFF) != 0;
	bool ahb_on = xo_on && (!vote_ahb ||
	    thyme_venus_set_branch(vcc, VENUS_VIDEO_AHB_CBCR, VENUS_CLK_ENABLE));
	uint32_t test_cfg = (cfg & ~VENUS_RCG_RATE_MASK) | VENUS_RCG_XO_DIV1;

	bool changed = ahb_on && test_cfg != cfg;
	bool source_ready = ahb_on && (!changed || thyme_venus_set_rcg(vcc, test_cfg));
	bool running = source_ready && thyme_venus_set_branch(vcc, VENUS_MVS0C_CBCR, VENUS_CLK_ENABLE);
	hakios_tl("UMI venus: core clock test running=%d cbcr=%#x cfg=%#x expected_hz=%u\n",
	    running, keys_reg(vcc, VENUS_MVS0C_CBCR), keys_reg(vcc, VENUS_MVS0_RCG_CFG),
	    19200000u / div);

	bool core_restored = !source_ready || thyme_venus_set_branch(vcc, VENUS_MVS0C_CBCR, core);
	bool cfg_restored = !changed || thyme_venus_set_rcg(vcc, cfg);
	bool ahb_restored = !vote_ahb || thyme_venus_set_branch(vcc, VENUS_VIDEO_AHB_CBCR, ahb);
	bool xo_restored = !vote_xo || thyme_venus_set_branch(vcc, VENUS_VIDEO_XO_CBCR, xo);
	hakios_tl("UMI venus: core clock restore ok=%d cbcr=%#x cfg=%#x ahb=%#x xo=%#x\n",
	    core_restored && cfg_restored && ahb_restored && xo_restored,
	    keys_reg(vcc, VENUS_MVS0C_CBCR), keys_reg(vcc, VENUS_MVS0_RCG_CFG),
	    keys_reg(vcc, VENUS_VIDEO_AHB_CBCR), keys_reg(vcc, VENUS_VIDEO_XO_CBCR));
}

static void
thyme_venus_power_probe(uint32_t mode)
{
	const vm_offset_t gcc_pa = trunc_page(VENUS_GCC_VIDEO_BASE);
	const vm_offset_t gcc_offset = VENUS_GCC_VIDEO_BASE - gcc_pa;
	const vm_size_t gcc_size = round_page(gcc_offset + 0x1000UL);
	vm_offset_t gcc_mapping = ml_io_map_unmappable(gcc_pa, gcc_size, VM_WIMG_IO);
	if (gcc_mapping == 0) {
		hakios_tl("UMI venus: gcc_video map failed\n");
		return;
	}

	const vm_offset_t gcc = gcc_mapping + gcc_offset;
	const uint32_t ahb = keys_reg(gcc, VENUS_GCC_AHB_CBCR);
	const uint32_t xo = keys_reg(gcc, VENUS_GCC_XO_CBCR);
	hakios_tl("UMI venus: gcc_video ahb=%#x xo=%#x\n", ahb, xo);
	if (mode == 1) {
		ml_io_unmap(gcc_mapping, gcc_size);
		return;
	}

	keys_write(gcc, VENUS_GCC_AHB_CBCR, ahb | VENUS_CLK_ENABLE);
	keys_write(gcc, VENUS_GCC_XO_CBCR, xo | VENUS_CLK_ENABLE);
	const uint32_t running = VENUS_CLK_ENABLE | VENUS_CLK_OFF;
	bool ahb_on = thyme_venus_wait_reg(gcc, VENUS_GCC_AHB_CBCR, running, VENUS_CLK_ENABLE);
	bool xo_on = thyme_venus_wait_reg(gcc, VENUS_GCC_XO_CBCR, running, VENUS_CLK_ENABLE);
	hakios_tl("UMI venus: gcc_video enabled ahb=%#x xo=%#x\n",
	    keys_reg(gcc, VENUS_GCC_AHB_CBCR), keys_reg(gcc, VENUS_GCC_XO_CBCR));

	vm_offset_t vcc = (ahb_on && xo_on) ? ml_io_map_unmappable(VENUS_VIDEO_CC_BASE, PAGE_SIZE, VM_WIMG_IO) : 0;
	if (vcc != 0) {
		uint32_t gdsc = keys_reg(vcc, VENUS_MVS0C_GDSCR);
		hakios_tl("UMI venus: mvs0c_gdsc before=%#x\n", gdsc);
		if ((gdsc & VENUS_SW_COLLAPSE) != 0) {
			keys_write(vcc, VENUS_MVS0C_GDSCR, gdsc & ~VENUS_SW_COLLAPSE);
		}
		bool powered = thyme_venus_wait_reg(vcc, VENUS_MVS0C_GDSCR, VENUS_PWR_ON, VENUS_PWR_ON);
		hakios_tl("UMI venus: mvs0c_gdsc after=%#x\n", keys_reg(vcc, VENUS_MVS0C_GDSCR));
		if (powered && mode == 3) {
			thyme_venus_core_probe(vcc);
		} else if (powered) {
			hakios_tl("UMI venus: mvs0c_core cbcr=%#x\n", keys_reg(vcc, VENUS_MVS0C_CBCR));
		}
		if ((gdsc & VENUS_SW_COLLAPSE) != 0) {
			uint32_t current = keys_reg(vcc, VENUS_MVS0C_GDSCR);
			keys_write(vcc, VENUS_MVS0C_GDSCR, current | VENUS_SW_COLLAPSE);
			(void)thyme_venus_wait_reg(vcc, VENUS_MVS0C_GDSCR, VENUS_PWR_ON, gdsc & VENUS_PWR_ON);
		}
		hakios_tl("UMI venus: mvs0c_gdsc restored=%#x\n", keys_reg(vcc, VENUS_MVS0C_GDSCR));
		ml_io_unmap(vcc, PAGE_SIZE);
	} else {
		hakios_tl("UMI venus: video_cc unavailable (ahb=%d xo=%d)\n", ahb_on, xo_on);
	}

	if ((xo & VENUS_CLK_ENABLE) == 0) {
		keys_write(gcc, VENUS_GCC_XO_CBCR, keys_reg(gcc, VENUS_GCC_XO_CBCR) & ~VENUS_CLK_ENABLE);
		(void)thyme_venus_wait_reg(gcc, VENUS_GCC_XO_CBCR, VENUS_CLK_OFF, xo & VENUS_CLK_OFF);
	}
	if ((ahb & VENUS_CLK_ENABLE) == 0) {
		keys_write(gcc, VENUS_GCC_AHB_CBCR, keys_reg(gcc, VENUS_GCC_AHB_CBCR) & ~VENUS_CLK_ENABLE);
		(void)thyme_venus_wait_reg(gcc, VENUS_GCC_AHB_CBCR, VENUS_CLK_OFF, ahb & VENUS_CLK_OFF);
	}
	hakios_tl("UMI venus: gcc_video restored ahb=%#x xo=%#x\n",
	    keys_reg(gcc, VENUS_GCC_AHB_CBCR), keys_reg(gcc, VENUS_GCC_XO_CBCR));
	ml_io_unmap(gcc_mapping, gcc_size);
}

static void
thyme_venus_pil_boot(void)
{
	uint32_t v_en = 0, c_en = 0;

	if (PE_parse_boot_argn("venus", &v_en, sizeof(v_en)) && v_en >= 1 && v_en <= 3) {
		/* The GDSC alone is insufficient: stock PIL also votes the core clock and interconnect. */
		thyme_venus_power_probe(v_en);
		thyme_pil_boot_one("venus", "venus.mdt", "venus.b%02u",
		    UMI_VENUS_PAS_ID, UMI_VENUS_PA, UMI_VENUS_SIZE,
		    false, &venus_ready, &venus_segments);
	}
	if (PE_parse_boot_argn("cdsp", &c_en, sizeof(c_en)) && c_en >= 1 && c_en <= 2) {
		if (c_en == 2 && (hakios_thyme_cdsp_publish == NULL || hakios_thyme_cdsp_kick == NULL ||
		    !hakios_thyme_cdsp_publish())) {
			hakios_tl("UMI cdsp: APSS SMP2P edge unavailable; PAS startup skipped\n");
			return;
		}
		uint32_t before;
		if (hakios_thyme_cdsp_smp2p != NULL) {
			bool sampled = hakios_thyme_cdsp_smp2p(&before);
			cdsp_smp2p_state = sampled ? (int)before : -1;
			cdsp_saw_not_ready = !sampled || (before & CDSP_SMP2P_READY) == 0;
			hakios_tl("UMI cdsp: SMP2P before PAS sampled=%d state=%#x\n",
			    sampled, (uint32_t)cdsp_smp2p_state);
		}
		thyme_pil_boot_one("cdsp", "cdsp.mdt", "cdsp.b%02u",
		    UMI_CDSP_PAS_ID, UMI_CDSP_PA, UMI_CDSP_SIZE,
		    true, &cdsp_ready, &cdsp_segments);
		if (cdsp_ready && c_en == 2) {
			hakios_thyme_cdsp_kick();
		}
		if (cdsp_ready && hakios_thyme_cdsp_smp2p != NULL) {
			cdsp_probe_ticks = CDSP_SMP2P_POLL_TICKS;
		}
	}
}

/* SM8250 APSS hardware watchdog (qcom,wdt@17c10000): probe, opt-in wdt=1 arming, and loader_return quiesce. */
void hakios_wdt_quiesce(void);

void
hakios_wdt_quiesce(void)
{
	if (venus_ready) {
		uint64_t ans = ~0ULL;
		int32_t ret = thyme_scm_call(UMI_SCM_PAS_SHUTDOWN, 1u, UMI_VENUS_PAS_ID, 0, 0, &ans);

		venus_ready = 0;
		hakios_tl("UMI venus: TZ SHUTDOWN ret=%d ans=%#llx\n", ret, ans);
	}
	if (cdsp_ready) {
		uint64_t ans = ~0ULL;
		int32_t ret = thyme_scm_call(UMI_SCM_PAS_SHUTDOWN, 1u, UMI_CDSP_PAS_ID, 0, 0, &ans);

		cdsp_ready = 0;
		hakios_tl("UMI cdsp: TZ SHUTDOWN ret=%d ans=%#llx\n", ret, ans);
	}
	if (wdt_base != 0 && wdt_armed) {
		keys_write(wdt_base, UMI_WDT_CTL, 0u);
		keys_write(wdt_base, UMI_WDT_RST, 1u);
		uint32_t ctl = keys_reg(wdt_base, UMI_WDT_CTL);

		wdt_ctl = (int)ctl;
		wdt_armed = 0;
		hakios_tl("UMI wdt: quiesced (ctl=%#x pets=%d)\n", ctl, wdt_pets);
	}
}

static void
thyme_wdt_probe(void)
{
	wdt_base = ml_io_map(UMI_WDT_PA, 0x1000UL);

	if (wdt_base == 0) {
		hakios_tl("UMI wdt: map failed\n");
		return;
	}
	uint32_t ctl = keys_reg(wdt_base, UMI_WDT_CTL);
	uint32_t sts = keys_reg(wdt_base, UMI_WDT_STS);
	uint32_t bark = keys_reg(wdt_base, UMI_WDT_BARK);
	uint32_t bite = keys_reg(wdt_base, UMI_WDT_BITE);

	wdt_ctl = (int)ctl;
	wdt_sts = (int)sts;
	hakios_tl("UMI wdt: 0x17c10000 ctl=%#x sts=%#x bark=%u bite=%u\n", ctl, sts, bark, bite);

	uint32_t wdt_en = 0;
	if (PE_parse_boot_argn("wdt", &wdt_en, sizeof(wdt_en)) && wdt_en == 1) {
		keys_write(wdt_base, UMI_WDT_CTL, 0u);
		keys_write(wdt_base, UMI_WDT_BARK, 524287u);
		keys_write(wdt_base, UMI_WDT_BITE, 1048575u);
		keys_write(wdt_base, UMI_WDT_RST, 1u);
		keys_write(wdt_base, UMI_WDT_CTL, 1u);
		ctl = keys_reg(wdt_base, UMI_WDT_CTL);
		sts = keys_reg(wdt_base, UMI_WDT_STS);
		wdt_ctl = (int)ctl;
		wdt_sts = (int)sts;
		wdt_armed = (ctl & 1u) ? 1 : 0;
		hakios_tl("UMI wdt: armed (bark=16s bite=32s ctl=%#x sts=%#x)\n", ctl, sts);
	}
}

/* Strictly read-only Synopsys DWC3 hardware telemetry probe (dwc3@a600000). */
static void
thyme_usb_dwc3_probe(void)
{
	vm_offset_t dwc3 = ml_io_map(UMI_USB3_DWC3_PA, 0x1000UL);

	if (dwc3 == 0) {
		hakios_tl("UMI usb_dwc3: map failed\n");
		return;
	}
	uint32_t id = keys_reg(dwc3, 0x120u);
	uint32_t hw0 = keys_reg(dwc3, 0x140u);
	uint32_t hw3 = keys_reg(dwc3, 0x14cu);
	uint32_t dsts = keys_reg(dwc3, 0x70cu);
	unsigned speed = dsts & 0x7u;
	unsigned link = (dsts >> 18) & 0xfu;

	usb_dwc3_id = (int)id;
	usb_dwc3_dsts = (int)dsts;
	hakios_tl("UMI usb_dwc3: id=%#x hw0=%#x hw3=%#x dsts=%#x speed=%u link=%u\n",
	    id, hw0, hw3, dsts, speed, link);
}

/* Strictly read-only I2C telemetry helper for QUP15 wired 33W (bq25970) & USB-C audio switch (fsa4480). */
static void
thyme_qup15_poll(bool initial)
{
	static bool qup15_refreshed;
	uint32_t saved_cfg[2], saved_io[2];

	if (!i2c_bus_take(south_tlmm, initial ? 15 : 0, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, saved_cfg, saved_io)) {
		return;
	}
	uint8_t part = (uint8_t)fastchg_part, adc[10] = { 0 }, fsa_id = (uint8_t)usbc_audio_id;
	int prev_st = usbc_switch_status;
	bool bq_ok = !initial || i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x66u, 0x13u, false, &part, 1);

	if (bq_ok && part != 0) {
		fastchg_part = (int)part;
		if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x66u, 0x16u, false, adc, sizeof(adc))) {
			int ibus = (int)(((uint16_t)adc[0] << 8) | adc[1]);
			int vbus = (int)(((uint16_t)adc[2] << 8) | adc[3]);
			int vac  = (int)(((uint16_t)adc[4] << 8) | adc[5]);
			int vbat = (int)(((uint16_t)adc[8] << 8) | adc[9]);

			fastchg_ibus_ma = ibus;
			fastchg_vbus_mv = vbus;
			fastchg_vbat_mv = vbat;
			if (initial) {
				hakios_tl("UMI fastchg: bq25970@0x66 part=%#x vbus=%d mV vac=%d mV vbat=%d mV ibus=%d mA\n",
				    part, vbus, vac, vbat, ibus);
			}
		} else if (initial) {
			hakios_tl("UMI fastchg: bq25970@0x66 part=%#x (ADC read failed)\n", part);
		}
	} else if (initial) {
		hakios_tl("UMI fastchg: bq25970@0x66 NACK\n");
	}
	if (initial) {
		if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x00u, false, &fsa_id, 1)) {
			uint8_t en = 0, sel = 0, st0 = 0, st1 = 0, jack = 0;

			(void)i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x04u, false, &en, 1);
			(void)i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x05u, false, &sel, 1);
			(void)i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x06u, false, &st0, 1);
			(void)i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x07u, false, &st1, 1);
			(void)i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x17u, false, &jack, 1);
			usbc_audio_id = (int)fsa_id;
			usbc_switch_status = ((int)st1 << 8) | (int)st0;
			hakios_tl("UMI usbc_audio: fsa4480@0x42 id=%#x en=%#x sel=%#x st=%#x/%#x jack=%#x\n",
			    fsa_id, en, sel, st0, st1, jack);
		}
	} else if (usbc_audio_id != 0) {
		uint8_t st0 = 0, st1 = 0;

		if (i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x06u, false, &st0, 1) &&
		    i2c_read_regs(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, 0x42u, 0x07u, false, &st1, 1)) {
			usbc_switch_status = ((int)st1 << 8) | (int)st0;
		}
		if (!qup15_refreshed || usbc_switch_status != prev_st) {
			qup15_refreshed = true;
			hakios_tl("UMI fastchg: refresh vbus=%d mV vbat=%d mV ibus=%d mA usbc_st=%#x\n",
			    fastchg_vbus_mv, fastchg_vbat_mv, fastchg_ibus_ma, usbc_switch_status);
		}
	}
	i2c_bus_release(south_tlmm, UMI_QUP15_SDA_PIN, UMI_QUP15_SCL_PIN, saved_cfg, saved_io);
}

/* Strictly read-only I2C telemetry probe for wired 33W (bq25970), wireless 30W (idtp9415/ln8282) & NFC (nq-nci). */
static void
thyme_charger_probe(void)
{
	uint32_t saved_cfg[2], saved_io[2];

	thyme_qup15_poll(true);

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
	if (north_tlmm != 0) {
		uint32_t ven_cfg = tlmm_cfg_rd(north_tlmm, UMI_NFC_VEN_PIN);
		uint32_t ven_io = tlmm_io_rd(north_tlmm, UMI_NFC_VEN_PIN);
		uint32_t firm_cfg = tlmm_cfg_rd(north_tlmm, UMI_NFC_FIRM_PIN);
		uint32_t firm_io = tlmm_io_rd(north_tlmm, UMI_NFC_FIRM_PIN);
		uint32_t irq_cfg = tlmm_cfg_rd(north_tlmm, UMI_NFC_IRQ_PIN);
		uint32_t irq_io = tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN);
		bool ven_toggled = false;

		hakios_tl("UMI nfc: pins ven(83)=%#x/%#x firm(110)=%#x/%#x irq(111)=%#x/%#x\n",
		    ven_cfg, ven_io, firm_cfg, firm_io, irq_cfg, irq_io);
		if (ven_cfg == 0) {
			hakios_tl("UMI nfc: gpio83 VEN cfg=0 (RAZ/suspended); skip toggling\n");
		} else {
			nfc_saved_ven_cfg = ven_cfg;
			nfc_saved_ven_io = ven_io;
			nfc_saved_firm_cfg = firm_cfg;
			nfc_saved_firm_io = firm_io;
			nfc_saved_valid = true;
			/* Drive FIRM=0, then pulse VEN LOW (10 ms) -> HIGH (25 ms) for clean NCI mode wake. */
			tlmm_io_wr(north_tlmm, UMI_NFC_FIRM_PIN, 0);
			tlmm_cfg_wr(north_tlmm, UMI_NFC_FIRM_PIN, (firm_cfg & ~0x3cu) | 0x200u);
			tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, 0);
			tlmm_cfg_wr(north_tlmm, UMI_NFC_VEN_PIN, (ven_cfg & ~0x3cu) | 0x200u);
			delay_for_interval(10, NSEC_PER_MSEC);
			tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, 2u);
			delay_for_interval(25, NSEC_PER_MSEC);
			ven_toggled = true;
		}
		if (i2c_bus_take(north_tlmm, 1, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io)) {
			static const uint8_t nci_reset[4] = { 0x20, 0x00, 0x01, 0x01 };
			bool tx_ok = false;

			for (unsigned attempt = 0; attempt < 3 && !tx_ok; attempt++) {
				tx_ok = i2c_write_raw(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN,
				    0x28u, nci_reset, sizeof(nci_reset));
				if (!tx_ok) {
					delay_for_interval(15, NSEC_PER_MSEC);
				}
			}
			nfc_present = tx_ok ? 1 : 0;
			hakios_tl("UMI nfc: nq-nci@0x28 write %s%s\n", tx_ok ? "ACK" : "NACK",
			    ven_toggled ? " (VEN pulsed)" : "");
			if (tx_ok) {
				uint8_t rsp[32] = { 0 };
				unsigned rsp_len = 0, waited = 0;

				while (waited < 50 && (tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) == 0) {
					delay_for_interval(1, NSEC_PER_MSEC);
					waited++;
				}
				if (waited >= 50) {
					delay_for_interval(20, NSEC_PER_MSEC);
				}
				if (nfc_read_packet(rsp, sizeof(rsp), &rsp_len)) {
					if (rsp[0] == 0x40 && rsp[1] == 0x00 && rsp_len >= 5u && rsp[2] >= 2u) {
						nfc_nci_ver = (int)rsp[4];
					}
					hakios_tl("UMI nfc: CORE_RESET rsp (%u ms irq=%u len=%u): %02x %02x %02x %02x %02x %02x\n",
					    waited, tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u, rsp_len,
					    rsp[0], rsp[1], rsp[2], rsp[3], rsp[4], rsp[5]);
					for (unsigned w = 0; w < 30 && (tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) == 0; w++) {
						delay_for_interval(1, NSEC_PER_MSEC);
					}
					if ((tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) != 0) {
						uint8_t ntf[32] = { 0 };
						unsigned ntf_len = 0;

						if (nfc_read_packet(ntf, sizeof(ntf), &ntf_len)) {
							if (ntf[0] == 0x60 && ntf[1] == 0x00 && ntf_len >= 6u && ntf[2] >= 4u) {
								nfc_nci_ver = (int)ntf[5];
							}
							hakios_tl("UMI nfc: CORE_RESET ntf (len=%u ver=%#x): %02x %02x %02x %02x %02x %02x %02x %02x\n",
							    ntf_len, nfc_nci_ver,
							    ntf[0], ntf[1], ntf[2], ntf[3], ntf[4], ntf[5], ntf[6], ntf[7]);
						}
					}
					/* Send NCI 2.0 or 1.x CORE_INIT_CMD to verify controller initialization. */
					uint8_t nci_init[5] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
					unsigned init_len = (nfc_nci_ver != 0 && nfc_nci_ver < 0x20) ? 3u : 5u;

					if (init_len == 3u) {
						nci_init[2] = 0x00;
					}
					if (i2c_write_raw(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN,
					    0x28u, nci_init, init_len)) {
						uint8_t init_rsp[64] = { 0 };
						unsigned init_rsp_len = 0, iw = 0;

						while (iw < 50 && (tlmm_io_rd(north_tlmm, UMI_NFC_IRQ_PIN) & 1u) == 0) {
							delay_for_interval(1, NSEC_PER_MSEC);
							iw++;
						}
						if (nfc_read_packet(init_rsp, sizeof(init_rsp), &init_rsp_len)) {
							hakios_tl("UMI nfc: CORE_INIT rsp (%u ms len=%u): %02x %02x %02x %02x %02x %02x %02x %02x\n",
							    iw, init_rsp_len, init_rsp[0], init_rsp[1], init_rsp[2], init_rsp[3],
							    init_rsp[4], init_rsp[5], init_rsp[6], init_rsp[7]);
						}
					}
				}
			} else {
				uint8_t found = 0;

				for (uint8_t a = 0x28u; a <= 0x2bu; a++) {
					if (i2c_probe_write_addr(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, a)) {
						found = a;
						break;
					}
				}
				hakios_tl("UMI nfc: QUP1 write scan 0x28..0x2b -> %#x\n", found);
			}
			i2c_bus_release(north_tlmm, UMI_QUP1_SDA_PIN, UMI_QUP1_SCL_PIN, saved_cfg, saved_io);
		}
		if (ven_toggled) {
			tlmm_io_wr(north_tlmm, UMI_NFC_VEN_PIN, ven_io & 2u);
			tlmm_cfg_wr(north_tlmm, UMI_NFC_VEN_PIN, ven_cfg);
			tlmm_io_wr(north_tlmm, UMI_NFC_FIRM_PIN, firm_io & 2u);
			tlmm_cfg_wr(north_tlmm, UMI_NFC_FIRM_PIN, firm_cfg);
		}
		if (nfc_present == 1) {
			int nmajor = cdevsw_add(-1, &nfc_cdevsw);

			if (nmajor >= 0 &&
			    devfs_make_node(makedev(nmajor, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-nfc") != NULL) {
				nfc_dev_ready = 1;
				hakios_tl("UMI nfc: /dev/hakios-nfc ready (NCI ver=%#x)\n", nfc_nci_ver);
			}
		}
		/* IR Blaster (ir-spi@0 on QUP2 gpio115..118, MOSI gpio116) */
		{
			uint32_t c115 = tlmm_cfg_rd(north_tlmm, 115u);
			uint32_t c116 = tlmm_cfg_rd(north_tlmm, UMI_IR_MOSI_PIN);
			uint32_t i116 = tlmm_io_rd(north_tlmm, UMI_IR_MOSI_PIN);
			uint32_t c117 = tlmm_cfg_rd(north_tlmm, 117u);
			uint32_t c118 = tlmm_cfg_rd(north_tlmm, 118u);

			hakios_tl("UMI ir: pins 115..118 cfg=%#x/%#x(io=%#x)/%#x/%#x\n",
			    c115, c116, i116, c117, c118);
			if (c116 != 0 || i116 != 0) {
				int major = cdevsw_add(-1, &ir_cdevsw);

				if (major >= 0 &&
				    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-ir") != NULL) {
					ir_ready = 1;
					hakios_tl("UMI ir: /dev/hakios-ir ready (38 kHz NEC TX on gpio116)\n");
				}
			}
		}
	}
	if (south_tlmm != 0) {
		uint32_t fpwr_cfg = tlmm_cfg_rd(south_tlmm, UMI_FOD_PWR_PIN);
		uint32_t fpwr_io = tlmm_io_rd(south_tlmm, UMI_FOD_PWR_PIN);
		uint32_t firq_cfg = tlmm_cfg_rd(south_tlmm, UMI_FOD_IRQ_PIN);
		uint32_t firq_io = tlmm_io_rd(south_tlmm, UMI_FOD_IRQ_PIN);
		uint32_t frst_cfg = tlmm_cfg_rd(south_tlmm, UMI_FOD_RST_PIN);
		uint32_t frst_io = tlmm_io_rd(south_tlmm, UMI_FOD_RST_PIN);

		hakios_tl("UMI fod: pins pwr(10)=%#x/%#x irq(23)=%#x/%#x rst(24)=%#x/%#x\n",
		    fpwr_cfg, fpwr_io, firq_cfg, firq_io, frst_cfg, frst_io);
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
						uint64_t epoch = 0;

						rtc_ready = true;
						hakios_tl("UMI rtc: PM8150 RTC ready ppid=%#x apid=%u seconds=%u\n",
						    rtc_candidates[c], rtc_apid, rtc_boot_seconds);
						if (PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) && epoch > 1700000000ULL) {
							umi_rtc_epoch_offset = (int64_t)epoch - (int64_t)rtc_boot_seconds;
							umi_rtc_synced = true;
							hakios_tl("UMI rtc: synced unix=%llu (rtc=%u offset=%lld)\n",
							    epoch, rtc_boot_seconds, umi_rtc_epoch_offset);
						}
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
	thyme_usb_dwc3_probe();
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
