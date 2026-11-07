/* UMI main-camera bring-up. cam=2 identifies; cam=3 also programs mode 3.
 * cam=4 verifies ISP reset; cam=6 also tests hardware TPG capture.
 * These diagnostics never start a sensor stream.
 * Board wiring and sequencing come from the live UMI DT, V2 sensor module,
 * and Android rear-1x GPIO/regulator/SPMI traces. The PM8150L GPIO sequence
 * below is the traced sequence, including its input/output mode and pull.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_camera_vreg(unsigned, bool);
extern uint64_t umi_camera_iomap_free(void);
int umi_camera_probe(void);
int umi_camera_prepare(void);
int umi_camera_isp_probe(void);
int umi_camera_capture_tpg(void);
int umi_camera_capture_sensor(void);
void umi_camera_ios_prepare(void);
extern int umi_camera_tpg(vm_offset_t, vm_offset_t);
extern int umi_camera_stream_arm(vm_offset_t, vm_offset_t, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, const char *);
extern bool umi_camera_stream_wait(unsigned);
extern void umi_camera_stream_report(const char *);
extern int umi_camera_stream_stop(void);
extern uint32_t umi_camera_stream_mean(void);
extern void umi_camera_stream_rearm(void);
extern void umi_camera_stream_advance(void);
extern int umi_camera_stream_copy(void *, unsigned, unsigned);
extern int umi_camera_stream_grab(void *, unsigned, unsigned);
extern int umi_camera_dma_reserve(unsigned, unsigned);
extern void umi_camera_dma_quiesce(bool);
extern const uint8_t *hakios_wifi_file(const char *, uint64_t *);
void umi_camera_probe_quiesce(void);

static struct {
	vm_offset_t core, config, observer, cc, gcc, csiphy, pin[4];
	uint32_t pin_cfg[4], pin_io[4], clock_cfg, clock_branch, gdsc;
	uint32_t gcc_ahb, gcc_xo;
	unsigned pins_valid, pin_io_valid;
	bool active, panic, reset_changed, clock_changed, gdsc_changed, gcc_saved, rails[4];
} uc;
/* The sensor mode cam_mode selected (the module file's resN table and entry). */
static struct {
	uint32_t width, height, stride, lanes, lane_assign, settle, pixel_clock, line_length, frame_length, data_type, cphy;
	uint64_t rate;
	unsigned mode;
} um;
/* A cam=5 cam_real session owns the sensor: it keeps uprobe's state across the
 * session, runs the module file's 1080p mode and copies frames on demand.
 */
static unsigned uios_force_mode;
static bool uios_on, uc_keep;
/* The exposure a session starts with (the capture stack's first request): the
 * S5K3T2 ignores exposure writes once it streams, so it is set before 0x0100.
 */
static uint32_t ustart_lines, ustart_gain;
/* cam_sensor=front: the S5K3T2 on CSIPHY 4 (DT cam-sensor@2) instead of the
 * S5KHMX: its own CCI pins, reset, MCLK4 (24 MHz from camcc's PLL2), power
 * order and slave address.
 */
static bool ufront, ufront_lite0, ufront_from_session;
static uint8_t uslave = 0x7a;
static uint32_t umclk_cmd = 0x501c, umclk_cfg = 0x5020, umclk_branch = 0x5034;
static struct {
	uint16_t ppid, channel;
	vm_offset_t write;
	uint8_t saved[8];
	bool changed;
} gates[3] = { { .ppid = 0x4c2 }, { .ppid = 0x4c0 }, { .ppid = 0x4c3 } };
static const uint8_t gate_regs[8] = { 0x41, 0x42, 0x45, 0x43, 0x40, 0x4a, 0x44, 0x46 };
static const uint8_t gate_on[8] = { 0, 4, 1, 0, 2, 0, 0x80, 0x80 };

/* The loader's IO window is small and shared with every later driver: a cam=5
 * cam_real session starts after they have taken theirs, so the UMI blocks are
 * mapped once early (umi_camera_ios_prepare) and handed back from here.
 */
static struct {
	vm_offset_t physical, mapped;
	vm_size_t size;
} umaps[12];
static unsigned umaps_used;

static vm_offset_t
umap(vm_offset_t physical, vm_size_t size)
{
	for (unsigned i = 0; i < umaps_used; ++i)
		if (umaps[i].physical == physical && umaps[i].size == size)
			return umaps[i].mapped;
	uint64_t available = umi_camera_iomap_free();
	uint64_t required = (size + (physical & 0x3fffUL) + 0x3fffUL) & ~0x3fffUL;
	hakios_tl("UMI camera: mapping %#lx + %#lx, IO window free=%llu\n",
	    (unsigned long)physical, (unsigned long)size, available);
	if (required > available) return 0;
	vm_offset_t mapped = ml_io_map(physical, size);
#if defined(__aarch64__)
	__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
	hakios_tl("UMI camera: mapping %#lx complete\n", (unsigned long)physical);
	if (mapped && umaps_used < sizeof(umaps) / sizeof(umaps[0])) {
		umaps[umaps_used].physical = physical;
		umaps[umaps_used].size = size;
		umaps[umaps_used].mapped = mapped;
		umaps_used++;
	}
	return mapped;
}

/* Reserve the session's mappings while the loader's IO window still has room. */
void
umi_camera_ios_prepare(void)
{
	(void)umap(0x0c440000, 0x1100);  /* PMIC arbiter core */
	(void)umap(0x0c40a000, 0x1000);  /* its config */
	(void)umap(0x0e600000, 0x10000); /* observer */
	(void)umap(0x0ad00000, 0x10000); /* camcc */
	(void)umap(0x00100000, 0x80000); /* gcc */
	(void)umap(0x0f500000, 0x80000); /* TLMM south */
	(void)umap(0x0f900000, 0x80000); /* TLMM north */
	(void)umap(0x0acd9000, 0x2200);  /* IFE-lite 0 (CSID at +0x200) */
	(void)umap(0x0acdb200, 0x2200);  /* IFE-lite 1, the front's lite block */
	(void)umap(0x0ac70000, 0x2000);  /* CSIPHY 3 */
	(void)umap(0x0ac72000, 0x2000);  /* CSIPHY 4 */
	/* The front's 2592x1940 mode is the largest frame a session runs. */
	if (umi_camera_dma_reserve(3008, 2256) != 0)
		hakios_tl("UMI camera: no frame buffer for the iOS session\n");
	hakios_tl("UMI camera: iOS session mappings reserved (%u blocks)\n", umaps_used);
}

