/*
 * THYME DSI backlight control.
 *
 * The panel selected by the boot firmware is Xiaomi's "42 02 0a" command-mode
 * AMOLED panel. Its device tree says bl_ctrl_dcs and a 12-bit brightness range
 * (0 through 0xfff), so this driver sends DCS 0x51 through the DSI command DMA
 * path the firmware left enabled.
 *
 * The bootloader already powers the panel and its clocks. This driver only
 * verifies that the display block and its register bus are alive, maps DSI0,
 * and keeps the programmed brightness in kern.hakios.brightness. The userspace
 * CoreBrightness hooks translate iOS slider changes into writes to that sysctl.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.board.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

#define GCC_PHYS                   hakios_board_addr("gcc", 0x00100000UL)
#define DISPCC_PHYS                hakios_board_addr("dispcc", 0x0af00000UL)
#define DSI0_PHYS                  hakios_board_addr("dsi0", 0x0ae94000UL)

#define GCC_DISP_AHB_CBCR          0x0b00c
#define DISPCC_MDSS_CORE_GDSCR     0x3000
#define DISPCC_MDSS_AHB_CBCR       0x2080

#define DSI_CTRL                   0x0004
#define DSI_STATUS                 0x0008
#define DSI_CMD_DMA_CTRL           0x0038
#define DSI_CMD_DMA_CTRL_LOW_POWER (1u << 26)   /* the device tree's dsi_lp_mode for the timing switch */
#define DSI_COMMAND_MODE_DMA_CTRL  0x003c
#define DSI_DMA_CMD_LENGTH         0x004c
#define DSI_TRIG_CTRL              0x0084
#define DSI_CMD_MODE_DMA_SW_TRIGGER 0x0090
#define DSI_INT_CTRL               0x0110
#define DSI_TEST_PATTERN_GEN_CTRL  0x015c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x017c
#define DSI_TPG_DMA_FIFO_RESET     0x01ec

#define DSI_CTRL_ENABLE            (1u << 0)
#define DSI_CTRL_CMD_MODE_EN       (1u << 2)
#define DSI_STATUS_CMD_BUSY        ((1u << 1) | (1u << 2))
#define DSI_INT_CMD_DMA_DONE       (1u << 0)
#define DSI_INT_CMD_DMA_DONE_MASK  (1u << 24)

#define CBCR_ENABLE                (1u << 0)
#define CBCR_OFF                   (1u << 31)
#define GDSC_POWER_ON              (1u << 31)

#define THYME_BL_MIN_LEVEL         16u
#define THYME_BL_MAX_LEVEL         4095u
#define THYME_BL_DEFAULT_PERMILLE  600

static vm_offset_t gcc_map, dispcc_map, dsi0;
static bool brightness_ready;
static int brightness_permille = THYME_BL_DEFAULT_PERMILLE;
static uint32_t brightness_last_level;
static unsigned brightness_logs;

/*
 * The rate the framebuffer may use. It stays 60 until the full panel timing
 * sequence has actually been accepted: pacing at 90 into a panel that did not
 * switch produces the late-swap ghosting seen in the seq=1 runs.
 */
int hakios_panel_refresh_hz = 60;

/*
 * The framebuffer starts before this driver and announces the 60 Hz it sees
 * then. It offers this so that the new rate is announced after the panel has
 * actually switched. A framebuffer without it (another phone) leaves the weak
 * symbol undefined.
 */
extern void hakios_fb_publish_refresh(void) __attribute__((weak));

static LCK_GRP_DECLARE(brightness_lck_grp, "hakios-brightness");
static LCK_MTX_DECLARE(brightness_mtx, &brightness_lck_grp);

static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

static inline uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/*
 * A read of a clocked block while its clock is off can hang the interconnect.
 * Check GCC first, then DISPCC's power and AHB clock, before touching DSI.
 */
static bool
display_regs_alive(void)
{
	uint32_t value;

	if (gcc_map == 0 || dispcc_map == 0) {
		return false;
	}
	value = rd(gcc_map, GCC_DISP_AHB_CBCR & 0xfffu);
	if (value == ~0u || !(value & CBCR_ENABLE) || (value & CBCR_OFF)) {
		return false;
	}
	value = rd(dispcc_map, DISPCC_MDSS_CORE_GDSCR);
	if (value == ~0u || !(value & GDSC_POWER_ON)) {
		return false;
	}
	value = rd(dispcc_map, DISPCC_MDSS_AHB_CBCR);
	return value != ~0u && (value & CBCR_ENABLE) && !(value & CBCR_OFF);
}

