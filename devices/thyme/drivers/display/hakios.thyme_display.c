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
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
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

static void
backlight_packet(uint32_t level, uint32_t *word0, uint32_t *word1)
{
	if (level > THYME_BL_MAX_LEVEL) {
		level = THYME_BL_MAX_LEVEL;
	}
	*word0 = 0xc0390003u;
	*word1 = 0xff000051u | (((level >> 8) & 0xffu) << 8) | ((level & 0xffu) << 16);
}

static bool
send_backlight(uint32_t level)
{
	uint32_t word0, word1, old_dma_ctrl, old_trig;
	bool done = false;

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

	backlight_packet(level, &word0, &word1);
	old_dma_ctrl = rd(dsi0, DSI_COMMAND_MODE_DMA_CTRL);
	old_trig = rd(dsi0, DSI_TRIG_CTRL);
	wr(dsi0, DSI_INT_CTRL,
	    (rd(dsi0, DSI_INT_CTRL) & ~DSI_INT_CMD_DMA_DONE_MASK) | DSI_INT_CMD_DMA_DONE);
	if ((old_trig & 0xfu) != 0x4u) {
		wr(dsi0, DSI_TRIG_CTRL, (old_trig & ~0xfu) | 0x4u);
	}
	wr(dsi0, DSI_TEST_PATTERN_GEN_CTRL, 0x00030006u);
	wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, word0);
	wr(dsi0, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, word1);
	wr(dsi0, DSI_COMMAND_MODE_DMA_CTRL, (old_dma_ctrl & ~0xc0000000u) | 0x14000000u);
	wr(dsi0, DSI_DMA_CMD_LENGTH, 8u);
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
	return done;
}

SYSCTL_DECL(_kern_hakios);

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
	uint32_t level = permille_to_level(brightness_permille);
	bool ok = send_backlight(level);
	if (ok) {
		brightness_last_level = level;
	}
	tell("display: THYME brightness ready: DSI_CTRL=%#x, %d permille -> level %u (%s)\n",
	    control, brightness_permille, level, ok ? "ok" : "skipped");
}