static uint32_t
ur(vm_offset_t p, unsigned o)
{
	uint32_t v = *(volatile uint32_t *)(p + o);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
uw(vm_offset_t p, unsigned o, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(p + o) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
ums(unsigned n)
{
	if (uc.panic) delay(n * 1000);
	else delay_for_interval(n, NSEC_PER_MSEC);
}

static bool
upoll(vm_offset_t p, unsigned o, uint32_t mask, uint32_t value, unsigned us)
{
	for (unsigned i = 0; i < us; ++i) {
		if ((ur(p, o) & mask) == value) return true;
		delay(1);
	}
	return false;
}

/* The observer channel can read a peripheral regardless of writer ownership.
 * Writes require EE0 ownership and a still-matching PPID on every transfer.
 */
static bool
ugpio_io(unsigned g, uint8_t reg, uint8_t *byte, bool write)
{
	if (g >= 3 || !gates[g].write) return false;
	vm_offset_t channel = write ? gates[g].write : uc.observer + 0x80UL * gates[g].channel;

	/* Other PMIC clients (the torch's flash channels, the charger) share the
	 * arbiter: a transfer can come back with status bit 3 when it collides.
	 * Retry rather than taking the camera down for one lost transaction.
	 */
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		if (((ur(uc.core, 0x900 + 4 * gates[g].channel) >> 8) & 0xfff) != gates[g].ppid ||
		    (ur(uc.config, 0x700 + 4 * gates[g].channel) & 7) != 0) return false;
		if (!uc.panic) hakios_spmi_lock();
		if (write) uw(channel, 0x10, *byte);
		uw(channel, 0, (write ? 0 : 1u << 27) | (uint32_t)reg << 4);
		bool done = upoll(channel, 8, 1, 1, 2000);
		uint32_t status = ur(channel, 8);
		if (!write && done && !(status & 14)) *byte = (uint8_t)ur(channel, 0x18);
		if (!uc.panic) hakios_spmi_unlock();
		if (done && !(status & 14)) return true;
		hakios_tl("UMI camera: GPIO PPID %#x register %#x write=%u status=%#x (attempt %u)\n",
		    gates[g].ppid, reg, write, status, attempt + 1);
		if (!uc.panic) delay(2000);
	}
	return false;
}

static bool
ugpio_put(unsigned g, uint8_t reg, uint8_t value)
{
	bool allowed = false;
	for (unsigned i = 0; i < 8; ++i) allowed |= reg == gate_regs[i];
	uint8_t back = 0;
	if (!allowed || !ugpio_io(g, reg, &value, true) || !ugpio_io(g, reg, &back, false)) return false;
	if (back != value) hakios_tl("UMI camera: PPID %#x register %#x wrote %#x read %#x\n", gates[g].ppid, reg, value, back);
	return back == value;
}

static int
uvreg(unsigned rail, bool enable)
{
	int error = EBUSY;
	for (unsigned tries = 0; tries < 50; ++tries) {
		error = umi_rpmh_camera_vreg(rail, enable);
		if (error != EBUSY || uc.panic) return error;
		ums(5);
	}
	return error;
}

static bool
ugpio_prepare(void)
{
	uint32_t version = ur(uc.core, 0), count = ur(uc.core, 4) & 0x7ff;
	hakios_tl("UMI camera: PMIC arbiter version=%#x channels=%u\n", version, count);
	if ((version & 0xf0000000u) != 0x50000000u || count > 512) return false;
	if (!count) count = 512; /* UMI v5 table size, as used by the torch/key clients. */
	for (unsigned g = 0; g < 3; ++g) {
		bool found = false;
		for (unsigned c = 0; c < count; ++c) {
			if (((ur(uc.core, 0x900 + 4 * c) >> 8) & 0xfff) != gates[g].ppid ||
			    (ur(uc.config, 0x700 + 4 * c) & 7) != 0) continue;
			if (found) return false;
			gates[g].channel = (uint16_t)c;
			gates[g].write = umap(0x0c600000UL + 0x10000UL * c, 0x1000);
			found = gates[g].write != 0;
		}
		uint8_t type = 0, subtype = 0;
		if (!found || !ugpio_io(g, 4, &type, false) || !ugpio_io(g, 5, &subtype, false) ||
		    type != 0x10 || (subtype != 0x10 && subtype != 0x11)) return false;
		for (unsigned r = 0; r < 8; ++r)
			if (!ugpio_io(g, gate_regs[r], &gates[g].saved[r], false)) return false;
		if ((gates[g].saved[6] & 0x80) && (gates[g].saved[7] & 0x80)) {
			/* A supply left on by the last session (or by Android before the
			 * loader): the camera rails are ours alone in this session, and
			 * the saved state is what the release puts back.
			 */
			hakios_tl("UMI camera: PPID %#x drives high already; taking it over\n", gates[g].ppid);
		}
	}
	return true;
}

static bool
ugpio_on(unsigned g)
{
	gates[g].changed = true;
	for (unsigned r = 0; r < 8; ++r)
		if (!ugpio_put(g, gate_regs[r], gate_on[r])) return false;
	delay(135); /* DT regulator-enable-ramp-delay */
	return true;
}

static void
upin(unsigned p, bool high)
{
	uint32_t config = uc.pin_cfg[p] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12; /* Kona extended-GPIO ownership */
	uw(uc.pin[p], 4, high ? 2 : 0);
	uw(uc.pin[p], 0, config | 1u << 9); /* reset: GPIO output, no pull, 2 mA */
}

/* Open drain bit-banging on the actual CCI0 master1 pins. No CCI MMIO or
 * bus recovery clocks are used. Every clock-high wait has a 1 ms bound.
 */
static bool
uline(unsigned p, bool high)
{
	uint32_t config = uc.pin_cfg[p] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12;
	uw(uc.pin[p], 4, 0);
	uw(uc.pin[p], 0, config | 3u | (high ? 0 : 1u << 9));
	if (p == 3 && high && !upoll(uc.pin[p], 4, 1, 1, 1000)) return false;
	delay(5);
	return true;
}

static bool
ustart(void)
{
	return uline(2, true) && uline(3, true) && (ur(uc.pin[2], 4) & 1) && uline(2, false) && uline(3, false);
}

static void
ustop(void)
{
	(void)uline(2, false); (void)uline(3, true); (void)uline(2, true);
}

static bool
uput(uint8_t byte)
{
	for (unsigned b = 0; b < 8; ++b) {
		if (!uline(2, (byte & 0x80) != 0) || !uline(3, true) || !uline(3, false)) return false;
		byte <<= 1;
	}
	if (!uline(2, true) || !uline(3, true)) return false;
	bool ack = !(ur(uc.pin[2], 4) & 1);
	(void)uline(3, false);
	return ack;
}

static bool
uget(uint8_t *byte, bool ack)
{
	*byte = 0;
	if (!uline(2, true)) return false;
	for (unsigned b = 0; b < 8; ++b) {
		if (!uline(3, true)) return false;
		*byte = (uint8_t)((*byte << 1) | (ur(uc.pin[2], 4) & 1));
		if (!uline(3, false)) return false;
	}
	return uline(2, !ack) && uline(3, true) && uline(3, false) && uline(2, true);
}

static bool
usensor_read(uint16_t reg, uint16_t *value)
{
	uint8_t hi = 0, lo = 0;
	bool ok = ustart() && uput(uslave) && uput((uint8_t)(reg >> 8)) && uput((uint8_t)reg) &&
	    ustart() && uput(uslave | 1) && uget(&hi, true) && uget(&lo, false);
	ustop();
	*value = (uint16_t)((uint16_t)hi << 8 | lo);
	return ok;
}

/* Validate the complete container before issuing any initialization writes.
 * The diagnostic uses only init/res3; the stream-on table is never applied.
 */
static uint32_t
utab_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The wide camera's lens motor: a DW9800 on the sensor's I2C. The module file's
 * actuatorDriver names the chip's 7-bit address 0x0c; the CCI bus here is
 * addressed the 8-bit way everywhere else (the sensor answers at 0x7a), and on
 * this bus 0x0c is nobody: the motor answers at 0x18. Its ten-bit position is in
 * 0x03 and 0x04 within one transfer. The front camera's lens is fixed.
 */
#define VCM_ADDRESS     0x18

static bool
uvcm_write_at(uint8_t address, uint8_t reg, uint8_t value)
{
	bool ok = ustart() && uput(address) && uput(reg) && uput(value);

	ustop();
	return ok;
}

static bool
uvcm_write(uint8_t reg, uint8_t value)
{
	return uvcm_write_at(VCM_ADDRESS, reg, value);
}

static bool uvcm_read_at(uint8_t address, uint8_t reg, uint8_t *value);

/* The addresses the bus scan found, each asked whether it takes a position and
 * gives it back: the motor is one of them, at an address the module file did not
 * name for this phone.
 */
static void
uvcm_probe_neighbours(void)
{
	static const uint8_t candidates[] = { 0x18, 0x1c };

	for (unsigned i = 0; i < sizeof(candidates); i++) {
		uint8_t a = candidates[i], r0 = 0, hi = 0, lo = 0, hi2 = 0, lo2 = 0;

		if (!uvcm_read_at(a, 0x00, &r0)) {
			continue;
		}
		uvcm_write_at(a, 0x03, 0x01);
		uvcm_write_at(a, 0x04, 0x23);
		uvcm_read_at(a, 0x03, &hi);
		uvcm_read_at(a, 0x04, &lo);
		uvcm_write_at(a, 0x03, 0x02);
		uvcm_write_at(a, 0x04, 0x34);
		uvcm_read_at(a, 0x03, &hi2);
		uvcm_read_at(a, 0x04, &lo2);
		hakios_tl("UMI camera: CCI %#x: register 0 %#x; wrote 0x123 and read %#x%02x; wrote 0x234 and read %#x%02x\n",
		    a, r0, hi, lo, hi2, lo2);
	}
}

static bool uvcm_read(uint8_t reg, uint8_t *value);

static bool
uvcm_position(uint32_t position)
{
	/* The chip refuses a position while it is busy: the DW9807 driver polls
	 * status 0x05 until it reads zero before writing 0x03/0x04 (this family's
	 * sequence, from the module file, matches that driver's).
	 */
	for (unsigned i = 0; i < 10; i++) {
		uint8_t status = 0;

		if (uvcm_read(0x05, &status) && (int8_t)status <= 0) {
			break;
		}
		ums(1);
	}
	bool ok = ustart() && uput(VCM_ADDRESS) && uput(0x03) && uput((position >> 8) & 3) && uput(position & 0xff);

	ustop();
	return ok;
}

/* The motor's own registers back: a write the chip took and a lens that moves are
 * two different things, and the readback is the only way to tell them apart here.
 */
static bool
uvcm_read_at(uint8_t address, uint8_t reg, uint8_t *value)
{
	uint8_t v = 0;
	bool ok = ustart() && uput(address) && uput(reg) && ustart() && uput(address | 1) && uget(&v, false);

	ustop();
	*value = v;
	return ok;
}

static bool
uvcm_read(uint8_t reg, uint8_t *value)
{
	return uvcm_read_at(VCM_ADDRESS, reg, value);
}

/* Which addresses answer on the sensor's CCI bus once the motor's supply is on:
 * the module file's 0x0c is a claim, and the chip has been silent there.
 */
static void
uvcm_scan(void)
{
	for (uint8_t a = 0x08; a < 0x80; a += 2) {
		uint8_t v = 0;

		if (uvcm_read_at(a, 0x00, &v)) {
			hakios_tl("UMI camera: CCI address %#x answers: register 0 = %#x\n", a, v);
		}
	}
}

static bool
usensor_write(uint16_t reg, uint16_t value)
{
	bool ok = ustart() && uput(uslave) && uput(reg >> 8) && uput(reg) &&
	    uput(value >> 8) && uput(value);
	ustop();
	return ok;
}

static int
umode_prepare(unsigned version)
{
	/* The S5KHMX module has two versioned sets (3 and 0x13); the front
	 * S5K3T2 module one unversioned set, which its exporter writes as 0.
	 */
	if (ufront ? version != 0 : (version != 3 && version != 0x13)) return ENODEV;
	unsigned mode_n = ufront ? 2 : 3;      /* the front's 1280x720 mode */
	if (uios_force_mode) mode_n = uios_force_mode;
	else if (!PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n)) || mode_n > 6)
		mode_n = ufront ? 2 : 3;
	uint64_t size = 0;
	const char *name = ufront ? "camera-s5k3t2.tab" :
	    version == 3 ? "camera-s5khmx-v3.tab" : "camera-s5khmx-v19.tab";
	const uint8_t *file = hakios_wifi_file(name, &size);
	if (!file || size < 40 || size > 65536 || memcmp(file, "RWCTAB02", 8) ||
	    utab_u32(file+8) != (ufront ? 0x20 : 0x7a) || utab_u32(file+12) != 2 || utab_u32(file+16) != 2 ||
	    utab_u32(file+20) != 0 || utab_u32(file+24) != (ufront ? 0x3142 : 0x1ad0) ||
	    utab_u32(file+36) <= mode_n || utab_u32(file+36) > 16) return EINVAL;
	uint32_t modes_in_tab = utab_u32(file+36);
	uint64_t offset = 40 + (uint64_t)modes_in_tab * 48;
	if (offset > size) return EINVAL;
	const uint8_t *mode = file + 40 + mode_n*48;
	uint32_t width = utab_u32(mode), height = utab_u32(mode+4), lanes = utab_u32(mode+16);
	uint32_t cphy = utab_u32(mode+20), settle = utab_u32(mode+24), pclk = utab_u32(mode+28);
	uint32_t line = utab_u32(mode+32), frame = utab_u32(mode+36), dt = utab_u32(mode+44);
	if (!width || !height || width > 16384 || height > 16384 || width % 4 ||
	    !lanes || lanes > 4 || (cphy && lanes > 3) || cphy > 1 || dt != 0x2b || !pclk || frame <= 40) return EINVAL;
	for (unsigned p = 0; p < 2; ++p) {
		if (size-offset < 12) return EINVAL;
		uint64_t bytes = 12 + 12ull*utab_u32(file+offset+8);
		if (bytes > size-offset) return EINVAL;
		offset += bytes;
	}
	if (size-offset < 4) return EINVAL;
	unsigned tables = utab_u32(file+offset); offset += 4;
	if (!tables || tables > 32) return EINVAL;
	const uint8_t *selected[2] = { NULL, NULL };
	unsigned counts[2] = { 0, 0 };
	char res[8];
	snprintf(res, sizeof(res), "res%u", mode_n);
	for (unsigned t = 0; t < tables; ++t) {
		if (size-offset < 12) return EINVAL;
		const uint8_t *table = file+offset;
		unsigned count = utab_u32(table+8);
		uint64_t bytes = 12 + 12ull*count;
		if (bytes > size-offset) return EINVAL;
		int slot = !memcmp(table, "init\0\0\0\0", 8) ? 0 :
		    !strncmp((const char *)table, res, 8) ? 1 : -1;
		if (slot >= 0) {
			if (selected[slot] || !count || count > 4096) return EINVAL;
			selected[slot] = table+12; counts[slot] = count;
			for (unsigned i = 0; i < count; ++i) {
				const uint8_t *e = table+12+12*i;
				if (e[4] != 2 || e[5] != 2 || e[6] || e[7] || utab_u32(e+8) > 100000 ||
				    (e[0] == 0 && e[1] == 1 && (e[2] || e[3]))) return EINVAL;
			}
		}
		offset += bytes;
	}
	if (offset != size || !selected[0] || !selected[1]) return EINVAL;
	hakios_tl("UMI camera: initialize version %#x mode %u (%ux%u) without stream-on (%u+%u writes)\n",
	    version, mode_n, width, height, counts[0], counts[1]);
	for (unsigned t = 0; t < 2; ++t)
		for (unsigned i = 0; i < counts[t]; ++i) {
			const uint8_t *e = selected[t]+12*i;
			uint16_t reg = (uint16_t)(e[0] | e[1] << 8), value = (uint16_t)(e[2] | e[3] << 8);
			if (!usensor_write(reg, value)) {
				hakios_tl("UMI camera: table %u entry %u register %#x NACK\n", t, i, reg);
				return EIO;
			}
			unsigned us = utab_u32(e+8);
			if (us >= 1000) ums(us/1000);
			if (us%1000) delay(us%1000);
		}
	/* Restore the indirect-write page before verifying direct output registers. */
	if (!usensor_write(0x6028, 0x4000)) return EIO;
	static const unsigned regs[5] = { 0x034c, 0x034e, 0x0342, 0x0340, 0x0100 };
	const unsigned expected[5] = { width, height, line, frame, 0 };
	for (unsigned i = 0; i < 5; ++i) {
		uint16_t value = 0;
		if (!usensor_read(regs[i], &value)) return EIO;
		hakios_tl("UMI camera: mode register %#x=%#x expected %#x\n", regs[i], value, expected[i]);
		if (value != expected[i]) return EIO;
	}
	um.width = width; um.height = height; um.stride = ((width * 10 / 8 + 15) & ~15u);
	um.lanes = lanes; um.lane_assign = utab_u32(file+32); um.settle = settle; um.pixel_clock = pclk;
	um.line_length = line; um.frame_length = frame; um.data_type = dt;
	um.cphy = cphy;
	um.rate = (uint64_t)pclk * 10 / lanes; um.mode = mode_n;
	return 0;
}

