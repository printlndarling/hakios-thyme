/*
 * What is on the phone's power-management chips, and what time their clock says. Only with boot-arg pmic=1.
 *
 * The phone's PMICs (the PMK8350 and PM7325 beside the SoC, with a PM8350C and others on the board) hang on
 * one serial bus, SPMI, and the SoC reaches them through the "PMIC arbiter" (dt: qcom,spmi@c440000). Each chip
 * is a set of small blocks ("peripherals", 256 registers each) with a type and a subtype in registers 0x04
 * and 0x05: a GPIO, a regulator, the power-on block, a temperature alarm, a flash LED driver, a charger, the
 * real-time clock. Android's drivers find theirs from the device tree. This one asks the arbiter what it has
 * and what each block says it is, and puts the answer in the log: an inventory, so that the next driver to
 * be written for one of these blocks starts from what the phone really has instead of from what a table
 * for another phone says (this phone's device tree has no node for its real-time clock, for instance).
 *
 * THIS DRIVER ONLY READS. It reads through the arbiter's "observer" channels exactly as drivers/input/hakios.buttons.c
 * does, one bus transfer at a time under the same lock (hakios_spmi_lock()), and writes only the command word
 * that asks the arbiter for a read. Nothing is written to a PMIC: they make the phone's supplies. Where the arbiter
 * answers "denied" or "failed" for a block, the line says so and the driver goes on.
 *
 * Register layout and the version (5) of the arbiter: Linux drivers/spmi/spmi-pmic-arb.c, as drivers/input/hakios.buttons.c
 * has it and has run it on this phone. A phone whose arbiter is another version gets one line and nothing else.
 *
 * The clock. The PMICs have a real-time clock that Android sets, and keeps while the phone is off (Linux:
 * rtc-pm8xxx.c). When the inventory finds a block of type 0x07 (the clock) it reads four bytes at register 0x48 of
 * the one with subtype 0x01 -- the seconds since 1970, little-endian, as Linux reads them -- twice, a little over a
 * second apart, and says whether the second is later than the first (it ticks), how many days that is, and what the
 * kernel's own time (the loader gives the kernel the host's time in epoch=) minus it comes to. The value is kept as
 * kern.hakios.rtc_seconds. The kernel's own clock is NOT set from it. (The first run on the phone showed why: the
 * counter reads 60110539 seconds, 695 days of counting, which as a date is 1971. It is a counter, not wall-clock time; see
 * the comment at the report.)
 *
 * Status (what has and has not been seen): RUN ON THE PHONE (POCO X5 Pro, 2026-10-27, four runs): the arbiter is version 0x50020000
 * (5.2) with 512 channels, 444 distinct peripherals; slaves 0 to 3 answer (0, 1 and 2 are the PMK8350, PM7325 and PM8350C, as the buttons and flashlight drivers have them), the
 * peripherals of slaves 4, 8 and 9 answer "failed" (no chip there: 98 of them); the clock is found where Linux has it
 * (slave 0 0x6100, type 0x07 subtype 0x01) and ticks once a second. The access is the buttons driver's, which is
 * known to work on this phone; what is new is the walk over all channels and the clock's registers (0x48 for the read), taken from Linux's rtc-pm8xxx.c
 * as remembered. The first run on the phone (2026-10-27) found the clock's blocks where Linux has them: slave 0 0x6100 is type 0x07
 * subtype 0x01 (the write block) and 0x6200 type 0x07 subtype 0x03 (the alarm); the type is 0x07, not the 0x06 first assumed. Nothing here has a recorded run on the phone until a
 * line below says so; it is not run in QEMU, which has no arbiter.
 *
 * Boot-args:
 *   pmic=1            do it (once, from a thread of its own)
 *   pmic_list=0       only the clock, not the inventory of every block
 *   pmic_charger=1    also dump the charger block's registers (slave 2, 0x1000..0x10ff), read only;
 *                     works without pmic=1
 *   pmic_charge=1     the same dump, and then set the USB input current limit from the APSD result
 *                     as the vendor driver does (SDP 500 mA, CDP 1.5 A, DCP 1.6 A), and when it
 *                     writes, pet the watchdog once; no float voltage, no fast-charge current
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* the arbiter, as drivers/input/hakios.buttons.c maps it */
#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))               /* [19:8] slave and peripheral */
#define ARB_MAX_CHANNELS        512
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_WDATA0         0x10
#define OBSERVER_WDATA1         0x14
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define READ_TIMEOUT_US         2000