static bool
clock_on(vm_offset_t base, uint32_t cbcr, const char *name)
{
	uint32_t value = rd(base, cbcr);

	if (value == ~0u) {
		return false;
	}
	if (!(value & CBCR_ENABLE) || (value & CBCR_OFF)) {
		wr(base, cbcr, value | CBCR_ENABLE);
		delay_for_interval(10, NSEC_PER_MSEC);
		value = rd(base, cbcr);
	}
	if (!(value & CBCR_ENABLE) || (value & CBCR_OFF)) {
		tell("display: the %s clock does not take (%#x): brightness unavailable\n", name, value);
		return false;
	}
	return true;
}

static uint32_t
permille_to_level(int permille)
{
	if (permille <= 0) {
		return THYME_BL_MIN_LEVEL;
	}
	if (permille >= 1000) {
		return THYME_BL_MAX_LEVEL;
	}
	return THYME_BL_MIN_LEVEL +
	    ((THYME_BL_MAX_LEVEL - THYME_BL_MIN_LEVEL) * (uint32_t)permille + 500u) / 1000u;
}

/*
 * Send a run of 32-bit words through the DSI command DMA path the firmware
 * left enabled. The first word is the packet header (0xc0, the 0x39
 * long-write data type and the payload length); the words after it are the
 * payload four bytes each, least significant byte first. The controller's DMA
 * and trigger state is restored whether the transfer finishes or not.
 */
static bool dsi_lp_command;      /* the panel's timing switch is a dsi_lp_mode command set */

static bool
dsi_send_words(const uint32_t *words, unsigned count)
{
	uint32_t old_dma_ctrl, old_trig, old_cmd_dma = 0;
	bool done = false;

	if (count == 0 || count > 9) {
		return false;
	}
	if (!brightness_ready || dsi0 == 0 || !display_regs_alive()) {
		return false;
	}
	if ((rd(dsi0, DSI_CTRL) & (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) !=
	    (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) {
		return false;
	}
	for (unsigned i = 0; i < 40; i++) {
		if (!display_regs_alive()) {
			return false;
		}
		if ((rd(dsi0, DSI_STATUS) & DSI_STATUS_CMD_BUSY) == 0) {
			break;
		}
		if (i == 39) {
			return false;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}

	old_dma_ctrl = rd(dsi0, DSI_COMMAND_MODE_DMA_CTRL);
	old_trig = rd(dsi0, DSI_TRIG_CTRL);
	if (dsi_lp_command) {
		old_cmd_dma = rd(dsi0, DSI_CMD_DMA_CTRL);
		wr(dsi0, DSI_CMD_DMA_CTRL, old_cmd_dma | DSI_CMD_DMA_CTRL_LOW_POWER);
	}
	wr(dsi0, DSI_INT_CTRL,
	    (rd(dsi0, DSI_INT_CTRL) & ~DSI_INT_CMD_DMA_DONE_MASK) | DSI_INT_CMD_DMA_DONE);
	if ((old_trig & 0xfu) != 0x4u) {
		wr(dsi0, DSI_TRIG_CTRL, (old_trig & ~0xfu) | 0x4u);
	}
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0x00030006u);
	for (unsigned i = 0; i < count; i++) {
		wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, words[i]);
	}
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, (old_dma_ctrl & ~0xc0000000u) | 0x14000000u);
	wr(dsi0, DSI_DMA_CMD_LENGTH, count * 4u);
	wr(dsi0, DSI_CMD_MODE_DMA_SW_TRIGGER, 1u);
	for (unsigned i = 0; i < 100; i++) {
		if (rd(dsi0, DSI_INT_CTRL) & DSI_INT_CMD_DMA_DONE) {
			wr(dsi0, DSI_INT_CTRL,
			    (rd(dsi0, DSI_INT_CTRL) & ~DSI_INT_CMD_DMA_DONE_MASK) | DSI_INT_CMD_DMA_DONE);
			done = true;
			break;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0u);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 1u);
	delay_for_interval(2, NSEC_PER_USEC);
	wr(dsi0, DSI_TPG_DMA_FIFO_RESET, 0u);
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, old_dma_ctrl);
	if ((old_trig & 0xfu) != 0x4u) {
		wr(dsi0, DSI_TRIG_CTRL, old_trig);
	}
	if (dsi_lp_command) {
		wr(dsi0, DSI_CMD_DMA_CTRL, old_cmd_dma);
	}
	return done;
}