/* camcc's PLL2 (Zonda, 1440 MHz) for the front S5K3T2's 24 MHz MCLK4: the
 * values of mainline's camcc-sm8250 (L 0x4b, config 0x08200920/0x05002015)
 * set and started as clk_zonda_pll_configure()/enable() do (its user_ctl is
 * 0x100 here; the yupik file's 0x301 carries extra post-divider bits).
 */
static bool upll2_on;

static bool
upll2_up(void)
{
	if (ur(uc.cc, 0x2000) & (1u | 1u << 20)) {
		hakios_tl("UMI camera: camcc PLL2 is running already (mode %#x)\n", ur(uc.cc, 0x2000));
		return false;
	}
	uw(uc.cc, 0x2004, 0x4b);
	uw(uc.cc, 0x2010, 0x08200920);
	uw(uc.cc, 0x2014, 0x05002015);
	uw(uc.cc, 0x2018, 0x00000000);
	uw(uc.cc, 0x200c, 0x00000100);
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) & ~2u);   /* into bypass */
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) & ~1u);   /* outputs off */
	uw(uc.cc, 0x2028, 0);                          /* standby */
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) | 4u);    /* out of reset */
	if (!(ur(uc.cc, 0x2000) & 4u)) return false;
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) | 2u);    /* out of bypass */
	delay(1);
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) | 4u);
	uw(uc.cc, 0x2028, 1);                          /* run */
	uint32_t lock = (ur(uc.cc, 0x201c) & (1u << 16)) ? 1u << 29 : 1u << 31;

	if (!upoll(uc.cc, 0x2000, lock, lock, 2000)) {
		hakios_tl("UMI camera: camcc PLL2 did not lock (mode %#x)\n", ur(uc.cc, 0x2000));
		return false;
	}
	uw(uc.cc, 0x200c, ur(uc.cc, 0x200c) | 0xfu);   /* early, main, even, odd */
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) | 1u);
	upll2_on = true;
	hakios_tl("UMI camera: camcc PLL2 locked at 1440 MHz (mode %#x)\n", ur(uc.cc, 0x2000));
	return true;
}

static void
upll2_down(void)
{
	if (!upll2_on) return;
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) & ~1u);
	uw(uc.cc, 0x200c, ur(uc.cc, 0x200c) & ~0xfu);
	uw(uc.cc, 0x2000, ur(uc.cc, 0x2000) & ~6u);
	uw(uc.cc, 0x2028, 0);
	upll2_on = false;
}

static bool
uclock_on(void)
{
	uc.clock_changed = true;
	/* The module file's power sequence asks for MCLK = 0x124f800 (19.2 MHz)
	 * for both sensors; the device tree's 24 MHz is only the clock table's
	 * turbo rate. MCLK4 therefore runs from the crystal like MCLK1 does.
	 */
	uw(uc.cc, umclk_cfg, (uc.clock_cfg & ~0x371fu) | 1); /* XO 19.2 MHz, divide by 1 */
	uw(uc.cc, umclk_cmd, ur(uc.cc, umclk_cmd) | 1);
	if (!upoll(uc.cc, umclk_cmd, 1, 0, 1000)) return false;
	uw(uc.cc, umclk_branch, uc.clock_branch | 1);
	if (!upoll(uc.cc, umclk_branch, 1u << 31, 0, 1000)) return false;
	uint32_t config = uc.pin_cfg[1] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12;
	uw(uc.pin[1], 0, config | 1u << 2 | 1u << 6); /* the sensor's MCLK pin, function 1, 4 mA */
	if (ufront)
		hakios_tl("UMI camera: MCLK4 cfg=%#x M=%#x N=%#x D=%#x branch=%#x pin cfg=%#x\n",
		    ur(uc.cc, umclk_cfg), ur(uc.cc, umclk_cfg + 4), ur(uc.cc, umclk_cfg + 8),
		    ur(uc.cc, umclk_cfg + 12), ur(uc.cc, umclk_branch), ur(uc.pin[1], 0));
	return true;
}

/* Kona-v2 ISP diagnostic: establish MMIO/reset operation at XO frequency.
 * No PLL, stream, DMA master, or SMMU entry is enabled at this stage.
 * All affected clock branches must be unused before their parents change.
 */
static struct {
	unsigned cmd; uint32_t saved; bool changed;
} isp_rcgs[] = {
	{ .cmd = 0x7058 }, /* slow AHB */
	{ .cmd = 0xc0f8 }, /* CAMNOC AXI */
	{ .cmd = 0xa068 }, /* C-PHY RX */
	{ .cmd = 0x6060 }, /* PHY3 timer (the sensor's CSIPHY: DT csiphy-sd-index 3) */
	{ .cmd = 0xc000 }, /* IFE-lite */
	{ .cmd = 0xc01c }, /* IFE-lite CSID */
};
static struct {
	unsigned reg; bool gcc, delay_only, changed;
} isp_branches[] = {
	{ .reg = 0xb02c, .gcc = true }, { .reg = 0xb030, .gcc = true },
	{ .reg = 0xc0f4 }, /* CPAS AHB */
	{ .reg = 0xc11c }, /* CAMNOC DCD XO */
	{ .reg = 0xc114 }, /* CAMNOC AXI */
	{ .reg = 0xc040 }, /* IFE-lite AHB */
	{ .reg = 0xc044 }, /* IFE-lite AXI */
	{ .reg = 0xc018 }, /* IFE-lite */
	{ .reg = 0xc034 }, /* IFE-lite CSID */
	{ .reg = 0xc03c }, /* IFE-lite C-PHY RX */
	{ .reg = 0x6078 }, /* PHY3 timer (camcc-kona: csi3phytimer_clk) */
	{ .reg = 0x607c }, /* PHY3 (camcc-kona: csiphy3_clk) */
};
static vm_offset_t isp_ife, isp_csid;
static bool isp_resetting;
static uint32_t isp_saved_ife_mask;

static void
uisp_down(void)
{
	umi_camera_dma_quiesce(uc.panic);
	if (isp_resetting) {
		/* Mask/clear only reset IRQs owned by this diagnostic. */
		uw(isp_ife, 0x28, 0); uw(isp_ife, 0x34, 1u << 17); uw(isp_ife, 0x24, 1);
		uw(isp_ife, 0x28, isp_saved_ife_mask);
		uw(isp_csid, 0x74, 0); uw(isp_csid, 0x78, 1); uw(isp_csid, 0x80, 1);
		isp_resetting = false;
	}
	for (unsigned n = sizeof(isp_branches)/sizeof(isp_branches[0]); n; --n) {
		unsigned i = n-1;
		if (!isp_branches[i].changed) continue;
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		uw(base, isp_branches[i].reg, ur(base, isp_branches[i].reg) & ~1u);
		isp_branches[i].changed = false;
	}
	for (unsigned n = sizeof(isp_rcgs)/sizeof(isp_rcgs[0]); n; --n) {
		unsigned i = n-1, cmd = isp_rcgs[i].cmd;
		if (!isp_rcgs[i].changed) continue;
		uw(uc.cc, cmd+4, isp_rcgs[i].saved);
		uw(uc.cc, cmd, ur(uc.cc, cmd) | 1);
		if (!upoll(uc.cc, cmd, 1, 0, 1000))
			hakios_tl("UMI camera: ISP clock %#x restore timed out\n", cmd);
		isp_rcgs[i].changed = false;
	}
}