static vm_offset_t arb_core, arb_observer;

static bool
pmic_mapped(void)
{
	if (arb_core == 0) {
		arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	}
	if (arb_observer == 0) {
		arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	}
	return arb_core != 0 && arb_observer != 0;
}

/* Read `count` (1 to 8) registers of the peripheral at channel `channel`, from register `reg`; NULL, or what went wrong. */
static const char *
spmi_read(uint32_t channel, uint16_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t ch = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2];

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	wr(ch + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(ch + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(ch + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(ch + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();

	if (!(status & STATUS_DONE)) {
		return "no answer";
	} else if (status & STATUS_DENIED) {
		return "denied";
	} else if (status & STATUS_FAILURE) {
		return "failed";
	} else if (status & STATUS_DROPPED) {
		return "dropped";
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* Write `count` (1 to 8) registers of the peripheral at channel `channel`; NULL, or what went wrong. */
static const char *
spmi_write(uint32_t channel, uint16_t reg, const uint8_t *bytes, uint32_t count)
{
	vm_offset_t ch = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2] = { 0, 0 };

	for (uint32_t i = 0; i < count; i++) {
		data[i / 4] |= (uint32_t)bytes[i] << (8 * (i % 4));
	}
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	wr(ch + OBSERVER_WDATA0, data[0]);
	if (count > 4) {
		wr(ch + OBSERVER_WDATA1, data[1]);
	}
	wr(ch + OBSERVER_COMMAND, (uint32_t)reg << 4 | (count - 1)); /* bit 27 clear: a write */
	do {
		status = rd(ch + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	hakios_spmi_unlock();

	if (!(status & STATUS_DONE)) {
		return "no answer";
	} else if (status & STATUS_DENIED) {
		return "denied";
	} else if (status & STATUS_FAILURE) {
		return "failed";
	} else if (status & STATUS_DROPPED) {
		return "dropped";
	}
	return NULL;
}

/* ---- the inventory --------------------------------------------------------------------------------------- */

#define REG_TYPE                0x04
#define RTC_TYPE                0x07
#define RTC_SUBTYPE_WRITE       0x01
#define PON_TYPE                0x01
#define PON_SUBTYPE             0x04  /* the phone DT: qcom,power-on@800 under the PM8150 */
#define PON_REG_FIRST           0x860 /* the block sits at 0x800 in the PMIC; its state registers are 0x860..0x86f */
#define PON_REG_COUNT           0x10
#define PON_MODEM_PWR_OFF       0x862
#define PON_MODEM_PWR_OFF_CTL   0x863
#define RTC_REG_READ            0x48

static int64_t rtc_seconds_value = -1;
static int pmic_peripherals_value;
SYSCTL_QUAD(_kern_hakios, OID_AUTO, rtc_seconds, CTLFLAG_RD | CTLFLAG_LOCKED, &rtc_seconds_value,
    "the PMIC real-time clock, seconds since 1970 as it read once at boot; -1: not read");
SYSCTL_INT(_kern_hakios, OID_AUTO, pmic_peripherals, CTLFLAG_RD | CTLFLAG_LOCKED, &pmic_peripherals_value, 0,
    "how many blocks of the power-management chips the arbiter showed us");

static uint8_t seen_type[ARB_MAX_CHANNELS][2];

static bool
read_clock(uint32_t channel, uint32_t *seconds)
{
	uint8_t b[4];
	const char *problem = spmi_read(channel, RTC_REG_READ, b, 4);

	if (problem != NULL) {
		say("pmic: the clock's seconds: %s\n", problem);
		return false;
	}
	*seconds = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	return true;
}

static void
clock_report(uint32_t channel)
{
	uint32_t first, second;
	clock_sec_t kernel_seconds;
	clock_usec_t kernel_micro;

	if (!read_clock(channel, &first)) {
		return;
	}
	delay_for_interval(1100, NSEC_PER_MSEC);
	if (!read_clock(channel, &second)) {
		return;
	}
	clock_get_calendar_microtime(&kernel_seconds, &kernel_micro);
	rtc_seconds_value = second;
	/*
	 * What this counter is, as the first run on the phone showed: it ticks once a second, and it is NOT wall-clock time (it
	 * read 60110539, which as a date is in 1971). Qualcomm phones keep the clock as a free-running counter in the PMIC, and
	 * Android's time service keeps the offset to real time in a file of its own; the counter's use to this kernel is a
	 * count of seconds that goes on through a reboot and a flat battery's worth of power-off. If the kernel was given the
	 * host's time (epoch=), the difference printed is that offset, which can be compared with the file's.
	 */
	say("pmic: the real-time clock reads %u, then %u a second and a tenth later (%s); that is %u days of counting, not a date;"
	    " the kernel's time minus it is %lld s\n", first, second, second > first ? "it ticks" : "IT DOES NOT TICK", second / 86400,
	    (long long)kernel_seconds - (long long)second);
}

/*
 * The power-on block's registers 0x860-0x86f, read-only. The block sits at
 * reg 0x800 in the PMIC (the phone's DT: qcom,power-on@800), so the modem
 * power-off pair the vendor's qpnp_pon_modem_pwr_off writes is the PMIC's
 * 0x862/0x863: 0x863 = 0, a pause, then 0x862 = the reason (1 = warm reset).
 * Nothing here writes them; this only says what they hold.
 */
static void
pon_report(uint32_t channel)
{
	for (unsigned base = PON_REG_FIRST; base < PON_REG_FIRST + PON_REG_COUNT; base += 8) {
		uint8_t b[8];
		const char *problem = spmi_read(channel, (uint16_t)base, b, 8);

		if (problem != NULL) {
			say("pmic: pon 0x%02x: %s\n", base, problem);
			continue;
		}
		say("pmic: pon 0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x\n", base, b[0], b[1], b[2], b[3], b[4], b[5],
		    b[6], b[7]);
	}
}

/*
 * The vendor's qpnp_pon_modem_pwr_off(1): clear the PON block's modem power-off
 * control (0x63), a short pause, then set the modem power-off bit (0x62). Its
 * ext-sdx55m first-power-on and power-down both call it with the same value --
 * it is the modem's power cycle, after which the AP raises AP2MDM_STATUS. Only
 * those two registers are written.
 * Returns 0, or a negative errno.
 */
int
hakios_pmic_pon_modem_reset(void)
{
	uint32_t channels, type = 1, want_sid = 0xffffffff;
	uint8_t value;
	const char *problem;

	/*
	 * pmic_modem_pon=N picks the reset type the vendor's enum names: 1 warm
	 * reset, 2 shutdown, 3 hard reset.  pmic_modem_pon_sid=S picks the SPMI
	 * slave whose PON block is written: the mainline thyme DTS puts the
	 * modem's PON at slave 8 (pmxprairie), while this function otherwise
	 * takes the first PON block it meets -- the AP's PM8150 at slave 0.
	 */
	(void)PE_parse_boot_argn("pmic_modem_pon", &type, sizeof(type));
	(void)PE_parse_boot_argn("pmic_modem_pon_sid", &want_sid, sizeof(want_sid));
	if (!pmic_mapped()) {
		return -ENODEV;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));
		uint16_t ppid = (uint16_t)((map >> 8) & 0xfff);
		uint8_t is[2];

		if (map == 0) {
			continue;
		}
		if (want_sid != 0xffffffff && (ppid >> 8) != want_sid) {
			continue;
		}
		if (spmi_read(n, REG_TYPE, is, 2) != NULL || is[0] != PON_TYPE || is[1] != PON_SUBTYPE) {
			continue;
		}
		say("pmic: the modem's PON: slave %u 0x%02x00 (channel %u), reset type %u\n", ppid >> 8,
		    ppid & 0xff, n, type);
		value = 0;
		problem = spmi_write(n, PON_MODEM_PWR_OFF_CTL, &value, 1);
		if (problem != NULL) {
			say("pmic: the modem's power-off control: %s\n", problem);
			return -EIO;
		}
		delay(50000);
		value = (uint8_t)type;
		problem = spmi_write(n, PON_MODEM_PWR_OFF, &value, 1);
		if (problem != NULL) {
			say("pmic: the modem's power-off bit: %s\n", problem);
			return -EIO;
		}
		return 0;
	}
	return -ENODEV;
}

/* The same block's registers, for a driver that has just pulsed it. */
void
hakios_pmic_pon_report(void)
{
	uint32_t channels;

	if (!pmic_mapped()) {
		say("pmic: pon: no arbiter\n");
		return;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint8_t is[2];

		if (rd(arb_core + ARB_CHANNEL(n)) == 0) {
			continue;
		}
		if (spmi_read(n, REG_TYPE, is, 2) == NULL && is[0] == PON_TYPE && is[1] == PON_SUBTYPE) {
			pon_report(n);
			return;
		}
	}
	say("pmic: pon: no power-on block found\n");
}

static bool list_all = true;
static bool charger_dump_enabled;
static bool charge_apply_enabled;

/*
 * The charger block, read only.  Slave 2 of the arbiter is the PM8150B (the charger, the fuel
 * gauge and the USB port controller); its charger peripheral answers type 0x02 subtype 0x80 at
 * base 0x1000, the block Qualcomm's SMB5 driver drives.  Nothing is written: the dump says what
 * the firmware left behind (input current limit, charging enable, status) before any driver of
 * ours touches it.  Boot-arg pmic_charger=1.
 */
#define CHARGER_TYPE            0x02
#define CHARGER_SUBTYPE         0x80
#define CHARGER_BASE            0x1000
#define CHARGER_SPAN            0x100

static void
block_report(const char *what, uint32_t channel, uint16_t base)
{
	uint8_t bytes[8];

	say("pmic: %s block, channel %u, registers %#x..%#x (read only)\n", what, channel, base, base + 0xFF);
	for (uint16_t reg = base; reg < base + CHARGER_SPAN; reg += 8) {
		const char *problem = spmi_read(channel, reg, bytes, 8);

		if (problem != NULL) {
			say("pmic:   %#06x: %s\n", reg, problem);
			continue;
		}
		say("pmic:   %#06x: %02x %02x %02x %02x %02x %02x %02x %02x\n", reg, bytes[0], bytes[1],
		    bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
	}
}

/* The arbiter's channel for slave 2's peripheral at `base` (ppid = slave << 8 | base >> 8);
 * ARB_MAX_CHANNELS when the table has none. */
static uint32_t
peripheral_channel(uint16_t ppid)
{
	uint32_t channels = rd(arb_core + ARB_FEATURES) & 0x7ff;

	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && (uint16_t)((map >> 8) & 0xfff) == ppid) {
			return n;
		}
	}
	return ARB_MAX_CHANNELS;
}

static const char *
spmi_update_bits(uint32_t channel, uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t byte;
	const char *problem = spmi_read(channel, reg, &byte, 1);

	if (problem != NULL) {
		return problem;
	}
	byte = (uint8_t)((byte & ~mask) | (value & mask));
	return spmi_write(channel, reg, &byte, 1);
}

/*
 * The input current limit follows the port, as the vendor's driver does (qpnp-smb5.c and
 * smb5-lib.c: SDP 500 mA, CDP 1.5 A, DCP 1.6 A; APSD result in 0x1308), and nothing else is
 * written: no float voltage, no fast-charge current.  On a high-current source the vendor's
 * override is USBIN_MODE_CHG plus "override after APSD" (0x1366 BIT(0), 0x1365 BIT(4)) with the
 * software ICL override clear (0x1342 BIT(0)); SDP keeps the hardware's own mode.  A port that
 * advertises standard current is left exactly at the 500 mA it already has -- the limit is the
 * port's, not the charger's.  On the write path the watchdog is petted once (0x1643 BIT(0)); the
 * firmware's 0x1653 has its bite wired not to disable charging, so this is courtesy, not a gate.
 */
#define USBIN_ICL_REG           0x1370
#define USBIN_ICL_OPTIONS_REG   0x1366
#define USBIN_LOAD_CFG_REG      0x1365
#define USBIN_CMD_IL_REG        0x1340
#define CMD_ICL_OVERRIDE_REG    0x1342
#define APSD_RESULT_STATUS_REG  0x1308
#define BARK_BITE_WDOG_PET_REG  0x1643

static void
charger_set_icl(uint32_t usbin)
{
	uint8_t apsd = 0, icl = 0;
	uint8_t target;
	const char *what;
	const char *problem = spmi_read(usbin, APSD_RESULT_STATUS_REG, &apsd, 1);

	if (problem != NULL) {
		say("pmic: charge: APSD result unreadable: %s\n", problem);
		return;
	}
	apsd &= 0x7f;
	switch (apsd) {
	case 0x01:
		what = "SDP";
		target = 0x0a;          /* 500 mA */
		break;
	case 0x04:
		what = "CDP";
		target = 0x1e;          /* 1.5 A */
		break;
	case 0x08:
		what = "DCP";
		target = 0x20;          /* 1.6 A */
		break;
	default:
		say("pmic: charge: APSD result %#x (%s); input current limit left alone\n", apsd,
		    apsd == 0x10 ? "floating" : apsd == 0x20 ? "QC2" : apsd == 0x40 ? "QC3" : "unknown");
		return;
	}
	if (spmi_read(usbin, USBIN_ICL_REG, &icl, 1) == NULL && icl == target) {
		say("pmic: charge: %s, input current limit already %u mA\n", what, (unsigned)target * 50);
		return;
	}
	if (apsd != 0x01) {
		problem = spmi_update_bits(usbin, USBIN_ICL_OPTIONS_REG, 1u << 0, 1u << 0);
		if (problem == NULL) {
			problem = spmi_update_bits(usbin, USBIN_LOAD_CFG_REG, 1u << 4, 1u << 4);
		}
		if (problem == NULL) {
			problem = spmi_update_bits(usbin, CMD_ICL_OVERRIDE_REG, 1u << 0, 0);
		}
		if (problem != NULL) {
			say("pmic: charge: %s: the override: %s\n", what, problem);
			return;
		}
	}
	problem = spmi_write(usbin, USBIN_ICL_REG, &target, 1);
	if (problem == NULL) {
		problem = spmi_update_bits(usbin, USBIN_CMD_IL_REG, 1u << 0, 0);    /* not suspended */
	}
	if (problem != NULL) {
		say("pmic: charge: %s: %s\n", what, problem);
		return;
	}
	if (spmi_read(usbin, USBIN_ICL_REG, &icl, 1) == NULL) {
		say("pmic: charge: %s, input current limit set to %u mA (reads back %u mA)\n", what,
		    (unsigned)target * 50, (unsigned)icl * 50);
	} else {
		say("pmic: charge: %s, input current limit written, read-back failed\n", what);
	}
	{
		uint32_t misc = peripheral_channel((uint16_t)((2 << 8) | 0x16));
		uint8_t pet = 1;

		if (misc != ARB_MAX_CHANNELS) {
			(void)spmi_write(misc, BARK_BITE_WDOG_PET_REG, &pet, 1);
		}
	}
}

/* The blocks the charger's state lives in (smb5-reg.h): the charger itself, the DCDC block with
 * the ICL status, the USB input where the input current limit is, the Type-C port controller and
 * the MISC block with the watchdog.  Each is its own SPMI peripheral and must be read through its
 * own channel -- reading 0x1370 through the charger's channel answers 0x1070 again, the arbiter
 * taking only the low address byte.  Still read only. */
static void
charger_report(uint32_t charger_channel)
{
	static const struct {
		const char *what;
		uint16_t base;
	} blocks[] = {
		{ "charger", 0x1000 },
		{ "DCDC", 0x1100 },
		{ "USBIN", 0x1300 },
		{ "Type-C", 0x1500 },
		{ "MISC", 0x1600 },
	};

	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		uint16_t ppid = (uint16_t)((2 << 8) | (blocks[i].base >> 8));
		uint32_t channel = i == 0 ? charger_channel : peripheral_channel(ppid);

		if (channel == ARB_MAX_CHANNELS) {
			say("pmic: no %s peripheral (slave 2 0x%02x00) to read\n", blocks[i].what,
			    blocks[i].base >> 8);
			continue;
		}
		block_report(blocks[i].what, channel, blocks[i].base);
	}
	if (charge_apply_enabled) {
		uint32_t usbin = peripheral_channel((uint16_t)((2 << 8) | 0x13));

		if (usbin != ARB_MAX_CHANNELS) {
			charger_set_icl(usbin);
		} else {
			say("pmic: charge: no USBIN peripheral, nothing set\n");
		}
	}
}

static void
pmic_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	bool list = list_all;
	uint32_t version, channels, found = 0, rtc_channel = 0, pon_channel = 0, charger_channel = 0;
	bool rtc_found = false, pon_found = false, charger_found = false;
	uint16_t ppids[ARB_MAX_CHANNELS];
	uint16_t channel_of[ARB_MAX_CHANNELS];

	if (!pmic_mapped()) {
		say("pmic: could not map the PMIC arbiter\n");
		goto out;
	}
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	say("pmic: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("pmic: this driver knows version 5 of the arbiter only: nothing read\n");
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	/* one entry for each distinct slave/peripheral, with the first channel that reaches it */
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));
		uint16_t ppid = (uint16_t)((map >> 8) & 0xfff);
		bool again = false;

		if (map == 0) {
			continue;
		}
		for (uint32_t i = 0; i < found; i++) {
			if (ppids[i] == ppid) {
				again = true;
				break;
			}
		}
		if (!again) {
			ppids[found] = ppid;
			channel_of[found] = (uint16_t)n;
			found++;
		}
	}
	pmic_peripherals_value = (int)found;
	say("pmic: %u distinct peripherals in the arbiter's table; reading the type of each\n", found);
	for (uint32_t i = 0; i < found; i++) {
		uint8_t is[2] = { 0xff, 0xff };
		const char *problem = spmi_read(channel_of[i], REG_TYPE, is, 2);

		seen_type[i][0] = is[0];
		seen_type[i][1] = is[1];
		if (list) {
			if (problem != NULL) {
				say("pmic:   slave %u 0x%02x00 (channel %u): %s\n", ppids[i] >> 8, ppids[i] & 0xff, channel_of[i], problem);
			} else {
				say("pmic:   slave %u 0x%02x00 (channel %u): type %#04x subtype %#04x\n", ppids[i] >> 8, ppids[i] & 0xff,
				    channel_of[i], is[0], is[1]);
			}
		}
		if (problem == NULL && is[0] == RTC_TYPE && is[1] == RTC_SUBTYPE_WRITE && !rtc_found) {
			rtc_found = true;
			rtc_channel = channel_of[i];
			say("pmic: the real-time clock is slave %u 0x%02x00 (type %#x, subtype %#x)\n", ppids[i] >> 8, ppids[i] & 0xff,
			    is[0], is[1]);
		}
		if (problem == NULL && is[0] == PON_TYPE && is[1] == PON_SUBTYPE && !pon_found) {
			pon_found = true;
			pon_channel = channel_of[i];
			say("pmic: the power-on block is slave %u 0x%02x00 (type %#x, subtype %#x)\n", ppids[i] >> 8, ppids[i] & 0xff,
			    is[0], is[1]);
		}
		if (problem == NULL && is[0] == CHARGER_TYPE && is[1] == CHARGER_SUBTYPE && !charger_found) {
			charger_found = true;
			charger_channel = channel_of[i];
			say("pmic: the charger block is slave %u 0x%02x00 (type %#x, subtype %#x)\n", ppids[i] >> 8,
			    ppids[i] & 0xff, is[0], is[1]);
		}
	}
	if (rtc_found) {
		clock_report(rtc_channel);
	} else {
		say("pmic: none of the %u blocks says it is a real-time clock (type %#x, subtype %#x): none read\n", found, RTC_TYPE,
		    RTC_SUBTYPE_WRITE);
	}
	if (pon_found) {
		pon_report(pon_channel);
	} else {
		say("pmic: none of the %u blocks says it is a power-on block (type %#x, subtype %#x): none read\n", found, PON_TYPE,
		    PON_SUBTYPE);
	}
	if (charger_dump_enabled) {
		if (charger_found) {
			charger_report(charger_channel);
		} else {
			say("pmic: no charger block (type %#x, subtype %#x) to read\n", CHARGER_TYPE, CHARGER_SUBTYPE);
		}
	}
out:
	return;
}

__startup_func
static void
pmic_init(void)
{
	uint32_t enabled = 0, list = 1, charger = 0, charge = 0;

	if (!PE_parse_boot_argn("pmic", &enabled, sizeof(enabled))) {
		enabled = 0;
	}
	(void)PE_parse_boot_argn("pmic_list", &list, sizeof(list));
	(void)PE_parse_boot_argn("pmic_charger", &charger, sizeof(charger));
	(void)PE_parse_boot_argn("pmic_charge", &charge, sizeof(charge));
	charge_apply_enabled = charge != 0;
	charger_dump_enabled = charger != 0 || charge_apply_enabled;
	if (!enabled && !charger_dump_enabled) {
		return;
	}
	/* the dump alone does not list every block; pmic=1 still does */
	list_all = enabled && list != 0;
	/* once, from a thread call: the reads wait on the bus, and that is no place for the startup thread */
	thread_call_enter(thread_call_allocate(pmic_run, NULL));
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pmic_init);