/* One DCS long write: the command byte and its parameters, up to 32 bytes. */
static bool
dsi_send_long(const uint8_t *payload, unsigned length)
{
	uint32_t words[1 + 8];
	unsigned count = 1 + (length + 3) / 4;

	if (length == 0 || length > 32) {
		return false;
	}
	words[0] = 0xc0390000u | length;
	for (unsigned i = 1; i < count; i++) {
		words[i] = 0;
	}
	for (unsigned i = 0; i < length; i++) {
		words[1 + i / 4] |= (uint32_t)payload[i] << (8 * (i % 4));
	}
	return dsi_send_words(words, count);
}

/*
 * One DCS short write: data type 0x15 and one or two parameters. The panel's
 * timing-switch commands (0x2f and 0x26) use this form in the device tree;
 * sending them as long writes wedges the command DMA and every later
 * brightness command is skipped (logs/190626-run.log).
 */
static bool
dsi_send_short(const uint8_t *payload, unsigned length)
{
	uint32_t words[2];

	if (length == 0 || length > 2) {
		return false;
	}
	words[0] = 0xc0150000u | length;
	words[1] = payload[0] | (length > 1 ? (uint32_t)payload[1] << 8 : 0);
	return dsi_send_words(words, 2);
}

static bool
send_backlight(uint32_t level)
{
	uint8_t payload[3];

	if (level > THYME_BL_MAX_LEVEL) {
		level = THYME_BL_MAX_LEVEL;
	}
	payload[0] = 0x51;
	payload[1] = (level >> 8) & 0xff;
	payload[2] = level & 0xff;
	return dsi_send_long(payload, sizeof(payload));
}

/*
 * The panel's own timing switch, from the live device tree's J2 panel node
 * (timing@0 60 Hz, timing@1 90 Hz): the vendor page F0, the B4 timing table
 * for the mode, then DCS 0x2f (the rate) and 0x26. The framebuffer must make
 * frames at the same rate (fb_hz=90); switching only the panel would leave a
 * 90 Hz scan behind 60 Hz content.
 */
static const uint8_t panel_page_f0[] = {
	0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
};
static const uint8_t panel_b4_60[] = {
	0xb4, 0xcb, 0xbb, 0xbb, 0xaa, 0x99, 0x77, 0x66, 0x00, 0x00,
	0x00, 0xb4, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0, 0x86, 0x86, 0x3c,
	0x3c, 0xf2, 0xf2, 0xa8, 0xa8, 0x36, 0x36, 0x36, 0x36, 0x0a,
};
static const uint8_t panel_b4_90[] = {
	0xb4, 0x98, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x00, 0x00,
	0x00, 0x60, 0xb8, 0xb8, 0xb8, 0xb8, 0xb8, 0xc8, 0xc8, 0xd8,
	0xd8, 0xe8, 0xe8, 0xf8, 0xf8, 0x48, 0x48, 0x48, 0x48, 0x0a,
};

static bool
send_panel_timing_cmd(uint32_t hz, uint32_t sequence)
{
	const uint8_t *b4 = hz >= 90 ? panel_b4_90 : panel_b4_60;
	uint8_t rate[2] = { 0x2f, hz >= 90 ? 0x04 : 0x01 };
	uint8_t mode[2] = { 0x26, hz >= 90 ? 0x02 : 0x01 };

	if (sequence >= 2) {
		return true;
	}
	if (sequence == 0) {
		/*
		 * The live device tree's timing-switch-command for this panel (J2,
		 * panel-id 0x4a32: timing@0 60 Hz, timing@1 90 Hz), verbatim:
		 * F0 page 0, the mode's B4 table, 2f, 26. The J1S nodes carry a
		 * longer sequence (C1, F0 page 3, BB); that is another panel's.
		 */
		tell("display: THYME panel timing: F0 page\n");
		if (!dsi_send_long(panel_page_f0, sizeof(panel_page_f0))) {
			return false;
		}
		tell("display: THYME panel timing: B4 table\n");
		if (!dsi_send_long(b4, 30)) {
			return false;
		}
	}
	tell("display: THYME panel timing: 2f %#x\n", rate[1]);
	if (!dsi_send_short(rate, sizeof(rate))) {
		return false;
	}
	tell("display: THYME panel timing: 26 %#x\n", mode[1]);
	return dsi_send_short(mode, sizeof(mode));
}