static int
uisp_probe(void)
{
	/* Check every consumer before changing any source. This is not a way
	 * to take ownership of an ISP already used by another driver.
	 */
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		if (ur(base, isp_branches[i].reg) & 1) {
			hakios_tl("UMI camera: ISP branch %#x already enabled; refusing reparent\n", isp_branches[i].reg);
			return EBUSY;
		}
	}
	for (unsigned i = 0; i < sizeof(isp_rcgs)/sizeof(isp_rcgs[0]); ++i) {
		unsigned cmd = isp_rcgs[i].cmd;
		isp_rcgs[i].saved = ur(uc.cc, cmd+4);
		isp_rcgs[i].changed = true;
		uw(uc.cc, cmd+4, (isp_rcgs[i].saved & ~0x371fu) | 1); /* XO / 1 */
		uw(uc.cc, cmd, ur(uc.cc, cmd) | 1);
		if (!upoll(uc.cc, cmd, 1, 0, 1000)) return ETIMEDOUT;
	}
	/* Register access also needs IFE/CSID core clocks. Keep the entire
	 * external memory path (CAMNOC AXI and both GCC camera AXI branches)
	 * gated while inspecting inherited DMA/path enables.
	 */
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		if (isp_branches[i].gcc || isp_branches[i].reg == 0xc114) continue;
		vm_offset_t base = uc.cc;
		unsigned reg = isp_branches[i].reg;
		isp_branches[i].changed = true;
		uw(base, reg, ur(base, reg) | 1);
		if (isp_branches[i].delay_only) delay(10);
		if (!(ur(base, reg) & 1) || (!isp_branches[i].delay_only &&
		    !upoll(base, reg, 1u<<31, 0, 2000))) {
			hakios_tl("UMI camera: ISP branch %#x failed to start, value %#x\n", reg, ur(base, reg));
			return EIO;
		}
	}
	/* Physical regions overlap: map IFE once, derive CSID's 0x200 offset. */
	/* The front sensor is on CSIPHY 4, which the second lite block
	 * (ife-lite1 at 0xacdb200, csid-lite1 at +0x200) receives.
	 */
	uint32_t lite_base = 0x0acd9000;
	if (ufront && !ufront_lite0) lite_base = 0x0acdb200;
	if (!isp_ife) isp_ife = umap(lite_base, 0x2200);
	if (!isp_ife) return ENOMEM;
	isp_csid = isp_ife + 0x200;
	hakios_tl("UMI camera: reading IFE core version\n");
	uint32_t ife_version = ur(isp_ife, 0);
	hakios_tl("UMI camera: IFE core version %#x; reading local bus version\n", ife_version);
	uint32_t bus_version = ur(isp_ife, 0x1a00);
	hakios_tl("UMI camera: bus version %#x; reading CSID version\n", bus_version);
	uint32_t csid_version = ur(isp_csid, 0);
	hakios_tl("UMI camera: XO-clocked ISP IDs IFE=%#x bus=%#x CSID=%#x\n", ife_version, bus_version, csid_version);
	if (!ife_version || ife_version == ~0u || !bus_version || bus_version == ~0u ||
	    !csid_version || csid_version == ~0u) return ENODEV;
	/* An inherited reset-only mask is harmless and restored afterward.
	 * WM bit0 is enable; reset-default bit4 alone does not enable DMA. */
	hakios_tl("UMI camera: ISP inherited masks CSID top=%#x RX=%#x IFE=%#x/%#x/%#x\n",
	    ur(isp_csid, 0x74), ur(isp_csid, 0x24), ur(isp_ife, 0x28), ur(isp_ife, 0x2c), ur(isp_ife, 0x30));
	bool busy = ur(isp_csid, 0x74) || ur(isp_csid, 0x24) || (ur(isp_ife, 0x28) & ~(1u<<17)) ||
	    ur(isp_ife, 0x2c) || ur(isp_ife, 0x30);
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t cfg = ur(isp_csid, 0x200+0x100*i), mask = ur(isp_csid, 0x34+0x10*i), wm = ur(isp_ife, 0x1c00+0x100*i);
		hakios_tl("UMI camera: ISP inherited RDI%u cfg=%#x mask=%#x WM=%#x\n", i, cfg, mask, wm);
		busy |= (cfg & (1u<<31)) || mask || (wm & 1);
	}
	if (busy) return EBUSY;
	isp_saved_ife_mask = ur(isp_ife, 0x28);
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		if (isp_branches[i].changed) continue;
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		unsigned reg = isp_branches[i].reg;
		isp_branches[i].changed = true;
		uw(base, reg, ur(base, reg) | 1);
		if (isp_branches[i].delay_only) delay(10);
		if (!(ur(base, reg) & 1) || (!isp_branches[i].delay_only &&
		    !upoll(base, reg, 1u<<31, 0, 2000))) {
			hakios_tl("UMI camera: ISP branch %#x failed to start, value %#x\n", reg, ur(base, reg));
			return EIO;
		}
	}
	isp_resetting = true;
	uw(isp_csid, 0x28, 0x0fffffff);
	for (unsigned i = 0; i < 4; ++i) uw(isp_csid, 0x38+0x10*i, 0x7fff);
	uw(isp_csid, 0x104, 0x80);
	for (unsigned i = 0; i < 4; ++i) uw(isp_csid, 0x200+0x100*i, 2);
	/* The SM8250 driver's global reset performs hardware, then registers;
	 * cam_ife_csid_hw_ver1.c's reset completion checks top status bit 0.
	 */
	uint32_t rst_done = 1u;
	for (unsigned step = 0; step < 2; ++step) {
		bool hw = step == 0;
		uw(isp_csid, 0x78, rst_done); uw(isp_csid, 0x80, 1);
		if (!upoll(isp_csid, 0x70, rst_done, 0, 1000)) return EIO;
		if (hw) { uw(isp_csid, 0x74, 1); uw(isp_csid, 0x80, 1); }
		uw(isp_csid, 0x10, hw ? 0x1e : 1);
		if (!hw) { uw(isp_csid, 0x74, 1); uw(isp_csid, 0x80, 1); }
		if (!upoll(isp_csid, 0x70, rst_done, rst_done, 100000)) {
			hakios_tl("UMI camera: CSID %s reset timed out, IRQ %#x\n", hw ? "hardware" : "register", ur(isp_csid, 0x70));
			/* The second lite block's reset acknowledgement does not
			 * reach this status bit on the front's instance; the block
			 * still takes its configuration (cam=4 on the phone). */
			if (!ufront) return ETIMEDOUT;
		}
		uw(isp_csid, 0x78, rst_done); uw(isp_csid, 0x80, 1);
	}
	/* VFE lite48x/top-v3: command 2 resets HW+registers; bit17 ACK.
	 * Redwood's top-v2 command 0x3f9f/bit31 must never be used here.
	 */
	uw(isp_ife, 0x34, 1u<<17); uw(isp_ife, 0x24, 1);
	if (!upoll(isp_ife, 0x40, 1u<<17, 0, 1000) && !ufront) return EIO;
	uw(isp_ife, 0x28, 1u<<17); uw(isp_ife, 0xc, 2);
	if (!upoll(isp_ife, 0x40, 1u<<17, 1u<<17, 100000) && !ufront) {
		hakios_tl("UMI camera: IFE-lite48x reset timed out, IRQ %#x\n", ur(isp_ife, 0x40));
		return ETIMEDOUT;
	}
	if (ufront) hakios_tl("UMI camera: front lite1 block reset acknowledged IRQ %#x\n", ur(isp_ife, 0x40));
	hakios_tl("UMI camera: CSID480 and IFE-lite48x resets complete; DMA remains disabled\n");
	return 0;
}

/* ---- cam=7: the sensor's own frames ---------------------------------------------------------------- */

/*
 * The clocks Android's camera runs at: camcc's PLL0 (Lucid, 1200 MHz: L 0x3e,
 * alpha 0x8000, the values of camcc-kona.c and mainline's camcc-sm8250), then
 * the RCGs from its even (600) and odd (400) outputs at the rates the device
 * tree gives: ife_lite 400, ife_lite_csid 400, cphy_rx 400, csi1phytimer 300
 * (the settle count's 300 MHz time base), camnoc_axi 150, slow_ahb 80. The
 * branches are off while their sources change.
 */
#define UMI_CSIPHY3_PHYS	0x0ac70000UL

static bool upll0_on, ucsiphy_on;

static bool
upll0_up(void)
{
	static const struct { uint32_t offset, value; } config[] = {
		{ 0x04, 0x3e }, { 0x08, 0x44 }, { 0x40, 0x8000 }, { 0x18, 0x20485699 },
		{ 0x1c, 0x00002261 }, { 0x20, 0x329a699c }, { 0x0c, 0x00003100 },
		{ 0x10, 0x00000805 }, { 0x14, 0x00000000 },
	};

	if (ur(uc.cc, 0x00) & 1) {
		hakios_tl("UMI camera: camcc PLL0 is running already (mode %#x); not taking it\n", ur(uc.cc, 0x00));
		return false;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); ++i)
		uw(uc.cc, config[i].offset, config[i].value);
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) | 1u << 23); /* update bypass */
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) & ~1u);       /* outputs off */
	uw(uc.cc, 0x38, 0);                            /* standby */
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) | 1u << 2);    /* out of reset */
	uw(uc.cc, 0x38, 1);                            /* run */
	if (!upoll(uc.cc, 0x00, 1u << 31, 1u << 31, 2000)) {
		hakios_tl("UMI camera: camcc PLL0 did not lock (mode %#x)\n", ur(uc.cc, 0x00));
		return false;
	}
	uw(uc.cc, 0x0c, ur(uc.cc, 0x0c) | 7);          /* main, even, odd */
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) | 1);
	upll0_on = true;
	hakios_tl("UMI camera: camcc PLL0 locked at 1200 MHz (mode %#x)\n", ur(uc.cc, 0x00));
	return true;
}

static void
upll0_down(void)
{
	if (!upll0_on) return;
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) & ~1u);
	uw(uc.cc, 0x0c, ur(uc.cc, 0x0c) & ~7u);
	uw(uc.cc, 0x38, 0);
	uw(uc.cc, 0x00, ur(uc.cc, 0x00) & ~(1u << 2));
	upll0_on = false;
}

/* An RCG set (source, divider field 2n - 1, no M/N) and updated. */
static bool
urcg_set(uint32_t cmd, uint32_t source, uint32_t divider)
{
	uw(uc.cc, cmd + 4, (ur(uc.cc, cmd + 4) & ~0x371fu) | source << 8 | divider);
	uw(uc.cc, cmd, ur(uc.cc, cmd) | 1);
	if (!upoll(uc.cc, cmd, 1, 0, 1000)) {
		hakios_tl("UMI camera: RCG %#x does not take its configuration (cmd %#x)\n", cmd, ur(uc.cc, cmd));
		return false;
	}
	return true;
}

static bool
ucsh_put(unsigned offset, uint32_t value)
{
	uw(uc.csiphy, offset, value);
	uint32_t back = ur(uc.csiphy, offset);
	if (back != value) {
		hakios_tl("UMI camera: CSIPHY %#x <- %#x, but it reads %#x\n", offset, value, back);
		return false;
	}
	return true;
}

/* cam_csiphy_config_dev's common part and the C-PHY register set (cam_csiphy_1_2_1). */
static bool
ucsiphy_common(uint32_t lane_enable, bool cphy)
{
	if (!ucsh_put(0x814, lane_enable) || !ucsh_put(0x818, 0x01) || !ucsh_put(0x81c, cphy ? 0x52 : 0x02))
		return false;
	uw(uc.csiphy, 0x800, 0x03);
	delay(1);
	uw(uc.csiphy, 0x800, cphy ? 0x0e : 0x02);
	return true;
}

/* D-PHY: csiphy_2ph_v1_2_1_reg, every lane position (data lanes 0..3 and the clock lane) */
static bool
ucsiphy_dphy(unsigned settle)
{
	static const uint32_t lanes[5][2] = {
		{ 0x000, 0x900 }, { 0x700, 0xc80 }, { 0x200, 0xa00 }, { 0x400, 0xb00 }, { 0x600, 0xc00 },
	};

	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l][0], m = lanes[l][1];
		bool clock = l == 1;

		if (!ucsh_put(b + 0x30, 0x00) || !ucsh_put(m + 0x0, 0x05) ||
		    !ucsh_put(m + 0x8, 0x10) || !ucsh_put(m + 0x4, 0x00) ||
		    !ucsh_put(m + 0x4, 0x07) || !ucsh_put(b + 0x04, 0x0c) ||
		    !ucsh_put(b + 0x2c, 0x01) || !ucsh_put(b + 0x34, 0x07) ||
		    !ucsh_put(b + 0x10, 0x02) || !ucsh_put(b + 0x1c, 0x08) ||
		    !ucsh_put(b + 0x3c, 0xb8) || !ucsh_put(b + 0x08, settle & 0xff) ||
		    !ucsh_put(b + 0x00, clock ? 0x80 : 0x8d) ||
		    (clock && !ucsh_put(b + 0x0c, 0xa5)) ||
		    !ucsh_put(b + 0x38, clock ? 0x1f : 0xfe) || !ucsh_put(b + 0x14, 0x60) ||
		    (clock && !ucsh_put(b + 0x28, 0x04)) || !ucsh_put(0x800, 0x02) ||
		    !ucsh_put(0x884, 0x01)) {
			return false;
		}
	}
	hakios_tl("UMI camera CSIPHY: D-PHY configured, settle %u\n", settle);
	return true;
}

static bool
ucsiphy_cphy(unsigned settle, uint64_t bits_per_trio)
{
	static const uint32_t trios[3][2] = { { 0x100, 0x980 }, { 0x300, 0xa80 }, { 0x500, 0xb80 } };
	static const struct {
		uint64_t bandwidth;
		uint8_t r144, r164, r16c, rb4;
	} rates[3] = {
		{ 5700000000ull, 0x32, 0x0b, 0x25, 0 }, { 7980000000ull, 0xb2, 0x33, 0x1d, 0x03 },
		{ 10260000000ull, 0xb2, 0x33, 0x25, 0x02 },
	};
	unsigned r = 0;

	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!ucsh_put(m + 0x10, 0x00) || !ucsh_put(m + 0x14, 0x00) ||
		    !ucsh_put(m + 0x18, 0x00) || !ucsh_put(m + 0x10, 0x08) ||
		    !ucsh_put(m + 0x14, 0x08) || !ucsh_put(m + 0x18, 0x1a) ||
		    !ucsh_put(m + 0x0c, 0xaf)) return false;
		delay(1);
		if (!ucsh_put(b + 0x5c, 0x46) || !ucsh_put(b + 0x68, 0xa0) ||
		    !ucsh_put(b + 0x04, 0x06) || !ucsh_put(b + 0x0c, settle & 0xff) ||
		    !ucsh_put(b + 0x08, (settle >> 8) & 0xff) || !ucsh_put(b + 0x14, 0x20) ||
		    !ucsh_put(b + 0x50, 0x00) || !ucsh_put(b + 0x88, 0x00) ||
		    !ucsh_put(b + 0x8c, 0x7f) || !ucsh_put(b + 0x90, 0x7f) ||
		    !ucsh_put(b + 0x18, 0x3e) || !ucsh_put(b + 0x1c, 0x41) ||
		    !ucsh_put(b + 0x20, 0x41) || !ucsh_put(b + 0x24, 0x7f) ||
		    !ucsh_put(b + 0x28, 0x00) || !ucsh_put(b + 0x2c, 0x00) ||
		    !ucsh_put(b + 0x60, 0x02) || !ucsh_put(b + 0xcc, 0x41) ||
		    !ucsh_put(b + 0xdc, 0x50) || !ucsh_put(m + 0x04, 0x20) ||
		    !ucsh_put(m + 0x08, 0x05) || !ucsh_put(m + 0x00, 0x60) ||
		    !ucsh_put(m + 0x2c, 0x35) || !ucsh_put(m + 0x30, 0x01) ||
		    !ucsh_put(0x800, 0x0e) || !ucsh_put(0x884, 0x01)) return false;
	}
	while (r < 2 && bits_per_trio > rates[r].bandwidth) r++;
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!ucsh_put(b + 0x44, rates[r].r144) || !ucsh_put(b + 0x64, rates[r].r164) ||
		    !ucsh_put(b + 0x6c, rates[r].r16c) ||
		    (rates[r].rb4 != 0 && !ucsh_put(m + 0x34, rates[r].rb4))) return false;
		if (rates[r].rb4 != 0) delay(1);
	}
	hakios_tl("UMI camera CSIPHY: C-PHY row %u (up to %llu bit/s a trio) for %llu, settle %u\n",
	    r, rates[r].bandwidth, bits_per_trio, settle);
	return true;
}

static bool
ucsiphy_up(unsigned lanes, bool cphy, uint32_t lane_assign, unsigned settle, uint64_t bits_per_lane)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0;

	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > (cphy ? 2u : 3u)) {
			hakios_tl("UMI camera: CSIPHY lane assignment %#x not known here\n", lane_assign);
			return false;
		}
		lane_enable |= cphy ? 1u << (2 * n + 1) : 1u << (2 * n) | 0x80u;
	}
	if (!uc.csiphy) {
		uc.csiphy = umap(ufront ? 0x0ac72000UL : UMI_CSIPHY3_PHYS, 0x2000);
		if (!uc.csiphy) return false;
	}
	uw(uc.csiphy, 0x814, 0x00);
	delay(1);
	uw(uc.csiphy, 0x818, 0x00);
	uw(uc.csiphy, 0x81c, 0x00);
	uw(uc.csiphy, 0x800, 0x01);
	delay(1);
	uw(uc.csiphy, 0x800, 0x00);
	ucsiphy_on = true;
	if (!ucsiphy_common(lane_enable, cphy) ||
	    !(cphy ? ucsiphy_cphy(settle, bits_per_lane) : ucsiphy_dphy(settle))) return false;
	for (unsigned i = 0; i < 11; i++) {
		uw(uc.csiphy, 0x82c + 4 * i, irq_masks[i]);
		uw(uc.csiphy, 0x858 + 4 * i, 0xff);
	}
	uw(uc.csiphy, 0x828, 0x1);
	uw(uc.csiphy, 0x828, 0x0);
	hakios_tl("UMI camera: CSIPHY %u configured: %u %s, lanes %#x, settle %u\n", ufront ? 4 : 3, lanes,
	    cphy ? "trio(s)" : "lane(s)", lane_enable, settle);
	return true;
}

/* The PHY's own error status (11 registers from 0x8B0): a lane that saw
 * activity but lost it, a clock lane error, or nothing at all.
 */
static void
ucsiphy_status(const char *when)
{
	if (!uc.csiphy || !ucsiphy_on) return;
	char line[240];
	int n = snprintf(line, sizeof(line), "UMI camera CSIPHY status %s:", when);
	for (unsigned i = 0; i < 11 && n < (int)sizeof(line) - 8; i++)
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %#x", ur(uc.csiphy, 0x8b0 + 4 * i));
	hakios_tl("%s\n", line);
}

static void
ucsiphy_down(void)
{
	if (!ucsiphy_on) return;
	uw(uc.csiphy, 0x814, 0x00);
	delay(1);
	uw(uc.csiphy, 0x818, 0x00);
	uw(uc.csiphy, 0x81c, 0x00);
	uw(uc.csiphy, 0x800, 0x01);
	delay(1);
	uw(uc.csiphy, 0x800, 0x00);
	ucsiphy_on = false;
}

/* The branches off, PLL0 and the cameras' rates, the branches on, the PHY. */
static int
ustream_up(void)
{
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		if (!isp_branches[i].changed) continue;
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		uw(base, isp_branches[i].reg, ur(base, isp_branches[i].reg) & ~1u);
	}
	if (!upll0_up()) return EIO;
	static const struct { uint32_t cmd, source, divider; } rcgs[] = {
		{ 0x7058, 2, 14 },      /* slow_ahb 600 / 7.5 = 80 MHz */
		{ 0xc0f8, 2, 7 },       /* camnoc_axi 600 / 4 = 150 MHz */
		{ 0xa068, 3, 1 },       /* cphy_rx 400 / 1 */
		{ 0x6060, 2, 3 },       /* csi3phytimer 600 / 2 = 300 MHz */
		{ 0xc000, 3, 1 },       /* ife_lite 400 / 1 */
		{ 0xc01c, 3, 1 },       /* ife_lite_csid 400 / 1 */
	};
	uint32_t timer_rcg = ufront ? 0x6080 : 0x6060;   /* csi4phytimer or csi3phytimer */
	for (unsigned i = 0; i < sizeof(rcgs)/sizeof(rcgs[0]); ++i) {
		uint32_t cmd = rcgs[i].cmd == 0x6060 ? timer_rcg : rcgs[i].cmd;
		if (!urcg_set(cmd, rcgs[i].source, rcgs[i].divider)) return EIO;
	}
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		unsigned reg = isp_branches[i].reg;
		uw(base, reg, ur(base, reg) | 1);
		if (!(ur(base, reg) & 1) || (!isp_branches[i].delay_only && !upoll(base, reg, 1u<<31, 0, 2000))) {
			hakios_tl("UMI camera: stream clock %#x failed to start (%#x)\n", reg, ur(base, reg));
			return EIO;
		}
	}
	if (!ucsiphy_up(um.lanes, um.cphy != 0, um.lane_assign, um.settle, um.rate)) return EIO;
	return 0;
}