static bool
send_panel_timing(uint32_t hz, uint32_t sequence)
{
	bool ok;

	/* the device tree marks the timing-switch command as "dsi_lp_mode" */
	dsi_lp_command = true;
	ok = send_panel_timing_cmd(hz, sequence);
	dsi_lp_command = false;
	return ok;
}

SYSCTL_DECL(_kern_hakios);

/*
 * panel_dsi_probe=1: sample the DSI controller's status and TE registers after
 * the panel timing sequence. The INTF block is not readable on THYME
 * (display_probe hangs before user space), so the DSI controller is the only
 * live display block that can tell us where the panel's TE is.
 */
/*
 * panel_te_rate=1: count the edges of the panel's TE line (the device tree's platform-te-gpio,
 * pin 66) for about a second. A TE pulse starts every scan, so the count is the panel's real
 * refresh rate: neither the DSI status bits nor the INTF frame counter (not readable on THYME)
 * show it. A pin that never moves reads 0 edges.
 */
/*
 * panel_te_probe=1: count the edges of the panel's TE line itself. TE is the
 * panel's own "I am about to scan" pulse, so its rate is the panel's real
 * refresh rate — the only direct readout on THYME (the INTF block is unsafe
 * and the DSI status bits say nothing about the scan). The device tree's
 * platform-te-gpio is pin 66; the pad's IO register bit 0 is the level
 * whatever function the pin is muxed to. DCS 0x35 (set tear on) is sent
 * first because the panel may only drive TE once asked.
 */