static void
uset_exposure(uint32_t shutter, uint32_t gain)
{
	/* SMIA exposure inside the sensor's group hold: coarse integration in
	 * lines (0x0202) and the analog gain code (0x0204, 32 is 1x). The hold is
	 * bit 0 of 0x0104 on the S5KHMX (the module file's 0x0100, bit 8, registers
	 * the values but the sensor keeps exposing with the old ones: the frame
	 * level stayed put across a 60 -> 1000 line change, with bit 0 it goes
	 * 29 -> 41 in the dark). The S5K3T2's front module specifies 0x0100.
	 */
	/* The sensor may be busy with a frame; a refused transfer is retried. */
	bool ok = false;

	for (unsigned tries = 0; tries < 3 && !ok; tries++) {
		ok = usensor_write(0x0104, ufront ? 0x0100 : 0x0001);
		ums(1);
		ok = ok && (!shutter || usensor_write(0x0202, (uint16_t)shutter));
		ok = ok && (!gain || usensor_write(0x0204, (uint16_t)gain));
		ums(1);
		ok = ok && usensor_write(0x0104, 0x0000);
		if (!ok) {
			ums(2);
		}
	}
	if (!ok) {
		hakios_tl("UMI camera: exposure %u lines, gain %#x not taken (3 tries)\n", shutter, gain);
	}
}

/* The cameras up and armed, the sensor set and streaming: what a session keeps. */
static int
ustream_begin(void)
{
	if (!um.width) {
		hakios_tl("UMI camera: no sensor mode was selected for a capture\n");
		return EINVAL;
	}
	int error = ustream_up();
	if (error) {
		hakios_tl("UMI camera: stream clock and PHY setup failed error=%d\n", error);
		return error;
	}
	error = umi_camera_stream_arm(isp_ife, isp_csid, um.width, um.height, um.stride, um.lanes,
	    um.lane_assign & ((1u << (4 * um.lanes)) - 1), ufront ? 4 : 3, um.cphy ? 1 : 0, 0, um.data_type, 1,
	    ufront ? "S5K3T2" : "S5KHMX");
	if (error) {
		hakios_tl("UMI camera: stream arm failed error=%d\n", error);
		return error;
	}
	uint32_t frame_lines = 0;
	(void)PE_parse_boot_argn("cam_frame_lines", &frame_lines, sizeof(frame_lines));
	if (frame_lines >= 64) {
		/* The frame length register (0x0340): the exposure's ceiling and the
		 * frame time. A still look at a dim scene uses a long one.
		 */
		(void)usensor_write(0x0104, 0x0100);
		(void)usensor_write(0x0340, (uint16_t)frame_lines);
		(void)usensor_write(0x0104, 0x0000);
		hakios_tl("UMI camera: frame length %u lines\n", frame_lines);
	}
	uint32_t shutter = 0, gain = 0;
	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	if (shutter || gain) {
		uset_exposure(shutter, gain);
		hakios_tl("UMI camera: exposure %u lines, gain %#x\n", shutter, gain);
		uint16_t readback = 0;
		if (usensor_read(0x0202, &readback)) hakios_tl("UMI camera: exposure register 0x0202=%#x\n", readback);
		if (usensor_read(0x0204, &readback)) hakios_tl("UMI camera: gain register 0x0204=%#x\n", readback);
	}
	if (ustart_lines || ustart_gain) {
		/* before the stream starts: the front sensor takes it only then */
		uset_exposure(ustart_lines, ustart_gain);
		ustart_lines = ustart_gain = 0;
	}
	if (!usensor_write(0x0100, 0x0100)) hakios_tl("UMI camera: stream-on write was not acknowledged\n");
	uint16_t stream_state = 0;
	if (usensor_read(0x0100, &stream_state))
		hakios_tl("UMI camera: sensor stream register 0x0100=%#x\n", stream_state);
	hakios_tl("UMI camera: sensor streaming mode %u (%ux%u)\n", um.mode, um.width, um.height);
	return 0;
}

static int
ucapture(void)
{
	int error = ustream_begin();
	if (error) return error;
	bool done;
	if (ufront) {
		/* Which CSID input the front's CSIPHY 4 reaches is not in the device
		 * tree: sweep the receiver's phy select and see whether packets appear.
		 */
		done = false;
		for (unsigned phy = 0; phy < 8 && !done; ++phy) {
			uw(isp_csid, 0x100, (um.lanes - 1) | (um.lane_assign & 0xffff) << 4 | (phy & 7) << 20);
			uw(isp_csid, 0x208, 1);
			done = umi_camera_stream_wait(200);
			hakios_tl("UMI camera: front phy %u: CSID ver %#x cfg %#x packets %u, rx irq %#x, ecc %#x, crc %u, "
			    "RDI cfg %#x status %#x bytes %u/%u, IFE irq %#x/%#x, done %u\n", phy, ur(isp_csid, 0),
			    ur(isp_csid, 0x100), ur(isp_csid, 0x160), ur(isp_csid, 0x20), ur(isp_csid, 0x164),
			    ur(isp_csid, 0x168), ur(isp_csid, 0x200), ur(isp_csid, 0x250), ur(isp_csid, 0x2e0),
			    ur(isp_csid, 0x2e4), ur(isp_ife, 0x40), ur(isp_ife, 0x44), done);
		}
		ucsiphy_status(done ? "after the first frame" : "after the phy sweep");
		umi_camera_stream_report(done ? "after the first frame" : "without a frame (phy sweep)");
	} else {
		done = umi_camera_stream_wait(1500);
		/* cam_shutter2/cam_gain2: change the exposure while streaming and let
		 * several frames land, to see whether the sensor takes it mid-stream
		 * (the iOS session's exposure changes go the same way).
		 */
		uint32_t shutter2 = 0, gain2 = 0;
		(void)PE_parse_boot_argn("cam_shutter2", &shutter2, sizeof(shutter2));
		(void)PE_parse_boot_argn("cam_gain2", &gain2, sizeof(gain2));
		if (done && (shutter2 || gain2)) {
			/* Bank 0 holds the stream's first frame; advance arms bank 1 for
			 * the next one (0xa5 = 165 if the WM did not write it), then the
			 * exposure changes and bank 0 is armed again.
			 */
			uint32_t bank0 = umi_camera_stream_mean();

			umi_camera_stream_advance();
			ums(100);
			uint32_t bank1 = umi_camera_stream_mean();
			/* the hold at bit 0 rather than the module's bit 8 */
			(void)usensor_write(0x0104, 0x0001);
			(void)usensor_write(0x0202, (uint16_t)shutter2);
			(void)usensor_write(0x0204, (uint16_t)gain2);
			(void)usensor_write(0x0104, 0x0000);
			uint16_t rb = 0;
			if (usensor_read(0x0202, &rb)) hakios_tl("UMI camera: mid-stream exposure register reads %#x\n", rb);
			if (usensor_read(0x0204, &rb)) hakios_tl("UMI camera: mid-stream gain register reads %#x\n", rb);
			umi_camera_stream_advance();
			ums(120);
			uint32_t again = umi_camera_stream_mean();
			hakios_tl("UMI camera: banks %u (first), %u (next), %u (after %u lines, gain %#x)\n",
			    bank0, bank1, again, shutter2, gain2);
		}
		umi_camera_stream_report(done ? "after the first frame" : "without a frame");
	}
	(void)usensor_write(0x0100, 0x0000);
	ums(30);
	return umi_camera_stream_stop();
}

/* ---- cam=5 cam_real: the sensor's frames for iOS ------------------------------------------------------ */

static int uprobe(unsigned stage);
static void ucleanup(void);

/* Between cam=5 sessions: the sensor the next session runs. The request is
 * kept until umi_camera_ios_start(), when the last session is down: the two
 * sensors use different CSIPHY blocks and pin sets, so the PHY mapping is
 * dropped and the mode table is re-read by uprobe().
 */
static bool unext_front, unext_pending;

void
umi_camera_ios_set_start_exposure(unsigned lines, unsigned gain)
{
	ustart_lines = lines;
	ustart_gain = gain;
}

void
umi_camera_ios_set_sensor(unsigned front)
{
	unext_front = front != 0;
	unext_pending = true;
	hakios_tl("UMI camera: the %s camera asked for the next iOS session\n", unext_front ? "front" : "rear");
}

int
umi_camera_ios_start(unsigned width, unsigned height)
{
	if (uc.active || uios_on) return EBUSY;
	if (unext_pending) {
		/* (the session's sensor is the app's choice: uprobe() must not
		 * overwrite it from the boot argument, which only sets the first)
		 */
		ufront_from_session = true;
		if (unext_front != ufront) {
			ufront = unext_front;
			uc.csiphy = 0;  /* the other CSIPHY block is mapped afresh */
		}
		unext_pending = false;
	}
	if (width < 64 || height < 64 || width > 4096 || height > 3072 || width % 4) return EINVAL;
	/* The module file's 1080p mode for the S5KHMX (the session gets a centre
	 * crop); the S5K3T2's 2592x1940 mode 1 is the app's front stream.
	 */
	uios_force_mode = ufront ? 1 : 3;
	int error = uprobe(8);
	uios_force_mode = 0;
	if (error) {
		uios_on = false;
		uc_keep = false;
		return error;
	}
	uios_on = true;
	/* The stream's first frame; the read that follows copies it. */
	ums(80);
	/* A room's first exposure until the program asks for its own. */
	uset_exposure(1100, 1024);
	hakios_tl("UMI camera: iOS session armed at %ux%u (the %ux%u sensor's centre crop)\n",
	    width, height, um.width, um.height);
	return 0;
}

int
umi_camera_ios_grab(void *dst, unsigned width, unsigned height)
{
	if (!uios_on) return ENODEV;
	/* The WM is given the other buffer and the frame already in flight is
	 * copied whole -- reading the bank the WM is writing tore the picture
	 * into bands of two frames.
	 */
	return umi_camera_stream_grab(dst, width, height);
}

void
umi_camera_ios_set_focus(unsigned position)
{
	if (position == 0xffffffffu) return;   /* the lens is fixed (or absent) */
	if (!uvcm_position(position & 0x3ff)) {
		hakios_tl("UMI camera: lens position %u not taken\n", position);
	}
}