static void
panel_te_count(const char *label)
{
	vm_offset_t tlmm = ml_io_map(0x0f100000, 0x100000);
	uint64_t began = 0, now = 0;
	unsigned edges = 0;
	uint32_t previous;

	if (tlmm == 0) {
		tell("display: TE %s: TLMM is not mapped\n", label);
		return;
	}
	previous = rd(tlmm, 0x1000 * 66 + 4) & 1u;
	absolutetime_to_nanoseconds(mach_absolute_time(), &began);
	do {
		for (unsigned n = 0; n < 20000; n++) {
			uint32_t level = rd(tlmm, 0x1000 * 66 + 4) & 1u;

			if (level != previous) {
				edges++;
				previous = level;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	} while (now - began < NSEC_PER_SEC);
	tell("display: TE %s: pin 66 config %#x, %u edges in %llu ms\n",
	    label, rd(tlmm, 0x1000 * 66), edges, (now - began) / NSEC_PER_MSEC);
}

static void
panel_te_probe(void)
{
	uint8_t te_on[2] = { 0x35, 0x00 };
	uint8_t te_on_both[2] = { 0x35, 0x02 };
	uint32_t dcs = 0, rate_gpio = 0;
	vm_offset_t tlmm;

	/* the read-only count: what the TE line does as the loader left it */
	panel_te_count("default");
	tlmm = ml_io_map(0x0f100000, 0x100000);
	if (tlmm != 0) {
		tell("display: TE probe: pin 50 (disp-rate) config %#x, level %u\n",
		    rd(tlmm, 0x1000 * 50), rd(tlmm, 0x1000 * 50 + 4) & 1u);
	}
	/*
	 * Both steps below change panel or pad state and are opt-in: a run that
	 * also re-muxed pin 66 hung the kernel (2026-11-09), so each is isolated
	 * and tested on its own.
	 */
	if (PE_parse_boot_argn("panel_te_dcs", &dcs, sizeof(dcs)) && dcs) {
		if (dsi_send_short(te_on, sizeof(te_on))) {
			delay_for_interval(20, NSEC_PER_MSEC);
			panel_te_count("after 0x35 00");
		}
		if (dsi_send_short(te_on_both, sizeof(te_on_both))) {
			delay_for_interval(20, NSEC_PER_MSEC);
			panel_te_count("after 0x35 02");
		}
	}
	/*
	 * The vendor display driver holds GPIO 50 (mi,mdss-dsi-panel-disp-rate-gpio)
	 * high as an output for the whole session; the loader leaves it
	 * unconfigured and low. Raise it and see whether the panel starts
	 * scanning the way the DSI switch asked.
	 */
	if (PE_parse_boot_argn("panel_rate_gpio", &rate_gpio, sizeof(rate_gpio)) && rate_gpio && tlmm != 0) {
		uint32_t config = rd(tlmm, 0x1000 * 50);

		wr(tlmm, 0x1000 * 50, (config & ~0x3cu) | (1u << 9) | (7u << 6));
		wr(tlmm, 0x1000 * 50 + 4, 2u);
		delay_for_interval(20, NSEC_PER_MSEC);
		tell("display: TE probe: pin 50 now config %#x, level %u\n",
		    rd(tlmm, 0x1000 * 50), rd(tlmm, 0x1000 * 50 + 4) & 1u);
		panel_te_count("after disp-rate gpio high");
	}
}

static void
panel_te_rate(void)
{
	vm_offset_t tlmm = ml_io_map(0x0f100000, 0x100000);
	const uint32_t pin = 66;
	uint32_t previous, bit0 = 0, bit2 = 0;
	uint64_t began = 0, now = 0, ns;

	if (tlmm != 0) {
		tell("display: TE pin %u: config %#x, level %u\n", pin, rd(tlmm, 0x1000 * pin),
		    rd(tlmm, 0x1000 * pin + 4) & 1);
	}
	if (!brightness_ready || dsi0 == 0 || !display_regs_alive()) {
		tell("display: TE rate: the DSI controller is not ready\n");
		return;
	}
	/*
	 * The DSI status bits 0 and 2 move with the command DMA. Their rate over a second, measured
	 * with the kernel's clock and no sleeps in the loop, says how often the panel's memory is
	 * pushed: the rate the panel really scans at.
	 */
	previous = rd(dsi0, DSI_STATUS);
	absolutetime_to_nanoseconds(mach_absolute_time(), &began);
	do {
		for (unsigned n = 0; n < 1000; n++) {
			uint32_t status = rd(dsi0, DSI_STATUS);
			uint32_t changed = status ^ previous;

			if (changed & 1u) {
				bit0++;
			}
			if (changed & 4u) {
				bit2++;
			}
			previous = status;
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	} while (now - began < NSEC_PER_SEC);
	ns = now - began;
	tell("display: DSI status over %llu ms: bit 0 changed %u times, bit 2 %u\n", ns / NSEC_PER_MSEC, bit0, bit2);
}

static void
dsi_probe(void)
{
	uint32_t transitions[32] = { 0 };
	uint32_t previous, te_ctl, te_start, te_end, te_cnt, trig, dma, int0, int1;

	if (!brightness_ready || dsi0 == 0 || !display_regs_alive()) {
		return;
	}
	delay_for_interval(200, NSEC_PER_MSEC);
	te_ctl = rd(dsi0, 0x00ac);
	te_start = rd(dsi0, 0x00b0);
	te_end = rd(dsi0, 0x00b4);
	te_cnt = rd(dsi0, 0x00b8);
	trig = rd(dsi0, DSI_TRIG_CTRL);
	dma = rd(dsi0, DSI_COMMAND_MODE_DMA_CTRL);
	int0 = rd(dsi0, DSI_INT_CTRL);
	previous = rd(dsi0, DSI_STATUS);
	for (unsigned n = 0; n < 1000; n++) {
		uint32_t status, changed;

		delay_for_interval(100, NSEC_PER_USEC);
		status = rd(dsi0, DSI_STATUS);
		changed = status ^ previous;
		for (unsigned bit = 0; bit < 32; bit++) {
			if (changed & (1u << bit)) {
				transitions[bit]++;
			}
		}
		previous = status;
	}
	int1 = rd(dsi0, DSI_INT_CTRL);
	tell("display: DSI probe: te_ctl=%#x start=%#x end=%#x cnt=%#x trig=%#x dma=%#x int0=%#x int1=%#x status=%#x\n",
	    te_ctl, te_start, te_end, te_cnt, trig, dma, int0, int1, previous);
	for (unsigned bit = 0; bit < 32; bit++) {
		if (transitions[bit] >= 3) {
			tell("display: DSI probe: status bit %u changed %u times in 100 ms\n", bit, transitions[bit]);
		}
	}
}

static int
sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int value = brightness_permille, error;
	uint32_t level;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value < 0) {
		value = 0;
	}
	if (value > 1000) {
		value = 1000;
	}
	lck_mtx_lock(&brightness_mtx);
	brightness_permille = value;
	level = permille_to_level(value);
	if (level != brightness_last_level) {
		bool ok = send_backlight(level);

		if (ok) {
			brightness_last_level = level;
		}
		if (brightness_logs < 12 || !ok) {
			brightness_logs++;
			tell("display: THYME backlight %d permille -> DCS 0x51 level %u (%s)\n",
			    value, level, ok ? "ok" : "skipped");
		}
	}
	lck_mtx_unlock(&brightness_mtx);
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_brightness, "I", "display brightness in permille (0..1000)");

void
hakios_brightness_init(void)
{
	DTEntry chosen;
	const void *board;
	unsigned size;
	uint32_t control;

	if (brightness_ready) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-board", &board, &size) != kSuccess ||
	    size != 6 || memcmp(board, "thyme", 5) != 0) {
		tell("display: brightness board mismatch\n");
		return;
	}

	gcc_map = ml_io_map(GCC_PHYS + (GCC_DISP_AHB_CBCR & 0xfffu), 0x1000);
	if (gcc_map == 0 ||
	    !clock_on(gcc_map, GCC_DISP_AHB_CBCR & 0xfffu, "display controller bus")) {
		return;
	}
	dispcc_map = ml_io_map(DISPCC_PHYS, 0x4000);
	if (dispcc_map == 0 ||
	    !(rd(dispcc_map, DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON) ||
	    !clock_on(dispcc_map, DISPCC_MDSS_AHB_CBCR, "display registers bus")) {
		tell("display: brightness unavailable: display block is not powered\n");
		return;
	}
	dsi0 = ml_io_map(DSI0_PHYS, 0x1000);
	if (dsi0 == 0 || !display_regs_alive()) {
		tell("display: brightness unavailable: DSI0 is not mapped or alive\n");
		return;
	}
	control = rd(dsi0, DSI_CTRL);
	if ((control & (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) !=
	    (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) {
		tell("display: brightness unavailable: DSI_CTRL=%#x is not command mode\n", control);
		return;
	}

	brightness_ready = true;
	uint32_t fb_hz = 0;
	/* the thyme panel's hardware is 90 Hz at most (the device tree's 0x78 entry is not this panel's) */
	if (PE_parse_boot_argn("fb_hz", &fb_hz, sizeof(fb_hz)) && (fb_hz == 60 || fb_hz == 90)) {
		uint32_t sequence = 0;
		(void)PE_parse_boot_argn("panel_seq", &sequence, sizeof(sequence));
		if (sequence > 2) {
			sequence = 0;
		}
		bool switched = false;

		for (unsigned attempt = 0; attempt < 5 && !switched; attempt++) {
			if (attempt != 0) {
				delay_for_interval(20, NSEC_PER_MSEC);
			}
			switched = send_panel_timing(fb_hz, sequence);
		}
		if (switched && fb_hz >= 90) {
			hakios_panel_refresh_hz = (int)fb_hz;
			if (&hakios_fb_publish_refresh != NULL) {
				hakios_fb_publish_refresh();
			}
		}
		tell("display: THYME panel timing %u Hz seq=%u (%s), framebuffer rate %d Hz\n", fb_hz, sequence,
		    switched ? "ok" : "skipped", hakios_panel_refresh_hz);
	}
	uint32_t level = permille_to_level(brightness_permille);
	bool ok = false;
	for (unsigned attempt = 0; attempt < 5 && !ok; attempt++) {
		if (attempt != 0) {
			delay_for_interval(10, NSEC_PER_MSEC);
		}
		ok = send_backlight(level);
	}
	if (ok) {
		brightness_last_level = level;
	}
	tell("display: THYME brightness ready: DSI_CTRL=%#x, %d permille -> level %u (%s)\n",
	    control, brightness_permille, level, ok ? "ok" : "skipped");
	uint32_t dsi_probe_enabled = 0;
	if (PE_parse_boot_argn("panel_dsi_probe", &dsi_probe_enabled, sizeof(dsi_probe_enabled)) &&
	    dsi_probe_enabled) {
		dsi_probe();
	}
	uint32_t te_probe_enabled = 0;
	if (PE_parse_boot_argn("panel_te_probe", &te_probe_enabled, sizeof(te_probe_enabled)) && te_probe_enabled) {
		panel_te_probe();
	}
	uint32_t te_rate_enabled = 0;
	if (PE_parse_boot_argn("panel_te_rate", &te_rate_enabled, sizeof(te_rate_enabled)) && te_rate_enabled) {
		panel_te_rate();
	}
}