void
umi_camera_ios_set_exposure(unsigned lines, unsigned gain)
{
	if (lines == 0 || lines > (um.frame_length > 40 ? um.frame_length - 40 : 1))
		lines = um.frame_length > 40 ? um.frame_length - 40 : 1;
	gain = gain < 32 ? 32 : gain > 0x800 ? 0x800 : gain;
	if (!uios_on) {
		/* Between sessions (the capture stack asks while the last one is
		 * going down): the next session starts with it.
		 */
		ustart_lines = lines;
		ustart_gain = gain;
		hakios_tl("UMI camera: exposure %u lines, gain %#x kept for the next session\n", lines, gain);
		return;
	}
	if (ufront) {
		/* The S5K3T2 takes the exposure only while it is not streaming, so the
		 * change is made between a stop and a start (a frame's gap), with the
		 * sensor given time to reach standby and the register read back.
		 */
		(void)usensor_write(0x0100, 0x0000);
		ums(30);
		uset_exposure(lines, gain);
		uint16_t rb = 0;
		if (usensor_read(0x0202, &rb)) {
			hakios_tl("UMI camera: front exposure %u lines (reads %#x), gain %#x\n", lines, rb, gain);
		}
		ums(10);
		(void)usensor_write(0x0100, 0x0100);
	} else {
		hakios_tl("UMI camera: rear exposure %u lines, gain %#x\n", lines, gain);
		uset_exposure(lines, gain);
	}
}

void
umi_camera_ios_stop(void)
{
	if (!uios_on) return;
	(void)usensor_write(0x0100, 0x0000);
	ums(30);
	(void)umi_camera_stream_stop();
	uios_on = false;
	uc_keep = false;
	ucleanup();
}

static void
ugpio_restore(unsigned g)
{
	if (!gates[g].changed) return;
	for (unsigned r = 0; r < 8; ++r)
		if (!ugpio_put(g, gate_regs[r], gates[g].saved[r]))
			hakios_tl("UMI camera: could not restore PPID %#x register %#x\n", gates[g].ppid, gate_regs[r]);
	gates[g].changed = false;
}

static void
ucleanup(void)
{
	if (!uc.active) return;
	ucsiphy_down();
	uisp_down();
	upll0_down();
	if (uc.clock_changed) {
		uw(uc.cc, umclk_branch, uc.clock_branch);
		(void)upoll(uc.cc, umclk_branch, 1u << 31, 1u << 31, 1000);
		uw(uc.cc, umclk_cfg, uc.clock_cfg);
		uw(uc.cc, umclk_cmd, ur(uc.cc, umclk_cmd) | 1);
		(void)upoll(uc.cc, umclk_cmd, 1, 0, 1000);
		uc.clock_changed = false;
	}
	if (ufront) upll2_down();
	if (uc.reset_changed) { upin(0, false); ums(1); uc.reset_changed = false; }
	if (ufront) {
		/* Reverse of the S5K3T2's order: CUSTOM1, VDIG, VIO, VANA. */
		ugpio_restore(1);
		if (uc.rails[3]) {
			int error = uvreg(3, false);
			hakios_tl("UMI camera: release front VDIG error=%d\n", error);
			uc.rails[3] = false;
		}
		if (uc.rails[1]) {
			int error = uvreg(1, false);
			hakios_tl("UMI camera: release front VIO error=%d\n", error);
			uc.rails[1] = false;
		}
		if (uc.rails[2]) {
			int error = uvreg(2, false);
			hakios_tl("UMI camera: release front VDIG alternative error=%d\n", error);
			uc.rails[2] = false;
		}
		if (uc.rails[0]) {
			int error = uvreg(0, false);
			hakios_tl("UMI camera: release front 2.85 V rail error=%d\n", error);
			uc.rails[0] = false;
		}
		ums(1);
		ugpio_restore(2);
		ugpio_restore(0);       /* the third gate the power-up took */
	} else {
	/* Reverse module power order: CUSTOM2, CUSTOM1, VDIG, VANA, VIO, VAF. */
	for (unsigned step = 0; step < 6; ++step) {
		if (step == 0 || step == 1 || step == 3) {
			ugpio_restore(step == 0 ? 2 : step == 1 ? 1 : 0);
		} else {
			unsigned rail = step == 2 ? 2 : step == 4 ? 1 : 0;
			if (uc.rails[rail]) {
				int error = uvreg(rail, false);
				hakios_tl("UMI camera: release supply %u error=%d\n", rail, error);
				uc.rails[rail] = false;
			}
		}
		ums(1);
	}
		if (!ufront) {
			(void)uvreg(4, false);
		}
	}
	for (unsigned p = 0; p < 4; ++p)
		if (uc.pins_valid & (1u << p)) {
			if (uc.pin_io_valid & (1u << p)) uw(uc.pin[p], 4, uc.pin_io[p] & 2);
			uw(uc.pin[p], 0, uc.pin_cfg[p]);
		}
	uc.pins_valid = uc.pin_io_valid = 0;
	/* Return CCI pins before collapsing their camera power domain. */
	if (uc.gdsc_changed) { uw(uc.cc, 0xc144, uc.gdsc & ~(1u << 31)); uc.gdsc_changed = false; }
	if (uc.gcc_saved) {
		uw(uc.gcc, 0xb040, (ur(uc.gcc, 0xb040) & ~1u) | (uc.gcc_xo & 1));
		uw(uc.gcc, 0xb008, (ur(uc.gcc, 0xb008) & ~1u) | (uc.gcc_ahb & 1));
		uc.gcc_saved = false;
	}
	uc.active = false;
}

void
umi_camera_probe_quiesce(void)
{
	uc.panic = true;
	ucleanup();
}

static int
uprobe(unsigned stage)
{
	if (uc.active) return EBUSY;
	uc.panic = false;
	uc.core = umap(0x0c440000, 0x1100);
	uc.config = umap(0x0c40a000, 0x1000);
	uc.observer = umap(0x0e600000, 0x10000);
	uc.cc = umap(0x0ad00000, 0x10000);
	uc.gcc = umap(0x00100000, 0x80000);
	vm_offset_t south = umap(0x0f500000, 0x80000);
	vm_offset_t north = umap(0x0f900000, 0x80000);
	static const unsigned hmx_pins[4] = { 62, 95, 103, 104 };
	/* cam-sensor@2 sits on the CCI controller at ac50000, whose master 0 is
	 * CCI_I2C_DATA2/CLK2 = GPIO105/106 (its node's gpios property), not on
	 * the ac4f000 controller the rear sensor uses.
	 */
	static const unsigned front_pins[4] = { 109, 98, 105, 106 };
	char which[16] = "";

	if (!ufront_from_session) {
		ufront = PE_parse_boot_argn("cam_sensor", which, sizeof(which)) && !strncmp(which, "front", 5);
	}
	uint32_t lite0_arg = 0;
	ufront_lite0 = ufront &&
	    PE_parse_boot_argn("cam_front_lite0", &lite0_arg, sizeof(lite0_arg)) && lite0_arg != 0;
	const unsigned *pins = ufront ? front_pins : hmx_pins;
	/* The lite block's last clock entries are the sensor's CSIPHY
	 * timer and PHY branches: CSIPHY 3 for the rear, CSIPHY 4 (RCG
	 * 0x6080, branches 0x6098/0x609c) for the front.
	 */
	isp_rcgs[3].cmd = ufront ? 0x6080 : 0x6060;
	isp_branches[10].reg = ufront ? 0x6098 : 0x6078;
	isp_branches[11].reg = ufront ? 0x609c : 0x607c;
	uslave = ufront ? 0x20 : 0x7a;
	umclk_cmd = ufront ? 0x5070 : 0x501c;
	umclk_cfg = umclk_cmd + 4;
	umclk_branch = ufront ? 0x5088 : 0x5034;
	if (!uc.core || !uc.config || !uc.observer || !uc.cc || !uc.gcc || !south || !north) return ENOMEM;
	uc.active = true;
	int error = EIO;
	uint16_t id = 0, revision = 0;
	/* gcc-kona.c marks these clocks critical. Both use BRANCH_HALT_DELAY:
	 * enable, verify the vote, and allow propagation instead of waiting on
	 * a halt status that this branch type does not implement reliably.
	 * CamCC register reads before the AHB vote cause an external abort.
	 */
	uc.gcc_ahb = ur(uc.gcc, 0xb008); uc.gcc_xo = ur(uc.gcc, 0xb040);
	uc.gcc_saved = true;
	hakios_tl("UMI camera: GCC camera AHB=%#x XO=%#x; enabling bus clocks\n", uc.gcc_ahb, uc.gcc_xo);
	uw(uc.gcc, 0xb008, uc.gcc_ahb | 1);
	uw(uc.gcc, 0xb040, uc.gcc_xo | 1);
	delay(10);
	if (!(ur(uc.gcc, 0xb008) & 1) || !(ur(uc.gcc, 0xb040) & 1)) goto done;
	uc.clock_cfg = ur(uc.cc, umclk_cfg); uc.clock_branch = ur(uc.cc, umclk_branch); uc.gdsc = ur(uc.cc, 0xc144);
	if ((uc.clock_branch & 1) || !ugpio_prepare()) {
		hakios_tl("UMI camera: MCLK1 busy or PM8150L GPIOs unavailable\n");
		error = EBUSY;
		goto done;
	}
	/* Stock CCI enables its Titan-top supply before selecting its pinctrl
	 * state. Preserve that ordering for CCI GPIO access, including reads.
	 */
	uc.gdsc_changed = true;
	hakios_tl("UMI camera: enabling Titan-top, initial GDSCR=%#x\n", uc.gdsc);
	uw(uc.cc, 0xc144, uc.gdsc & ~1u);
	if (!upoll(uc.cc, 0xc144, 1u << 31, 1u << 31, 2000)) {
		hakios_tl("UMI camera: Titan-top did not power on, GDSCR=%#x\n", ur(uc.cc, 0xc144));
		goto done;
	}
	for (unsigned p = 0; p < 4; ++p) {
		uc.pin[p] = (pins[p] < 64 ? south : north) + 0x1000UL * pins[p];
		hakios_tl("UMI camera: reading GPIO%u configuration\n", pins[p]);
		uc.pin_cfg[p] = ur(uc.pin[p], 0);
#if defined(__aarch64__)
		__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
		uc.pins_valid |= 1u << p;
		if ((uc.pin_cfg[p] & (1u << 11)) && !(uc.pin_cfg[p] & (1u << 12)))
			uw(uc.pin[p], 0, uc.pin_cfg[p] | 1u << 12);
		hakios_tl("UMI camera: GPIO%u cfg=%#x; reading level\n", pins[p], uc.pin_cfg[p]);
		uc.pin_io[p] = ur(uc.pin[p], 4);
#if defined(__aarch64__)
		__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
		hakios_tl("UMI camera: GPIO%u io=%#x\n", pins[p], uc.pin_io[p]);
		uc.pin_io_valid |= 1u << p;
	}
	uc.reset_changed = true;
	upin(0, false);
	if (ufront) {
		/* The S5K3T2 module's order: VANA, VIO, VDIG (+3 ms), CUSTOM1
		 * (+1 ms), reset (+2 ms), MCLK (+10 ms).
		 */
		/* The rear camera's 2.85 V rail goes first as well: the wide/tele
		 * fixed regulators are fed from the same board rail the rear module
		 * uses, and the front sensor is on those regulators' outputs.
		 */
		error = uvreg(0, true);
		if (error) { hakios_tl("UMI camera: front 2.85 V rail failed errno=%d\n", error); goto done; }
		uc.rails[0] = true;
		/* The rear camera's VDIG rail too: if the front node's pm8009_l4
		 * numbering is off, the sensor's VDIG is this one at the same 1.104 V.
		 */
		error = uvreg(2, true);
		if (error) { hakios_tl("UMI camera: front VDIG alternative failed errno=%d\n", error); goto done; }
		uc.rails[2] = true;
		/* All three PM8150L gate channels: the front node names the same
		 * wide/tele fixed regulators the rear camera uses (GPIO1/GPIO4), and
		 * powering the third (GPIO3) as well costs nothing.
		 */
		if (!ugpio_on(2) || !ugpio_on(0) || !ugpio_on(1)) goto done;
		ums(1);
		error = uvreg(1, true);                         /* VIO: PM8009 LDOF7 1.8 V */
		if (error) { hakios_tl("UMI camera: front VIO failed errno=%d\n", error); goto done; }
		uc.rails[1] = true;
		error = uvreg(3, true);                         /* VDIG: PM8009 LDOF4 1.104 V */
		if (error) { hakios_tl("UMI camera: front VDIG failed errno=%d\n", error); goto done; }
		uc.rails[3] = true;
		ums(3);
		if (!ugpio_on(1)) goto done;                    /* CUSTOM1: camera_wide_vana_ldo (PM8150L GPIO1) */
		ums(1);
		upin(0, true); ums(2);
		if (!uclock_on()) { hakios_tl("UMI camera: front MCLK4 startup failed\n"); goto done; }
		ums(10);
	} else {
		/* The module's first power step: the lens motor's supply (VAF, ldoc9
		 * at 2.8 V), ten milliseconds before the rest. Through the UMI RPMh
		 * path, not the table path's (its RSC is not mapped here and the
		 * vote hung the kernel).
		 */
		int vaf_error = uvreg(4, true);
		if (vaf_error) {
			hakios_tl("UMI camera: the lens motor's supply failed errno=%d\n", vaf_error);
		}
		ums(10);
		uvcm_scan();
		for (unsigned step = 0; step < 6; ++step) {
			if (step == 2 || step == 4 || step == 5) {
				unsigned g = step == 2 ? 0 : step == 4 ? 1 : 2;
				if (!ugpio_on(g)) goto done;
			} else {
				unsigned rail = step == 0 ? 0 : step == 1 ? 1 : 2;
				int rail_error = uvreg(rail, true);
				if (rail_error) {
					error = rail_error;
					hakios_tl("UMI camera: supply %u enable failed errno=%d\n", rail, error);
					goto done;
				}
				uc.rails[rail] = true;
			}
			ums(step == 0 ? 10 : 1);
		}
		upin(0, true); ums(3);
		if (!uclock_on()) { hakios_tl("UMI camera: MCLK1/Titan-top startup failed\n"); goto done; }
		ums(2);
	}
	if (ufront) {
		/* Nine clocks, then a stop: the bus-recovery sequence, in case the
		 * sensor's I2C state machine came up mid-byte.
		 */
		for (unsigned i = 0; i < 9; ++i) {
			(void)uline(3, false);
			(void)uline(3, true);
		}
		ustart();
		ustop();
		/* Both lines released high: if the sensor were holding one low the
		 * bus would read 0 here (unpowered sensor, or a stuck pin).
		 */
		(void)uline(2, true);
		(void)uline(3, true);
		hakios_tl("UMI camera: front CCI idle levels: SDA %u, SCL %u (GPIO%u/GPIO%u)\n",
		    ur(uc.pin[2], 4) & 1, ur(uc.pin[3], 4) & 1, pins[2], pins[3]);
	}
	for (unsigned retry = 0; retry < 3 && !usensor_read(0, &id); ++retry) {
		ums(10);
	}
	if (ufront && id != 0x3142) {
		/* The module file's 0x20 is the address this phone's stack uses; if
		 * nothing answers there, look for the sensor on the bus before
		 * giving up (a wrong address is the cheapest thing to disprove).
		 */
		uint8_t found = uslave;
		for (uint8_t a = 0x08; a < 0x7f; a += 2) {
			uint16_t probe_id = 0;
			if (a == uslave) continue;
			uslave = a;
			if (usensor_read(0, &probe_id) && probe_id != 0 && probe_id != 0xffff) {
				hakios_tl("UMI camera: front sensor answers at 8-bit %#x: ID %#x\n", a, probe_id);
				found = a;
				id = probe_id;
				break;
			}
			ums(1);
		}
		if (found != uslave) uslave = found;
	}
	if (ufront && id != 0x3142) {
		/* Reset polarity, if this module's "RESET" acts the other way round. */
		upin(0, false);
		ums(20);
		for (unsigned retry = 0; retry < 3 && !usensor_read(0, &id); ++retry) {
			ums(10);
		}
		if (id == 0x3142) hakios_tl("UMI camera: front sensor answers with reset low\n");
		upin(0, true);
	}
	if (ufront && id != 0x3142) {
		/* The other CCI0 master's pins, in case the port map is the reverse
		 * of the device tree's: the rear sensor is not powered in this run,
		 * so nothing else can answer there.
		 */
		uc.pin[2] = north + 0x1000UL * 103;
		uc.pin[3] = north + 0x1000UL * 104;
		for (unsigned retry = 0; retry < 3 && !usensor_read(0, &id); ++retry) {
			ums(5);
		}
		if (id == 0x3142) hakios_tl("UMI camera: front sensor answers on GPIO103/104\n");
	}
	if (ufront && id != 0x3142) {
		/* Swapped SDA/SCL on the device tree's pair, in case the pinctrl
		 * order lists the clock line first.
		 */
		uc.pin[2] = north + 0x1000UL * 102;
		uc.pin[3] = north + 0x1000UL * 101;
		for (unsigned retry = 0; retry < 3 && !usensor_read(0, &id); ++retry) {
			ums(5);
		}
		if (id == 0x3142) hakios_tl("UMI camera: front sensor answers on swapped SDA/SCL (102/101)\n");
	}
	if (id != (ufront ? 0x3142 : 0x1ad0)) {
		hakios_tl("UMI camera: %s ID read failed, got %#x (expected %#x)\n", ufront ? "S5K3T2" : "S5KHMX", id,
		    ufront ? 0x3142 : 0x1ad0);
		goto done;
	}
	uint16_t selector = 0;
	if (ufront) {
		hakios_tl("UMI camera: S5K3T2 ID %#x\n", id);
	} else {
		/* The lens motor's initialisation, the module file's: power down, up,
		 * ring control, SAC3 with its timing, then 0x190 and 0x12c.
		 */
		uvcm_scan();                    /* (the bus is up now: the sensor answers) */
		uvcm_probe_neighbours();
		bool wrote = uvcm_write(0x02, 0x01) && uvcm_write(0x02, 0x00) && uvcm_write(0x02, 0x02) &&
		    uvcm_write(0x06, 0x40) && uvcm_write(0x07, 0x07) && uvcm_position(0x190) && uvcm_position(0x12c);
		uint8_t mode = 0, hi = 0, lo = 0, m6 = 0, m7 = 0;
		bool read = uvcm_read(0x02, &mode) && uvcm_read(0x03, &hi) && uvcm_read(0x04, &lo) &&
		    uvcm_read(0x06, &m6) && uvcm_read(0x07, &m7);
		hakios_tl("UMI camera: lens motor initialised (DW9800 at 0x0c): writes %s, mode %#x register 6 %#x 7 %#x, position %#x%02x (%s)\n",
		    wrote ? "taken" : "REFUSED", mode, m6, m7, hi, lo, read ? "read back" : "NO ANSWER");
		uint32_t focus_pos = 0xffffffff;
		if (PE_parse_boot_argn("cam_focus", &focus_pos, sizeof(focus_pos)) && focus_pos != 0xffffffff) {
			(void)uvcm_position(focus_pos & 0x3ff);
			uint8_t fhi = 0, flo = 0;
			bool fr = uvcm_read(0x03, &fhi) && uvcm_read(0x04, &flo);
			hakios_tl("UMI camera: lens at %u (boot arg), register %#x%02x (%s)\n", focus_pos & 0x3ff, fhi, flo,
			    fr ? "read back" : "NO ANSWER");
		}
		/* Samsung's chip revision register is read separately; no table selection
		 * is inferred from it until it has been compared with the module selector.
		 */
		if (usensor_read(2, &revision)) hakios_tl("UMI camera: S5KHMX ID %#x, revision register %#x\n", id, revision);
		else hakios_tl("UMI camera: S5KHMX ID %#x; revision read did not answer\n", id);
		/* The module's EEPROM map appends a sensor read at 0x000e after
		 * 0x4000 EEPROM bytes. SensorVersionOffset selects that appended byte,
		 * not register 0x0002 and not EEPROM address 0x4000. Read the full word
		 * here to retain byte-order evidence before selecting a version table.
		 */
		if (usensor_read(0x000e, &selector))
			hakios_tl("UMI camera: module sensor-version source register 0x000e=%#x\n", selector);
		else hakios_tl("UMI camera: module sensor-version source did not answer\n");
	}
	error = stage >= 3 ? umode_prepare(selector >> 8) : 0;
	if (!error && stage >= 4) error = uisp_probe();
	/* cam=6 on the front runs the CSID's own test pattern through lite1: it
	 * proves the lite1 CSID/IFE path without the PHY or the sensor.
	 */
	if (!error && stage == 6) error = umi_camera_tpg(isp_ife, isp_csid);
	if (!error && stage == 7) error = ucapture();
	if (!error && stage == 8) {
		error = ustream_begin();
		if (!error) {
			uc_keep = true;        /* the session owns the cameras until ios_stop() */
			uios_on = true;
		}
	}
done:
	if (!uc_keep) ucleanup();
	if (stage == 7) hakios_tl("UMI camera: capture finished error=%d\n", error);
	else if (stage == 8) hakios_tl("UMI camera: iOS session start error=%d\n", error);
	else hakios_tl("UMI camera: sensor probe finished error=%d; sensor stream was not started\n", error);
	return error;
}

int
umi_camera_probe(void)
{
	return uprobe(2);
}

int
umi_camera_prepare(void)
{
	return uprobe(3);
}

int
umi_camera_isp_probe(void)
{
	return uprobe(4);
}

int
umi_camera_capture_tpg(void)
{
	return uprobe(6);
}

int
umi_camera_capture_sensor(void)
{
	return uprobe(7);
}
