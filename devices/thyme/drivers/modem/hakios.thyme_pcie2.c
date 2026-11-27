/*
 * The Mi 10S's modem on PCIe2 (0002:01:00.0), the link only.
 *
 * The phone's own device tree has no glink/modem and no smp2p-modem: the AP
 * talks to its SDX55M over PCIe2 and MHI (docs/cellular.md), so the reference
 * SMEM/GLINK driver (hakios.mpss.c) cannot start here. This is the first step
 * of that path, a diagnostic on the style of the Wi-Fi's PCIe0 bootstrap
 * (hakios.thyme_pcie.c, whose sequence it follows): power the PCIe block,
 * train the link, and read the endpoint's configuration space. Nothing is
 * flashed and no DMA is enabled; pcie2_test=1 asks for it.
 *
 * The sequence now follows the vendor's own msm_pcie_enable() and
 * msm_pcie_link_train() from the CAF SM8250 tree (drivers/pci/controller/
 * pci-msm.c), which is the code the phone's kernel runs: PERST (GPIO 85,
 * active low) held while the PARF block and PHY are set up, the PHY ready
 * poll at qcom,phy-status-offset 0xa14 bit 6 (clear = ready), then the
 * endpoint is released and the DBI link-training writes run. The clocks are
 * the vendor's clk_branch data from the phone's own kernel Image: GDSC
 * 0x6004, CBCRs 0x6010-0x602c, refgen source 0x6f014, refgen branch
 * 0x6f034 bit 0, branch votes 0x52010 bits 10-15, and the two resets the
 * DT names -- GCC_PCIE_2_BCR 0x6000 and GCC_PCIE_2_PHY_BCR 0x1f01c. The
 * PHY sequence is the phone's DTB qcom,phy-sequence (136 triples).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.nas.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include "umi_baseband.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_wifi_power(void);
extern int hakios_pmic_pon_modem_reset(void);
extern void hakios_pmic_pon_report(void);
extern kern_return_t thread_terminate(thread_t thread);
extern uint64_t kvtophys(vm_offset_t va);

/* pcie2_mhi=2 (or pcie2_cc=1): native AppleBaseband / CommCenter bridge mode */
static uint32_t pcie2_mhi_mode;
static int mhi_bb_wake_channel;
static int mhi_bb_thread_alive;
static int mhi_bb_live;
static bool mhi_bb_data_open;
static bool mhi_hw_events_ready;
static uint64_t mhi_bb_data_tx, mhi_bb_data_rx, mhi_bb_data_drop;
static uint64_t mhi_bb_active_at, mhi_bb_log_at;
static uint32_t mhi_bb_host_svc[5];
static bool mhi_bb_host_known;
static unsigned mhi_bb_modem_node = 2;

static uint64_t
mhi_now_us(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000u;
}

static void
mhi_bb_idle(unsigned ms)
{
	struct timespec pause = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * NSEC_PER_MSEC
	};

	(void)msleep(&mhi_bb_wake_channel, NULL, PZERO, "thyme-modem", &pause);
}

void
umi_modem_wake(void)
{
	wakeup(&mhi_bb_wake_channel);
}

int
umi_modem_state(void)
{
	if (!__atomic_load_n(&mhi_bb_thread_alive, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	return __atomic_load_n(&mhi_bb_live, __ATOMIC_ACQUIRE) ? 1 : 2;
}

int
umi_modem_restart(void)
{
	return 0;
}

unsigned
umi_modem_restarts(void)
{
	return 0;
}

#define GCC  0x00100000u
#define PARF 0x01c10000u
#define PHY  0x01c16000u
#define DBI  0x64000000u
#define IATU 0x64001000u
#define CONF 0x64100000u
#define BIT(n) (1u << (n))

/*
 * pcie2_keep: leave the endpoint's PERST alone.  The phone's bootloader may
 * have booted the modem already, in which case the endpoint sits in AMSS with
 * its MHI ready rather than in the PBL's boot window; asserting PERST would
 * send it back to the PBL.  The survey and the MHI reads then say which
 * execution environment the endpoint really is in when this kernel starts.
 */
static uint32_t pcie2_keep;
static uint32_t pcie2_perst_on = 1;
static uint32_t pcie2_perst_early;
static uint32_t pcie2_msi;
static uint32_t pcie2_pon_late;

static const struct { uint32_t offset, value, delay_us; } pcie2_phy[] = {
	{ 0xa40, 0x03, 0 },
	{ 0x010, 0x01, 0 },
	{ 0x01c, 0x31, 0 },
	{ 0x020, 0x01, 0 },
	{ 0x024, 0xde, 0 },
	{ 0x028, 0x07, 0 },
	{ 0x030, 0x4c, 0 },
	{ 0x034, 0x06, 0 },
	{ 0x048, 0x90, 0 },
	{ 0x058, 0x0f, 0 },
	{ 0x074, 0x06, 0 },
	{ 0x078, 0x06, 0 },
	{ 0x07c, 0x16, 0 },
	{ 0x080, 0x16, 0 },
	{ 0x084, 0x36, 0 },
	{ 0x088, 0x36, 0 },
	{ 0x094, 0x08, 0 },
	{ 0x0a4, 0x42, 0 },
	{ 0x0ac, 0x0a, 0 },
	{ 0x0b0, 0x1a, 0 },
	{ 0x0b4, 0x14, 0 },
	{ 0x0b8, 0x34, 0 },
	{ 0x0bc, 0x82, 0 },
	{ 0x0c4, 0x68, 0 },
	{ 0x0cc, 0x55, 0 },
	{ 0x0d0, 0x55, 0 },
	{ 0x0d4, 0x03, 0 },
	{ 0x0d8, 0xab, 0 },
	{ 0x0dc, 0xaa, 0 },
	{ 0x0e0, 0x02, 0 },
	{ 0x10c, 0x02, 0 },
	{ 0x110, 0x24, 0 },
	{ 0x118, 0xb4, 0 },
	{ 0x11c, 0x03, 0 },
	{ 0x154, 0x34, 0 },
	{ 0x158, 0x01, 0 },
	{ 0x16c, 0x08, 0 },
	{ 0x1ac, 0xca, 0 },
	{ 0x1b0, 0x1e, 0 },
	{ 0x1b4, 0xa2, 0 },
	{ 0x1b8, 0x18, 0 },
	{ 0x1bc, 0x11, 0 },
	{ 0x23c, 0x11, 0 },
	{ 0x284, 0x75, 0 },
	{ 0x29c, 0x12, 0 },
	{ 0x304, 0x02, 0 },
	{ 0x408, 0x0c, 0 },
	{ 0x414, 0x03, 0 },
	{ 0x434, 0x7f, 0 },
	{ 0x444, 0x70, 0 },
	{ 0x460, 0x30, 0 },
	{ 0x4d4, 0x04, 0 },
	{ 0x4d8, 0x07, 0 },
	{ 0x4dc, 0x1b, 0 },
	{ 0x4e8, 0x04, 0 },
	{ 0x4ec, 0x0e, 0 },
	{ 0x4f0, 0x4a, 0 },
	{ 0x4f4, 0x0f, 0 },
	{ 0x4f8, 0xc0, 0 },
	{ 0x4fc, 0x00, 0 },
	{ 0x510, 0x17, 0 },
	{ 0x518, 0x1c, 0 },
	{ 0x51c, 0x03, 0 },
	{ 0x524, 0x1e, 0 },
	{ 0x570, 0xbf, 0 },
	{ 0x574, 0x3f, 0 },
	{ 0x578, 0xff, 0 },
	{ 0x57c, 0x7f, 0 },
	{ 0x580, 0x15, 0 },
	{ 0x584, 0x24, 0 },
	{ 0x588, 0xe4, 0 },
	{ 0x58c, 0xec, 0 },
	{ 0x590, 0x3b, 0 },
	{ 0x594, 0x36, 0 },
	{ 0x598, 0xd4, 0 },
	{ 0x59c, 0x54, 0 },
	{ 0x5a0, 0xdb, 0 },
	{ 0x5a4, 0x3b, 0 },
	{ 0x5a8, 0x31, 0 },
	{ 0x5bc, 0x0c, 0 },
	{ 0x5b8, 0x38, 0 },
	{ 0x63c, 0x11, 0 },
	{ 0x684, 0x75, 0 },
	{ 0x69c, 0x12, 0 },
	{ 0x704, 0x20, 0 },
	{ 0x808, 0x0c, 0 },
	{ 0x814, 0x03, 0 },
	{ 0x834, 0x7f, 0 },
	{ 0x844, 0x70, 0 },
	{ 0x860, 0x30, 0 },
	{ 0x8d4, 0x04, 0 },
	{ 0x8d8, 0x07, 0 },
	{ 0x8dc, 0x1b, 0 },
	{ 0x8e8, 0x04, 0 },
	{ 0x8ec, 0x0e, 0 },
	{ 0x8f0, 0x4a, 0 },
	{ 0x8f4, 0x0f, 0 },
	{ 0x8f8, 0xc0, 0 },
	{ 0x8fc, 0x00, 0 },
	{ 0x910, 0x17, 0 },
	{ 0x918, 0x1c, 0 },
	{ 0x91c, 0x03, 0 },
	{ 0x924, 0x1e, 0 },
	{ 0x970, 0xbf, 0 },
	{ 0x974, 0x3f, 0 },
	{ 0x978, 0xff, 0 },
	{ 0x97c, 0x7f, 0 },
	{ 0x980, 0x15, 0 },
	{ 0x984, 0x24, 0 },
	{ 0x988, 0xe4, 0 },
	{ 0x98c, 0xec, 0 },
	{ 0x990, 0x3b, 0 },
	{ 0x994, 0x36, 0 },
	{ 0x998, 0xd4, 0 },
	{ 0x99c, 0x54, 0 },
	{ 0x9a0, 0xdb, 0 },
	{ 0x9a4, 0x3b, 0 },
	{ 0x9a8, 0x31, 0 },
	{ 0x9bc, 0x0c, 0 },
	{ 0x9b8, 0x38, 0 },
	{ 0xadc, 0x05, 0 },
	{ 0xb88, 0x77, 0 },
	{ 0xb98, 0x0b, 0 },
	{ 0xba4, 0x01, 0 },
	{ 0xbe0, 0x0f, 0 },
	{ 0xe0c, 0x0d, 0 },
	{ 0xe14, 0x07, 0 },
	{ 0xe1c, 0xc1, 0 },
	{ 0xe40, 0x01, 0 },
	{ 0xe48, 0x01, 0 },
	{ 0xe90, 0x00, 0 },
	{ 0xeb4, 0x33, 0 },
	{ 0xebc, 0x00, 0 },
	{ 0xee0, 0x58, 0 },
	{ 0xa00, 0x00, 0 },
	{ 0xa44, 0x03, 0 },
};

static struct {
	uint32_t address, size;
	vm_offset_t mapping;
} pcie2_regions[] = {
	{ 0x00100000, 0x1f0000, 0 }, /* GCC: the phone's DT's whole 0x1f0000, the PHY reset is at 0x1f01c */
	{ 0x01c10000, 0x3000, 0 },   /* PARF */
	{ 0x01c16000, 0x2000, 0 },   /* QMP PHY */
	{ 0x64000000, 0x1000, 0 },   /* DBI, and ELBI at +0xf20 */
	{ 0x64001000, 0x1000, 0 },   /* iATU */
	{ 0x64100000, 0x100000, 0 }, /* endpoint configuration (ECAM, bus 1) */
	{ 0x64300000, 0x1000, 0 },   /* the MHI BAR the phone assigns to the SDX55M */
	/* TLMM, only the south-tile pin pages this driver touches (1, 3, 56, 57,
	 * 85). The whole 16 MB aperture mapped here took ten megabytes of the
	 * 32 MB io_submap that every driver shares; with the SLPI heap's 8 MB it
	 * left the later diagnostics' maps to block in kmem_alloc (KMA_NOFAIL)
	 * instead of failing -- the sid=5 probe went silent that way. */
	{ 0x0f501000, 0x1000, 0 },
	{ 0x0f503000, 0x1000, 0 },
	{ 0x0f538000, 0x1000, 0 },
	{ 0x0f539000, 0x1000, 0 },
	{ 0x0f555000, 0x1000, 0 },
};

static vm_offset_t
pcie2_register(uint32_t address)
{
	for (unsigned i = 0; i < sizeof(pcie2_regions) / sizeof(pcie2_regions[0]); i++) {
		uint32_t base = pcie2_regions[i].address;

		if (pcie2_regions[i].mapping != 0 && address >= base && address - base <= pcie2_regions[i].size - 4) {
			return pcie2_regions[i].mapping + (address - base);
		}
	}
	return 0;
}

static uint32_t
rd(uint32_t address)
{
	vm_offset_t reg = pcie2_register(address);
	uint32_t value;

	if (reg == 0) {
		return UINT32_MAX;
	}
	value = *(volatile uint32_t *)reg;
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
wr(uint32_t address, uint32_t value)
{
	vm_offset_t reg = pcie2_register(address);

	if (reg == 0) {
		return;
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)reg = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
update(uint32_t address, uint32_t clear, uint32_t set)
{
	uint32_t value = rd(address);

	if (value == UINT32_MAX) {
		return EIO;
	}
	wr(address, (value & ~clear) | set);
	return 0;
}

static int
wait_bits(uint32_t address, uint32_t mask, uint32_t wanted, unsigned tries)
{
	for (unsigned i = 0; i < tries; i++) {
		uint32_t value = rd(address);

		if ((value & mask) == wanted) {
			return 0;
		}
		delay(1000);
	}
	return ETIMEDOUT;
}

/* 19.2 MHz XO for the PCIe2 refgen clock. */
static int
source_xo(uint32_t cmd)
{
	int error = update(cmd + 4, 0x371f, 1);

	if (error) {
		return error;
	}
	error = update(cmd, 0, 1);
	return error ? error : wait_bits(cmd, 1, 0, 10000);
}

static int
clocks(void)
{
	int error;

	/* the GDSC first: the controller and PHY registers are dead without it */
	error = update(GCC + 0x6004, 7, 0);
	if (error) {
		return error;
	}
	delay(1);
	error = wait_bits(GCC + 0x6004, BIT(31), BIT(31), 10000);
	if (error) {
		hakios_tl("pcie2: the GDSC does not come up: %#x\n", rd(GCC + 0x6004));
		return error;
	}
	/* the auxiliary clock's source, and the PHY's reference clock source:
	 * both 19.2 MHz XO, the same registers the vendor's clk driver picks. */
	error = source_xo(GCC + 0x6038);
	if (error) {
		return error;
	}
	error = source_xo(GCC + 0x6f014);
	if (error) {
		return error;
	}
	/* all six branch votes: slv_q2a 10, slv_axi 11, mstr_axi 12, cfg_ahb 13,
	 * aux 14, pipe 15 (the enable masks in the vendor's clk_branch data). */
	error = update(GCC + 0x52010, 0, 0xfc00);
	if (error) {
		return error;
	}
	static const uint32_t gates[] = { 0x6010, 0x6014, 0x601c, 0x6024, 0x6028 };
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		error = wait_bits(GCC + gates[i], BIT(31), 0, 10000);
		if (error) {
			hakios_tl("pcie2: the clock at %#x stays halted: %#x\n", GCC + gates[i], rd(GCC + gates[i]));
			return error;
		}
	}
	/* the per-controller PHY reference clock branch (its own halt bit too) */
	error = update(GCC + 0x6f034, 0, 1);
	if (error) {
		return error;
	}
	error = wait_bits(GCC + 0x6f034, BIT(31), 0, 10000);
	if (error) {
		hakios_tl("pcie2: the PHY refgen stays halted: %#x\n", rd(GCC + 0x6f034));
		return error;
	}
	/* the modem's reference clock gate -- the DT's "pcie_mdm_clkref"
	 * (GCC_PCIE_MDM_CLKREF_EN, binding 71) -- and the two TBU clocks the
	 * vendor's RC2 clock table enables: GCC+0x8c00c, 0x9000c, 0x8d058,
	 * each a branch whose enable is bit 0 and whose halt is bit 31. */
	static const uint32_t others[] = { 0x8c00c, 0x9000c, 0x8d058 };
	for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		error = update(GCC + others[i], 0, 1);
		if (error) {
			return error;
		}
		error = wait_bits(GCC + others[i], BIT(31), 0, 10000);
		if (error) {
			hakios_tl("pcie2: the clock at %#x stays halted: %#x\n", GCC + others[i], rd(GCC + others[i]));
			return error;
		}
	}
	/* the two resets the phone's DT names: GCC_PCIE_2_BCR 0x6000 and
	 * GCC_PCIE_2_PHY_BCR 0x1f01c (the vendor's gcc_kona_resets table).
	 * The vendor's loop does each one in turn: assert, a millisecond,
	 * deassert -- core first, then the PHY. */
	static const uint32_t resets[] = { 0x6000, 0x1f01c };
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
		error = update(GCC + resets[i], 0, 1);
		if (error) {
			return error;
		}
		delay(1000);
		error = update(GCC + resets[i], 1, 0);
		if (error) {
			return error;
		}
	}
	hakios_tl("pcie2: clocks: GDSC %#x, refgen %#x, PHY reset %#x\n", rd(GCC + 0x6004), rd(GCC + 0x6f034),
	    rd(GCC + 0x1f01c));
	return 0;
}

static void pcie2_perst(unsigned high);

static int
link_train(void)
{
	int error;
	uint32_t value;

	/* PERST asserted while the controller and PHY come up: the phone's
	 * DT pcie2_perst_default is GPIO 85, active low (the vendor's
	 * msm_pcie_enable asserts it before touching any PCIe register).
	 * pcie2_keep skips it, so an endpoint the bootloader already booted
	 * keeps its execution environment. */
	if (pcie2_perst_early) {
		hakios_tl("pcie2: PERST held from before AP2MDM_STATUS (pcie2_perst_early=1)\n");
	} else if (pcie2_keep || !pcie2_perst_on) {
		hakios_tl("pcie2: PERST left alone (%s)\n",
		    pcie2_keep ? "pcie2_keep=1" : "pcie2_perst=0");
	} else {
		pcie2_perst(0);
	}

	/* the vendor's msm_pcie_enable's PARF block, register for register */
	wr(PARF + 0x1000, 4);                     /* RC mode */
	error = update(PARF + 0x20, BIT(5), 0);   /* L1 entry allowed */
	if (error) {
		return error;
	}
	error = update(PARF + 0x40, 1, 0);        /* PHY_CTRL: PHY powered up */
	if (error) {
		return error;
	}
	wr(PARF + 0x350, 0);                      /* DBI base address */
	wr(PARF, 0x365e);                         /* SYS_CTRL */
	error = update(PARF + 0x174, 0, BIT(4));  /* MHI clock reset control */
	if (error) {
		return error;
	}
	wr(PARF + 0x22c, 0);                      /* no interrupts yet */
	wr(PARF + 0x358, 0x04000000);             /* 64 MB slave address space */
	error = update(PARF + 0x1a8, 0, BIT(31)); /* halt writes for the iATU */
	if (error) {
		return error;
	}
	hakios_tl("pcie2: PARF programmed; PARF+0x350 %#x, +0x358 %#x\n", rd(PARF + 0x350), rd(PARF + 0x358));

	/* the PHY sequence: the phone's DTB qcom,phy-sequence, in its order */
	for (unsigned i = 0; i < sizeof(pcie2_phy) / sizeof(pcie2_phy[0]); i++) {
		wr(PHY + pcie2_phy[i].offset, pcie2_phy[i].value);
		if (pcie2_phy[i].delay_us) {
			delay(pcie2_phy[i].delay_us);
		}
	}
	delay(1000);                              /* PHY stabilization */
	error = update(GCC + 0x52010, 0, 0x8000); /* the pipe clock's vote */
	if (error) {
		return error;
	}
	/* qcom,phy-status-offset 0xa14, bit 6: clear means the PHY is ready
	 * (the vendor's pcie_phy_is_ready returns false while the bit is set;
	 * its 10 ms becomes 100 ms here, the PLL lock being the slow part) */
	error = wait_bits(PHY + 0xa14, BIT(6), 0, 100);
	if (error) {
		static const uint32_t phy_regs[] = { 0xa00, 0xa04, 0xa08, 0xa0c, 0xa10, 0xa14, 0xa18, 0xa40, 0xa44, 0x800, 0x814 };
		hakios_tl("pcie2: the PHY does not report ready: %#x\n", rd(PHY + 0xa14));
		for (unsigned i = 0; i < sizeof(phy_regs) / sizeof(phy_regs[0]); i++) {
			hakios_tl("pcie2:   PHY+%#x = %#x\n", phy_regs[i], rd(PHY + phy_regs[i]));
		}
		hakios_tl("pcie2:   GCC 0x6f000 %#x, 0x6f004 %#x, 0x6f00c %#x, 0x6f010 %#x, 0x6f034 %#x, 0x8c00c %#x\n",
		    rd(GCC + 0x6f000), rd(GCC + 0x6f004), rd(GCC + 0x6f00c), rd(GCC + 0x6f010), rd(GCC + 0x6f034),
		    rd(GCC + 0x8c00c));
		return error;
	}
	hakios_tl("pcie2: PHY ready (%#x); releasing PERST\n", rd(PHY + 0xa14));
	/* umi holds PERST 100 ms past PHY ready (umi_modem_pcie.inc:138); the
	 * vendor's qcom,ep-latency here is 10 ms. */
	delay(pcie2_perst_early ? 100000 : 10000);

	pcie2_perst(1);
	delay(1000);                              /* perst-delay-us */

	/* the vendor's msm_pcie_link_train */
	error = update(DBI + 0x80c, 0x1f00, 0x100);
	if (error) {
		return error;
	}
	error = update(DBI + 0x8a8, 0, 0x20);
	if (error) {
		return error;
	}
	error = update(DBI + 0x890, BIT(0), 0);
	if (error) {
		return error;
	}
	error = update(DBI + 0x8bc, 0, BIT(0));    /* DBI read-only writes off */
	if (error) {
		return error;
	}
	wr(DBI + 0x154, 0x55555555);              /* PCIE_GEN3_PRESET_DEFAULT */
	error = update(DBI + 0x8bc, BIT(0), 0);
	if (error) {
		return error;
	}
	/* no speed forcing: qcom,target-link-speed is absent from the DT, so
	 * the core trains at the endpoint's best rate (Android shows 8 GT/s) */
	error = update(DBI + 0x78, 0x7000, 5 << 12); /* TLP read size 5 */
	if (error) {
		return error;
	}
	error = update(PARF + 0x1b0, 0, BIT(8));   /* enable link training */
	if (error) {
		return error;
	}
	hakios_tl("pcie2: link requests done; ELBI %#x, link status %#x\n", rd(DBI + 0xf28), rd(DBI + 0x80));
	/* the vendor allows 20 checks 5 ms apart: 100 ms, here 100 x 1 ms */
	error = wait_bits(DBI + 0xf28, BIT(10), BIT(10), 100);
	if (error) {
		hakios_tl("pcie2: no link training done: %#x\n", rd(DBI + 0xf28));
		return error;
	}
	error = wait_bits(DBI + 0x80, BIT(29), BIT(29), 100);
	if (error) {
		hakios_tl("pcie2: the link does not come up: %#x\n", rd(DBI + 0x80));
		return error;
	}
	value = rd(DBI);
	if (value == UINT32_MAX || value == 0xffffffffu) {
		hakios_tl("pcie2: the RC's own config space reads %#x\n", value);
		return ENODEV;
	}
	hakios_tl("pcie2: the link is up; RC id %#x, link status %#x, ELBI %#x\n", value, rd(DBI + 0x80), rd(DBI + 0xf28));

	/* The vendor's msm_pcie_config_controller, which runs after the link:
	 * the 19.2 MHz aux clock the phone's DT names
	 * (qcom,use-19p2mhz-aux-clk), the N-FTS default (no qcom,n-fts in the
	 * DT, so ACK_F_ASPM_CTRL bit 15 is set), and AER reporting on the RC
	 * (BRIDGE_CTRL bits 16/17 and the device control's four reporting
	 * enables).  None of these was in the port's link bring-up. */
	wr(DBI + 0xb40, 0x14);                    /* AUX_CLK_FREQ_REG, 19.2 MHz */
	update(DBI + 0x70c, 0, BIT(15));          /* ACK_F_ASPM_CTRL, n_fts = 0 */
	update(DBI + 0x3c, 0, BIT(16) | BIT(17)); /* BRIDGE_CTRL: AER */
	update(DBI + 0x78, 0, 0xf);               /* DEVCTRLSTATUS: error reports */
	hakios_tl("pcie2: RC config: aux %#x, ack_f %#x, bridge %#x, devctl %#x\n", rd(DBI + 0xb40),
	    rd(DBI + 0x70c), rd(DBI + 0x3c), rd(DBI + 0x78));

	/* The RC's type-1 header needs its secondary bus number set before
	 * any config request to the endpoint can be forwarded; Linux's
	 * pci_scan_bridge does this, and without it the CONF window answers
	 * all ones. Byte 0x18: primary 0, secondary 1, subordinate 1. */
	value = rd(DBI + 0x18);
	wr(DBI + 0x18, (value & 0xff000000u) | 0x00010100u);
	hakios_tl("pcie2: bus numbers %#x\n", rd(DBI + 0x18));
	return 0;
}

/* One iATU region, the vendor's msm_pcie_iatu_config: host window to target
 * BDF, type 0x04 = config type 0, enabled by bit 31 of the control register. */
static void
iatu_config(uint32_t nr, uint32_t type, uint32_t host, uint32_t host_end, uint32_t target)
{
	uint32_t base = IATU + nr * 0x200;

	wr(base + 0x04, 0);
	wr(base + 0x00, type);
	wr(base + 0x08, host);
	wr(base + 0x0c, 0);
	wr(base + 0x10, host_end);
	wr(base + 0x14, target);
	wr(base + 0x18, 0);
	wr(base + 0x04, BIT(31));
}

/*
 * The endpoint's MHI registers, once its config space answers: assign it the
 * BAR the phone's own kernel gives it (Android's dmesg: BAR0 0x64300000, and
 * the DT's memory range starts there), turn memory decoding on, translate the
 * window with iATU region 1 as an identity mapping, and read MHI's own idea of
 * itself -- MHICFG's channel/event counts, the doorbell offsets, and the state
 * in MHICTRL. Nothing is written to the device but its BAR and command word.
 */
#define MHI_BAR 0x64300000u

static void
mhi_peek(void)
{
	uint32_t bar0, bar0_before, bar1, command, cfg, control, mhistatus;

	bar0 = rd(CONF + 0x10);
	bar0_before = bar0;
	bar1 = rd(CONF + 0x14);
	if ((bar0 & 0xfffffff0u) == 0 || (bar0 & 0xfffffff0u) == 0xfffffff0u) {
		wr(CONF + 0x10, MHI_BAR);
		if ((rd(CONF + 0x10) & 0x6) == 0x4) {
			wr(CONF + 0x14, 0); /* a 64-bit BAR's upper half */
		}
		bar0 = rd(CONF + 0x10);
	}
	command = rd(CONF + 4);
	/* Memory space, and the bus master bit the vendor's pci_set_master
	 * sets: the BHI engine's reads of host memory are the endpoint's DMA,
	 * and a device whose bus master enable is clear does not start one. */
	wr(CONF + 4, (command & 0xffff0000u) | 0x0006u);
	/* The root port's own command register, one level up: the wifi driver
	 * writes 6 here ("allow protected endpoint transactions upstream")
	 * before its stream works, and this driver never did. */
	wr(DBI + 4, (rd(DBI + 4) & 0xffff0000u) | 0x0006u);
	hakios_tl("pcie2: MHI: root port command %#x\n", rd(DBI + 4));
	iatu_config(1, 0x00, MHI_BAR, MHI_BAR + 0xfff, MHI_BAR);

	cfg = rd(MHI_BAR + 0x10);
	control = rd(MHI_BAR + 0x38);
	mhistatus = rd(MHI_BAR + 0x48);
	hakios_tl("pcie2: MHI: BAR0 %#x (was %#x, next %#x), command %#x, reglen %#x\n", bar0,
	    bar0_before, bar1, rd(CONF + 4), rd(MHI_BAR + 0x0));
	hakios_tl("pcie2: MHI: cfg %#x (channels %u, events %u, hw channels %u), chdb %#x, erdb %#x\n", cfg, cfg & 0xff,
	    (cfg >> 16) & 0xff, (cfg >> 8) & 0xff, rd(MHI_BAR + 0x18), rd(MHI_BAR + 0x20));
	hakios_tl("pcie2: MHI: control %#x, status %#x (ready %u, state %u, syserr %u)\n", control, mhistatus,
	    mhistatus & 1, (mhistatus >> 8) & 0xff, (mhistatus >> 2) & 1);

	/* BHI/BHIE: the device's boot interface, where its PBL waits for an
	 * image. Their offsets are in the MHI register block. */
	uint32_t bhioff = rd(MHI_BAR + 0x28);
	uint32_t bhieoff = rd(MHI_BAR + 0x2c);
	hakios_tl("pcie2: MHI: BHIOFF %#x, BHIEOFF %#x\n", bhioff, bhieoff);
	if (bhioff != 0 && bhioff != UINT32_MAX && bhioff < 0x1000) {
		uint32_t bhi = MHI_BAR + bhioff;

		hakios_tl("pcie2: BHI: version %u.%u, execenv %#x, status %#x, errcode %#x, imgaddr %#x/%#x, imgsize %#x, numseg %#x\n",
		    rd(bhi + 0x4), rd(bhi + 0x0), rd(bhi + 0x28), rd(bhi + 0x2c), rd(bhi + 0x30), rd(bhi + 0x0c),
		    rd(bhi + 0x8), rd(bhi + 0x10), rd(bhi + 0x48));
	}
	if (bhieoff != 0 && bhieoff != UINT32_MAX && bhieoff < 0x1000) {
		uint32_t bhie = MHI_BAR + bhieoff;

		hakios_tl("pcie2: BHIE: msmsoid %#x, txvec %#x/%#x size %#x, txstatus %#x, rxvec %#x/%#x size %#x, rxstatus %#x\n",
		    rd(bhie + 0x0), rd(bhie + 0x30), rd(bhie + 0x2c), rd(bhie + 0x34), rd(bhie + 0x44),
		    rd(bhie + 0x64), rd(bhie + 0x60), rd(bhie + 0x68), rd(bhie + 0x78));
	}
}

/* The window the boot image is placed in: the SLPI's reserved heap, below 4 GB. */
#define MODEM_IMAGE_PA   0xc0000000u
#define MODEM_IMAGE_SIZE 0x800000u
/* What the heap window actually has to hold: the image is 548,056 bytes, and
 * the io_map of the whole 8 MB heap took a quarter of the 32 MB io_submap
 * every driver shares -- enough, with the wifi's maps beside it, for
 * kmem_alloc to block and take the kernel with it (the sid=6 run hung that
 * way). 640 KB is the image with room. */
#define MODEM_IMAGE_MAP  0xa0000u

/*
 * pcie2_img=4's buffer: 576 KB of the kernel's own BSS. The kernel image is
 * loaded below 4 GB (0xf7004000 on this phone), so this buffer's physical
 * address is too, and the loader has already proven that range is DRAM the
 * AP may use. It is what the PBL's reads need: readable physical memory below
 * 4 GB -- a flexible allocation lands above it, and none of the low pages are
 * free to kmem_alloc_contig.
 */
static char modem_image_buf[0x90000] __attribute__((aligned(0x4000)));

extern lck_mtx_t hakios_smmu_lock;
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t umi_camera_iomap_free(void);

/* ---- a stage-1 context of the apps SMMU for the modem, with our tables ----
 *
 * The same machinery drivers/wifi/hakios.wifi.c uses for the radio's stream
 * (radio_space_on): take the lowest context no match register uses and that
 * is switched off, build two levels of page tables in kernel memory, program
 * the context (CBAR stage 1, 39-bit, 4K pages, cached walks) and send the
 * stream through it. The address the device is given is from the DT's
 * qcom,iommu-dma-addr-pool (0x20000000), as Linux's MHI gets its IOVAs.
 */
#define SMMU_BASE        0x15000000u
#define SMMU_PTE_ADDRESS 0x0000fffffffff000ull
#define SMMU_PTE         (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define SMMU_CBAR(n)     (4 * (n))                    /* in the second register page */
#define SMMU_CBAR_STAGE_1 (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)    (0x800 + 4 * (n))
#define SMMU_CB_SCTLR    0x00
#define SMMU_CB_TCR2     0x10
#define SMMU_CB_TTBR0    0x20
#define SMMU_CB_TTBR1    0x28
#define SMMU_CB_TCR      0x30
#define SMMU_CB_MAIR0    0x38
#define SMMU_CB_MAIR1    0x3c
#define SMMU_CB_FSR      0x58
#define SMMU_CB_TLBIASID 0x610
#define SMMU_CB_TLBSYNC  0x7f0
#define SMMU_CB_TLBSTATUS 0x7f4
#define SMMU_SCTLR_ON    (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2_VAL    (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0_VAL   0xf404ff44

#define SMMU_IOVA        0x20000000ull   /* the DT's iommu-dma-addr-pool */
static uint64_t smmu_iova = SMMU_IOVA;   /* pcie2_iova=N moves the image inside the pool */

static struct {
	vm_offset_t tables;              /* the root, then the levels it needs */
	uint32_t count;
	uint32_t context;
	bool on;
} modem_space;

static vm_offset_t smmu_va;

/*
 * The apps SMMU's register window, mapped once for the whole run: a second
 * ml_io_map of the same range does not return -- the sid=5 probe's
 * diagnostics stopped at it (no SGFSR line, no iATU dump after the AER
 * print), and the same silence hid the apps-SMMU stream dump in another run.
 */
static vm_offset_t
smmu_window(void)
{
	if (smmu_va == 0) {
		smmu_va = (vm_offset_t)ml_io_map(SMMU_BASE, 0x100000);
	}
	return smmu_va;
}

static uint64_t *
smmu_table(uint32_t n)
{
	return (uint64_t *)(modem_space.tables + 4096 * n);
}

static uint64_t *
smmu_next(uint64_t *entry)
{
	if (*entry & 1) {
		for (uint32_t i = 0; i < modem_space.count; i++) {
			if (kvtophys((vm_offset_t)smmu_table(i)) == (*entry & SMMU_PTE_ADDRESS)) {
				return smmu_table(i);
			}
		}
		return NULL;
	}
	if (modem_space.count == 4) {
		return NULL;
	}
	uint64_t *table = smmu_table(modem_space.count++);

	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

/* Our memory at `physical` behind the modem's addresses from SMMU_IOVA on. */
static bool
smmu_enter(vm_offset_t buffer, size_t size)
{
	if (modem_space.tables == 0) {
		if (kmem_alloc(kernel_map, &modem_space.tables, 4 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		modem_space.count = 1;
	}
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		uint64_t at = smmu_iova + offset;
		uint64_t *level2 = smmu_next(&smmu_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : smmu_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			hakios_tl("pcie2: SMMU: no page table left for %#llx\n", at);
			return false;
		}
		level3[at >> 12 & 0x1ff] = kvtophys(buffer + offset) | SMMU_PTE;
	}
	for (vm_offset_t line = modem_space.tables; line < modem_space.tables + 4 * 4096; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return true;
}

/*
 * One page more in the same context, away from the image, so the walk starts
 * at the root again: the four table pages are the root, the level 2, the
 * image's level 3 and this one's. The attributes are the caller's -- the GIC's
 * page is not cacheable memory like the image's.
 */
static bool
smmu_enter_page(uint64_t at, uint64_t physical, uint64_t attributes)
{
	uint64_t *level2 = smmu_next(&smmu_table(0)[at >> 30 & 0x1ff]);
	uint64_t *level3 = level2 == NULL ? NULL : smmu_next(&level2[at >> 21 & 0x1ff]);

	if (level3 == NULL) {
		hakios_tl("pcie2: SMMU: no page table left for %#llx\n", (unsigned long long)at);
		return false;
	}
	level3[at >> 12 & 0x1ff] = physical | attributes;
	for (vm_offset_t line = modem_space.tables; line < modem_space.tables + 4 * 4096; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* This page is added after modem_smmu_map turned the context on: the
	 * walker may hold the old invalid entry in its TLB, so the page is not
	 * visible to the device until the context's entries are invalidated. */
	if (modem_space.on && smmu_va != 0) {
		volatile uint32_t *regs = (volatile uint32_t *)smmu_va;
		uint32_t idr1 = regs[0x024 / 4];
		uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
		uint32_t pages = 2u << ((idr1 >> 28) & 7);
		volatile uint32_t *cb = (volatile uint32_t *)(smmu_va + (pages + modem_space.context) * page);

		lck_mtx_lock(&hakios_smmu_lock);
		cb[SMMU_CB_TLBIASID / 4] = modem_space.context;
		cb[SMMU_CB_TLBSYNC / 4] = 1;
		for (unsigned ms = 0; ms < 100 && (cb[SMMU_CB_TLBSTATUS / 4] & 1); ms++) {
			delay(1000);
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	return true;
}

/*
 * A run of pages in the same context, for the MHI pool: one walk for the whole
 * range, the tables cleaned once and the context's entries invalidated once at
 * the end, where smmu_enter_page would do both for every page. The range may
 * not cross its level 3 table's 2 MB boundary -- the one table pointer below
 * could not follow it -- and callers keep it inside one.
 */
static bool
smmu_enter_range(uint64_t at, uint64_t physical, size_t size, uint64_t attributes)
{
	uint64_t *level2 = smmu_next(&smmu_table(0)[at >> 30 & 0x1ff]);
	uint64_t *level3 = level2 == NULL ? NULL : smmu_next(&level2[at >> 21 & 0x1ff]);

	if (level3 == NULL || size == 0 || (at >> 21) != ((at + size - 1) >> 21)) {
		hakios_tl("pcie2: SMMU: no page table left for %#llx..%#llx\n", (unsigned long long)at,
		    (unsigned long long)(at + size));
		return false;
	}
	for (size_t offset = 0; offset < size; offset += 0x1000) {
		level3[(at + offset) >> 12 & 0x1ff] = physical + offset | attributes;
	}
	for (vm_offset_t line = modem_space.tables; line < modem_space.tables + 4 * 4096; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* the pool is added after modem_smmu_map turned the context on: the
	 * walker may hold the old invalid entries in its TLB */
	if (modem_space.on && smmu_va != 0) {
		volatile uint32_t *regs = (volatile uint32_t *)smmu_va;
		uint32_t idr1 = regs[0x024 / 4];
		uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
		uint32_t pages = 2u << ((idr1 >> 28) & 7);
		volatile uint32_t *cb = (volatile uint32_t *)(smmu_va + (pages + modem_space.context) * page);

		lck_mtx_lock(&hakios_smmu_lock);
		cb[SMMU_CB_TLBIASID / 4] = modem_space.context;
		cb[SMMU_CB_TLBSYNC / 4] = 1;
		for (unsigned ms = 0; ms < 100 && (cb[SMMU_CB_TLBSTATUS / 4] & 1); ms++) {
			delay(1000);
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	return true;
}

/*
 * The context and the two streams (the BDF-to-SID table's offset and the DT's
 * full SID), programmed and verified; returns the IOVA the device is to use,
 * or 0. Called with the image in memory that stays put.
 */
static uint64_t
modem_smmu_map(vm_offset_t buffer, size_t size)
{
	volatile uint32_t *regs;
	uint32_t scr0, idr0, idr1, matches, contexts, wide, page, pages, n, c, made = 0;
	uint8_t used[256];
	uint64_t root;

	if (smmu_va == 0) {
		smmu_va = smmu_window();
	}
	if (smmu_va == 0) {
		hakios_tl("pcie2: SMMU: cannot map the block\n");
		return 0;
	}
	regs = (volatile uint32_t *)smmu_va;
	/* The DT's stream IDs are 16-bit (0x1d01); the boot chain may leave
	 * SCR0.EXIDENABLE (bit 3) clear, in which case the SMMU matches 8-bit
	 * streams and the wide SMR/S2CR format is not in force. pcie2_exid=1
	 * asks for the wide mode: the bit is set before the match registers
	 * are (re)programmed below, so the wide encodings are used for them. */
	{
		uint32_t exid = 0;

		PE_parse_boot_argn("pcie2_exid", &exid, sizeof(exid));
		if (exid && !(regs[0x000 / 4] & (1u << 3))) {
			regs[0x000 / 4] = regs[0x000 / 4] | (1u << 3);
			__asm__ volatile("dsb sy" ::: "memory");
			hakios_tl("pcie2: SMMU: EXIDENABLE set (SCR0 %#x)\n", regs[0x000 / 4]);
		}
	}
	scr0 = regs[0x000 / 4];
	idr0 = regs[0x020 / 4];
	idr1 = regs[0x024 / 4];
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	wide = scr0 & (1u << 3);
	page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);

	memset(used, 0, sizeof(used));
	for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
		uint32_t valid = wide ? (s2cr & (1u << 10)) : (smr >> 31);

		if (valid) {
			used[s2cr & 0xff] = 1;
		}
	}
	for (c = 0; c < contexts; c++) {
		volatile uint32_t *cb = (volatile uint32_t *)(smmu_va + (pages + c) * page);

		if (!used[c] && !(cb[SMMU_CB_SCTLR / 4] & 1)) {
			break;
		}
	}
	if (c == contexts) {
		hakios_tl("pcie2: SMMU: no free context\n");
		return 0;
	}
	if (!smmu_enter(buffer, size)) {
		return 0;
	}

	lck_mtx_lock(&hakios_smmu_lock);
	{
		volatile uint32_t *second = (volatile uint32_t *)(smmu_va + page);
		volatile uint32_t *cb = (volatile uint32_t *)(smmu_va + (pages + c) * page);

		root = kvtophys(modem_space.tables) | (uint64_t)c << 48;
		second[SMMU_CBA2R(c) / 4] = 1;                      /* 64-bit addresses */
		second[SMMU_CBAR(c) / 4] = SMMU_CBAR_STAGE_1;
		cb[SMMU_CB_SCTLR / 4] = 0;
		cb[SMMU_CB_TCR2 / 4] = SMMU_TCR2_VAL;
		cb[SMMU_CB_TCR / 4] = SMMU_TCR_39_BITS;
		cb[SMMU_CB_TTBR0 / 4] = (uint32_t)root;
		cb[SMMU_CB_TTBR0 / 4 + 1] = (uint32_t)(root >> 32);
		cb[SMMU_CB_TTBR1 / 4] = 0;
		cb[SMMU_CB_TTBR1 / 4 + 1] = 0;
		cb[SMMU_CB_MAIR0 / 4] = SMMU_MAIR0_VAL;
		cb[SMMU_CB_MAIR1 / 4] = 0;
		cb[SMMU_CB_FSR / 4] = 0xc00001fe;
		cb[SMMU_CB_TLBIASID / 4] = c;
		cb[SMMU_CB_TLBSYNC / 4] = 1;
		for (unsigned ms = 0; ms < 100 && (cb[SMMU_CB_TLBSTATUS / 4] & 1); ms++) {
			delay(1000);
		}
		__asm__ volatile("dsb sy" ::: "memory");
		cb[SMMU_CB_SCTLR / 4] = SMMU_SCTLR_ON;

		/* The table's offset (1) and the DT SID: a match register that
		 * already carries one of them (a previous run's, e.g.) is
		 * repointed at this context rather than duplicated -- a duplicate
		 * would never be reached, the first match wins. SMR 0 stays the
		 * boot chain's. */
		{
			static const uint32_t streams[] = { 1, 0x1d01 };

			for (uint32_t s = 0; s < 2; s++) {
				uint32_t existing = matches;

				for (n = 1; n < matches && 0x800 + 4 * n < 0xc00; n++) {
					uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
					uint32_t valid = wide ? (s2cr & (1u << 10)) : (smr >> 31);
					uint32_t stream = smr & (wide ? 0xffff : 0x7fff);
					uint32_t mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

					if (valid && mask == 0 && stream == streams[s]) {
						existing = n;
						break;
					}
				}
				if (existing != matches) {
					regs[(0xc00 + 4 * existing) / 4] = wide ? ((1u << 10) | c) : c;
					made++;
					hakios_tl("pcie2: SMMU: SMR %u repointed to context %u for stream %#x\n", existing, c,
					    streams[s]);
					continue;
				}
				for (n = 1; n < matches && 0x800 + 4 * n < 0xc00; n++) {
					uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
					uint32_t valid = wide ? (s2cr & (1u << 10)) : (smr >> 31);

					if (valid) {
						continue;
					}
					regs[(0x800 + 4 * n) / 4] = wide ? streams[s] : ((1u << 31) | streams[s]);
					regs[(0xc00 + 4 * n) / 4] = wide ? ((1u << 10) | c) : c;
					made++;
					break;
				}
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		hakios_tl("pcie2: SMMU: context %u (page %#x, %u pages) tables %#llx, SCTLR %#x, streams %u\n", c, page,
		    pages, (unsigned long long)modem_space.tables, cb[SMMU_CB_SCTLR / 4], made);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (made != 2) {
		hakios_tl("pcie2: SMMU: %u of 2 streams programmed\n", made);
	}
	modem_space.context = c;
	modem_space.on = true;
	return smmu_iova;
}

/*
 * The modem's streams in the apps SMMU. The RC's BDF-to-SID entry is
 * `BDF [31:16] | SID [15:8] | NEXT [7:0]` (the vendor's msm_pcie_config_sid),
 * so the boot chain's 0x01000100 is BDF 1:0.0 -> SID 0x1d00 + 1 = 0x1d01,
 * which matches an SMR the boot chain made: S2CR type 0, translate, into
 * context 3 (S2CR_TYPE_TRANS is 0 in the vendor's arm-smmu-regs.h; an older
 * note here read that as FAULT). What that context's tables do with the
 * PBL's address, and whether the read completes, is what the context dump
 * and the AER probe now measure.
 *
 * Two writes let a probe take the stream the way the DT intends: the
 * BDF-to-SID table gets BDF 1:0.0 (0x0100) -> the DT SID's offset in every
 * entry, so the hash the RC computes cannot miss it; and two free SMRs are
 * given the table offset (1) and the full SID (0x1d01) with S2CR type
 * BYPASS, like the SLPI's FastRPC stream is given one. SID 0's entry is left
 * as the boot chain made it -- writing that SMR was the one change followed
 * by a log-less reset.
 */
static void
modem_sid_setup(uint32_t mode)
{
	/* BDF 1:0.0 with the table's 8-bit SID offset 1 (the DT's 0x1d01
	 * minus qcom,smmu-sid-base 0x1d00), as the vendor fills it */
	uint32_t entry = (0x0100u << 16) | (0x01u << 8);
	vm_offset_t smmu;
	volatile uint32_t *regs;
	uint32_t scr0, idr0, matches, wide, n, sid0 = 0;

	if (mode == 1 || mode == 2 || mode == 4 || mode == 6) {
		uint32_t nonz = 0;

		for (uint32_t i = 0; i < 256; i++) {
			wr(PARF + 0x2000 + 4 * i, entry);
		}
		for (uint32_t i = 0; i < 256; i++) {
			if (rd(PARF + 0x2000 + 4 * i) != 0) {
				nonz++;
			}
		}
		hakios_tl("pcie2: SID: BDF-to-SID table all %#x (BDF 1:0.0 -> offset 1 of 0x1d00); read back %u of 256\n",
		    entry, nonz);
	}
	if (mode == 2) {
		return;
	}
	if (mode == 6) {
		/* S2CR type 2 for the endpoint's stream: if this SMMU calls it a
		 * fault, the next read of SGFSYNR1 names the stream; if it is a
		 * bypass, the stream's reads pass and the global fault stays 0. The
		 * SMMU is not reset between boots, so an earlier run's SMR for
		 * 0x1d01 can be there already and would win the match; it is
		 * repointed in place first, and only a clean table gets a new one. */
		uint32_t faulted = 0;

		hakios_tl("pcie2: SID: mode 6: mapping the SMMU\n");
		smmu = smmu_window();
		if (smmu == 0) {
			hakios_tl("pcie2: SID: mode 6: the SMMU cannot be mapped\n");
			return;
		}
		regs = (volatile uint32_t *)smmu;
		matches = regs[0x020 / 4] & 0xff;
		wide = regs[0x000 / 4] & (1u << 3);
		hakios_tl("pcie2: SID: mode 6: %u SMRs, %s; taking the SMMU lock\n", matches, wide ? "wide" : "8-bit");
		lck_mtx_lock(&hakios_smmu_lock);
		hakios_tl("pcie2: SID: mode 6: the SMMU lock is held\n");
		for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
			uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
			uint32_t used = wide ? (s2cr & (1u << 10)) : (smr >> 31);
			uint32_t stream = smr & (wide ? 0xffff : 0x7fff);
			uint32_t mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

			if (!used || mask != 0 || stream != 0x1d01) {
				continue;
			}
			regs[(0xc00 + 4 * n) / 4] = wide ? ((1u << 10) | 0x20000) : 0x20000;
			hakios_tl("pcie2: SID: SMR %u (0x1d01) turned into a fault in place: s2cr %#x\n", n,
			    regs[(0xc00 + 4 * n) / 4]);
			faulted = 1;
			break;
		}
		if (faulted == 0) {
			for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
				uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
				uint32_t used = wide ? (s2cr & (1u << 10)) : (smr >> 31);

				if (used) {
					continue;
				}
				regs[(0x800 + 4 * n) / 4] = wide ? 0x1d01 : ((1u << 31) | 0x1d01);
				regs[(0xc00 + 4 * n) / 4] = wide ? ((1u << 10) | 0x20000) : 0x20000;
				hakios_tl("pcie2: SID: SMR %u stream 0x1d01 into a free slot, s2cr read back %#x (type %u)\n", n,
				    regs[(0xc00 + 4 * n) / 4], (regs[(0xc00 + 4 * n) / 4] >> 16) & 3);
				break;
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	if (mode == 4) {
		/* a probe: 0x1d01 matched but pointed at context 0, which is off
		 * -- if the endpoint's reads reach the SMMU at all, this makes
		 * the SMMU record the fault and the stream */
		smmu = smmu_window();
		if (smmu == 0) {
			return;
		}
		regs = (volatile uint32_t *)smmu;
		matches = regs[0x020 / 4] & 0xff;
		wide = regs[0x000 / 4] & (1u << 3);
		lck_mtx_lock(&hakios_smmu_lock);
		for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
			uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
			uint32_t used = wide ? (s2cr & (1u << 10)) : (smr >> 31);

			if (used) {
				continue;
			}
			regs[(0x800 + 4 * n) / 4] = wide ? 0x1d01 : ((1u << 31) | 0x1d01);
			regs[(0xc00 + 4 * n) / 4] = 0;
			hakios_tl("pcie2: SID: SMR %u set to stream 0x1d01, s2cr 0 (a fault to observe)\n", n);
			break;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}

	smmu = smmu_window();
	if (smmu == 0) {
		hakios_tl("pcie2: SID: the apps SMMU cannot be mapped\n");
		return;
	}
	regs = (volatile uint32_t *)smmu;
	scr0 = regs[0x000 / 4];
	idr0 = regs[0x020 / 4];
	matches = idr0 & 0xff;
	wide = scr0 & (1u << 3);

	/* The stream-0 SMR is the boot chain's and stays untouched: writing it
	 * was followed by a reset with no log at all. Only free match
	 * registers are used -- for the table's offset (1) and for the full
	 * SID the DT names (0x1d01), in case the RC adds qcom,smmu-sid-base
	 * itself. */
	lck_mtx_lock(&hakios_smmu_lock);
	{
		static const uint32_t streams[] = { 1, 0x1d01 };
		uint32_t made = 0;

		for (uint32_t s = 0; s < sizeof(streams) / sizeof(streams[0]); s++) {
			for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
				uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
				uint32_t used = wide ? (s2cr & (1u << 10)) : (smr >> 31);

				if (used) {
					continue;
				}
				if (wide) {
					regs[(0x800 + 4 * n) / 4] = streams[s];
					regs[(0xc00 + 4 * n) / 4] = (1u << 10) | (1u << 16);
				} else {
					regs[(0x800 + 4 * n) / 4] = (1u << 31) | streams[s];
					regs[(0xc00 + 4 * n) / 4] = 1u << 16;
				}
				made++;
				hakios_tl("pcie2: SID: SMR %u now smr %#x s2cr %#x (stream %#x bypass)\n", n,
				    regs[(0x800 + 4 * n) / 4], regs[(0xc00 + 4 * n) / 4], streams[s]);
				break;
			}
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
		if (made == 0) {
			hakios_tl("pcie2: SID: no free SMR in the apps SMMU\n");
		}
	}
	(void)sid0;
}

/*
 * A whole file of the scene root, read into `dest`, a mapped buffer of
 * `capacity` bytes. The scene root is mounted by launchd after boot, so the
 * caller retries while it appears; the image is at /redwood/fw/modem/sbl1.mbn.
 */
static int
root_file_into(const char *name, vm_offset_t dest, size_t capacity, size_t *size)
{
	char path[96];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return ENOENT;
	}
	snprintf(path, sizeof(path), "/redwood/fw/%s", name);
	error = vnode_lookup(path, 0, &vnode, context);
	if (error != 0) {
		return error;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	error = vnode_getattr(vnode, &attributes, context);
	if (error != 0 || attributes.va_data_size == 0 || attributes.va_data_size > capacity) {
		hakios_tl("pcie2: fw: %s: getattr %d, size %lld (capacity %zu)\n", path, error,
		    (long long)attributes.va_data_size, capacity);
		vnode_put(vnode);
		return error != 0 ? error : EFBIG;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)dest, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		hakios_tl("pcie2: fw: read %s: error %d, %d left\n", path, error, left);
		return error != 0 ? error : EIO;
	}
	*size = (size_t)attributes.va_data_size;
	return 0;
}

/*
 * The stage-1 context's fault register: cleared before the doorbell and read
 * after it, so a fault then is the transfer's. FAR is the input address the
 * endpoint asked for.
 */
static void
modem_context_fsr(bool clear)
{
	volatile uint32_t *regs, *cb;
	uint32_t idr1, page, pages;

	if (!modem_space.on || smmu_va == 0) {
		return;
	}
	regs = (volatile uint32_t *)smmu_va;
	idr1 = regs[0x024 / 4];
	page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	cb = (volatile uint32_t *)(smmu_va + (pages + modem_space.context) * page);
	if (clear) {
		cb[SMMU_CB_FSR / 4] = 0xc00001fe;
		__asm__ volatile("dsb sy" ::: "memory");
		return;
	}
	/* the FAULT bits of the SMMU-500's CBF FSR; 0x400 (FORMAT) is not one */
	hakios_tl("pcie2: SMMU: our context %u FSR %#x (fault bits %#x) FAR %#x\n", modem_space.context,
	    cb[SMMU_CB_FSR / 4], cb[SMMU_CB_FSR / 4] & 0xc000011eu, cb[0x60 / 4]);
}

/* A table page for the walk below, mapped once per probe: the same page comes
 * up again for each level and each address, and a second ml_io_map of a range
 * already mapped is refused (one diagnostic went silent that way). */
static vm_offset_t
mmu_table_map(uint64_t pa, uint64_t *seen_pa, vm_offset_t *seen_va, int *seen)
{
	for (int i = 0; i < *seen; i++) {
		if (seen_pa[i] == pa) {
			return seen_va[i];
		}
	}
	if (*seen >= 8 || pa < 0x80000000ull || pa >= 0x300000000ull) {
		return 0;                       /* not a DRAM page this probe reads */
	}
	seen_va[*seen] = (vm_offset_t)ml_io_map((vm_offset_t)pa, 0x1000);
	if (seen_va[*seen] == 0) {
		return 0;
	}
	seen_pa[(*seen)++] = pa;
	return seen_va[*seen - 1];
}

/*
 * Walk a context's stage-1 tables for one address: the boot chain's SMR 5
 * translates the modem's stream through context 3, so what the PBL reads is
 * decided by context 3's tables. 39-bit VA, 4 KB granule, three levels, the
 * layout TCR 0x803519 names (T0SZ 25). A block descriptor at level 1 or 2 is
 * a valid leaf; an invalid one at any level is the translation fault the
 * SMMU should have recorded.
 */
static void
mmu_walk(uint32_t cx, uint64_t ttbr0, uint32_t tcr, uint64_t va)
{
	static const uint32_t shifts[3] = { 30, 21, 12 };
	uint64_t seen_pa[8];
	vm_offset_t seen_va[8];
	int seen = 0, level;
	uint64_t table = ttbr0 & 0x0000fffffffff000ull;
	uint32_t t0sz = tcr & 0x3f;

	if ((tcr & (3u << 14)) != 0 || 64 - t0sz != 39) {
		hakios_tl("pcie2: SMMU: context %u: TCR %#x is not the 39-bit 4 KB form; no walk\n", cx, tcr);
		return;
	}
	for (level = 0; level < 3; level++) {
		uint32_t index = (uint32_t)((va >> shifts[level]) & 0x1ff);
		vm_offset_t page = mmu_table_map(table, seen_pa, seen_va, &seen);
		uint64_t desc, mask;

		if (page == 0) {
			hakios_tl("pcie2: SMMU: context %u: %#llx: table page %#llx not readable\n", cx,
			    (unsigned long long)va, (unsigned long long)table);
			return;
		}
		desc = ((volatile uint64_t *)page)[index];
		if ((desc & 3) == 0) {
			hakios_tl("pcie2: SMMU: context %u: %#llx: level %d entry %u is %#llx: a translation fault\n",
			    cx, (unsigned long long)va, level + 1, index, (unsigned long long)desc);
			return;
		}
		mask = ~((1ull << shifts[level]) - 1) & 0x0000fffffffff000ull;
		if ((desc & 3) == 1) {              /* a block at level 1 or 2 */
			hakios_tl("pcie2: SMMU: context %u: %#llx: level %d block %#llx -> physical %#llx\n", cx,
			    (unsigned long long)va, level + 1, (unsigned long long)desc,
			    (unsigned long long)((desc & mask) | (va & ((1ull << shifts[level]) - 1))));
			return;
		}
		if (level == 2) {                   /* the page itself */
			hakios_tl("pcie2: SMMU: context %u: %#llx: page %#llx -> physical %#llx\n", cx,
			    (unsigned long long)va, (unsigned long long)desc,
			    (unsigned long long)((desc & mask) | (va & 0xfff)));
			return;
		}
		table = desc & 0x0000fffffffff000ull;
	}
}

/*
 * The two root complexes side by side. PCIe0's endpoint (the wifi) DMAs
 * through the apps SMMU and works; PCIe2's (the modem) does not, and its
 * reads are refused above both the iATU and the SMMU. The registers that
 * decide where an endpoint's traffic goes are read from both here: the
 * BDF-to-SID table entry its BDF hashes to, the configuration register, the
 * AXI master write halt, the slave address space, and the root port's own
 * command word.
 */
static void
pcie2_compare_rc0(void)
{
	volatile uint32_t *parf0 = (volatile uint32_t *)ml_io_map(0x01c00000u, 0x10000);
	volatile uint32_t *dbi0 = (volatile uint32_t *)ml_io_map(0x60000000u, 0x1000);
	uint8_t hash = 0;
	const uint8_t bdf[] = { 1, 0 };

	if (parf0 == 0 || dbi0 == 0) {
		hakios_tl("pcie2: compare: PCIe0's registers cannot be mapped\n");
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		hash ^= bdf[i];
		for (unsigned b = 0; b < 8; b++) {
			hash = (uint8_t)((hash << 1) ^ ((hash & 0x80) ? 7 : 0));
		}
	}
	hakios_tl("pcie2: compare: PCIe0 SID cfg %#x, AXI halt %#x, slave space %#x, DBI command %#x link %#x\n",
	    parf0[0x2c00 / 4], parf0[0x1a8 / 4], parf0[0x358 / 4], dbi0[0x4 / 4], dbi0[0x80 / 4]);
	hakios_tl("pcie2: compare: PCIe0 BDF entry %u = %#x; PCIe2's = %#x\n", hash, parf0[0x2000 / 4 + hash],
	    rd(PARF + 0x2000 + 4 * hash));
	hakios_tl("pcie2: compare: PCIe2 SID cfg %#x, AXI halt %#x, slave space %#x, DBI command %#x link %#x\n",
	    rd(PARF + 0x2c00), rd(PARF + 0x1a8), rd(PARF + 0x358), rd(DBI + 0x4), rd(DBI + 0x80));
}

/*
 * The MSI half of the vendor's md_enable_dma (umi_modem_dma.inc:191-216): walk
 * the endpoint's capability list to the MSI capability, insist on the 64-bit
 * and per-vector-masking forms the vendor requires, point vector 0 at the
 * GIC's MBI alias the Android kernel gives the modem (0x17a10040, data 864,
 * the SPI its MHI GIC doorbell uses) and enable it with every vector masked as
 * the vendor leaves them. The SID table and the PARF bit are this driver's
 * already (modem_sid_setup(2) and the caller). The vendor's own comment on the
 * page behind this address: it covers "SBL's direct MSI writes".
 */
static int
pcie2_msi_enable(void)
{
	uint32_t cap = rd(CONF + 0x34) & 0xff, seen = 0;

	hakios_tl("pcie2: MSI: the capability list starts at %#x\n", cap);
	while (cap != 0) {
		uint32_t header;

		if (cap < 0x40 || cap > 0xfc || (cap & 3) || (seen & (1u << (cap / 4)))) {
			hakios_tl("pcie2: MSI: the list is broken at %#x (seen %#x)\n", cap, seen);
			return EPROTO;
		}
		seen |= 1u << (cap / 4);
		header = rd(CONF + cap);
		if (header == UINT32_MAX) {
			hakios_tl("pcie2: MSI: %#x reads all ones\n", cap);
			return EIO;
		}
		hakios_tl("pcie2: MSI: capability %u at %#x, next %#x, header %#x\n", header & 0xff, cap,
		    (header >> 8) & 0xff, header);
		if ((header & 0xff) == 5) {
			uint32_t ctrl = header >> 16;

			if (!(ctrl & 0x80) || !(ctrl & 0x100) || ((ctrl >> 1) & 7) < 3 || cap > 0xe8) {
				hakios_tl("pcie2: MSI: not the vendor's form: control %#x\n", ctrl);
				return ENOTSUP;
			}
			wr(CONF + cap, header & ~(1u << 16));
			wr(CONF + cap + 16, UINT32_MAX);
			wr(CONF + cap + 4, 0x17a10040);
			wr(CONF + cap + 8, 0);
			wr(CONF + cap + 12, 864);
			wr(CONF + cap, (header & ~(0x71u << 16)) | (0x31u << 16));
			hakios_tl("pcie2: MSI: %#x: control %#x -> %#x, address %#x:%#x data %#x mask %#x\n", cap,
			    header >> 16, rd(CONF + cap) >> 16, rd(CONF + cap + 8), rd(CONF + cap + 4),
			    rd(CONF + cap + 12), rd(CONF + cap + 16));
			return 0;
		}
		cap = (header >> 8) & 0xff;
	}
	hakios_tl("pcie2: MSI: no capability id 5 in the list\n");
	return ENODEV;
}

/*
 * The MHI engine the SBL speaks after BHI accepts it: the vendor's
 * md_dma_install, md_contexts, md_start_channel and its Sahara server
 * (umi_modem_mhi.inc:114-676, umi_modem_dma.inc:74-216). BHI has taken
 * sbl1.mbn (result 2) and the SBL is in its execution environment
 * (BHI_EXECENV 1); from here the host has to offer the transfer engine it
 * boots through: a pool the device reaches through the same SMMU context,
 * channel and event context arrays, a command ring and two software
 * channels. The Sahara byte stream that follows asks for the images that
 * come next -- qdsp6sw.mbn, the AMSS, above all -- and once it runs
 * BHI_EXECENV becomes 2.
 *
 * The pool is touched only through the write-combining alias: the device's
 * writes land in DRAM and the kernel's cacheable mapping of the same pages
 * is used once, for the clean after kmem_alloc_contig zeroed them.
 */
#define MHI_NER         6u
#define MHI_TRES        128u
#define MHI_PACKET      32768u
#define MHI_POOL_SIZE   0x100000u
#define MHI_IMAGE_LIMIT (512ull * 1024 * 1024)

struct mhi_event_ring {
	volatile uint32_t *words;
	uint64_t base;
	uint32_t count, consumer, wp;
};

struct mhi_channel {
	unsigned id, er, wp;
	volatile uint32_t *ring;
	uint64_t ring_base;
	volatile uint8_t *buffer;
	uint64_t buffer_iova;
	bool tx_pending, tx_done, rx_posted, rx_ready, db_mode;
	size_t rx_length;
};

static struct {
	vm_offset_t cached, io;
	uint64_t iova, used;
} mhi_pool;
static struct mhi_event_ring mhi_events[MHI_NER];
#define MHI_CHANNELS    10u  /* 0/1 SBL Sahara; 2/3 QMI0; 4/5 IPCR; 6/7 EFS; 8/9 IP_HW0 (100/101) */
static struct mhi_channel mhi_channels[MHI_CHANNELS];
static volatile uint32_t *mhi_chctx, *mhi_erctx, *mhi_cmdctx, *mhi_cmdring;
static uint64_t mhi_chctx_iova, mhi_erctx_iova, mhi_cmdctx_iova;
static uint64_t mhi_cmdring_off;
static uint32_t mhi_chdb, mhi_erdb, mhi_bhi;
static unsigned mhi_cmdwp;
static uint64_t mhi_command_wait;
static int mhi_command_cc;
static uint32_t mhi_last_channel, mhi_last_code, mhi_last_bytes;
static uint32_t mhi_win_base = 0x20000000u, mhi_win_stop = 0x3ffffffeu;
static const char *mhi_stage = "MHI";
static unsigned mhi_sahara_images;
static uint64_t mhi_sahara_bytes;    /* what the list has served, for the watch's heartbeat */
static unsigned mhi_round;           /* the Sahara image list's pass, 1 or 2 */
static uint64_t mhi_sid_iova;        /* the image's IOVA from the first round */
static uint64_t mhi_sid_pbase;       /* ... and the physical base it maps */
static int mhi_sid_ready;
static uint8_t mhi_sahara_stream[MHI_PACKET];
static uint8_t mhi_sahara_packet[256];
static uint8_t mhi_chunk[MHI_PACKET];
static size_t mhi_sahara_pos, mhi_sahara_size;
static const uint32_t mhi_event_counts[MHI_NER] = { 64, 128, 64, 64, 1024, 2048 };

static uint32_t
mhi_u32(const volatile void *pointer)
{
	const volatile uint8_t *p = (const volatile uint8_t *)pointer;

	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
mhi_u64(const volatile void *pointer)
{
	return mhi_u32(pointer) | (uint64_t)mhi_u32((const volatile uint8_t *)pointer + 4) << 32;
}

static void
mhi_set64(volatile uint32_t *words, unsigned n, uint64_t value)
{
	words[n] = (uint32_t)value;
	words[n + 1] = (uint32_t)(value >> 32);
}

/*
 * A doorbell register holds the whole 64-bit ring address: the vendor's
 * md_address writes both halves, and the Wi-Fi MHI's write_address does the
 * same. Writing only the low word here left the upper word as the SBL had
 * it, and a stale upper half would send the device to an address that is not
 * the ring the moment it used the doorbell.
 */
static void
mhi_doorbell(uint32_t offset, uint64_t value)
{
	wr(MHI_BAR + offset, (uint32_t)value);
	wr(MHI_BAR + offset + 4, (uint32_t)(value >> 32));
}

/* the rings are in normal memory the device writes: one barrier after the
 * host's stores, before anything that tells the device to look at them */
static void
mhi_publish(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static bool
mhi_ringptr(uint64_t base, unsigned count, uint64_t pointer)
{
	return pointer >= base && pointer - base < (uint64_t)count * 16 && !((pointer - base) & 15);
}

static uint64_t
mhi_alloc(size_t size, size_t align)
{
	uint64_t at;

	size = (size + 4095) & ~(size_t)4095;
	if (align < 4096 || (align & (align - 1))) {
		align = 4096;
	}
	at = (mhi_pool.used + align - 1) & ~(uint64_t)(align - 1);
	if (size == 0 || at > MHI_POOL_SIZE || size > MHI_POOL_SIZE - at) {
		return UINT64_MAX;
	}
	mhi_pool.used = at + size;
	return at;
}

static volatile uint32_t *
mhi_words(uint64_t offset)
{
	return (volatile uint32_t *)(mhi_pool.io + offset);
}

static volatile uint32_t *
mhi_channel_context(unsigned id)
{
	return mhi_chctx + id * 11;
}

static unsigned pcie2_pin_in(unsigned pin);

/*
 * The AP's view of the endpoint after a fault, as umi's proven host takes it
 * (umi_modem_mhi.inc, md_fault_diag): MDM_ERRFATAL (TLMM 1) asserted means
 * the modem announced its own fatal error, MDM_STATUS (TLMM 3) whether it
 * still claims to be alive, and the root port's LTSSM (PARF+0x1b0, 0x11 = L0)
 * whether the link is still trained.  The endpoint's config window is read
 * once, and only while LTSSM still says L0: touching config on a dead link is
 * what hung umi's phone.
 */
static void
mhi_fault_diag(uint32_t state)
{
	unsigned errfatal = pcie2_pin_in(1), status = pcie2_pin_in(3);
	uint32_t ltssm = rd(PARF + 0x1b0) & 0x3f;

	if (ltssm != 0x11) {
		hakios_tl("pcie2: MHI: fault diag: MDM_ERRFATAL %u, MDM_STATUS %u, LTSSM %#x: link not L0, config not read\n",
		    errfatal, status, ltssm);
	} else {
		hakios_tl("pcie2: MHI: fault diag: MDM_ERRFATAL %u, MDM_STATUS %u, LTSSM L0, config ID %#x, MHI status %#x\n",
		    errfatal, status, rd(CONF), state);
	}
}

/*
 * The engine's fault registers, as the vendor reads them: an all-ones status
 * word, the ERROR bit, the execution environment the device retired BHI in,
 * or a stage 1 fault in our context all mean the conversation is over.  The
 * first fault is described once -- the loop that finds it asks again every
 * poll -- and the modem's own fatal line is sampled beside it.
 */
static int
mhi_fault(void)
{
	static bool told;
	uint32_t state = rd(MHI_BAR + 0x48), ee = rd(mhi_bhi + 0x28);

	if (state == UINT32_MAX || (state & 4) || ee == 3 || ee == 6 || ee > 7) {
		if (!told) {
			told = true;
			hakios_tl("pcie2: MHI: fault at %s: status %#x, execenv %#x\n", mhi_stage, state, ee);
			mhi_fault_diag(state);
		}
		return EIO;
	}
	/* M1 may be offered by the modem; keep this bounded diagnostic in M0. */
	if (((state >> 8) & 255) == 3) {
		(void)update(MHI_BAR + 0x38, 0xff00, 0x200);
	}
	if (modem_space.on && smmu_va != 0) {
		volatile uint32_t *regs = (volatile uint32_t *)smmu_va;
		uint32_t idr1 = regs[0x024 / 4];
		uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
		uint32_t pages = 2u << ((idr1 >> 28) & 7);
		uint32_t fsr = ((volatile uint32_t *)(smmu_va +
		    (pages + modem_space.context) * page))[SMMU_CB_FSR / 4];

		if (fsr & 0xc00001fe) {
			if (!told) {
				told = true;
				hakios_tl("pcie2: MHI: fault at %s: context %u FSR %#x\n", mhi_stage,
				    modem_space.context, fsr);
				mhi_fault_diag(state);
			}
			return EIO;
		}
	}
	return 0;
}

static int mhi_post(struct mhi_channel *channel, size_t length);
static int mhi_bb_data_poll(void);

static void
mhi_channel_doorbell(struct mhi_channel *c)
{
	if (c->id >= 100 && !c->db_mode) {
		return;
	}
	mhi_doorbell(mhi_chdb + c->id * 8, c->ring_base + (uint64_t)c->wp * 16);
	if (c->id >= 100) {
		c->db_mode = false;
	}
}

/*
 * One pass over the six event rings: command completions, transfer
 * completions, and the others the device may report. Entry: the address
 * word, two words of information (code and byte count in [2], channel and
 * type in [3]).
 */
static int
mhi_poll(void)
{
	int error = mhi_fault();

	if (error) {
		return error;
	}
	for (unsigned e = 0; e < MHI_NER; e++) {
		struct mhi_event_ring *ring = &mhi_events[e];
		volatile uint32_t *ctx = mhi_erctx + e * 11;
		uint64_t producer = mhi_u64(ctx + 7);
		unsigned end, processed = 0;

		if (ring->words == NULL) {
			return ENODEV;
		}
		if (!mhi_ringptr(ring->base, ring->count, producer)) {
			hakios_tl("pcie2: MHI: event ring %u: producer %#llx outside %#llx+%u entries\n", e,
			    (unsigned long long)producer, (unsigned long long)ring->base, ring->count);
			return EPROTO;
		}
		end = (unsigned)((producer - ring->base) / 16);
		for (unsigned n = 0; ring->consumer != end && n < ring->count; n++, processed++) {
			volatile uint32_t *event = ring->words + 4 * ring->consumer;
			uint64_t pointer = mhi_u64(event);
			unsigned type = (event[3] >> 16) & 255;
			unsigned ch = event[3] >> 24;
			unsigned code = event[2] >> 24;

			mhi_last_channel = ch;
			mhi_last_code = code;
			mhi_last_bytes = event[2] & 65535;
			/* M0 transition restores burst doorbells on hardware channels (IP_HW0). */
			if (type == 0x20 && code == 2 && e == 0) {
				for (unsigned i = 8; i < MHI_CHANNELS; i++) {
					struct mhi_channel *hc = &mhi_channels[i];

					hc->db_mode = true;
					if (hc->ring != NULL && (hc->tx_pending || hc->rx_posted)) {
						mhi_channel_doorbell(hc);
					}
				}
			}
			if (type == 0x21 && pointer == mhi_command_wait && e == 0) {
				mhi_command_cc = (int)code;
			}
			if (type == 0x22) {
				struct mhi_channel *c = NULL;

				for (unsigned i = 0; i < MHI_CHANNELS; i++) {
					if (mhi_channels[i].ring != NULL && mhi_channels[i].id == ch) {
						c = &mhi_channels[i];
					}
				}
				/* Hardware channels (100/101) ask for a new doorbell via OOB (5) / DB_MODE (6). */
				if (c != NULL && c->er == e && ch >= 100 && (code == 5 || code == 6)) {
					c->db_mode = true;
					if (c->tx_pending || c->rx_posted) {
						mhi_channel_doorbell(c);
					}
					goto next_event;
				}
				if (c == NULL || c->er != e || (code != 2 && !(ch >= 100 && code == 4)) ||
				    !mhi_ringptr(c->ring_base, MHI_TRES, pointer) ||
				    pointer != c->ring_base + (uint64_t)((c->wp + MHI_TRES - 1) % MHI_TRES) * 16) {
					hakios_tl("pcie2: MHI: erratic transfer event ER=%u ch=%u code=%u bytes=%u pointer %#llx\n",
					    e, ch, code, event[2] & 65535, (unsigned long long)pointer);
					return EPROTO;
				}
				if (!(ch & 1)) {
					if (!c->tx_pending) {
						return EPROTO;
					}
					c->tx_pending = false;
					c->tx_done = true;
				} else {
					if (!c->rx_posted || c->rx_ready || (event[2] & 65535) > MHI_PACKET) {
						return EPROTO;
					}
					c->rx_length = event[2] & 65535;
					c->rx_ready = true;
					c->rx_posted = false;
				}
			}
next_event:
			ring->consumer = (ring->consumer + 1) % ring->count;
			ring->wp = (ring->wp + 1) % ring->count;
		}
		/* the vendor's md_poll only gives the ring back when it took
		 * something out of it: an idle ring is not rung every millisecond */
		if (processed) {
			mhi_set64(ctx, 9, ring->base + (uint64_t)ring->wp * 16);
			mhi_publish();
			mhi_doorbell(mhi_erdb + e * 8, ring->base + (uint64_t)ring->wp * 16);
		}
	}
	if (mhi_bb_data_open) {
		return mhi_bb_data_poll();
	}
	return 0;
}

/*
 * One transfer request entry in the channel's ring, its doorbell, and for
 * odd channels the receive buffer is counted as posted.
 */
static int
mhi_post(struct mhi_channel *c, size_t length)
{
	volatile uint32_t *tre;

	if (length == 0 || length > MHI_PACKET || c->tx_pending || c->rx_posted || c->rx_ready) {
		return EBUSY;
	}
	tre = c->ring + c->wp * 4;
	mhi_set64(tre, 0, c->buffer_iova);
	tre[2] = (uint32_t)length;
	tre[3] = (2u << 16) | (1u << 9);
	c->wp = (c->wp + 1) % MHI_TRES;
	if (c->id & 1) {
		c->rx_posted = true;
	} else {
		c->tx_pending = true;
		c->tx_done = false;
	}
	mhi_set64(mhi_channel_context(c->id), 9, c->ring_base + (uint64_t)c->wp * 16);
	mhi_publish();
	mhi_channel_doorbell(c);
	return 0;
}

static int
mhi_send(struct mhi_channel *c, const void *data, size_t length)
{
	int error;

	if (length == 0 || length > MHI_PACKET) {
		return EINVAL;
	}
	memcpy((void *)c->buffer, data, length);
	mhi_publish();
	error = mhi_post(c, length);
	if (error) {
		return error;
	}
	for (unsigned ms = 0; ms < 5000; ms++) {
		error = mhi_poll();
		if (error) {
			return error;
		}
		if (c->tx_done) {
			return 0;
		}
		if ((ms % 1000) == 999) {
			hakios_tl("pcie2: MHI: channel %u: %u ms without a transfer completion\n", c->id,
			    ms + 1);
		}
		delay(1000);
	}
	hakios_tl("pcie2: MHI: TX channel %u stalls (last event ch %u code %u bytes %u)\n", c->id,
	    mhi_last_channel, mhi_last_code, mhi_last_bytes);
	return ETIMEDOUT;
}

static int
mhi_receive(struct mhi_channel *c, void *data, size_t *length, unsigned timeout_ms)
{
	int error;

	if (!c->rx_posted && !c->rx_ready) {
		error = mhi_post(c, MHI_PACKET);
		if (error) {
			return error;
		}
	}
	for (unsigned ms = 0; ms < timeout_ms; ms++) {
		error = mhi_poll();
		if (error) {
			return error;
		}
		if (!c->rx_ready && timeout_ms >= 5000 && (ms % 5000) == 4999) {
			hakios_tl("pcie2: MHI: channel %u: %u ms without a packet\n", c->id, ms + 1);
		}
		if (c->rx_ready) {
			if (c->rx_length > *length) {
				return EMSGSIZE;
			}
			*length = c->rx_length;
			memcpy(data, (const void *)c->buffer, *length);
			c->rx_ready = false;
			/* ready for the next message before this one is parsed */
			return mhi_post(c, MHI_PACKET);
		}
		delay(1000);
	}
	return ETIMEDOUT;
}

/*
 * The pool, before MHI is told where anything is: contiguous memory, the one
 * clean, the write-combining alias, and 256 pages of it in the same SMMU
 * context as the image, half a megabyte after it in the same level 3 table.
 * The descriptor is the vendor's 0x0e43, exactly the image's.
 */
static int
mhi_pool_setup(void)
{
	uint64_t iova;

	if (mhi_pool.io != 0) {
		return 0;
	}
	if (!modem_space.on || smmu_va == 0) {
		hakios_tl("pcie2: MHI: no SMMU context for the pool\n");
		return ENODEV;
	}
	if (kmem_alloc_contig(kernel_map, &mhi_pool.cached, MHI_POOL_SIZE, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		hakios_tl("pcie2: MHI: no contiguous %u KB for the pool\n", MHI_POOL_SIZE / 1024);
		return ENOMEM;
	}
	flush_dcache(mhi_pool.cached, MHI_POOL_SIZE, 0);
	mhi_pool.io = ml_io_map_wcomb(kvtophys(mhi_pool.cached), MHI_POOL_SIZE);
	if (mhi_pool.io == 0) {
		hakios_tl("pcie2: MHI: cannot map the pool write-combining\n");
		return ENOMEM;
	}
	iova = smmu_iova + 0x100000;
	if (iova + MHI_POOL_SIZE > 0x40000000ull) {
		hakios_tl("pcie2: MHI: the pool at %#llx would leave the MHI window\n", (unsigned long long)iova);
		return ERANGE;
	}
	if (!smmu_enter_range(iova, kvtophys(mhi_pool.cached), MHI_POOL_SIZE, SMMU_PTE)) {
		return ENOMEM;
	}
	mhi_pool.iova = iova;
	mhi_pool.used = 0;
	hakios_tl("pcie2: MHI: pool %u KB, physical %#llx at IOVA %#llx\n", MHI_POOL_SIZE / 1024,
	    (unsigned long long)kvtophys(mhi_pool.cached), (unsigned long long)iova);
	return 0;
}

/*
 * The context arrays, the command ring and the six event rings, then MHICFG
 * and the M0 transition: after this the device owns the rings and the SBL is
 * no longer the only one talking.
 */
static int
mhi_contexts(void)
{
	uint64_t chctx, erctx, cmdctx;
	uint32_t cfg;
	int error;

	mhi_stage = "MHI-READY";
	error = wait_bits(MHI_BAR + 0x38, 2, 0, 20000);
	if (error) {
		hakios_tl("pcie2: MHI: control %#x still holds the reset bit\n", rd(MHI_BAR + 0x38));
		return error;
	}
	error = wait_bits(MHI_BAR + 0x48, 0xff05, 0x101, 20000);
	if (error) {
		hakios_tl("pcie2: MHI: status %#x is not the READY pattern 0x101\n", rd(MHI_BAR + 0x48));
		return error;
	}
	mhi_chdb = rd(MHI_BAR + 0x18);
	mhi_erdb = rd(MHI_BAR + 0x20);
	if ((mhi_chdb & 7) || mhi_chdb < 0xa8 || mhi_chdb > 0x1000 - 1024 ||
	    (mhi_erdb & 7) || mhi_erdb < 0xa8 || mhi_erdb > 0x1000 - MHI_NER * 8) {
		hakios_tl("pcie2: MHI: doorbells at %#x/%#x are not the vendor's form\n", mhi_chdb, mhi_erdb);
		return EPROTO;
	}
	chctx = mhi_alloc(128 * 44, 4096);
	erctx = mhi_alloc(MHI_NER * 44, 4096);
	cmdctx = mhi_alloc(44, 4096);
	mhi_cmdring_off = mhi_alloc(MHI_TRES * 16, MHI_TRES * 16);
	if (chctx == UINT64_MAX || erctx == UINT64_MAX || cmdctx == UINT64_MAX || mhi_cmdring_off == UINT64_MAX) {
		hakios_tl("pcie2: MHI: the pool is too small for the contexts\n");
		return ENOMEM;
	}
	mhi_chctx = mhi_words(chctx);
	mhi_erctx = mhi_words(erctx);
	mhi_cmdctx = mhi_words(cmdctx);
	mhi_cmdring = mhi_words(mhi_cmdring_off);
	mhi_chctx_iova = mhi_pool.iova + chctx;
	mhi_erctx_iova = mhi_pool.iova + erctx;
	mhi_cmdctx_iova = mhi_pool.iova + cmdctx;
	mhi_cmdwp = 0;
	mhi_command_wait = 0;
	mhi_command_cc = -1;

	/* the twelve channel contexts the vendor fills, all idle */
	for (unsigned i = 0; i < 12; i++) {
		static const uint32_t ids[12] = { 2, 3, 4, 5, 10, 11, 14, 15, 20, 21, 100, 101 };
		unsigned id = ids[i];
		volatile uint32_t *c = mhi_channel_context(id);

		c[0] = (id >= 100 ? 3u : 2u) << 8;
		c[1] = (id & 1) ? 2 : 1;
		c[2] = id >= 100 ? id - 96 : (id < 12 ? 1 : 0);
	}

	for (unsigned e = 0; e < MHI_NER; e++) {
		struct mhi_event_ring *ring = &mhi_events[e];
		volatile uint32_t *c = mhi_erctx + e * 11;
		uint64_t base = mhi_alloc(mhi_event_counts[e] * 16, mhi_event_counts[e] * 16);

		if (base == UINT64_MAX) {
			return ENOMEM;
		}
		ring->words = mhi_words(base);
		ring->base = mhi_pool.iova + base;
		ring->count = mhi_event_counts[e];
		ring->consumer = 0;
		ring->wp = e < 4 ? ring->count - 1 : 0;
		c[0] = e == 1 ? 5u << 16 : (e >= 4 ? 1u << 16 : 0);
		c[1] = 1;
		c[2] = e + 1;
		mhi_set64(c, 3, ring->base);
		mhi_set64(c, 5, ring->count * 16);
		mhi_set64(c, 7, ring->base);
		mhi_set64(c, 9, ring->base + (uint64_t)ring->wp * 16);
		hakios_tl("pcie2: MHI: event ring %u: %u entries at %#llx\n", e, ring->count,
		    (unsigned long long)ring->base);
	}
	mhi_hw_events_ready = false;
	mhi_set64(mhi_cmdctx, 3, mhi_pool.iova + mhi_cmdring_off);
	mhi_set64(mhi_cmdctx, 5, MHI_TRES * 16);
	mhi_set64(mhi_cmdctx, 7, mhi_pool.iova + mhi_cmdring_off);
	mhi_set64(mhi_cmdctx, 9, mhi_pool.iova + mhi_cmdring_off);

	/* Write-ahead: the loader's log keeps only its first 256 KB and last
	 * 1 MB, so a stage whose lines fall in between is lost (run 204534 did
	 * exactly that). Each device access after this point is announced
	 * before it happens, and a wedged one names itself. */
	/* CRCBAP is the command ring's *context*, one 44-byte entry, not the
	 * ring itself (vendor md_address(0x68, md_cmdctx.a); the Wi-Fi MHI's
	 * write_address(0x68, commands.address) is the same 44-byte entry).
	 * The ring's own address goes into the context's words 3/7/9 above. */
	hakios_tl("pcie2: MHI: contexts: CCABAP %#llx, ECABAP %#llx, CRCBAP %#llx\n",
	    (unsigned long long)(mhi_pool.iova + chctx), (unsigned long long)(mhi_pool.iova + erctx),
	    (unsigned long long)(mhi_pool.iova + cmdctx));
	wr(MHI_BAR + 0x58, (uint32_t)(mhi_pool.iova + chctx));
	wr(MHI_BAR + 0x5c, 0);
	wr(MHI_BAR + 0x60, (uint32_t)(mhi_pool.iova + erctx));
	wr(MHI_BAR + 0x64, 0);
	wr(MHI_BAR + 0x68, (uint32_t)(mhi_pool.iova + cmdctx));
	wr(MHI_BAR + 0x6c, 0);
	/* the vendor's md_contexts writes the two windows here as well; BHI
	 * has run since this driver wrote them, so they are written again */
	wr(MHI_BAR + 0x80, mhi_win_base);
	wr(MHI_BAR + 0x84, 0);
	wr(MHI_BAR + 0x88, mhi_win_stop);
	wr(MHI_BAR + 0x8c, 0);
	wr(MHI_BAR + 0x98, mhi_win_base);
	wr(MHI_BAR + 0x9c, 0);
	wr(MHI_BAR + 0xa0, mhi_win_stop);
	wr(MHI_BAR + 0xa4, 0);
	hakios_tl("pcie2: MHI: contexts: arrays and windows written; reading MHICFG\n");
	cfg = rd(MHI_BAR + 0x10);
	if (cfg == UINT32_MAX) {
		hakios_tl("pcie2: MHI: MHICFG does not read\n");
		return EIO;
	}
	hakios_tl("pcie2: MHI: contexts: MHICFG %#x -> %#x; arming the doorbells\n", cfg,
	    (cfg & 0xffff) | (MHI_NER << 16) | (2u << 24));
	wr(MHI_BAR + 0x10, (cfg & 0xffff) | (MHI_NER << 16) | (2u << 24));
	mhi_publish();
	for (unsigned e = 0; e < 4; e++) {
		mhi_doorbell(mhi_erdb + e * 8, mhi_events[e].base + (uint64_t)mhi_events[e].wp * 16);
	}
	hakios_tl("pcie2: MHI: contexts: doorbells armed (ER0 wp %#llx); requesting M0\n",
	    (unsigned long long)(mhi_events[0].base + (uint64_t)mhi_events[0].wp * 16));
	error = update(MHI_BAR + 0x38, 0xff00, 0x200);
	if (error) {
		hakios_tl("pcie2: MHI: contexts: MHICTRL does not read (%d)\n", error);
		return error;
	}
	mhi_stage = "M0";
	hakios_tl("pcie2: MHI: contexts: M0 requested; waiting for the state\n");
	{
		/* wait_bits could not say when the endpoint left: it only reports
		 * the last value. Run 210742 read 0xffffffff on every MHI and BHI
		 * register once the wait was over, so this loop names the failure
		 * as it happens and stops on the first all-ones read. */
		uint32_t status = 0;
		unsigned ms;

		for (ms = 0; ms < 20000; ms += 100) {
			status = rd(MHI_BAR + 0x48);
			if (status == UINT32_MAX) {
				hakios_tl("pcie2: MHI: no M0: status reads all ones after %u ms\n", ms);
				break;
			}
			if ((status & 0xff04) == 0x200) {
				break;
			}
			if (status & 4) {
				hakios_tl("pcie2: MHI: no M0: status %#x has SYSERR after %u ms\n", status,
				    ms);
				break;
			}
			if (ms != 0 && (ms % 1000) == 0) {
				hakios_tl("pcie2: MHI: %u ms without M0 (status %#x, MHICTRL %#x)\n", ms,
				    status, rd(MHI_BAR + 0x38));
			}
			delay(100000);
		}
		if ((status & 0xff04) != 0x200) {
			hakios_tl("pcie2: MHI: no M0: status %#x after %u ms\n", rd(MHI_BAR + 0x48), ms);
			/* The host side of the question: is the endpoint still
			 * there? The RC's link status and AER view are readable
			 * even when the endpoint's BAR is not. */
			hakios_tl("pcie2: MHI: after the timeout: MHICTRL %#x, MHISTATUS %#x, BHI_STATUS %#x\n",
			    rd(MHI_BAR + 0x38), rd(MHI_BAR + 0x48), rd(mhi_bhi + 0x2c));
			hakios_tl("pcie2: MHI: after the timeout: DBI link %#x, ELBI %#x, PARF int %#x status %#x errors %#x\n",
			    rd(DBI + 0x80), rd(DBI + 0xf28), rd(PARF + 0x224), rd(PARF + 0x230),
			    rd(PARF + 0x3c0));
			hakios_tl("pcie2: MHI: after the timeout: AER uncorr %#x corr %#x root %#x\n",
			    rd(DBI + 0x104), rd(DBI + 0x110), rd(DBI + 0x130));
			/* and what the device would have read had it walked here */
			if (modem_space.on && smmu_va != 0) {
				volatile uint32_t *regs = (volatile uint32_t *)smmu_va;
				uint32_t idr1 = regs[0x024 / 4];
				uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
				uint32_t pages = 2u << ((idr1 >> 28) & 7);
				uint32_t at = (pages + modem_space.context) * page / 4;
				uint64_t ttbr0 = ((uint64_t)regs[at + SMMU_CB_TTBR0 / 4 + 1] << 32) |
				    regs[at + SMMU_CB_TTBR0 / 4];

				mmu_walk(modem_space.context, ttbr0, regs[at + SMMU_CB_TCR / 4],
				    mhi_pool.iova);
			}
			return ETIMEDOUT;
		}
	}
	mhi_stage = "channel";
	hakios_tl("pcie2: MHI: M0, six event rings, contexts at %#llx, doorbells %#x/%#x\n",
	    (unsigned long long)mhi_pool.iova, mhi_chdb, mhi_erdb);
	return 0;
}

static int
mhi_enable_hw_events(void)
{
	if (mhi_hw_events_ready) {
		return 0;
	}
	for (unsigned e = 4; e < MHI_NER; e++) {
		struct mhi_event_ring *ring = &mhi_events[e];
		volatile uint32_t *ctx = mhi_erctx + e * 11;

		if (ring->words == NULL || ring->count == 0) {
			return EPROTO;
		}
		ring->consumer = 0;
		ring->wp = ring->count - 1;
		mhi_set64(ctx, 9, ring->base + (uint64_t)ring->wp * 16);
		mhi_publish();
		mhi_doorbell(mhi_erdb + e * 8, ring->base + (uint64_t)ring->wp * 16);
	}
	mhi_hw_events_ready = true;
	hakios_tl("pcie2: MHI: hardware event rings ready (ER4 %u, ER5 %u)\n",
	    mhi_events[4].count, mhi_events[5].count);
	return 0;
}

/*
 * A channel is opened by a START_CHAN command entry in the command ring, and
 * the device answers with the command completion event (0x21) on event ring
 * 0 carrying that entry's address and code 1.
 */
static int
mhi_start_channel(unsigned slot, unsigned id, unsigned er)
{
	struct mhi_channel *c = &mhi_channels[slot];
	uint64_t ring = mhi_alloc(MHI_TRES * 16, MHI_TRES * 16);
	uint64_t buffer = mhi_alloc(MHI_PACKET, 4096);
	volatile uint32_t *ctx, *cmd;
	uint64_t wait, doorbell;
	int error;

	if (ring == UINT64_MAX || buffer == UINT64_MAX) {
		return ENOMEM;
	}
	c->id = id;
	c->er = er;
	c->wp = 0;
	c->db_mode = true;
	c->ring = mhi_words(ring);
	c->ring_base = mhi_pool.iova + ring;
	c->buffer = (volatile uint8_t *)(mhi_pool.io + buffer);
	c->buffer_iova = mhi_pool.iova + buffer;
	c->tx_pending = c->tx_done = c->rx_posted = c->rx_ready = false;
	c->rx_length = 0;
	ctx = mhi_channel_context(id);
	ctx[0] = id >= 100 ? 0x301 : 0x201;
	ctx[1] = (id & 1) ? 2 : 1;
	ctx[2] = er;
	mhi_set64(ctx, 3, c->ring_base);
	mhi_set64(ctx, 5, MHI_TRES * 16);
	mhi_set64(ctx, 7, c->ring_base);
	mhi_set64(ctx, 9, c->ring_base);

	cmd = mhi_cmdring + mhi_cmdwp * 4;
	cmd[0] = cmd[1] = cmd[2] = 0;
	cmd[3] = (id << 24) | (18u << 16);
	wait = mhi_pool.iova + mhi_cmdring_off + (uint64_t)mhi_cmdwp * 16;
	mhi_command_wait = wait;
	mhi_command_cc = -1;
	mhi_cmdwp = (mhi_cmdwp + 1) % MHI_TRES;
	doorbell = mhi_pool.iova + mhi_cmdring_off + (uint64_t)mhi_cmdwp * 16;
	mhi_set64(mhi_cmdctx, 9, doorbell);
	mhi_publish();
	hakios_tl("pcie2: MHI: channel %u: START_CHAN at %#llx, doorbell %#llx\n", id,
	    (unsigned long long)wait, (unsigned long long)doorbell);
	mhi_doorbell(0x70, doorbell);
	for (unsigned ms = 0; ms < 5000; ms++) {
		error = mhi_poll();
		if (error) {
			return error;
		}
		if (mhi_command_cc < 0 && (ms % 1000) == 999) {
			hakios_tl("pcie2: MHI: channel %u: %u ms without a command completion (last event ch %u code %u)\n",
			    id, ms + 1, mhi_last_channel, mhi_last_code);
		}
		if (mhi_command_cc >= 0) {
			if (mhi_command_cc != 1) {
				hakios_tl("pcie2: MHI: channel %u: the command completed with %d\n", id,
				    mhi_command_cc);
				return EIO;
			}
			hakios_tl("pcie2: MHI: channel %u open on event ring %u\n", id, er);
			return (id & 1) ? mhi_post(c, MHI_PACKET) : 0;
		}
		delay(1000);
	}
	hakios_tl("pcie2: MHI: channel %u: no command completion (last event ch %u code %u)\n", id,
	    mhi_last_channel, mhi_last_code);
	return ETIMEDOUT;
}

/*
 * The images the SBL can ask for, the vendor's builtin sahara.map with the
 * file names this tree carries (the vendor's mdm1m9kefs*.img are our
 * efs*.bin, its mdmddr.img our mdmddr.mbn; xbl_cfg.elf is not here). The set
 * is 88 MB, qdsp6sw.mbn alone 84, so it rides in the system image's
 * /fw/modem (build.sh, SDX55=1); root.apfs keeps only the sbl1.mbn the BHI
 * load needs, and is looked for first all the same.
 */
static const struct {
	uint32_t id;
	const char *name;
} mhi_images[] = {
	{ 6, "apps.mbn" },
	{ 8, "qdsp6sw.mbn" },
	{ 16, "efs1.bin" },
	{ 17, "efs2.bin" },
	{ 20, "efs3.bin" },
	{ 21, "sbl1.mbn" },
	{ 23, "aop.mbn" },
	{ 25, "tz.mbn" },
	{ 29, "acdb.mbn" },
	{ 33, "hyp.mbn" },
	{ 34, "mdmddr.mbn" },
	{ 36, "multi_image_qti.mbn" },
	{ 37, "multi_image.mbn" },
	{ 40, "apdp.mbn" },
	{ 41, "devcfg.mbn" },
	{ 42, "sec.elf" },
};

static const char *
mhi_image_name(uint64_t id)
{
	for (unsigned i = 0; i < sizeof(mhi_images) / sizeof(mhi_images[0]); i++) {
		if (mhi_images[i].id == id) {
			return mhi_images[i].name;
		}
	}
	return NULL;
}

static int
mhi_image_open(const char *name, vnode_t *vnode, uint64_t *size)
{
	static const char *const directories[] = { "/redwood/fw/modem", "/fw/modem" };
	vfs_context_t context = vfs_context_kernel();
	struct vnode_attr attributes;
	char path[96];
	int error = ENOENT;

	if (rootvnode == NULLVP) {
		return ENOENT;
	}
	for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", directories[i], name);
		error = vnode_lookup(path, 0, vnode, context);
		if (error) {
			continue;
		}
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		error = vnode_getattr(*vnode, &attributes, context);
		if (error || vnode_vtype(*vnode) != VREG || attributes.va_data_size == 0 ||
		    attributes.va_data_size > MHI_IMAGE_LIMIT) {
			vnode_put(*vnode);
			*vnode = NULLVP;
			error = error ? error : EINVAL;
			continue;
		}
		*size = (uint64_t)attributes.va_data_size;
		return 0;
	}
	return error;
}

static int
mhi_image_read(vnode_t vnode, uint64_t offset, void *data, size_t length)
{
	vfs_context_t context = vfs_context_kernel();
	int left = 0, error;

	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)length, (off_t)offset, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	if (error == 0 && left != 0) {
		error = EIO;
	}
	return error;
}

/*
 * The Sahara IDs the vendor's host serves from the phone's board-local stores,
 * and the partition each is read from.  Run 005621: with the stores' real first
 * bytes packed into the system image, the SBL read mdmddr's header (40 bytes),
 * then the 1596 bytes at 0x28 that header declares (3c 06 00 00 at 16), and the
 * 64-byte file stopped the whole image list at request 2 with ERANGE.  The
 * partition is the source then, read only, the way the vendor's own host does
 * it; the file stays the fallback for boots without ufs=2.
 */
static const char *
mhi_sahara_partition(uint64_t id)
{
	switch (id) {
	case 16:
		return "mdm1m9kefs1";
	case 17:
		return "mdm1m9kefs2";
	case 20:
		return "mdm1m9kefs3";
	case 34:
		return "mdmddr";
	}
	return NULL;
}

/*
 * One READ_DATA request: the named image's bytes from offset, count of them,
 * in the vendor's 32 KB packets on the host's TX channel, a progress line
 * every 8 MB.
 */
static int
mhi_sahara_read(uint64_t id, uint64_t offset, uint64_t length)
{
	const char *name = mhi_image_name(id);
	const char *partition = mhi_sahara_partition(id);
	vnode_t vnode = NULLVP;
	uint64_t size = 0, sent = 0, started = 0, now = 0;
	uint64_t next_report = 8ull * 1024 * 1024;
	bool from_partition = false;
	int error;

	if (name == NULL || length == 0 || offset > MHI_IMAGE_LIMIT || length > MHI_IMAGE_LIMIT - offset) {
		hakios_tl("pcie2: Sahara: image id %llu at %#llx+%#llx is not servable\n",
		    (unsigned long long)id, (unsigned long long)offset, (unsigned long long)length);
		return name == NULL ? ENOENT : ERANGE;
	}
	if (partition != NULL) {
		uint64_t bytes = 0;

		/* ENXIO without the loader's block page (no ufs=2 in the boot's
		 * arguments): then the file is the only source there is */
		if (hakios_ufs_read_partition(partition, 0, NULL, 0, &bytes) == 0 && bytes != 0) {
			size = bytes;
			from_partition = true;
		}
	}
	if (from_partition) {
		if (offset > size || length > size - offset) {
			hakios_tl("pcie2: Sahara: %s is %llu bytes, asked for %#llx+%#llx\n", partition,
			    (unsigned long long)size, (unsigned long long)offset, (unsigned long long)length);
			return ERANGE;
		}
		hakios_tl("pcie2: Sahara: READ_DATA id %llu (%s, the phone's partition) offset %llu length %llu of %llu\n",
		    (unsigned long long)id, partition, (unsigned long long)offset, (unsigned long long)length,
		    (unsigned long long)size);
	} else {
		/* the system volume carries the set and is mounted before the
		 * small one, but a request that arrives while it is still being
		 * attached is worth a wait, not a failed boot */
		for (unsigned attempt = 0; attempt < 10; attempt++) {
			error = mhi_image_open(name, &vnode, &size);
			if (error != ENOENT) {
				break;
			}
			delay(1000000);
		}
		if (error) {
			hakios_tl("pcie2: Sahara: %s is not on this boot's volumes (%d)\n", name, error);
			return error;
		}
		if (offset > size || length > size - offset) {
			hakios_tl("pcie2: Sahara: %s is %llu bytes, asked for %#llx+%#llx\n", name,
			    (unsigned long long)size, (unsigned long long)offset, (unsigned long long)length);
			vnode_put(vnode);
			return ERANGE;
		}
		hakios_tl("pcie2: Sahara: READ_DATA id %llu (%s) offset %llu length %llu of %llu\n",
		    (unsigned long long)id, name, (unsigned long long)offset, (unsigned long long)length,
		    (unsigned long long)size);
	}
	clock_get_uptime(&started);
	while (length) {
		size_t count = length > MHI_PACKET ? MHI_PACKET : (size_t)length;

		/* the task's kernel thread, so the block service's own sleeps
		 * are legal here; ufs_read loops internally over any length */
		error = from_partition
		    ? hakios_ufs_read_partition(partition, offset, mhi_chunk, count, NULL)
		    : mhi_image_read(vnode, offset, mhi_chunk, count);
		if (error) {
			break;
		}
		error = mhi_send(&mhi_channels[0], mhi_chunk, count);
		if (error) {
			break;
		}
		offset += count;
		length -= count;
		sent += count;
		if (sent >= next_report || length == 0) {
			uint64_t elapsed_ns = 0, elapsed_us, mbps;

			/* clock_get_uptime counts the counter's ticks, not
			 * nanoseconds; the delta is what converts exactly */
			clock_get_uptime(&now);
			if (now > started) {
				absolutetime_to_nanoseconds(now - started, &elapsed_ns);
			}
			elapsed_us = elapsed_ns / 1000;
			if (elapsed_us == 0) {
				elapsed_us = 1;
			}
			mbps = sent * 1000000 / elapsed_us / 1048576;
			hakios_tl("pcie2: Sahara: id %llu: %llu bytes in %llu ms (%llu MB/s)\n",
			    (unsigned long long)id, (unsigned long long)sent,
			    (unsigned long long)(elapsed_us / 1000), (unsigned long long)mbps);
			while (next_report <= sent) {
				next_report += 8ull * 1024 * 1024;
			}
		}
	}
	if (vnode != NULLVP) {
		vnode_put(vnode);
	}
	return error;
}

/*
 * The byte stream on the SBL's RX channel: packets are not messages, so the
 * header's size word is what says where one ends. EAGAIN while the SBL is
 * still the one answering is not an error -- the AMSS retires BHI -- but it
 * is not proof the AMSS runs either; that is BHI_EXECENV 2 below.
 */
static int
mhi_sahara_exact(void *data, size_t length)
{
	uint8_t *p = data;

	while (length) {
		size_t n;

		if (mhi_sahara_pos == mhi_sahara_size) {
			size_t received = MHI_PACKET;
			int error;

			error = mhi_fault();
			if (error) {
				return error;
			}
			if (rd(mhi_bhi + 0x28) == 2) {
				return EAGAIN;
			}
			/* One 20 s receive hid the handover: after the last image
			 * the SBL only acknowledges it, goes quiet, and the AMSS
			 * takes BHI over seconds later -- but the host sat the
			 * whole timeout out (run 030228: image 6 at 03:02:42,
			 * EE 2 noticed at 03:03:03).  Stock's host is answering
			 * DMS 2.2 s after its Sahara; every second spent here is
			 * a second of the modem's SHUTTING_DOWN window running
			 * without a host.  The wait is sliced so EE 2 ends it at
			 * once; a packet inside a slice is taken as before. */
			error = ETIMEDOUT;
			for (unsigned waited = 0; waited < 20000; waited += 200) {
				if (rd(mhi_bhi + 0x28) == 2) {
					return EAGAIN;
				}
				error = mhi_receive(&mhi_channels[1], mhi_sahara_stream,
				    &received, 200);
				if (error != ETIMEDOUT) {
					break;
				}
			}
			if (error) {
				return error;
			}
			if (received == 0) {
				return EIO;
			}
			mhi_sahara_size = received;
			mhi_sahara_pos = 0;
		}
		n = mhi_sahara_size - mhi_sahara_pos;
		if (n > length) {
			n = length;
		}
		memcpy(p, mhi_sahara_stream + mhi_sahara_pos, n);
		mhi_sahara_pos += n;
		p += n;
		length -= n;
	}
	return 0;
}

static int
mhi_sahara_message(uint8_t *packet, size_t *length)
{
	uint32_t size;
	int error = mhi_sahara_exact(packet, 8);

	if (error) {
		return error;
	}
	size = mhi_u32(packet + 4);
	if (size < 8 || size > *length) {
		return EPROTO;
	}
	*length = size;
	return mhi_sahara_exact(packet + 8, size - 8);
}

static int
mhi_sahara_hello(const uint8_t *packet, size_t length, uint32_t mode, uint32_t response[12])
{
	if (length != 48 || mhi_u32(packet + 8) < 2 || mhi_u32(packet + 12) > 2) {
		/* Run 215200's Sahara stage ended here -- ENOTSUP with no other
		 * line, so the SBL sent a command 1 the vendor's checks reject
		 * and the run could not say which field failed. This line does.
		 * Run 220739 then showed the mode check itself (mode <= 1)
		 * refusing a genuine re-handshake in mode 3 after mdmddr; the
		 * mode is the host's answer to give, so the caller decides it
		 * and the check here is only the packet's. */
		hakios_tl("pcie2: Sahara: HELLO refused: %zu bytes, header %#x %#x %#x %#x %#x %#x\n",
		    length, mhi_u32(packet), mhi_u32(packet + 4), mhi_u32(packet + 8),
		    mhi_u32(packet + 12), mhi_u32(packet + 16), mhi_u32(packet + 20));
		return ENOTSUP;
	}
	memset(response, 0, 48);
	response[0] = 2;
	response[1] = 48;
	response[2] = 2;
	response[3] = 1;
	response[5] = mode;
	return 0;
}

static int
mhi_sahara(void)
{
	uint64_t transferred = 0;
	unsigned mode3_streak = 0;
	int error;

	mhi_stage = "Sahara";
	error = mhi_start_channel(0, 2, 1);
	if (error == 0) {
		error = mhi_start_channel(1, 3, 1);
	}
	if (error) {
		hakios_tl("pcie2: Sahara: the channels did not open (%d)\n", error);
		return error;
	}
	for (unsigned requests = 0; requests < 16384; requests++) {
		size_t length = 256;
		uint32_t command;

		error = mhi_sahara_message(mhi_sahara_packet, &length);
		if (error == EAGAIN) {
			hakios_tl("pcie2: Sahara: BHI_EXECENV 2 after %u requests and %llu bytes\n", requests,
			    (unsigned long long)transferred);
			return 0;
		}
		if (error) {
			hakios_tl("pcie2: Sahara: no message (%d) at request %u\n", error, requests);
			return error;
		}
		command = mhi_u32(mhi_sahara_packet);
		/* the dialogue is a handful of packets; the stage-end line alone
		 * could not say what the SBL sent after mdmddr (run 215200) */
		hakios_tl("pcie2: Sahara: request %u: command %#x, %u bytes\n", requests, command,
		    (unsigned)length);
		if (command != 1) {
			mode3_streak = 0;
		}
		if (command == 1) {
			uint32_t hello[12];
			uint32_t offered = mhi_u32(mhi_sahara_packet + 20);
			uint32_t answer = 0;

			/* Run 220739: after the 40-byte mdmddr stub the SBL took
			 * the DONE, answered DONE_RSP status 0, and re-handshook
			 * with HELLO mode 3 (SAHARA_MODE_COMMAND) -- which the
			 * vendor's mode <= 1 check refused. In the protocol the
			 * mode in HELLO_RSP is the host's to give; the reference
			 * host answers a re-handshake with image transfer and
			 * continues. That is the first answer here. Only a second
			 * mode offer with no packet in between (the SBL not
			 * accepting it) is answered as offered, and in command
			 * mode this host sends no command at all. */
			if (offered > 1) {
				if (mode3_streak++ == 0) {
					hakios_tl("pcie2: Sahara: HELLO offers mode %u; answering image transfer to keep the image list\n",
					    offered);
				} else {
					answer = offered;
					hakios_tl("pcie2: Sahara: HELLO offers mode %u again; answering it, no command follows (read-only)\n",
					    offered);
				}
			}
			error = mhi_sahara_hello(mhi_sahara_packet, length, answer, hello);
			if (error) {
				return error;
			}
			hakios_tl("pcie2: Sahara: HELLO, answering mode %u\n", hello[5]);
			error = mhi_send(&mhi_channels[0], hello, sizeof(hello));
		} else if (command == 3 || command == 0x12) {
			uint64_t id, offset, count;

			if (length != (command == 3 ? 20 : 32)) {
				return EPROTO;
			}
			id = command == 3 ? mhi_u32(mhi_sahara_packet + 8) : mhi_u64(mhi_sahara_packet + 8);
			offset = command == 3 ? mhi_u32(mhi_sahara_packet + 12) : mhi_u64(mhi_sahara_packet + 16);
			count = command == 3 ? mhi_u32(mhi_sahara_packet + 16) : mhi_u64(mhi_sahara_packet + 24);
			if (count > MHI_IMAGE_LIMIT - transferred) {
				return EFBIG;
			}
			error = mhi_sahara_read(id, offset, count);
			if (error == 0) {
				transferred += count;
				mhi_sahara_bytes = transferred;
			}
		} else if (command == 4) {
			uint32_t image, status;

			if (length != 16) {
				return EPROTO;
			}
			image = mhi_u32(mhi_sahara_packet + 8);
			status = mhi_u32(mhi_sahara_packet + 12);
			hakios_tl("pcie2: Sahara: image %u complete, status %u\n", image, status);
			if (status) {
				return EIO;
			}
			mhi_sahara_images++;
			{
				uint32_t done[2] = { 5, 8 };

				error = mhi_send(&mhi_channels[0], done, sizeof(done));
			}
		} else if (command == 5) {
			/* the device's DONE_REQ: the image list is over, and the
			 * host's answer is DONE_RSP (12 bytes, status 0). No run
			 * has seen one yet -- the SBL answered the host's own
			 * DONE with 6 -- so this is the protocol-complete path
			 * for the last image, not an observed one. */
			uint32_t done[3] = { 6, 12, 0 };

			if (length != 8) {
				return EPROTO;
			}
			hakios_tl("pcie2: Sahara: DONE from the SBL after %llu bytes; answering\n",
			    (unsigned long long)transferred);
			error = mhi_send(&mhi_channels[0], done, sizeof(done));
			if (error == 0) {
				return 0;
			}
		} else if (command == 6) {
			if (length != 12 || mhi_u32(mhi_sahara_packet + 8) > 1) {
				return EPROTO;
			}
			if (mhi_u32(mhi_sahara_packet + 8) == 1) {
				hakios_tl("pcie2: Sahara: complete, %llu bytes served\n",
				    (unsigned long long)transferred);
				return 0;
			}
		} else {
			hakios_tl("pcie2: Sahara: refusing command %#x of %u bytes (read-only policy)\n", command,
			    (unsigned)length);
			return ENOTSUP;
		}
		if (error) {
			hakios_tl("pcie2: Sahara: request %u (command %#x) failed: %d\n", requests, command, error);
			return error;
		}
	}
	return ELOOP;
}

/*
 * What the endpoint does after the SBL's last image: run 222749 served all
 * sixteen images and then read all ones from BAR0 and the BHI block, which
 * the driver reported as a fault (EIO, "no message (5)") -- but the vendor's
 * own host treats the SBL going quiet after the image list as expected and
 * waits up to 20 s for BHI_EXECENV 2.  This samples the endpoint's config
 * space, the link, BHI_EXECENV and MHISTATUS every 200 ms and logs the
 * transitions plus a 5 s heartbeat, so one run can say whether the modem went away, came back,
 * and in which execution environment -- or stayed gone.  It claims nothing
 * by itself; the stage's last line is still the one that says what happened.
 */
static unsigned pcie2_pin_in(unsigned pin);

static void
mhi_amss_watch(void)
{
	uint32_t id = rd(CONF), link = rd(DBI + 0x80) & 0x20000000u;
	uint32_t execenv = rd(mhi_bhi + 0x28), status = rd(MHI_BAR + 0x48);
	unsigned mstatus = pcie2_pin_in(3), errfatal = pcie2_pin_in(1);

	hakios_tl("pcie2: MHI: after %u images (%llu bytes): endpoint id %#x, link %#x, BHI_EXECENV %#x, "
	    "MHISTATUS %#x, MDM2AP_STATUS %u, MDM2AP_ERRFATAL %u\n", mhi_sahara_images, mhi_sahara_bytes, id,
	    link, execenv, status, mstatus, errfatal);
	/* 60 s: the modem's own boot after the image list has to finish before
	 * its PCIe can come back, and the last run's 20 s window saw none of it.
	 * The modem's status lines are sampled with the bus because they do not
	 * depend on the link -- they say whether the modem itself is alive.
	 * A heartbeat every 5 s (line 25 of every 25) keeps a recent state line
	 * in the newest part of the console log: the kernel keeps a 256 KB head
	 * and the newest 1 MB (osfmk/arm64/loader_return.c), and runs 014904 and
	 * 015338 lost the exchange's tail -- including whatever came after id 8
	 * -- in the dropped middle even at XL_SECS=45, because the boot logs
	 * faster than the window shrinks. The transitions stay edge-triggered;
	 * only silence also gets a line. */
	for (unsigned i = 0; i < 300 && execenv != 2; i++) {
		uint32_t next_id, next_link, next_execenv, next_status;
		unsigned next_mstatus, next_errfatal;

		delay(200000);
		next_id = rd(CONF);
		next_link = rd(DBI + 0x80) & 0x20000000u;
		next_execenv = rd(mhi_bhi + 0x28);
		next_status = rd(MHI_BAR + 0x48);
		next_mstatus = pcie2_pin_in(3);
		next_errfatal = pcie2_pin_in(1);
		if (next_id != id || next_link != link || next_execenv != execenv || next_status != status ||
		    next_mstatus != mstatus || next_errfatal != errfatal) {
			hakios_tl("pcie2: MHI: at %u ms: endpoint id %#x, link %#x, BHI_EXECENV %#x, MHISTATUS %#x, "
			    "MDM2AP_STATUS %u, MDM2AP_ERRFATAL %u\n", (i + 1) * 200, next_id, next_link, next_execenv,
			    next_status, next_mstatus, next_errfatal);
			id = next_id;
			link = next_link;
			execenv = next_execenv;
			status = next_status;
			mstatus = next_mstatus;
			errfatal = next_errfatal;
		} else if ((i + 1) % 25 == 0) {
			hakios_tl("pcie2: MHI: at %u ms: %u images, %llu bytes served, BHI_EXECENV %#x, "
			    "MHISTATUS %#x, link %#x, MDM2AP_STATUS %u\n", (i + 1) * 200, mhi_sahara_images,
			    mhi_sahara_bytes, next_execenv, next_status, next_link, next_mstatus);
		}
	}
	if (execenv == 2) {
		hakios_tl("pcie2: MHI: the AMSS has taken BHI over (BHI_EXECENV 2)\n");
	}
}

/*
 * One QMI request and its answer over QMI0: the request goes out on channel
 * 14, the answer comes back on 15.  A transfer the modem has not yet taken is
 * re-announced to its doorbell rather than posted a second time over it.  The
 * answer's first bytes say what it is: 0x01 marker, the length, the QMUX
 * flags/service/client, then the type at [6] and, for a data service, the
 * transaction at [7..8] and the message id at [9..10].
 */
#define QMI_ANSWER      256u
static uint8_t mhi_qmi_answer[QMI_ANSWER];
static unsigned mhi_qmi_answer_length;

static int
mhi_qmi_roundtrip(const uint8_t *request, size_t n, const char *label, unsigned tries)
{
	for (unsigned attempt = 0; attempt < tries; attempt++) {
		volatile uint32_t *txctx = mhi_channel_context(14);
		uint64_t rp = mhi_u64(txctx + 7);
		size_t length = MHI_PACKET;
		int error;

		if (!mhi_channels[2].tx_pending) {
			error = mhi_send(&mhi_channels[2], request, n);
			if (error != 0 && error != ETIMEDOUT) {
				hakios_tl("pcie2: AMSS: %s: send %d (try %u)\n", label, error,
				    attempt + 1);
				return error;
			}
		} else {
			mhi_doorbell(mhi_chdb + 14 * 8,
			    mhi_channels[2].ring_base + (uint64_t)mhi_channels[2].wp * 16);
		}
		error = mhi_receive(&mhi_channels[3], mhi_sahara_stream, &length, 5000);
		if (error == 0) {
			uint8_t *p = mhi_sahara_stream;
			unsigned shown = (unsigned)length > 40 ? 40 : (unsigned)length;
			char hex[3 * 40 + 1];

			for (unsigned i = 0; i < shown; i++) {
				snprintf(hex + i * 3, 4, " %02x", p[i]);
			}
			hex[shown * 3] = 0;
			mhi_qmi_answer_length = (unsigned)length > QMI_ANSWER ? QMI_ANSWER :
			    (unsigned)length;
			memcpy(mhi_qmi_answer, p, mhi_qmi_answer_length);
			hakios_tl("pcie2: AMSS: %s: %u bytes:%s%s\n", label, (unsigned)length, hex,
			    (unsigned)length > shown ? " ..." : "");
			return 0;
		}
		hakios_tl("pcie2: AMSS: %s: no answer (try %u: %d; TX rp %#llx -> %#llx)\n",
		    label, attempt + 1, error, (unsigned long long)rp,
		    (unsigned long long)mhi_u64(txctx + 7));
	}
	return ETIMEDOUT;
}

/* What the channel's context holds, from the device's side: rbase/rlen are
 * the host's own last writes, rp/wp are the ones the device moves. */
static void
mhi_log_channel(unsigned id)
{
	volatile uint32_t *c = mhi_channel_context(id);

	hakios_tl("pcie2: AMSS: channel %u context: state %#x, type %#x, er %u, rbase %#llx, "
	    "rlen %#llx, rp %#llx, wp %#llx\n", id, c[0], c[1], c[2],
	    (unsigned long long)mhi_u64(c + 3), (unsigned long long)mhi_u64(c + 5),
	    (unsigned long long)mhi_u64(c + 7), (unsigned long long)mhi_u64(c + 9));
}

/* Everything the device's side of the conversation can be read from: the
 * engine's registers, both QMI0 contexts, every event ring's pointers (a
 * completion that never came shows as an rp that never moved, and one that
 * came to a ring the host does not drain shows as an rp that did), the TREs
 * the host wrote, and a stage 1 fault if the device's DMA was refused. */
static void
mhi_amss_dump(const char *why)
{
	hakios_tl("pcie2: AMSS: %s: MHISTATUS %#x, MHICTRL %#x, MHICFG %#x, CHDBOFF %#x, "
	    "ERDBOFF %#x\n", why, rd(MHI_BAR + 0x48), rd(MHI_BAR + 0x38), rd(MHI_BAR + 0x10),
	    rd(MHI_BAR + 0x18), rd(MHI_BAR + 0x20));
	mhi_log_channel(14);
	mhi_log_channel(15);
	for (unsigned e = 0; e < MHI_NER; e++) {
		volatile uint32_t *c = mhi_erctx + e * 11;

		hakios_tl("pcie2: AMSS: %s: ER%u rbase %#llx rlen %#llx rp %#llx wp %#llx\n",
		    why, e, (unsigned long long)mhi_u64(c + 3),
		    (unsigned long long)mhi_u64(c + 5), (unsigned long long)mhi_u64(c + 7),
		    (unsigned long long)mhi_u64(c + 9));
	}
	for (unsigned s = 2; s < 4; s++) {
		struct mhi_channel *c = &mhi_channels[s];

		if (c->ring != NULL) {
			hakios_tl("pcie2: AMSS: %s: ch%u TRE0 %#x %#x %#x %#x, the host's wp %u\n",
			    why, c->id, c->ring[0], c->ring[1], c->ring[2], c->ring[3], c->wp);
		}
	}
	modem_context_fsr(false);
}

/*
 * QRTR over IPCR -- channels 20 (out) and 21 (in), event ring 2 in the
 * vendor's table -- is the pair Android's net/qrtr/mhi.c rides, and the first
 * frame that driver's endpoint sends is this hello: a v1 header (version,
 * type, source node and port, confirm_rx, payload length, destination node
 * and port) with the 20-byte control packet behind it, QRTR_TYPE_HELLO in
 * its first word.
 *
 * Run 030922: the hello's transfer completed and the modem answered in 36
 * bytes -- its own header, version 3 (the vendor tree's qrtr_hdr_v2: the
 * four version/type/flags/optlen bytes, a 32-bit size, then 16-bit node and
 * port fields), a HELLO of size 20 from node 2 at the control port to the
 * broadcast node.  So the AMSS's data path is up and running for this host,
 * QMI0 is simply not what this image serves, and the answer names the node
 * the next request goes to.  What follows the hello is the modem's own
 * announcements: the services it hosts, UIM first among them.
 */
static const uint8_t mhi_qrtr_hello[52] = {
	0x01, 0x00, 0x00, 0x00,		/* version 1 */
	0x02, 0x00, 0x00, 0x00,		/* QRTR_TYPE_HELLO */
	0x01, 0x00, 0x00, 0x00,		/* the host's node */
	0xfe, 0xff, 0xff, 0xff,		/* QRTR_PORT_CTRL */
	0x00, 0x00, 0x00, 0x00,		/* confirm_rx */
	0x14, 0x00, 0x00, 0x00,		/* payload: the control packet */
	0xff, 0xff, 0xff, 0xff,		/* broadcast */
	0xfe, 0xff, 0xff, 0xff,		/* the control port */
	0x02, 0x00, 0x00, 0x00,		/* cmd: QRTR_TYPE_HELLO */
	0x00, 0x00, 0x00, 0x00,		/* the control packet's union, empty */
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

static uint32_t
mhi_qrtr_word(const uint8_t *frame, unsigned at)
{
	return (uint32_t)frame[at] | (uint32_t)frame[at + 1] << 8 |
	    (uint32_t)frame[at + 2] << 16 | (uint32_t)frame[at + 3] << 24;
}

static void
mhi_qrtr_put(uint8_t *frame, unsigned at, uint32_t value)
{
	frame[at] = (uint8_t)value;
	frame[at + 1] = (uint8_t)(value >> 8);
	frame[at + 2] = (uint8_t)(value >> 16);
	frame[at + 3] = (uint8_t)(value >> 24);
}

/* One frame out in the version 3 header the modem itself uses (the vendor
 * tree's qrtr_hdr_v2): version 3, type, flags, optlen, a 32-bit size, then
 * 16-bit node and port fields.  The control port 0xfffffffe travels as its
 * low half, 0xfffe -- the modem's own frames carry exactly that. */
static size_t
mhi_qrtr_put3(uint8_t *frame, unsigned type, unsigned src_port, unsigned dst_node,
    unsigned dst_port, const void *payload, unsigned size)
{
	frame[0] = 3;
	frame[1] = (uint8_t)type;
	frame[2] = 0;					/* flags */
	frame[3] = 0;					/* optlen */
	mhi_qrtr_put(frame, 4, size);
	frame[8] = 1;					/* the host's node */
	frame[9] = 0;
	frame[10] = (uint8_t)src_port;
	frame[11] = (uint8_t)(src_port >> 8);
	frame[12] = (uint8_t)dst_node;
	frame[13] = (uint8_t)(dst_node >> 8);
	frame[14] = (uint8_t)dst_port;
	frame[15] = (uint8_t)(dst_port >> 8);
	if (size != 0) {
		memcpy(frame + 16, payload, size);
	}
	return 16 + size;
}

/* The same frame, with the size field and the bytes on the wire told apart:
 * libqmi sends a seven-byte QMI message as a 24-byte frame whose size field
 * says 7 -- the frame is padded to a multiple of four, the size is not. */
static size_t
mhi_qrtr_put3_sized(uint8_t *frame, unsigned type, unsigned src_port, unsigned dst_node,
    unsigned dst_port, const void *payload, unsigned size, unsigned length)
{
	(void)mhi_qrtr_put3(frame, type, src_port, dst_node, dst_port, payload, size);
	mhi_qrtr_put(frame, 4, size);
	if (length > 16 + size) {
		memset(frame + 16 + size, 0, length - (16 + size));
	}
	return length;
}

/* The same frame padded to the four-byte multiple the transport demands.
 * Linux's qrtr-mhi will not transmit any other length, and the modem's side
 * behaves the same way: its own 30-byte DMS answer arrives as 32, and run
 * 105401's OACK -- 54 bytes, size 38 -- drew no DATA, no retransmission and
 * no error, while the padded QMI frames are answered.  Every TFTP frame goes
 * out through here. */
static size_t
mhi_qrtr_put3_frame(uint8_t *frame, unsigned type, unsigned src_port, unsigned dst_node,
    unsigned dst_port, const void *payload, unsigned size)
{
	return mhi_qrtr_put3_sized(frame, type, src_port, dst_node, dst_port, payload, size,
	    (16u + size + 3u) & ~3u);
}

/* The payload length the frame's own header declares: the padding the
 * transport adds is not payload.  (Without this the zero pad of a short last
 * DATA block would be kept as file bytes, and a bare WRQ padded to four
 * would parse as if it carried options.) */
static size_t
mhi_qrtr_payload(const uint8_t *frame, size_t size, unsigned header)
{
	uint32_t declared = header == 32 ? mhi_qrtr_word(frame, 20) : mhi_qrtr_word(frame, 4);

	if ((size_t)header + declared < size) {
		size = (size_t)header + declared;
	}
	return size;
}

/* The flow-control answer a frame with the confirm bit is owed, as Linux's
 * qrtr_send_resume_tx sends it and umi's md_qrtr_packet does before every
 * dispatch: from and naming the port the frame was addressed to.  The
 * modem's frames observed so far carry flags 0, but a flow-controlled
 * sender stalls until this arrives. */
static void
mhi_qrtr_resume(struct mhi_channel *out, unsigned our_port, unsigned node, unsigned peer)
{
	uint32_t resume[5] = { 7, 1, our_port, 0, 0 };
	uint8_t frame[16 + sizeof(resume)];

	(void)mhi_send(out, frame, mhi_qrtr_put3(frame, 7, our_port, node, peer, resume,
	    sizeof(resume)));
}

/* The services the modem expects on the AP side before it leaves OFFLINE,
 * by the numbers stock's userspace gives them (drivers/modem/hakios.mpss.c,
 * the other phone's port, serves the same three): 14 RMTFS, the modem's file
 * systems; 64 the service registry's locator (pd-mapper); 4096 TFTP
 * (tqftpserv), the files the modem reads while it boots.  Phone #1's modem
 * answered DMS Set Operating Mode with error 52, exactly as this one does,
 * until those services were served, and then accepted ONLINE.  The instances
 * are the wire form (version | instance << 8): RMTFS 1.0, servreg 1.1, TFTP
 * 1.0 and 3.0 (tqftpserv publishes both versions, and the SDX55's client
 * looks up version 3).  The ports number from 0x3000, above the source port
 * the QMI calls use (0x1001) and below Linux's dynamic range (0x4000). */
/* What stock's phones offer the modem, and nothing else: on the thyme,
 * qrtr-ns announces the servers its userspace clients registered -- pd-mapper
 * (the service registry, version 257) and tftp_server (QMI TFTP) -- and
 * rmt_storage is stopped (init.svc.vendor.rmt_storage: stopped, no
 * /dev/qcom_rmtfs_mem*), so this platform's modem asks for no RMTFS at all.
 * Advertising it here would only invite a request this host cannot serve. */
static const struct {
	unsigned service;
	unsigned instance;
	unsigned port;
	const char *name;
} mhi_host_services[] = {
	{ 64,   257, 0x3002, "service registry" },
	{ 4096, 1,   0x3003, "TFTP (v1)" },
	/* Two registrations, as umi's md_tftp_announce sends them: version 1
	 * and version 3 ({4,4096,1,1,...} and {4,4096,3,1,...}).  Run 062843
	 * announced version 1 alone, the modem never asked for a single file,
	 * and DMS answered ONLINE with FAILURE (1) -- the version 3 lookup its
	 * client makes found no server. */
	{ 4096, 3,   0x3004, "TFTP (v3)" },
	/* The modem's IMS stack is a client of the phone's IMS data service --
	 * Android's imsdatadaemon, whose QMI IDL is "IMSDCM".  Restarted on the
	 * reference phone, it registers three servers with the name service
	 * within 200 ms: services 0x302 and 0x28, both instance 1
	 * (docs/cellular.md, the 2026-11-24 capture).  Both are announced; the
	 * log says which port the stack writes to, and that port answers. */
	{ 0x302, 1,   0x3005, "IMS data (IMSDCM)" },
	{ 0x28,  1,   0x3006, "IMS data (0x28)" },
};

#define MHI_HOST_SERVICE_COUNT \
	((unsigned)(sizeof(mhi_host_services) / sizeof(mhi_host_services[0])))

/* Which of the host's services a destination port names, or -1. */
static int
mhi_host_service(unsigned port)
{
	for (unsigned i = 0; i < MHI_HOST_SERVICE_COUNT; i++) {
		if (mhi_host_services[i].port == port) {
			return (int)i;
		}
	}
	return -1;
}

/* ---- the service registry (QMI service 64): stock's pd-mapper, replayed ---- */
/*
 * Run 051432 was the first with the host's services announced, and the first
 * evidence of what the modem's boot waits for: two requests to the host's
 * registry, from node 3 port 0x5a to 1:0x3002, QMI message 0x21 -- "where is
 * the service avs/audio", "where is tms/pddump_disabled" -- and then, still
 * unanswered, the death at ~60 s that runs 043256 and 045244 also had.
 *
 * The answers stock gives come from the phone's own *.jsn files under
 * /vendor/firmware_mnt/image, read here as a table: each file is one domain
 * (soc/domain/subdomain and its QMI instance) with the services it provides.
 * The wire format is pd-mapper's, ported from phone #1 (drivers/modem/
 * hakios.mpss.c, servreg_receive): result TLV, then TLV 0x10 the count, 0x11
 * the revision, 0x12 the list of domain strings with their instances.  A
 * service no file lists ("tms/pddump_disabled" is one) gets success and an
 * empty list, which is what pd-mapper answers there too.
 */
struct mhi_servreg {
	const char *service;
	const char *domain;
	unsigned instance;
};
static const struct mhi_servreg mhi_servreg[] = {
	{ "tms/servreg", "mdm/modem/root_pd", 165 },	/* sdx55m/modemr.jsn */
	{ "tms/pdr_enabled", "mdm/modem/root_pd", 165 },
	{ "gps/gps_service", "mdm/modem/root_pd", 165 },
	{ "tms/servreg", "msm/adsp/root_pd", 74 },	/* adspr.jsn */
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },	/* adspua.jsn */
	{ "avs/audio", "msm/adsp/audio_pd", 74 },
	{ "tms/servreg", "msm/cdsp/root_pd", 76 },	/* cdspr.jsn */
	{ "tms/servreg", "msm/slpi/root_pd", 90 },	/* slpir.jsn */
	{ "tms/servreg", "msm/slpi/sensor_pd", 90 },	/* slpius.jsn */
};
#define MHI_SERVREG_COUNT \
	((unsigned)(sizeof(mhi_servreg) / sizeof(mhi_servreg[0])))

/* The value of one TLV of a QMI request, or NULL. */
static const uint8_t *
mhi_qmi_tlv(const uint8_t *message, size_t length, unsigned type, unsigned *size)
{
	unsigned at = 7;

	if (length < 7 || message[0] != 0) {
		return NULL;
	}
	while (at + 3 <= length) {
		unsigned this_type = message[at];
		unsigned this_size = message[at + 1] | (unsigned)message[at + 2] << 8;

		if (at + 3 + this_size > length) {
			break;
		}
		if (this_type == type) {
			*size = this_size;
			return message + at + 3;
		}
		at += 3 + this_size;
	}
	return NULL;
}

/* One registry request, answered where it came from.  The frame's header is
 * 16 bytes (version 3, what the modem itself speaks) or 32 (version 1). */
static void
mhi_servreg_answer(struct mhi_channel *out, const uint8_t *frame, size_t size,
    unsigned header, const char *what)
{
	const uint8_t *message, *value;
	uint8_t answer[1200], reply[16 + sizeof(answer)];
	size_t answered;
	unsigned node, port, at = 7 + 7, found = 0, value_size = 0;
	unsigned message_id;
	char wanted[64] = "";

	if (size < header + 7) {
		return;
	}
	message = frame + header;
	message_id = message[3] | (unsigned)message[4] << 8;
	if (header == 32) {
		node = mhi_qrtr_word(frame, 8);
		port = mhi_qrtr_word(frame, 12);
	} else {
		node = frame[8] | (unsigned)frame[9] << 8;
		port = frame[10] | (unsigned)frame[11] << 8;
	}
	if (message_id != 0x21) {
		/* GET_DOMAIN_LIST is what the modem has asked; a failure report
		 * (0x24) is only logged, and anything else is unknown */
		hakios_tl("pcie2: AMSS: %s: the registry is asked message %#x (%u bytes); not answered\n",
		    what, message_id, (unsigned)(size - header));
		return;
	}
	value = mhi_qmi_tlv(message, size - header, 1, &value_size);
	if (value != NULL) {
		snprintf(wanted, sizeof(wanted), "%.*s",
		    (int)(value_size < sizeof(wanted) - 1 ? value_size : sizeof(wanted) - 1),
		    (const char *)value);
	}
	memset(answer, 0, sizeof(answer));
	answer[0] = 2;				/* QMI response */
	answer[1] = message[1];
	answer[2] = message[2];
	answer[3] = message[3];
	answer[4] = message[4];
	answer[7] = 2;				/* result: success */
	answer[8] = 4;
	answer[9] = 0;				/* error: none */
	for (unsigned i = 0; i < MHI_SERVREG_COUNT; i++) {
		found += strcmp(mhi_servreg[i].service, wanted) == 0;
	}
	answer[at++] = 0x10;			/* how many domains in all */
	answer[at++] = 2;
	answer[at++] = 0;
	answer[at++] = (uint8_t)found;
	answer[at++] = (uint8_t)(found >> 8);
	answer[at++] = 0x11;			/* the table's revision */
	answer[at++] = 2;
	answer[at++] = 1;
	answer[at++] = 0;
	if (found != 0) {
		unsigned list = at;

		answer[at++] = 0x12;		/* the list itself */
		at += 2;			/* the length, filled in below */
		answer[at++] = (uint8_t)found;
		for (unsigned i = 0; i < MHI_SERVREG_COUNT && at + 64 < sizeof(answer); i++) {
			size_t name;

			if (strcmp(mhi_servreg[i].service, wanted) != 0) {
				continue;
			}
			name = strlen(mhi_servreg[i].domain);
			answer[at++] = (uint8_t)name;
			memcpy(answer + at, mhi_servreg[i].domain, name);
			at += (unsigned)name;
			for (unsigned b = 0; b < 4; b++) {
				answer[at++] = (uint8_t)(mhi_servreg[i].instance >> (8 * b));
			}
			answer[at++] = 0;	/* no service data */
			at += 4;
		}
		answer[list + 1] = (uint8_t)(at - list - 3);
		answer[list + 2] = (uint8_t)((at - list - 3) >> 8);
	}
	answer[5] = (uint8_t)(at - 7);
	answer[6] = (uint8_t)((at - 7) >> 8);
	answered = mhi_qrtr_put3(reply, 1, 0x3002, node, port, answer, at);
	hakios_tl("pcie2: AMSS: %s: the registry: \"%s\" is in %u domain%s; answer %s\n",
	    what, wanted, found, found == 1 ? "" : "s",
	    mhi_send(out, reply, answered) == 0 ? "sent" : "not sent");
}

/* ---- the IMS data service (the vendor "IMSDCM"): stock's imsdatadaemon ---- */
/*
 * Android's imsdatadaemon is not only a QMI client: the modem's IMS stack
 * talks to it as a server.  Restarted on the reference phone with qrtr-ns
 * under strace (2026-11-24, .scratch/imscap), it registers three servers
 * with the name service within 200 ms -- 0x302 and 0x28, both instance 1 --
 * and then holds this conversation with the modem's client at node 3 port
 * 0x3e:
 *
 *	< 0x0021 {0x01[1]=15, 0x10[4]=2}
 *	> 0x0021 {result 0, 0x10[1]=0xfa, 0x11[4]=2}   (143-164 ms later
 *	> 0x0020 {0x02[4]=00000d00, 0x01[1]=15, 0x10[4]=W, 0x12[4]=2}
 *	    the ready; the modem answers it with IMSA 0x26 flag 0 in 14 ms)
 *	< 0x0020 {0x01[20]=03"ims"...0200ffff, 0x10[4]=P, 0x11[4]=1,
 *	    0x12[4]=2, 0x13[4]=2}                       (8.4-10.4 s later)
 *	> 0x0020 {result 0, 0x10[1]=15, 0x11[4]=P, 0x12[4]=2}
 *	> 0x0020 {0x02[4]=0, 0x01[1]=15, 0x10[4]=P,
 *	    0x11[43]=01 00 00 00 26 "240a:42e6:810:...", 0x12[4]=2}
 *	    (0.44-0.53 s after the answer, once WDS answers;
 *	    the modem's IMSA 0x26 flag 1 follows in 16 ms)
 *
 * Both traces, read back with imsseq.py on 2026-11-24, carry W 0x64 and
 * P 0x65, and the address repeats P.  The ready announces the data
 * profile that is already up, not the IMS one the stack is about to ask
 * for, and it always carries the fixed 0x64 (mhi_imsdcm_ready).  (An
 * earlier transcription here had W 0x65 for imscap's exchange; neither
 * trace shows that.)
 *
 * The modem asks which IMS data service the host runs, hands it the APN it
 * wants ("ims"), and is handed the IMS PDN's IPv6 address as text.  Until
 * that exchange the modem's IMS service (0x12) and IMSA (0x21) answer every
 * request result 1 error 70 -- run 030016's 0x0048, 0x008f and 0x0090
 * reads, the IMSA 0x0020/0x0021 polls and the dial's error 48 -- and
 * 030016's log holds no request to any host service at all: the host had
 * announced only the registry and TFTP, nothing an IMS stack asks for.
 *
 * This is that service, replayed: the same messages and TLVs, and the
 * address from the IMS PDN the driver itself brings up (mhi_wds_ims_up,
 * whose fresh client's 0x26 TLV carries it).  Which of 0x302/0x28 the stack
 * writes to is left to the log; that port answers. */
static unsigned mhi_imsdcm_node, mhi_imsdcm_port, mhi_imsdcm_from;
static char mhi_imsdcm_address[64];
static unsigned mhi_imsdcm_txn = 4;
/* The profile the stack's APN request names in TLV 0x10 (0x65 in both
 * Android captures) and whether it has asked at all.  Android's daemon
 * sends the address indication only after that request and repeats the
 * request's word in it; the ready indication that leads the request
 * carries the fixed 0x64 (mhi_imsdcm_ready). */
static uint32_t mhi_imsdcm_profile = 100;
static bool mhi_imsdcm_asked, mhi_imsdcm_indicated;
/* The ask's TLV 0x11 word (the family: 0 V4, 1 V6).  The CMCC run's
 * early ask (191905, 19:19:34, before any bind) carried 1/2/2 -- Android's
 * own family words -- while the re-asks carried 0/1/1; logged again with
 * every address indication, so a lost ask line does not lose the value. */
static unsigned mhi_imsdcm_ask11 = ~0u;
/* The 0x12 word the stack's ask names.  Android's working ask carries 2 and
 * its answer and address indication repeat 2 (imscap2 06:17:32.02 -> .56);
 * this firmware's ask carries 1 (every run), while the daemon's answer and
 * address kept the capture's fixed 2 -- a word that names an instance the
 * stack never bound.  Echo the ask's own word in both. */
static uint32_t mhi_imsdcm_ask12 = 2;
/* The instance word the stack's bind names (its TLV 0x10).  Android's bind
 * names 2 and its ready carries 0x12 = 2 (imscap2 06:17:21.45 -> .61); this
 * firmware's binds name 1, and the ready kept the capture's fixed 2.  Run
 * 191905's stack destroyed its instance (0x33) 0.6 s after an answer that
 * named the wrong instance, and run 012858's stack destroys it right after
 * a ready that still names 2: every message to the client repeats its
 * instance, so echo the bind's word in the ready too. */
static uint32_t mhi_imsdcm_instance = 2;
/* The source port of the last ask.  Run 004255 (2026-11-25 00:45) had the
 * stack ask under port 0x3d (transaction 9) before it bound that same port
 * (transaction 10) and, after the ready, never ask again: its pre-bind ask
 * was the outstanding request, and the blanket reset at the bind threw it
 * away, so the address was never handed.  A 0x33 SUB_DESTROY_INSTANCE from
 * that port clears the ask again, which keeps the hello-round ask (ask
 * txn 6, destroy txn 7) from counting for the later bind. */
static unsigned mhi_imsdcm_ask_port;
/* The ready has gone out for the current bind.  In both Android captures
 * the daemon sends it 143-164 ms after the bind answer and before the
 * stack asks; the bind branch pauses that long and sends it from there,
 * so no cellular cycle or slow QMI call can hold it back -- run 180808's
 * two-cycle countdown was starved by the SMS submit's ~2-minute block and
 * the ready went out 3m41s late, naming the wrong word besides. */
static bool mhi_imsdcm_ready_sent;
/* Run 154832 answered the stack's ask and handed it the address in the
 * same millisecond, and re-handed the address inside the bind's own burst;
 * the stack never echoed either with IMSA 0x26 and never left state 0.
 * Android's daemon, in imscap2, is paced instead: bind, ready, then the
 * ask -- and the address indication only 0.53 s after the answer, on a
 * later step.  So the address is held (mhi_imsdcm_pending) and delivered by
 * the cellular thread's own cycle, after the stack has bound AND asked,
 * never in the burst that answers the ask. */
static bool mhi_imsdcm_bound, mhi_imsdcm_pending;
/* How many address handoffs went out this boot.  The cellular thread's
 * handoff watch (mhi_sms_milestone) restarts its clock on a change, so a
 * re-indication after a late bind is watched from its own moment, not from
 * the first handoff's. */
static unsigned mhi_imsdcm_handoffs;
/* The stack's handle, carried in the ready indication's 0x01, the 0x20
 * answer's 0x10 and the address indication's 0x01.  It is the word the
 * stack itself declares in its bind's (0x21) TLV 0x01 -- 0x15 in both
 * Android captures (imscap 09:08 and imscap2 17:21, read back from the
 * raw traces), 0x64 in this firmware -- and the daemon must repeat it in
 * every message it addresses to the stack.  Run 191905's bind answer
 * echoed the wrong *instance* besides (0x11 = 2 while the stack bound 1),
 * and the stack destroyed that instance 0.6 s later (0x33 txn 11); every
 * ready and address sent after that reached no live instance.  The
 * default 0x15 stands only until the first bind names the real word. */
static uint8_t mhi_imsdcm_handle = 0x15;

/* A TLV, as the daemon writes them: id, 16-bit length, value. */
static unsigned
mhi_imsdcm_put(uint8_t *answer, unsigned at, unsigned type, const void *value,
    unsigned size)
{
	answer[at] = (uint8_t)type;
	answer[at + 1] = (uint8_t)size;
	answer[at + 2] = (uint8_t)(size >> 8);
	if (size != 0) {
		memcpy(answer + at + 3, value, size);
	}
	return at + 3 + size;
}

/* One message to the modem's IMS client, from the port it wrote to. */
static void
mhi_imsdcm_send(struct mhi_channel *out, unsigned type, unsigned txn,
    unsigned message, const uint8_t *tlvs, unsigned length, const char *what)
{
	uint8_t answer[128], frame[16 + sizeof(answer) + 4];
	size_t framed;

	memset(answer, 0, sizeof(answer));
	answer[0] = (uint8_t)type;		/* response (2) or indication (4) */
	answer[1] = (uint8_t)txn;
	answer[2] = (uint8_t)(txn >> 8);
	answer[3] = (uint8_t)message;
	answer[4] = (uint8_t)(message >> 8);
	answer[5] = (uint8_t)length;
	answer[6] = (uint8_t)(length >> 8);
	if (length != 0) {
		memcpy(answer + 7, tlvs, length);
	}
	/* The whole message, 32 bytes a line: the byte-for-byte comparison
	 * with Android's daemon (imscap 04:12, imscap2 06:17) is made from
	 * these lines. */
	for (size_t dump = 0; dump < 7 + (size_t)length; dump += 32) {
		char hex[3 * 32 + 1];
		size_t n = 7 + (size_t)length - dump > 32 ? 32 :
		    7 + (size_t)length - dump;

		for (size_t i = 0; i < n; i++) {
			snprintf(hex + i * 3, 4, " %02x", answer[dump + i]);
		}
		hex[n * 3] = 0;
		hakios_tl("pcie2: AMSS: %s: the message is %u bytes; at %zu:%s\n",
		    what, 7 + length, dump, hex);
	}
	/* Padded to four bytes like every QMI frame this driver sends: the
	 * modem's qrtr-mhi will not transmit any other length, and these
	 * answers went out unpadded until run 053422 -- whose log shows the
	 * stack re-sending the same transaction (0x20 txn 7 and 9, 0x33 txn
	 * 8) a hundred-odd times each, because every answer and the address
	 * indication of 93 bytes were dropped before they reached it. */
	framed = mhi_qrtr_put3_sized(frame, 1, mhi_imsdcm_from, mhi_imsdcm_node,
	    mhi_imsdcm_port, answer, 7 + length, (16u + 7u + length + 3u) & ~3u);
	hakios_tl("pcie2: AMSS: %s: the IMS data service (port %#x) sends node %u port %#x message %#x type %u, %u bytes: %s\n",
	    what, mhi_imsdcm_from, mhi_imsdcm_node, mhi_imsdcm_port, message,
	    type, 7 + length,
	    mhi_send(out, frame, framed) == 0 ? "sent" : "not sent");
}

/* The indication that carries the address (Android's last message):
 * TLV 0x10 repeats the profile the stack's own 0x20 request named (0x64 in
 * imscap 04:12, 0x65 in imscap2 06:17; the ready indication carries the
 * other half, mhi_imsdcm_ready), and TLV 0x11 is {the family word, 0, 0, 0,
 * the length, the text}, the text in inet_ntop's form -- "240a:42e6:810:...",
 * no leading zeros.  The family word follows the address actually handed:
 * 1 for the IPv6 text of Android's capture (whose ask named family 1), 0 for
 * an IPv4 one.  This firmware's stack asks family 0 (runs 140914, 222633,
 * 231129) while every PDN grant so far came back IPv6 and the indication
 * said 1 -- a word the stack may check against its ask.  False while the IMS
 * PDN has given no address, the stack has not bound yet, it has not asked,
 * or the ready has not gone out: Android's daemon sends the ready first, the
 * ask next, this last, each on its own step. */
static bool
mhi_imsdcm_indicate(struct mhi_channel *out, const char *what)
{
	uint8_t tlvs[96], value[5 + sizeof(mhi_imsdcm_address)];
	uint32_t zero = 0;
	uint32_t nobind = 0;
	size_t name;
	unsigned at = 0;

	/* pcie2_imsdcm_nobind: run 205643's stack asked the daemon (0x20)
	 * without ever sending its bind (0x21), so the bound/ready gates
	 * withheld the address for the whole boot and the vendor state never
	 * climbed to 4.  With this flag an ask alone is enough; the ready is
	 * sent just before the address under the defaults Android's stack
	 * uses (handle 0x15, instance 2). */
	PE_parse_boot_argn("pcie2_imsdcm_nobind", &nobind, sizeof(nobind));
	if (mhi_imsdcm_address[0] == 0 || mhi_imsdcm_port == 0 ||
	    !mhi_imsdcm_asked ||
	    (!mhi_imsdcm_bound && nobind == 0) ||
	    (!mhi_imsdcm_ready_sent && nobind == 0)) {
		return false;
	}
	at = mhi_imsdcm_put(tlvs, at, 0x02, &zero, 4);
	at = mhi_imsdcm_put(tlvs, at, 0x01, &mhi_imsdcm_handle, 1);
	at = mhi_imsdcm_put(tlvs, at, 0x10, &mhi_imsdcm_profile, 4);
	name = strlen(mhi_imsdcm_address);
	/* pcie2_imsdcm_ask_family: hand the address under the family the
	 * stack's own ask named (mhi_imsdcm_ask11, 0 or 1) instead of the
	 * address's own.  The experiment that tells whether the word itself,
	 * rather than the address it describes, is what the stack checks. */
	{
		uint32_t ask_family_on = 0;

		PE_parse_boot_argn("pcie2_imsdcm_ask_family", &ask_family_on,
		    sizeof(ask_family_on));
		value[0] = ask_family_on != 0 && mhi_imsdcm_ask11 <= 1 ?
		    (uint8_t)mhi_imsdcm_ask11 :
		    (strchr(mhi_imsdcm_address, ':') != NULL ? 1 : 0);
	}
	value[1] = 0;
	value[2] = 0;
	value[3] = 0;
	value[4] = (uint8_t)name;
	memcpy(value + 5, mhi_imsdcm_address, name);
	at = mhi_imsdcm_put(tlvs, at, 0x11, value, 5 + (unsigned)name);
	at = mhi_imsdcm_put(tlvs, at, 0x12, &mhi_imsdcm_ask12, 4);
	hakios_tl("pcie2: AMSS: %s: the IMS data service hands the stack the address %s (family word %u, handle %#x, profile %u, %u characters; the ask's 0x11 word %u)\n",
	    what, mhi_imsdcm_address, (unsigned)value[0], mhi_imsdcm_handle,
	    (unsigned)mhi_imsdcm_profile, (unsigned)name, mhi_imsdcm_ask11);
	mhi_imsdcm_send(out, 4, mhi_imsdcm_txn++, 0x0020, tlvs, at, what);
	mhi_imsdcm_indicated = true;
	mhi_imsdcm_handoffs++;
	return true;
}

/* Android's ready indication: after the answer to the bind, before the
 * stack asks for the APN, the daemon tells it profile 0x64 is up.  The
 * 0x10 word is fixed at 0x64 in both captures; the 0x01 byte is the
 * stack's bound word echoed back (see mhi_imsdcm_handle).  The stack's
 * ask 10.4 s later is the one that names the IMS
 * profile (0x65 in the captures).  Run 185152 showed the ready's 0x10
 * does not set the ask's word (ready 0x64, on time, ask still 0x64/V4).
 * The 0x02 TLV on the wire is 00 00 0d 00 -- result 0 and error 13 -- so
 * as a little-endian word that is 0x000d0000; 0x00000d00, which stood
 * here, put the 13 in the result half and said 0x0d00 to the stack. */
static void
mhi_imsdcm_ready(struct mhi_channel *out, const char *what)
{
	uint8_t tlvs[32];
	uint32_t ready = 0x000d0000;
	uint32_t profile = 0x64;
	unsigned at = 0;

	at = mhi_imsdcm_put(tlvs, at, 0x02, &ready, 4);
	at = mhi_imsdcm_put(tlvs, at, 0x01, &mhi_imsdcm_handle, 1);
	at = mhi_imsdcm_put(tlvs, at, 0x10, &profile, 4);
	at = mhi_imsdcm_put(tlvs, at, 0x12, &mhi_imsdcm_instance, 4);
	hakios_tl("pcie2: AMSS: %s: the ready indication announces profile %u (0x%x), the word every Android capture carries; the stack's ask follows with 0x65\n",
	    what, (unsigned)profile, (unsigned)profile);
	mhi_imsdcm_send(out, 4, mhi_imsdcm_txn++, 0x0020, tlvs, at, what);
	mhi_imsdcm_ready_sent = true;
}

/* One request from the modem's IMS stack, answered where it came from and
 * from the service port it addressed. */
static void
mhi_imsdcm_answer(struct mhi_channel *out, const uint8_t *frame, size_t size,
    unsigned header, unsigned from, const char *what)
{
	const uint8_t *qmi = frame + header;
	uint8_t tlvs[32];
	uint8_t result[4] = { 0, 0, 0, 0 }, fa = 0xfa;
	uint32_t profile = 100;
	unsigned message, txn, node, port, at = 0;

	if (size < header + 7) {
		return;
	}
	if (header == 32) {
		node = mhi_qrtr_word(frame, 8);
		port = mhi_qrtr_word(frame, 12);
	} else {
		node = frame[8] | (unsigned)frame[9] << 8;
		port = frame[10] | (unsigned)frame[11] << 8;
	}
	message = qmi[3] | (unsigned)qmi[4] << 8;
	txn = qmi[1] | (unsigned)qmi[2] << 8;
	mhi_imsdcm_node = node;
	mhi_imsdcm_port = port;
	mhi_imsdcm_from = from;
	hakios_tl("pcie2: AMSS: %s: the modem's IMS stack asks the IMS data service message %#x (transaction %u) from node %u port %#x\n",
	    what, message, txn, node, port);
	/* The whole request, so a message this service does not answer yet
	 * (0x23, 0x2e, 0x33, 0x34 have arrived) can be read out of the log.
	 * Thirty-two bytes a line: the console cuts a line at about 240
	 * characters, and the 48-byte lines of run 131620 lost the APN
	 * request's profile TLV beyond byte 27. */
	if (size > header + 7) {
		size_t shown = size - header;
		size_t n;

		if (shown > 128) {
			shown = 128;
		}
		for (size_t dump = 0; dump < shown; dump += 32) {
			char hex[3 * 32 + 1];

			n = shown - dump > 32 ? 32 : shown - dump;
			for (size_t i = 0; i < n; i++) {
				snprintf(hex + i * 3, 4, " %02x", qmi[dump + i]);
			}
			hex[n * 3] = 0;
			hakios_tl("pcie2: AMSS: %s: the request is %zu bytes; at %zu:%s\n",
			    what, size - header, dump, hex);
		}
	}
	if (from != 0x3005) {
		/* The daemon's second registration, service 0x28, carries a
		 * different protocol -- its 0x20 is TLV 0x01[4], 0x11[4],
		 * 0x12[4] counter values, not IMSDCM's APN string and profile.
		 * It is not the service the IMS stack's 0x21/0x20 exchange
		 * comes to (that one arrives on 0x3005 and matches the
		 * daemon's IMSDCM IDL), so it is answered the one way every
		 * QMI client can read: result 0, and nothing claimed. */
		at = mhi_imsdcm_put(tlvs, at, 0x02, result, sizeof(result));
		mhi_imsdcm_send(out, 2, txn, message, tlvs, at, what);
		hakios_tl("pcie2: AMSS: %s: the 0x28 service is asked message %#x; answered result 0\n",
		    what, message);
		return;
	}
	if (message == 0x0021) {
		/* The bind names two words.  TLV 0x01 is the stack's handle
		 * for this exchange -- every indication and answer must carry
		 * it back (see mhi_imsdcm_handle) -- and TLV 0x10 the instance
		 * it binds; Android's stack binds {0x15, 2}, and the daemon's
		 * answer echoes the instance in its 0x11.  This firmware binds
		 * {0x64, 1}: run 191905's answer still carried the fixed 2 the
		 * capture shows, the stack destroyed instance 1 within 0.6 s
		 * (0x33 txn 11), and neither the ready nor the address was
		 * used after that.  Echo both words. */
		unsigned stack_word = ~0u;
		uint32_t stack_instance = ~0u;

		for (unsigned eat = header + 7; eat + 3 <= (unsigned)size; ) {
			unsigned etype = frame[eat];
			unsigned elen = frame[eat + 1] |
			    (unsigned)frame[eat + 2] << 8;

			if (eat + 3 + elen > size) {
				break;
			}
			if (etype == 0x01 && elen >= 1) {
				stack_word = frame[eat + 3];
			} else if (etype == 0x10 && elen >= 4) {
				stack_instance = (uint32_t)frame[eat + 3] |
				    (uint32_t)frame[eat + 4] << 8 |
				    (uint32_t)frame[eat + 5] << 16 |
				    (uint32_t)frame[eat + 6] << 24;
			}
			eat += 3 + elen;
		}
		if (stack_word != ~0u) {
			mhi_imsdcm_handle = (uint8_t)stack_word;
		}
		if (stack_instance == ~0u) {
			stack_instance = 2;
		}
		mhi_imsdcm_instance = stack_instance;
		hakios_tl("pcie2: AMSS: %s: the stack binds with the word %#x and instance %u; the answer echoes the instance, the ready and the address carry the word\n",
		    what, stack_word, stack_instance);
		at = mhi_imsdcm_put(tlvs, at, 0x02, result, sizeof(result));
		at = mhi_imsdcm_put(tlvs, at, 0x10, &fa, 1);
		at = mhi_imsdcm_put(tlvs, at, 0x11, &stack_instance, 4);
		mhi_imsdcm_send(out, 2, txn, 0x0021, tlvs, at, what);
		mhi_imsdcm_bound = true;
		/* Android's order is ready -> ask -> answer -> address.  An ask
		 * the stack sent before binding counts for this bound instance
		 * only when pcie2_imsdcm_keep_ask is set, it came from the very
		 * port that binds, and no SUB_DESTROY_INSTANCE followed it (run
		 * 004255: ask txn 9, bind txn 10, then silence); the hello-round
		 * ask is followed by its destroy (txn 6, txn 7) and is cleared
		 * there.  Otherwise, as before, the address goes out only after
		 * the stack asks again under the new bind. */
		{
			uint32_t keep_ask = 0;

			PE_parse_boot_argn("pcie2_imsdcm_keep_ask", &keep_ask,
			    sizeof(keep_ask));
			if (keep_ask == 0 || mhi_imsdcm_ask_port != port) {
				mhi_imsdcm_asked = false;
			}
		}
		/* The ready follows the bind answer 143-164 ms later in the
		 * captures, not inside its burst (run 163801 sent it there
		 * and the modem's IMS service never acknowledged it).  The
		 * pause is taken here, on this thread: the countdown that
		 * used to run on the cellular cycle was starved in run
		 * 180808 by a slow QMI call in the same milestone step and
		 * the ready went out 3m41s late.  Android's daemon pauses
		 * in its receive path too, and the MHI rings hold the
		 * modem's frames for the 150 ms. */
		mhi_imsdcm_ready_sent = false;
		delay(150000);
		mhi_imsdcm_ready(out, what);
		/* The bind only answers and readies the client.  Runs 140914 and
		 * 154832 handed the address here too, inside the bind's burst
		 * and under the new handle; the stack then sat at state 0 and
		 * destroyed its instance.  Android's daemon waits for the
		 * stack's own ask and sends the address as a later step, so
		 * let the cellular thread's cycle deliver it.  The one
		 * exception is the kept pre-bind ask (pcie2_imsdcm_keep_ask):
		 * that stack asked before it bound and will not ask again (run
		 * 004255), so the address goes out here, 530 ms after the
		 * ready -- the pace the ask path uses -- not in the bind's own
		 * burst. */
		if (mhi_imsdcm_asked && mhi_imsdcm_address[0] != 0 &&
		    !mhi_imsdcm_indicated) {
			uint32_t keep_ask = 0;

			PE_parse_boot_argn("pcie2_imsdcm_keep_ask", &keep_ask,
			    sizeof(keep_ask));
			if (keep_ask != 0 && mhi_imsdcm_ask_port == port) {
				delay(530000);
				(void)mhi_imsdcm_indicate(out, what);
				mhi_imsdcm_pending = false;
			} else {
				mhi_imsdcm_pending = true;
				hakios_tl("pcie2: AMSS: %s: the address waits for the cellular thread's next step (bound, asked, not yet handed)\n",
				    what);
			}
		}
	} else if (message == 0x0020) {
		/* Android's answer repeats the request's TLV 0x10 as its own
		 * 0x11: its capture has the stack asking 0x65 and the daemon
		 * answering 0x65, and the address indication that follows
		 * carries 0x65 too.  Echo whatever arrives, and remember it
		 * for the indication.
		 *
		 * The other words of the ask are read out too.  The daemon's
		 * PDP_ACTIVATE handler (imsdatadaemon, ims_qmi_dcm_pdp_
		 * activate_handler.cfi at 0x216b4) decodes them into a struct
		 * whose " IP Address Family : %d" field at +0x70 it validates
		 * to 0 or 1 (0 = V4, 1 = V6), and its LINK_ADDR logger names
		 * the pair outright: "family_type[%d]. 0 is V4, 1 is V6".
		 * Of the ask's three 4-byte words only one can be that family
		 * -- Android's working ask carries 1, 2, 2 (imscap2 06:17),
		 * and a 2 would fail the check -- so 0x11 is the family.
		 * Run 140914's ask carried 0, 1, 1: the stack asked for an
		 * IPv4 PDP while the only "ims" leg this network grants is
		 * IPv6.  The daemon's mismatch path keeps the actual family
		 * in the indication, which mhi_imsdcm_indicate already does
		 * (TLV 0x11 = 1, IPv6); the words are logged so the next ask
		 * says whether the stack keeps asking V4. */
		{
			unsigned t11 = ~0u, t12 = ~0u, t13 = ~0u;

			for (unsigned eat = header + 7; eat + 3 <= (unsigned)size; ) {
				unsigned etype = frame[eat];
				unsigned elen = frame[eat + 1] |
				    (unsigned)frame[eat + 2] << 8;
				uint32_t word;

				if (eat + 3 + elen > size) {
					break;
				}
				word = elen >= 4 ?
				    ((uint32_t)frame[eat + 3] |
				    (uint32_t)frame[eat + 4] << 8 |
				    (uint32_t)frame[eat + 5] << 16 |
				    (uint32_t)frame[eat + 6] << 24) : ~0u;
				if (etype == 0x10 && elen >= 4) {
					profile = word;
				} else if (etype == 0x11 && elen >= 4) {
					t11 = word;
				} else if (etype == 0x12 && elen >= 4) {
					t12 = word;
				} else if (etype == 0x13 && elen >= 4) {
					t13 = word;
				}
				eat += 3 + elen;
			}
			hakios_tl("pcie2: AMSS: %s: the ask's words: 0x11 %u (the family: 0 V4, 1 V6), 0x12 %u, 0x13 %u; Android's working ask has 1, 2, 2\n",
			    what, t11, t12, t13);
			mhi_imsdcm_ask11 = t11;
			if (t12 != ~0u) {
				mhi_imsdcm_ask12 = t12;
			}
			mhi_imsdcm_ask_port = port;
		}
		mhi_imsdcm_profile = profile;
		mhi_imsdcm_asked = true;
		mhi_imsdcm_indicated = false;
		/* Run 205643: this stack's first words to the service were 0x33
		 * (destroy instance) and 0x20 (the ask) -- the bind Android's
		 * stack sends first never came, so the ready never went out and
		 * the address never followed.  With pcie2_imsdcm_nobind the ask
		 * is treated as the bind for delivery's sake: the ready goes out
		 * now, the address after the same 530 ms pause the ask path
		 * uses. */
		{
			uint32_t nobind = 0;

			PE_parse_boot_argn("pcie2_imsdcm_nobind", &nobind,
			    sizeof(nobind));
			if (nobind != 0 && !mhi_imsdcm_bound &&
			    !mhi_imsdcm_ready_sent) {
				hakios_tl("pcie2: AMSS: %s: the stack asked without binding (no 0x21 this boot); the ready goes out anyway, then the address (pcie2_imsdcm_nobind=1)\n",
				    what);
				mhi_imsdcm_ready(out, what);
			}
		}
		at = mhi_imsdcm_put(tlvs, at, 0x02, result, sizeof(result));
		at = mhi_imsdcm_put(tlvs, at, 0x10, &mhi_imsdcm_handle, 1);
		at = mhi_imsdcm_put(tlvs, at, 0x11, &profile, 4);
		at = mhi_imsdcm_put(tlvs, at, 0x12, &mhi_imsdcm_ask12, 4);
		mhi_imsdcm_send(out, 2, txn, 0x0020, tlvs, at, what);
		/* The address is a separate step, not part of this answer's
		 * burst: Android's daemon sends it 0.53 s later, after its own
		 * PDN work (imscap2 06:17:32.03 -> .56); run 154832's
		 * same-millisecond handoff never reached the stack.  It goes
		 * out from here, paced, when the IMS PDN already has an
		 * address -- waiting for a cellular cycle let a slow QMI call
		 * in the milestone hold the handshake in run 180808.  Without
		 * an address yet, the cellular thread delivers it once the
		 * PDN brings one (mhi_imsdcm_pending). */
		if (!mhi_imsdcm_indicated && mhi_imsdcm_address[0] != 0) {
			delay(530000);
			(void)mhi_imsdcm_indicate(out, what);
			mhi_imsdcm_pending = false;
		} else {
			mhi_imsdcm_pending = true;
			hakios_tl("pcie2: AMSS: %s: the address is held for the cellular thread's next step (asked%s)\n",
			    what, mhi_imsdcm_bound ? ", bound" : ", not bound yet");
		}
	} else if (message == 0x0022 || message == 0x0023 ||
	    message == 0x002e || message == 0x0033 || message == 0x0034) {
		/* The daemon's own answer (imsdatadaemon, its IDL recovered from
		 * the binary): GET_IP_ADDRESS 0x22, LINK_ADDR 0x23,
		 * REGISTER_APP_STATE 0x2e, SUB_DESTROY_INSTANCE 0x33 and
		 * SERVICE_ENABLE_STATUS 0x34 are each answered result 0 with no
		 * other TLV (each handler zeroes an 8- or 16-byte response
		 * struct and sends it).  0x2e is the one that sets
		 * vendor.ims.ENABLE_HELPER on Android; here the plain answer is
		 * what the stack waits for.  Run 052027 had 0x23, 0x2e, 0x33
		 * and 0x34 arrive and heard nothing back. */
		at = mhi_imsdcm_put(tlvs, at, 0x02, result, sizeof(result));
		mhi_imsdcm_send(out, 2, txn, message, tlvs, at, what);
		if (message == 0x0033 && port == mhi_imsdcm_ask_port) {
			/* The instance that asked is gone; a later bind from the
			 * same port must ask again before the address goes out
			 * (the hello-round ask txn 6 / destroy txn 7 pair of run
			 * 004255). */
			mhi_imsdcm_asked = false;
		}
		hakios_tl("pcie2: AMSS: %s: the IMS data service answers %s with success\n",
		    what, message == 0x0022 ? "GET_IP_ADDRESS" :
		    message == 0x0023 ? "LINK_ADDR" :
		    message == 0x002e ? "REGISTER_APP_STATE" :
		    message == 0x0033 ? "SUB_DESTROY_INSTANCE" :
		    "SERVICE_ENABLE_STATUS");
	} else {
		hakios_tl("pcie2: AMSS: %s: the IMS data service is asked message %#x; not answered\n",
		    what, message);
	}
}

/* ---- TFTP (QMI service 4096): stock's tqftpserv, replayed ---- */
/*
 * What stock's tqftpserv gives the modem, by phone #1's evidence
 * (docs/ru/cellular.md): the modem writes /readwrite/server_check.txt, then
 * reads its image's files from /readonly/firmware/image/... in windows
 * (options blksize, wsize, tsize, seek and rsize), each transfer on a QRTR
 * port of its own, as TFTP wants.  The files here come from the run's root,
 * /redwood/fw/image/<name> -- a copy of the phone's /vendor/firmware_mnt/image
 * that the scene builder puts there.  A name outside the firmware prefixes,
 * or a file not there, is answered "not found" and logged; what the modem
 * writes is kept in memory for the run and read back from there.
 */
#define MHI_TFTP_BLOCK_MAX	8000u		/* as much as the modem asks for */
#define MHI_TFTP_WINDOW_MAX	4u
#define MHI_TFTP_WRITE_MAX	(256u * 1024u)
#define MHI_TFTP_KEEP_MAX	(128u * 1024u)
#define MHI_TFTP_KEEP_SLOTS	8u
/* Run 123149 shows the modem asks for one file twice (its client retries when
 * the answer is slow; 229 reads served it 60 configs), and one copy is then
 * left to the modem's EOT error.  Run 125953 had the card and EFS work keeping
 * the pump busy around those reads, the leftovers stayed in their slots, and
 * four ran out: the next read of mbn_sw.txt was answered "busy", after which
 * the modem read no carrier configuration at all.  Eight slots leave room for
 * the copies a transfer in flight casts, and the request path below never
 * refuses a read for want of a slot anyway. */
#define MHI_TFTP_TRANSFERS	8u
#define MHI_TFTP_PORT(i)	(0x3100u + (i))
#define MHI_TFTP_RRQ		1
#define MHI_TFTP_WRQ		2
#define MHI_TFTP_DATA		3
#define MHI_TFTP_ACK		4
#define MHI_TFTP_ERROR_OP	5
#define MHI_TFTP_OACK		6
#define MHI_TFTP_ENOENT		1
#define MHI_TFTP_EBADOP		4
#define MHI_TFTP_EOT		9

struct mhi_tftp_transfer {
	bool used, writing;
	unsigned node, peer;
	vnode_t vnode;				/* a read: the file, a window at a time */
	const uint8_t *memory;			/* ... or what the modem wrote */
	uint64_t size;
	uint32_t blksize, wsize, rsize, seek, sent;
	uint32_t written_size, expected;
	uint8_t written[MHI_TFTP_WRITE_MAX];
	char name[96];
};
static struct mhi_tftp_transfer mhi_tftp_transfers[MHI_TFTP_TRANSFERS];

/* What the modem writes, kept for the run so it can read it back.  Run
 * 121008 showed how many names that is: as the modem worked through its
 * carrier table it wrote seven distinct files under /readwrite --
 * server_check.txt, mcfg.tmp and five processed configs named
 * mcfg_sw<HASH> -- and the old four-slot store answered the last three
 * "NOWHERE", so a config the modem had just written could not be read
 * back.  Eight slots cover those seven with one to spare; when they run
 * out the oldest is replaced, because the file written a moment ago is
 * the one the modem comes back for.  A write of no bytes is the modem's
 * way of asking whether the name takes writes at all (it sends one
 * before each real config write): it stores nothing and takes no slot. */
static struct {
	char name[96];
	uint32_t size;
	uint32_t stamp;
	uint8_t data[MHI_TFTP_KEEP_MAX];
} mhi_tftp_kept[MHI_TFTP_KEEP_SLOTS];
static uint32_t mhi_tftp_kept_clock;

/* Set when the modem ACKs the last block of mcfg_sw/mbn_sw.dig.  The
 * carrier-config load arms for about two seconds after that ACK and is
 * started by a Set Operating Mode ONLINE (0x2e, mode 0) request: every
 * run that scanned sent one within 1.8 s of the ACK (121008 at +1.75 s,
 * 123149 at +1.36 s, 133235 with the request already in flight at
 * +0.00 s) and the modem read oem_sw_j11 in the same breath; the run
 * that asked 3.1 s late (164316) was never read at all.  The block
 * before the card waits on this flag in quarter-second pump slices. */
static int mhi_mcfg_dig_done;

static void
mhi_tftp_error(struct mhi_channel *out, unsigned port, unsigned node, unsigned peer,
    unsigned code, const char *text)
{
	uint8_t packet[64], frame[16 + sizeof(packet) + 4];
	size_t n = strlen(text) + 1;
	size_t length;

	if (n > sizeof(packet) - 4) {
		n = sizeof(packet) - 4;
	}
	packet[0] = 0;
	packet[1] = MHI_TFTP_ERROR_OP;
	packet[2] = (uint8_t)(code >> 8);
	packet[3] = (uint8_t)code;
	memcpy(packet + 4, text, n);
	packet[4 + n - 1] = 0;
	length = mhi_qrtr_put3_frame(frame, 1, port, node, peer, packet, 4 + (unsigned)n);
	(void)mhi_send(out, frame, length);
}

static void
mhi_tftp_end(struct mhi_tftp_transfer *t)
{
	if (t->vnode != NULLVP) {
		vnode_put(t->vnode);
		t->vnode = NULLVP;
	}
	t->used = false;
	t->memory = NULL;
}

/* Bytes n at offset, read now: from the scene root's file, or from the copy
 * of what the modem wrote. */
static int
mhi_tftp_read_at(struct mhi_tftp_transfer *t, uint64_t offset, uint8_t *dest, unsigned n,
    unsigned *got)
{
	vfs_context_t context;
	int left = 0;
	int error;

	*got = 0;
	if (t->memory != NULL) {
		memcpy(dest, t->memory + offset, n);
		*got = n;
		return 0;
	}
	context = vfs_context_kernel();
	error = vn_rdwr(UIO_READ, t->vnode, (caddr_t)dest, (int)n, (off_t)offset, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	if (error != 0) {
		return error;
	}
	if (left != 0) {
		return EIO;
	}
	*got = n;
	return 0;
}

/* The file a /readonly/... name asks for, as a path under the firmware
 * image trees, or NULL.  The prefixes are the ones phone #1's clients use. */
static const char *
mhi_tftp_relative(const char *path)
{
	static const char *const prefixes[] = {
		"/readonly/firmware/image/",
		"/readonly/vendor/firmware_mnt/image/",
		"/readonly/vendor/firmware/",
		"/readonly/firmware/",
	};

	for (const char *at = path; at[0] != 0 && at[1] != 0; at++) {
		if (at[0] == '.' && at[1] == '.') {
			return NULL;
		}
	}
	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);

		if (strncmp(path, prefixes[i], n) == 0) {
			return path + n;
		}
	}
	return NULL;
}

static int
mhi_tftp_open(const char *name, vnode_t *vnode, uint64_t *size)
{
	/* Two homes: the root volume's /redwood/fw/image (a scene can carry a
	 * tree of its own) and the system image's /fw/image, beside /fw/modem
	 * where the Sahara set lives -- the system image is the resident RAM
	 * disk, so the 40 MB carrier configuration goes there: kernel plus root
	 * scene share the loader's 63 MiB payload window, and the scene alone
	 * takes 48.  /fw/modem is read the same way and serves the AMSS. */
	static const char *const roots[] = { "/redwood/fw/image", "/fw/image" };
	char path[192];
	const char *relative;
	vfs_context_t context = vfs_context_kernel();
	struct vnode_attr attributes;
	int error = ENOENT;

	*vnode = NULLVP;
	*size = 0;
	relative = mhi_tftp_relative(name);
	if (relative == NULL || strlen(relative) > sizeof(path) - 32) {
		return ENOENT;
	}
	for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", roots[i], relative);
		error = vnode_lookup(path, 0, vnode, context);
		if (error == 0) {
			break;
		}
	}
	if (error != 0) {
		return error;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	error = vnode_getattr(*vnode, &attributes, context);
	if (error != 0 || attributes.va_data_size == 0) {
		vnode_put(*vnode);
		*vnode = NULLVP;
		return error != 0 ? error : ENOENT;
	}
	*size = (uint64_t)attributes.va_data_size;
	return 0;
}

/* Blocks from+1 onward, a window of them: TFTP's blocks are numbered from 1. */
static void
mhi_tftp_window(struct mhi_channel *out, struct mhi_tftp_transfer *t, uint32_t from)
{
	static uint8_t packet[4 + MHI_TFTP_BLOCK_MAX];
	static uint8_t frame[16 + 4 + MHI_TFTP_BLOCK_MAX + 4];
	unsigned slot = (unsigned)(t - mhi_tftp_transfers);

	for (uint32_t w = 0; w < t->wsize; w++) {
		uint32_t block = from + 1 + w;
		uint64_t offset = (uint64_t)t->seek + (uint64_t)(block - 1) * t->blksize;
		uint32_t n = 0;
		unsigned got = 0;
		size_t length;

		if (t->rsize != 0 && (uint64_t)(block - 1) * t->blksize >= t->rsize) {
			break;			/* (as much as was asked for has gone) */
		}
		if (offset < t->size) {
			uint64_t left = t->size - offset;

			n = (uint32_t)(left < t->blksize ? left : t->blksize);
		}
		if (t->rsize != 0 && (uint64_t)block * t->blksize > t->rsize) {
			uint64_t left = t->rsize - (uint64_t)(block - 1) * t->blksize;

			if (left < n) {
				n = (uint32_t)left;	/* (no more than was asked for) */
			}
		}
		if (n != 0 && (mhi_tftp_read_at(t, offset, packet + 4, n, &got) != 0 || got != n)) {
			hakios_tl("pcie2: AMSS: tftp: \"%s\": block %u: the file could not be read\n",
			    t->name, block);
			mhi_tftp_error(out, MHI_TFTP_PORT(slot), t->node, t->peer, 0, "read failed");
			mhi_tftp_end(t);
			return;
		}
		packet[0] = 0;
		packet[1] = MHI_TFTP_DATA;
		packet[2] = (uint8_t)(block >> 8);
		packet[3] = (uint8_t)block;
		length = mhi_qrtr_put3_frame(frame, 1, MHI_TFTP_PORT(slot), t->node, t->peer, packet,
		    4 + n);
		if (mhi_send(out, frame, length) != 0) {
			hakios_tl("pcie2: AMSS: tftp: \"%s\": block %u: not sent\n", t->name, block);
			break;
		}
		t->sent = block;
		if (n < t->blksize) {
			break;			/* the last block */
		}
	}
}

/* One TFTP request (a read or a write) to our service port, answered where
 * it came from.  The frame's header is 16 bytes (version 3, what the modem
 * itself speaks) or 32 (version 1). */
static void
mhi_tftp_request(struct mhi_channel *out, const uint8_t *frame, size_t size, unsigned header,
    const char *what)
{
	const uint8_t *m = frame + header, *end;
	const char *name, *mode, *p;
	struct mhi_tftp_transfer *t = NULL;
	uint32_t blksize = 512, wsize = 1, rsize = 0, seek = 0;
	bool tsize = false, options = false, writing;
	uint8_t oack[128];
	unsigned node, port, service_port, at, slot;
	uint64_t file_size = 0;
	vnode_t vnode = NULLVP;
	const uint8_t *memory = NULL;
	size_t name_length;
	uint16_t opcode;

	size = mhi_qrtr_payload(frame, size, header);
	if (size < header + 4) {
		return;
	}
	end = m + (size - header);
	opcode = (uint16_t)(m[0] << 8 | m[1]);
	if (header == 32) {
		node = mhi_qrtr_word(frame, 8);
		port = mhi_qrtr_word(frame, 12);
		service_port = mhi_qrtr_word(frame, 28);
	} else {
		node = frame[8] | (unsigned)frame[9] << 8;
		port = frame[10] | (unsigned)frame[11] << 8;
		service_port = frame[14] | (unsigned)frame[15] << 8;
	}
	/* An error answers from the port the request was sent to -- the
	 * service's published port, v1 or v3, as tqftpserv does. */
	writing = opcode == MHI_TFTP_WRQ;
	if (opcode != MHI_TFTP_RRQ && !writing) {
		hakios_tl("pcie2: AMSS: %s: tftp: the modem's request %u: not a read nor a write\n",
		    what, opcode);
		mhi_tftp_error(out, service_port, node, port, MHI_TFTP_EBADOP, "not a read nor a write");
		return;
	}
	name = (const char *)m + 2;
	p = name;
	while (p < (const char *)end && *p != 0) {
		p++;
	}
	if (p >= (const char *)end) {
		return;
	}
	name_length = (size_t)(p - name);
	mode = p + 1;
	p = mode;
	while (p < (const char *)end && *p != 0) {
		p++;
	}
	if (p >= (const char *)end) {
		return;
	}
	p++;
	/* the options: name, value, name, value..., each ended by a zero */
	while (p < (const char *)end) {
		const char *option = p, *value;

		while (p < (const char *)end && *p != 0) {
			p++;
		}
		if (p >= (const char *)end) {
			break;
		}
		value = ++p;
		while (p < (const char *)end && *p != 0) {
			p++;
		}
		if (p >= (const char *)end) {
			break;
		}
		p++;
		options = true;
		if (strcmp(option, "blksize") == 0) {
			blksize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "wsize") == 0) {
			wsize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "rsize") == 0) {
			rsize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "seek") == 0) {
			seek = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "tsize") == 0) {
			tsize = true;
		}
	}
	(void)mode;
	if (blksize < 8) {
		blksize = 8;
	}
	if (blksize > MHI_TFTP_BLOCK_MAX) {
		blksize = MHI_TFTP_BLOCK_MAX;
	}
	if (wsize < 1) {
		wsize = 1;
	}
	if (wsize > MHI_TFTP_WINDOW_MAX) {
		wsize = MHI_TFTP_WINDOW_MAX;
	}
	if (name_length >= sizeof(mhi_tftp_transfers[0].name)) {
		mhi_tftp_error(out, service_port, node, port, MHI_TFTP_ENOENT, "name too long");
		return;
	}
	/* A repeat of a request that is still in a slot (the modem's client
	 * retries when the first answer is slow): same address, same file, same
	 * direction -- the same transfer, restarted and answered from its own
	 * slot, never a second one.  Run 125953: the copies took slots of their
	 * own, four ran out, and the "busy" answer stopped the carrier-config
	 * scan. */
	for (unsigned i = 0; i < MHI_TFTP_TRANSFERS; i++) {
		struct mhi_tftp_transfer *o = &mhi_tftp_transfers[i];

		if (o->used && o->node == node && o->peer == port &&
		    o->writing == writing && strcmp(o->name, name) == 0) {
			t = o;
			hakios_tl("pcie2: AMSS: %s: tftp: the modem %s \"%s\" again; the "
			    "same transfer answers\n", what, writing ? "writes" : "reads", name);
			break;
		}
	}
	for (unsigned i = 0; i < MHI_TFTP_TRANSFERS && t == NULL; i++) {
		if (!mhi_tftp_transfers[i].used) {
			t = &mhi_tftp_transfers[i];
		}
	}
	if (!writing) {
		unsigned f;

		/* a read: what the modem wrote under that name, else the image's file */
		for (f = 0; f < sizeof(mhi_tftp_kept) / sizeof(mhi_tftp_kept[0]); f++) {
			if (mhi_tftp_kept[f].name[0] != 0 &&
			    strcmp(mhi_tftp_kept[f].name, name) == 0) {
				memory = mhi_tftp_kept[f].data;
				file_size = mhi_tftp_kept[f].size;
				break;
			}
		}
		if (memory == NULL && mhi_tftp_open(name, &vnode, &file_size) != 0) {
			hakios_tl("pcie2: AMSS: %s: tftp: the modem reads \"%s\": NOT HERE "
			    "(neither /redwood/fw/image nor /fw/image has it)\n",
			    what, name);
			mhi_tftp_error(out, service_port, node, port, MHI_TFTP_ENOENT, "file not found");
			return;
		}
	}
	if (t == NULL) {
		struct mhi_tftp_transfer *victim = NULL;

		/* Every slot taken and the modem is waiting anyway: give it the
		 * read that has moved least -- in practice an abandoned copy of an
		 * earlier request, which the modem itself ends with an EOT error
		 * whenever it remembers it.  A write is never made way for; its
		 * bytes are the only copy until the last block lands. */
		for (unsigned i = 0; i < MHI_TFTP_TRANSFERS; i++) {
			struct mhi_tftp_transfer *o = &mhi_tftp_transfers[i];

			if (o->used && !o->writing &&
			    (victim == NULL || o->sent < victim->sent)) {
				victim = o;
			}
		}
		if (victim != NULL) {
			hakios_tl("pcie2: AMSS: %s: tftp: the modem %s \"%s\": every transfer "
			    "is out; \"%s\" (%u blocks in) makes way\n", what,
			    writing ? "writes" : "reads", name, victim->name, victim->sent);
			mhi_tftp_end(victim);
			t = victim;
		}
	}
	if (t == NULL) {
		if (vnode != NULLVP) {
			vnode_put(vnode);
		}
		hakios_tl("pcie2: AMSS: %s: tftp: the modem %s \"%s\": no free transfer\n",
		    what, writing ? "writes" : "reads", name);
		mhi_tftp_error(out, service_port, node, port, MHI_TFTP_ENOENT, "busy");
		return;
	}
	slot = (unsigned)(t - mhi_tftp_transfers);
	if (t->vnode != NULLVP) {
		vnode_put(t->vnode);		/* (a repeat takes its slot over) */
		t->vnode = NULLVP;
	}
	t->used = true;
	t->writing = writing;
	t->node = node;
	t->peer = port;
	t->vnode = vnode;
	t->memory = memory;
	t->size = file_size;
	t->blksize = blksize;
	t->wsize = wsize;
	t->rsize = rsize;
	t->seek = seek;
	t->sent = 0;
	t->written_size = 0;
	t->expected = 1;
	strlcpy(t->name, name, sizeof(t->name));
	if (writing) {
		hakios_tl("pcie2: AMSS: %s: tftp: the modem writes \"%s\": blocks of %u, window %u\n",
		    what, t->name, blksize, wsize);
	} else {
		hakios_tl("pcie2: AMSS: %s: tftp: the modem reads \"%s\": %llu bytes; blocks of %u, "
		    "window %u, %u asked for, from %u%s\n", what, t->name,
		    (unsigned long long)file_size, blksize, wsize, rsize, seek,
		    options ? "" : " (no options)");
	}
	if (!options) {
		if (writing) {
			uint8_t ack[4] = { 0, MHI_TFTP_ACK, 0, 0 };
			uint8_t aframe[16 + sizeof(ack)];
			size_t alen = mhi_qrtr_put3_frame(aframe, 1, MHI_TFTP_PORT(slot), node, port,
			    ack, sizeof(ack));

			(void)mhi_send(out, aframe, alen);
		} else {
			mhi_tftp_window(out, t, 0);
		}
		return;
	}
	/* OACK: the options as taken, as stock's tqftpserv answers them */
	oack[0] = 0;
	oack[1] = MHI_TFTP_OACK;
	at = 2;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "blksize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", blksize) + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "timeoutms") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "1000") + 1;
	if (tsize && !writing) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "tsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%llu",
		    (unsigned long long)file_size) + 1;
	}
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "wsize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", wsize) + 1;
	if (rsize != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "rsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", rsize) + 1;
	}
	if (seek != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "seek") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", seek) + 1;
	}
	{
		uint8_t oframe[16 + sizeof(oack) + 4];
		size_t olen = mhi_qrtr_put3_frame(oframe, 1, MHI_TFTP_PORT(slot), node, port, oack,
		    at);
		int error = mhi_send(out, oframe, olen);

		hakios_tl("pcie2: AMSS: %s: tftp: the OACK for \"%s\": %u bytes of options, TX %s\n",
		    what, t->name, at - 2, error == 0 ? "completed" : "no completion");
	}
}

/* What the modem says on a transfer's port: acknowledgements, its data for a
 * write, or an error.  True when the frame belonged to a transfer. */
static bool
mhi_tftp_ack(struct mhi_channel *out, const uint8_t *frame, size_t size, unsigned header,
    unsigned dst_port, const char *what)
{
	struct mhi_tftp_transfer *t;
	uint16_t opcode, block;
	unsigned slot;

	if (dst_port < MHI_TFTP_PORT(0) || dst_port >= MHI_TFTP_PORT(MHI_TFTP_TRANSFERS)) {
		return false;
	}
	slot = dst_port - MHI_TFTP_PORT(0);
	t = &mhi_tftp_transfers[slot];
	if (!t->used) {
		return false;
	}
	size = mhi_qrtr_payload(frame, size, header);
	if (size < header + 4) {
		return true;
	}
	opcode = (uint16_t)(frame[header] << 8 | frame[header + 1]);
	block = (uint16_t)(frame[header + 2] << 8 | frame[header + 3]);
	if (t->writing && opcode == MHI_TFTP_DATA) {
		uint32_t n = (uint32_t)(size - header - 4);
		uint8_t ack[4] = { 0, MHI_TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };
		uint8_t aframe[16 + sizeof(ack)];
		size_t alen;

		if (block != t->expected) {
			mhi_tftp_error(out, MHI_TFTP_PORT(slot), t->node, t->peer,
			    MHI_TFTP_EBADOP, "block out of sequence");
			mhi_tftp_end(t);
			return true;
		}
		if (t->written_size + n > MHI_TFTP_WRITE_MAX) {
			mhi_tftp_error(out, MHI_TFTP_PORT(slot), t->node, t->peer, 3, "too much");
			mhi_tftp_end(t);
			return true;
		}
		memcpy(t->written + t->written_size, frame + header + 4, n);
		t->written_size += n;
		t->expected++;
		if (block % t->wsize == 0 || n < t->blksize) {
			alen = mhi_qrtr_put3_frame(aframe, 1, MHI_TFTP_PORT(slot), t->node,
			    t->peer, ack, sizeof(ack));
			(void)mhi_send(out, aframe, alen);
		}
		if (n < t->blksize) {
			/* the last block: the file is kept, under its name, for the run */
			unsigned f, free_slot = MHI_TFTP_KEEP_SLOTS, oldest = 0;
			bool have_oldest = false, kept;

			if (t->written_size == 0) {
				/* the empty write that asks whether the name takes
				 * writes; it is not a file and takes no slot */
				hakios_tl("pcie2: AMSS: %s: tftp: the modem has written \"%s\": "
				    "nothing, not stored\n", what, t->name);
				mhi_tftp_end(t);
				return true;
			}
			for (f = 0; f < MHI_TFTP_KEEP_SLOTS; f++) {
				if (mhi_tftp_kept[f].name[0] != 0 &&
				    strcmp(mhi_tftp_kept[f].name, t->name) == 0) {
					break;		/* its own slot */
				}
				if (mhi_tftp_kept[f].name[0] == 0 &&
				    free_slot == MHI_TFTP_KEEP_SLOTS) {
					free_slot = f;
				}
				if (mhi_tftp_kept[f].name[0] != 0 &&
				    (!have_oldest ||
				    mhi_tftp_kept[f].stamp < mhi_tftp_kept[oldest].stamp)) {
					oldest = f;
					have_oldest = true;
				}
			}
			if (f == MHI_TFTP_KEEP_SLOTS) {
				f = free_slot != MHI_TFTP_KEEP_SLOTS ? free_slot :
				    (have_oldest ? oldest : 0);
			}
			kept = t->written_size <= MHI_TFTP_KEEP_MAX;
			if (kept) {
				if (mhi_tftp_kept[f].name[0] != 0 &&
				    strcmp(mhi_tftp_kept[f].name, t->name) != 0) {
					hakios_tl("pcie2: AMSS: %s: tftp: \"%s\" makes way for "
					    "\"%s\"\n", what, mhi_tftp_kept[f].name, t->name);
				}
				memcpy(mhi_tftp_kept[f].data, t->written,
				    t->written_size);
				mhi_tftp_kept[f].size = t->written_size;
				mhi_tftp_kept[f].stamp = ++mhi_tftp_kept_clock;
				strlcpy(mhi_tftp_kept[f].name, t->name,
				    sizeof(mhi_tftp_kept[f].name));
			}
			hakios_tl("pcie2: AMSS: %s: tftp: the modem has written \"%s\": %u bytes, "
			    "%s\n", what, t->name, t->written_size,
			    kept ? "kept" : "NOWHERE (over 128 KB)");
			mhi_tftp_end(t);
		}
		return true;
	}
	if (opcode == MHI_TFTP_ERROR_OP) {
		if (block != MHI_TFTP_EOT) {
			hakios_tl("pcie2: AMSS: %s: tftp: \"%s\": the modem ends it, error %u\n",
			    what, t->name, block);
		}
		mhi_tftp_end(t);
		return true;
	}
	if (opcode != MHI_TFTP_ACK) {
		mhi_tftp_error(out, MHI_TFTP_PORT(slot), t->node, t->peer, MHI_TFTP_EBADOP,
		    "expected ACK");
		mhi_tftp_end(t);
		return true;
	}
	if ((uint64_t)t->seek + (uint64_t)block * t->blksize >= t->size ||
	    (t->rsize != 0 && (uint64_t)block * t->blksize >= t->rsize)) {
		hakios_tl("pcie2: AMSS: %s: tftp: \"%s\": %u blocks gone\n", what, t->name, t->sent);
		{
			size_t name_len = strlen(t->name);

			if (name_len >= 10 &&
			    strcmp(t->name + name_len - 10, "mbn_sw.dig") == 0) {
				mhi_mcfg_dig_done = 1;
			}
		}
		mhi_tftp_end(t);
		return true;
	}
	mhi_tftp_window(out, t, block);
	return true;
}

/*
 * Mission-mode EFS sync over MHI channels 10 and 11, run the way umi's
 * proven host runs it (umi_modem_mhi.inc, md_efs_message): once AMSS is up
 * the modem opens a Sahara memory-debug session on the EFS pair and offers
 * its EFS image; the host reads every region the session names, discards
 * every byte, RESETs, and counts the RESET_RESP as the acknowledgement.  The
 * channels are started with IPCR in mission mode (md_start_channel(4,10,1) /
 * (5,11,1) there): without them the offer has no one to go to and the
 * modem's boot never completes.  RAM only: nothing is stored and no
 * partition is opened.
 */
#define MHI_EFS_OUT_SLOT 6u
#define MHI_EFS_IN_SLOT  7u
#define MHI_EFS_OUT_ID   10u
#define MHI_EFS_IN_ID    11u
/* ER 1, the ring umi's proven host gives the EFS pair
 * (md_start_channel(4,10,1)/(5,11,1)) and the ring the vendor's own channel
 * contexts put every id below 12 on.  Run 055654 used ER 3 and the modem
 * opened both channels without a word ever arriving on the in ring. */
#define MHI_EFS_ER       1u

static uint8_t mhi_efs_stream[MHI_PACKET];
static uint8_t mhi_efs_table[4096];
static size_t mhi_efs_pos, mhi_efs_size;
static unsigned mhi_efs_regions;
static uint64_t mhi_efs_bytes;
static bool mhi_efs_offered, mhi_efs_done;

/* data NULL discards.  Mid-message silence is a protocol error, not the
 * idle ETIMEDOUT the IPCR loops ignore. */
static int
mhi_efs_exact(void *data, size_t length)
{
	uint8_t *p = data;

	while (length != 0) {
		size_t n;

		if (mhi_efs_pos == mhi_efs_size) {
			int error;

			mhi_efs_size = sizeof(mhi_efs_stream);
			mhi_efs_pos = 0;
			error = mhi_receive(&mhi_channels[MHI_EFS_IN_SLOT],
			    mhi_efs_stream, &mhi_efs_size, 5000);
			if (error != 0) {
				mhi_efs_size = 0;
				return error == ETIMEDOUT ? EIO : error;
			}
			if (mhi_efs_size == 0) {
				return EIO;
			}
		}
		n = mhi_efs_size - mhi_efs_pos;
		if (n > length) {
			n = length;
		}
		if (p != NULL) {
			memcpy(p, mhi_efs_stream + mhi_efs_pos, n);
			p += n;
		}
		mhi_efs_pos += n;
		length -= n;
	}
	return 0;
}

static int
mhi_efs_read(uint64_t address, uint64_t length, bool wide, void *data)
{
	uint32_t read[6] = { 10, 16, (uint32_t)address, (uint32_t)length };

	if (wide) {
		read[0] = 17;
		read[1] = 24;
		mhi_set64(read, 2, address);
		mhi_set64(read, 4, length);
	} else if (address + length > (1ull << 32)) {
		return EPROTO;
	}
	{
		int error = mhi_send(&mhi_channels[MHI_EFS_OUT_SLOT], read, read[1]);

		if (error != 0) {
			return error;
		}
	}
	return mhi_efs_exact(data, (size_t)length);
}

static int
mhi_efs_message(void)
{
	struct mhi_channel *out = &mhi_channels[MHI_EFS_OUT_SLOT];
	uint32_t m[64];
	int error;

	error = mhi_efs_exact(m, 8);
	if (error != 0) {
		return error;
	}
	if (m[1] < 8 || m[1] > sizeof(m)) {
		return EPROTO;
	}
	error = mhi_efs_exact(m + 2, m[1] - 8);
	if (error != 0) {
		return error;
	}
	if (m[0] == 1 && m[1] == 48) {		/* HELLO */
		uint32_t hello[12] = { 2, 48, 0, 0, 0, 2 };

		if (m[2] < 1 || m[3] > 2 || m[5] != 2) {
			return ENOTSUP;
		}
		hello[2] = m[2] < 2 ? m[2] : 2;
		hello[3] = m[3];
		mhi_efs_regions = 0;
		mhi_efs_bytes = 0;
		mhi_efs_offered = true;
		hakios_tl("pcie2: AMSS: EFS sync: the modem opens its EFS session (version %u.%u)\n",
		    m[2], m[3]);
		return mhi_send(out, hello, sizeof(hello));
	}
	if ((m[0] == 9 && m[1] == 16) || (m[0] == 16 && m[1] == 24)) {
		bool wide = m[0] == 16;
		uint64_t address = wide ? mhi_u64(m + 2) : m[2];
		uint64_t size = wide ? mhi_u64(m + 4) : m[3], total = 0;
		size_t stride = wide ? 64 : 52;

		if (size == 0 || size > sizeof(mhi_efs_table) || (size % stride) != 0) {
			return EPROTO;
		}
		error = mhi_efs_read(address, size, wide, mhi_efs_table);
		if (error != 0) {
			return error;
		}
		for (size_t at = 0; at < size; at += stride) {
			const uint8_t *entry = mhi_efs_table + at;
			uint64_t base = wide ? mhi_u64(entry + 8) : mhi_u32(entry + 4);
			uint64_t n = wide ? mhi_u64(entry + 16) : mhi_u32(entry + 8);
			uint64_t off;

			if (n > (64ull << 20) || (total += n) > (128ull << 20)) {
				return EPROTO;
			}
			for (off = 0; off < n; off += MHI_PACKET) {
				error = mhi_efs_read(base + off,
				    n - off < MHI_PACKET ? n - off : MHI_PACKET, wide, NULL);
				if (error != 0) {
					return error;
				}
			}
		}
		mhi_efs_regions = (unsigned)(size / stride);
		mhi_efs_bytes = total;
		hakios_tl("pcie2: AMSS: EFS sync: read %u regions, %llu bytes (discarded); RESET\n",
		    mhi_efs_regions, (unsigned long long)mhi_efs_bytes);
		{
			uint32_t reset[2] = { 7, 8 };

			return mhi_send(out, reset, sizeof(reset));
		}
	}
	if (m[0] == 8 && m[1] == 8) {		/* RESET_RESP */
		mhi_efs_done = true;
		hakios_tl("pcie2: AMSS: EFS sync: %u regions, %llu bytes acknowledged in RAM (discarded)\n",
		    mhi_efs_regions, (unsigned long long)mhi_efs_bytes);
		return 0;
	}
	hakios_tl("pcie2: AMSS: EFS sync: unexpected command %u length %u\n", m[0], m[1]);
	return EPROTO;
}

/* Service one EFS message when one is waiting (or a message is half read).
 * Returns 0 when there was nothing to do; a protocol error is the caller's
 * to log and move past. */
static int
mhi_efs_service(void)
{
	int error;

	if (mhi_channels[MHI_EFS_IN_SLOT].ring == NULL) {
		return 0;
	}
	if (!mhi_channels[MHI_EFS_IN_SLOT].rx_ready && mhi_efs_pos == mhi_efs_size) {
		return 0;
	}
	error = mhi_efs_message();
	if (error != 0) {
		hakios_tl("pcie2: AMSS: EFS sync: the exchange stopped (error %d); stream dropped\n",
		    error);
		mhi_efs_pos = mhi_efs_size = 0;
	}
	return error;
}

/* The host's services, told to the modem's nameservice the way stock's
 * kernel tells it: Linux's name service answers a HELLO with its own and
 * announces its whole server table to the node that said it (net/qrtr/ns.c,
 * ctrl_cmd_hello -> announce_servers), and answers a lookup with the
 * matching servers and an empty NEW_SERVER that ends the answer.  Until the
 * modem hears those announcements its clients cannot address node 1, which
 * is why the modem's boot never found the services it was waiting for. */
static void
mhi_qrtr_announce(struct mhi_channel *out, unsigned modem, unsigned service, const char *what)
{
	unsigned sent = 0;

	for (unsigned i = 0; i < MHI_HOST_SERVICE_COUNT; i++) {
		uint8_t packet[20] = { 0 }, frame[40];
		size_t length;

		if (pcie2_mhi_mode == 2 &&
		    (mhi_host_services[i].service == 0x302 || mhi_host_services[i].service == 0x28)) {
			continue;
		}
		if (service != ~0u && mhi_host_services[i].service != service) {
			continue;
		}
		mhi_qrtr_put(packet, 0, 4);	/* QRTR_TYPE_NEW_SERVER */
		mhi_qrtr_put(packet, 4, mhi_host_services[i].service);
		mhi_qrtr_put(packet, 8, mhi_host_services[i].instance);
		mhi_qrtr_put(packet, 12, 1);	/* the host's node */
		mhi_qrtr_put(packet, 16, mhi_host_services[i].port);
		length = mhi_qrtr_put3(frame, 4, 0xfffe, modem, 0xfffe, packet,
		    sizeof(packet));
		if (mhi_send(out, frame, length) == 0) {
			sent++;
		}
	}
	if (pcie2_mhi_mode == 2 && mhi_bb_host_known &&
	    (service == ~0u || service == mhi_bb_host_svc[1])) {
		uint8_t frame[40];
		size_t length = mhi_qrtr_put3(frame, 4, 0xfffe, modem, 0xfffe,
		    mhi_bb_host_svc, sizeof(mhi_bb_host_svc));

		if (mhi_send(out, frame, length) == 0) {
			sent++;
		}
	}
	hakios_tl("pcie2: AMSS: %s: %u of the host's %u services announced to node %u\n",
	    what, sent, MHI_HOST_SERVICE_COUNT, modem);
}

/* The 20-byte control packet a hello and a lookup carry: the cmd, then the
 * union's first word -- the service a lookup asks for. */
static void
mhi_qrtr_control(uint8_t *packet, unsigned cmd, unsigned service)
{
	memset(packet, 0, 20);
	mhi_qrtr_put(packet, 0, cmd);
	mhi_qrtr_put(packet, 4, service);
}

/* Ask the modem's name service where it now puts TFTP: the answer (a
 * NEW_SERVER naming service 4096, node 1 and one of our ports) is the
 * evidence that the host's registration stuck.  A lookup has no answer worth
 * reading when nothing matched, so its absence is a finding too.  The
 * instance asked for is version 3's word (3), what the SDX55's TFTP client
 * looks up. */
static void
mhi_qrtr_tftp_lookup(struct mhi_channel *out)
{
	uint8_t control[20], frame[64];
	size_t length;

	mhi_qrtr_control(control, 10, 4096);
	mhi_qrtr_put(control, 8, 3);
	length = mhi_qrtr_put3(frame, 10, 0xfffe, 0xffff, 0xfffe, control,
	    sizeof(control));
	hakios_tl("pcie2: AMSS: IPCR TFTP lookup (v3): TX %s\n",
	    mhi_send(out, frame, length) == 0 ? "completed" : "no completion");
}

/* One received frame in either header the two ends use: this host sends the
 * vendor kernel's version 1 (eight 32-bit words, ports and nodes 32 bits) and
 * the modem answers in version 3 -- the vendor tree's qrtr_hdr_v2, sixteen
 * bytes of version/type/flags/optlen, a 32-bit size, then four 16-bit node
 * and port fields.  A control frame names the service it is about after its
 * cmd word; a data frame carries one QMI message. */
static void
mhi_qrtr_frame(const char *what, const uint8_t *frame, size_t length)
{
	char hex[3 * 48 + 1];
	unsigned shown = (unsigned)length > 48 ? 48 : (unsigned)length;
	unsigned type = 0, size = 0, src_node = 0, src_port = 0, dst_node = 0, dst_port = 0;
	unsigned header;

	for (unsigned i = 0; i < shown; i++) {
		snprintf(hex + i * 3, 4, " %02x", frame[i]);
	}
	hex[shown * 3] = 0;
	hakios_tl("pcie2: AMSS: %s: %u bytes:%s%s\n", what, (unsigned)length, hex,
	    (unsigned)length > shown ? " ..." : "");
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		type = mhi_qrtr_word(frame, 4);
		src_node = mhi_qrtr_word(frame, 8);
		src_port = mhi_qrtr_word(frame, 12);
		size = mhi_qrtr_word(frame, 20);
		dst_node = mhi_qrtr_word(frame, 24);
		dst_port = mhi_qrtr_word(frame, 28);
		header = 32;
		hakios_tl("pcie2: AMSS: %s: v1 type %u size %u from %u:%#x to %u:%#x\n", what,
		    type, size, src_node, src_port, dst_node, dst_port);
	} else if (length >= 16) {
		type = frame[1];
		size = mhi_qrtr_word(frame, 4);
		src_node = frame[8] | (unsigned)frame[9] << 8;
		src_port = frame[10] | (unsigned)frame[11] << 8;
		dst_node = frame[12] | (unsigned)frame[13] << 8;
		dst_port = frame[14] | (unsigned)frame[15] << 8;
		header = 16;
		hakios_tl("pcie2: AMSS: %s: v%u type %u size %u from %u:%#x to %u:%#x\n", what,
		    frame[0], type, size, src_node, src_port, dst_node, dst_port);
	} else {
		return;
	}
	if (header + size > length) {
		return;
	}
	if (type == 1 && size >= 7) {
		const uint8_t *qmi = frame + header;

		hakios_tl("pcie2: AMSS: %s: QMI type %u transaction %u message %#x length %u\n", what,
		    qmi[0], qmi[1] | (unsigned)qmi[2] << 8, qmi[3] | (unsigned)qmi[4] << 8,
		    qmi[5] | (unsigned)qmi[6] << 8);
	} else if (type != 1 && size >= 20) {
		const uint8_t *packet = frame + header;

		hakios_tl("pcie2: AMSS: %s: control cmd %u: service %#x instance %u node %u port %#x\n",
		    what, mhi_qrtr_word(packet, 0), mhi_qrtr_word(packet, 4),
		    mhi_qrtr_word(packet, 8), mhi_qrtr_word(packet, 12),
		    mhi_qrtr_word(packet, 16));
	}
}

/* The frame's QRTR type in either header shape; ~0u when it is too short. */
static unsigned
mhi_qrtr_type(const uint8_t *frame, size_t length)
{
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		return mhi_qrtr_word(frame, 4);
	}
	if (length >= 16) {
		return frame[1];
	}
	return ~0u;
}

/* The node a HELLO frame names as its source, in either header shape. */
static bool
mhi_qrtr_hello_node(const uint8_t *frame, size_t length, unsigned *node)
{
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1 && mhi_qrtr_word(frame, 4) == 2) {
		*node = mhi_qrtr_word(frame, 8);
	} else if (length >= 16 && frame[1] == 2) {
		*node = frame[8] | (unsigned)frame[9] << 8;
	} else {
		return false;
	}
	return *node != 0 && *node != 1;
}

/* NEW_SERVER (4) for the UIM service (0x0b): the announcement's {service,
 * instance, node, port} says where that service's QMI messages go. */
static bool
mhi_qrtr_uim(const uint8_t *frame, size_t length, unsigned *node, unsigned *port)
{
	const uint8_t *packet;
	unsigned header, type, size;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		type = mhi_qrtr_word(frame, 4);
		size = mhi_qrtr_word(frame, 20);
	} else if (length >= 16) {
		header = 16;
		type = frame[1];
		size = mhi_qrtr_word(frame, 4);
	} else {
		return false;
	}
	if (type != 4 || size < 20 || header + 20 > length) {
		return false;
	}
	packet = frame + header;
	if (mhi_qrtr_word(packet, 0) != 4 || mhi_qrtr_word(packet, 4) != 0x0b) {
		return false;
	}
	*node = mhi_qrtr_word(packet, 12);
	*port = mhi_qrtr_word(packet, 16);
	return true;
}

/* The {service, instance, node, port} of a NEW_SERVER (4) announcement,
 * whichever service it names. */
static bool
mhi_qrtr_server(const uint8_t *frame, size_t length, unsigned *service, unsigned *node,
    unsigned *port, unsigned *instance)
{
	const uint8_t *packet;
	unsigned header, type, size;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		type = mhi_qrtr_word(frame, 4);
		size = mhi_qrtr_word(frame, 20);
	} else if (length >= 16) {
		header = 16;
		type = frame[1];
		size = mhi_qrtr_word(frame, 4);
	} else {
		return false;
	}
	if (type != 4 || size < 20 || header + 20 > length) {
		return false;
	}
	packet = frame + header;
	if (mhi_qrtr_word(packet, 0) != 4) {
		return false;
	}
	*service = mhi_qrtr_word(packet, 4);
	*instance = mhi_qrtr_word(packet, 8);
	*node = mhi_qrtr_word(packet, 12);
	*port = mhi_qrtr_word(packet, 16);
	return true;
}

/* The empty NEW_SERVER that ends the answer to a lookup (Linux's name
 * service sends one after the matching servers, net/qrtr/ns.c). */
static void
mhi_qrtr_announce_end(struct mhi_channel *out, unsigned modem)
{
	uint8_t packet[20] = { 0 }, frame[40];
	size_t length;

	mhi_qrtr_put(packet, 0, 4);
	length = mhi_qrtr_put3(frame, 4, 0xfffe, modem, 0xfffe, packet,
	    sizeof(packet));
	(void)mhi_send(out, frame, length);
}

static void mhi_sms_note(const uint8_t *qmi, size_t body, const char *what);
static int mhi_qrtr_from(unsigned node, unsigned port, unsigned service);

static void mhi_qrtr_remember(unsigned service, unsigned node, unsigned port);
static void mhi_qrtr_learn(unsigned service, unsigned instance, unsigned node,
    unsigned port);
static void mhi_ims_probe_note(const uint8_t *frame, size_t length, const char *what);
static void mhi_imsa_note(const uint8_t *frame, size_t length, const char *what);

/* QMI VOICE (service 0x09, libqmi's qmi-service-voice.json): the All Call
 * Status indication 0x002e carries TLV 0x01 as a u8 count and then that
 * many 7-byte records {ID, State, Type, Direction, Mode, Multipart, ALS}.
 * Decoded by name here so the call's progress (origination, alerting,
 * conversation) is readable in the log, not only as hex. */
static const char *
mhi_call_state(unsigned state)
{
	static const char *const states[] = {
		"unknown", "origination", "incoming", "conversation",
		"CC in progress", "alerting", "hold", "waiting",
		"disconnecting", "end", "setup"
	};

	return state < sizeof(states) / sizeof(states[0]) ? states[state] : "?";
}

static const char *
mhi_call_type(unsigned type)
{
	static const char *const types[] = {
		"voice", "invalid", "voice IP", "OTAPA", "USSD", "emergency"
	};

	return type < sizeof(types) / sizeof(types[0]) ? types[type] : "?";
}

/* The last call an All Call Status indication named and has not yet ended.
 * The dial's own answer can be lost -- run 182052's dial was answered
 * "invalid argument" although an outgoing call already existed -- and End
 * Call needs the ID whether or not the answer carried one. */
static unsigned voice_indication_call = ~0u;

/* Whether a call has been seen ringing or talking (state 3 conversation,
 * 5 alerting, 6 hold, 7 waiting).  mhi_voice_retry leaves a first dial
 * alone when it did get that far. */
static bool voice_call_progressed;

static void
mhi_voice_note(const uint8_t *qmi, size_t length, const char *what)
{
	unsigned at;

	if (length < 7 || (qmi[0] & 0x06) != 0x04) {
		return;
	}
	if ((qmi[3] | (unsigned)qmi[4] << 8) != 0x002e) {
		return;
	}
	for (at = 7; at + 3 <= length; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > length) {
			break;
		}
		if (type == 0x14 && size >= 2) {
			const uint8_t *value = qmi + at + 3;
			unsigned kind = value[0] | (unsigned)value[1] << 8;
			unsigned code = size >= 4 ?
			    (unsigned)value[2] | (unsigned)value[3] << 8 : 0;

			/* QMI VOICE's call end reason, four bytes on the wire.
			 * Run 154832's status frames carry it, but the generic
			 * 48-byte hex dump cut it off; this reads it out. */
			hakios_tl("pcie2: cellular: VOICE: the call end reason (%s): type %u, code %u (%02x %02x %02x %02x)\n",
			    what, kind, code, value[0], value[1],
			    size >= 4 ? value[2] : 0, size >= 4 ? value[3] : 0);
		}
		if (type == 0x01 && size >= 1) {
			const uint8_t *value = qmi + at + 3;
			unsigned count = value[0];

			for (unsigned i = 0; i < count && 1 + (i + 1) * 7 <= size; i++) {
				const uint8_t *call = value + 1 + i * 7;

				hakios_tl("pcie2: cellular: VOICE: the call status (%s) says id %u, state %u (%s), type %u (%s), direction %u, mode %u, multipart %u, ALS %u\n",
				    what, call[0], call[1], mhi_call_state(call[1]),
				    call[2], mhi_call_type(call[2]), call[3], call[4],
				    call[5], call[6]);
				if (call[1] == 3 || call[1] == 5 || call[1] == 6 ||
				    call[1] == 7) {
					voice_call_progressed = true;
				}
				if (call[1] != 0 && call[1] != 9) {
					voice_indication_call = call[0];
				} else if (call[1] == 9) {
					voice_indication_call = ~0u;
				}
			}
		}
		at += 3 + size;
	}
}

/* The channel, drained for a while without asking anything: whatever the
 * modem sends while its boot waits -- its nameservice's control frames and
 * any QMI request to the host's services -- is logged as it arrives, a hello
 * or a lookup is answered with the host's announcements, and a request to a
 * host service is printed in full, the registry's answered on the spot
 * (mhi_servreg_answer) and TFTP's left for the next round.
 * The probe's own requests then find the channel clear, and the requests the
 * modem made while nothing was watching are no longer guessed at.  Returns
 * the frames heard. */
static unsigned
mhi_qrtr_pump(struct mhi_channel *out, struct mhi_channel *in, unsigned ms, const char *what)
{
	/* a whole datagram: a TFTP write can be blocks of 8000 */
	static uint8_t frame[MHI_PACKET];
	unsigned heard = 0;

	/* The modem's TFTP client keeps one window in flight at a time, so
	 * while its scan runs there is nearly always a request or an ACK
	 * queued.  A pump that counted every receive attempt against its
	 * budget -- six per call, as this one did -- paced the scan at a
	 * handful of frames a second: run 133235's five-minute watch served
	 * 35 of mbn_sw.txt's 267 configs and the modem was still in SEA/XTL
	 * when its 240 rounds ran out.  A frame that is there is taken at
	 * once; only an empty channel is waited on, and only that wait
	 * counts against the budget.  The cap keeps one pump from never
	 * returning while the modem floods. */
	for (unsigned waited = 0; waited < ms && heard < 4000; ) {
		size_t got = sizeof(frame);
		unsigned header, type, src_node, src_port, dst_port;
		bool confirm;

		/* The modem's EFS offer rides its own channel; take it first,
		 * as umi's md_ipcr_receive does. */
		(void)mhi_efs_service();
		if (mhi_receive(in, frame, &got, 100) != 0) {
			waited += 100;
			continue;
		}
		heard++;
		if (got >= 32 && mhi_qrtr_word(frame, 0) == 1) {
			header = 32;
			type = mhi_qrtr_word(frame, 4);
			src_node = mhi_qrtr_word(frame, 8);
			src_port = mhi_qrtr_word(frame, 12);
			confirm = mhi_qrtr_word(frame, 16) != 0;
			dst_port = mhi_qrtr_word(frame, 28);
		} else if (got >= 16) {
			header = 16;
			type = frame[1];
			src_node = frame[8] | (unsigned)frame[9] << 8;
			src_port = frame[10] | (unsigned)frame[11] << 8;
			confirm = (frame[2] & 1) != 0;
			dst_port = frame[14] | (unsigned)frame[15] << 8;
		} else {
			continue;
		}
		if (confirm) {
			mhi_qrtr_resume(out, dst_port, src_node, src_port);
		}
		if (type == 1) {
			int service = mhi_host_service(dst_port);

			if (service >= 0 && mhi_host_services[service].service == 4096) {
				mhi_tftp_request(out, frame, got, header, what);
				continue;
			}
			if (service < 0 && mhi_tftp_ack(out, frame, got, header, dst_port, what)) {
				continue;
			}
			if (service < 0 && mhi_qrtr_from(src_node, src_port, 0x05)) {
				/* The modem's own QMI traffic to this host: an
				 * indication is not an answer, and the WMS
				 * new-message one is the receive test's event. */
				mhi_sms_note(frame + header, got - header, what);
			}
			mhi_voice_note(frame + header, got - header, what);
			mhi_ims_probe_note(frame, got, what);
			mhi_imsa_note(frame, got, what);
			mhi_qrtr_frame(service >= 0 ? "IPCR host request" : "IPCR data",
			    frame, got);
			if (service >= 0) {
				const uint8_t *qmi = frame + header;
				size_t shown = got;

				if (shown > 160) {
					shown = 160;
				}
				for (size_t at = 0; at < shown; at += 48) {
					char hex[3 * 48 + 1];
					size_t n = shown - at > 48 ? 48 : shown - at;

					for (size_t i = 0; i < n; i++) {
						snprintf(hex + i * 3, 4, " %02x",
						    frame[at + i]);
					}
					hex[n * 3] = 0;
					hakios_tl("pcie2: AMSS: %s: the host's %s, %zu bytes at %zu:%s\n",
					    what, mhi_host_services[service].name, got,
					    at, hex);
				}
				if (got >= header + 7) {
					hakios_tl("pcie2: AMSS: %s: the host's %s: message %#x, "
					    "transaction %u, %u bytes\n", what,
					    mhi_host_services[service].name,
					    qmi[3] | (unsigned)qmi[4] << 8,
					    qmi[1] | (unsigned)qmi[2] << 8,
					    qmi[5] | (unsigned)qmi[6] << 8);
					if (mhi_host_services[service].service == 64) {
						mhi_servreg_answer(out, frame, got, header, what);
					} else if (mhi_host_services[service].service == 0x302 ||
					    mhi_host_services[service].service == 0x28) {
						mhi_imsdcm_answer(out, frame, got, header,
						    mhi_host_services[service].port, what);
					}
				}
			}
			continue;
		}
		mhi_qrtr_frame("IPCR control", frame, got);
		if (type == 4) {
			/* A service announced after the boot's hello rounds:
			 * the modem's WMS can register late (run 091313: the
			 * milestone waited 6718 cycles for it), and the table
			 * dump a later hello draws comes through here. */
			unsigned service, node, port, instance;

			if (mhi_qrtr_server(frame, got, &service, &node, &port,
			    &instance)) {
				mhi_qrtr_learn(service, instance, node, port);
			}
		} else if (type == 2) {
			/* A hello: answer it once with our own -- two peers that
			 * answer every hello never stop (drivers/modem/hakios.mpss.c
			 * learned that on phone #1) -- and then tell it the host's
			 * services, which is stock's kernel's part (net/qrtr/ns.c
			 * ctrl_cmd_hello -> announce_servers); its clients can then
			 * address node 1. */
			static bool hello_echoed;

			if (!hello_echoed) {
				uint8_t hello[4] = { 0 };
				size_t length;

				hello_echoed = true;
				mhi_qrtr_put(hello, 0, 2);
				length = mhi_qrtr_put3(frame, 4, 0xfffe, src_node,
				    0xfffe, hello, sizeof(hello));
				hakios_tl("pcie2: AMSS: %s: the modem said hello; ours to it: %s\n",
				    what, mhi_send(out, frame, length) == 0 ?
				    "sent" : "not sent");
			}
			mhi_qrtr_announce(out, src_node, ~0u, what);
		} else if (type == 10 && got >= header + 20) {
			unsigned service = mhi_qrtr_word(frame + header, 4);

			hakios_tl("pcie2: AMSS: %s: the modem looks for service %#x; answering\n",
			    what, service);
			mhi_qrtr_announce(out, src_node, service, what);
			mhi_qrtr_announce_end(out, src_node);
		}
	}
	if (heard != 0) {
		hakios_tl("pcie2: AMSS: %s: %u frames heard while it waited\n", what, heard);
	}
	return heard;
}

/* The host's services, served the moment one of the modem's requests is
 * heard.  While the hello rounds wait on the modem's announcements its boot
 * keeps asking node 1 for what it needs -- run 044127's round 0 carried 51
 * requests: the registry's "tms/pddump_disabled" and "avs/audio" lookups and
 * TFTP's "/readwrite/server_check.txt", and the drain dropped every one
 * unanswered, logging it as "IPCR other"; the modem did not publish DMS
 * until 15 s in, when the post-contact pump finally served the queue.  Same
 * serving as mhi_qrtr_pump's type-1 branch, without its hexdump.  True when
 * the frame was one of those requests and has been answered. */
static bool
mhi_host_request(struct mhi_channel *out, const uint8_t *frame, size_t got,
    const char *what)
{
	unsigned header, type, src_node, src_port, dst_port;
	bool confirm;
	int service;

	if (got >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		type = mhi_qrtr_word(frame, 4);
		src_node = mhi_qrtr_word(frame, 8);
		src_port = mhi_qrtr_word(frame, 12);
		confirm = mhi_qrtr_word(frame, 16) != 0;
		dst_port = mhi_qrtr_word(frame, 28);
	} else if (got >= 16) {
		header = 16;
		type = frame[1];
		src_node = frame[8] | (unsigned)frame[9] << 8;
		src_port = frame[10] | (unsigned)frame[11] << 8;
		confirm = (frame[2] & 1) != 0;
		dst_port = frame[14] | (unsigned)frame[15] << 8;
	} else {
		return false;
	}
	if (type != 1) {
		return false;
	}
	if (confirm) {
		mhi_qrtr_resume(out, dst_port, src_node, src_port);
	}
	service = mhi_host_service(dst_port);
	if (service >= 0 && mhi_host_services[service].service == 4096) {
		hakios_tl("pcie2: AMSS: %s: the host's %s asked; answering\n", what,
		    mhi_host_services[service].name);
		mhi_tftp_request(out, frame, got, header, what);
		return true;
	}
	if (service >= 0 && mhi_host_services[service].service == 64) {
		hakios_tl("pcie2: AMSS: %s: the host's %s asked; answering\n", what,
		    mhi_host_services[service].name);
		mhi_servreg_answer(out, frame, got, header, what);
		return true;
	}
	if (service >= 0 && (mhi_host_services[service].service == 0x302 ||
	    mhi_host_services[service].service == 0x28)) {
		hakios_tl("pcie2: AMSS: %s: the host's %s asked; answering\n", what,
		    mhi_host_services[service].name);
		mhi_imsdcm_answer(out, frame, got, header,
		    mhi_host_services[service].port, what);
		return true;
	}
	if (service < 0 && mhi_tftp_ack(out, frame, got, header, dst_port, what)) {
		return true;
	}
	return false;
}

/* The services a GET_VERSION goes to once the card question has failed --
 * WDS, DMS, NAS, WMS, UIM: is the NO_MEMORY one sick service or the whole
 * QMI path?  Their node and port are remembered as announced. */
static const char *
mhi_qrtr_name(unsigned service)
{
	switch (service) {
	case 0x01:
		return "WDS";
	case 0x02:
		return "DMS";
	case 0x03:
		return "NAS";
	case 0x04:
		return "QOS";
	case 0x05:
		return "WMS";
	case 0x09:
		return "VOICE";
	case 0x0b:
		return "UIM";
	case 0x11:
		return "SAR";
	case 0x12:
		return "IMS";
	case 0x1a:
		return "WDA";
	case 0x21:
		return "IMSA";
	case 0x24:
		return "PDC";
	case 0x44:
		/* Not the settings stream: the 08:17 capture (qcrild.trace)
		 * shows qcrild's fd 49 talking to 0x44 with 0x20 {u32 2},
		 * 0x22, 0x2d {u64 1/2} and repeating 0x23/0x28 with 3000 ms
		 * (0x0bb8) and 50 (0x32) -- a keepalive/timer table, not the
		 * IMS feature words.  The feature/APN stream goes to 0x2a. */
		return "IMS timers";
	case 0x2a:
		return "DSD";
	case 0x2f:
		return "DPM";
	default:
		return "service";
	}
}

static struct { unsigned service, node, port; } mhi_qrtr_servers[16];
static unsigned mhi_qrtr_server_count;

static void
mhi_qrtr_remember(unsigned service, unsigned node, unsigned port)
{
	/* 0x11 is SAR, not IMS: run 121242 sent the IMS settings there and
	 * every message was refused error 57, while the IMS service proper
	 * (0x12) was announced at node 3 port 0x49.  DPM (0x2f) and WDA
	 * (0x1a) are the data-port pair the PDN needs.  PDC (0x24) is in the
	 * list because run 185826's name-service dump shows the modem
	 * announces it at node 3 port 0x11 -- the probe there never ran:
	 * this whitelist had dropped the announcement, so the probe logged
	 * "not announced" while the table carried it.  Service 0x04 (QOS in
	 * libqmi) is in the list because the reference phone's data-port
	 * client looks it up and sends its mux bind there (see
	 * mhi_wds_data_path); without it here that lookup fails "not
	 * announced" and the frames never go out. */
	bool wanted = service == 0x01 || service == 0x02 || service == 0x03 ||
	    service == 0x04 || service == 0x05 || service == 0x09 ||
	    service == 0x0b ||
	    service == 0x12 || service == 0x1a || service == 0x21 ||
	    service == 0x24 || service == 0x2a || service == 0x2f ||
	    /* 0x44 is the keepalive/timer service qcrild's fd 49 configures
	     * in the same capture (0x20/0x22/0x23/0x28/0x2d with 3000 ms).
	     * The IMS feature/APN stream is 0x2a, already in the list. */
	    service == 0x44;

	/* DSD (0x2a) is in the list because the reference phone's qmipriod
	 * holds a DSD client and its conversation is the last host action
	 * before the accepted WDS Start (see mhi_dsd_arm); this whitelist
	 * dropped the announcement for every earlier run. */
	if (!wanted) {
		/* Every announcement the table drops is logged -- capped so a
		 * re-announcing modem cannot flood the console -- because the
		 * silent return is what kept DSD invisible for many runs. */
		static unsigned dropped;

		if (dropped < 40) {
			dropped++;
			hakios_tl("pcie2: AMSS: the modem announced service 0x%02x at %u:%#x; not in the table, not remembered\n",
			    service, node, port);
		}
		return;
	}
	for (unsigned i = 0; i < mhi_qrtr_server_count; i++) {
		if (mhi_qrtr_servers[i].service == service) {
			/* A second node or port for a remembered service is
			 * worth one line: the table keeps the first and every
			 * later send goes there. */
			static unsigned duplicates;

			if ((mhi_qrtr_servers[i].node != node ||
			    mhi_qrtr_servers[i].port != port) &&
			    duplicates < 12) {
				duplicates++;
				hakios_tl("pcie2: AMSS: the modem announced %s (0x%02x) again at %u:%#x; the table keeps %u:%#x\n",
				    mhi_qrtr_name(service), service, node,
				    port, mhi_qrtr_servers[i].node,
				    mhi_qrtr_servers[i].port);
			}
			return;
		}
	}
	if (mhi_qrtr_server_count < sizeof(mhi_qrtr_servers) / sizeof(mhi_qrtr_servers[0])) {
		mhi_qrtr_servers[mhi_qrtr_server_count].service = service;
		mhi_qrtr_servers[mhi_qrtr_server_count].node = node;
		mhi_qrtr_servers[mhi_qrtr_server_count].port = port;
		mhi_qrtr_server_count++;
		/* The line that tells a later run the announcement was learned,
		 * not merely heard: run 091313 could not say which of the two
		 * had failed for WMS. */
		hakios_tl("pcie2: AMSS: the modem announced %s (0x%02x) at %u:%#x; remembered\n",
		    mhi_qrtr_name(service), service, node, port);
	}
}

/* Every service the modem's nameservice announces, instance included.  The
 * 16-entry table above keeps only the whitelist and drops the instance,
 * which is enough for the messages HakiOS already sends.  Android's IMS
 * vendor client, however, gets result 0 for its 0x4a/0x48 from a port our
 * service-0x12 entry has never matched while our byte-identical requests
 * there are refused (run 071041: 0x4a error 3, 0x48 error 70).  The vendor
 * probe needs the whole announcement list -- the 0x12 instance at another
 * port, or a vendor service the whitelist drops -- so every announcement is
 * surveyed here first. */
#define MHI_QRTR_ALL 96
static struct { unsigned service, instance, node, port; } mhi_qrtr_all[MHI_QRTR_ALL];
static unsigned mhi_qrtr_all_count;

static void
mhi_qrtr_survey(unsigned service, unsigned instance, unsigned node, unsigned port)
{
	for (unsigned i = 0; i < mhi_qrtr_all_count; i++) {
		if (mhi_qrtr_all[i].service == service &&
		    mhi_qrtr_all[i].instance == instance) {
			mhi_qrtr_all[i].node = node;
			mhi_qrtr_all[i].port = port;
			return;
		}
	}
	if (mhi_qrtr_all_count < MHI_QRTR_ALL) {
		unsigned i = mhi_qrtr_all_count++;

		mhi_qrtr_all[i].service = service;
		mhi_qrtr_all[i].instance = instance;
		mhi_qrtr_all[i].node = node;
		mhi_qrtr_all[i].port = port;
	}
}

/* One announcement: surveyed in full for the vendor probe, and remembered in
 * the short table when it is a service HakiOS sends to. */
static void
mhi_qrtr_learn(unsigned service, unsigned instance, unsigned node, unsigned port)
{
	mhi_qrtr_survey(service, instance, node, port);
	mhi_qrtr_remember(service, node, port);
	if (pcie2_mhi_mode == 2) {
		umi_bb_new_server(service, instance, node, port);
	}
}

/* The whole survey, once per boot: the rolling run log drops the boot's
 * announcement lines from its front, so the only place the full modem table
 * can still be read is here, when the IMS clients resolve their peers.  A
 * service the table dropped until now (0x44 was one) is visible either way. */
static void
mhi_qrtr_survey_dump(const char *what)
{
	static bool dumped;

	if (dumped) {
		return;
	}
	dumped = true;
	hakios_tl("pcie2: AMSS: %s: the modem's name service surveyed %u services\n",
	    what, mhi_qrtr_all_count);
	for (unsigned i = 0; i < mhi_qrtr_all_count; i++) {
		hakios_tl("pcie2: AMSS: %s: service 0x%02x (instance %u) at %u:%#x\n",
		    what, mhi_qrtr_all[i].service, mhi_qrtr_all[i].instance,
		    mhi_qrtr_all[i].node, mhi_qrtr_all[i].port);
	}
}

/* Whether a frame's source is the announced service: the modem's DMS sends
 * its own Event Report -- message 0x0001, the same id as WMS's -- so the
 * new-message handler must hear only the WMS port (run 064223 mistook the
 * power-state reports for new messages). */
static int
mhi_qrtr_from(unsigned node, unsigned port, unsigned service)
{
	for (unsigned i = 0; i < mhi_qrtr_server_count; i++) {
		if (mhi_qrtr_servers[i].service == service &&
		    mhi_qrtr_servers[i].node == node &&
		    mhi_qrtr_servers[i].port == port) {
			return 1;
		}
	}
	return 0;
}

/* Where an announced service's QMI messages go (0: it was not announced). */
static int
mhi_qrtr_service(unsigned service, unsigned *node, unsigned *port)
{
	for (unsigned i = 0; i < mhi_qrtr_server_count; i++) {
		if (mhi_qrtr_servers[i].service == service) {
			*node = mhi_qrtr_servers[i].node;
			*port = mhi_qrtr_servers[i].port;
			return 1;
		}
	}
	return 0;
}

/* The services the modem's nameservice has announced so far: a hello makes
 * it announce its whole table again (net/qrtr/ns.c: ctrl_cmd_hello ->
 * announce_servers), so the same names keep coming round after round and
 * only a new one deserves a line. */
static uint32_t mhi_qrtr_seen[48];
static unsigned mhi_qrtr_seen_count;

/* Set once a HELLO from the modem has named its node.  The host's own
 * servers go out the moment that is known: stock's kernel answers the
 * modem's hello with them, and the modem's boot waits on those servers. */
static bool mhi_qrtr_hello_heard;

static bool
mhi_qrtr_first(uint32_t service)
{
	for (unsigned i = 0; i < mhi_qrtr_seen_count; i++) {
		if (mhi_qrtr_seen[i] == service) {
			return false;
		}
	}
	if (mhi_qrtr_seen_count < sizeof(mhi_qrtr_seen) / sizeof(mhi_qrtr_seen[0])) {
		mhi_qrtr_seen[mhi_qrtr_seen_count++] = service;
	}
	return true;
}

/* One frame heard while the probe works the control port: a HELLO names the
 * modem's own node, a NEW_SERVER (4) announces one service, and the first
 * announcement of each service is logged with its hex.  UIM is learned
 * apart: its service (0x0b) and port are where the card-status question
 * goes. */
static void
mhi_qrtr_note(const uint8_t *frame, size_t length, unsigned round, unsigned *modem,
    unsigned *uim_node, unsigned *uim_port)
{
	unsigned header, type, service, node, port, instance;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		type = mhi_qrtr_word(frame, 4);
	} else if (length >= 16) {
		header = 16;
		type = frame[1];
	} else {
		return;
	}
	if (type == 2) {
		if (mhi_qrtr_hello_node(frame, length, modem)) {
			mhi_qrtr_hello_heard = true;
		}
		if (round == 0) {
			mhi_qrtr_frame("IPCR hello", frame, length);
		}
		return;
	}
	if (type != 4 || length < header + 20) {
		mhi_qrtr_frame("IPCR other", frame, length);
		return;
	}
	if (mhi_qrtr_first(mhi_qrtr_word(frame + header, 4))) {
		mhi_qrtr_frame(round == 0 ? "IPCR service" : "IPCR again", frame, length);
	}
	(void)mhi_qrtr_uim(frame, length, uim_node, uim_port);
	if (mhi_qrtr_server(frame, length, &service, &node, &port, &instance)) {
		mhi_qrtr_learn(service, instance, node, port);
	}
}

/* The result TLV (type 2) of a QMI answer, whichever message it answers. */
static void
mhi_qmi_outcome(const uint8_t *frame, size_t length, unsigned *result, unsigned *error)
{
	const uint8_t *qmi;
	unsigned header, at, body;

	*result = ~0u;
	*error = ~0u;
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return;
	}
	if (length < header + 7) {
		return;
	}
	qmi = frame + header;
	body = (unsigned)(length - header);
	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > body) {
			break;
		}
		if (type == 2 && size >= 4) {
			*result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
			*error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
		}
		at += 3 + size;
	}
}

/* What a QMI answer says about itself, whichever message it answers: its id,
 * and the result TLV every answer carries (type 2: result, then error). */
static void
mhi_qmi_result(const uint8_t *frame, size_t length, const char *what)
{
	const uint8_t *qmi;
	unsigned header, result, error;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return;
	}
	if (length < header + 7) {
		return;
	}
	mhi_qmi_outcome(frame, length, &result, &error);
	qmi = frame + header;
	hakios_tl("pcie2: AMSS: %s: message %#x, result %u, error %u\n", what,
	    qmi[3] | (unsigned)qmi[4] << 8, result, error);
}

/* The value of one TLV of a QMI answer (the frame with its QRTR header), or
 * NULL.  mhi_qmi_tlv above serves requests -- it insists on the request flag
 * -- and an answer carries 0x02 there. */
static const uint8_t *
mhi_qmi_answer_tlv(const uint8_t *frame, size_t length, unsigned type, size_t *size)
{
	const uint8_t *qmi;
	unsigned header, at, body;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return NULL;
	}
	if (length < header + 7) {
		return NULL;
	}
	qmi = frame + header;
	body = (unsigned)(length - header);
	for (at = 7; at + 3 <= body; ) {
		unsigned this_type = qmi[at];
		unsigned this_size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + this_size > body) {
			break;
		}
		if (this_type == type) {
			*size = this_size;
			return qmi + at + 3;
		}
		at += 3 + this_size;
	}
	return NULL;
}

/* A QMI request built in three steps: the seven-byte header {request, the
 * transaction, 0, the message id (two bytes), the body length (two bytes)},
 * the TLVs, and the length the last TLV settled. */
static size_t
mhi_qmi_open(uint8_t *request, unsigned txn, unsigned message)
{
	request[0] = 0x00;
	request[1] = (uint8_t)txn;
	/* Run 074446's probe asked with transactions 0x1300+n and the modem
	 * answered 6..31: this byte was pinned to 0, so only the low half went
	 * out.  Every other caller stays under 0x100 and keeps its wire bytes. */
	request[2] = (uint8_t)(txn >> 8);
	request[3] = (uint8_t)message;
	request[4] = (uint8_t)(message >> 8);
	request[5] = 0x00;
	request[6] = 0x00;
	return 7;
}

static size_t
mhi_qmi_put(uint8_t *request, size_t at, unsigned type, const void *value, size_t size)
{
	request[at] = (uint8_t)type;
	request[at + 1] = (uint8_t)size;
	request[at + 2] = (uint8_t)(size >> 8);
	if (size != 0) {
		memcpy(request + at + 3, value, size);
	}
	return at + 3 + size;
}

/* A little-endian u32 from one TLV of an answer (~0u when the TLV is
 * missing or too short). */
static unsigned
mhi_qmi_u32(const uint8_t *frame, size_t length, unsigned type)
{
	const uint8_t *value;
	size_t size = 0;

	value = mhi_qmi_answer_tlv(frame, length, type, &size);
	if (value == NULL || size < 4) {
		return ~0u;
	}
	return value[0] | (unsigned)value[1] << 8 | (unsigned)value[2] << 16 |
	    (unsigned)value[3] << 24;
}

static unsigned
mhi_qmi_u16(const uint8_t *frame, size_t length, unsigned type)
{
	const uint8_t *value;
	size_t size = 0;

	value = mhi_qmi_answer_tlv(frame, length, type, &size);
	if (value == NULL || size < 2) {
		return ~0u;
	}
	return value[0] | (unsigned)value[1] << 8;
}

/* Every TLV of an answer, by id and size, the first twelve bytes of each
 * value in hex: NAS's preference answer is 288 bytes of fields and only
 * the mode preference has ever been read out of it; the rest is the
 * record of what the host did and did not ask the modem for. */
static void
mhi_qmi_tlvs(const uint8_t *frame, size_t length, const char *what)
{
	const uint8_t *qmi;
	unsigned header, body, at;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return;
	}
	if (length < header + 7) {
		return;
	}
	qmi = frame + header;
	body = (unsigned)(length - header);
	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;
		char hex[3 * 12 + 1];
		unsigned shown;

		if (at + 3 + size > body) {
			break;
		}
		shown = size < 12 ? size : 12;
		for (unsigned i = 0; i < shown; i++) {
			snprintf(hex + i * 3, 4, " %02x", qmi[at + 3 + i]);
		}
		hex[shown * 3] = 0;
		hakios_tl("pcie2: cellular: %s: TLV %#04x %u bytes:%s%s\n",
		    what, type, size, hex, size > shown ? " ..." : "");
		at += 3 + size;
	}
}

static void
mhi_qmi_seal(uint8_t *request, size_t at)
{
	request[5] = (uint8_t)(at - 7);
	request[6] = (uint8_t)((at - 7) >> 8);
}

/* What the last card-status answer said, kept for the steps that act on it:
 * who the subscription's application is (0xffff: none -- run 041016), and
 * the AID of a READY (or only detected) USIM/SIM, in logical slot 1 and in
 * whichever slot was first found holding one. */
static struct {
	unsigned primary_gw;
	unsigned secondary_gw;
	unsigned logical_aid_len;
	uint8_t logical_aid[32];
	unsigned ready_slot;
	unsigned ready_aid_len;
	uint8_t ready_aid[32];
	unsigned card_present;	/* slots whose card is PRESENT in the last answer */
} mhi_uim_state;

/* Set when the dig-window sequence already pointed logical slot 1 at the
 * card; the card block that follows must not power-cycle it a second
 * time. */
static int mhi_uim_dig_switched;

/* UIM Get Card Status's answer (QMI message 0x2f), the card itself: the
 * result TLV and TLV 0x10 -- four 16-bit application indices, the slot
 * count, and per slot the card's state (0 absent, 1 present, 2 error), its
 * UPIN state and retries, PUK retries, error code and application count,
 * then per application its type (1 SIM, 2 USIM, 3 RUIM, 4 CSIM, 5 ISIM),
 * state (0 unknown, 1 detected, 2 PIN1 wanted, ..., 7 READY), its AID and
 * PIN1 state.  The layout is the one drivers/modem/hakios.mpss.c parses for
 * the other phone's modem. */
#include "hakios.uim_status.h"

static int
mhi_uim_card_status(const uint8_t *frame, size_t length)
{
	static const char *const cards[] = { "ABSENT", "PRESENT", "ERROR" };
	static const char *const states[] = { "unknown", "detected", "PIN1 wanted",
	    "PUK1 wanted", "personalization", "PIN1 blocked", "illegal", "READY" };
	const uint8_t *qmi, *status = NULL;
	size_t at, body, status_size = 0;
	unsigned header, result = ~0u, error = ~0u, slots;
	struct uim_inventory inventory;

	mhi_uim_state.primary_gw = UIM_NO_SESSION;
	mhi_uim_state.secondary_gw = UIM_NO_SESSION;
	mhi_uim_state.logical_aid_len = 0;
	mhi_uim_state.ready_slot = 0;
	mhi_uim_state.ready_aid_len = 0;
	mhi_uim_state.card_present = 0;
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return 0;
	}
	if (length < header + 7) {
		return 0;
	}
	qmi = frame + header;
	body = length - header;
	if ((qmi[3] | (unsigned)qmi[4] << 8) != 0x2f) {
		return 0;
	}
	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > body) {
			break;
		}
		if (type == 2 && size >= 4) {
			result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
			error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
		} else if (type == 0x10) {
			status = qmi + at + 3;
			status_size = size;
		}
		at += 3 + size;
	}
	hakios_tl("pcie2: AMSS: UIM card status: result %u, error %u\n", result, error);
	if (result != 0 || status == NULL || !uim_status_inventory(status, status_size, &inventory)) {
		return 0;
	}
	slots = inventory.slots;
	mhi_uim_state.primary_gw = inventory.primary_gw;
	mhi_uim_state.secondary_gw = inventory.secondary_gw;
	mhi_uim_state.card_present = inventory.present;
	hakios_tl("pcie2: AMSS: UIM inventory: %u detected slots, %u present, %u ready SIMs, "
	    "primary GW index %#x, secondary GW index %#x\n",
	    slots, inventory.present, inventory.ready, inventory.primary_gw, inventory.secondary_gw);
	hakios_tl("pcie2: AMSS: UIM: primary GW application %u (slot %u), %u slots\n",
	    status[0], status[1], slots);
	at = 9;
	for (unsigned slot = 0; slot < slots && at + 6 <= status_size; slot++) {
		unsigned card = status[at], applications = status[at + 5];

		hakios_tl("pcie2: AMSS: UIM slot %u: card %s (%u), UPIN state %u, error %u, %u applications\n",
		    slot + 1, card < 3 ? cards[card] : "?", card, status[at + 1],
		    status[at + 4], applications);
		at += 6;
		for (unsigned a = 0; a < applications && at + 7 <= status_size; a++) {
			unsigned type = status[at], state = status[at + 1];
			unsigned aid = status[at + 6], pin = at + 7 + aid;
			bool complete = at + 7 + aid <= status_size;
			bool ready = complete && card == 1 && (state == 7 || state == 1) &&
			    (type == 1 || type == 2) && aid <= 32;

			hakios_tl("pcie2: AMSS: UIM slot %u application %u: type %u, state %u (%s), PIN1 state %u, retries %u, AID %u bytes\n",
			    slot + 1, a, type, state, state < 8 ? states[state] : "?",
			    pin + 1 < status_size ? status[pin + 1] : 0,
			    pin + 2 < status_size ? status[pin + 2] : 0, complete ? aid : 0);
			if (ready) {
				if (mhi_uim_state.ready_slot == 0) {
					mhi_uim_state.ready_slot = slot + 1;
					mhi_uim_state.ready_aid_len = aid;
					memcpy(mhi_uim_state.ready_aid, status + at + 7, aid);
				}
				if (slot == 0) {
					mhi_uim_state.logical_aid_len = aid;
					memcpy(mhi_uim_state.logical_aid, status + at + 7, aid);
				}
			}
			at += 7 + aid + 7;
		}
	}
	return result == 0;	/* the card was described, not just answered */
}

/* The operating mode the DMS answer carries (TLV 1: 0 online, 1 low power,
 * 3 offline), with why it is offline (TLV 0x10) when it says; the mode, or
 * ~0 when the answer does not carry it. */
static unsigned
mhi_dms_mode(const uint8_t *frame, size_t length)
{
	static const char *const modes[] = { "ONLINE", "LOW POWER", "factory test",
	    "OFFLINE", "resetting", "shutting down", "persistent low power",
	    "mode-only low power", "network test" };
	const uint8_t *qmi;
	unsigned header, at, body, mode = ~0u, reason = ~0u;

	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return ~0u;
	}
	if (length < header + 7) {
		return ~0u;
	}
	qmi = frame + header;
	body = (unsigned)(length - header);
	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > body) {
			break;
		}
		if (type == 1 && size >= 1) {
			mode = qmi[at + 3];
		} else if (type == 0x10 && size >= 2) {
			reason = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
		}
		at += 3 + size;
	}
	hakios_tl("pcie2: AMSS: DMS: the modem's operating mode is %u (%s), offline reason %#x\n",
	    mode, mode < sizeof(modes) / sizeof(modes[0]) ? modes[mode] : "?", reason);
	return mode;
}

static size_t mhi_qrtr_answer(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *frame, size_t size, unsigned expect_port, unsigned expect_txn,
    unsigned expect_msg, unsigned timeout_ms, unsigned quiet_run, const char *what);

/* One QMI request to a service over the IPCR channel, framed the way libqmi
 * frames it: the size field is the message's own length and the frame is
 * padded to a multiple of four.  Run 041016 proved the distinction matters
 * on this AMSS: a size that counts the pad byte is answered result 1,
 * error 2 (NO_MEMORY) on every message, while the true size is answered.
 * The first DATA frame is returned in answer (its length, or 0). */
static size_t
mhi_qmi_call(struct mhi_channel *out, struct mhi_channel *in, unsigned src_port,
    unsigned node, unsigned port, const void *message, unsigned size, const char *what,
    uint8_t *answer, size_t answer_size)
{
	uint8_t frame[128] = { 0 };
	size_t length, got;

	length = mhi_qrtr_put3_sized(frame, 1, src_port, node, port, message, size,
	    (16u + size + 3u) & ~3u);
	mhi_qrtr_frame(what, frame, length);
	if (mhi_send(out, frame, length) != 0) {
		hakios_tl("pcie2: AMSS: %s: TX no completion\n", what);
		return 0;
	}
	got = mhi_qrtr_answer(out, in, answer, answer_size, src_port,
	    ((const uint8_t *)message)[1],
	    ((const uint8_t *)message)[3] |
	    (unsigned)((const uint8_t *)message)[4] << 8, 5000, 2, what);
	if (got != 0) {
		mhi_qmi_result(answer, got, what);
	} else {
		hakios_tl("pcie2: AMSS: %s: no answer\n", what);
	}
	return got;
}

/* The same call for a request whose answer waits on the network: the raw
 * send is answered only after the modem's SMS stack finishes with it, and
 * run 072221's answer came 9 s out -- past the 7 s of silence the plain call
 * allows.  Here the silence budget is 40 s (each empty read after the first
 * waits 2 s), so the answer is taken instead of being left for the next
 * poll. */
static size_t
mhi_qmi_call_slow(struct mhi_channel *out, struct mhi_channel *in, unsigned src_port,
    unsigned node, unsigned port, const void *message, unsigned size, const char *what,
    uint8_t *answer, size_t answer_size)
{
	uint8_t frame[128] = { 0 };
	size_t length, got;

	length = mhi_qrtr_put3_sized(frame, 1, src_port, node, port, message, size,
	    (16u + size + 3u) & ~3u);
	mhi_qrtr_frame(what, frame, length);
	if (mhi_send(out, frame, length) != 0) {
		hakios_tl("pcie2: AMSS: %s: TX no completion\n", what);
		return 0;
	}
	got = mhi_qrtr_answer(out, in, answer, answer_size, src_port,
	    ((const uint8_t *)message)[1],
	    ((const uint8_t *)message)[3] |
	    (unsigned)((const uint8_t *)message)[4] << 8, 5000, 20, what);
	if (got != 0) {
		mhi_qmi_result(answer, got, what);
	} else {
		hakios_tl("pcie2: AMSS: %s: no answer\n", what);
	}
	return got;
}

/* ---- the DSD settings stream (QMI service 0x2a): Android's IMS config ----
 *
 * Android's boot runs two qcrild clients against service 0x2a besides the
 * access-list conversation this driver already replays (mhi_dsd_arm): one
 * configuration stream and one status stream.  The 08:17 capture
 * (qcrild.trace) pins the peer down: the modem announces 0x2a instance 1 at
 * 3:0x3b, fd 99 looks 0x2a up, the NEW_SERVER names 0x3b, and the frames
 * below go there -- the 0x27 bind answers result 0, 0x38 result 0, and the
 * empty 0x24 comes back with the APN list ("cbnet", "CMNET", "CMWAP",
 * "ims", "SOS" in TLVs 0x11/0x13).  An earlier reading blamed service 0x44
 * for this stream; the same capture shows 0x44's real traffic (fd 49:
 * 0x20/0x22/0x23/0x28/0x2d, 3000 ms timers -- keepalive configuration),
 * and every one of these frames sent to 0x44 is refused error 57, so the
 * stream is replayed to 0x2a now.
 *
 * What it adds over mhi_dsd_arm: the 0x27 subscription bind first, 0x34,
 * 0x65, 0x6d, the 0x25 with TLV 0x13, the 0x38 feature words (0x15, 0x18,
 * 0x1a, then 0x1e/0x1f together), 0x47/0x4f, and 0x69.  Its 0x27 carries a
 * subscription (1 for the first instance, 2 for the second); both streams
 * bind 1 as the status socket of the capture does.  The 0x24/0x25/0x69/
 * 0x68 later in the run repeat what mhi_dsd_arm sends -- Android itself
 * repeats them across clients, and every one answers result 0.
 *
 * The frames are replayed byte for byte, one client per stream as Android
 * opens them: 0x100e for the configuration stream (fd 52 of the 09:43
 * capture, fd 99 of the 08:17 one) and 0x100f for the status stream (fd
 * 102).  Answers are logged as they come; a service that stops answering
 * ends a stream after three silent steps, so a wrong peer costs seconds,
 * not the run. */
static const struct {
	unsigned message, length;
	uint8_t tlvs[20];
} mhi_dsd_settings_config[] = {
	{ 0x0027, 7, { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0027, 7, { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0027, 7, { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0034, 7, { 0x12, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0065, 14, { 0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
	    0x12, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 } },
	{ 0x006d, 0, { 0 } },
	{ 0x0025, 4, { 0x13, 0x01, 0x00, 0x01 } },
	{ 0x0038, 4, { 0x15, 0x01, 0x00, 0x01 } },
	{ 0x0047, 0, { 0 } },
	{ 0x004f, 0, { 0 } },
	{ 0x0038, 4, { 0x18, 0x01, 0x00, 0x01 } },
	{ 0x0065, 7, { 0x10, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0069, 12, { 0x10, 0x01, 0x00, 0x01,
	    0x11, 0x01, 0x00, 0x01, 0x12, 0x01, 0x00, 0x01 } },
	{ 0x0068, 0, { 0 } },
	{ 0x0068, 0, { 0 } },
	{ 0x0038, 8, { 0x1e, 0x01, 0x00, 0x01, 0x1f, 0x01, 0x00, 0x01 } },
	{ 0x0068, 0, { 0 } },
};

static const struct {
	unsigned message, length;
	uint8_t tlvs[20];
} mhi_dsd_settings_status[] = {
	{ 0x0027, 7, { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 } },
	{ 0x0038, 12, { 0x15, 0x01, 0x00, 0x01,
	    0x18, 0x01, 0x00, 0x01, 0x1a, 0x01, 0x00, 0x01 } },
	{ 0x0024, 0, { 0 } },
	{ 0x0025, 0, { 0 } },
	{ 0x0047, 0, { 0 } },
	{ 0x0025, 4, { 0x10, 0x01, 0x00, 0x00 } },
	{ 0x0024, 0, { 0 } },
	{ 0x0025, 4, { 0x10, 0x01, 0x00, 0x01 } },
	{ 0x0024, 0, { 0 } },
};

/* Send one of the two captured streams once per boot.  The service's own
 * announcement is the peer; when this boot's table never learned it, the
 * name service is asked from both client ports first -- Android's clients
 * are introduced that way before they speak -- and the answer is read out of
 * the pump. */
static void
mhi_dsd_settings_client(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size)
{
	static const unsigned clients[2] = { 0x100e, 0x100f };
	uint8_t request[64], control[20], frame[64];
	unsigned node = 0, port = 0;
	size_t at, length, got;
	static bool done;

	if (done) {
		return;
	}
	done = true;
	mhi_qrtr_survey_dump("IPCR DSD settings");
	if (!mhi_qrtr_service(0x2a, &node, &port)) {
		for (unsigned i = 0; i < mhi_qrtr_all_count; i++) {
			if (mhi_qrtr_all[i].service == 0x2a) {
				node = mhi_qrtr_all[i].node;
				port = mhi_qrtr_all[i].port;
				break;
			}
		}
	}
	if (port == 0) {
		for (unsigned c = 0; c < 2; c++) {
			mhi_qrtr_control(control, 10, 0x2a);
			mhi_qrtr_put(control, 8, 1);
			length = mhi_qrtr_put3(frame, 10, clients[c],
			    0xffff, 0xfffe, control, sizeof(control));
			hakios_tl("pcie2: AMSS: IPCR DSD settings lookup: TX %s\n",
			    mhi_send(out, frame, length) == 0 ?
			    "completed" : "no completion");
		}
		(void)mhi_qrtr_pump(out, in, 800, "IPCR DSD settings lookup");
		if (!mhi_qrtr_service(0x2a, &node, &port)) {
			for (unsigned i = 0; i < mhi_qrtr_all_count; i++) {
				if (mhi_qrtr_all[i].service == 0x2a) {
					node = mhi_qrtr_all[i].node;
					port = mhi_qrtr_all[i].port;
					break;
				}
			}
		}
	}
	if (port == 0) {
		hakios_tl("pcie2: cellular: IMS: the DSD settings service (0x2a) is not announced; Android's configuration of it is skipped\n");
		return;
	}
	hakios_tl("pcie2: cellular: IMS: the DSD settings service (0x2a) is at %u:%#x; the configuration stream goes out on client %#x, the status stream on %#x\n",
	    node, port, clients[0], clients[1]);

	for (unsigned i = 0; i < sizeof(mhi_dsd_settings_config) /
	    sizeof(mhi_dsd_settings_config[0]); i++) {
		char what[48];
		static unsigned silent;

		at = mhi_qmi_open(request, 0x1600 + i,
		    mhi_dsd_settings_config[i].message);
		memcpy(request + at, mhi_dsd_settings_config[i].tlvs,
		    mhi_dsd_settings_config[i].length);
		at += mhi_dsd_settings_config[i].length;
		mhi_qmi_seal(request, at);
		snprintf(what, sizeof(what), "IPCR DSD settings %#x",
		    mhi_dsd_settings_config[i].message);
		got = mhi_qmi_call(out, in, clients[0], node, port, request,
		    (unsigned)at, what, answer, answer_size);
		if (got != 0) {
			unsigned result = ~0u, error = ~0u;

			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: the DSD settings configuration, step %u (message %#x): result %u, error %u\n",
			    i + 1, mhi_dsd_settings_config[i].message,
			    result, error);
			silent = 0;
		} else {
			hakios_tl("pcie2: cellular: IMS: the DSD settings configuration, step %u (message %#x): no answer\n",
			    i + 1, mhi_dsd_settings_config[i].message);
			if (++silent >= 3) {
				hakios_tl("pcie2: cellular: IMS: three configuration steps went unanswered; the rest are left out\n");
				silent = 0;
				break;
			}
		}
	}

	for (unsigned i = 0; i < sizeof(mhi_dsd_settings_status) /
	    sizeof(mhi_dsd_settings_status[0]); i++) {
		char what[48];
		static unsigned silent;

		at = mhi_qmi_open(request, 0x1700 + i,
		    mhi_dsd_settings_status[i].message);
		memcpy(request + at, mhi_dsd_settings_status[i].tlvs,
		    mhi_dsd_settings_status[i].length);
		at += mhi_dsd_settings_status[i].length;
		mhi_qmi_seal(request, at);
		snprintf(what, sizeof(what), "IPCR DSD settings status %#x",
		    mhi_dsd_settings_status[i].message);
		got = mhi_qmi_call(out, in, clients[1], node, port, request,
		    (unsigned)at, what, answer, answer_size);
		if (got != 0) {
			unsigned result = ~0u, error = ~0u;

			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: the DSD settings status, step %u (message %#x): result %u, error %u\n",
			    i + 1, mhi_dsd_settings_status[i].message,
			    result, error);
			silent = 0;
		} else {
			hakios_tl("pcie2: cellular: IMS: the DSD settings status, step %u (message %#x): no answer\n",
			    i + 1, mhi_dsd_settings_status[i].message);
			if (++silent >= 3) {
				hakios_tl("pcie2: cellular: IMS: three status steps went unanswered; the rest are left out\n");
				silent = 0;
				break;
			}
		}
	}
}

/* Android's own DMS opening, replayed byte for byte.  Captured from qcril's
 * DMS socket (port 0x040) while the stock system brought this same modem
 * online: Set Event Report with the operating-mode, PRL and 0x19 reports;
 * the vendor 0x0003 PIN/activation report; Get Capabilities; vendor 0x005e
 * with TLV 0x10; Set AP Version with "MIUIOS3.0.10.0.WPACNXM"; Get Operating
 * Mode; Get Manufacturer; and the second vendor 0x0003 with TLV 0x14/0x15.
 * Its DMS client is not merely opened before the mode change -- it is
 * configured this way in the first second of RILD, and only then does the
 * mode change go through.  Every step is sent whatever the last answered:
 * the point is to reproduce the opening, not to stop at the first refusal.
 * The operating mode the warm-up's own Get carries is returned (~0 when it
 * did not answer). */
static unsigned
mhi_dms_warmup(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	static const uint8_t event_report[] = {
		0x00, 0x90, 0x00, 0x01, 0x00, 0x0c, 0x00,
		0x14, 0x01, 0x00, 0x01, 0x17, 0x01, 0x00, 0x01,
		0x19, 0x01, 0x00, 0x01
	};
	static const uint8_t vendor_0003_pin[] = {
		0x00, 0x91, 0x00, 0x03, 0x00, 0x04, 0x00,
		0x12, 0x01, 0x00, 0x01
	};
	static const uint8_t capabilities[] = {
		0x00, 0x92, 0x00, 0x20, 0x00, 0x00, 0x00
	};
	static const uint8_t vendor_005e[] = {
		0x00, 0x93, 0x00, 0x5e, 0x00, 0x04, 0x00,
		0x10, 0x01, 0x00, 0x01
	};
	static const uint8_t ap_version[] = {
		0x00, 0x94, 0x00, 0x69, 0x00, 0x19, 0x00,
		0x11, 0x16, 0x00,
		'M', 'I', 'U', 'I', 'O', 'S', '3', '.', '0', '.', '1', '0',
		'.', '0', '.', 'W', 'P', 'A', 'C', 'N', 'X', 'M'
	};
	static const uint8_t get_mode[] = {
		0x00, 0x95, 0x00, 0x2d, 0x00, 0x00, 0x00
	};
	static const uint8_t get_manufacturer[] = {
		0x00, 0x96, 0x00, 0x21, 0x00, 0x00, 0x00
	};
	static const uint8_t vendor_0003_reports[] = {
		0x00, 0x97, 0x00, 0x03, 0x00, 0x08, 0x00,
		0x14, 0x01, 0x00, 0x01, 0x15, 0x01, 0x00, 0x01
	};
	static const struct {
		const uint8_t *message;
		unsigned size;
		const char *what;
	} steps[] = {
		{ event_report, sizeof(event_report),
		  "IPCR DMS warm-up: set event report" },
		{ vendor_0003_pin, sizeof(vendor_0003_pin),
		  "IPCR DMS warm-up: vendor 0x0003 (PIN report)" },
		{ capabilities, sizeof(capabilities),
		  "IPCR DMS warm-up: get capabilities" },
		{ vendor_005e, sizeof(vendor_005e),
		  "IPCR DMS warm-up: vendor 0x005e" },
		{ ap_version, sizeof(ap_version),
		  "IPCR DMS warm-up: set AP version MIUIOS3.0.10.0.WPACNXM" },
		{ get_mode, sizeof(get_mode),
		  "IPCR DMS warm-up: get operating mode" },
		{ get_manufacturer, sizeof(get_manufacturer),
		  "IPCR DMS warm-up: get manufacturer" },
		{ vendor_0003_reports, sizeof(vendor_0003_reports),
		  "IPCR DMS warm-up: vendor 0x0003 (reports)" }
	};
	unsigned mode = ~0u;

	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		size_t got = mhi_qmi_call(out, in, 0x1001, node, port,
		    steps[i].message, steps[i].size, steps[i].what, answer,
		    answer_size);

		if (got != 0 && steps[i].message == get_mode) {
			mode = mhi_dms_mode(answer, got);
		}
	}
	return mode;
}

/* The mode change, in the order Android's own client does it (captured from
 * qcril's DMS socket): LOW POWER first (0x002e, mode 1), then ONLINE
 * (0x002e, mode 0).  qcril also sends a vendor 0x0073/0x0074 pair between
 * the two, but that pair is out: run 233725 sent it with every attempt and
 * the modem's carrier-config scan stopped dead at mbn_sw.dig (six TFTP
 * reads, then nothing for the rest of the run), where runs 123149 and
 * 133235, which never sent it, read all 51 configs -- and with the pair
 * ONLINE was answered 52 (DEVICE_NOT_READY) where without it the answer was
 * 60, so the pair does move the modem's DMS state and may need a modem whose
 * boot is over.  Run 233725 is also why the caller leaves the modem alone
 * until its boot traffic is quiet.  The order here used to be ONLINE first
 * and LOW POWER only after a 52; every ONLINE request since run 112752 was
 * refused 52 -- twenty-three of them in run 115721 -- and every LOW POWER
 * after one of those 60 (INVALID_TRANSITION), which is what a request out of
 * order gets.  Android never asks ONLINE from the boot OFFLINE state; it
 * lowers first.  mhi_dms_warmup must have run before this.  The TLV is the
 * one the modem's own Get answer shows, type 1 length 1 value <mode>, in an
 * eleven-byte message whose length counts the pad byte.  Returns 1 when the
 * modem agreed (result 0) and the answer's error in *error_out otherwise. */
static int
mhi_dms_online(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, const char *what, uint8_t *answer,
    size_t answer_size, unsigned *error_out)
{
	uint8_t low[11] = {
		0x00, 0x00, 0x00, 0x2e, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x01
	};
	uint8_t set[11] = {
		0x00, 0x00, 0x00, 0x2e, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00
	};
	char low_what[128];
	unsigned result = ~0u, error = ~0u;
	size_t got;

	low[1] = (uint8_t)(transaction + 0x10);
	snprintf(low_what, sizeof(low_what), "%s: low power first", what);
	got = mhi_qmi_call(out, in, 0x1001, node, port, low, sizeof(low),
	    low_what, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got != 0 && result == 0) {
		(void)mhi_qrtr_pump(out, in, 2000, "after low power");
	} else {
		hakios_tl("pcie2: AMSS: DMS: LOW POWER refused (error %u); the mode change is asked anyway\n",
		    error);
	}
	/* No vendor pair here: see the comment above. */
	set[1] = (uint8_t)(transaction + 0x20);
	got = mhi_qmi_call(out, in, 0x1001, node, port, set, sizeof(set), what,
	    answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (error_out != NULL) {
		*error_out = error;
	}
	return got != 0 && result == 0;
}

/* Set the first time the DMS client setup (mhi_dms_warmup) runs, whichever
 * of the two places asks first: a fresh AMSS gets it once.  Cleared at the
 * start of every QRTR bring-up. */
static bool mhi_dms_warmed;

/* Set once the early WDS steering (mhi_wds_early: default profile 1, attach
 * list {1}) has been accepted, so the cellular poll's retries stop.  The
 * boot's staged calls go out regardless -- each is what proves whether the
 * modem has reverted the list.  Cleared at the start of every QRTR
 * bring-up. */
static bool mhi_wds_steered;

/* The first contact with DMS, made as soon as the nameservice has named its
 * port -- before the UIM rounds finish.  Stock's host asks DMS 2.2 s after
 * the AMSS takes over, reads mode 5 (SHUTTING_DOWN), lowers the radio to LOW
 * POWER 6.6 s in and asks ONLINE 2.6 s later; the request is accepted and the
 * modem is ONLINE 9.3 s after its Sahara.  When this host's first question
 * came only after the QMI0 retries and the UIM wait (run 030228: 55 s past
 * Sahara) the modem already sat in OFFLINE and answered LOW POWER 60, ONLINE
 * 52, forever -- the transitions this firmware accepts only exist while its
 * boot is still settling.  The warm-up is the client setup stock's qcril does
 * before its mode change; it runs once per boot, here or in the card-time
 * block.  Reading SHUTTING_DOWN, the LOW POWER request goes out before that
 * warm-up: run 044127 read 5, spent its round trips on the setup, read 3 and
 * was refused 60 -- the accepting state is a window, and it closed inside
 * the client setup.  Returns 1 when the modem then reads ONLINE. */
static int
mhi_dms_early(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	uint8_t get_mode[7] = { 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x00 };
	unsigned mode = ~0u, error = ~0u;
	bool lowered = false;
	size_t got;

	got = mhi_qmi_call(out, in, 0x1001, node, port, get_mode, sizeof(get_mode),
	    "IPCR DMS get operating mode (first contact)", answer, answer_size);
	if (got != 0) {
		mode = mhi_dms_mode(answer, got);
	}
	/* SHUTTING_DOWN: stock's radio-off arrives here and its LOW POWER is
	 * sent while this state still holds.  Ask before the warm-up's round
	 * trips, or the state passes and the request is refused 60. */
	if (mode == 5 && !mhi_dms_warmed) {
		uint8_t low[11] = {
			0x00, 0x31, 0x00, 0x2e, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x01
		};
		unsigned result = ~0u, low_error = ~0u;

		got = mhi_qmi_call(out, in, 0x1001, node, port, low, sizeof(low),
		    "IPCR DMS set operating mode (first contact): low power in the shutdown window",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &low_error);
		lowered = got != 0 && result == 0;
		if (lowered) {
			hakios_tl("pcie2: AMSS: DMS: LOW POWER accepted in the shutdown window\n");
		} else {
			hakios_tl("pcie2: AMSS: DMS: LOW POWER in the shutdown window refused (error %u)\n",
			    low_error);
		}
	}
	if (mode != 0 && !mhi_dms_warmed) {
		mhi_dms_warmed = true;
		mode = mhi_dms_warmup(out, in, node, port, answer, answer_size);
	}
	if (mode == 0) {
		return 1;
	}
	if (lowered) {
		uint8_t set[11] = {
			0x00, 0x41, 0x00, 0x2e, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00
		};
		unsigned result = ~0u, set_error = ~0u;

		got = mhi_qmi_call(out, in, 0x1001, node, port, set, sizeof(set),
		    "IPCR DMS set operating mode (first contact): online after the low power",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &set_error);
		if (got != 0 && result == 0) {
			return 1;
		}
		hakios_tl("pcie2: AMSS: DMS: ONLINE after the shutdown-window LOW POWER refused (error %u)\n",
		    set_error);
	}
	return mhi_dms_online(out, in, node, port, 0x21,
	    "IPCR DMS set operating mode (first contact)", answer, answer_size, &error);
}

/* GET_CARD_STATUS asked (up to tries times, three seconds apart) until the
 * card is described; 1 when it was. */
static int
mhi_uim_ask_status(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, unsigned tries, uint8_t *answer, size_t answer_size)
{
	int described = 0;

	for (unsigned t = 0; t < tries && !described; t++) {
		uint8_t request[7] = { 0x00, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00 };
		size_t got;

		request[1] = (uint8_t)(transaction + t);
		got = mhi_qmi_call(out, in, 0x1001, node, port, request, sizeof(request),
		    "IPCR UIM card status", answer, answer_size);
		if (got != 0) {
			described = mhi_uim_card_status(answer, got);
		}
		if (!described && t + 1 < tries) {
			delay(3000000);	/* three seconds for the card stack */
		}
	}
	return described;
}

/* Whether the UIM block moves the card (Switch Slot) and binds its USIM
 * to the primary GW session (Change Provisioning Session), the phone #1
 * remedy.  Android's own modem needs neither: with the card in slot 2 it
 * reads slot 1 ERROR / slot 2 READY, primary GW application 65535, and
 * every qcrild WDS client dials on subscription 2 (docs/cellular.md, the
 * live baseline).  After HakiOS's switch the card is on subscription 1:
 * a Start there passes the CM ACL (run 021620: the refusal changes from
 * verbose type 3 / 1042 to type 2 / internal 208) while every client
 * bound to subscription 2 is still ACL-denied.  pcie2_uiswitch=0 keeps
 * the card where the modem found it -- Android's posture -- so a run can
 * test the working baseline's combination; the default keeps the phone
 * #1 remedy. */

/* The subscription the SIM answers on.  2 (secondary) while the card sits
 * in the slot the modem found it in; once the UIM block's switch moves the
 * card to logical slot 1 and Change Provisioning Session makes it the
 * primary GW application, the card is subscription 1 and every WDS client
 * that needs it must bind that.  Run 021620 proved the CM ACL is per
 * subscription: after the switch a Start under 2 got verbose type 3 /
 * 1042, one under 1 got past the ACL (type 2 / internal 208). */
static unsigned mhi_wds_card_sub = 2;

static int
mhi_uim_switch_allowed(void)
{
	int on = 1;

	PE_parse_boot_argn("pcie2_uiswitch", &on, sizeof(on));
	return on != 0 && mhi_uim_state.card_present <= 1;
}

/* UIM Switch Slot: TLV 1 the logical slot, TLV 2 the physical one (32 bits),
 * both numbered from 1.  The answer comes fast; the card it moves powers down
 * and initializes again, which the wait below covers. */
static int
mhi_uim_switch_slot(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, unsigned physical)
{
	uint8_t sw[18] = {
		0x00, 0x00, 0x00, 0x46, 0x00, 0x0b, 0x00,
		0x01, 0x01, 0x00, 0x01,
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	uint8_t answer[512];

	if (!mhi_uim_switch_allowed()) {
		hakios_tl("pcie2: AMSS: UIM: single-SIM slot switching disabled: preserving the card in slot %u\n",
		    physical);
		return 0;
	}
	sw[1] = (uint8_t)transaction;
	sw[14] = (uint8_t)physical;
	hakios_tl("pcie2: AMSS: UIM: LOGICAL SLOT 1 IS POINTED AT SLOT %u, where the card is ready\n",
	    physical);
	return mhi_qmi_call(out, in, 0x1001, node, port, sw, sizeof(sw),
	    "IPCR UIM switch slot", answer, sizeof(answer)) != 0;
}

/* UIM Change Provisioning Session: TLV 1 the session change (type 0 primary
 * GW, activate 1), TLV 0x10 logical slot 1 and the application's AID -- the
 * reference's request, byte for byte. */
static int
mhi_uim_provision(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, uint8_t *answer, size_t answer_size)
{
	uint8_t pr[17 + 32];
	unsigned aid = mhi_uim_state.logical_aid_len;

	if (!mhi_uim_switch_allowed()) {
		hakios_tl("pcie2: AMSS: UIM: single-SIM provisioning disabled: preserving the primary GW application\n");
		return 0;
	}
	if (aid == 0 || aid > 32) {
		return 0;
	}
	memset(pr, 0, sizeof(pr));
	pr[0] = 0x00; pr[1] = (uint8_t)transaction; pr[2] = 0x00;
	pr[3] = 0x38; pr[4] = 0x00;
	pr[5] = (uint8_t)(10 + aid); pr[6] = 0x00;
	pr[7] = 0x01; pr[8] = 0x02; pr[9] = 0x00;
	pr[10] = 0x00; pr[11] = 0x01;
	pr[12] = 0x10; pr[13] = (uint8_t)(2 + aid); pr[14] = 0x00;
	pr[15] = 0x01; pr[16] = (uint8_t)aid;
	memcpy(pr + 17, mhi_uim_state.logical_aid, aid);
	hakios_tl("pcie2: AMSS: UIM: THE USIM OF LOGICAL SLOT 1 IS MADE THE PRIMARY GW APPLICATION (%u-byte AID)\n",
	    aid);
	if (mhi_qmi_call(out, in, 0x1001, node, port, pr, 17 + aid,
	    "IPCR UIM change provisioning", answer, answer_size) == 0) {
		return 0;
	}
	/* The card is the primary GW application now: the subscription the
	 * WDS clients must bind is 1, the primary (run 021620: a Start on it
	 * passes the CM ACL). */
	mhi_wds_card_sub = 1;
	hakios_tl("pcie2: cellular: the provisioning took: the WDS clients bind subscription %u from here on\n",
	    mhi_wds_card_sub);
	return 1;
}

/* The card after a slot switch.  Run 123149 read the status two seconds after
 * the switch and found both logical slots ABSENT -- the card powering down to
 * initialize again -- and that single re-read was the last one ever made, so
 * the AID the provisioning needs was never seen and the subscription stayed
 * "none" (0xffff).  The status is asked here every five seconds, tries times,
 * until an application or a primary GW session shows; if the mapping reverts
 * (a READY card first found in another slot again), the switch is sent again,
 * at most three in all.  Returns the last status parse. */
static int
mhi_uim_wait_ready(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, unsigned tries, uint8_t *answer,
    size_t answer_size)
{
	int described = 0;
	unsigned switches = 0;

	for (unsigned t = 0; t < tries; t++) {
		delay(5000000);
		described = mhi_uim_ask_status(out, in, node, port,
		    transaction + t, 1, answer, answer_size);
		if (described && (mhi_uim_state.logical_aid_len != 0 ||
		    mhi_uim_state.primary_gw != 0xffff)) {
			break;
		}
		if (described && mhi_uim_state.primary_gw == 0xffff &&
		    mhi_uim_state.logical_aid_len == 0 &&
		    mhi_uim_state.ready_slot > 1 && switches < 2) {
			hakios_tl("pcie2: AMSS: UIM: the card is first found in slot %u again; the switch is sent again\n",
			    mhi_uim_state.ready_slot);
			(void)mhi_uim_switch_slot(out, in, node, port,
			    transaction + 0x10, mhi_uim_state.ready_slot);
			switches++;
		}
	}
	return described;
}

/* The next DATA frame from the IPCR in channel that is addressed to
 * expect_port, skipping the announcements and any other traffic that may
 * still be queued behind the request; returns its length, or 0.  Once the
 * host's services are announced, the modem's own requests to them travel the
 * same channel (the IPCR pair carries every QRTR service), so a data frame
 * for another port is not this request's answer and is logged apart.  The
 * first receive waits timeout_ms, the rest two seconds each, and the drain
 * ends after quiet_run timeouts in a row (or 48 frames). */
static size_t
mhi_qrtr_answer(struct mhi_channel *out, struct mhi_channel *in, uint8_t *frame,
    size_t size, unsigned expect_port, unsigned expect_txn, unsigned expect_msg,
    unsigned timeout_ms, unsigned quiet_run, const char *what)
{
	/* a whole datagram, so a TFTP write is taken, not left unread
	 * (mhi_receive refuses one bigger than the buffer and keeps it) */
	static uint8_t heard[MHI_PACKET];
	unsigned quiet = 0, frames = 0;

	while (quiet < quiet_run && frames < 48) {
		size_t got = sizeof(heard);
		unsigned header, dst_port, src_node, src_port;
		bool confirm;
		int result;

		(void)mhi_efs_service();
		result = mhi_receive(in, heard, &got, frames == 0 ? timeout_ms : 2000);

		if (result) {
			quiet++;
			continue;
		}
		quiet = 0;
		frames++;
		if (got >= 32 && mhi_qrtr_word(heard, 0) == 1) {
			header = 32;
			dst_port = mhi_qrtr_word(heard, 28);
			src_node = mhi_qrtr_word(heard, 8);
			src_port = mhi_qrtr_word(heard, 12);
			confirm = mhi_qrtr_word(heard, 16) != 0;
		} else if (got >= 16) {
			header = 16;
			dst_port = heard[14] | (unsigned)heard[15] << 8;
			src_node = heard[8] | (unsigned)heard[9] << 8;
			src_port = heard[10] | (unsigned)heard[11] << 8;
			confirm = (heard[2] & 1) != 0;
		} else {
			header = 0;
			dst_port = ~0u;
			src_node = 0;
			src_port = 0;
			confirm = false;
		}
		if (confirm) {
			mhi_qrtr_resume(out, dst_port, src_node, src_port);
		}
		if (mhi_qrtr_type(heard, got) == 1) {
			if (mhi_host_service(dst_port) >= 0) {
				const uint8_t *qmi = heard + header;
				int service = mhi_host_service(dst_port);

				if (mhi_host_services[service].service == 4096) {
					mhi_tftp_request(out, heard, got, header, what);
					continue;
				}
				mhi_qrtr_frame("IPCR host request", heard, got);
				if (got >= header + 7) {
					hakios_tl("pcie2: AMSS: %s: the host's %s asked "
					    "(message %#x, %u bytes) while %s waited\n",
					    what, mhi_host_services[service].name,
					    qmi[3] | (unsigned)qmi[4] << 8,
					    qmi[5] | (unsigned)qmi[6] << 8, what);
				}
				if (mhi_host_services[service].service == 64 && got >= header + 7) {
					mhi_servreg_answer(out, heard, got, header, what);
				}
				if (got >= header + 7 &&
				    (mhi_host_services[service].service == 0x302 ||
				    mhi_host_services[service].service == 0x28)) {
					mhi_imsdcm_answer(out, heard, got, header,
					    mhi_host_services[service].port, what);
				}
				continue;
			}
			if (mhi_tftp_ack(out, heard, got, header, dst_port, what)) {
				continue;
			}
			if (dst_port == expect_port) {
				/* An indication is not this request's answer.  The DMS
				 * warm-up turns event reporting on, and an indication
				 * that arrived between a request and its answer would
				 * otherwise be read as the answer.  Logged by name:
				 * a mode change the modem reports on its own is
				 * evidence of what a mode request did. */
				if (got >= header + 7 && (heard[header] & 0x06) == 0x04) {
					hakios_tl("pcie2: AMSS: %s: the modem's own "
					    "indication %#x (%u bytes) while it waited\n",
					    what, heard[header + 3] |
					    (unsigned)heard[header + 4] << 8,
					    (unsigned)(got - header));
					mhi_qrtr_frame("IPCR indication", heard, got);
					if (mhi_qrtr_from(src_node, src_port, 0x05)) {
						mhi_sms_note(heard + header,
						    got - header, what);
					}
					mhi_voice_note(heard + header,
					    got - header, what);
					mhi_imsa_note(heard, got, what);
					continue;
				}
				if (got < header + 7 ||
				    heard[header + 1] != expect_txn ||
				    (heard[header + 3] |
				    (unsigned)heard[header + 4] << 8) != expect_msg) {
					/* A late answer to another request.  Run 072221:
					 * the raw send's answer came 9 s after it went
					 * out; this call had given up at 7 s and the
					 * next NAS poll took the answer for its own
					 * (result 1, error 56 out of a NAS call). */
					mhi_qrtr_frame("IPCR a late answer elsewhere",
					    heard, got);
					continue;
				}
				mhi_qrtr_frame(what, heard, got);
				if (got > size) {
					hakios_tl("pcie2: AMSS: %s: a %zu-byte answer, "
					    "not the %zu there was room for\n", what, got, size);
					return 0;
				}
				memcpy(frame, heard, got);
				return got;
			}
			/* The IMSA indications come to 0x1004 while the wait
			 * is on another port; without this the registration
			 * word they carry is only a hex dump ("IPCR data
			 * elsewhere", runs through 123850). */
			mhi_imsa_note(heard, got, what);
			mhi_qrtr_frame("IPCR data elsewhere", heard, got);
			continue;
		}
		if (mhi_qrtr_type(heard, got) == 4) {
			/* The modem restates its whole service table on every
			 * hello, and those restatements arrive here -- while a
			 * request waits for its answer -- far more often than in
			 * the quiet pumps.  Run 064118 logged IMS (0x12 at
			 * 3:0x49) and IMSA (0x21 at 3:0x47) announced 227 times
			 * each and never remembered either: every one was
			 * dropped here, so the IMS bring-up read "not announced"
			 * while the table was being restated around it.  The
			 * announcements carry the only ports those two services
			 * have; remembering them here is what lets the IMS
			 * settings (0x12) and the registration question (0x21)
			 * go out at all. */
			unsigned service, node, port, instance;

			if (mhi_qrtr_server(heard, got, &service, &node, &port,
			    &instance)) {
				mhi_qrtr_learn(service, instance, node, port);
			}
		}
		mhi_qrtr_frame("IPCR other", heard, got);
	}
	return 0;
}

/* ---- PDC: the modem's per-boot configuration, read but never written ----
 *
 * Service 0x24 (Persistent Device Configuration).  Xiaomi's qcril drives it
 * at every boot -- its log names pdc_get_config_lists_by_type returning 0
 * and 99 software configurations -- and the CM access list that denies
 * every Start Network is part of the configuration PDC selects.  HakiOS has
 * never sent a single PDC frame.  The QMI shapes are libqmi's
 * qmi-service-pdc.json: Get Selected Config (0x22) takes Config Type TLV
 * 0x01 and Token TLV 0x10 (both guint32) and puts the Active Id in
 * indication TLV 0x11 and the Pending Id in 0x12 (u8-prefixed byte
 * arrays); List Configs (0x24) takes Token 0x10 and Config Type 0x11 and
 * puts a page of configs in indication TLV 0x11 (a u8 count, then {u32
 * config type, u8 id length, id} entries).  Every message sent here is a
 * Get or a List; Set Selected Config (0x23) is not used.
 */

/* A PDC id, a u8-prefixed byte array: the first byte is the count.  The
 * whole id is logged: a platform id is a path (the selected one is 81
 * bytes under /data/vendor/...) and a software id is a 20-byte binary
 * config id that must be compared byte for byte. */
static void
mhi_pdc_id(const char *what, const char *which, const uint8_t *value, size_t size)
{
	char hex[3 * 96 + 1];
	unsigned n, shown;

	if (value == NULL || size < 1) {
		hakios_tl("pcie2: cellular: PDC: %s: no %s id in the indication\n",
		    what, which);
		return;
	}
	n = value[0];
	if (n > size - 1) {
		n = (unsigned)(size - 1);
	}
	shown = n < 96 ? n : 96;
	for (unsigned i = 0; i < shown; i++) {
		snprintf(hex + i * 3, 4, " %02x", value[1 + i]);
	}
	hex[shown * 3] = 0;
	hakios_tl("pcie2: cellular: PDC: %s: the %s id is %u bytes:%s%s\n", what,
	    which, n, hex, n > shown ? " ..." : "");
}

/* The software config ids List Configs returned, kept for Get Config Info
 * (0x28), whose Description TLV 0x12 names each one -- the only way to
 * learn which MBN a 20-byte hash id stands for.  The pool is 25 configs
 * on this device. */
static uint8_t mhi_pdc_sw_ids[32][24];
static uint8_t mhi_pdc_sw_lengths[32];
static char mhi_pdc_sw_names[32][64];
static uint8_t mhi_pdc_sw_active[24];
static uint8_t mhi_pdc_sw_active_len;
static unsigned mhi_pdc_sw_count;
/* The one causal experiment with the selection, and its undo: select and
 * activate the CMCC config (index 1, "Volte_OpenMkt-Commercial-CMCC"),
 * let one start matrix run under it, then select the config the modem
 * booted with again.  Both ids are copied out here because the re-read
 * overwrites mhi_pdc_sw_active. */
static uint8_t mhi_pdc_cbn_id[24];
static uint8_t mhi_pdc_cbn_len;
static int mhi_pdc_experiment_selected;
static int mhi_pdc_experiment_restored;
/* pcie2_pdc_keep: the CMCC selection stays for the whole boot.  The
 * modem's own config here is index 0, "Volte_OpenMkt-Commercial-CBN"
 * (China Broadnet), while the card is CMCC; Android's working ask comes
 * under the CMCC MBN.  With the selection kept, the IMS stack's bind,
 * ready, ask and IMSA registration all run under the carrier's config. */
static int mhi_pdc_experiment_kept;

/* A page of List Configs: u8 count, then {guint32 config type, u8 id
 * length, id} entries -- one indication carries as many as fit 255 bytes,
 * so a long list arrives as several indications. */
static void
mhi_pdc_configs(const char *what, const uint8_t *value, size_t size)
{
	unsigned count, at = 1, i;

	if (value == NULL || size < 1) {
		hakios_tl("pcie2: cellular: PDC: %s: no config list in the indication\n",
		    what);
		return;
	}
	count = value[0];
	hakios_tl("pcie2: cellular: PDC: %s: this indication lists %u configs\n",
	    what, count);
	for (i = 0; i < count; i++) {
		unsigned type, id_len, shown;
		char hex[3 * 32 + 1];

		if (at + 5 > size) {
			break;
		}
		type = value[at] | (unsigned)value[at + 1] << 8 |
		    (unsigned)value[at + 2] << 16 | (unsigned)value[at + 3] << 24;
		id_len = value[at + 4];
		at += 5;
		if (id_len > size - at) {
			break;
		}
		if (type == 1 && id_len <= 24 && mhi_pdc_sw_count < 32) {
			memcpy(mhi_pdc_sw_ids[mhi_pdc_sw_count], value + at,
			    id_len);
			mhi_pdc_sw_lengths[mhi_pdc_sw_count] = (uint8_t)id_len;
			mhi_pdc_sw_count++;
		}
		shown = id_len < 32 ? id_len : 32;
		for (unsigned j = 0; j < shown; j++) {
			snprintf(hex + j * 3, 4, " %02x", value[at + j]);
		}
		hex[shown * 3] = 0;
		hakios_tl("pcie2: cellular: PDC: %s: config %u type %u id %u bytes:%s%s\n",
		    what, i, type, id_len, hex, id_len > shown ? " ..." : "");
		at += id_len;
	}
}

/* Case-insensitive substring test against the config names Get Config
 * Info returns; the needle is lowercase. */
static int
mhi_pdc_name_has(const char *name, const char *needle)
{
	size_t nlen = strlen(needle), i;

	for (i = 0; name[i] != 0; i++) {
		size_t j = 0;

		while (j < nlen && name[i + j] != 0) {
			char a = name[i + j];

			if (a >= 'A' && a <= 'Z') {
				a = (char)(a - 'A' + 'a');
			}
			if (a != needle[j]) {
				break;
			}
			j++;
		}
		if (j == nlen) {
			return 1;
		}
	}
	return 0;
}

/* One PDC indication of the requested message id.  Get Selected Config
 * (0x22) puts Active Id in TLV 0x11 and Pending Id in 0x12; List Configs
 * (0x24) puts its page in TLV 0x11; Get Config Info (0x28) puts the total
 * size in 0x11, a length-prefixed Description string in 0x12 and a version
 * in 0x13.  All carry the indication result in TLV 0x01 (guint16) and the
 * token in TLV 0x10 (guint32). */
static void
mhi_pdc_indication(const uint8_t *qmi, size_t body, const char *what,
    unsigned message)
{
	const uint8_t *active = NULL, *pending = NULL, *configs = NULL;
	const uint8_t *description = NULL;
	size_t active_size = 0, pending_size = 0, configs_size = 0;
	size_t description_size = 0;
	unsigned at, result = ~0u, token = ~0u, total = ~0u, version = ~0u;

	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at], size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > body) {
			break;
		}
		if (type == 0x01 && size >= 2) {
			result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
		} else if (type == 0x10 && size >= 4) {
			token = qmi[at + 3] | (unsigned)qmi[at + 4] << 8 |
			    (unsigned)qmi[at + 5] << 16 |
			    (unsigned)qmi[at + 6] << 24;
		} else if (type == 0x11) {
			if (message == 0x24) {
				configs = qmi + at + 3;
				configs_size = size;
			} else if (message == 0x28) {
				if (size >= 4) {
					total = qmi[at + 3] |
					    (unsigned)qmi[at + 4] << 8 |
					    (unsigned)qmi[at + 5] << 16 |
					    (unsigned)qmi[at + 6] << 24;
				}
			} else {
				active = qmi + at + 3;
				active_size = size;
			}
		} else if (type == 0x12) {
			if (message == 0x28) {
				description = qmi + at + 3;
				description_size = size;
			} else if (message != 0x24) {
				pending = qmi + at + 3;
				pending_size = size;
			}
		} else if (type == 0x13 && message == 0x28 && size >= 4) {
			version = qmi[at + 3] | (unsigned)qmi[at + 4] << 8 |
			    (unsigned)qmi[at + 5] << 16 |
			    (unsigned)qmi[at + 6] << 24;
		}
		at += 3 + size;
	}
	hakios_tl("pcie2: cellular: PDC: %s: the indication says result %u, token %#x\n",
	    what, result, token);
	if (message == 0x24) {
		mhi_pdc_configs(what, configs, configs_size);
	} else if (message == 0x28) {
		char text[97];
		unsigned i, len = 0;

		if (description != NULL && description_size >= 1) {
			len = description[0];
			if (len > description_size - 1) {
				len = (unsigned)(description_size - 1);
			}
			if (len > 96) {
				len = 96;
			}
			for (i = 0; i < len; i++) {
				unsigned char c = description[1 + i];

				text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
			}
		}
		text[len] = 0;
		hakios_tl("pcie2: cellular: PDC: %s: size %u, version %#x, description \"%s\"%s\n",
		    what, total, version, text, len >= 96 ? " ..." : "");
		/* The token is the config index the request used
		 * (0x5b5b0000 + i), so the name lands in its slot. */
		if (token >= 0x5b5b0000 && token < 0x5b5b0000 + 32) {
			snprintf(mhi_pdc_sw_names[token - 0x5b5b0000],
			    sizeof(mhi_pdc_sw_names[0]), "%s", text);
		}
	} else {
		mhi_pdc_id(what, "active", active, active_size);
		mhi_pdc_id(what, "pending", pending, pending_size);
		/* A software active id is at most 24 bytes; the platform
		 * one is an 81-byte path, which this keeps out. */
		if (active != NULL && active_size >= 1 && active[0] <= 24) {
			mhi_pdc_sw_active_len = active[0];
			memcpy(mhi_pdc_sw_active, active + 1,
			    mhi_pdc_sw_active_len);
		}
	}
}

/* PDC's answer does not fit the generic call: the request is acknowledged
 * with an Operation Result and the data arrives in a separate indication
 * of the same message id, so a call that returns at the first answer would
 * never see it.  This exchange drains a fixed window: it logs the
 * acknowledgement, parses every indication of expect_msg, and keeps
 * serving the rest of the channel meanwhile (the modem's TFTP scan does
 * not stop for us).  Read-only -- every message sent through here is a Get
 * or a List.  The client port is 0x1005. */
static void
mhi_pdc_exchange(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, const uint8_t *request, unsigned size, const char *what,
    unsigned expect_msg, unsigned budget_ms)
{
	static uint8_t heard[MHI_PACKET];
	uint8_t frame[64] = { 0 };
	size_t length;
	unsigned waited = 0, frames = 0, indications = 0;
	bool acked = false;

	length = mhi_qrtr_put3_sized(frame, 1, 0x1005, node, port, request, size,
	    (16u + size + 3u) & ~3u);
	mhi_qrtr_frame(what, frame, length);
	if (mhi_send(out, frame, length) != 0) {
		hakios_tl("pcie2: AMSS: %s: TX no completion\n", what);
		return;
	}
	while (waited < budget_ms && frames < 64) {
		size_t got = sizeof(heard);
		unsigned header, dst_port, src_node, src_port;
		bool confirm;
		int service;

		(void)mhi_efs_service();
		if (mhi_receive(in, heard, &got, 200) != 0) {
			waited += 200;
			continue;
		}
		frames++;
		if (got >= 32 && mhi_qrtr_word(heard, 0) == 1) {
			header = 32;
			dst_port = mhi_qrtr_word(heard, 28);
			src_node = mhi_qrtr_word(heard, 8);
			src_port = mhi_qrtr_word(heard, 12);
			confirm = mhi_qrtr_word(heard, 16) != 0;
		} else if (got >= 16) {
			header = 16;
			dst_port = heard[14] | (unsigned)heard[15] << 8;
			src_node = heard[8] | (unsigned)heard[9] << 8;
			src_port = heard[10] | (unsigned)heard[11] << 8;
			confirm = (heard[2] & 1) != 0;
		} else {
			continue;
		}
		if (confirm) {
			mhi_qrtr_resume(out, dst_port, src_node, src_port);
		}
		if (mhi_qrtr_type(heard, got) != 1) {
			mhi_qrtr_frame("IPCR control", heard, got);
			if (mhi_qrtr_type(heard, got) == 4) {
				unsigned service_id, s_node, s_port, s_instance;

				if (mhi_qrtr_server(heard, got, &service_id,
				    &s_node, &s_port, &s_instance)) {
					mhi_qrtr_learn(service_id, s_instance,
					    s_node, s_port);
				}
			}
			continue;
		}
		service = mhi_host_service(dst_port);
		if (service >= 0) {
			if (mhi_host_services[service].service == 4096) {
				mhi_tftp_request(out, heard, got, header, what);
				continue;
			}
			mhi_qrtr_frame("IPCR host request", heard, got);
			if (mhi_host_services[service].service == 64) {
				mhi_servreg_answer(out, heard, got, header, what);
			}
			/* "Keeps serving the rest of the channel" includes the
			 * IMS data service: in run 191905 the IMS stack's first
			 * bind (txn 10) and ask (txn 12) fell inside this PDC
			 * window at 19:21:27 and were only logged -- the stack
			 * destroyed instance 1 0.7 s later, unanswered, and
			 * bound again only 7 s after.  Same dispatch as
			 * mhi_qrtr_pump. */
			if (got >= header + 7 &&
			    (mhi_host_services[service].service == 0x302 ||
			    mhi_host_services[service].service == 0x28)) {
				mhi_imsdcm_answer(out, heard, got, header,
				    mhi_host_services[service].port, what);
			}
			continue;
		}
		if (mhi_tftp_ack(out, heard, got, header, dst_port, what)) {
			continue;
		}
		if (dst_port != 0x1005 || got < header + 7 ||
		    (heard[header + 3] |
		    (unsigned)heard[header + 4] << 8) != expect_msg) {
			mhi_qrtr_frame("IPCR data elsewhere", heard, got);
			continue;
		}
		if ((heard[header] & 0x06) == 0x04) {
			indications++;
			mhi_qrtr_frame("IPCR PDC indication", heard, got);
			mhi_pdc_indication(heard + header, got - header, what,
			    expect_msg);
			continue;
		}
		if (heard[header + 1] == request[1]) {
			unsigned result = ~0u, error = ~0u;

			acked = true;
			mhi_qrtr_frame(what, heard, got);
			mhi_qmi_outcome(heard, got, &result, &error);
			hakios_tl("pcie2: cellular: PDC: %s: the acknowledgement says result %u, error %u\n",
			    what, result, error);
			continue;
		}
		mhi_qrtr_frame("IPCR a late answer elsewhere", heard, got);
	}
	if (!acked) {
		hakios_tl("pcie2: cellular: PDC: %s: no acknowledgement in the window\n",
		    what);
	}
	if (indications == 0) {
		hakios_tl("pcie2: cellular: PDC: %s: no indication in the window\n",
		    what);
	}
}

/* One QMI GET_VERSION to one service, sent the way libqmi sends it -- a
 * seven-byte message in a padded frame whose size says 7 -- and drained
 * like the card question.  The answer's result goes to the log. */
static void
mhi_qmi_get_version(struct mhi_channel *out, struct mhi_channel *in, unsigned src_port,
    unsigned node, unsigned port, const char *what, uint8_t *answer, size_t answer_size)
{
	static const uint8_t request[8] = {
		0x00, 0x02, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00
	};
	uint8_t frame[64] = { 0 };
	size_t length, got;
	int error;

	length = mhi_qrtr_put3_sized(frame, 1, src_port, node, port, request, 7, 24);
	mhi_qrtr_frame(what, frame, length);
	error = mhi_send(out, frame, length);
	hakios_tl("pcie2: AMSS: %s: TX %s\n", what,
	    error == 0 ? "completed" : "no completion");
	if (error) {
		return;
	}
	got = mhi_qrtr_answer(out, in, answer, answer_size, src_port,
	    request[1], request[3] | (unsigned)request[4] << 8, 5000, 2, what);
	if (got != 0) {
		mhi_qmi_result(answer, got, what);
	} else {
		hakios_tl("pcie2: AMSS: %s: no answer\n", what);
	}
}

/* ---- kern.hakios.cellular_*: the modem's network state for the system ----
 *
 * userland/src/inject_sb.c draws the status bar's cellular item from these
 * sysctls, the names the legacy driver publishes (drivers/modem/hakios.mpss.c).
 * NAS is the source, and hakios.nas.c is the parser the legacy driver used:
 * GET_SERVING_SYSTEM (0x24) names the registration, its PLMN and operator,
 * GET_SIGNAL_INFO (0x4f) the signal.  The answers are polled every two
 * seconds from a thread of its own once the probe is done; the pumps between
 * polls keep serving the modem's TFTP, registry and EFS traffic, which the
 * boot keeps asking for after the probe has finished.
 */
static struct hakios_nas_state cellular_nas;
static int cellular_up, cellular_registered, cellular_bars, cellular_dbm, cellular_radio;
static char cellular_operator[40];
static volatile bool cellular_wanted;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_up, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_up, 0,
    "1: the modem's NAS service is there");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_registered, 0,
    "1: registered with a network");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_bars, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_bars, 0,
    "signal, 0 to 4 bars");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_dbm, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_dbm, 0,
    "signal, dBm; 0: none read");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_radio, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_radio, 0,
    "4 GSM, 5 UMTS, 8 LTE, 12 NR");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_operator, CTLFLAG_RD | CTLFLAG_LOCKED,
    cellular_operator, 0, "the serving network's name");

static void
mhi_cellular_publish(void)
{
	cellular_registered = cellular_nas.registered;
	cellular_bars = cellular_nas.bars;
	cellular_dbm = cellular_nas.has_signal ? cellular_nas.dbm : 0;
	cellular_radio = cellular_nas.radio_interface;
	snprintf(cellular_operator, sizeof(cellular_operator), "%s",
	    cellular_nas.registered ? cellular_nas.operator_name : "");
	hakios_tl("pcie2: cellular: %s, %u bars, %d dBm (radio %u), operator \"%s\" (MCC %u MNC %u)\n",
	    cellular_registered ? "REGISTERED WITH A NETWORK" : "not registered",
	    cellular_bars, cellular_dbm, cellular_radio, cellular_operator,
	    cellular_nas.mcc, cellular_nas.mnc);
}

/* Ask NAS once: serving system, then signal.  hakios_nas_apply wants the QMI
 * payload alone, so the transport header is skipped and the body is cut to
 * the QMI length field, which excludes the frame's pad bytes. */
static void
mhi_cellular_poll(struct mhi_channel *out, struct mhi_channel *in)
{
	static uint8_t transaction = 0xc0;
	uint8_t request[7] = { 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00 };
	uint8_t answer[512];
	static unsigned dead;
	unsigned node, port;

	if (!mhi_qrtr_service(0x03, &node, &port)) {
		cellular_up = 0;
		return;
	}
	/* Run 055427's endpoint dropped off the bus at 05:58:26 (LTSSM not
	 * L0); every later send failed and this poll wrote 1796 "TX no
	 * completion" lines, pushing the IMS evidence out of the kept log.
	 * Ten silent answers in a row and the poll goes quiet for the run --
	 * a link that fell off the bus does not come back before the next
	 * boot. */
	if (dead >= 10) {
		cellular_up = 0;
		return;
	}
	cellular_up = 1;
	for (unsigned i = 0; i < 2; i++) {
		size_t got;

		transaction++;
		request[1] = transaction;
		request[3] = i == 0 ? 0x24 : 0x4f;
		request[4] = 0x00;
		got = mhi_qmi_call(out, in, 0x1001, node, port, request, sizeof(request),
		    i == 0 ? "IPCR NAS serving system (cellular state)"
			   : "IPCR NAS signal info (cellular state)",
		    answer, sizeof(answer));
		dead = got == 0 ? dead + 1 : 0;
		if (got >= 16) {
			unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;

			if (got > header + 7) {
				const uint8_t *qmi = answer + header;
				size_t body = got - header;
				size_t said = (size_t)qmi[5] | (size_t)qmi[6] << 8;

				if (said + 7 <= body) {
					body = said + 7;
				}
				/* The serving system's own TLVs, three times:
				 * TLV 0x10 is the roaming indicator, 0x11 the
				 * data service capability, 0x12 the current
				 * PLMN.  The parser reads 0x12 now; this dump
				 * puts the raw values on the record. */
				if (i == 0) {
					static unsigned dumps;

					if (dumps++ < 3) {
						mhi_qmi_tlvs(qmi, body,
						    "IPCR NAS serving system");
					}
				}
				/* Serving system and signal answer separately and
				 * alternate on this phone: the parser keeps the
				 * last signal reading across a "not registered"
				 * sample, and the bars only while registered. */
				if (hakios_nas_apply(&cellular_nas, qmi, body) == 1) {
					mhi_cellular_publish();
				}
			}
		}
	}
}

/* ---- SMS: the card's own number and the WMS send-to-self test ----
 *
 * The milestone after registration: EF_MSISDN names the card's own number,
 * WMS Set Event Report turns the modem's new-message indication on, and WMS
 * Raw Send submits a GSM 03.40 SMS-SUBMIT to that same number.  The MT
 * message the network delivers back arrives as a WMS Event Report
 * indication; mhi_sms_note takes down the storage and index it names (or its
 * raw PDU, when it carries one), and the next cellular cycle reads the
 * message with Raw Read and decodes sender and text.  Read Record is asked
 * the way libqmi asks it for qmicli --uim-read-record: the session TLV names
 * the primary GW application and the card's AID, the file TLV EF_MSISDN
 * (0x6f40) under ADF_USIM (path 7fff), and the record TLV record 1 with a
 * 28-byte read -- and a card answer of 6c XX asks for exactly XX bytes, so
 * the read is sent a second time with that length.
 */
static struct {
	int started;
	bool pending;
	unsigned storage;
	uint32_t index;
	unsigned reads;
	char own[24];
	unsigned own_digits;
	bool own_intl;
	bool ack_pending;	/* an MT over IMS asked for its RP ack */
	uint32_t ack_txn;
} sms_state;

static char cellular_own[24];
static char cellular_sms[96];

SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_own_number, CTLFLAG_RD | CTLFLAG_LOCKED,
    cellular_own, 0, "the card's own number (EF_MSISDN)");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_sms, CTLFLAG_RD | CTLFLAG_LOCKED,
    cellular_sms, 0, "the last SMS event");

/* The GSM 7-bit default alphabet as characters; the ones ASCII has no
 * letter for read '?'.  0x1b is the escape to the extension table, skipped
 * by the unpackers. */
static const char mhi_sms_gsm7[128] = {
	'@', '?', '$', '?', '?', '?', '?', '?', '?', '?', '\n', '?', '?', '\r', '?', '?',
	'?', '_', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
	' ', '!', '"', '#', '?', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	'?', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '?', '?', '?', '?', '?',
	'?', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '?', '?', '?', '?', '?'
};

/* The digits of a BCD number as characters, low nibble first; 0xf ends the
 * number.  Returns how many were taken. */
static unsigned
mhi_sms_bcd(char *out, size_t size, const uint8_t *data, size_t bytes)
{
	size_t n = 0;

	for (size_t i = 0; i < bytes; i++) {
		unsigned low = data[i] & 0x0f, high = data[i] >> 4;

		if (low > 9 || n + 2 > size) {
			break;
		}
		out[n++] = (char)('0' + low);
		if (high == 0x0f || high > 9) {
			break;
		}
		out[n++] = (char)('0' + high);
	}
	if (n < size) {
		out[n] = 0;
	}
	return (unsigned)n;
}

/* EF_MSISDN's record: an alpha identifier of unstated length, then the BCD
 * number's byte count, its TON/NPI and the digits.  The length byte is found
 * by its landing exactly on the record's end; a TON/NPI that says
 * international gets the '+'. */
static void
mhi_sms_parse_msisdn(const uint8_t *record, size_t size)
{
	for (size_t at = 0; at + 2 < size; at++) {
		unsigned bytes = record[at];

		if (bytes >= 2 && bytes <= 12 && at + 1 + bytes == size) {
			char number[24];
			size_t n = 0;
			unsigned digits;

			if (((record[at + 1] >> 4) & 0x07) == 0x01) {
				number[n++] = '+';
			}
			digits = mhi_sms_bcd(number + n, sizeof(number) - n,
			    record + at + 2, bytes - 1);
			if (digits == 0) {
				continue;
			}
			snprintf(sms_state.own, sizeof(sms_state.own), "%s", number);
			snprintf(cellular_own, sizeof(cellular_own), "%s", number);
			sms_state.own_digits = digits;
			sms_state.own_intl = ((record[at + 1] >> 4) & 0x07) == 0x01;
			hakios_tl("pcie2: cellular: SMS: THE CARD'S OWN NUMBER IS %s\n", number);
			return;
		}
	}
	{
		char hex[3 * 32 + 1];
		size_t shown = size > 32 ? 32 : size;

		for (size_t i = 0; i < shown; i++) {
			snprintf(hex + i * 3, 4, " %02x", record[i]);
		}
		hex[shown * 3] = 0;
		hakios_tl("pcie2: cellular: SMS: EF_MSISDN holds no readable number; %zu bytes:%s\n",
		    size, hex);
	}
}

/* A number handed in as text (DMS's string, the sms_to boot argument):
 * optional '+', then digits, spaces allowed.  Takes it as the card's own
 * number and says where it came from. */
static int
mhi_sms_take_number(const char *text, const char *source)
{
	char number[24];
	size_t n = 0, digits = 0;
	bool intl = false;

	if (text == NULL) {
		return 0;
	}
	if (*text == '+') {
		intl = true;
		text++;
	}
	for (; *text != 0; text++) {
		if (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
			continue;
		}
		if (*text < '0' || *text > '9' || n + 2 >= sizeof(number)) {
			return 0;
		}
		number[n++] = *text;
		digits++;
	}
	if (digits < 6 || digits > 20) {
		return 0;
	}
	number[n] = 0;
	if (intl) {
		memmove(number + 1, number, n + 1);
		number[0] = '+';
		n++;
	}
	snprintf(sms_state.own, sizeof(sms_state.own), "%s", number);
	sms_state.own_digits = (unsigned)digits;
	sms_state.own_intl = intl;
	snprintf(cellular_own, sizeof(cellular_own), "%s", number);
	hakios_tl("pcie2: cellular: SMS: THE CARD'S OWN NUMBER IS %s (%s)\n", number, source);
	return 1;
}

/* The modem's own view of the card's number (QMI DMS Get MSISDN, 0x24):
 * the stack reads EF_MSISDN through the card session it already holds for
 * the network, the session the AP's UIM read has been refused (runs
 * 054035 and 060227: result 1, error 3, no card result). */
static int
mhi_sms_dms_msisdn(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size)
{
	uint8_t request[7] = { 0x00, 0xc1, 0x00, 0x24, 0x00, 0x00, 0x00 };
	unsigned node, port, result = ~0u, error = ~0u, at;
	const uint8_t *string = NULL;
	size_t length = 0, got;
	char text[32];

	if (!mhi_qrtr_service(0x02, &node, &port)) {
		return 0;
	}
	got = mhi_qmi_call(out, in, 0x1001, node, port, request, sizeof(request),
	    "IPCR DMS get MSISDN", answer, answer_size);
	if (got < 16) {
		return 0;
	}
	{
		unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
		const uint8_t *qmi = answer + header;
		size_t body = got - header;

		if (body < 7) {
			return 0;
		}
		for (at = 7; at + 3 <= body; ) {
			unsigned type = qmi[at];
			unsigned size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

			if (at + 3 + size > body) {
				break;
			}
			if (type == 2 && size >= 4) {
				result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
				error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
			} else if (type == 1 && size >= 1) {
				length = qmi[at + 3];
				string = qmi + at + 4;
				if (length > size - 1) {
					string = NULL;
				}
			}
			at += 3 + size;
		}
	}
	hakios_tl("pcie2: cellular: SMS: DMS get MSISDN: result %u, error %u, %zu characters\n",
	    result, error, string != NULL ? length : 0);
	if (string == NULL || result != 0) {
		return 0;
	}
	{
		size_t n = 0;

		while (n < length && n + 1 < sizeof(text)) {
			text[n] = (char)string[n];
			n++;
		}
		text[n] = 0;
	}
	return mhi_sms_take_number(text, "the modem's DMS service, QMI Get MSISDN");
}

/* UIM Get File Attributes (0x24) for EF_MSISDN at path: its existence and
 * record size, so the read below asks for the length the card has. */
static void
mhi_sms_file_attributes(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, const uint8_t *path, unsigned path_len,
    const char *label, unsigned *record_size, uint8_t *answer, size_t answer_size)
{
	uint8_t request[7 + 5 + 8 + 8];
	size_t at = 7, got;
	unsigned result = ~0u, error = ~0u, sw1 = 0, sw2 = 0;
	unsigned file_size = 0, file_id = 0, file_type = 0, size = 0, count = 0;

	memset(request, 0, sizeof(request));
	request[0] = 0x00;
	request[1] = (uint8_t)transaction;
	request[2] = 0x00;
	request[3] = 0x24;	/* UIM get file attributes */
	request[4] = 0x00;
	/* TLV 1: the session -- primary GW provisioning, no AID */
	request[at++] = 0x01;
	request[at++] = 0x02;
	request[at++] = 0x00;
	request[at++] = 0x00;
	request[at++] = 0x00;
	/* TLV 2: the file -- EF_MSISDN at the path */
	request[at++] = 0x02;
	request[at++] = (uint8_t)(3 + path_len);
	request[at++] = 0x00;
	request[at++] = 0x40;
	request[at++] = 0x6f;
	request[at++] = (uint8_t)path_len;
	for (unsigned i = 0; i < path_len; i++) {
		request[at++] = path[i];
	}
	request[5] = (uint8_t)(at - 7);
	request[6] = (uint8_t)((at - 7) >> 8);
	got = mhi_qmi_call(out, in, 0x1001, node, port, request, (unsigned)at,
	    "IPCR UIM get file attributes (EF_MSISDN)", answer, answer_size);
	if (got < 16) {
		return;
	}
	{
		unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
		const uint8_t *qmi = answer + header;
		size_t body = got - header;

		if (body < 7) {
			return;
		}
		for (at = 7; at + 3 <= body; ) {
			unsigned type = qmi[at];
			unsigned tsize = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

			if (at + 3 + tsize > body) {
				break;
			}
			if (type == 2 && tsize >= 4) {
				result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
				error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
			} else if (type == 0x10 && tsize >= 2) {
				sw1 = qmi[at + 3];
				sw2 = qmi[at + 4];
			} else if (type == 0x11 && tsize >= 12) {
				file_size = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
				file_id = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
				file_type = qmi[at + 7];
				size = qmi[at + 8] | (unsigned)qmi[at + 9] << 8;
				count = qmi[at + 10] | (unsigned)qmi[at + 11] << 8;
			}
			at += 3 + tsize;
		}
	}
	hakios_tl("pcie2: cellular: SMS: file 6F40 under %s: result %u, error %u, card %02x %02x, file %u bytes (id %04x type %u), record %u bytes x %u\n",
	    label, result, error, sw1, sw2, file_size, file_id, file_type, size, count);
	if (result == 0 && size >= 2 && size <= 64) {
		*record_size = size;
	}
}

/* UIM Read Record (0x21) for EF_MSISDN at path under session type: one
 * attempt, and when the card answers 6Cxx the record is read again at the
 * length it named.  Returns 1 when the record carried a number. */
static int
mhi_sms_uim_read_record(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned transaction, unsigned session, const uint8_t *path,
    unsigned path_len, unsigned record_number, unsigned length, const char *label,
    uint8_t *answer, size_t answer_size)
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint8_t request[7 + 5 + 8 + 8];
		size_t at = 7, got;
		unsigned result = ~0u, error = ~0u, sw1 = 0, sw2 = 0;
		const uint8_t *record = NULL;
		size_t record_size = 0;

		memset(request, 0, sizeof(request));
		request[0] = 0x00;
		request[1] = (uint8_t)(transaction + attempt);
		request[2] = 0x00;
		request[3] = 0x21;	/* UIM read record */
		request[4] = 0x00;
		/* TLV 1: the session -- the type, no AID (libqmi's read-record
		 * builder passes an empty AID: the session already knows the
		 * application). */
		request[at++] = 0x01;
		request[at++] = 0x02;
		request[at++] = 0x00;
		request[at++] = (uint8_t)session;
		request[at++] = 0x00;
		/* TLV 2: the file -- EF_MSISDN at the path.  Each 16-bit id is
		 * little-endian, the way libqmi's read-record builder writes
		 * qmicli's "file" spec (the low byte first): 7FFF is ff 7f.
		 * Runs 054035 and 060227 wrote 7f ff and the stack answered
		 * result 1, error 3, with no card result at all. */
		request[at++] = 0x02;
		request[at++] = (uint8_t)(3 + path_len);
		request[at++] = 0x00;
		request[at++] = 0x40;
		request[at++] = 0x6f;
		request[at++] = (uint8_t)path_len;
		for (unsigned i = 0; i < path_len; i++) {
			request[at++] = path[i];
		}
		/* TLV 3: the record number, the length asked */
		request[at++] = 0x03;
		request[at++] = 0x04;
		request[at++] = 0x00;
		request[at++] = (uint8_t)record_number;
		request[at++] = 0x00;
		request[at++] = (uint8_t)length;
		request[at++] = (uint8_t)(length >> 8);
		request[5] = (uint8_t)(at - 7);
		request[6] = (uint8_t)((at - 7) >> 8);
		got = mhi_qmi_call(out, in, 0x1001, node, port, request, (unsigned)at,
		    label, answer, answer_size);
		if (got < 16) {
			return 0;
		}
		{
			unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
			const uint8_t *qmi = answer + header;
			size_t body = got - header;

			if (body < 7) {
				return 0;
			}
			for (at = 7; at + 3 <= body; ) {
				unsigned type = qmi[at];
				unsigned tsize = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

				if (at + 3 + tsize > body) {
					break;
				}
				if (type == 2 && tsize >= 4) {
					result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
					error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
				} else if (type == 0x10 && tsize >= 2) {
					sw1 = qmi[at + 3];
					sw2 = qmi[at + 4];
				} else if (type == 0x11 && tsize >= 2) {
					record_size = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
					record = qmi + at + 5;
					if (record_size == 0 || record_size > tsize - 2) {
						record = NULL;
					}
				}
				at += 3 + tsize;
			}
		}
		hakios_tl("pcie2: cellular: SMS: read EF_MSISDN %s, record %u: result %u, error %u, card %02x %02x, %zu-byte record\n",
		    label, record_number, result, error, sw1, sw2, record_size);
		if (record != NULL) {
			mhi_sms_parse_msisdn(record, record_size);
			if (sms_state.own_digits != 0) {
				return 1;
			}
		}
		if (sw1 == 0x6c && sw2 >= 2 && sw2 <= 64 && attempt == 0) {
			length = sw2;	/* the card named the record's length */
			continue;
		}
		return 0;
	}
	return 0;
}

/* The number to send the self-test to, when neither the card nor the
 * modem names the line (this card: EF_MSISDN is blank and the modem's DMS
 * answers NOT_PROVISIONED, error 16).  The header is deliberately not a
 * repository file: it is placed by hand next to this driver in the kernel
 * tree, so the number never enters the source; docs/cellular.md tells how. */
#if defined(HAKIOS_SMS_TARGET)
/* the build already names the number */
#elif __has_include("hakios_sms_target.h")
#include "hakios_sms_target.h"
#endif

/* UIM Read Transparent (0x0020) for a transparent EF: offset 0, the length
 * asked, and the card's answer bytes logged raw.  The first caller is
 * EF_ACL (6f57) under ADF_USIM -- the UICC's own APN control list, the one
 * ds707's "APN not enabled in UICC card" check reads -- so its bytes say
 * whether this card restricts the APN a data call may name.  A card answer
 * of 6c XX asks for exactly XX bytes, so the read is sent a second time
 * with that length. */
static void
mhi_sms_uim_read_transparent(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, unsigned transaction, const uint8_t *path,
    unsigned path_len, unsigned file_id, unsigned length, const char *label,
    uint8_t *answer, size_t answer_size)
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint8_t request[7 + 5 + 8 + 8];
		size_t at = 7, got;
		unsigned result = ~0u, error = ~0u, sw1 = 0, sw2 = 0;
		const uint8_t *data = NULL;
		size_t data_size = 0;

		memset(request, 0, sizeof(request));
		request[0] = 0x00;
		request[1] = (uint8_t)(transaction + attempt);
		request[2] = 0x00;
		request[3] = 0x20;	/* UIM read transparent */
		request[4] = 0x00;
		/* TLV 1: the session -- primary GW provisioning, no AID */
		request[at++] = 0x01;
		request[at++] = 0x02;
		request[at++] = 0x00;
		request[at++] = 0x00;
		request[at++] = 0x00;
		/* TLV 2: the file -- its 16-bit id little-endian, then the path */
		request[at++] = 0x02;
		request[at++] = (uint8_t)(3 + path_len);
		request[at++] = 0x00;
		request[at++] = (uint8_t)file_id;
		request[at++] = (uint8_t)(file_id >> 8);
		request[at++] = (uint8_t)path_len;
		for (unsigned i = 0; i < path_len; i++) {
			request[at++] = path[i];
		}
		/* TLV 3: offset 0, the length asked */
		request[at++] = 0x03;
		request[at++] = 0x04;
		request[at++] = 0x00;
		request[at++] = 0x00;
		request[at++] = 0x00;
		request[at++] = (uint8_t)length;
		request[at++] = (uint8_t)(length >> 8);
		request[5] = (uint8_t)(at - 7);
		request[6] = (uint8_t)((at - 7) >> 8);
		got = mhi_qmi_call(out, in, 0x1001, node, port, request,
		    (unsigned)at, label, answer, answer_size);
		if (got < 16) {
			return;
		}
		{
			unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
			const uint8_t *qmi = answer + header;
			size_t body = got - header;

			if (body < 7) {
				return;
			}
			for (at = 7; at + 3 <= body; ) {
				unsigned type = qmi[at];
				unsigned tsize = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

				if (at + 3 + tsize > body) {
					break;
				}
				if (type == 2 && tsize >= 4) {
					result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
					error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
				} else if (type == 0x10 && tsize >= 2) {
					sw1 = qmi[at + 3];
					sw2 = qmi[at + 4];
				} else if (type == 0x11 && tsize >= 1) {
					data = qmi + at + 3;
					data_size = tsize;
				}
				at += 3 + tsize;
			}
		}
		if (sw1 == 0x6c && attempt == 0) {
			length = sw2;	/* the card names the right length */
			continue;
		}
		{
			char hex[3 * 48 + 1];
			size_t n = data_size > 48 ? 48 : data_size;

			for (size_t i = 0; i < n; i++) {
				snprintf(hex + i * 3, 4, " %02x",
				    data != NULL ? data[i] : 0);
			}
			hex[n * 3] = 0;
			hakios_tl("pcie2: cellular: SMS: %s: result %u, error %u, card %02x %02x, %zu bytes%s\n",
			    label, result, error, sw1, sw2, data_size,
			    data != NULL ? hex : "");
		}
		return;
	}
}

/* Where EF_MSISDN may live, each 16-bit id written little-endian as qmicli
 * writes its "file" spec, and -- on the first path only -- the three session
 * types some stacks accept for an AP read: primary GW provisioning (0,
 * libqmi's), primary GW (3) and card slot 1 (6). */
static void
mhi_sms_read_msisdn(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	static const uint8_t paths[4][4] = {
		{ 0xff, 0x7f, 0x00, 0x00 },
		{ 0x10, 0x7f, 0x00, 0x00 },
		{ 0xff, 0x7f, 0x20, 0x7f },
		{ 0x00, 0x00, 0x00, 0x00 }
	};
	static const unsigned path_lens[4] = { 2, 2, 4, 0 };
	static const char *const path_names[4] = {
		"under ADF_USIM 7fff", "under DF_TELECOM 7f10",
		"under ADF_USIM/DF_GSM 7fff 7f20", "with no path"
	};
	static const unsigned sessions[3] = { 0, 3, 6 };
	unsigned length = 28;

	/* The pathless request is the one this AMSS answers at the card
	 * (run 062146: result 0, card 90 00), so ask it first for the file's
	 * identity and record size: the file id it names tells what the
	 * pathless read below actually selected. */
	mhi_sms_file_attributes(out, in, node, port, 0xd8, paths[3], path_lens[3],
	    path_names[3], &length, answer, answer_size);
	mhi_sms_file_attributes(out, in, node, port, 0xd0, paths[0], path_lens[0],
	    path_names[0], &length, answer, answer_size);
	/* The card's own APN control list, read once per boot: EF_ACL (6f57)
	 * under ADF_USIM.  Every data call's APN is checked against it by the
	 * firmware (the "APN not enabled in UICC card" path), so its bytes
	 * say what this card allows. */
	mhi_sms_uim_read_transparent(out, in, node, port, 0x9e, paths[0],
	    path_lens[0], 0x6f57, 128,
	    "IPCR UIM read transparent EF_ACL (6f57)", answer, answer_size);
	for (unsigned p = 0; p < 4; p++) {
		for (unsigned s = 0; s < 3; s++) {
			char label[72];

			if (p != 0 && s != 0) {
				continue;	/* other sessions on 7fff only */
			}
			snprintf(label, sizeof(label), "%s, session %u",
			    path_names[p], sessions[s]);
			if (mhi_sms_uim_read_record(out, in, node, port,
			    0xd1 + p * 3 + s, sessions[s], paths[p], path_lens[p], 1,
			    length, label, answer, answer_size)) {
				return;
			}
		}
	}
	/* Record 1 came back empty there (run 062146: 28 bytes of ff).
	 * Some cards carry the number in a later record; try them on the
	 * one request shape the stack accepts before giving up. */
	for (unsigned r = 2; r <= 4; r++) {
		char label[72];

		snprintf(label, sizeof(label), "%s, session %u",
		    path_names[3], sessions[0]);
		if (mhi_sms_uim_read_record(out, in, node, port, 0xe0 + r,
		    sessions[0], paths[3], path_lens[3], r, length, label,
		    answer, answer_size)) {
			return;
		}
	}
	hakios_tl("pcie2: cellular: SMS: EF_MSISDN is not readable under any path, session or record tried\n");
}

/* WMS Set Event Report, TLV 0x10 "new MT message" on: the modem then sends
 * an Event Report indication for every message the network delivers. */
static void
mhi_sms_event_report(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	static const uint8_t request[11] = {
		0x00, 0xb2, 0x00, 0x01, 0x00, 0x04, 0x00,
		0x10, 0x01, 0x00, 0x01
	};
	size_t got;

	got = mhi_qmi_call(out, in, 0x1001, node, port, request, sizeof(request),
	    "IPCR WMS set event report (new MT message)", answer, answer_size);
	if (got != 0) {
		hakios_tl("pcie2: cellular: SMS: the modem's new-message indication is on\n");
	}
}

/* The SMS-SUBMIT PDU (3GPP 23.040) for the fixed test text, UCS2, to
 * target (the card's own number, '+' optional): the SMSC field left empty
 * says "the modem's own".  Returns the PDU's length, or 0. */
static size_t
mhi_sms_submit_pdu(uint8_t *pdu, size_t size, const char *target, bool international)
{
	static const char text[] = "HakiOS SMS test";
	size_t n = 0, digits = 0, data, text_len = sizeof(text) - 1;

	if (size < 16 + text_len * 2 + 16) {
		return 0;
	}
	for (const char *p = target; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			digits++;
		}
	}
	if (digits == 0 || digits > 20) {
		return 0;
	}
	pdu[n++] = 0x00;	/* no SMSC address: the modem's own */
	pdu[n++] = 0x01;	/* SMS-SUBMIT, no validity period, no header */
	pdu[n++] = 0x00;	/* message reference */
	pdu[n++] = (uint8_t)digits;
	pdu[n++] = international ? 0x91 : 0x81;	/* TON, NPI ISDN */
	data = n;
	n += (digits + 1) / 2;
	memset(pdu + data, 0, (digits + 1) / 2);
	{
		unsigned d = 0;

		for (const char *p = target; *p; p++) {
			if (*p < '0' || *p > '9') {
				continue;
			}
			if ((d & 1) == 0) {
				pdu[data + d / 2] = (uint8_t)(*p - '0');
			} else {
				pdu[data + d / 2] = (uint8_t)(pdu[data + d / 2] |
				    (unsigned)(*p - '0') << 4);
			}
			d++;
		}
		if (digits & 1) {
			pdu[data + digits / 2] |= 0xf0;
		}
	}
	pdu[n++] = 0x00;	/* protocol identifier */
	pdu[n++] = 0x08;	/* UCS2 */
	pdu[n++] = (uint8_t)(text_len * 2);
	for (size_t i = 0; i < text_len; i++) {
		pdu[n++] = 0x00;
		pdu[n++] = (uint8_t)text[i];
	}
	return n;
}

/* WMS Raw Send: TLV 1 {format 6 (GSM/WCDMA point-to-point), a u16 length,
 * the PDU}, and TLV 0x13 "SMS on IMS" (a u8, 1 = send over the IMS
 * registration) when the caller has the IMS transport registered -- what
 * qcril marks on this SIM, whose network has no CS domain.  The answer
 * carries the modem's message id and, when the send failed, the RP/TP
 * causes. */
static void
mhi_sms_send(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, int over_ims, uint8_t *answer, size_t answer_size)
{
	uint8_t request[7 + 3 + 3 + 96 + 4];
	uint8_t pdu[96];
	size_t pdu_len, at = 7;
	unsigned result = ~0u, error = ~0u, message = 0, rp = 0, tp = 0;
	unsigned failure = ~0u, attempt, tries = over_ims ? 3 : 1;
	int skip = 0;

	/* An experiment run can hold the submit back: with the IMS transport
	 * unregistered its call waits ~100 s for an answer that never comes,
	 * and the runs that ended there lost the post-dial IMSA reads (191905
	 * stopped mid-wait).  Armed with "pcie2_no_sms=1"; the submit is the
	 * default behavior otherwise. */
	PE_parse_boot_argn("pcie2_no_sms", &skip, sizeof(skip));
	if (skip != 0) {
		hakios_tl("pcie2: cellular: SMS: the submit is skipped this run (pcie2_no_sms); the IMS evidence after the dial keeps the thread\n");
		return;
	}
	if (sms_state.own_digits == 0) {
		hakios_tl("pcie2: cellular: SMS: no own number was read; the send-to-self test is skipped\n");
		return;
	}
	pdu_len = mhi_sms_submit_pdu(pdu, sizeof(pdu), sms_state.own,
	    sms_state.own_intl);
	if (pdu_len == 0) {
		hakios_tl("pcie2: cellular: SMS: the SMS-SUBMIT could not be built\n");
		return;
	}
	memset(request, 0, sizeof(request));
	request[0] = 0x00;
	request[2] = 0x00;
	request[3] = 0x20;	/* WMS raw send */
	request[4] = 0x00;
	request[at++] = 0x01;
	request[at++] = (uint8_t)(3 + pdu_len);
	request[at++] = 0x00;
	request[at++] = 0x06;	/* GSM/WCDMA point-to-point */
	request[at++] = (uint8_t)pdu_len;
	request[at++] = (uint8_t)(pdu_len >> 8);
	memcpy(request + at, pdu, pdu_len);
	at += pdu_len;
	if (over_ims) {
		request[at++] = 0x13;	/* SMS on IMS */
		request[at++] = 0x01;
		request[at++] = 0x00;
		request[at++] = 0x01;
		hakios_tl("pcie2: cellular: SMS: the SMS-SUBMIT carries the SMS-on-IMS marker\n");
	}
	request[5] = (uint8_t)(at - 7);
	request[6] = (uint8_t)((at - 7) >> 8);
	/* Run 072221: the modem took the SMS-SUBMIT (its answer came 9 s out,
	 * message id 11) and failed it with result 1, error 56 (WMS message
	 * delivery failure), failure type 0 -- temporary.  Temporary failures
	 * are what a retry is for; a permanent one is not repeated.  Each
	 * attempt carries a transaction of its own so a late answer to an
	 * earlier attempt is told apart.  Without the SMS-on-IMS marker there
	 * is no transport at all on this SIM's network (no CS domain), so one
	 * attempt says that and three would only cost three timeouts -- run
	 * 163801 spent 3 x ~100 s unanswered here and the dial behind it
	 * never ran. */
	for (attempt = 1; attempt <= tries; attempt++) {
		size_t got, pos;
		unsigned header;

		request[1] = (uint8_t)(0xb3 + attempt - 1);
		result = ~0u;
		error = ~0u;
		message = 0;
		rp = 0;
		tp = 0;
		failure = ~0u;
		hakios_tl("pcie2: cellular: SMS: the SMS-SUBMIT to %s goes out (attempt %u of %u, %zu bytes of PDU)\n",
		    sms_state.own, attempt, tries, pdu_len);
		got = mhi_qmi_call_slow(out, in, 0x1001, node, port, request,
		    (unsigned)at, "IPCR WMS raw send (SMS to the card's own number)",
		    answer, answer_size);
		if (got >= 16) {
			header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
			const uint8_t *qmi = answer + header;
			size_t body = got - header;

			for (pos = 7; pos + 3 <= body; ) {
				unsigned type = qmi[pos];
				unsigned size = qmi[pos + 1] |
				    (unsigned)qmi[pos + 2] << 8;

				if (pos + 3 + size > body) {
					break;
				}
				if (type == 2 && size >= 4) {
					result = qmi[pos + 3] |
					    (unsigned)qmi[pos + 4] << 8;
					error = qmi[pos + 5] |
					    (unsigned)qmi[pos + 6] << 8;
				} else if (type == 0x01 && size >= 2) {
					message = qmi[pos + 3] |
					    (unsigned)qmi[pos + 4] << 8;
				} else if (type == 0x12 && size >= 3) {
					rp = qmi[pos + 3] |
					    (unsigned)qmi[pos + 4] << 8;
					tp = qmi[pos + 5];
				} else if (type == 0x13 && size >= 1) {
					failure = qmi[pos + 3];
				}
				pos += 3 + size;
			}
		}
		if (result == 0) {
			break;
		}
		if (got >= 16) {
			hakios_tl("pcie2: cellular: SMS: attempt %u of %u FAILED: result %u, error %u, RP cause %#x, TP cause %#x, delivery failure %s\n",
			    attempt, tries, result, error, rp, tp,
			    failure == 0 ? "temporary" :
			    failure == 1 ? "permanent" : "not given");
		}
		if (failure == 1) {
			break;
		}
		if (attempt < tries) {
			hakios_tl("pcie2: cellular: SMS: the failure is temporary, not the end; another attempt comes in 8 s\n");
			delay(8000000);
		}
	}
	if (result == 0) {
		snprintf(cellular_sms, sizeof(cellular_sms), "sent to %s (message %u)",
		    sms_state.own, message);
		hakios_tl("pcie2: cellular: SMS: THE SMS-SUBMIT TO %s WAS TAKEN (message %u); the delivery back is the receive test\n",
		    sms_state.own, message);
	} else {
		hakios_tl("pcie2: cellular: SMS: the SMS-SUBMIT to %s FAILED after %u attempts: result %u, error %u, RP cause %#x, TP cause %#x\n",
		    sms_state.own, attempt > tries ? tries : attempt, result,
		    error, rp, tp);
	}
}

/* WMS Get Transport Network Registration Status (0x4a): the SMS stack's own
 * view of the transport a submit will ride -- the MSC over CS, or the IMS
 * registration -- and so the first thing to know when a submit fails
 * "temporary".  Returns the status byte (~0u when the modem did not answer):
 * 0 no service, 1 in process, 2 permanent failure, 3 limited, 4 full. */
static unsigned
mhi_sms_transport(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	static const char *const states[] = {
		"no service", "in process", "permanent failure",
		"limited service", "full service"
	};
	uint8_t request[7] = { 0x00, 0x4c, 0x00, 0x4a, 0x00, 0x00, 0x00 };
	unsigned result = ~0u, error = ~0u, status = ~0u;
	size_t got, at;

	got = mhi_qmi_call(out, in, 0x1001, node, port, request, sizeof(request),
	    "IPCR WMS transport status", answer, answer_size);
	if (got < 16) {
		return ~0u;
	}
	{
		unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
		const uint8_t *qmi = answer + header;
		size_t body = got - header;

		for (at = 7; at + 3 <= body; ) {
			unsigned type = qmi[at];
			unsigned size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

			if (at + 3 + size > body) {
				break;
			}
			if (type == 2 && size >= 4) {
				result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
				error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
			} else if (type == 0x10 && size >= 1) {
				status = qmi[at + 3];
			}
			at += 3 + size;
		}
	}
	hakios_tl("pcie2: cellular: SMS: the transport status is %u (%s), result %u, error %u\n",
	    status, status < 5 ? states[status] : "not given", result, error);
	return status;
}

/* ---- IMS: the PDN, the services and the registration SMS rides ----
 *
 * This network has no CS domain to carry the SMS over SGs: Android's own
 * registration on it is NR SA with the IMS services advertised (dumpsys
 * telephony.registry: CS domain availableServices=[VOICE,SMS,VIDEO]) and
 * the Qualcomm IMS stack reports "VOLTE ims registered" (QImsService,
 * ImsRegistrationInfo state 1), with imssms.sms_over_ims_supported_bool
 * true and the carrier's APNs naming apn=ims, protocol=IPV4V6 for 460/15.
 * The IMS itself runs in the AMSS; the host's part is what qcril does over
 * QMI: bring the "ims" PDN up (WDS), turn IMS, VoLTE and IMS SMS on (IMS
 * service 0x8f), and read the registration (IMSA 0x20/0x21).  Run
 * 081226's three submits failed "temporary" (error 56, failure type 0)
 * with no such transport under them.
 */

/* The modem's own 3GPP profile list (0x002a) and the name and APN each
 * profile carries (0x002b, TLV 0x10 and 0x14).  Returns the index of the
 * first profile whose APN is "ims", or -1; *data_out takes the first
 * profile whose APN is anything else, for the control start that tells an
 * ims-specific refusal from a broken data path.  Start Network by profile
 * is what qcril does; an APN string the modem never provisioned was
 * refused outright in run 101216 (result 1, error 70 InvalidOperation). */
static int
mhi_wds_ims_profile(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, uint8_t *answer, size_t answer_size,
    int *data_out)
{
	uint8_t request[32];
	uint8_t list[160];
	const uint8_t *value;
	size_t got, at, list_size, size;
	unsigned result = ~0u, error = ~0u;
	int found = -1, data = -1;

	at = mhi_qmi_open(request, 0xd0, 0x002a);
	{
		uint8_t type = 0x00;	/* 3GPP */

		at = mhi_qmi_put(request, at, 0x10, &type, 1);
	}
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS profile list", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got == 0 || result != 0) {
		hakios_tl("pcie2: cellular: IMS: the profile list says result %u, error %u\n",
		    result, error);
		return -1;
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x01, &list_size);
	if (value == NULL || list_size < 1) {
		hakios_tl("pcie2: cellular: IMS: the profile list came back without entries\n");
		return -1;
	}
	/* The per-profile settings calls below share `answer`, so the list is
	 * copied out first: run 141245 parsed entry 1 and then read the
	 * settings answer as if it were the list, and every profile after the
	 * first disappeared from the survey (the ims profile was never found,
	 * and the ims-profile starts never ran). */
	if (list_size > sizeof(list)) {
		list_size = sizeof(list);
	}
	memcpy(list, value, list_size);
	for (at = 0; at < list_size; at += 32) {
		char hex[3 * 32 + 1];
		size_t shown = list_size - at > 32 ? 32 : list_size - at;

		for (size_t i = 0; i < shown; i++) {
			snprintf(hex + i * 3, 4, " %02x", list[at + i]);
		}
		hex[shown * 3] = 0;
		hakios_tl("pcie2: cellular: IMS: the profile list's bytes from %u:%s\n",
		    (unsigned)at, hex);
	}
	/* TLV 0x01: a u8 element count, then that many {type u8, index u8,
	 * name string} (libqmi's definition and wwand's wire schema agree).
	 * Run 113605 read the count as a profile: every entry was relabelled
	 * by one field and profile 1 -- the one the modem's own attach uses --
	 * was never surveyed. */
	{
		unsigned count = value[0], entries;

		hakios_tl("pcie2: cellular: IMS: the profile list names %u profiles\n",
		    count);
		for (at = 1, entries = 0;
		    entries < count && at + 3 <= list_size; entries++) {
			unsigned type = list[at];
			unsigned index = list[at + 1];
			unsigned length = list[at + 2];
			unsigned sresult = ~0u, serror = ~0u;

			if (at + 3 + length > list_size) {
				break;
			}
			hakios_tl("pcie2: cellular: IMS: profile %u of type %u is \"%.*s\"\n",
			    index, type, (int)length,
			    (const char *)&list[at + 3]);
			{
				uint8_t id[2];

				id[0] = (uint8_t)type;
				id[1] = (uint8_t)index;
				got = mhi_qmi_open(request,
				    (uint8_t)(0xd1 + index), 0x002b);
				got = mhi_qmi_put(request, got, 0x01, id, 2);
				mhi_qmi_seal(request, got);
				got = mhi_qmi_call(out, in, 0x1002, node,
				    port, request, (unsigned)got,
				    "IPCR WDS profile settings", answer,
				    answer_size);
			}
			at += 3 + length;
			if (got == 0) {
				hakios_tl("pcie2: cellular: IMS: profile %u settings went unanswered\n",
				    index);
			} else {
				const uint8_t *apn, *pname;
				size_t nsize = 0;

				mhi_qmi_outcome(answer, got, &sresult, &serror);
				apn = sresult == 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x14, &size) : NULL;
				pname = sresult == 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x10, &nsize) : NULL;
				if (pname != NULL && (nsize == 0 || nsize > 63)) {
					pname = NULL;
				}
				if (sresult != 0) {
					hakios_tl("pcie2: cellular: IMS: profile %u settings say result %u, error %u\n",
					    index, sresult, serror);
				} else if (apn == NULL) {
					hakios_tl("pcie2: cellular: IMS: profile %u (\"%.*s\") reports no APN TLV\n",
					    index,
					    pname != NULL ? (int)nsize : 0,
					    pname != NULL ? (const char *)pname : "");
				} else {
					/* The APN is a plain string (libqmi's
					 * "WDS APN Name"); run 121242 read a
					 * length prefix that is not there.
					 * The name in TLV 0x10 names the
					 * store slot ("qdp_profile_ia" is the
					 * initial-attach profile). */
					unsigned alen = (unsigned)size;
					const char *name = (const char *)apn;

					hakios_tl("pcie2: cellular: IMS: profile %u (\"%.*s\") carries the APN \"%.*s\"\n",
					    index,
					    pname != NULL ? (int)nsize : 0,
					    pname != NULL ? (const char *)pname : "",
					    (int)alen, name);
					/* The APN type mask (TLV 0xdd, u64;
					 * libqmi's QmiWdsApnTypeMask: bit 0
					 * default, 1 ims, 2 mms, 4 supl,
					 * 5 hipri, 6 fota, 8 initial attach,
					 * 9 emergency, 10 xcap; qcril's 1073
					 * is 0x431, default+supl+hipri+xcap).
					 * The earlier decode here read bit 1 as
					 * mms and bit 5 as ims, so run 154032
					 * printed the ims profile's 0x2 as
					 * "ims 0".  Xiaomi's qcril sets the mask
					 * per profile at every boot
					 * ("setProfileSupportedAPNTypes"); a
					 * CM ACL refusal can be this mask
					 * missing the call's type. */
					{
						const uint8_t *mask =
						    mhi_qmi_answer_tlv(answer, got,
						    0xdd, &size);

						if (mask != NULL && size >= 8) {
							unsigned low = mask[0] |
							    (unsigned)mask[1] << 8 |
							    (unsigned)mask[2] << 16 |
							    (unsigned)mask[3] << 24;

							hakios_tl("pcie2: cellular: IMS: profile %u's APN type mask is %#x (default %u, ims %u, mms %u, supl %u, hipri %u, fota %u, ia %u, emergency %u, xcap %u)\n",
							    index, low,
							    (low >> 0) & 1u,
							    (low >> 1) & 1u,
							    (low >> 2) & 1u,
							    (low >> 4) & 1u,
							    (low >> 5) & 1u,
							    (low >> 6) & 1u,
							    (low >> 8) & 1u,
							    (low >> 9) & 1u,
							    (low >> 10) & 1u);
						}
					}
					/* The profile's PDP type (TLV 0x11,
					 * QmiWdsPdpType: 0 IPv4, 2 IPv6,
					 * 3 IPv4v6).  The start forms
					 * below leave the IP family to it. */
					{
						const uint8_t *pdp =
						    mhi_qmi_answer_tlv(answer, got,
						    0x11, &size);

						if (pdp != NULL && size >= 1) {
							hakios_tl("pcie2: cellular: IMS: profile %u's PDP type is %u (%s)\n",
							    index, pdp[0],
							    pdp[0] == 0 ? "IPv4" :
							    pdp[0] == 2 ? "IPv6" :
							    pdp[0] == 3 ? "IPv4v6" :
							    "other");
						}
					}
					/* The modem's own ACL check is
					 * ds707_data_session_call_allowed, which asks
					 * whether the APN is enabled: in the profile
					 * (TLV 0xa9, and the newer 0x2f disabled flag)
					 * and in the UICC's EF_ACL, and whether the
					 * profile disallows roaming (0x3e).  All are
					 * logged so a refusal can be told from an
					 * unprovisioned profile. */
					{
						const uint8_t *enabled;
						const uint8_t *aclass;
						const uint8_t *disabled;
						const uint8_t *roaming;
						size_t esize = 0, csize = 0;
						size_t dsize = 0, rsize = 0;

						enabled = mhi_qmi_answer_tlv(answer,
						    got, 0xa9, &esize);
						aclass = mhi_qmi_answer_tlv(answer,
						    got, 0xab, &csize);
						disabled = mhi_qmi_answer_tlv(answer,
						    got, 0x2f, &dsize);
						roaming = mhi_qmi_answer_tlv(answer,
						    got, 0x3e, &rsize);
						hakios_tl("pcie2: cellular: IMS: profile %u's APN enabled is %d, APN class %u, APN disabled %d, roaming disallowed %d\n",
						    index,
						    enabled != NULL && esize >= 1 ?
						    (int)enabled[0] : -1,
						    aclass != NULL && csize >= 1 ?
						    (unsigned)aclass[0] : 0,
						    disabled != NULL && dsize >= 1 ?
						    (int)disabled[0] : -1,
						    roaming != NULL && rsize >= 1 ?
						    (int)roaming[0] : -1);
					}
					if (alen == 3 && name[0] == 'i' &&
					    name[1] == 'm' && name[2] == 's') {
						if (found < 0) {
							found = (int)index;
						}
					} else if (data < 0) {
						data = (int)index;
					}
				}
			}
		}
	}
	if (data_out != NULL) {
		*data_out = data;
	}
	if (found >= 0) {
		hakios_tl("pcie2: cellular: IMS: the ims profile is %d; the data profile for the control is %d\n",
		    found, data);
	} else {
		hakios_tl("pcie2: cellular: IMS: no profile carries the ims APN; the data profile for the control is %d\n",
		    data);
	}
	return found;
}

/* No profile carried the ims APN: create one the way the APN database's
 * ims row describes it (IPv4v6, IMCN flag set) and return its index.
 * Create Profile 0x0027 input: TLV 0x01 profile type, 0x10 name, 0x11 PDP
 * type, 0x14 APN, 0x22 IMCN flag; output TLV 0x01 {type u8, index u8}. */
static int
mhi_wds_ims_create(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, uint8_t *answer, size_t answer_size)
{
	uint8_t request[64];
	const uint8_t *value;
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u;

	at = mhi_qmi_open(request, 0xd7, 0x0027);
	{
		uint8_t type = 0x00, pdp = 0x03, imcn = 0x01;

		at = mhi_qmi_put(request, at, 0x01, &type, 1);
		at = mhi_qmi_put(request, at, 0x10, "ims", 3);
		at = mhi_qmi_put(request, at, 0x11, &pdp, 1);
		at = mhi_qmi_put(request, at, 0x14, "ims", 3);
		at = mhi_qmi_put(request, at, 0x22, &imcn, 1);
	}
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS profile create", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got == 0 || result != 0) {
		hakios_tl("pcie2: cellular: IMS: the ims profile could not be created: result %u, error %u\n",
		    result, error);
		return -1;
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x01, &size);
	if (value == NULL || size < 2) {
		hakios_tl("pcie2: cellular: IMS: the created profile came back unnamed\n");
		return -1;
	}
	{
		/* The create's answer is a promise, not a fact: read the profile
		 * back before dialling it, or a create that did not persist is
		 * only discovered as a start failure.  (Run 113605 reported
		 * profile 7 created and never saw it in a later list.) */
		uint8_t id[2] = { value[0], value[1] };
		unsigned vresult = ~0u, verror = ~0u;

		hakios_tl("pcie2: cellular: IMS: THE IMS PROFILE WAS CREATED as %u (type %u)\n",
		    id[1], id[0]);
		at = mhi_qmi_open(request, 0xd8, 0x002b);
		at = mhi_qmi_put(request, at, 0x01, id, 2);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS created profile read back", answer,
		    answer_size);
		if (got == 0) {
			hakios_tl("pcie2: cellular: IMS: the created profile %u did not read back\n",
			    id[1]);
		} else {
			const uint8_t *apn;

			mhi_qmi_outcome(answer, got, &vresult, &verror);
			apn = vresult == 0 ? mhi_qmi_answer_tlv(answer, got,
			    0x14, &size) : NULL;
			if (vresult != 0) {
				hakios_tl("pcie2: cellular: IMS: the created profile %u reads back result %u, error %u -- it did not persist\n",
				    id[1], vresult, verror);
			} else if (apn == NULL || size < 1) {
				hakios_tl("pcie2: cellular: IMS: the created profile %u reads back without an APN\n",
				    id[1]);
			} else {
				/* A plain string, no length prefix. */
				hakios_tl("pcie2: cellular: IMS: the created profile %u reads back with the APN \"%.*s\"\n",
				    id[1], (int)size, (const char *)apn);
			}
		}
		return id[1];
	}
}

/* qcril's own ims-profile Create Profile (0x0027), replayed byte for byte
 * from the two captures of the working phone (qmi5 line 1451 -- the 02:18
 * airplane-mode rebuild -- and qmi3 line 215, the boot; identical 37-byte
 * bodies): profile type 1 (the 3GPP2 store), TLV 0x8f = 0, 0x9a = 0, the
 * extended APN "ims" (0xa1), the extended IP type IPv6 (0xa2), 0xad = 0,
 * and the 8-byte APN type mask 2 ims (0xdd, QmiWdsApnTypeMask).  Every
 * captured boot creates it right before the Start Network the CM admits,
 * and that start names no profile: the modem resolves the dial by the
 * mask, so this is the profile the access list judges.  The answer's TLV
 * 0x01 {type, index} names where the store put it (both captures: type 1,
 * index 104).  Returns the index, -1 when the store refused. */
static int
mhi_wds_profile_create_ims(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, uint8_t *answer, size_t answer_size)
{
	uint8_t request[64];
	const uint8_t *value;
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u;
	uint8_t ptype = 0x01, zero = 0x00, ipv6 = 0x02;
	uint8_t mask[8] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	at = mhi_qmi_open(request, 0xf1, 0x0027);
	at = mhi_qmi_put(request, at, 0x01, &ptype, 1);
	at = mhi_qmi_put(request, at, 0x8f, &zero, 1);
	at = mhi_qmi_put(request, at, 0x9a, &zero, 1);
	at = mhi_qmi_put(request, at, 0xa1, "ims", 3);
	at = mhi_qmi_put(request, at, 0xa2, &ipv6, 1);
	at = mhi_qmi_put(request, at, 0xad, &zero, 1);
	at = mhi_qmi_put(request, at, 0xdd, mask, sizeof(mask));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS create qcril's ims profile", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got == 0 || result != 0) {
		hakios_tl("pcie2: cellular: IMS: qcril's ims profile could not be created: result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
		return -1;
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x01, &size);
	if (value == NULL || size < 2) {
		hakios_tl("pcie2: cellular: IMS: QCRIL'S IMS PROFILE CREATE ANSWERED WITHOUT AN INDEX\n");
		return -1;
	}
	hakios_tl("pcie2: cellular: IMS: QCRIL'S IMS PROFILE IS CREATED as %u of type %u (APN \"ims\", IPv6, the APN type mask 2)\n",
	    value[1], value[0]);
	return value[1];
}

/* The next Start Network's technology-preference probe: 0 none, 1 TLV 0x30
 * (3GPP allowed), 2 TLV 0x34 (EPC; no form sends it now -- run 172318 was
 * answered error 48 -- and the branch stays for a firmware that takes it).
 * A module flag because the probe is an axis added to one form, not a form
 * of its own; mhi_wds_start_try clears it as it reads it. */
static int mhi_wds_start_tech;

/* qcril's own Start Network dial, captured from the running phone (strace
 * on qcrild 2038/2074, 2026-11-23): auth none (0x16 = 0), the family
 * (0x19), the embedded call type (0x35 = 1), two one-byte flags
 * (0x39 = 1, 0x3d = 1) and an 8-byte APN type mask (0x3e, guint64 little
 * endian).  The mask is QmiWdsApnTypeMask (libqmi qmi-enums-wds.h):
 * 1 default, 2 ims.  Its frames carry no APN and no profile index -- the
 * modem picks the profile by the mask's APN type.  Flags consumed by the
 * next start like mhi_wds_start_tech; qcril also forces the Set IP Family
 * (0x004d) before the start, which qcril's frames show it always sends. */
static uint64_t mhi_wds_start_apn_mask;
static int mhi_wds_start_qcril;

/* The default data call's Start shape, captured from Android when Wi-Fi was
 * disabled and telephony brought the cbnet default PDN up (12:30:36,
 * qmiwifi.trace): auth none, the family, the embedded call type, the same
 * one-byte 0x39 = 1 as the IMS dial, and TLV 0x3c = 1 in place of the
 * dial's 0x3d = 1 -- and never the dial's 0x3e APN type mask.  Both of
 * Android's legs (family 4 and 6) were accepted with this shape (handles
 * 0x42acfe40 and 0x428e5520), while every Start this driver has sent used
 * the IMS shape and was refused result 1, error 14, verbose CM 1042 -- so
 * this flag is what tells the two call kinds apart on the wire.  Consumed
 * per start like the flags above. */
static int mhi_wds_start_data_shape;

/* The vendor IMS data service's own Start Network, captured from the
 * working phone's imsdatadaemon (imscap2 06:17:32.234-.335).  Its fresh WDS
 * client bound {type 3, interface 4} mux 3 and subscription 2, set IP family
 * 6, then sent a Start whose whole 22-byte body was APN "ims" (0x14), the
 * family (0x19 = 6), the 3GPP profile index 2 (0x31), one byte 0x32 = 0xff
 * and the embedded call type (0x35 = 1) -- no auth TLV, no 0x39/0x3d, no APN
 * type mask.  The modem took it (result 0, handle 0x748d0ee0), the IMS data
 * service answered the cap stack's APN request with the address 0.23 s later
 * and the modem's own IMS stack registered 0.04 s after that (IMSA 0x23
 * state 1 at 06:17:32.598, state 2 with the sip:/tel: URIs at 33.55).
 * Nothing this driver sends does that: every accepted leg so far carries
 * qcril's mask shape (0x16/0x39/0x3d/0x3e) instead, and IMSA stays at 0.
 * Consumed per start like the flags above. */
static int mhi_wds_start_daemon_ims;

/* The QRTR source port the next start's Set IP Family and Start Network go
 * out from -- the WDS client's identity, which on Android is one fresh qrtr
 * socket per role: the accepted IMS dial's client (the 02:18 two-way capture,
 * docs/cellular.md) had done nothing but its two binds and the family set,
 * while this driver's shared 0x1002 client carries every profile write,
 * attach set, bind and refused start of the boot and its identical start is
 * refused CM 1042.  0 keeps the long-lived 0x1002 client.  Consumed per
 * start like the flags above. */
static unsigned mhi_wds_start_client;

/* One Start Network attempt (0x0020), in the order wwand's hardware-verified
 * bring-up uses it.  Set IP Family (0x004d) goes first when a family is
 * named: a stack that expects the client's preference set refuses the start
 * with an internal error when it never was, and the reference sends Start
 * Network's own TLV 0x19 only when 0x004d itself was refused.  inline_family
 * flips that: the reference phone's own form sends TLV 0x19 and no 0x004d at
 * all.  profile >= 0 sends TLV 0x31 (the 3GPP profile index), apn (when not
 * NULL) sends TLV 0x14 with that APN and no authentication, and call_type
 * sends TLV 0x35 naming the call embedded -- the shape qcril dials a phone's
 * IMS PDN with
 * (QmiWdsCallType: 0 laptop, 1 embedded); the reference phone's start sends
 * no 0x35, so it is a parameter, not a constant.
 * Returns 1 when the modem accepted; the answer stays in the caller's
 * buffer and *got_out takes its size. */
static int
mhi_wds_start_try(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, int profile, unsigned family,
    const char *apn, int call_type, int inline_family, uint8_t txn,
    const char *what, uint8_t *answer, size_t answer_size, size_t *got_out)
{
	uint8_t request[96];
	size_t got, at;
	unsigned result = ~0u, error = ~0u;
	unsigned fres = ~0u, ferr = ~0u;
	int family_set = 0;
	int tech = mhi_wds_start_tech;
	uint64_t apn_mask = mhi_wds_start_apn_mask;
	int qcril = mhi_wds_start_qcril;
	int data_shape = mhi_wds_start_data_shape;
	int daemon_ims = mhi_wds_start_daemon_ims;
	unsigned client = mhi_wds_start_client;

	/* One start's worth: never let a forgotten flag leak into a later
	 * form. */
	mhi_wds_start_tech = 0;
	mhi_wds_start_apn_mask = 0;
	mhi_wds_start_qcril = 0;
	mhi_wds_start_data_shape = 0;
	mhi_wds_start_daemon_ims = 0;
	mhi_wds_start_client = 0;
	if (client == 0) {
		client = 0x1002;
	}

	if (family != 0xffu && (daemon_ims || !inline_family || qcril)) {
		uint8_t preference = (uint8_t)family;

		at = mhi_qmi_open(request, (uint8_t)(0x90 | (txn & 0x0fu)),
		    0x004d);
		at = mhi_qmi_put(request, at, 0x01, &preference, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, client, node, port, request,
		    (unsigned)at, "IPCR WDS set ip family", answer, answer_size);
		mhi_qmi_outcome(answer, got, &fres, &ferr);
		family_set = got != 0 && fres == 0;
		hakios_tl("pcie2: cellular: IMS: the set-ip-family %u before the start says result %u, error %u\n",
		    family, fres, ferr);
	}
	if (daemon_ims) {
		uint8_t ff = 0xff;

		/* The vendor IMS data service's whole body, in its byte order:
		 * the APN, the family (0x4d above did not suppress 0x19 on the
		 * captured start), the 3GPP profile index, 0x32 = 0xff and the
		 * embedded call type -- and nothing else. */
		at = mhi_qmi_open(request, txn, 0x0020);
		if (apn != NULL) {
			at = mhi_qmi_put(request, at, 0x14, apn, strlen(apn));
		}
		if (family != 0xffu) {
			uint8_t preference = (uint8_t)family;

			at = mhi_qmi_put(request, at, 0x19, &preference, 1);
		}
		if (profile >= 0) {
			uint8_t index = (uint8_t)profile;

			at = mhi_qmi_put(request, at, 0x31, &index, 1);
		}
		at = mhi_qmi_put(request, at, 0x32, &ff, 1);
		if (call_type) {
			uint8_t type = 0x01;	/* embedded */

			at = mhi_qmi_put(request, at, 0x35, &type, 1);
		}
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call_slow(out, in, client, node, port, request,
		    (unsigned)at, what, answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		*got_out = got;
		hakios_tl("pcie2: cellular: IMS: %s says result %u, error %u\n",
		    what, result, error);
		return got != 0 && result == 0;
	}
	at = mhi_qmi_open(request, txn, 0x0020);
	if (qcril && apn == NULL) {
		uint8_t none = 0x00;

		/* qcril's own byte order starts the body with auth none
		 * (0x16 = 0); run 013903 sent it after the family and the
		 * call type and was refused 1/14 with every other byte
		 * equal, so the order is replicated exactly. */
		at = mhi_qmi_put(request, at, 0x16, &none, 1);
	}
	if (apn != NULL) {
		uint8_t auth = 0x00;

		at = mhi_qmi_put(request, at, 0x14, apn, strlen(apn));
		at = mhi_qmi_put(request, at, 0x16, &auth, 1);
	}
	if (profile >= 0) {
		uint8_t index = (uint8_t)profile;

		at = mhi_qmi_put(request, at, 0x31, &index, 1);
	}
	if (family != 0xffu && (inline_family || !family_set)) {
		uint8_t preference = (uint8_t)family;

		at = mhi_qmi_put(request, at, 0x19, &preference, 1);
	}
	if (call_type) {
		uint8_t type = 0x01;	/* embedded */

		at = mhi_qmi_put(request, at, 0x35, &type, 1);
	}
	if (qcril) {
		uint8_t one = 0x01;

		at = mhi_qmi_put(request, at, 0x39, &one, 1);
		/* The two captured shapes differ in this byte and nothing
		 * else before it: the IMS dial carries 0x3d = 1, the default
		 * data call 0x3c = 1. */
		at = mhi_qmi_put(request, at, data_shape ? 0x3c : 0x3d, &one,
		    1);
	}
	if (apn_mask != 0) {
		at = mhi_qmi_put(request, at, 0x3e, &apn_mask, sizeof(apn_mask));
	}
	/* The technology the call may run on.  The firmware's ACL denials
	 * include ACL_DENY_REASON_UNPREFERRED_RAT, and no form from run
	 * 154032 or 165605 named one: TLV 0x30 (guint8 bitmask, 1 = 3GPP
	 * allowed) and TLV 0x34 (guint16, 34944 = 0x8880 EPC/LTE) are the
	 * two spellings a start carries. */
	if (tech == 1) {
		uint8_t allow = 0x01;

		at = mhi_qmi_put(request, at, 0x30, &allow, 1);
	} else if (tech == 2) {
		uint16_t epc = 34944;	/* QMI_WDS_EXTENDED_TECHNOLOGY_PREFERENCE_EPC */

		at = mhi_qmi_put(request, at, 0x34, &epc, sizeof(epc));
	}
	mhi_qmi_seal(request, at);
	/* A start waits on the network: the silent 5 s budget of the plain call
	 * is not enough for the modem to take a real dial. */
	got = mhi_qmi_call_slow(out, in, client, node, port, request,
	    (unsigned)at, what, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	*got_out = got;
	hakios_tl("pcie2: cellular: IMS: %s says result %u, error %u\n",
	    what, result, error);
	return got != 0 && result == 0;
}

/* QmiWdsConnectionStatus (qmi-enums-wds.h, libqmi 1.38): 0 unknown,
 * 1 disconnected, 2 connected, 3 suspended, 4 authenticating.  Run 113605's
 * table was shifted by one -- 1 was read as connected. */
static const char *
mhi_wds_conn_name(unsigned status)
{
	switch (status) {
	case 0:
		return "unknown";
	case 1:
		return "disconnected";
	case 2:
		return "connected";
	case 3:
		return "suspended";
	case 4:
		return "authenticating";
	default:
		return "not given";
	}
}

/* The data port the modem sends a session's packets to.  A modem whose data
 * path runs to the host has to be told where that port is before it will
 * start a session: on the reference phone, skipping DPM Open Port made WDA
 * answer InvalidOperation, the mux bind Internal and every Start Network
 * InvalidOperation (error 70) -- exactly what run 121242 saw on thyme
 * (docs/cellular.md).  DPM Open Port 0x0020 input TLV 0x11 (libqmi
 * qmi-service-dpm.json): a one-byte array count, then per hardware data port
 * {endpoint type u32, interface u32, the modem's receiving endpoint u32, its
 * transmitting one u32}.  The endpoint numbers are the host data engine's
 * (the reference phone's are its IPA's 2 and 16); this driver runs no IPA
 * yet, so the plausible pairs are probed and the first the modem accepts is
 * kept.  Returns 1 with *type_out and *iface_out set when a port opened. */
static int
mhi_wds_data_port(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned *type_out,
    unsigned *iface_out)
{
	/* QmiDataEndpointType (qmi-enums.h): 2 HSUSB, 3 PCIE, 4 EMBEDDED.
	 * The pair qcrild's own clients bind comes first: run 013903's
	 * DPM port was {4,1} and every start was refused 1042 CM_ACL_FAILURE,
	 * while the running phone's WDS clients bind {type 3 PCIe,
	 * interface 4} with mux 3 (strace 2026-11-23).  Then the embedded
	 * pair (the reference phone's), then the same pair named PCIe --
	 * this modem's data channels do run over MHI. */
	static const struct { unsigned type, iface, rx, tx; } tries[] = {
		{ 3, 4, 2, 16 },
		{ 4, 1, 2, 16 },
		{ 3, 1, 2, 16 },
		{ 4, 1, 22, 23 },
		{ 3, 1, 22, 23 },
		{ 3, 0, 2, 16 },
		{ 4, 0, 0, 0 },
	};
	uint8_t request[64];
	unsigned node, port, result = ~0u, error = ~0u;
	size_t at, got;

	if (!mhi_qrtr_service(0x2f, &node, &port)) {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced DPM; the data port is not opened\n");
		return 0;
	}
	for (unsigned i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
		uint8_t ports[17];
		unsigned words[4];

		words[0] = tries[i].type;
		words[1] = tries[i].iface;
		words[2] = tries[i].rx;
		words[3] = tries[i].tx;
		ports[0] = 0x01;	/* one hardware data port */
		for (unsigned w = 0; w < 4; w++) {
			ports[1 + w * 4] = (uint8_t)words[w];
			ports[2 + w * 4] = (uint8_t)(words[w] >> 8);
			ports[3 + w * 4] = (uint8_t)(words[w] >> 16);
			ports[4 + w * 4] = (uint8_t)(words[w] >> 24);
		}
		at = mhi_qmi_open(request, (uint8_t)(0xa0 + i), 0x0020);
		at = mhi_qmi_put(request, at, 0x11, ports, sizeof(ports));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR DPM open port", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: DPM open port (type %u, interface %u, endpoints %u/%u) says result %u, error %u\n",
		    tries[i].type, tries[i].iface, tries[i].rx, tries[i].tx,
		    result, error);
		if (got != 0 && result == 0) {
			*type_out = tries[i].type;
			*iface_out = tries[i].iface;
			hakios_tl("pcie2: cellular: IMS: THE DATA PORT IS OPEN (endpoint type %u, interface %u)\n",
			    tries[i].type, tries[i].iface);
			return 1;
		}
	}
	return 0;
}

/* The opened port's framing and the client's mux, in the reference phone's
 * order (hakios.mpss.c): WDA Set Data Format 0x0020 (TLV 0x11 link layer 2
 * raw IP, 0x12/0x13 UL/DL aggregation 5 QMAP, 0x15 at most 16 downlink
 * datagrams, 0x16 at most 0x3800 bytes per downlink aggregate, 0x17
 * {type, iface} the port), then WDS Bind Mux Data Port 0x00a2 (TLV 0x10
 * {type, iface}, 0x11 the mux id).  The bind is tried on mux 1 and then on
 * mux 0; a refusal is not fatal -- the reference phone starts the session
 * anyway. */
static void
mhi_wds_data_path(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned type, unsigned iface)
{
	uint8_t request[64];
	unsigned node, port, result = ~0u, error = ~0u;
	size_t at, got;

	if (mhi_qrtr_service(0x1a, &node, &port)) {
		uint32_t port_desc[2], link = 2, aggr = 5, datagrams = 16;
		uint32_t max_size = 0x3800;

		port_desc[0] = type;
		port_desc[1] = iface;
		at = mhi_qmi_open(request, 0xa8, 0x0020);
		at = mhi_qmi_put(request, at, 0x11, &link, sizeof(link));
		at = mhi_qmi_put(request, at, 0x12, &aggr, sizeof(aggr));
		at = mhi_qmi_put(request, at, 0x13, &aggr, sizeof(aggr));
		at = mhi_qmi_put(request, at, 0x15, &datagrams,
		    sizeof(datagrams));
		at = mhi_qmi_put(request, at, 0x16, &max_size, sizeof(max_size));
		at = mhi_qmi_put(request, at, 0x17, port_desc,
		    sizeof(port_desc));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDA set data format", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: WDA set data format (raw IP in QMAP, port type %u interface %u) says result %u, error %u\n",
		    type, iface, result, error);
	} else {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced WDA; the data format is skipped\n");
	}

	if (!mhi_qrtr_service(0x01, &node, &port)) {
		return;
	}
	/* The reference's own data-port client binds the mux on the fourth
	 * service right before its starts -- qmi5.dec fd=107: a name-service
	 * lookup of service 4 answered node 3 port 0x37, then Bind Mux Data
	 * Port 0x002b, TLV 0x10 {type 3, interface 4}, TLV 0x11 mux 2 ->
	 * result 0, then message 0x002d, TLV 0x01 = 2 -> result 0.  (libqmi
	 * calls service 4 QOS; the ids and TLVs are the data port mapper's.)
	 * This comment used to say DPM and the frames went to 0x2f: DPM
	 * answered error 57 both times -- invalid message id, since DPM
	 * implements only Open/Close Port -- so the experiment never actually
	 * ran.  The WDS bind below does not replace these: the CM access list
	 * may be keyed by this port mapping, and until now the driver never
	 * sent either message to the service the reference used.  The mux is
	 * the reference rebuild's 2. */
	if (mhi_qrtr_service(0x04, &node, &port)) {
		uint32_t qos_desc[2], qos_bearer = 2;
		uint8_t qos_mux = 2;

		qos_desc[0] = type;
		qos_desc[1] = iface;
		at = mhi_qmi_open(request, 0xab, 0x002b);
		at = mhi_qmi_put(request, at, 0x10, qos_desc,
		    sizeof(qos_desc));
		at = mhi_qmi_put(request, at, 0x11, &qos_mux, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR the reference's port bind", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the reference's port bind (mux %u on the {type %u, interface %u} port) says result %u, error %u\n",
		    qos_mux, type, iface, result, error);

		at = mhi_qmi_open(request, 0xac, 0x002d);
		at = mhi_qmi_put(request, at, 0x01, &qos_bearer,
		    sizeof(qos_bearer));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR the reference's port bind 0x002d",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the reference's 0x002d (TLV 0x01 = %u) says result %u, error %u\n",
		    qos_bearer, result, error);
	} else {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced the data port service (0x04); its bind is skipped\n");
	}

	/* The running phone's clients bind mux 3 (one legacy client mux 2)
	 * on the {3,4} port; run 013903 bound mux 1 and the CM refused every
	 * start, so the reference's own muxes come first. */
	static const unsigned mux_tries[] = { 3, 2, 1, 0 };

	for (unsigned i = 0; i < sizeof(mux_tries) / sizeof(mux_tries[0]); i++) {
		unsigned mux = mux_tries[i];
		uint32_t port_desc[2];
		uint8_t id = (uint8_t)mux;

		port_desc[0] = type;
		port_desc[1] = iface;
		at = mhi_qmi_open(request, (uint8_t)(0xa9 + i), 0x00a2);
		at = mhi_qmi_put(request, at, 0x10, port_desc,
		    sizeof(port_desc));
		at = mhi_qmi_put(request, at, 0x11, &id, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS bind mux data port", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the mux bind to mux %u of the data port says result %u, error %u\n",
		    mux, result, error);
		if (got != 0 && result == 0) {
			break;
		}
	}
}

/* The subscription the modem has this WDS client bound to (Get Bind
 * Subscription, 0x00b0; output TLV 0x10, QmiSubscriptionType: 0 default,
 * 1 primary, 2 secondary, 3 tertiary, 0xff any -- 1-based).  Returns ~0u
 * when the modem did not answer.  `client` is the QRTR source port of the
 * client asking: the fresh clients below read their own binding back. */
static unsigned
mhi_wds_bind_get_on(unsigned client, struct mhi_channel *out,
    struct mhi_channel *in, unsigned node, unsigned port, unsigned txn,
    uint8_t *answer, size_t answer_size)
{
	uint8_t request[16];
	size_t at, got;

	at = mhi_qmi_open(request, txn, 0x00b0);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR WDS get bind subscription", answer, answer_size);
	if (got == 0) {
		return ~0u;
	}
	return mhi_qmi_u32(answer, got, 0x10);
}

static unsigned
mhi_wds_bind_get(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, unsigned txn, uint8_t *answer, size_t answer_size)
{
	return mhi_wds_bind_get_on(0x1002, out, in, node, port, txn, answer,
	    answer_size);
}

static const char *
mhi_wds_subscription_name(unsigned subscription)
{
	switch (subscription) {
	case 0:
		return "default";
	case 1:
		return "primary";
	case 2:
		return "secondary";
	case 3:
		return "tertiary";
	case 0xff:
		return "any";
	case ~0u:
		return "no answer";
	default:
		return "unknown";
	}
}

/* One profile-id list as Get LTE Attach PDN List (0x0094) carries it: a
 * u8 count, then that many u16 ids little-endian (libqmi's definition;
 * Set's 0x0093 input TLV 0x01 has the same shape).  Returns how many ids
 * were parsed into `ids`. */
static unsigned
mhi_wds_attach_ids(const uint8_t *tlv, size_t size, uint16_t *ids,
    unsigned room)
{
	unsigned count, i;

	if (tlv == NULL || size < 1) {
		return 0;
	}
	count = tlv[0];
	if (count > (unsigned)(size - 1) / 2) {
		count = (unsigned)(size - 1) / 2;
	}
	if (count > room) {
		count = room;
	}
	for (i = 0; i < count; i++) {
		ids[i] = (uint16_t)(tlv[1 + i * 2] |
		    (unsigned)tlv[2 + i * 2] << 8);
	}
	return count;
}

/* Get LTE Attach Parameters (0x0085) once: TLV 0x10 the APN as a plain
 * string, 0x11 the IP type, 0x12 whether an attach ran (all u8 except the
 * APN).  Returns 1 when the modem answered successfully; the out
 * parameters take what it said.  A refused read is logged with its QMI
 * result (run 172318 answered result 1, error 74 InformationUnavailable
 * after the attach-list set, which a "(got != 0)" check misread as "the
 * values were not in the answer"). */
static int
mhi_wds_attach_params(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, uint8_t txn, uint8_t *answer,
    size_t answer_size, unsigned *attached, unsigned *iptype, char *apn,
    size_t apn_room)
{
	uint8_t request[32];
	const uint8_t *value;
	size_t got, at, size, n;
	unsigned result = ~0u, error = ~0u;

	at = mhi_qmi_open(request, txn, 0x0085);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS LTE attach parameters", answer, answer_size);
	if (got == 0) {
		return 0;
	}
	mhi_qmi_outcome(answer, got, &result, &error);
	if (result != 0) {
		hakios_tl("pcie2: cellular: IMS: the attach read says result %u, error %u\n",
		    result, error);
		return 0;
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x11, &size);
	if (value != NULL && size >= 1) {
		*iptype = value[0];
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x12, &size);
	if (value != NULL && size >= 1) {
		*attached = value[0];
	}
	if (apn != NULL && apn_room > 0) {
		apn[0] = 0;
		value = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
		if (value != NULL) {
			n = size < apn_room - 1 ? size : apn_room - 1;
			memcpy(apn, value, n);
			apn[n] = 0;
		}
	}
	return 1;
}

/* The attach the network settles on is built from the default profile as
 * it stands when the modem registers, not after it.  The default profile,
 * the attach list and the attach parameters are PER-SUBSCRIPTION state,
 * and the SIM answers on subscription 2 (QMI's 1-based id: Android's
 * dumpsys isub on this phone says logical slot 1, subId 1,
 * defaultDataSubId 1; every qcrild WDS client in the 11:06 capture binds
 * 2).  Every earlier run sent the steering below on a client bound to
 * subscription 1 -- the empty slot -- so the SIM's own default profile
 * never changed and the modem attached on its own configuration (APN
 * "ims", run 103830's 0x0085).  The client is bound to 2 first now, and
 * then qcril's boot set runs: the radio log and the 09:44 boot capture
 * show QMI_WDS_SET_DEFAULT_PROFILE_NUM_V01 for 1 and then 7, ending at
 * 7.  Android NEVER sends Set LTE Attach PDN List (0x0093) -- the list
 * is read only.  when names the stage in every line.  The cellular poll
 * retries when WDS was not announced yet.  Returns 1 when the bind, the
 * sets and the read-backs answered as expected. */
static int
mhi_wds_early(const char *when, struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size)
{
	uint8_t request[32];
	const uint8_t *see;
	char label[80];
	size_t got, at, size = 0;
	unsigned node = 0, port = 0;
	unsigned result = ~0u, error = ~0u;
	unsigned said;
	uint32_t binding;	/* QMI's 1-based id: the SIM's, set below */
	uint8_t which[3] = { 0x00, 0x00, 0x01 };
	uint8_t readback[2] = { 0x00, 0x00 };
	uint16_t ids[4];
	unsigned count, attached = ~0u, iptype = ~0u;
	char apn[24] = "";
	int okay = 1;

	/* The subscription the WDS clients bind: QMI's 1-based id of the
	 * card.  2 while the card sits where the modem found it -- Android's
	 * posture, kept by pcie2_uiswitch=0; the UIM block's switch and
	 * Change Provisioning Session move the card to logical slot 1 and
	 * set the variable to 1 (mhi_uim_provision).  pcie2_cardsub forces
	 * either value.  Run 093352's binds every one carried 1 while this
	 * source's default reads 2, so the posture the boot actually runs
	 * with is logged once, whatever the wiring turns out to have been. */
	{
		static int posture_said;

		if (!posture_said) {
			int forced = 0;

			posture_said = 1;
			PE_parse_boot_argn("pcie2_cardsub", &forced, sizeof(forced));
			if (forced == 1 || forced == 2) {
				mhi_wds_card_sub = (unsigned)forced;
			}
			hakios_tl("pcie2: cellular: the WDS clients bind subscription %u this boot (UIM switch %s, pcie2_cardsub %d)\n",
			    mhi_wds_card_sub,
			    mhi_uim_switch_allowed() ? "on" : "off", forced);
		}
	}
	binding = mhi_wds_card_sub;

	if (!mhi_qrtr_service(0x01, &node, &port)) {
		hakios_tl("pcie2: cellular: early (%s): the modem has not announced WDS; the attach steering waits\n",
		    when);
		return 0;
	}
	at = mhi_qmi_open(request, 0x70, 0x00af);
	at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS bind subscription %u (early, %s)", binding, when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: early (%s): the bind to subscription %u (the SIM's) says result %u, error %u%s\n",
	    when, binding, result, error, got == 0 ? " (no answer)" : "");
	if (got == 0 || result != 0) {
		okay = 0;
	}
	said = mhi_wds_bind_get(out, in, node, port, 0x71, answer, answer_size);
	hakios_tl("pcie2: cellular: early (%s): after the bind the client reads subscription %u (%s)\n",
	    when, said, mhi_wds_subscription_name(said));
	if (said != binding) {
		okay = 0;
	}
	at = mhi_qmi_open(request, 0x7e, 0x0049);
	at = mhi_qmi_put(request, at, 0x01, readback, sizeof(readback));
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS default profile number (early, %s)", when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	see = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
	if (see != NULL && size >= 1) {
		hakios_tl("pcie2: cellular: early (%s): before the set the default 3GPP profile reads %u\n",
		    when, see[0]);
	} else {
		hakios_tl("pcie2: cellular: early (%s): before the set the default profile did not answer\n",
		    when);
	}
	at = mhi_qmi_open(request, 0x7f, 0x004a);
	at = mhi_qmi_put(request, at, 0x01, which, sizeof(which));
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS set default profile number (early, %s)", when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: early (%s): the default 3GPP profile is set to 1: result %u, error %u%s\n",
	    when, result, error, got == 0 ? " (no answer)" : "");
	if (got == 0 || result != 0) {
		okay = 0;
	}
	/* qcril's boot sets 1 and then 7; its dial sees 7.  The sets are
	 * made here, before NAS registers, because the attach is built then.
	 * No Set LTE Attach PDN List: Android never sends 0x0093, and the
	 * modem then uses the default profile above. */
	which[2] = 0x07;
	at = mhi_qmi_open(request, 0x83, 0x004a);
	at = mhi_qmi_put(request, at, 0x01, which, sizeof(which));
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS set default profile number 7 (early, %s)", when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: early (%s): the default 3GPP profile is set to 7: result %u, error %u%s\n",
	    when, result, error, got == 0 ? " (no answer)" : "");
	if (got == 0 || result != 0) {
		okay = 0;
	}
	at = mhi_qmi_open(request, 0x84, 0x0049);
	at = mhi_qmi_put(request, at, 0x01, readback, sizeof(readback));
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS default profile number (after the sets, %s)", when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	see = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
	if (see != NULL && size >= 1) {
		hakios_tl("pcie2: cellular: early (%s): after the sets the default 3GPP profile reads %u\n",
		    when, see[0]);
	} else {
		hakios_tl("pcie2: cellular: early (%s): after the sets the default profile did not answer\n",
		    when);
	}
	at = mhi_qmi_open(request, 0x81, 0x0094);
	mhi_qmi_seal(request, at);
	snprintf(label, sizeof(label),
	    "IPCR WDS LTE attach PDN list (early, %s)", when);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    label, answer, answer_size);
	see = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	count = mhi_wds_attach_ids(see, see != NULL ? size : 0, ids,
	    sizeof(ids) / sizeof(ids[0]));
	if (count > 0) {
		hakios_tl("pcie2: cellular: early (%s): the attach PDN list names %u profiles, first %u%s\n",
		    when, count, ids[0], ids[0] == 1 ? " (cbnet)" : "");
	} else {
		hakios_tl("pcie2: cellular: early (%s): the attach PDN list did not answer\n",
		    when);
	}
	if (mhi_wds_attach_params(out, in, node, port, 0x82, answer, answer_size,
	    &attached, &iptype, apn, sizeof(apn))) {
		hakios_tl("pcie2: cellular: early (%s): the attach at this stage is attached %u, ip type %u, APN \"%s\"\n",
		    when, attached, iptype, apn);
	}
	return okay;
}

/* A profile read whole (Get Profile Settings 0x002b, input TLV 0x01
 * {type, index}; QmiWdsProfileType 0 is the 3GPP store, 1 the 3GPP2 one,
 * and the captures show Android reading both).  Run 113605's survey listed
 * index 6 as "qdp_profile_ia"; run 124850's Create Profile wrote the APN
 * "ims" into it (its comment below), and every attach since has carried
 * "ims" while the CM's access list refused cbnet Starts (1042).  Read
 * only: name (0x10), APN (0x14), PDP type (0x11), IMCN (0x22), and the APN
 * type mask (0xdd) -- qcril's own ims profile carries mask 2 there, and a
 * Start Network that names no profile is resolved by that mask. */
static int
mhi_wds_profile_read(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, unsigned type, unsigned index,
    const char *when, uint8_t *answer, size_t answer_size)
{
	uint8_t request[32], id[2];
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u;

	id[0] = (uint8_t)type;
	id[1] = (uint8_t)index;
	at = mhi_qmi_open(request, 0xe8, 0x002b);
	at = mhi_qmi_put(request, at, 0x01, id, sizeof(id));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call_slow(out, in, 0x1002, node, port, request,
	    (unsigned)at, "IPCR WDS profile read", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got == 0 || result != 0) {
		hakios_tl("pcie2: cellular: IMS: profile %u of type %u (%s) reads result %u, error %u%s\n",
		    index, type, when, result, error,
		    got == 0 ? " (no answer)" : "");
		return 0;
	}
	{
		const uint8_t *name = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
		size_t nsize = name != NULL ? size : 0;
		const uint8_t *apn = mhi_qmi_answer_tlv(answer, got, 0x14, &size);
		unsigned alen = apn != NULL ? (unsigned)size : 0;
		const uint8_t *pdp = mhi_qmi_answer_tlv(answer, got, 0x11, &size);
		unsigned pdpv = pdp != NULL && size >= 1 ? pdp[0] : ~0u;
		const uint8_t *imcn = mhi_qmi_answer_tlv(answer, got, 0x22, &size);
		unsigned imcnv = imcn != NULL && size >= 1 ? imcn[0] : ~0u;

		const uint8_t *mask = mhi_qmi_answer_tlv(answer, got, 0xdd, &size);
		char masktext[24] = "none";

		if (mask != NULL) {
			uint64_t maskv = 0;

			for (unsigned i = 0; i < size && i < 8; i++) {
				maskv |= (uint64_t)mask[i] << (8 * i);
			}
			snprintf(masktext, sizeof(masktext), "%#llx",
			    (unsigned long long)maskv);
		}
		if (nsize > 63) {
			nsize = 63;
		}
		hakios_tl("pcie2: cellular: IMS: profile %u of type %u (%s) is \"%.*s\", APN \"%.*s\", PDP type %u, IMCN %u, the APN type mask %s\n",
		    index, type, when, (int)nsize,
		    name != NULL ? (const char *)name : "", (int)alen,
		    apn != NULL ? (const char *)apn : "", pdpv, imcnv,
		    masktext);
	}
	return 1;
}

/* Repair the initial-attach profile's APN (Modify Profile 0x0028, input
 * TLV 0x01 {type 3GPP, index}, 0x10 name, 0x11 PDP type 3 IPv4v6, 0x14
 * APN, 0x22 IMCN flag 0).  The slot may be hidden from the 0x002a list,
 * so a refused modify falls back to Create Profile 0x0027 with the same
 * fields -- the write this store accepted in runs 113605 and 124850.
 * Returns 1 when either write was accepted. */
static int
mhi_wds_profile_apn(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, unsigned index, const char *name,
    const char *apn, uint8_t *answer, size_t answer_size)
{
	uint8_t request[64], id[2] = { 0x00, 0x00 };
	uint8_t pdp = 0x03, imcn = 0x00;
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u;

	id[1] = (uint8_t)index;
	at = mhi_qmi_open(request, 0xe9, 0x0028);
	at = mhi_qmi_put(request, at, 0x01, id, sizeof(id));
	at = mhi_qmi_put(request, at, 0x10, name, (unsigned)strlen(name));
	at = mhi_qmi_put(request, at, 0x11, &pdp, 1);
	at = mhi_qmi_put(request, at, 0x14, apn, (unsigned)strlen(apn));
	at = mhi_qmi_put(request, at, 0x22, &imcn, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS modify initial attach profile", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the initial attach profile %u is modified to \"%s\" (APN \"%s\"): result %u, error %u%s\n",
	    index, name, apn, result, error, got == 0 ? " (no answer)" : "");
	if (got != 0 && result == 0) {
		return 1;
	}
	at = mhi_qmi_open(request, 0xea, 0x0027);
	{
		uint8_t type = 0x00;

		at = mhi_qmi_put(request, at, 0x01, &type, 1);
	}
	at = mhi_qmi_put(request, at, 0x10, name, (unsigned)strlen(name));
	at = mhi_qmi_put(request, at, 0x11, &pdp, 1);
	at = mhi_qmi_put(request, at, 0x14, apn, (unsigned)strlen(apn));
	at = mhi_qmi_put(request, at, 0x22, &imcn, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS create the initial attach profile", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got != 0 && result == 0) {
		const uint8_t *value = mhi_qmi_answer_tlv(answer, got, 0x01, &size);

		if (value != NULL && size >= 2) {
			hakios_tl("pcie2: cellular: IMS: the initial attach profile is written (APN \"%s\") as %u of type %u\n",
			    apn, value[1], value[0]);
		} else {
			hakios_tl("pcie2: cellular: IMS: the create of the initial attach profile (APN \"%s\") answered without an index\n",
			    apn);
		}
		return 1;
	}
	hakios_tl("pcie2: cellular: IMS: the initial attach profile could not be written: result %u, error %u%s\n",
	    result, error, got == 0 ? " (no answer)" : "");
	return 0;
}

/* Set the LTE attach PDN list to one profile with an explicit action
 * (Set LTE Attach PDN List 0x0093, TLV 0x01 {count 1, u16 id}, TLV 0x10
 * the u32 action: 0x01 none, 0x02 detach-or-disconnect) and read the
 * list and the attach back: the action 0x02 form is what makes the modem
 * rebuild the attach now instead of at the next boot. */
static void
mhi_wds_attach_force(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, unsigned index, uint32_t action,
    uint8_t *answer, size_t answer_size)
{
	uint8_t request[32];
	const uint8_t *value;
	size_t got, at, size = 0;
	unsigned result = ~0u, error = ~0u;
	uint8_t list[3];
	uint16_t ids[6];
	unsigned count;

	list[0] = 0x01;
	list[1] = (uint8_t)(index & 0xff);
	list[2] = (uint8_t)(index >> 8);
	at = mhi_qmi_open(request, 0xeb, 0x0093);
	at = mhi_qmi_put(request, at, 0x01, list, sizeof(list));
	at = mhi_qmi_put(request, at, 0x10, &action, sizeof(action));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS set LTE attach PDN list (forced re-attach)", answer,
	    answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the attach PDN list is set to %u with action %u (the modem rebuilds the attach): result %u, error %u%s\n",
	    index, action, result, error, got == 0 ? " (no answer)" : "");
	at = mhi_qmi_open(request, 0xec, 0x0094);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS LTE attach PDN list (after the forced set)", answer,
	    answer_size);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	count = mhi_wds_attach_ids(value, value != NULL ? size : 0, ids,
	    sizeof(ids) / sizeof(ids[0]));
	if (count > 0) {
		hakios_tl("pcie2: cellular: IMS: after the forced set the attach PDN list names %u profiles, first %u\n",
		    count, ids[0]);
	} else {
		hakios_tl("pcie2: cellular: IMS: after the forced set the attach PDN list did not answer\n");
	}
}

/* WDS: bind the client to a subscription (the primary first, the secondary
 * when no bind answered and the read-back does not name the primary), read
 * what the firmware and the modem say about themselves (supported messages, the packet
 * service's state, the default profile, the LTE attach the network settled
 * on), survey the profiles for the ims APN, create one when none carries it,
 * try Start Network in the forms qcril and wwand's hardware-verified
 * bring-up use (Set IP Family first, the embedded call type, then by profile,
 * by the modem's own default profile and by APN), and poll the packet
 * service until the PDN is connected.  Returns 1 when a start was accepted
 * or the modem already had a session up. */
/* The CMCC experiment's undo: the config the modem booted with is
 * selected and activated again, and read back.  The refused fresh-client
 * matrix calls this before it returns, so after a refused matrix the
 * modem does not sit on CMCC for the rest of the boot; after an accepted
 * start the next call of mhi_wds_ims_up restores it instead, so the live
 * session is not disturbed by the config switch. */
static void
mhi_pdc_restore_own(struct mhi_channel *out, struct mhi_channel *in)
{
	unsigned node = 0, port = 0;

	if (!mhi_pdc_experiment_selected || mhi_pdc_experiment_restored) {
		return;
	}
	if (mhi_pdc_experiment_kept) {
		/* The selection stays for the whole boot: the IMS stack
		 * binds, asks and registers under the carrier's config. */
		mhi_pdc_experiment_restored = 1;
		hakios_tl("pcie2: cellular: PDC: the CMCC selection is kept for this boot (pcie2_pdc_keep=1); the modem's own config is not restored\n");
		return;
	}
	if (!mhi_qrtr_service(0x24, &node, &port)) {
		return;
	}
	{
		uint8_t request[64];
		uint8_t with_id[32];
		uint32_t wtype = 1, token;
		size_t at;

		memcpy(with_id, &wtype, sizeof(wtype));
		with_id[4] = mhi_pdc_cbn_len;
		memcpy(with_id + 5, mhi_pdc_cbn_id, mhi_pdc_cbn_len);
		token = 0x5d5d0001;
		at = mhi_qmi_open(request, 0xf2, 0x0023);
		at = mhi_qmi_put(request, at, 0x01, with_id,
		    5 + mhi_pdc_cbn_len);
		at = mhi_qmi_put(request, at, 0x10, &token, sizeof(token));
		mhi_qmi_seal(request, at);
		mhi_pdc_exchange(out, in, node, port, request, (unsigned)at,
		    "IPCR PDC set selected config (the modem's own)", 0x0023,
		    2500);

		token = 0x5d5d0002;
		at = mhi_qmi_open(request, 0xf3, 0x0027);
		at = mhi_qmi_put(request, at, 0x01, &wtype, sizeof(wtype));
		at = mhi_qmi_put(request, at, 0x10, &token, sizeof(token));
		mhi_qmi_seal(request, at);
		mhi_pdc_exchange(out, in, node, port, request, (unsigned)at,
		    "IPCR PDC activate config (the modem's own)", 0x0027, 3000);
		delay(2000000);

		token = 0x5d5d0003;
		at = mhi_qmi_open(request, 0xf4, 0x0022);
		at = mhi_qmi_put(request, at, 0x01, &wtype, sizeof(wtype));
		at = mhi_qmi_put(request, at, 0x10, &token, sizeof(token));
		mhi_qmi_seal(request, at);
		mhi_pdc_exchange(out, in, node, port, request, (unsigned)at,
		    "IPCR PDC get selected config (restored)", 0x0022, 2500);
		mhi_pdc_experiment_restored = 1;
		hakios_tl("pcie2: cellular: PDC: the modem's own config is selected again; the CMCC selection is undone\n");
	}
}

/* ---- DSD: the data system service the CM's access list answers to ----
 *
 * Android's qmipriod holds a DSD client (service 0x2A, node 3 port 0x3b in
 * the 02:18 airplane-mode capture) and its conversation is the last host
 * action before qcrild's accepted WDS Start, decoded byte-exactly from the
 * capture: System Status Change (0x0025) with TLV 0x10 = 1, vendor message
 * 0x0069 with TLV 0x10 = 0, 0x0025 with TLV 0x10 = 0, Get System Status
 * (0x0024) -- whose vendor TLVs 0x11 and 0x13 carry the APN list
 * cbnet/CMNET/CMWAP/ims/SOS with the enabled flags, TLV 0x10 the one
 * available system -- then 0x0069 with TLV 0x10/0x11/0x12 = 1 (answer
 * result 0) and 0x0068 (answer result 0, TLV 0x13 = a u16 10 or 11).
 * The replay goes out before the first Start: run 083846 put it after the
 * fresh clients' starts, they were refused 1042, the function returned
 * early and the replay never ran.  Every HakiOS
 * Start so far was denied CM_ACL_FAILURE (verbose reason 1042) while
 * nothing on this host ever addressed DSD -- the announcement whitelist
 * dropped it, so the table never carried it either.  The run replays the
 * reference's frames in its order and re-reads the list afterwards; the
 * APN flags and the Start matrix that follows say whether the access list
 * is what denied every call.
 */

/* The whole APN list TLV in hex lines: the enabled flag sits deep in a
 * 130-byte value that the generic dump shows only twelve bytes of. */
static void
mhi_dsd_apn_dump(const uint8_t *frame, size_t length, unsigned type,
    const char *what)
{
	const uint8_t *value;
	size_t size = 0, at;

	value = mhi_qmi_answer_tlv(frame, length, type, &size);
	for (at = 0; value != NULL && at < size && at < 240; at += 44) {
		char hex[3 * 44 + 1];
		size_t n = size - at > 44 ? 44 : size - at;

		for (size_t i = 0; i < n; i++) {
			snprintf(hex + i * 3, 4, " %02x", value[at + i]);
		}
		hex[n * 3] = 0;
		hakios_tl("pcie2: cellular: %s: TLV %#04x, %zu bytes at %zu:%s\n",
		    what, type, size, at, hex);
	}
}

static void
mhi_dsd_arm(struct mhi_channel *out, struct mhi_channel *in, uint8_t *answer,
    size_t answer_size)
{
	uint8_t request[32];
	unsigned node = 0, port = 0, result = ~0u, error = ~0u;
	size_t got, at;
	static bool replayed;

	/* One replay per boot: the conversation arms the CM's access list and
	 * the reference's clients send it once when they register, not before
	 * every Start.  The guard also keeps the fallback call site (for the
	 * no-data-path case) from sending it a second time. */
	if (replayed) {
		hakios_tl("pcie2: cellular: DSD: the access-list conversation was already replayed this boot; not again\n");
		return;
	}
	if (!mhi_qrtr_service(0x2a, &node, &port)) {
		uint8_t control[20], frame[64];
		size_t length;

		/* The boot's announcements were heard while the table still
		 * dropped unknown services; ask the name service where DSD
		 * is, in the lookup form the reference's clients use
		 * (service 0x2a, instance 0 -- a wildcard on the match). */
		mhi_qrtr_control(control, 10, 0x2a);
		length = mhi_qrtr_put3(frame, 10, 0xfffe, 0xffff, 0xfffe,
		    control, sizeof(control));
		(void)mhi_send(out, frame, length);
		for (unsigned round = 0; round < 10; round++) {
			if (mhi_qrtr_service(0x2a, &node, &port)) {
				break;
			}
			(void)mhi_qrtr_pump(out, in, 200,
			    "while the DSD lookup is out");
		}
	}
	if (!mhi_qrtr_service(0x2a, &node, &port)) {
		hakios_tl("pcie2: cellular: DSD: no announcement and the lookup drew no NEW_SERVER; the arming is skipped\n");
		return;
	}
	replayed = true;
	hakios_tl("pcie2: cellular: DSD: the service is at %u:%#x; replaying the reference's access-list conversation\n",
	    node, port);

	{
		static const uint8_t on = 1;

		at = mhi_qmi_open(request, 0x1df, 0x0025);
		at = mhi_qmi_put(request, at, 0x10, &on, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1007, node, port, request,
		    (unsigned)at,
		    "IPCR DSD system status change (the reference's first, 1)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: DSD: the first status change (1) says result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
	}
	{
		static const uint8_t off = 0;

		at = mhi_qmi_open(request, 0x1e0, 0x0069);
		at = mhi_qmi_put(request, at, 0x10, &off, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1007, node, port, request,
		    (unsigned)at,
		    "IPCR DSD set (0x0069, the reference's early zero)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: DSD: the early set (0x0069, zero) says result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
	}
	{
		static const uint8_t off = 0;

		at = mhi_qmi_open(request, 0x1e1, 0x0025);
		at = mhi_qmi_put(request, at, 0x10, &off, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1007, node, port, request,
		    (unsigned)at, "IPCR DSD system status change (0)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: DSD: the status change (0) says result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
	}
	at = mhi_qmi_open(request, 0x1e2, 0x0024);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1007, node, port, request, (unsigned)at,
	    "IPCR DSD get system status", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: DSD: the system status says result %u, error %u%s\n",
	    result, error, got == 0 ? " (no answer)" : "");
	if (got != 0) {
		mhi_qmi_tlvs(answer, got, "IPCR DSD get system status");
		mhi_dsd_apn_dump(answer, got, 0x11,
		    "IPCR DSD get system status");
		mhi_dsd_apn_dump(answer, got, 0x13,
		    "IPCR DSD get system status");
	}
	{
		static const uint8_t one = 1;

		at = mhi_qmi_open(request, 0x1e3, 0x0069);
		at = mhi_qmi_put(request, at, 0x10, &one, 1);
		at = mhi_qmi_put(request, at, 0x11, &one, 1);
		at = mhi_qmi_put(request, at, 0x12, &one, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1007, node, port, request,
		    (unsigned)at,
		    "IPCR DSD set (0x0069, the reference's three ones)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: DSD: the reference's set (0x0069, three ones) says result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
	}
	at = mhi_qmi_open(request, 0x1e4, 0x0068);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1007, node, port, request, (unsigned)at,
	    "IPCR DSD get (0x0068)", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: DSD: the reference's get (0x0068) says result %u, error %u%s\n",
	    result, error, got == 0 ? " (no answer)" : "");
	if (got != 0) {
		mhi_qmi_tlvs(answer, got, "IPCR DSD get (0x0068)");
	}
	at = mhi_qmi_open(request, 0x1e5, 0x0024);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1007, node, port, request, (unsigned)at,
	    "IPCR DSD get system status (after the set)", answer,
	    answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: DSD: the system status after the set says result %u, error %u%s\n",
	    result, error, got == 0 ? " (no answer)" : "");
	if (got != 0) {
		mhi_qmi_tlvs(answer, got,
		    "IPCR DSD get system status (after the set)");
		mhi_dsd_apn_dump(answer, got, 0x11,
		    "IPCR DSD get system status (after the set)");
		mhi_dsd_apn_dump(answer, got, 0x13,
		    "IPCR DSD get system status (after the set)");
	}
}

/* The modem's WDS route look up (vendor message 0xfff2), the request
 * Android's netmgrd sends to the modem before every default-data dial.
 * Captured from the live phone at 14:02 with LTE forced: netmgrd's WDS
 * client binds subscription 2, then sends 0xfff2 twice with bodies
 *   0x01=01, 0x14="cbnet", 0x19=04 (then 06), 0x35=01, 0x3c=01
 * and the modem answers result 0 with TLV 0x01 a ten-byte route value
 * (family 4: 00ffff06110000008188, family 6: 00ffff6d040000008088) and
 * TLV 0x11 five zeros.  The host side is netmgr_qmi_wds_route_look_up
 * (qmi_client_send_raw_msg_sync of 0xfff2); the firmware handler is
 * qmi_wdsi_route_look_up over ps_route_look_up_iface_by_policy(), the
 * same policy lookup the CM ACL runs -- if the ACL denies our Start
 * because the PS policy has no route for this host's APN, this call is
 * what asks the modem to make one.  Returns 1 when the modem answered
 * result 0. */
static int
mhi_wds_route_look_up(struct mhi_channel *out, struct mhi_channel *in,
    unsigned node, unsigned port, unsigned client, unsigned family,
    const char *apn, uint8_t txn, const char *what, uint8_t *answer,
    size_t answer_size, size_t *got_out)
{
	uint8_t request[64];
	static const uint8_t one = 1;
	uint8_t fam = (uint8_t)family;
	char hex[3 * 16 + 1];
	size_t got, at, size = 0, n, i;
	unsigned result = ~0u, error = ~0u;
	const uint8_t *route;

	at = mhi_qmi_open(request, txn, 0xfff2);
	at = mhi_qmi_put(request, at, 0x01, &one, 1);
	at = mhi_qmi_put(request, at, 0x14, apn, strlen(apn));
	at = mhi_qmi_put(request, at, 0x19, &fam, 1);
	at = mhi_qmi_put(request, at, 0x35, &one, 1);
	at = mhi_qmi_put(request, at, 0x3c, &one, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call_slow(out, in, client, node, port, request,
	    (unsigned)at, what, answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	route = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
	if (route != NULL) {
		n = size > 16 ? 16 : size;
		for (i = 0; i < n; i++) {
			snprintf(hex + i * 3, 4, " %02x", route[i]);
		}
		hex[n * 3] = 0;
		hakios_tl("pcie2: cellular: IMS: %s: result %u, error %u, route TLV 0x01 (%zu bytes):%s\n",
		    what, result, error, size, hex);
	} else {
		hakios_tl("pcie2: cellular: IMS: %s: result %u, error %u, no route TLV%s\n",
		    what, result, error, got == 0 ? " (no answer)" : "");
	}
	if (got == 0 || result != 0) {
		mhi_qmi_tlvs(answer, got, what);
	}
	*got_out = got;
	return got != 0 && result == 0;
}

/* The IPv4 address of a family-4 leg's started session, from a Get Current
 * Settings (0x002d) answer: TLV 0x1e.  This firmware's address TLVs are
 * bare value bytes (the IPv6 one, 0x26, was sixteen address bytes and then
 * the prefix), so the IPv4 address is the first four bytes; the caller logs
 * the raw bytes so a masked form would be visible.  The text goes into
 * mhi_imsdcm_address for mhi_imsdcm_indicate, and a rebuilt address is news
 * to the stack if it already had the old one. */
static int
mhi_wds_v4_address(const uint8_t *answer, size_t got)
{
	const uint8_t *see;
	size_t size;

	see = mhi_qmi_answer_tlv(answer, got, 0x1e, &size);
	if (see == NULL || size < 4) {
		return 0;
	}
	snprintf(mhi_imsdcm_address, sizeof(mhi_imsdcm_address),
	    "%u.%u.%u.%u", see[0], see[1], see[2], see[3]);
	mhi_imsdcm_indicated = false;
	return 1;
}

static int
mhi_wds_ims_up(struct mhi_channel *out, struct mhi_channel *in, uint8_t *answer,
    size_t answer_size)
{
	uint8_t request[96];
	const uint8_t *value;
	size_t got = 0, at, size;
	unsigned node, port, result = ~0u, error = ~0u;
	unsigned handle = ~0u, cause = ~0u, connected = ~0u, status = ~0u;
	unsigned default_profile = ~0u, bound = ~0u, said = ~0u;
	/* The DPM port the data path opened; the fresh client below binds its
	 * mux on the same port. */
	unsigned data_type = ~0u, data_iface = ~0u;
	uint32_t binding = mhi_wds_card_sub;
	int bind_works = 0;
	int ims_profile, data_profile = -1, accepted = 0, control = 0;
	/* Whether the attach answered the read this function made. */
	int attach_answered = 0;
	char attach_apn[24] = "";
	/* The modem's profile store stays read-only: see the call site. */
	static const int write_profile = 0;
	/* pcie2_ims_v4: the IMS data service's own start shape (APN "ims",
	 * profile 2, 0x32 = 0xff, the embedded call type) is captured with
	 * family 6; with this flag the same shape runs family 4 first, so the
	 * PDN's address matches the family this firmware's stack asks (the
	 * ask's 0x11 word is 0).  The captured V6 leg follows a refusal so
	 * the boot still gets its PDN either way.  Off by default. */
	uint32_t ims_v4_on = 0;
	/* pcie2_ims_f0v4: the accepted leg f0 itself (APN "ims", profile 2,
	 * mux 4, 0x32 = 0xff) runs family 4 first instead of 6; the captured
	 * V6 shape follows a refusal, so the boot still gets its PDN either
	 * way.  Run 052215's accepted f0 was family 6 and the stack still
	 * failed with 808; this leg says whether the PDN's family is what the
	 * stack checks.  Off by default. */
	uint32_t ims_f0v4_on = 0;

	PE_parse_boot_argn("pcie2_ims_v4", &ims_v4_on, sizeof(ims_v4_on));
	PE_parse_boot_argn("pcie2_ims_f0v4", &ims_f0v4_on, sizeof(ims_f0v4_on));

	/* Android configures DSD (0x2a) with the 0x27 bind and the feature
	 * words before its IMS registers; this driver only replays the
	 * access-list subset (mhi_dsd_arm, whose messages all answer result 0
	 * and leave IMSA at state 0).  The rest of Android's stream is
	 * replayed here, ahead of the PDN and the registration waits that
	 * follow every call of this function (once per boot). */
	mhi_dsd_settings_client(out, in, answer, answer_size);

	if (!mhi_qrtr_service(0x01, &node, &port)) {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced WDS; the PDN is skipped\n");
		return 0;
	}
	/* Bind Subscription (0x00af, TLV 1 = QmiSubscriptionType, 1-based:
	 * 0 default, 1 primary, 2 secondary).  The SIM's subscription is
	 * mhi_wds_card_sub: 2 (secondary) while the card sits in the slot
	 * the modem found it in, and 1 (primary) once the UIM block has
	 * switched the card to logical slot 1 and provisioned it -- run
	 * 021620 proved the CM ACL follows the card's subscription.  The
	 * bind is read back; the retry covers a lost first message (run
	 * 132112), and the other subscription stays only as a fallback when
	 * the firmware refuses this one outright. */
	bound = mhi_wds_bind_get(out, in, node, port, 0xc6, answer, answer_size);
	hakios_tl("pcie2: cellular: IMS: before the bind the modem says the client is on subscription %u (%s)\n",
	    bound, mhi_wds_subscription_name(bound));

	at = mhi_qmi_open(request, 0xc0, 0x00af);
	at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS bind subscription", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the WDS bind to subscription %u (the SIM's) says result %u, error %u\n",
	    binding, result, error);
	if (got != 0 && result == 0) {
		bind_works = 1;
		bound = binding;
	}

	said = mhi_wds_bind_get(out, in, node, port, 0xc7, answer, answer_size);
	if (said != ~0u) {
		bound = said;
	}
	hakios_tl("pcie2: cellular: IMS: after the bind to %u the modem says the client is on subscription %u (%s)\n",
	    binding, bound, mhi_wds_subscription_name(bound));

	/* The first WDS message of run 132112 was this bind and it went
	 * unanswered; a lost first message is not a refused one. */
	if (!bind_works && bound != binding) {
		at = mhi_qmi_open(request, 0xc8, 0x00af);
		at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at,
		    "IPCR WDS bind subscription (retry, the SIM's)", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the bind to %u retried says result %u, error %u\n",
		    binding, result, error);
		if (got != 0 && result == 0) {
			bind_works = 1;
			bound = binding;
		}
		said = mhi_wds_bind_get(out, in, node, port, 0xcc, answer,
		    answer_size);
		if (said != ~0u) {
			bound = said;
		}
		hakios_tl("pcie2: cellular: IMS: after the retried bind to %u the modem says the client is on subscription %u (%s)\n",
		    binding, bound, mhi_wds_subscription_name(bound));
	}

	if (!bind_works && bound != binding) {
		binding = binding == 1 ? 2 : 1;
		at = mhi_qmi_open(request, 0xce, 0x00af);
		at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at,
		    "IPCR WDS bind subscription (the primary fallback)", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the WDS bind to the other subscription %u (fallback) says result %u, error %u\n",
		    binding, result, error);
		if (got != 0 && result == 0) {
			bind_works = 1;
			bound = binding;
		}
		said = mhi_wds_bind_get(out, in, node, port, 0xd9, answer,
		    answer_size);
		if (said != ~0u) {
			bound = said;
		}
		hakios_tl("pcie2: cellular: IMS: after the fallback primary bind the modem says the client is on subscription %u (%s)\n",
		    bound, mhi_wds_subscription_name(bound));
	}

	/* What the firmware says it implements (Get Supported Messages 0x001e,
	 * output TLV 0x10: a u16 count, then the message ids).  A refused
	 * Start Network means something else when 0x0020 is not in this list. */
	at = mhi_qmi_open(request, 0xc9, 0x001e);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS supported messages", answer, answer_size);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	if (value != NULL && size >= 2) {
		unsigned count = value[0] | (unsigned)value[1] << 8;
		unsigned listed = size - 2 < count ? (unsigned)(size - 2) : count;
		unsigned have_start = 0, have_status = 0, have_list = 0;
		unsigned have_settings = 0, have_create = 0, have_default = 0;
		unsigned have_family = 0, have_attach = 0;

		for (unsigned i = 0; i < listed; i++) {
			switch (value[2 + i]) {
			case 0x20:
				have_start = 1;
				break;
			case 0x22:
				have_status = 1;
				break;
			case 0x27:
				have_create = 1;
				break;
			case 0x2a:
				have_list = 1;
				break;
			case 0x2b:
				have_settings = 1;
				break;
			case 0x49:
				have_default = 1;
				break;
			case 0x4d:
				have_family = 1;
				break;
			case 0x85:
				have_attach = 1;
				break;
			default:
				break;
			}
		}
		hakios_tl("pcie2: cellular: IMS: WDS lists %u supported messages: start %u, packet status %u, profile list %u, profile settings %u, profile create %u, default profile %u, set family %u, attach params %u\n",
		    count, have_start, have_status, have_list, have_settings,
		    have_create, have_default, have_family, have_attach);
	} else {
		hakios_tl("pcie2: cellular: IMS: WDS did not report its supported messages\n");
	}

	/* Is a call already up before one is asked for?  The packet service
	 * (0x0022, output TLV 0x01) is the only one that can say so. */
	at = mhi_qmi_open(request, 0xc1, 0x0022);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS packet service before the start", answer, answer_size);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
	if (value != NULL && size >= 1) {
		status = value[0];
	}
	hakios_tl("pcie2: cellular: IMS: before the start the packet service says %u (%s)\n",
	    status, mhi_wds_conn_name(status));

	/* The modem's own default 3GPP profile for embedded calls (0x0049,
	 * input {type 0 3GPP, family 0 embedded}, output TLV 0x01 the index):
	 * the profile its initial attach used, which wwand calls the attach
	 * profile. */
	at = mhi_qmi_open(request, 0xca, 0x0049);
	{
		uint8_t which[2] = { 0x00, 0x00 };

		at = mhi_qmi_put(request, at, 0x01, which, 2);
	}
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS default profile number", answer, answer_size);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
	if (value != NULL && size >= 1) {
		default_profile = value[0];
	}
	hakios_tl("pcie2: cellular: IMS: the modem's default 3GPP profile is %u\n",
	    default_profile);

	/* What the LTE attach was done with (0x0085: TLV 0x10 the APN, 0x11
	 * the IP type, 0x12 whether an attach ran): the network's own answer
	 * about the APN it accepted, independent of any profile list. */
	at = mhi_qmi_open(request, 0xcb, 0x0085);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS LTE attach parameters", answer, answer_size);
	{
		unsigned attached = ~0u, iptype = ~0u, alen = 0;
		const uint8_t *apn = NULL;

		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x11, &size) : NULL;
		if (value != NULL && size >= 1) {
			iptype = value[0];
		}
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x12, &size) : NULL;
		if (value != NULL && size >= 1) {
			attached = value[0];
		}
		if (got != 0) {
			apn = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
			if (apn != NULL) {
				/* A plain string, not length-prefixed:
				 * run 121242 printed "ims" as "ms". */
				alen = (unsigned)size;
			}
		}
		hakios_tl("pcie2: cellular: IMS: the LTE attach is attached %u, ip type %u, APN \"%.*s\"\n",
		    attached, iptype, (int)alen,
		    apn != NULL ? (const char *)apn : "");
	}

	/* The attach runs the initial-attach profile as stored, not the
	 * runtime list the boot seeds: run 002738 kept the list at {1} cbnet
	 * through the register -- read back 0.8 s before it -- and the
	 * attach still carried "ims".  Profile 6, the slot run 124850's
	 * Create Profile wrote APN "ims" into, is the one the attach names
	 * and the default profile points at, so its APN is repaired to the
	 * SIM's cbnet here, read back, and the attach is made to rebuild
	 * from it now (list {6}, action 0x02 detach-or-disconnect).  The
	 * reads after it say whether the network accepted the new APN. */
	{
		/* Android repairs nothing and forces no re-attach: it deletes
		 * its stale 3GPP2 profile and creates a fresh ims one, and its
		 * attach is built from the default profile at registration.
		 * Every repair and force up to run 103830 wrote SUBSCRIPTION 1
		 * state; with the client bound to 2 the same writes would now
		 * hit the SIM's store, and neither has a reference frame, so
		 * both stay off (the code stays for the run that needs it). */
		static const int repair_and_force = 0;

		if (repair_and_force) {
		unsigned attached2 = ~0u, iptype2 = ~0u;
		char apn2[24] = "";

		mhi_wds_profile_read(out, in, node, port, 0, 6,
		    "before the repair", answer, answer_size);
		if (mhi_wds_profile_apn(out, in, node, port, 6,
		    "qdp_profile_ia", "cbnet", answer, answer_size)) {
			mhi_wds_profile_read(out, in, node, port, 0, 6,
			    "after the repair", answer, answer_size);
		}
		mhi_wds_attach_force(out, in, node, port, 6, 0x02, answer,
		    answer_size);
		delay(3000000);
		if (mhi_wds_attach_params(out, in, node, port, 0x86, answer,
		    answer_size, &attached2, &iptype2, apn2, sizeof(apn2))) {
			hakios_tl("pcie2: cellular: IMS: after the forced re-attach the LTE attach is attached %u, ip type %u, APN \"%s\"\n",
			    attached2, iptype2, apn2);
		} else {
			hakios_tl("pcie2: cellular: IMS: after the forced re-attach the attach did not answer\n");
		}
		}
	}

	/* Which profiles the LTE attach runs from (Get LTE Attach PDN List
	 * 0x0094; max count 0x0092).  qcril sets this list to profile 1 --
	 * the cbnet initial-attach profile -- at every boot, and the attach
	 * then carries the APN its data calls use.  Run 154032's attach
	 * carried "ims" from default profile 6 (left there by run 124850),
	 * and every Start Network was refused by the CM's access list with
	 * verbose reason 1042.  The list is read first and set to profile 1
	 * (0x0093, action 0x01 none) only when profile 1 is not already in
	 * it: runtime state the modem re-establishes by itself.  The
	 * profile store is touched only by the repair above, and that only
	 * to restore the SIM's own APN. */
	{
		uint16_t cur[8];
		unsigned max_attach = ~0u, ncur = ~0u, named1 = 0;
		static const int set_attach_list = 0;

		at = mhi_qmi_open(request, 0xdb, 0x0092);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS max LTE attach PDN number", answer,
		    answer_size);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
		if (value != NULL && size >= 1) {
			max_attach = value[0];
		}
		at = mhi_qmi_open(request, 0xda, 0x0094);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS LTE attach PDN list", answer,
		    answer_size);
		if (got != 0) {
			value = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
			if (value != NULL) {
				ncur = mhi_wds_attach_ids(value, size, cur,
				    sizeof(cur) / sizeof(cur[0]));
			}
		}
		if (ncur == ~0u) {
			hakios_tl("pcie2: cellular: IMS: the modem did not say which profiles the LTE attach runs from (the max is %u)\n",
			    max_attach);
		} else {
			hakios_tl("pcie2: cellular: IMS: the LTE attach names %u profiles (the max is %u)\n",
			    ncur, max_attach);
			for (unsigned i = 0; i < ncur; i++) {
				hakios_tl("pcie2: cellular: IMS: attach profile %u of %u is %u%s\n",
				    i + 1, ncur, cur[i],
				    cur[i] == 1 ? " (the cbnet initial-attach profile)" : "");
				if (cur[i] == 1) {
					named1 = 1;
				}
			}
		}
		if (named1) {
			hakios_tl("pcie2: cellular: IMS: the attach already runs from profile 1; the list is left alone\n");
		} else if (!set_attach_list) {
			/* Android never sends Set LTE Attach PDN List (0x0093);
			 * its 09:44 boot capture has none, and the default
			 * profile is the lever the attach follows. */
			hakios_tl("pcie2: cellular: IMS: the attach list does not name profile 1 and is left alone (Android never sets it)\n");
		} else {
			/* The set is idempotent and Android's own boot does the
			 * same; when the read did not answer it is still sent,
			 * because a firmware may implement the set alone. */
			uint8_t list[3] = { 1, 1, 0 };	/* count 1, u16 1 */
			uint32_t action = 0x01;		/* none */
			unsigned sresult = ~0u, serror = ~0u;

			at = mhi_qmi_open(request, 0xdd, 0x0093);
			at = mhi_qmi_put(request, at, 0x01, list, sizeof(list));
			at = mhi_qmi_put(request, at, 0x10, &action,
			    sizeof(action));
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, 0x1002, node, port, request,
			    (unsigned)at, "IPCR WDS set LTE attach PDN list",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: the attach PDN list is set to profile 1: result %u, error %u%s\n",
			    result, error, got == 0 ? " (no answer)" : "");
			if (got == 0 || result != 0) {
				/* The list may only change mid-attach with the
				 * detach action; qcril sends none, but a
				 * refusal here is answered by the other form. */
				action = 0x02;	/* detach or PDN disconnect */
				at = mhi_qmi_open(request, 0xde, 0x0093);
				at = mhi_qmi_put(request, at, 0x01, list,
				    sizeof(list));
				at = mhi_qmi_put(request, at, 0x10, &action,
				    sizeof(action));
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x1002, node, port,
				    request, (unsigned)at,
				    "IPCR WDS set LTE attach PDN list (detach)",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &sresult, &serror);
				hakios_tl("pcie2: cellular: IMS: the attach PDN list set with the detach action says result %u, error %u%s\n",
				    sresult, serror,
				    got == 0 ? " (no answer)" : "");
			}
		}
	}

	/* The PS detach/attach rebuild block is gone: runs 175546, 203116 and
	 * 023826 all sent it in the libqmi-era form (NAS Attach Detach 0x0023,
	 * TLV 0x10, one-byte action 2 then 1) and the modem answered result 0
	 * and then NO_EFFECT 26 -- it never acted on it.  Android's own frames
	 * (qmi5 line 1112, qmi3 lines 151/289) carry the action in TLV 0x16 as
	 * a 4-byte value; that form is sent between the two fresh clients
	 * below, so the second client dials in the reference's own state. */

	/* The data port is opened and framed before the session is asked
	 * for: the reference phone's session answered InvalidOperation (70)
	 * until DPM had opened the port and WDA had set its format. */
	if (mhi_wds_data_port(out, in, answer, answer_size, &data_type,
	    &data_iface)) {
		mhi_wds_data_path(out, in, answer, answer_size,
		    data_type, data_iface);
	}

	/* qcril's own ims profile, reproduced exactly.  Both captured boots
	 * (qmi5: the 02:18 airplane-mode rebuild, qmi3: the 01:16 boot) have
	 * qcril's WDS client read the profile store's lists and then send
	 * Create Profile 0x0027 with seven TLVs -- profile type 1 (the 3GPP2
	 * store), 0x8f = 0, 0x9a = 0, extended APN "ims" (0xa1), extended IP
	 * type IPv6 (0xa2), 0xad = 0 and the 8-byte APN type mask 2 (0xdd) --
	 * and the modem answered {type 1, index 104}.  Its accepted IMS Start
	 * Network names no profile and no APN: the modem resolves the dial by
	 * that mask, so the profile carrying it is part of host parity.  The
	 * store persists in NV, so the reads first show whether an earlier
	 * Android boot left its profile behind, and the read-back after the
	 * create shows where this one landed. */
	{
		int created;

		mhi_wds_profile_read(out, in, node, port, 0, 1,
		    "the cbnet initial-attach profile", answer, answer_size);
		mhi_wds_profile_read(out, in, node, port, 0, 6,
		    "the default LTE attach profile", answer, answer_size);
		mhi_wds_profile_read(out, in, node, port, 1, 0,
		    "the 3GPP2 store's first slot", answer, answer_size);
		mhi_wds_profile_read(out, in, node, port, 1, 104,
		    "qcril's own ims slot", answer, answer_size);
		created = mhi_wds_profile_create_ims(out, in, node, port,
		    answer, answer_size);
		if (created >= 0) {
			mhi_wds_profile_read(out, in, node, port, 1,
			    (unsigned)created, "after qcril's create", answer,
			    answer_size);
		}
	}

	/* Which RATs the host has asked NAS for.  The same phone under
	 * Android sits on NR SA (run 132112's control), while every HakiOS
	 * boot has camped on LTE (radio 8), and the firmware's ACL deny
	 * reasons include ACL_DENY_REASON_UNPREFERRED_RAT -- if the access
	 * policy wants NR for data, a host that never asked NAS for NR would
	 * see every data call denied on LTE.  Read the mode preference, then
	 * ask for GSM, UMTS, LTE and NR5G (QmiNasRatModePreference bits 2, 3,
	 * 4, 6; LTE and UMTS alone would strand the search if NR is out of
	 * reach).  The preference is volatile modem state and the Android
	 * boot after this run sets its own; the profile survey below overlaps
	 * the reselection. */
	{
		uint8_t nas_request[16];
		unsigned nas_node = 0, nas_port = 0;
		unsigned pref = ~0u;
		int nas_found = mhi_qrtr_service(0x03, &nas_node, &nas_port);

		/* Run 085436: the first cellular cycle reached this stage before
		 * the modem had announced NAS -- WMS and WDS were in the table,
		 * NAS was not -- so everything below was skipped and that boot
		 * never asked the modem for NR at all.  Ask the name service for
		 * its table again and give the announcement a few seconds
		 * before giving up. */
		if (!nas_found) {
			(void)mhi_send(out, mhi_qrtr_hello, sizeof(mhi_qrtr_hello));
			for (unsigned i = 0; i < 6 && !nas_found; i++) {
				(void)mhi_qrtr_pump(out, in, 800,
				    "waiting for the NAS announcement");
				nas_found = mhi_qrtr_service(0x03, &nas_node,
				    &nas_port);
			}
			hakios_tl("pcie2: cellular: IMS: NAS %s after the wait\n",
			    nas_found ? "arrived" : "never arrived");
		}
		if (nas_found) {
			/* Android's boot takes NR SA out of deprioritization
			 * within its first second (boot capture 09:44:01.687:
			 * NAS 0x0127 with TLV 0x01 = 0, logged by qcril as
			 * "deprioritize_nr5g is 0").  A host that never sends
			 * it leaves the modem's own default in force, and that
			 * default is what keeps SA out of reselection while
			 * every start is denied on LTE. */
			uint8_t off = 0;

			at = mhi_qmi_open(nas_request, 0xf9, 0x0127);
			at = mhi_qmi_put(nas_request, at, 0x01, &off, 1);
			mhi_qmi_seal(nas_request, at);
			got = mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
			    nas_request, (unsigned)at,
			    "IPCR NAS deprioritize NR5G (allow SA)", answer,
			    answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: the ask to stop deprioritizing NR SA says result %u, error %u%s\n",
			    result, error, got == 0 ? " (no answer)" : "");
			if (got != 0) {
				mhi_qmi_tlvs(answer, got,
				    "IPCR NAS deprioritize NR5G");
			}

			at = mhi_qmi_open(nas_request, 0xf7, 0x0034);
			mhi_qmi_seal(nas_request, at);
			got = mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
			    nas_request, (unsigned)at,
			    "IPCR NAS system selection preference",
			    answer, answer_size);
			if (got != 0) {
				pref = mhi_qmi_u16(answer, got, 0x11);
				mhi_qmi_tlvs(answer, got,
				    "IPCR NAS system selection preference");
			}
			hakios_tl("pcie2: cellular: IMS: the modem's RAT mode preference is %#x (gsm %u, umts %u, lte %u, nr %u)%s\n",
			    pref, pref != ~0u && (pref & 0x04) != 0,
			    pref != ~0u && (pref & 0x08) != 0,
			    pref != ~0u && (pref & 0x10) != 0,
			    pref != ~0u && (pref & 0x40) != 0,
			    pref == ~0u ? " (not read)" : "");
			{
				uint16_t want = 0x5c;	/* GSM, UMTS, LTE, NR5G */

				at = mhi_qmi_open(nas_request, 0xf8, 0x0033);
				at = mhi_qmi_put(nas_request, at, 0x11, &want,
				    sizeof(want));
				mhi_qmi_seal(nas_request, at);
				got = mhi_qmi_call(out, in, 0x1001, nas_node,
				    nas_port, nas_request, (unsigned)at,
				    "IPCR NAS set system selection preference",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &result, &error);
				hakios_tl("pcie2: cellular: IMS: the ask for GSM, UMTS, LTE and NR5G says result %u, error %u%s\n",
				    result, error,
				    got == 0 ? " (no answer)" : "");
			}

			/* The vendor questions Android's dispatcher asks in the
			 * same second as the deprioritize (boot capture
			 * 09:44:01.7-02.9: NAS 0x005c, 0x0063, 0x007c, 0x007e,
			 * 0x00cc, 0x010c, each with no body).  Asked once the
			 * same way so the answers name them on the record;
			 * read-only, nothing is set. */
			{
				static const unsigned ids[] = { 0x005c, 0x0063,
				    0x007c, 0x007e, 0x00cc, 0x010c };
				static int probed;

				if (!probed) {
					probed = 1;
					for (unsigned i = 0;
					    i < sizeof(ids) / sizeof(ids[0]); i++) {
						char label[64];
						unsigned id = ids[i];

						snprintf(label, sizeof(label),
						    "IPCR NAS vendor probe 0x%04x", id);
						at = mhi_qmi_open(nas_request,
						    0xf0, id);
						mhi_qmi_seal(nas_request, at);
						got = mhi_qmi_call(out, in, 0x1001,
						    nas_node, nas_port, nas_request,
						    (unsigned)at, label, answer,
						    answer_size);
						if (got != 0) {
							mhi_qmi_tlvs(answer, got,
							    label);
						}
					}
				}
			}
		}
	}

	/* The NR-first experiment: the mode preference already names NR5G,
	 * yet every HakiOS boot has camped LTE while Android on this same
	 * phone sits on NR SA (telephony.registry: NR_SA, 46015, voice and
	 * SMS in service).  The modem's own acquisition order is the host
	 * input still unsent: Set System Selection Preference TLV 0x1e is a
	 * QmiNasRadioInterface array in preference order.  pcie2_nr=1 sends
	 * NR5G first (then LTE, UMTS, GSM) and reads back what NAS holds and
	 * what it sees of NR (Get System Info, TLVs 0x4a/0x4b/0x4e).  State
	 * mutated is volatile NAS state; the Android boot after the run sets
	 * its own. */
	{
		uint32_t nr_on = 0;
		uint8_t nr_request[32];
		unsigned nr_node = 0, nr_port = 0;
		size_t nat;

		PE_parse_boot_argn("pcie2_nr", &nr_on, sizeof(nr_on));
		hakios_tl("pcie2: cellular: IMS: the NR-first experiment is %s\n",
		    nr_on ? "armed (pcie2_nr=1)" : "off");
		if (nr_on && mhi_qrtr_service(0x03, &nr_node, &nr_port)) {
			/* A u8 count, then one QmiNasRadioInterface byte per
			 * entry: NR5G 0x40, LTE 0x10, UMTS 0x08, GSM 0x04. */
			static const uint8_t order[] = { 4, 0x40, 0x10, 0x08, 0x04 };
			uint16_t nr_want = 0x5c;

			nat = mhi_qmi_open(nr_request, 0x90, 0x0033);
			nat = mhi_qmi_put(nr_request, nat, 0x11, &nr_want,
			    sizeof(nr_want));
			nat = mhi_qmi_put(nr_request, nat, 0x1e, order,
			    sizeof(order));
			mhi_qmi_seal(nr_request, nat);
			got = mhi_qmi_call(out, in, 0x1001, nr_node, nr_port,
			    nr_request, (unsigned)nat,
			    "IPCR NAS set system selection preference (NR first)",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: THE NR-FIRST ASK (NR5G, LTE, UMTS, GSM) says result %u, error %u%s\n",
			    result, error, got == 0 ? " (no answer)" : "");
			nat = mhi_qmi_open(nr_request, 0x91, 0x0034);
			mhi_qmi_seal(nr_request, nat);
			got = mhi_qmi_call(out, in, 0x1001, nr_node, nr_port,
			    nr_request, (unsigned)nat,
			    "IPCR NAS preference after the NR-first ask",
			    answer, answer_size);
			if (got != 0) {
				mhi_qmi_tlvs(answer, got,
				    "IPCR NAS preference after the NR-first ask");
			}
			nat = mhi_qmi_open(nr_request, 0x92, 0x004d);
			mhi_qmi_seal(nr_request, nat);
			got = mhi_qmi_call(out, in, 0x1001, nr_node, nr_port,
			    nr_request, (unsigned)nat,
			    "IPCR NAS system info (NR availability)", answer,
			    answer_size);
			if (got != 0) {
				mhi_qmi_tlvs(answer, got,
				    "IPCR NAS system info (NR availability)");
			}
		}
	}

	/* What the modem's per-boot configuration says.  Xiaomi's qcril
	 * drives PDC at every boot and the CM access list that denies every
	 * start is part of the configuration PDC selects; nothing had ever
	 * asked this modem.  Run 191152: the software pool holds 25 configs
	 * and the selected one is id 0.  Read-only: Get Selected Config for
	 * SOFTWARE and PLATFORM, the two lists, and Get Config Info to name
	 * every software id.  The data arrives in indications (see
	 * mhi_pdc_exchange). */
	{
		/* The read probe runs once per boot.  Get Config Info (0x28)
		 * names every software config, and repeating the whole read at
		 * every call of this function would only burn window time. */
		static int pdc_read;
		unsigned pdc_node = 0, pdc_port = 0;

		if (!pdc_read && mhi_qrtr_service(0x24, &pdc_node, &pdc_port)) {
			uint8_t pdc_request[32];
			size_t pat;
			uint32_t token, type;
			unsigned i;

			pdc_read = 1;

			type = 1;	/* SOFTWARE */
			token = 0x5a5a0001;
			pat = mhi_qmi_open(pdc_request, 0xfc, 0x0022);
			pat = mhi_qmi_put(pdc_request, pat, 0x01, &type,
			    sizeof(type));
			pat = mhi_qmi_put(pdc_request, pat, 0x10, &token,
			    sizeof(token));
			mhi_qmi_seal(pdc_request, pat);
			mhi_pdc_exchange(out, in, pdc_node, pdc_port,
			    pdc_request, (unsigned)pat,
			    "IPCR PDC get selected config (software)", 0x0022,
			    2500);

			type = 0;	/* PLATFORM */
			token = 0x5a5a0002;
			pat = mhi_qmi_open(pdc_request, 0xfd, 0x0022);
			pat = mhi_qmi_put(pdc_request, pat, 0x01, &type,
			    sizeof(type));
			pat = mhi_qmi_put(pdc_request, pat, 0x10, &token,
			    sizeof(token));
			mhi_qmi_seal(pdc_request, pat);
			mhi_pdc_exchange(out, in, pdc_node, pdc_port,
			    pdc_request, (unsigned)pat,
			    "IPCR PDC get selected config (platform)", 0x0022,
			    2500);

			type = 1;	/* SOFTWARE */
			token = 0x5a5a0003;
			pat = mhi_qmi_open(pdc_request, 0xfe, 0x0024);
			pat = mhi_qmi_put(pdc_request, pat, 0x10, &token,
			    sizeof(token));
			pat = mhi_qmi_put(pdc_request, pat, 0x11, &type,
			    sizeof(type));
			mhi_qmi_seal(pdc_request, pat);
			mhi_pdc_exchange(out, in, pdc_node, pdc_port,
			    pdc_request, (unsigned)pat,
			    "IPCR PDC list software configs", 0x0024, 4000);

			/* The platform ids are paths under /data/vendor/...: the
			 * list names every tree the modem holds, so the selected
			 * path can be compared with the carrier's. */
			type = 0;	/* PLATFORM */
			token = 0x5a5a0004;
			pat = mhi_qmi_open(pdc_request, 0xf9, 0x0024);
			pat = mhi_qmi_put(pdc_request, pat, 0x10, &token,
			    sizeof(token));
			pat = mhi_qmi_put(pdc_request, pat, 0x11, &type,
			    sizeof(type));
			mhi_qmi_seal(pdc_request, pat);
			mhi_pdc_exchange(out, in, pdc_node, pdc_port,
			    pdc_request, (unsigned)pat,
			    "IPCR PDC list platform configs", 0x0024, 4000);

			/* Get Config Info names each software config: its
			 * Description TLV is the MBN identity behind the
			 * opaque 20-byte id, and it is what tells whether a
			 * China Broadnet config exists to select.  The input
			 * is Config Type And Id (TLV 0x01): guint32 type,
			 * then the u8-length-prefixed id. */
			for (i = 0; i < mhi_pdc_sw_count; i++) {
				uint8_t info[32];
				uint8_t name_request[64];
				char name_what[48];
				size_t nat;
				uint8_t id_len = mhi_pdc_sw_lengths[i];
				uint32_t itype = 1, itoken;

				memcpy(info, &itype, sizeof(itype));
				info[4] = id_len;
				memcpy(info + 5, mhi_pdc_sw_ids[i], id_len);
				itoken = 0x5b5b0000 + i;
				nat = mhi_qmi_open(name_request,
				    (uint8_t)(0xe0 + i), 0x0028);
				nat = mhi_qmi_put(name_request, nat, 0x01,
				    info, 5 + id_len);
				nat = mhi_qmi_put(name_request, nat, 0x10,
				    &itoken, sizeof(itoken));
				mhi_qmi_seal(name_request, nat);
				snprintf(name_what, sizeof(name_what),
				    "IPCR PDC get config info %u", i);
				mhi_pdc_exchange(out, in, pdc_node,
				    pdc_port, name_request, (unsigned)nat,
				    name_what, 0x0028, 2500);
			}

			/* One causal experiment: run 194256 named the pool and the
			 * active config 0 is "Volte_OpenMkt-Commercial-CBN" --
			 * China Broadnet's own VoLTE MBN -- with the CMCC one at
			 * index 1, so the CM ACL denial is not a wrong-MBN
			 * problem.  Selecting CMCC still tells whether the
			 * selected MBN gates the access list at all.  The modem's
			 * own config is copied out first; the restore block after
			 * this one puts it back on the next call of this
			 * function, after one full matrix ran under CMCC. */
			{
				int active_index = -1, cmcc_index = -1;
				uint32_t cmcc_on = 0;
				uint32_t keep_on = 0;
				unsigned j;

				/* One variable per boot: the boot-arg keeps the
				 * DPM-parity boot free of the CMCC mutation. */
				PE_parse_boot_argn("pcie2_pdc_cmcc", &cmcc_on,
				    sizeof(cmcc_on));
				PE_parse_boot_argn("pcie2_pdc_keep", &keep_on,
				    sizeof(keep_on));

				for (j = 0; j < mhi_pdc_sw_count; j++) {
					if (mhi_pdc_sw_lengths[j] ==
					    mhi_pdc_sw_active_len &&
					    memcmp(mhi_pdc_sw_ids[j],
					    mhi_pdc_sw_active,
					    mhi_pdc_sw_active_len) == 0) {
						active_index = (int)j;
					}
					if (mhi_pdc_name_has(
					    mhi_pdc_sw_names[j], "cmcc")) {
						cmcc_index = (int)j;
					}
				}
				hakios_tl("pcie2: cellular: PDC: the active software config is index %d%s; the CMCC config is index %d%s; the CMCC experiment is %s\n",
				    active_index,
				    active_index < 0 ? " (not in the list)" : "",
				    cmcc_index,
				    cmcc_index < 0 ? " (not in the list)" : "",
				    cmcc_on ? "armed" : "off (pcie2_pdc_cmcc=0)");
				if (cmcc_on && active_index >= 0 && cmcc_index >= 0 &&
				    mhi_pdc_name_has(mhi_pdc_sw_names[active_index],
				    "cbn")) {
					uint8_t write_request[64];
					uint8_t with_id[32];
					uint32_t wtype = 1, wtoken;
					size_t wid;
					uint8_t id_len =
					    mhi_pdc_sw_lengths[cmcc_index];

					mhi_pdc_cbn_len = mhi_pdc_sw_active_len;
					memcpy(mhi_pdc_cbn_id, mhi_pdc_sw_active,
					    mhi_pdc_cbn_len);

					memcpy(with_id, &wtype, sizeof(wtype));
					with_id[4] = id_len;
					memcpy(with_id + 5,
					    mhi_pdc_sw_ids[cmcc_index], id_len);
					wtoken = 0x5c5c0001;
					wid = mhi_qmi_open(write_request, 0xf6,
					    0x0023);
					wid = mhi_qmi_put(write_request, wid, 0x01,
					    with_id, 5 + id_len);
					wid = mhi_qmi_put(write_request, wid, 0x10,
					    &wtoken, sizeof(wtoken));
					mhi_qmi_seal(write_request, wid);
					mhi_pdc_exchange(out, in, pdc_node,
					    pdc_port, write_request, (unsigned)wid,
					    "IPCR PDC set selected config (CMCC)",
					    0x0023, 2500);

					wtoken = 0x5c5c0002;
					wid = mhi_qmi_open(write_request, 0xf7,
					    0x0027);
					wid = mhi_qmi_put(write_request, wid, 0x01,
					    &wtype, sizeof(wtype));
					wid = mhi_qmi_put(write_request, wid, 0x10,
					    &wtoken, sizeof(wtoken));
					mhi_qmi_seal(write_request, wid);
					mhi_pdc_exchange(out, in, pdc_node,
					    pdc_port, write_request, (unsigned)wid,
					    "IPCR PDC activate config (CMCC)",
					    0x0027, 3000);
					delay(2000000);

					wtoken = 0x5c5c0003;
					wid = mhi_qmi_open(write_request, 0xf5,
					    0x0022);
					wid = mhi_qmi_put(write_request, wid, 0x01,
					    &wtype, sizeof(wtype));
					wid = mhi_qmi_put(write_request, wid, 0x10,
					    &wtoken, sizeof(wtoken));
					mhi_qmi_seal(write_request, wid);
					mhi_pdc_exchange(out, in, pdc_node,
					    pdc_port, write_request, (unsigned)wid,
					    "IPCR PDC get selected config (after the CMCC selection)",
					    0x0022, 2500);
					mhi_pdc_experiment_selected = 1;
					if (keep_on != 0) {
						mhi_pdc_experiment_kept = 1;
					}
					hakios_tl("pcie2: cellular: PDC: the CMCC config is selected and activated; %s\n",
					    keep_on != 0 ? "pcie2_pdc_keep=1 keeps it for the whole boot, so the IMS stack's bind, ready, ask and registration run under it" : "the start matrix below runs under it and the modem's own config is selected again before the next cycle");
				}
			}
		} else if (!pdc_read) {
			hakios_tl("pcie2: cellular: PDC: the modem has not announced the PDC service; its configuration is not read\n");
		}
	}

	/* Android's default profile is 7 at its dial.  It was set before the
	 * register (mhi_wds_early, on subscription 2), and Android never
	 * re-sends it at the dial -- the read below says whether this
	 * firmware kept it.  When it drifted, qcril's own two sets (1 then
	 * 7) are replayed so the dial runs in the reference's state. */
	{
		uint8_t whichd[3] = { 0x00, 0x00, 0x00 };
		uint8_t readd[2] = { 0x00, 0x00 };
		unsigned dprof = ~0u;

		at = mhi_qmi_open(request, 0xd0, 0x0049);
		at = mhi_qmi_put(request, at, 0x01, readd, sizeof(readd));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at,
		    "IPCR WDS default profile number (before the dial)", answer,
		    answer_size);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
		if (value != NULL && size >= 1) {
			dprof = value[0];
		}
		hakios_tl("pcie2: cellular: IMS: before the dial the default 3GPP profile reads %u%s\n",
		    dprof, dprof == 7 ? " (the reference's)" : "");
		if (dprof != 7) {
			static const uint8_t idxs[2] = { 1, 7 };

			for (unsigned di = 0; di < 2; di++) {
				unsigned dresult = ~0u, derror = ~0u;

				whichd[2] = idxs[di];
				at = mhi_qmi_open(request,
				    (uint8_t)(0xd1 + di), 0x004a);
				at = mhi_qmi_put(request, at, 0x01, whichd,
				    sizeof(whichd));
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x1002, node, port,
				    request, (unsigned)at,
				    "IPCR WDS set default profile number (before the dial)",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &dresult, &derror);
				hakios_tl("pcie2: cellular: IMS: before the dial the default 3GPP profile is set to %u: result %u, error %u%s\n",
				    idxs[di], dresult, derror,
				    got == 0 ? " (no answer)" : "");
			}
		}
	}

	/* The DSD replay goes here, before the first Start: the capture has the
	 * conversation as the last host action before the accepted dial, and
	 * run 083846 proved the old site after the fresh clients was dead code
	 * when their starts were refused 1042 (the function returns there). */
	mhi_dsd_arm(out, in, answer, answer_size);

	/* The reference's last host words before its accepted dial, mirrored
	 * in its own order and spacing (the 09:44 boot capture): netmgr's DSD
	 * pair at 22.765/.788 (System Status Change 0x0025 with TLV 0x10 = 1,
	 * then vendor 0x0069 with TLV 0x10 = 0), a second's pause, then
	 * qcrild's WDS profile survey -- Get Profile Settings 0x002b for 1, 2,
	 * 3, 4, 5 and 7 and the 3GPP2 store's first slot, the profiles whose
	 * APNs a mask-resolved dial can land on -- then its probe client and,
	 * 70 ms later, its two dial clients.  This driver replayed the DSD
	 * conversation once, seconds earlier, and started at once; run
	 * 100618's fresh clients bound and started with nothing of this in
	 * between and were refused CM 1042, so the pair, the pause and the
	 * survey are replayed here the way the reference sends them. */
	{
		unsigned dnode = 0, dport = 0;

		if (mhi_qrtr_service(0x2a, &dnode, &dport)) {
			static const uint8_t on = 1, off = 0;

			at = mhi_qmi_open(request, 0x1e6, 0x0025);
			at = mhi_qmi_put(request, at, 0x10, &on, 1);
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, 0x1007, dnode, dport,
			    request, (unsigned)at,
			    "IPCR DSD the dial's status change (1), netmgr's own",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: DSD: the dial's first status change (1) says result %u, error %u\n",
			    result, error);
			at = mhi_qmi_open(request, 0x1e7, 0x0069);
			at = mhi_qmi_put(request, at, 0x10, &off, 1);
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, 0x1007, dnode, dport,
			    request, (unsigned)at,
			    "IPCR DSD the dial's set (0x0069, netmgr's zero)",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: DSD: the dial's set (0x0069, zero) says result %u, error %u\n",
			    result, error);
			delay(1200000);
		} else {
			hakios_tl("pcie2: cellular: DSD: the service is not announced; the dial's pair is skipped\n");
		}
	}
	/* Android reads profiles from a client bound to the card's
	 * subscription; run 033649's reads all came back unanswered from
	 * the shared client, which had never bound one.  Bind first and log
	 * the outcome, then survey. */
	{
		uint32_t ssub = mhi_wds_card_sub;
		unsigned sres = ~0u, serr = ~0u;

		at = mhi_qmi_open(request, 0xe5, 0x00af);
		at = mhi_qmi_put(request, at, 0x01, &ssub, sizeof(ssub));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at,
		    "IPCR WDS profile-survey client bind subscription (the SIM's)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &sres, &serr);
		hakios_tl("pcie2: cellular: IMS: the profile-survey client binds subscription %u (the SIM's): result %u, error %u%s\n",
		    ssub, sres, serr, got == 0 ? " (no answer)" : "");
	}
	mhi_wds_profile_read(out, in, node, port, 0, 1,
	    "the dial's survey of profile 1", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 0, 2,
	    "the dial's survey of profile 2", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 0, 3,
	    "the dial's survey of profile 3", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 0, 4,
	    "the dial's survey of profile 4", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 0, 5,
	    "the dial's survey of profile 5", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 0, 7,
	    "the dial's survey of profile 7", answer, answer_size);
	mhi_wds_profile_read(out, in, node, port, 1, 0,
	    "the dial's survey of the 3GPP2 store's first slot", answer,
	    answer_size);

	/* The reference's WDS client, reproduced exactly.  The two-way capture
	 * of Android's own airplane-mode data rebuild (02:18, docs/cellular.md)
	 * shows the accepted IMS dial came from a fresh qrtr socket whose only
	 * WDS traffic ever was: Bind Mux Data Port (0x00a2, port {3, 4}, mux
	 * 2) -> Bind Subscription (0x00af = 2) -> Set IP Family (0x004d = 6)
	 * -> Start Network (0x0020, the 31-byte body) -> Get Current Settings,
	 * which returned an ipv6 address and both ipv6 DNS servers.  The same
	 * 31-byte body from this driver's long-lived 0x1002 client -- which has
	 * carried every profile write, default-profile set, attach-list set,
	 * bind and refused start of the boot -- is refused result 1, error 14,
	 * verbose CM reason 1042 (run 015549).  If the ACL refusal is the
	 * shared client's accumulated state, this client's start is accepted;
	 * if it is refused the same way, the ACL is not per client.  Both
	 * dial clients of the 09:44 capture bind mux 2 on {3, 4}; they carry
	 * subscription 0 (the IPv4 leg) and 2 (the IPv6 leg).  A refused
	 * start here means the shared
	 * client's matrix (fully run in 015549) adds nothing and is skipped. */
	if (data_type != ~0u) {
		static const unsigned fresh_count = 4;
		static const unsigned fresh_muxes[4] = { 4, 4, 4, 2 };
		static const unsigned fresh_ports[4] =
		    { 0x1005, 0x1006, 0x1008, 0x1009 };
		/* The live Android reference (2026-11-25, the phone's own
		 * IMS registration in /tmp/cap.trace): its IMS data call
		 * binds mux 4 on {type 3, interface 4}, binds subscription
		 * 2, sets family 6, and Starts APN "ims", family 6,
		 * profile 2, 0x32 = 0xff and call type 1 -- accepted,
		 * result 0.  The same TLVs from run 033649's mux 2 were
		 * refused error 70 (INVALID_OPERATION); the only leg that
		 * boot accepted was the no-APN reference Start, and the
		 * default-profile (cbnet) address it returned is what the
		 * modem's IMS stack was handed -- after which it never
		 * left state 0.  The ladder below therefore runs, in
		 * order: f0, the data service's own body on Android's live
		 * mux (4) and profile (2); f1, qcril's accepted data body
		 * (auth none, APN inline, 0x39, 0x3c) with the ims APN
		 * and profile 2; f2, the data service's body with no
		 * profile index, so the modem resolves the APN itself
		 * (pcie2_ims_v4 still makes this leg try family 4 first);
		 * f3, the no-APN reference leg on mux 2, the fallback PDN
		 * every earlier boot got.  The first accepted leg ends the
		 * ladder and owns the address the stack is handed; every
		 * leg binds subscription 2, the card's. */
		static unsigned fresh_families[4] =
		    { 0x06, 0x06, 0x06, 0x06 };
		/* All four bind mhi_wds_card_sub, the card's subscription:
		 * on Android the card sits in its tray slot and every
		 * qcrild client is on 2, but HakiOS's UIM block moves the
		 * card to logical slot 1 and binds the USIM to subscription
		 * 1, and run 021620 showed the CM ACL follows the card --
		 * the sub-1 reference Start passed the ACL. */

		static const int fresh_profiles[4] = { -1, -1, -1, -1 };
		/* Off: the accepted dial's window (09:44:22.4-23.95) carries
		 * no NAS message at all; this attach-detach form came from the
		 * airplane-mode rebuild and run 100618 sent it between the
		 * clients with no effect. */
		static const int send_attach_detach = 0;
		unsigned fresh_refused = 0;
		unsigned fresh_accepted = 0;
		int fresh_apn_up[4] = { 0, 0, 0, 0 };

		/* The SIM's default (initial-attach) PDN, when the modem has
		 * not brought one up by itself.  Android's dial runs with the
		 * family-6 packet service CONNECTED (11:06:25.370/.376: set
		 * family 6, 0x0022 status 2), and its attach carries cbnet.
		 * The Wi-Fi-off capture (12:30:36, qmiwifi.trace) shows the
		 * exact Start telephony sends for it: the APN "cbnet", auth
		 * none, the family, the embedded call type, 0x39 = 1 and
		 * TLV 0x3c = 1 -- the data shape, no 0x3d and no 0x3e mask --
		 * from clients bound to mux 4, subscription 2, one second
		 * after the default profile was set to 1.  The attempt below
		 * runs from the earlier run's client (0x100b) and family 6
		 * but binds mux 3, the 14:02 capture's data mux, because run
		 * 024409 proved that with this PDN up on mux 2 the ims dial
		 * below can never start there (every leg refused); a refusal
		 * retries the same body on mux 5 (mux 4 is the live phone's
		 * IMS mux, kept free), and an accepted call stays up.
		 * Mux 2 is never bound by data here -- it is the ims dial's
		 * own mux on the reference phone. */
		{
			unsigned cnode = 0, cport = 0;
			uint32_t cdesc[2], csub = mhi_wds_card_sub;
			uint8_t cmux = 3;
			unsigned cres = ~0u, cerr = ~0u, csay = ~0u;
			uint8_t cwhich[3] = { 0x00, 0x00, 0x01 };
			unsigned cpres = ~0u, cperr = ~0u;
			int cup = 0;

			if (mhi_qrtr_service(0x01, &cnode, &cport)) {
				/* Android's working data setup set the
				 * default 3GPP profile back to 1 a second
				 * before its accepted starts (12:30:35.883),
				 * so the attempt below runs with the same
				 * default; it is set back to 7 afterwards
				 * (the boot capture's state at the ims
				 * dial) unless the call was accepted. */
				at = mhi_qmi_open(request, 0x3f, 0x004a);
				at = mhi_qmi_put(request, at, 0x01, cwhich,
				    sizeof(cwhich));
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x1002, cnode, cport,
				    request, (unsigned)at,
				    "IPCR WDS set default profile number (before the default-PDN start)",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &cpres, &cperr);
				hakios_tl("pcie2: cellular: IMS: the default 3GPP profile is set to 1 before the default-PDN start: result %u, error %u%s\n",
				    cpres, cperr, got == 0 ? " (no answer)" : "");
				/* netmgrd's prologue before Android's data
				 * dial: a WDS client bound to subscription 2
				 * asks the modem to look up its route for
				 * cbnet (0xfff2, families 4 and 6) before
				 * either dial client's Start.  The firmware
				 * handler is a policy lookup -- the machine
				 * the CM ACL runs -- so this is the one
				 * Android action our dial never replayed.
				 * A fresh client (0x100d) keeps the shared
				 * control client's state out of it. */
				{
					unsigned lnode = 0, lport = 0;
					uint32_t lsub = mhi_wds_card_sub;
					unsigned lres = ~0u, lerr = ~0u;
					size_t lgot = 0;

					if (mhi_qrtr_service(0x01, &lnode,
					    &lport)) {
						at = mhi_qmi_open(request,
						    0x38, 0x00af);
						at = mhi_qmi_put(request, at,
						    0x01, &lsub,
						    sizeof(lsub));
						mhi_qmi_seal(request, at);
						got = mhi_qmi_call(out, in,
						    0x100d, lnode, lport,
						    request, (unsigned)at,
						    "IPCR WDS route-look-up client bind subscription (the SIM's)",
						    answer, answer_size);
						mhi_qmi_outcome(answer, got,
						    &lres, &lerr);
						hakios_tl("pcie2: cellular: IMS: the route-look-up client binds subscription %u (the SIM's): result %u, error %u%s\n",
						    lsub, lres, lerr, got == 0 ?
						    " (no answer)" : "");
						mhi_wds_route_look_up(out, in,
						    lnode, lport, 0x100d,
						    0x04, "cbnet", 0x01,
						    "IPCR WDS route look up (cbnet, family 4)",
						    answer, answer_size,
						    &lgot);
						mhi_wds_route_look_up(out, in,
						    lnode, lport, 0x100d,
						    0x06, "cbnet", 0x02,
						    "IPCR WDS route look up (cbnet, family 6)",
						    answer, answer_size,
						    &lgot);
					}
				}
				/* telephony's own create, 0.5 s before its
				 * accepted starts: Create Profile 0x0027 in
				 * the 3GPP2 store ({profile type 1, index
				 * 0}) carrying the APN cbnet and APN type
				 * mask 0x3104, Android's exact TLVs from
				 * 12:30:35.870 and 14:02:22.826.  The dial's
				 * Start names the APN inline, so no profile
				 * index is dialed -- but a CM that still
				 * resolves the APN against the store would
				 * find this entry on Android and not here.
				 * The response's profile index is logged. */
				{
					static const uint8_t cmask[8] =
					    { 0x31, 0x04, 0x00, 0x00, 0x00,
					      0x00, 0x00, 0x00 };
					static const uint8_t czero = 0x00;
					static const uint8_t ctwo = 0x02;
					static const uint8_t ctype = 0x01;
					unsigned crres = ~0u, crerr = ~0u;
					unsigned cindex = ~0u;
					const uint8_t *cv;

					at = mhi_qmi_open(request, 0x39,
					    0x0027);
					at = mhi_qmi_put(request, at, 0x01,
					    &ctype, 1);
					at = mhi_qmi_put(request, at, 0x8f,
					    &czero, 1);
					at = mhi_qmi_put(request, at, 0x9a,
					    &czero, 1);
					at = mhi_qmi_put(request, at, 0xa1,
					    "cbnet", 5);
					at = mhi_qmi_put(request, at, 0xa2,
					    &ctwo, 1);
					at = mhi_qmi_put(request, at, 0xad,
					    &czero, 1);
					at = mhi_qmi_put(request, at, 0xdd,
					    cmask, sizeof(cmask));
					mhi_qmi_seal(request, at);
					got = mhi_qmi_call(out, in, 0x1002,
					    cnode, cport, request,
					    (unsigned)at,
					    "IPCR WDS the dial's create profile (3GPP2, cbnet)",
					    answer, answer_size);
					mhi_qmi_outcome(answer, got, &crres,
					    &crerr);
					cv = got != 0 ?
					    mhi_qmi_answer_tlv(answer, got,
					    0x01, &size) : NULL;
					if (cv != NULL && size >= 1) {
						cindex = size >= 2 ?
						    (unsigned)cv[0] |
						    (unsigned)cv[1] << 8 :
						    cv[0];
					}
					hakios_tl("pcie2: cellular: IMS: the dial's create (3GPP2 store, cbnet) says result %u, error %u, index %u\n",
					    crres, crerr, cindex);
				}
				cdesc[0] = data_type;
				cdesc[1] = data_iface;
				at = mhi_qmi_open(request, 0x40, 0x00a2);
				at = mhi_qmi_put(request, at, 0x10, cdesc,
				    sizeof(cdesc));
				at = mhi_qmi_put(request, at, 0x11, &cmux, 1);
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x100b, cnode, cport,
				    request, (unsigned)at,
				    "IPCR WDS default-PDN client bind mux", answer,
				    answer_size);
				mhi_qmi_outcome(answer, got, &cres, &cerr);
				hakios_tl("pcie2: cellular: IMS: the default-PDN client binds mux %u on the {type %u, interface %u} port: result %u, error %u\n",
				    cmux, data_type, data_iface, cres, cerr);
				at = mhi_qmi_open(request, 0x41, 0x00af);
				at = mhi_qmi_put(request, at, 0x01, &csub,
				    sizeof(csub));
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x100b, cnode, cport,
				    request, (unsigned)at,
				    "IPCR WDS default-PDN client bind subscription (the SIM's)",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &cres, &cerr);
				csay = mhi_wds_bind_get_on(0x100b, out, in, cnode,
				    cport, 0x42, answer, answer_size);
				hakios_tl("pcie2: cellular: IMS: the default-PDN client binds subscription %u (the SIM's): result %u, error %u, reads back %u (%s)\n",
				    csub, cres, cerr, csay,
				    mhi_wds_subscription_name(csay));
				mhi_wds_start_client = 0x100b;
				mhi_wds_start_qcril = 1;
				mhi_wds_start_data_shape = 1;
				/* Android's dial clients send Set IP Family
				 * 0x004d themselves right before Start
				 * (14:02:23.280/.289, families 4 and 6,
				 * fd 101/104) and still carry TLV 0x19 in
				 * the body -- both go out here. */
				cup = mhi_wds_start_try(out, in, cnode, cport, -1,
				    0x06, "cbnet", 1, 1, 0x43,
				    "IPCR WDS start the default PDN (Android's data shape, cbnet, family 6, mux 3)",
				    answer, answer_size, &got);
				if (cup) {
					unsigned chandle = mhi_qmi_u32(answer, got,
					    0x01);

					hakios_tl("pcie2: cellular: IMS: THE DEFAULT (cbnet) PDN WAS ACCEPTED ON MUX 3 (packet data handle %u); the ims dial below runs with it up\n",
					    chandle);
					for (unsigned w = 0; w < 6; w++) {
						const uint8_t *cs;
						unsigned cstatus = ~0u;

						at = mhi_qmi_open(request,
						    (uint8_t)(0x44 + w), 0x0022);
						mhi_qmi_seal(request, at);
						got = mhi_qmi_call(out, in,
						    0x100b, cnode, cport, request,
						    (unsigned)at,
						    "IPCR WDS default-PDN client packet service",
						    answer, answer_size);
						cs = got != 0 ?
						    mhi_qmi_answer_tlv(answer, got,
						    0x01, &size) : NULL;
						if (cs != NULL && size >= 1) {
							cstatus = cs[0];
						}
						hakios_tl("pcie2: cellular: IMS: the default-PDN client's packet service says %u (%s)\n",
						    cstatus,
						    mhi_wds_conn_name(cstatus));
						if (cstatus == 2) {
							break;
						}
						delay(5000000);
					}
				} else {
					const uint8_t *cv = got != 0 ?
					    mhi_qmi_answer_tlv(answer, got, 0x11,
					    &size) : NULL;

					mhi_qmi_outcome(answer, got, &cres, &cerr);
					if (cv != NULL && size >= 4) {
						hakios_tl("pcie2: cellular: IMS: the default (cbnet) PDN is refused result %u, error %u, verbose reason type %u %d\n",
						    cres, cerr,
						    cv[0] | (unsigned)cv[1] << 8,
						    (int)(int16_t)(cv[2] |
						    (unsigned)cv[3] << 8));
					} else {
						hakios_tl("pcie2: cellular: IMS: the default (cbnet) PDN is refused result %u, error %u (no verbose reason)\n",
						    cres, cerr);
					}
					/* Android's accepted data starts bind
					 * mux 4 in the 13:32 capture and mux 3
					 * in the 14:02 one (two fresh clients,
					 * subscription 2, the {3, 4} port),
					 * never the ims dial's mux 2.  The
					 * attempt above took the 14:02 capture's
					 * mux 3; a refusal here takes mux 5.
					 * Mux 2 stays free
					 * for the ims dial. */
					{
						unsigned rnode = 0, rport = 0;
						uint32_t rdesc[2], rsub =
						    mhi_wds_card_sub;
						uint8_t rmux = 5;
						unsigned rres = ~0u, rerr = ~0u;

						if (mhi_qrtr_service(0x01, &rnode,
						    &rport)) {
							rdesc[0] = data_type;
							rdesc[1] = data_iface;
							at = mhi_qmi_open(request,
							    0x4a, 0x00a2);
							at = mhi_qmi_put(request,
							    at, 0x10, rdesc,
							    sizeof(rdesc));
							at = mhi_qmi_put(request,
							    at, 0x11, &rmux, 1);
							mhi_qmi_seal(request, at);
							got = mhi_qmi_call(out, in,
							    0x100c, rnode, rport,
							    request, (unsigned)at,
							    "IPCR WDS default-PDN retry client bind mux 5",
							    answer, answer_size);
							mhi_qmi_outcome(answer, got,
							    &rres, &rerr);
							hakios_tl("pcie2: cellular: IMS: the default-PDN retry client binds mux %u: result %u, error %u\n",
							    rmux, rres, rerr);
							at = mhi_qmi_open(request,
							    0x4b, 0x00af);
							at = mhi_qmi_put(request,
							    at, 0x01, &rsub,
							    sizeof(rsub));
							mhi_qmi_seal(request, at);
							got = mhi_qmi_call(out, in,
							    0x100c, rnode, rport,
							    request, (unsigned)at,
							    "IPCR WDS default-PDN retry client bind subscription (the SIM's)",
							    answer, answer_size);
							mhi_qmi_outcome(answer, got,
							    &rres, &rerr);
							hakios_tl("pcie2: cellular: IMS: the default-PDN retry client binds subscription %u (the SIM's): result %u, error %u\n",
							    rsub, rres, rerr);
							mhi_wds_start_client = 0x100c;
							mhi_wds_start_qcril = 1;
							mhi_wds_start_data_shape = 1;
							cup = mhi_wds_start_try(out,
							    in, rnode, rport, -1, 0x06,
							    "cbnet", 1, 1, 0x4c,
							    "IPCR WDS start the default PDN (Android's data shape, cbnet, family 6, mux 5)",
							    answer, answer_size, &got);
							if (cup) {
								unsigned rhandle =
								    mhi_qmi_u32(answer,
								    got, 0x01);
								unsigned rstatus = ~0u;
								const uint8_t *rs;

								hakios_tl("pcie2: cellular: IMS: THE DEFAULT (cbnet) PDN WAS ACCEPTED ON MUX 5 (packet data handle %u); the ims dial below runs with it up\n",
								    rhandle);
								for (unsigned w = 0;
								    w < 6; w++) {
									at = mhi_qmi_open(request,
									    (uint8_t)(0x50 + w),
									    0x0022);
									mhi_qmi_seal(request,
									    at);
									got = mhi_qmi_call(out,
									    in, 0x100c, rnode,
									    rport, request,
									    (unsigned)at,
									    "IPCR WDS default-PDN retry client packet service",
									    answer,
									    answer_size);
									rs = got != 0 ?
									    mhi_qmi_answer_tlv(
									    answer, got, 0x01,
									    &size) : NULL;
									if (rs != NULL &&
									    size >= 1) {
										rstatus = rs[0];
									}
									hakios_tl("pcie2: cellular: IMS: the default-PDN retry client's packet service says %u (%s)\n",
									    rstatus,
									    mhi_wds_conn_name(
									    rstatus));
									if (rstatus == 2) {
										break;
									}
									delay(5000000);
								}
							} else {
								mhi_qmi_outcome(answer,
								    got, &rres, &rerr);
								hakios_tl("pcie2: cellular: IMS: the default (cbnet) PDN with the data shape is refused on mux 5 too: result %u, error %u\n",
								    rres, rerr);
							}
						}
					}
				}
				if (!cup) {
					/* The reference's ims dial (the boot
					 * capture) runs with default 7; put it
					 * back so that dial stays comparable
					 * to the earlier runs. */
					uint8_t cback[3] = { 0x00, 0x00, 0x07 };

					at = mhi_qmi_open(request, 0x4e, 0x004a);
					at = mhi_qmi_put(request, at, 0x01,
					    cback, sizeof(cback));
					mhi_qmi_seal(request, at);
					got = mhi_qmi_call(out, in, 0x1002,
					    cnode, cport, request, (unsigned)at,
					    "IPCR WDS set default profile number 7 (after the refused default-PDN start)",
					    answer, answer_size);
					mhi_qmi_outcome(answer, got, &cpres,
					    &cperr);
					hakios_tl("pcie2: cellular: IMS: the default 3GPP profile is set back to 7 after the refused default-PDN start: result %u, error %u%s\n",
					    cpres, cperr,
					    got == 0 ? " (no answer)" : "");
				}
			}
		}

		/* qcrild's probe client, the same 70 ms before the dial: a
		 * fresh WDS socket that binds {3, 4} mux 2 and the card's
		 * subscription, then reads its packet service under the
		 * IPv6 family and then the IPv4 one, before either dial
		 * client exists. */
		{
			unsigned pnode = 0, pport = 0;
			uint32_t pdesc[2], psub = mhi_wds_card_sub;
			uint8_t pmux = 2;

			if (mhi_qrtr_service(0x01, &pnode, &pport)) {
				pdesc[0] = data_type;
				pdesc[1] = data_iface;
				at = mhi_qmi_open(request, 0x166, 0x00a2);
				at = mhi_qmi_put(request, at, 0x10, pdesc,
				    sizeof(pdesc));
				at = mhi_qmi_put(request, at, 0x11, &pmux, 1);
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x100a, pnode,
				    pport, request, (unsigned)at,
				    "IPCR WDS probe client bind mux", answer,
				    answer_size);
				mhi_qmi_outcome(answer, got, &result, &error);
				hakios_tl("pcie2: cellular: IMS: the reference's probe client binds {type %u, interface %u} mux 2: result %u, error %u\n",
				    data_type, data_iface, result, error);
				at = mhi_qmi_open(request, 0x167, 0x00af);
				at = mhi_qmi_put(request, at, 0x01, &psub,
				    sizeof(psub));
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, 0x100a, pnode,
				    pport, request, (unsigned)at,
				    "IPCR WDS probe client bind subscription (the SIM's)",
				    answer, answer_size);
				mhi_qmi_outcome(answer, got, &result, &error);
				hakios_tl("pcie2: cellular: IMS: the probe client binds subscription %u (the SIM's): result %u, error %u\n",
				    psub, result, error);
				for (unsigned pf = 0; pf < 2; pf++) {
					static const uint8_t pfams[2] =
					    { 0x06, 0x04 };
					uint8_t fam = pfams[pf];

					at = mhi_qmi_open(request,
					    (uint8_t)(0x168 + pf), 0x004d);
					at = mhi_qmi_put(request, at, 0x01,
					    &fam, 1);
					mhi_qmi_seal(request, at);
					got = mhi_qmi_call(out, in, 0x100a,
					    pnode, pport, request,
					    (unsigned)at,
					    "IPCR WDS probe client set ip family",
					    answer, answer_size);
					mhi_qmi_outcome(answer, got, &result,
					    &error);
					at = mhi_qmi_open(request,
					    (uint8_t)(0x16a + pf), 0x0022);
					mhi_qmi_seal(request, at);
					got = mhi_qmi_call(out, in, 0x100a,
					    pnode, pport, request,
					    (unsigned)at,
					    "IPCR WDS probe client packet service",
					    answer, answer_size);
					mhi_qmi_outcome(answer, got, &result,
					    &error);
					{
						const uint8_t *ps = got != 0 ?
						    mhi_qmi_answer_tlv(answer,
						    got, 0x01, &size) : NULL;
						unsigned pstatus = ps != NULL &&
						    size >= 1 ? ps[0] : ~0u;

						hakios_tl("pcie2: cellular: IMS: the probe client's family-%u packet service says result %u, error %u, status %u (%s)\n",
						    fam, result, error, pstatus,
						    mhi_wds_conn_name(pstatus));
					}
				}
			} else {
				hakios_tl("pcie2: cellular: IMS: the WDS service is not announced; the probe client is skipped\n");
			}
		}

		for (unsigned f = 0; f < fresh_count; f++) {
			unsigned fnode = 0, wport = 0;
			uint32_t desc[2], sub = mhi_wds_card_sub;
			uint8_t id = (uint8_t)fresh_muxes[f];
			unsigned fres = ~0u, ferr = ~0u, fconnected = ~0u;
			unsigned fhandle;

			if (!mhi_qrtr_service(0x01, &fnode, &wport)) {
				break;
			}
			desc[0] = data_type;
			desc[1] = data_iface;
			at = mhi_qmi_open(request, (uint8_t)(0xb0 + f), 0x00a2);
			at = mhi_qmi_put(request, at, 0x10, desc, sizeof(desc));
			at = mhi_qmi_put(request, at, 0x11, &id, 1);
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, fresh_ports[f], fnode, wport,
			    request, (unsigned)at,
			    "IPCR WDS fresh client bind mux", answer, answer_size);
			mhi_qmi_outcome(answer, got, &fres, &ferr);
			hakios_tl("pcie2: cellular: IMS: the fresh client %#x binds mux %u on the {type %u, interface %u} port: result %u, error %u\n",
			    fresh_ports[f], fresh_muxes[f], data_type, data_iface,
			    fres, ferr);
			if (got == 0 || fres != 0) {
				fresh_refused = 1;
				continue;
			}
			at = mhi_qmi_open(request, (uint8_t)(0xb2 + f), 0x00af);
			at = mhi_qmi_put(request, at, 0x01, &sub, sizeof(sub));
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, fresh_ports[f], fnode, wport,
			    request, (unsigned)at,
			    "IPCR WDS fresh client bind subscription", answer,
			    answer_size);
			mhi_qmi_outcome(answer, got, &fres, &ferr);
			hakios_tl("pcie2: cellular: IMS: the fresh client %#x binds subscription %u (the SIM's): result %u, error %u\n",
			    fresh_ports[f], sub, fres, ferr);
			said = mhi_wds_bind_get_on(fresh_ports[f], out, in,
			    fnode, wport, (uint8_t)(0xe0 + f), answer,
			    answer_size);
			hakios_tl("pcie2: cellular: IMS: after the bind the fresh client %#x reads subscription %u (%s)\n",
			    fresh_ports[f], said,
			    mhi_wds_subscription_name(said));

			/* The vendor IMS data service's own Start (imscap2
			 * 06:17:32.319) on the IPv6 leg: Set IP Family 6,
			 * then exactly APN "ims", the family, profile 2,
			 * 0x32 = 0xff and the embedded call type -- no auth,
			 * no 0x39/0x3d, no mask.  On the working phone the
			 * modem's IMS stack registered 0.28 s after this
			 * start's answer; every mask-shape leg this driver
			 * has had accepted left IMSA at state 0. */
			mhi_wds_start_client = fresh_ports[f];
			if (f == 0 || f == 2) {
				unsigned daemon_family =
				    (f == 2 && ims_v4_on != 0) ? 0x04 :
				    (f == 0 && ims_f0v4_on != 0) ? 0x04 : 0x06;

				mhi_wds_start_daemon_ims = 1;
				accepted = mhi_wds_start_try(out, in, fnode,
				    wport, f == 0 ? 2 : -1, daemon_family, "ims", 1, 1,
				    (uint8_t)(0xb4 + f),
				    daemon_family == 0x04 ?
				    (f == 0 ?
				    "IPCR WDS start on the fresh client (the IMS data service's own shape, profile 2, family 4: pcie2_ims_f0v4=1)" :
				    "IPCR WDS start on the fresh client (the IMS data service's own shape, no profile index, family 4: pcie2_ims_v4=1)") :
				    "IPCR WDS start on the fresh client (the IMS data service's own shape: APN ims, the leg's profile index, 0x32 = 0xff)",
				    answer, answer_size, &got);
				if (accepted) {
					fresh_apn_up[f] = 1;
					/* The leg's family decides which
					 * address TLV the current-settings
					 * read below looks for (0x1e V4,
					 * 0x26 V6); without this an
					 * accepted family-4 leg kept the
					 * static 0x06 and never gave the
					 * stack an address. */
					fresh_families[f] = daemon_family;
				} else {
					mhi_qmi_outcome(answer, got, &fres,
					    &ferr);
					value = got != 0 ?
					    mhi_qmi_answer_tlv(answer, got,
					    0x11, &size) : NULL;
					if (value != NULL && size >= 4) {
						hakios_tl("pcie2: cellular: IMS: the family-%u form of the IMS data service's own start is refused result %u, error %u, verbose reason type %u %d\n",
						    daemon_family, fres, ferr,
						    value[0] | (unsigned)value[1] << 8,
						    (int)(int16_t)(value[2] |
						    (unsigned)value[3] << 8));
					} else {
						hakios_tl("pcie2: cellular: IMS: the family-%u form of the IMS data service's own start is refused result %u, error %u\n",
						    daemon_family, fres, ferr);
					}
					if (daemon_family == 0x04) {
						/* The first start consumed the
						 * client global; the retry needs
						 * the fresh client again, not the
						 * shared 0x1002 default. */
						mhi_wds_start_client = fresh_ports[f];
						/* The captured shape's own
						 * family is 6: a refused V4 leg
						 * still gets the boot its PDN,
						 * and the log then says the
						 * grant was V6 against a V4
						 * ask. */
						mhi_wds_start_daemon_ims = 1;
						accepted = mhi_wds_start_try(out,
						    in, fnode, wport,
						    f == 0 ? 2 : -1, 0x06,
						    "ims", 1, 1,
						    (uint8_t)(0xb5 + f),
						    "IPCR WDS start on the fresh client (the captured V6 leg after the refused V4 one)",
						    answer, answer_size, &got);
						if (accepted) {
							fresh_apn_up[f] = 1;
							fresh_families[f] = 0x06;
						}
					}
				}
			}
			if (f == 1) {
				/* The Start body this very boot accepted for the
				 * default PDN -- auth none, the APN inline, the
				 * family, the embedded call type, 0x39 = 1 and
				 * the data shape's 0x3c = 1 -- carrying the ims
				 * APN and profile 2, the profile the live phone's
				 * accepted IMS call names.  Same posture as f0
				 * (mux 4, subscription 2); the byte order and
				 * qcril's extra TLVs are the difference. */
				mhi_wds_start_qcril = 1;
				mhi_wds_start_data_shape = 1;
				accepted = mhi_wds_start_try(out, in, fnode,
				    wport, 2, 0x06, "ims", 1, 1,
				    (uint8_t)(0xb6 + f),
				    "IPCR WDS start on the fresh client (qcril's accepted data body with APN ims and profile 2, mux 4)",
				    answer, answer_size, &got);
				if (accepted) {
					fresh_apn_up[f] = 1;
					fresh_families[f] = 0x06;
				}
			}
			if (f == 3) {
				/* The fallback leg (mux 2, no APN): Set IP Family for this
				 * leg, then the 31-byte Start (auth none,
				 * family, embedded call type, 0x39 = 1,
				 * 0x3d = 1, the ims APN-type mask 2), the form
				 * every earlier boot accepted,
				 * kept last so a refused ladder still leaves a PDN. */
				mhi_wds_start_client = fresh_ports[f];
				mhi_wds_start_qcril = 1;
				mhi_wds_start_apn_mask = 2;
				accepted = mhi_wds_start_try(out, in, fnode,
				    wport, fresh_profiles[f],
				    fresh_families[f], NULL, 1, 1,
				    (uint8_t)(0xb4 + f),
				    f == 0 ?
				    "IPCR WDS start on the fresh client (the fallback: the reference v6 leg on mux 2, no APN)" :
				    f == 1 ?
				    "IPCR WDS start on the fresh client (the fallback: the reference v6 leg on mux 2, no APN)" :
				    "IPCR WDS start on the fresh client (the fallback: the reference v6 leg on mux 2, no APN)",
				    answer, answer_size, &got);
			}
			if (!accepted) {
				fresh_refused = 1;
				mhi_qmi_outcome(answer, got, &fres, &ferr);
				value = got != 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x11, &size) : NULL;
				if (value != NULL && size >= 4) {
					hakios_tl("pcie2: cellular: IMS: the fresh client %#x's start is refused result %u, error %u, verbose reason type %u %d -- its only WDS traffic was the reference's\n",
					    fresh_ports[f], fres, ferr,
					    value[0] | (unsigned)value[1] << 8,
					    (int)(int16_t)(value[2] |
					    (unsigned)value[3] << 8));
				} else {
					hakios_tl("pcie2: cellular: IMS: the fresh client %#x's start is refused result %u, error %u -- its only WDS traffic was the reference's\n",
					    fresh_ports[f], fres, ferr);
				}
				/* A refused start can still mean the session is
				 * already up (autoconnect); the packet service
				 * on this client is the answer. */
				at = mhi_qmi_open(request, (uint8_t)(0xba + f),
				    0x0022);
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, fresh_ports[f],
				    fnode, wport, request, (unsigned)at,
				    "IPCR WDS fresh client packet service", answer,
				    answer_size);
				value = got != 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x01, &size) : NULL;
				if (value != NULL && size >= 1) {
					fconnected = value[0];
				}
				hakios_tl("pcie2: cellular: IMS: the fresh client %#x's packet service says %u (%s)\n",
				    fresh_ports[f], fconnected,
				    mhi_wds_conn_name(fconnected));
				if (send_attach_detach && f == 0) {
					/* The reference's own attach-detach form,
					 * between the two clients: NAS Attach
					 * Detach 0x0023 with TLV 0x16 and a
					 * 4-byte action (qmi5 line 1112 -- the
					 * airplane-mode data rebuild -- and the
					 * qmi3 boot lines 151/289), value 2.
					 * The removed rebuild block sent the
					 * libqmi-era TLV 0x10 instead, one byte,
					 * and the modem answered result 0 and
					 * then NO_EFFECT -- the second client
					 * never dialled in a state this host had
					 * actually set. */
					uint8_t nreq[16];
					unsigned nnode = 0, nport = 0;
					unsigned nresult = ~0u, nerror = ~0u;
					uint32_t nact = 0x02;

					if (mhi_qrtr_service(0x03, &nnode,
					    &nport)) {
						size_t nat = mhi_qmi_open(nreq,
						    0xf5, 0x0023);

						nat = mhi_qmi_put(nreq, nat,
						    0x16, &nact,
						    sizeof(nact));
						mhi_qmi_seal(nreq, nat);
						got = mhi_qmi_call(out, in,
						    0x1001, nnode, nport, nreq,
						    (unsigned)nat,
						    "IPCR NAS attach detach (qcril's 0x16 form, action 2)",
						    answer, answer_size);
						mhi_qmi_outcome(answer, got,
						    &nresult, &nerror);
						hakios_tl("pcie2: cellular: IMS: qcril's attach detach (TLV 0x16, action 2) before the second client says result %u, error %u%s\n",
						    nresult, nerror,
						    got == 0 ?
						    " (no answer)" : "");
					} else {
						hakios_tl("pcie2: cellular: IMS: the NAS service is not announced; the second client dials attached\n");
					}
				}
				continue;
			}
			fhandle = mhi_qmi_u32(answer, got, 0x01);
			hakios_tl("pcie2: cellular: IMS: THE FRESH CLIENT'S START WAS ACCEPTED (client %#x, mux %u, family %u, packet data handle %u): the CM ACL refusal was not this client's state\n",
			    fresh_ports[f], fresh_muxes[f], fresh_families[f],
			    fhandle);

			/* The session's address, from the client that started it,
			 * with Android's own Get Current Settings mask. */
			at = mhi_qmi_open(request, (uint8_t)(0xbc + f), 0x002d);
			{
				uint32_t asked = 0x00008210u;

				at = mhi_qmi_put(request, at, 0x10, &asked,
				    sizeof(asked));
			}
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, fresh_ports[f], fnode, wport,
			    request, (unsigned)at,
			    "IPCR WDS fresh client current settings", answer,
			    answer_size);
			if (got != 0) {
				const uint8_t *see;
				char hex[8 * 5 + 1], dns1[8 * 5 + 1];
				char dns2[8 * 5 + 1] = "";
				unsigned flen = 0, ffam = ~0u;
				size_t hx = 0;

				see = mhi_qmi_answer_tlv(answer, got, 0x26,
				    &size);
				if (see != NULL && size >= 16) {
					/* Android's own capture of this TLV
					 * (qcrild's IMS client, mask 0x400):
					 * 24 0a 42 e6 08 10 0e bb 60 e3 56
					 * ce 48 e0 43 58 40 -- the sixteen
					 * address bytes first and the prefix
					 * length last, a /64.  Read the other
					 * way round it looked like /36 and an
					 * address beginning 0a42, outside
					 * 2000::/3: that is the text every
					 * run so far handed the modem's IMS
					 * stack, which cannot register a
					 * source address the PDN never gave. */
					flen = size >= 17 ? see[16] : 64u;
					hx = 0;
					for (unsigned h = 0; h < 8; h++) {
						hx += (size_t)snprintf(hex + hx,
						    sizeof(hex) - hx, "%s%02x%02x",
						    h ? ":" : "", see[2 * h],
						    see[1 + 2 * h]);
					}
					/* The IMS data service hands the modem's
					 * IMS stack this address as text, in
					 * inet_ntop's form; keep it in that form
					 * for the indication (mhi_imsdcm_indicate).
					 * Only a V6 leg's address is this one; a
					 * V4 leg's is TLV 0x1e below. */
					if (fresh_families[f] == 0x06) {
						hx = 0;
						for (unsigned h = 0; h < 8; h++) {
							unsigned group =
							    see[2 * h] << 8 | see[1 + 2 * h];

							hx += (size_t)snprintf(
							    mhi_imsdcm_address + hx,
							    sizeof(mhi_imsdcm_address) - hx,
							    "%s%x", h ? ":" : "", group);
						}
						/* A rebuilt address is news to the
						 * stack if it already had the old one. */
						mhi_imsdcm_indicated = false;
					}
				} else {
					snprintf(hex, sizeof(hex), "none");
				}
				see = mhi_qmi_answer_tlv(answer, got, 0x27,
				    &size);
				if (see != NULL && size >= 16) {
					hx = 0;
					for (unsigned h = 0; h < 8; h++) {
						hx += (size_t)snprintf(dns1 + hx,
						    sizeof(dns1) - hx, "%s%02x%02x",
						    h ? ":" : "", see[2 * h],
						    see[1 + 2 * h]);
					}
				} else {
					snprintf(dns1, sizeof(dns1), "none");
				}
				see = mhi_qmi_answer_tlv(answer, got, 0x28,
				    &size);
				if (see != NULL && size >= 16) {
					hx = 0;
					for (unsigned h = 0; h < 8; h++) {
						hx += (size_t)snprintf(dns2 + hx,
						    sizeof(dns2) - hx, "%s%02x%02x",
						    h ? ":" : "", see[2 * h],
						    see[1 + 2 * h]);
					}
				} else {
					snprintf(dns2, sizeof(dns2), "none");
				}
				see = mhi_qmi_answer_tlv(answer, got, 0x2b, &size);
				if (see != NULL && size >= 1) {
					ffam = see[0];
				}
				hakios_tl("pcie2: cellular: IMS: the family-%u leg's started session carries ipv6 %s/%u, DNS %s and %s, family %u\n",
				    fresh_families[f], hex, flen, dns1, dns2, ffam);
				/* Every TLV of this answer: the mask above asks
				 * for the address and DNS only, but the dump shows
				 * whether the modem also volunteers the P-CSCF
				 * (TLV 0x2a in libqmi's IDL) the IMS stack needs. */
				mhi_qmi_tlvs(answer, got,
				    "IPCR WDS fresh client current settings");
				/* The IPv4 address of a V4 leg: TLV 0x1e of the
				 * same answer; when the captured mask carried
				 * none, one more read with the shared client's
				 * mask (the one the earlier current-settings
				 * read used) runs before the leg is called
				 * addressless. */
				if (fresh_families[f] == 0x04) {
					const uint8_t *a4;
					char b4[16 * 3 + 1];
					unsigned bn;

					a4 = mhi_qmi_answer_tlv(answer, got, 0x1e,
					    &size);
					bn = 0;
					for (unsigned h = 0; a4 != NULL &&
					    h < size && h < 16; h++) {
						bn += (size_t)snprintf(b4 + bn,
						    sizeof(b4) - bn, "%s%02x",
						    h ? " " : "", a4[h]);
					}
					b4[bn] = 0;
					hakios_tl("pcie2: cellular: IMS: the family-4 leg's current settings carry TLV 0x1e %s%s\n",
					    a4 != NULL ? "(" : "not at all",
					    a4 != NULL ? b4 : "");
					if (!mhi_wds_v4_address(answer, got)) {
						uint32_t asked2 = 0x0000830cu;

						at = mhi_qmi_open(request,
						    (uint8_t)(0xc4 + f), 0x002d);
						at = mhi_qmi_put(request, at, 0x10,
						    &asked2, sizeof(asked2));
						mhi_qmi_seal(request, at);
						got = mhi_qmi_call(out, in,
						    fresh_ports[f], fnode, wport,
						    request, (unsigned)at,
						    "IPCR WDS fresh client current settings (the ipv4 mask)",
						    answer, answer_size);
						if (mhi_wds_v4_address(answer, got)) {
							hakios_tl("pcie2: cellular: IMS: the family-4 leg's address is %s (from the second current-settings read)\n",
							    mhi_imsdcm_address);
						} else {
							hakios_tl("pcie2: cellular: IMS: the family-4 leg reports no ipv4 address in either read\n");
						}
					} else {
						hakios_tl("pcie2: cellular: IMS: the family-4 leg's address is %s\n",
						    mhi_imsdcm_address);
					}
				}
			} else {
				hakios_tl("pcie2: cellular: IMS: the fresh client's current settings did not answer\n");
			}

			/* The PDN connects asynchronously; this client's packet
			 * service names the moment. */
			for (unsigned waited = 0; waited < 60000;
			    waited += 10000) {
				at = mhi_qmi_open(request, (uint8_t)(0xbe + f),
				    0x0022);
				mhi_qmi_seal(request, at);
				got = mhi_qmi_call(out, in, fresh_ports[f],
				    fnode, wport, request, (unsigned)at,
				    "IPCR WDS fresh client packet service", answer,
				    answer_size);
				value = got != 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x01, &size) : NULL;
				if (value != NULL && size >= 1) {
					fconnected = value[0];
					if (fconnected == 2) {
						break;
					}
				}
				delay(10000000);
			}
			hakios_tl("pcie2: cellular: IMS: the family-%u leg's packet service says %u (%s)\n",
			    fresh_families[f], fconnected,
			    mhi_wds_conn_name(fconnected));
			/* With the PDN up, the IMS data service hands the
			 * address the modem's IMS stack asked for.  Android's
			 * daemon sends the address indication only after the
			 * stack's APN request (imscap2 06:17:32.02 -> .56), so
			 * when the request came first -- before this PDN -- it
			 * goes out here, at that pace; the ask's own branch in
			 * mhi_imsdcm_answer covers the other order. */
			if (fconnected == 2 && mhi_imsdcm_port != 0 &&
			    mhi_imsdcm_asked && !mhi_imsdcm_indicated) {
				if (mhi_imsdcm_address[0] != 0) {
					/* Android's daemon hands the address
					 * over in its own receive path 0.44-0.53 s
					 * after the stack's ask (imscap2
					 * 06:17:32.02 -> .56); delivering it here
					 * is that pace.  Waiting for the cellular
					 * thread's next cycle left run 214445's
					 * handoff minutes late, and its stack
					 * never left state 0. */
					delay(530000);
					if (mhi_imsdcm_indicate(out,
					    "the fresh client's PDN is up")) {
						mhi_imsdcm_pending = false;
					}
				} else {
					mhi_imsdcm_pending = true;
					hakios_tl("pcie2: cellular: IMS: the address is held for the cellular thread's next step (the PDN is up, no address)\n");
				}
			}
			fresh_accepted = 1;
			if (fresh_apn_up[f]) {
				/* An ims-APN PDN is up; a
				 * second start on this mux would only muddy
				 * the registration evidence. */
				break;
			}
			continue;
		}
		accepted = fresh_accepted;
		if (accepted) {
			return 1;
		}
		if (fresh_refused) {
			uint32_t sub1 = 1;
			unsigned showed, fnode2 = 0, wport2 = 0;

			hakios_tl("pcie2: cellular: IMS: every fresh client's start is refused like the shared client's; the shared client's start matrix is not repeated (run 015549 ran it in full)\n");
			if (!mhi_qrtr_service(0x01, &fnode2, &wport2)) {
				mhi_pdc_restore_own(out, in);
				return 0;
			}
			/* The card moves.  The UIM block's switch sends
			 * {logical slot 1, physical slot 2} and, once the card
			 * comes back in logical slot 1, makes application 0 of
			 * that slot the primary GW one.  Every WDS client in this
			 * boot binds subscription 2 -- where the card sat before
			 * the switch -- and every start is refused with the CM
			 * ACL reason 1042, while live Android (whose card status
			 * reads slot 1 ERROR, slot 2 PRESENT READY, primary GW
			 * application 65535, no switch ever sent) dials its PDN
			 * from clients bound to 2 without a refusal.  One bind to
			 * subscription 1, read back, then the reference start
			 * under it separates "the clients are on the slot the
			 * card left" from the ACL itself. */
			at = mhi_qmi_open(request, 0xe8, 0x00af);
			at = mhi_qmi_put(request, at, 0x01, &sub1,
			    sizeof(sub1));
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, fresh_ports[0], fnode2,
			    wport2, request, (unsigned)at,
			    "IPCR WDS bind subscription 1 (the card's post-switch slot)",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: the fresh client %#x binds subscription 1 (the card's post-switch logical slot): result %u, error %u\n",
			    fresh_ports[0], result, error);
			if (got != 0 && result == 0) {
				showed = mhi_wds_bind_get_on(fresh_ports[0],
				    out, in, fnode2, wport2, 0xe9, answer,
				    answer_size);
				hakios_tl("pcie2: cellular: IMS: after the bind to 1 the fresh client %#x reads subscription %u (%s)\n",
				    fresh_ports[0], showed,
				    mhi_wds_subscription_name(showed));
				mhi_wds_start_client = fresh_ports[0];
				mhi_wds_start_qcril = 1;
				mhi_wds_start_apn_mask = 2;
				accepted = mhi_wds_start_try(out, in, fnode2,
				    wport2, -1, 0x04, NULL, 1, 1, 0xea,
				    "IPCR WDS start on subscription 1 (the card's post-switch slot)",
				    answer, answer_size, &got);
				if (accepted) {
					hakios_tl("pcie2: cellular: IMS: THE START ON SUBSCRIPTION 1 WAS ACCEPTED: the CM ACL refusal was the subscription binding\n");
					return 1;
				}
				mhi_qmi_outcome(answer, got, &result, &error);
				value = got != 0 ? mhi_qmi_answer_tlv(answer,
				    got, 0x11, &size) : NULL;
				if (value != NULL && size >= 4) {
					hakios_tl("pcie2: cellular: IMS: the start on subscription 1 is refused result %u, error %u, verbose reason type %u %d\n",
					    result, error,
					    value[0] | (unsigned)value[1] << 8,
					    (int)(int16_t)(value[2] |
					    (unsigned)value[3] << 8));
				} else {
					hakios_tl("pcie2: cellular: IMS: the start on subscription 1 is refused result %u, error %u\n",
					    result, error);
				}
			}
			mhi_pdc_restore_own(out, in);
			return 0;
		}
	}

	/* The CMCC experiment's undo lives in mhi_pdc_restore_own (it also
	 * runs before the fresh-client matrix returns): the modem's own
	 * config is selected and activated again and read back. */
	mhi_pdc_restore_own(out, in);

	ims_profile = mhi_wds_ims_profile(out, in, node, port, answer,
	    answer_size, &data_profile);
	/* No create here.  Run 124850's Create Profile landed on profile 6 --
	 * named "qdp_profile_ia", the initial-attach profile the modem booted
	 * with -- and left the APN "ims" in a store Android shares.  The
	 * APN-inline and control starts below need no new profile at all;
	 * mhi_wds_ims_create stays for a deliberate, indexed write only. */
	if (ims_profile < 0 && write_profile) {
		ims_profile = mhi_wds_ims_create(out, in, node, port, answer,
		    answer_size);
	}
	/* The attach by now: the set above asks the modem to rebuild it, and
	 * the profile survey took minutes, so these reads show whether the
	 * attach moved to profile 1's APN before the matrix runs.  Run
	 * 165605's single read went unanswered -- most likely while the modem
	 * rebuilt the attach -- and a silence is not an answer, so it is
	 * retried. */
	{
		unsigned attached = ~0u, iptype = ~0u, round;

		for (round = 0; round < 3; round++) {
			if (mhi_wds_attach_params(out, in, node, port,
			    (uint8_t)(0xb7 + round), answer, answer_size,
			    &attached, &iptype, attach_apn,
			    sizeof(attach_apn))) {
				attach_answered = 1;
				break;
			}
			hakios_tl("pcie2: cellular: IMS: the attach read did not answer successfully (round %u of 3)\n",
			    round + 1);
			if (round < 2) {
				delay(5000000);
			}
		}
		hakios_tl("pcie2: cellular: IMS: before the start the LTE attach is attached %u, ip type %u, APN \"%s\"\n",
		    attached, iptype, attach_apn);
	}
	at = mhi_qmi_open(request, 0xba, 0x0094);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS LTE attach PDN list before the start", answer,
	    answer_size);
	if (got != 0) {
		uint16_t cur[8];
		unsigned ncur = 0;

		value = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
		if (value != NULL) {
			ncur = mhi_wds_attach_ids(value, size, cur,
			    sizeof(cur) / sizeof(cur[0]));
		}
		hakios_tl("pcie2: cellular: IMS: before the start the attach PDN list holds %u profiles\n",
		    ncur);
		for (unsigned i = 0; i < ncur; i++) {
			hakios_tl("pcie2: cellular: IMS: before the start attach profile %u of %u is %u\n",
			    i + 1, ncur, cur[i]);
		}
	}
	/* A list set with action none only takes effect the next time the modem
	 * rebuilds the attach.  Run 203116 tried the list's detach action here
	 * to force that rebuild: the PS domain went down, every attach read
	 * after it stayed unanswered, and the matrix that followed ran with
	 * the attach in that state -- so the action is not repeated.  The
	 * default profile number set below is Android's own lever, and the
	 * reason the attach runs "ims" at all: the modem's default is
	 * profile 6, run 124850's create, whose APN is "ims". */
	if (attach_answered && attach_apn[0] != 0 &&
	    strcmp(attach_apn, "cbnet") != 0) {
		hakios_tl("pcie2: cellular: IMS: the attach runs on \"%s\", not cbnet; the default profile set below is the lever\n",
		    attach_apn);
	}
	/* The default-profile dance now runs before the dial (see the block
	 * before the DSD replay): Android's sets happen at boot and before
	 * its Start, never here, and applying them to the wrong subscription
	 * changed nothing for 99 runs. */
	/* The attach is steered before the registration, not rebuilt after it:
	 * run 225123's mid-run OFFLINE was accepted at once, every ONLINE in
	 * the next 50 s was refused 60 INVALID_TRANSITION (the closed window
	 * mhi_dms_online's note records), and the modem stayed OFFLINE into
	 * the matrix -- the rest of that run was spent blind.  The sets that
	 * count (default profile 1, attach list {1}) go out from the boot
	 * before NAS registers (mhi_wds_early), so nothing is asked here. */
	/* What the attach runs now, read (0x0085) rather than forced: with the
	 * early steering it should carry cbnet by the time the IMS phase
	 * starts. */
	{
		unsigned attached = ~0u, iptype = ~0u, round;

		for (round = 0; round < 10; round++) {
			if (mhi_wds_attach_params(out, in, node, port,
			    (uint8_t)(0xac + round), answer, answer_size,
			    &attached, &iptype, attach_apn,
			    sizeof(attach_apn))) {
				hakios_tl("pcie2: cellular: IMS: the attach after the default set is attached %u, ip type %u, APN \"%s\" (round %u)\n",
				    attached, iptype, attach_apn, round + 1);
				if (attach_apn[0] != 0) {
					break;
				}
			} else {
				hakios_tl("pcie2: cellular: IMS: the attach read after the default set did not answer (round %u of 10)\n",
				    round + 1);
			}
			delay(5000000);
		}
	}

	/* The registration the starts below are judged against, read now that
	 * the attach has had its rebuild window: NAS serving system and
	 * signal, the same answers the status bar's sysctls come from. */
	mhi_cellular_poll(out, in);

	/* The fallback DSD replay for a boot whose data path never opened
	 * (the normal call sits before the fresh-client starts, whose refusal
	 * returns early): mhi_dsd_arm's one-per-boot guard keeps this from
	 * sending the conversation a second time. */
	mhi_dsd_arm(out, in, answer, answer_size);

	/* What qcrild's own accepted dial looks like, captured from the running
	 * phone at the 2026-11-23 01:16 airplane-mode re-setup (strace on
	 * qcrild 2038/2074, both families): its clients bind to subscription 2
	 * (0x00af TLV 0x01 = 2), Set IP Family (0x004d), then Start Network
	 * with auth none (0x16 = 0), the family (0x19), the embedded call type
	 * (0x35 = 1), two one-byte flags the forms below never sent
	 * (0x39 = 1, 0x3d = 1) and an 8-byte APN type mask (0x3e = 2, the
	 * QmiWdsApnTypeMask ims bit) -- and no APN and no profile index, so
	 * the modem picks the profile by the mask's type.  Every form below
	 * lacks 0x39/0x3d/0x3e and every one was denied 1042 CM_ACL_FAILURE on
	 * this modem, so the reference's shape goes first.  Its secondary bind
	 * differs from the early setup's primary one too; the bind is
	 * unanswered on this firmware like the primary was, so the read-back
	 * afterwards says whether the modem took it. */
	if (!accepted) {
		uint32_t sub = mhi_wds_card_sub;

		at = mhi_qmi_open(request, 0xc9, 0x00af);
		at = mhi_qmi_put(request, at, 0x01, &sub, sizeof(sub));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at,
		    "IPCR WDS bind subscription (the reference's, the SIM's)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the bind to subscription %u (the reference's, the SIM's) says result %u, error %u%s\n",
		    sub, result, error, got == 0 ? " (no answer)" : "");
		said = mhi_wds_bind_get(out, in, node, port, 0xca, answer,
		    answer_size);
		if (said != ~0u) {
			bound = said;
		}
		hakios_tl("pcie2: cellular: IMS: after the bind to 2 the client reads subscription %u (%s)\n",
		    bound, mhi_wds_subscription_name(bound));
	}
	if (!accepted) {
		mhi_wds_start_qcril = 1;
		mhi_wds_start_apn_mask = 2;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    NULL, 1, 1, 0xc5,
		    "IPCR WDS start the reference way (ims mask, family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		mhi_wds_start_qcril = 1;
		mhi_wds_start_apn_mask = 2;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x06,
		    NULL, 1, 1, 0xc6,
		    "IPCR WDS start the reference way (ims mask, family 6)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		mhi_wds_start_qcril = 1;
		mhi_wds_start_apn_mask = 1;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    NULL, 1, 1, 0xc7,
		    "IPCR WDS start the reference way (default mask, family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		mhi_wds_start_qcril = 1;
		mhi_wds_start_apn_mask = 1;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    "cbnet", 1, 1, 0xc8,
		    "IPCR WDS start the reference way (default mask and cbnet APN)",
		    answer, answer_size, &got);
	}
	/* What Android's own accepted start looked like, never tried here:
	 * its radio log shows the default profile set to 1 (.365) and 7
	 * (.391) and then "ProfileId based Bringup" with APN cbnet and the
	 * embedded call type at .51.  Android's IPV4V6 has no QmiWdsIpFamily
	 * value (family 8 is UNSPECIFIED, not IPv4v6), so the profile forms
	 * go first with no family at all -- the profile's PDP type decides --
	 * then profile 1 with family 4, then profile 1 alone to isolate the
	 * APN and call type. */
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, 1, 0xff,
		    "cbnet", 1, 0, 0xfc,
		    "IPCR WDS start by profile 1 and the cbnet APN (family left to the profile)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, 7, 0xff,
		    "cbnet", 1, 0, 0xfd,
		    "IPCR WDS start by profile 7 and the cbnet APN (family left to the profile)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, 1, 0x04,
		    "cbnet", 1, 0, 0xfe,
		    "IPCR WDS start by profile 1 and the cbnet APN (family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, 1, 0xff,
		    NULL, 1, 0, 0xff,
		    "IPCR WDS start by profile 1 alone (embedded call type)",
		    answer, answer_size, &got);
	}
	/* The reference phone's form next: no profile, no APN, the IP family
	 * inline, no call type.  Then qcril's forms with the embedded call
	 * type, with family 4 -- run 121242 sent family 8, which is
	 * UNSPECIFIED in QmiWdsIpFamily (not IPv4v6: that is the profile's
	 * PDP type), and the firmware refused it error 48.  The last ims
	 * profile form drops the call type to isolate TLV 0x35. */
	accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04, NULL, 0, 1,
	    0xe8, "IPCR WDS start by the modem's default profile (reference form)",
	    answer, answer_size, &got);
	if (!accepted && ims_profile >= 0) {
		accepted = mhi_wds_start_try(out, in, node, port, ims_profile,
		    0x04, NULL, 1, 0, 0xe1,
		    "IPCR WDS start by the ims profile (family 4)", answer,
		    answer_size, &got);
	}
	if (!accepted && ims_profile >= 0) {
		accepted = mhi_wds_start_try(out, in, node, port,
		    ims_profile, 0xff, NULL, 1, 0, 0xe2,
		    "IPCR WDS start by the ims profile (no family)",
		    answer, answer_size, &got);
	}
	if (!accepted && ims_profile >= 0) {
		accepted = mhi_wds_start_try(out, in, node, port,
		    ims_profile, 0x04, "ims", 1, 0, 0xe7,
		    "IPCR WDS start by the ims profile and APN (family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted && default_profile != ~0u &&
	    (int)default_profile != ims_profile) {
		accepted = mhi_wds_start_try(out, in, node, port,
		    (int)default_profile, 0x04, NULL, 1, 0, 0xe6,
		    "IPCR WDS start by the modem's own profile (family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04, "ims",
		    1, 0, 0xe3, "IPCR WDS start by the ims APN (family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted && ims_profile >= 0) {
		accepted = mhi_wds_start_try(out, in, node, port,
		    ims_profile, 0x04, "ims", 0, 0, 0xe9,
		    "IPCR WDS start by the ims profile and APN (no call type)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x06, "ims",
		    1, 0, 0xe4, "IPCR WDS start by the ims APN (family 6)",
		    answer, answer_size, &got);
	}
	/* The control: a plain data PDN by the profile the survey found with
	 * a non-ims APN.  If this one connects, the data path is sound and
	 * the ims refusals above are the ims PDN itself, not DPM/WDA/WDS. */
	if (!accepted && data_profile >= 0) {
		control = 1;
		accepted = mhi_wds_start_try(out, in, node, port, data_profile,
		    0x04, NULL, 0, 0, 0xea,
		    "IPCR WDS start by the data profile (family 4, control)",
		    answer, answer_size, &got);
	}
	/* No form before this ever named a technology, and the firmware's ACL
	 * denials include ACL_DENY_REASON_UNPREFERRED_RAT: the 3GPP preference
	 * (TLV 0x30) is the axis.  The EPC preference (TLV 0x34, 34944) is
	 * not tried: run 172318's two forms carrying it were answered error 48
	 * (InvalidArgument) before any call was attempted, so this firmware
	 * will not take it.
	 *
	 * The SIM is China Broadnet's (中国广电) and its data APN is "cbnet",
	 * not "ims": profiles 1 ("qdp_profile_ia") and 7 both carry cbnet,
	 * and the user's own network settings say data needs APN cbnet.  The
	 * denials include APN_DISABLED and MANDATORY_APN_DISABLED, so a start
	 * that names cbnet tests whether the CM's access policy wants exactly
	 * that APN.  The reference-shaped form first, then qcril's embedded
	 * one, then the 3GPP preference with the APN. */
	if (!accepted) {
		mhi_wds_start_tech = 1;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04, NULL,
		    0, 1, 0xeb,
		    "IPCR WDS start with the 3GPP technology preference",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    "cbnet", 0, 1, 0xf0,
		    "IPCR WDS start by the cbnet APN (reference form)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    "cbnet", 1, 0, 0xf1,
		    "IPCR WDS start by the cbnet APN (family 4)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		mhi_wds_start_tech = 1;
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x04,
		    "cbnet", 1, 0, 0xf2,
		    "IPCR WDS start by the cbnet APN with the 3GPP preference",
		    answer, answer_size, &got);
	}
	/* CBN's APNs under Android are IPV4V6; the family-4 cbnet forms above
	 * never asked for IPv6, and profile 7 carries the same "qdp_profile_ia"
	 * APN as profile 1 but was never started by index (the control used
	 * profile 1). */
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, -1, 0x06,
		    "cbnet", 1, 0, 0xfa,
		    "IPCR WDS start by the cbnet APN (family 6)",
		    answer, answer_size, &got);
	}
	if (!accepted) {
		accepted = mhi_wds_start_try(out, in, node, port, 7, 0x04,
		    NULL, 0, 0, 0xfb,
		    "IPCR WDS start by profile 7 (qdp_profile_ia, family 4)",
		    answer, answer_size, &got);
	}
	/* If every start was refused and a bind answered at all, one start
	 * under the other subscription separates "the wrong subscription"
	 * from the CM's access list itself. */
	if (!accepted && bind_works) {
		uint32_t other = bound == 2 ? 1 : 2;

		binding = other;
		at = mhi_qmi_open(request, 0xdf, 0x00af);
		at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS bind subscription (the other)",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: after the refusals the bind to subscription %u (the other one) says result %u, error %u\n",
		    (unsigned)binding, result, error);
		if (got != 0 && result == 0) {
			bound = (unsigned)binding;
			said = mhi_wds_bind_get(out, in, node, port, 0xdc,
			    answer, answer_size);
			if (said != ~0u) {
				bound = said;
			}
			hakios_tl("pcie2: cellular: IMS: after the other bind the modem says the client is on subscription %u (%s)\n",
			    bound, mhi_wds_subscription_name(bound));
			accepted = mhi_wds_start_try(out, in, node, port, -1,
			    0x04, NULL, 0, 1, 0xed,
			    "IPCR WDS start under the other subscription (reference form)",
			    answer, answer_size, &got);
			if (accepted) {
				control = 1;
				hakios_tl("pcie2: cellular: IMS: THE START UNDER SUBSCRIPTION %u WAS ACCEPTED: the CM ACL refusal was the subscription binding\n",
				    (unsigned)binding);
			}
		}
	}
	if (!accepted) {
		mhi_qmi_outcome(answer, got, &result, &error);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
		if (value != NULL && size >= 2) {
			cause = value[0] | (unsigned)value[1] << 8;
		}
		hakios_tl("pcie2: cellular: IMS: the ims PDN was refused in every form (last result %u, error %u, call end reason %#x)\n",
		    result, error, cause);
		/* Verbose Call End Reason (TLV 0x11): {type u16, reason s16}. */
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x11, &size) : NULL;
		if (value != NULL && size >= 4) {
			unsigned vtype = value[0] | (unsigned)value[1] << 8;
			int vreason = (int16_t)(value[2] |
			    (unsigned)value[3] << 8);

			/* The raw bytes too: reason 1042 (0x0412) is not in
			 * libqmi's QmiWdsVerboseCallEndReason3gpp enum, and the
			 * frame hex dump caps at ~50 bytes, so the bytes of
			 * this 52-byte answer were never seen. */
			hakios_tl("pcie2: cellular: IMS: the verbose call end reason is type %u, reason %d, raw %02x %02x %02x %02x of %zu bytes\n",
			    vtype, vreason, value[0], value[1], value[2], value[3],
			    size);
		}
		/* A refused start can still mean "no effect" -- the session is
		 * already up (autoconnect).  The packet service is the only
		 * answer that can tell. */
		at = mhi_qmi_open(request, 0xcf, 0x0022);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request,
		    (unsigned)at, "IPCR WDS packet service after the refusals",
		    answer, answer_size);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
		status = ~0u;
		if (value != NULL && size >= 1) {
			status = value[0];
		}
		hakios_tl("pcie2: cellular: IMS: after the refusals the packet service says %u (%s)\n",
		    status, mhi_wds_conn_name(status));
		if (status == 2) {
			hakios_tl("pcie2: cellular: IMS: A PACKET SESSION IS CONNECTED ANYWAY; the IMSA poll takes it\n");
			return 1;
		}
		return 0;
	}
	handle = mhi_qmi_u32(answer, got, 0x01);
	if (control) {
		hakios_tl("pcie2: cellular: IMS: THE DATA PROFILE STARTED (packet data handle %u): the data path works, so the ims refusals above are the ims PDN itself\n",
		    handle);
	} else {
		hakios_tl("pcie2: cellular: IMS: THE IMS PDN WAS STARTED (packet data handle %u)\n",
		    handle);
	}

	/* What the started call carries, from the modem itself (Get Current
	 * Settings 0x002d; TLV 0x14 the APN, 0x2b the IP family, 0x1e IPv4). */
	at = mhi_qmi_open(request, 0xcd, 0x002d);
	{
		uint32_t asked = (1u << 2) | (1u << 3) | (1u << 8) | (1u << 9) |
		    (1u << 15);

		at = mhi_qmi_put(request, at, 0x10, &asked, sizeof(asked));
	}
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
	    "IPCR WDS current settings of the started call", answer,
	    answer_size);
	if (got != 0) {
		const uint8_t *apn, *family_tlv, *ipv4;
		unsigned alen = 0, family_value = ~0u;

		apn = mhi_qmi_answer_tlv(answer, got, 0x14, &size);
		if (apn != NULL) {
			alen = (unsigned)size;	/* a plain string */
		}
		family_tlv = mhi_qmi_answer_tlv(answer, got, 0x2b, &size);
		if (family_tlv != NULL && size >= 1) {
			family_value = family_tlv[0];
		}
		ipv4 = mhi_qmi_answer_tlv(answer, got, 0x1e, &size);
		hakios_tl("pcie2: cellular: IMS: the started call carries the APN \"%.*s\", ip family %u (%s)\n",
		    (int)alen, apn != NULL ? (const char *)apn : "",
		    family_value, ipv4 != NULL ?
		    "an ipv4 address is reported" : "no ipv4 address yet");
		/* The full answer, for the P-CSCF (TLV 0x2a) and any PCO
		 * companion: the run log has never shown more than the three
		 * fields above. */
		mhi_qmi_tlvs(answer, got, "IPCR WDS current settings of the started call");
	}

	/* The PDN connects asynchronously; the packet service status names the
	 * moment.  The 60 s budget covers the bearer setup itself. */
	for (unsigned waited = 0; waited < 60000; waited += 10000) {
		at = mhi_qmi_open(request, 0xc2, 0x0022);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1002, node, port, request, (unsigned)at,
		    "IPCR WDS packet service status", answer, answer_size);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x01, &size) : NULL;
		if (value != NULL && size >= 1) {
			connected = value[0];
			if (connected == 2) {
				break;
			}
		}
		delay(10000000);
	}
	hakios_tl("pcie2: cellular: IMS: the packet service says %u (%s)\n",
	    connected, mhi_wds_conn_name(connected));
	return 1;
}

/* IMS service 0x12: Bind (0x98, subscription 1), then Set IMS Services
 * Enabled Setting (0x8f) with Voice over LTE (TLV 0x10), the IMS service
 * (0x18) and IMS SMS (0x1a) on, then Get IMS Services Enabled Setting
 * (0x90) to read the SMS flag back.  Run 121242 sent these to 0x11, which
 * is SAR on this firmware: every message came back error 57 (0x12 is the
 * IMS settings service; libqmi's qmi-service-ims.json). */
/* IMSP service 0x1f: Get Enabler State (0x24; output TLV 0x10, a guint32:
 * 1 uninitialized, 2 initialized, 3 airplane, 4 registered).  This is the
 * modem's own IMS enabler layer, under IMSA, and nothing in this driver has
 * ever asked it; its value before and after the ims PDN tells whether the
 * modem's stack is up at all or still uninitialized behind every error 70. */
static void
mhi_imsp_state(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size)
{
	static const char *const state[] = {
		"unknown", "uninitialized", "initialized", "airplane",
		"registered"
	};
	uint8_t request[16];
	size_t got, at, size;
	const uint8_t *value;
	unsigned node, port, result = ~0u, error = ~0u, which = ~0u;

	if (!mhi_qrtr_service(0x1f, &node, &port)) {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced IMSP; its enabler state is unknown\n");
		return;
	}
	at = mhi_qmi_open(request, 0xc9, 0x0024);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1006, node, port, request, (unsigned)at,
	    "IPCR IMSP enabler state", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	if (value != NULL && size >= 4) {
		which = value[0] | (unsigned)value[1] << 8 |
		    (unsigned)value[2] << 16 | (unsigned)value[3] << 24;
	}
	hakios_tl("pcie2: cellular: IMS: the IMSP enabler state says result %u, error %u; the state is %u (%s)\n",
	    result, error, which, which < sizeof(state) / sizeof(state[0]) ?
	    state[which] : "beyond the known states");
}

/* Get Supported Messages (0x001e) answer TLV 0x10 is a bitmap, not an id
 * list: a 16-bit byte count, then that many bytes, bit N of byte N/8 naming
 * message id N.  Service 0x12's bytes (00 00 00 c0 ... 03 40 e0 f8 ...) hold
 * 0x48/0x49 in byte 9 and 0x8f/0x90/0x98 further on -- run 071041 read the
 * 22 bytes as 22 ids and reported all four absent while the firmware
 * implements them. */
static unsigned
mhi_ims_supported(const uint8_t *value, size_t size, unsigned id)
{
	unsigned bytes;

	if (size < 2) {
		return 0;
	}
	bytes = value[0] | (unsigned)value[1] << 8;
	if (id / 8 >= bytes || 2 + id / 8 >= size) {
		return 0;
	}
	return (value[2 + id / 8] >> (id % 8)) & 1;
}

/* The vendor probe: Android's IMS settings client's 0x4a reaches a service
 * our 0x12 table entry does not.  Run 071041's byte-identical 0x4a to
 * 3:0x49 was refused error 3 (0x48 error 70) while Android's got result 0
 * and state 4 from 3:0x3b on this firmware.  Announcements carry {service,
 * instance, node, port}; the short table keeps only the first port of the
 * whitelisted services, so every surveyed service on the modem's node is
 * asked once -- each from its own client port -- and the pump's hook decodes
 * the answers.
 *
 * Run 074446 answered at the transport level but every answer was dropped:
 * mhi_qmi_open pinned the transaction's high byte to 0, so the probe's
 * 0x1300+n went out as n and the matcher's txn >= 0x1300 test never held.
 * The answers themselves are the finding: 3:0x3b -- Android's own settings
 * peer -- refused the bare 0x4a with result 1, error 0x34, byte for byte
 * what Android's *other* client fd=37 gets from it (imscap2 06:17:33.06),
 * while Android's long-lived fd=60 gets result 0 and a state TLV.  The two
 * differ in that fd=60 was introduced to the service through the name
 * service; a client that only sends at the port is refused.  So each probe
 * client is now registered with a NEW_LOOKUP before its 0x4a, and the
 * winner's client port carries the whole vendor sequence -- a second port
 * would be a second unregistered client. */
#define MHI_IMS_PROBES 64

static struct {
	unsigned service, instance, node, port, client;
} mhi_ims_probe[MHI_IMS_PROBES];
static unsigned mhi_ims_probe_count;
static struct {
	unsigned service, instance, node, port, client, state;
} mhi_ims_winner;
static bool mhi_ims_winner_found;
static bool mhi_ims_scan_done;

/* Set by the 0x3d write below: the vendor routes are in the modem once per
 * boot, and the per-cycle catch-up then stops asking. */
static bool mhi_ims_vendor_done;

/* The address of the modem's IMSA service, learned when it is first used:
 * the indication hook (mhi_imsa_note) hears the registration the getters
 * refuse to give.  0 until then. */
static unsigned mhi_imsa_node, mhi_imsa_port;

/* The last registration state the modem pushed as a 0x23 indication, and
 * whether the 0x22 registration went out this boot. */
static unsigned mhi_imsa_ind_state = ~0u;
static unsigned mhi_imsa_ind_sms = ~0u, mhi_imsa_ind_voice = ~0u;
static bool mhi_imsa_indications;
/* The bind took.  Android binds IMSA once per boot; the post-handoff watch
 * calls mhi_imsa_wait repeatedly, and a client that re-binds on every call
 * may lose the indication registration with the replaced client. */
static bool mhi_imsa_bound;

/* Called for every data frame the pump hears: an answer to one of the
 * probe's requests (same transaction, message 0x4a) is decoded here.  The
 * result says whether the service implements the vendor read; TLV 0x10's
 * first byte carries the state.  The first service that answers result 0
 * with a state wins and is remembered for the configuration sequence. */
static void
mhi_ims_probe_note(const uint8_t *frame, size_t length, const char *what)
{
	const uint8_t *qmi, *value;
	size_t size;
	unsigned header, txn, message, i;
	unsigned result = ~0u, error = ~0u, state = ~0u;

	if (mhi_ims_probe_count == 0) {
		return;
	}
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
	} else if (length >= 16) {
		header = 16;
	} else {
		return;
	}
	if (length < header + 7) {
		return;
	}
	qmi = frame + header;
	if ((qmi[0] & 0x06) != 0x02) {
		return;
	}
	txn = qmi[1] | (unsigned)qmi[2] << 8;
	message = qmi[3] | (unsigned)qmi[4] << 8;
	if (message != 0x004a && message != 0x004c) {
		return;
	}
	/* The transaction is the probe's (0x1300+i for the state read,
	 * 0x1400+i for the bind; mhi_qmi_open keeps the high byte now).  The
	 * probe client's port is not a match by itself any more: the winner
	 * client carries the later vendor sequence too (prelude 0x5c0+,
	 * catch-up 0xce, 0x3d/0x3c 0xcf/0xd0), and those answers are read by
	 * their own calls, not this hook. */
	if (txn >= 0x1300 && txn < 0x1300 + mhi_ims_probe_count) {
		i = txn - 0x1300;
	} else if (txn >= 0x1400 && txn < 0x1400 + mhi_ims_probe_count) {
		i = txn - 0x1400;
	} else {
		return;
	}
	mhi_qmi_outcome(frame, length, &result, &error);
	if (message == 0x004c) {
		/* The bind the vendor state service (the WMS port, service
		 * 0x05) needs before it serves any 0x4a: Android's own client
		 * sends it first (qcrild capture 08:17:49.97), and run
		 * 080138's bare 0x4a to the same service was refused error 52
		 * (DeviceNotReady) on all 44 probed services.  The answer
		 * separates "not this service" (error 57/71) from "bound". */
		hakios_tl("pcie2: AMSS: %s: the vendor bind: service 0x%x (instance %u) at %u:%#x says result %u, error %u\n",
		    what, mhi_ims_probe[i].service, mhi_ims_probe[i].instance,
		    mhi_ims_probe[i].node, mhi_ims_probe[i].port, result, error);
		/* The vendor state service is the DSD settings service, 0x2a
		 * instance 1 (the name service maps it to the port Android's
		 * settings client talks to, imscap2 fd 60 -> 3:0x3b, and qcrild
		 * opens it with 0x27/0x38).  A 0x4a answer from it is preferred
		 * over any winner taken earlier, the WMS bind included.  WMS
		 * (0x05) stays the fallback: it answered Android's own 0x4c bind
		 * in the qcrild capture (3:0x3d there) and, in run 084442, was
		 * the only announced service whose bind was accepted besides DSD
		 * (which has no 0x4a state); its bare 0x4a is refused error 52
		 * (DeviceNotReady) until its set-up runs. */
		if (result == 0 && error == 0 && mhi_ims_probe[i].service == 0x05 &&
		    !mhi_ims_winner_found) {
			mhi_ims_winner_found = true;
			mhi_ims_winner.service = mhi_ims_probe[i].service;
			mhi_ims_winner.instance = mhi_ims_probe[i].instance;
			mhi_ims_winner.node = mhi_ims_probe[i].node;
			mhi_ims_winner.port = mhi_ims_probe[i].port;
			mhi_ims_winner.client = mhi_ims_probe[i].client;
			mhi_ims_winner.state = ~0u;
			hakios_tl("pcie2: cellular: IMS: the vendor state service is WMS (0x5, instance %u) at %u:%#x, client %#x; its 0x4c bind is accepted, the set-up follows\n",
			    mhi_ims_probe[i].instance, mhi_ims_probe[i].node,
			    mhi_ims_probe[i].port, mhi_ims_probe[i].client);
		}
		return;
	}
	value = mhi_qmi_answer_tlv(frame, length, 0x10, &size);
	if (value != NULL && size >= 1) {
		state = value[0];
	}
	hakios_tl("pcie2: AMSS: %s: the vendor probe: service 0x%x (instance %u) at %u:%#x says result %u, error %u, state %u\n",
	    what, mhi_ims_probe[i].service, mhi_ims_probe[i].instance,
	    mhi_ims_probe[i].node, mhi_ims_probe[i].port, result, error, state);
	if (result == 0 && state != ~0u &&
	    (!mhi_ims_winner_found || (mhi_ims_winner.service != 0x2a &&
	    mhi_ims_probe[i].service == 0x2a))) {
		mhi_ims_winner_found = true;
		mhi_ims_winner.service = mhi_ims_probe[i].service;
		mhi_ims_winner.instance = mhi_ims_probe[i].instance;
		mhi_ims_winner.node = mhi_ims_probe[i].node;
		mhi_ims_winner.port = mhi_ims_probe[i].port;
		mhi_ims_winner.client = mhi_ims_probe[i].client;
		mhi_ims_winner.state = state;
		hakios_tl("pcie2: cellular: IMS: the vendor state service is %s (0x%x, instance %u) at %u:%#x, client %#x; its state is %u\n",
		    mhi_qrtr_name(mhi_ims_probe[i].service),
		    mhi_ims_probe[i].service, mhi_ims_probe[i].instance,
		    mhi_ims_probe[i].node, mhi_ims_probe[i].port,
		    mhi_ims_probe[i].client, state);
	}
}

/* The DSD settings service's (0x2a instance 1) own opening, as qcrild sends
 * it to a fresh client: 0x27 with TLV 0x01 = 1 (the client's subscription
 * bind), then 0x38 with the three event-report enables TLV 0x15/0x18/0x1a = 1
 * (qcrild capture 08:17:50.502 -> .526, both answered result 0; the
 * mhi_dsd_settings stream above already replays the same opening from its
 * own clients).  Only a client opened this way gets the profile list (0x24)
 * and the 0x4a state reads; this is also the port Android's IMS settings
 * client does all its vendor traffic on (imscap2 fd 60 -> 3:0x3b, the name
 * service's 0x2a instance-1 entry in qcrild.trace).  Run 093352 bound this
 * service with WMS's 0x4c shape instead and its bare 0x4a was never
 * answered -- the bind result 0 did not make the client one the service
 * serves.  A 0x24 read follows as the identity check: its answer carries
 * the firmware's profiles (ims, SOS, cbnet, CMNET, CMWAP). */
static void
mhi_ims_vendor_register(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned client, unsigned node,
    unsigned port)
{
	static const uint8_t binding[4] = { 0x00, 0x00, 0x00, 0x01 };
	static const uint8_t on = 0x01;
	uint8_t request[32];
	const uint8_t *value;
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u;

	at = mhi_qmi_open(request, 0x1700, 0x0027);
	at = mhi_qmi_put(request, at, 0x01, binding, sizeof(binding));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR DSD settings register", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the 0x2a subscription bind (0x27) at %u:%#x says result %u, error %u\n",
	    node, port, result, error);

	at = mhi_qmi_open(request, 0x1701, 0x0038);
	at = mhi_qmi_put(request, at, 0x15, &on, 1);
	at = mhi_qmi_put(request, at, 0x18, &on, 1);
	at = mhi_qmi_put(request, at, 0x1a, &on, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR DSD settings reports", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the 0x2a event reports (0x38) at %u:%#x say result %u, error %u\n",
	    node, port, result, error);

	at = mhi_qmi_open(request, 0x1702, 0x0024);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR DSD settings profiles", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x11, &size) : NULL;
	hakios_tl("pcie2: cellular: IMS: the 0x2a profile read (0x24) says result %u, error %u, %u bytes of profiles\n",
	    result, error, value != NULL ? (unsigned)size : 0u);
}

/* Ask every announced service on the modem's node for the 0x4a state, then
 * pump while the answers arrive.  Returns 1 with the winner's node and port
 * when a service answered result 0.  Runs once per boot: the answers do not
 * depend on the IMS PDN, and the winner is reused by the second
 * mhi_ims_enable call. */
static int
mhi_ims_vendor_scan(struct mhi_channel *out, struct mhi_channel *in, unsigned modem,
    unsigned *node, unsigned *port)
{
	static uint8_t frame[64];
	uint8_t reply[512];
	uint8_t request[16];
	static const uint8_t off = 0x00;
	size_t at, length;
	unsigned sent = 0;

	mhi_ims_probe_count = 0;
	mhi_ims_winner_found = false;
	for (unsigned i = 0; i < mhi_qrtr_all_count && sent < MHI_IMS_PROBES; i++) {
		unsigned service = mhi_qrtr_all[i].service;
		unsigned s_node = mhi_qrtr_all[i].node;
		unsigned s_port = mhi_qrtr_all[i].port;

		/* Only the modem's own node: the other nodes are other
		 * processors and were never the IMS client's peer.  Service 0
		 * is the name service itself, not a QMI service. */
		if (s_node != modem || service == 0) {
			continue;
		}
		mhi_ims_probe[sent].service = service;
		mhi_ims_probe[sent].instance = mhi_qrtr_all[i].instance;
		mhi_ims_probe[sent].node = s_node;
		mhi_ims_probe[sent].port = s_port;
		mhi_ims_probe[sent].client = 0x1200 + sent;
		sent++;
	}
	mhi_ims_probe_count = sent;
	/* Introduce every probe client through the name service before it
	 * speaks.  Android's fd=60 gets result 0 from 3:0x3b while fd=37's
	 * byte-identical 0x4a -- and run 074446's -- is refused error 0x34;
	 * only the clients the name service has introduced are served.  The
	 * lookup is the frame mhi_qrtr_tftp_lookup uses (cmd 10, header type
	 * 10) with the client port as its source, so the modem's answer and
	 * its own notification to the service name that client. */
	for (unsigned i = 0; i < sent; i++) {
		uint8_t control[20];

		mhi_qrtr_control(control, 10, mhi_ims_probe[i].service);
		mhi_qrtr_put(control, 8, mhi_ims_probe[i].instance);
		length = mhi_qrtr_put3_sized(frame, 10, mhi_ims_probe[i].client,
		    0xffff, 0xfffe, control, sizeof(control),
		    (16u + sizeof(control) + 3u) & ~3u);
		(void)mhi_send(out, frame, length);
	}
	if (sent != 0) {
		delay(200000);
	}
	/* The 0x2a (DSD settings) service gets its own opening instead of the
	 * WMS-shaped 0x4c: qcrild registers with 0x27/0x38 and only then are
	 * its reads served.  Run 093352's 0x4c to this service was accepted
	 * (result 0) and its bare 0x4a was never answered, so the bind shape
	 * is not what opens it; the registration and the 0x24 identity read
	 * run from the probe's own client so the later 0x4a reads come from
	 * the client the service knows. */
	for (unsigned i = 0; i < sent; i++) {
		if (mhi_ims_probe[i].service == 0x2a) {
			mhi_ims_vendor_register(out, in, reply, sizeof(reply),
			    mhi_ims_probe[i].client, mhi_ims_probe[i].node,
			    mhi_ims_probe[i].port);
		}
	}
	/* Each remaining candidate is bound (0x4c, TLV 0x01 = 00) before its
	 * 0x4a: WMS refuses a bare 0x4a with DeviceNotReady.  The per-port
	 * ordering of the ring keeps the bind ahead of the read on every
	 * client.  0x2a is skipped: 0x4c is not one of its messages, and it
	 * was already registered above. */
	for (unsigned i = 0; i < sent; i++) {
		if (mhi_ims_probe[i].service == 0x2a) {
			continue;
		}
		at = mhi_qmi_open(request, 0x1400 + i, 0x004c);
		at = mhi_qmi_put(request, at, 0x01, &off, 1);
		mhi_qmi_seal(request, at);
		length = mhi_qrtr_put3_sized(frame, 1, mhi_ims_probe[i].client,
		    mhi_ims_probe[i].node, mhi_ims_probe[i].port, request,
		    (unsigned)at, (16u + at + 3u) & ~3u);
		(void)mhi_send(out, frame, length);
	}
	if (sent != 0) {
		delay(300000);
	}
	for (unsigned i = 0; i < sent; i++) {
		at = mhi_qmi_open(request, 0x1300 + i, 0x004a);
		mhi_qmi_seal(request, at);
		length = mhi_qrtr_put3_sized(frame, 1, mhi_ims_probe[i].client,
		    mhi_ims_probe[i].node, mhi_ims_probe[i].port, request,
		    (unsigned)at, (16u + at + 3u) & ~3u);
		(void)mhi_send(out, frame, length);
	}
	hakios_tl("pcie2: cellular: IMS: the vendor probe asks %u of the modem's announced services (%u surveyed) for the 0x4a state, each client introduced by a name-service lookup\n",
	    sent, mhi_qrtr_all_count);
	if (sent != 0) {
		(void)mhi_qrtr_pump(out, in, 4000, "IPCR IMS vendor probe");
	}
	mhi_ims_probe_count = 0;
	if (!mhi_ims_winner_found) {
		return 0;
	}
	*node = mhi_ims_winner.node;
	*port = mhi_ims_winner.port;
	return 1;
}

/* The set-up Android's vendor state client sends before its 0x4a means
 * anything (qcrild capture 08:17:52 on this firmware, service 0x05 instance
 * 1): compile (0x5c), Set Routes (0x32) with the capture's table, the
 * generic set (0x01), a configuration set (0x47), the capabilities read
 * (0x4d), compile again and the configuration read (0x48) -- every answer
 * result 0.  Only then does the state climb to 4 by itself, once the ims
 * PDN is up.  At most three of these per boot (before and after the PDN);
 * its requests reuse one transaction sequence and one client. */
static void
mhi_ims_vendor_prelude(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned client, unsigned node,
    unsigned port)
{
	static const uint8_t routes[26] = {
		0x06, 0x00, 0x00, 0x00, 0xff, 0x02, 0x00, 0x01, 0xff, 0x02,
		0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0xff, 0x02, 0x00, 0x04,
		0xff, 0x02, 0x00, 0x05, 0xff, 0x02
	};
	static const uint8_t on = 0x01;
	uint8_t request[64];
	const uint8_t *value;
	size_t got, at, size;
	unsigned result = ~0u, error = ~0u, flag = ~0u;
	static unsigned sent;

	if (sent >= 3) {
		return;
	}
	sent++;
	hakios_tl("pcie2: cellular: IMS: the vendor set-up %u of at most 3 goes to %u:%#x, client %#x\n",
	    sent, node, port, client);

	at = mhi_qmi_open(request, 0x5c0, 0x005c);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor compile", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor compile (0x5c) says result %u, error %u\n",
	    result, error);

	at = mhi_qmi_open(request, 0x5c1, 0x0032);
	at = mhi_qmi_put(request, at, 0x01, routes, sizeof(routes));
	at = mhi_qmi_put(request, at, 0x10, &on, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor routes", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor routes (0x32, the capture's table) say result %u, error %u\n",
	    result, error);

	at = mhi_qmi_open(request, 0x5c2, 0x0001);
	at = mhi_qmi_put(request, at, 0x10, &on, 1);
	at = mhi_qmi_put(request, at, 0x11, &on, 1);
	at = mhi_qmi_put(request, at, 0x12, &on, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor set 1", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor set (0x01) says result %u, error %u\n",
	    result, error);

	at = mhi_qmi_open(request, 0x5c3, 0x0047);
	at = mhi_qmi_put(request, at, 0x10, &on, 1);
	at = mhi_qmi_put(request, at, 0x11, &on, 1);
	at = mhi_qmi_put(request, at, 0x13, &on, 1);
	at = mhi_qmi_put(request, at, 0x15, &on, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor config set", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor config set (0x47) says result %u, error %u\n",
	    result, error);

	at = mhi_qmi_open(request, 0x5c4, 0x004d);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor capabilities", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	{
		char text[80] = "";
		size_t used = 0;

		for (unsigned i = 0; i < 9 && got != 0; i++) {
			value = mhi_qmi_answer_tlv(answer, got, 0x10 + i, &size);
			if (value != NULL && size >= 1 && used < sizeof(text) - 12) {
				used += (size_t)snprintf(text + used,
				    sizeof(text) - used, " %x:%u", 0x10 + i,
				    value[0]);
			}
		}
		hakios_tl("pcie2: cellular: IMS: the vendor capabilities (0x4d) say result %u, error %u;%s\n",
		    result, error, text[0] != 0 ? text : " no capability is given");
	}

	at = mhi_qmi_open(request, 0x5c5, 0x005c);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor compile again", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the second vendor compile (0x5c) says result %u, error %u\n",
	    result, error);

	at = mhi_qmi_open(request, 0x5c6, 0x0048);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor config read", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	if (value != NULL && size >= 1) {
		flag = value[0];
	}
	hakios_tl("pcie2: cellular: IMS: the vendor config read (0x48) says result %u, error %u; its state is %u\n",
	    result, error, flag);
}

/* The IMS vendor state read (0x004a): Android's own client (captures 06:17
 * and 06:22 of this same firmware) sends it with no payload about once a
 * second and reads a one-byte state in TLV 0x10 -- 00 while the modem's
 * IMS stack is still coming up, 04 once it is ready for the configuration
 * sequence, after which its 0x0048 is answered result 0 with TLV 0x10 = 01
 * and TLV 0x11 = 00 01.  HakiOS has never sent 0x4a, and its 0x0048 has
 * only ever been refused InvalidOperation; this read says which state our
 * client is given.  Poll until the state is 4 or the rounds are spent;
 * return the last state (~0u when no answer named one). */
static unsigned
mhi_ims_vendor_state(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned client, unsigned node,
    unsigned port, unsigned rounds)
{
	uint8_t request[16];
	size_t got, at, size;
	const uint8_t *value;
	unsigned state = ~0u, result = ~0u, error = ~0u;

	for (unsigned round = 0; round < rounds; round++) {
		at = mhi_qmi_open(request, 0xca + round, 0x004a);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, client, node, port, request,
		    (unsigned)at, "IPCR IMS vendor state", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		state = ~0u;
		value = got != 0 ?
		    mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
		if (value != NULL && size >= 1) {
			state = value[0];
		}
		hakios_tl("pcie2: cellular: IMS: the vendor state read (0x4a, round %u) says result %u, error %u; the state is %u\n",
		    round, result, error, state);
		if (state == 4) {
			break;
		}
		delay(2000000);
	}
	return state;
}

/* Android's vendor client writes these only once 0x4a answered state 4:
 * 0x3d with TLV 0x01 = 01 and a 37-byte TLV 0x10 whose bytes are the
 * capture's verbatim (a count of 7 and seven five-byte entries, the SMS
 * routes), then 0x3c with TLV 0x01 = 01 01; both answered result 0
 * (imscap2 06:17:34.023 and 06:17:36.336).  Its other 0x3d shape
 * (TLV 0x01 = 00, TLV 0x11, 27 bytes) is refused error 25 and is not
 * replayed.  Once accepted the routes stay for the boot; a second write
 * would only be refused as already configured. */
static int
mhi_ims_vendor_configure(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned client, unsigned node,
    unsigned port)
{
	static const uint8_t services[37] = {
		0x07, 0x00, 0x00, 0x11, 0x02, 0x11, 0x01, 0x03,
		0x11, 0x03, 0x11, 0x00, 0x04, 0x11, 0x04, 0x11,
		0x01, 0x12, 0x11, 0x1b, 0x11, 0x01, 0x1c, 0x11,
		0x1e, 0x11, 0x00, 0x1f, 0x11, 0x28, 0x11, 0x01,
		0x29, 0x11, 0x2b, 0x11, 0x00
	};
	static const uint8_t registrable[2] = { 0x01, 0x01 };
	static const uint8_t on = 0x01;
	uint8_t request[64];
	size_t got, at;
	unsigned result = ~0u, error = ~0u;

	if (mhi_ims_vendor_done) {
		return 1;
	}
	at = mhi_qmi_open(request, 0xcf, 0x003d);
	at = mhi_qmi_put(request, at, 0x01, &on, 1);
	at = mhi_qmi_put(request, at, 0x10, services, sizeof(services));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor services", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor services write (0x3d) says result %u, error %u\n",
	    result, error);
	if (result == 0 && error == 0) {
		mhi_ims_vendor_done = true;
	}

	at = mhi_qmi_open(request, 0xd0, 0x003c);
	at = mhi_qmi_put(request, at, 0x01, registrable, sizeof(registrable));
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, client, node, port, request, (unsigned)at,
	    "IPCR IMS vendor command", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	hakios_tl("pcie2: cellular: IMS: the vendor command (0x3c) says result %u, error %u\n",
	    result, error);
	return mhi_ims_vendor_done;
}

/* One quiet 0x4a read (no per-call trace lines): the state byte, or ~0u. */
static unsigned
mhi_ims_vendor_peek(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size, unsigned client, unsigned node,
    unsigned port)
{
	uint8_t frame[64];
	uint8_t request[16];
	const uint8_t *value;
	size_t got, at, length, size;
	unsigned state = ~0u;

	/* A transaction of its own: mhi_ims_vendor_state's rounds use 0xca..
	 * 0xce and a stray answer to the same client would be taken by either
	 * caller otherwise. */
	at = mhi_qmi_open(request, 0xd4, 0x004a);
	mhi_qmi_seal(request, at);
	length = mhi_qrtr_put3_sized(frame, 1, client, node, port, request,
	    (unsigned)at, (16u + at + 3u) & ~3u);
	if (mhi_send(out, frame, length) != 0) {
		return ~0u;
	}
	got = mhi_qrtr_answer(out, in, answer, answer_size, client, 0xd4, 0x004a,
	    5000, 2, "IPCR IMS vendor state");
	if (got == 0) {
		return ~0u;
	}
	value = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
	if (value != NULL && size >= 1) {
		state = value[0];
	}
	return state;
}

/* Called once per cellular cycle while the vendor service has not taken the
 * routes: Android's state reaches 4 tens of seconds after the ims PDN
 * (imscap2: 00 -> 04), longer than one enable call waits, so the last step
 * (0x3d/0x3c) would otherwise never run.  One look per cycle for the first
 * fifteen, then one in ten; a line only when the state changes.  The look
 * follows the endpoint that answered last (the probe's winner is usually
 * 0x2a, the service Android's settings client reads this state from); the
 * IMS stack's own client port is asked every fifth look, and before
 * anything has answered the winner is preferred and the stack port is the
 * every-fifth alternative. */
static void
mhi_ims_vendor_catchup(struct mhi_channel *out, struct mhi_channel *in)
{
	uint8_t answer[512];
	unsigned client, node, port, state;
	static unsigned polls;
	static unsigned last = ~0u;
	/* Run 055427: the stack's own port (3:0x3b) never answered a 0x4a,
	 * while the announced entry (3:0x39) answered state 1; the catch-up
	 * followed the silent port and could never see a move to 4.  The
	 * endpoint that last answered is followed now; the stack's port
	 * still gets every fifth look, so a late transition there is not
	 * missed. */
	static unsigned answered_client, answered_node, answered_port;
	int stack_known = mhi_imsdcm_port != 0;
	int stack_peer = stack_known && answered_node == mhi_imsdcm_node &&
	    answered_port == mhi_imsdcm_port;
	int other = polls > 0 && polls % 5 == 0;
	int pick;

	if (mhi_ims_vendor_done) {
		return;
	}
	/* The endpoint that answered last is the primary; every fifth look
	 * asks the other one (the IMS stack's own port and the probe's
	 * winner), so a state that moves only there is not missed.  Before
	 * anything has answered, the winner is preferred and the stack gets
	 * every fifth look: run 093352's 3:0x3b never answered a 0x4a, and
	 * the older shape asked it on every look while answered_port was
	 * still 0, so the endpoint that does answer was never polled. */
	if (stack_peer) {
		pick = (other && mhi_ims_winner_found &&
		    !(mhi_ims_winner.node == mhi_imsdcm_node &&
		      mhi_ims_winner.port == mhi_imsdcm_port)) ? 2 : 0;
	} else if (answered_port != 0) {
		pick = (other && stack_known) ? 0 : 1;
	} else if (mhi_ims_winner_found) {
		pick = (other && stack_known) ? 0 : 2;
	} else if (stack_known) {
		pick = 0;
	} else {
		return;
	}
	if (pick == 0) {
		client = mhi_imsdcm_from;
		node = mhi_imsdcm_node;
		port = mhi_imsdcm_port;
	} else if (pick == 1) {
		client = answered_client;
		node = answered_node;
		port = answered_port;
	} else {
		client = mhi_ims_winner.client;
		node = mhi_ims_winner.node;
		port = mhi_ims_winner.port;
	}
	if (polls >= 15 && polls % 10 != 0) {
		polls++;
		return;
	}
	polls++;
	state = mhi_ims_vendor_peek(out, in, answer, sizeof(answer), client,
	    node, port);
	if (state != ~0u) {
		answered_client = client;
		answered_node = node;
		answered_port = port;
	}
	if (state != last) {
		hakios_tl("pcie2: cellular: IMS: the vendor state is %u at look %u (%u:%#x)\n",
		    state, polls, node, port);
		last = state;
	}
	if (state == 4) {
		(void)mhi_ims_vendor_configure(out, in, answer, sizeof(answer),
		    client, node, port);
	}
}

static unsigned
mhi_ims_enable(struct mhi_channel *out, struct mhi_channel *in, uint8_t *answer,
    size_t answer_size)
{
	uint8_t request[64];
	const uint8_t *value;
	size_t got, at, size;
	unsigned node, port, vendor_node, vendor_port, vendor_client;
	unsigned result = ~0u, error = ~0u, vendor_state = ~0u;
	/* Android binds 0x0098 with TLV 0x01 = 0 (qcrild.trace 08:17:49 on
	 * 3:0x4b, payload 01 04 00 00 00 00 00); 092358 sent 1. */
	uint32_t binding = 0;
	uint8_t on = 0x01;
	/* Whether the vendor sequence follows the IMS stack's own port
	 * instead of the announced 0x12 entry or the probe's winner. */
	int on_stack = 0;
	static unsigned bound;

	if (!mhi_qrtr_service(0x12, &node, &port)) {
		/* Run 124850 reached this before the modem had announced 0x12;
		 * the announcement came only later and the settings were never
		 * retried.  The name service's hello makes the modem restate
		 * its table (net/qrtr/ns.c), so ask and look again for a few
		 * seconds instead of giving up. */
		hakios_tl("pcie2: cellular: IMS: the IMS settings service is not known yet; the name service is asked and looked for again\n");
		for (unsigned waited = 0; waited < 12000000; waited += 3000000) {
			(void)mhi_send(out, mhi_qrtr_hello,
			    sizeof(mhi_qrtr_hello));
			/* Draining, not sleeping: the answer to the hello is
			 * the modem's whole service table, and only a drain
			 * hands it to mhi_qrtr_remember.  Run 064118's 12 s of
			 * delay() read nothing and learned nothing. */
			(void)mhi_qrtr_pump(out, in, 3000,
			    "IPCR IMS nameservice");
			if (mhi_qrtr_service(0x12, &node, &port)) {
				break;
			}
		}
	}
	if (!mhi_qrtr_service(0x12, &node, &port)) {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced the IMS settings service; its settings are skipped\n");
		return 0;
	}
	/* The vendor configuration messages (0x48, 0x4a, 0x3c, 0x3d) may
	 * live at another port than the service-0x12 table entry; the probe
	 * below finds that port and these follow it.  The libqmi IMS
	 * messages (0x1e/0x8f/0x90/0x98) stay on the 0x12 entry. */
	vendor_node = node;
	vendor_port = port;
	vendor_client = 0x1003;
	/* One bind per boot: the settings are sent twice in a run (before
	 * and after the IMS PDN, see the call sites), a second bind on the
	 * same client would only be refused as already bound. */
	if (!bound) {
		at = mhi_qmi_open(request, 0xc3, 0x0098);
		at = mhi_qmi_put(request, at, 0x01, &binding, sizeof(binding));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1003, node, port, request, (unsigned)at,
		    "IPCR IMS bind", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the IMS bind says result %u, error %u\n",
		    result, error);
		if (result == 0 && error == 0) {
			bound = 1;
		}
	}

	/* Which of the messages this firmware implements: Android's client
	 * polls 0x0048 here and is answered result 0 while HakiOS is refused
	 * InvalidOperation, and libqmi's qmi-service-ims.json carries only
	 * the Get 0x0090 and the Bind.  Get Supported Messages (0x001e) was
	 * refused by WDS with error 71 (invalid QMI command); this probe
	 * says whether the IMS service is different. */
	at = mhi_qmi_open(request, 0xbe, 0x001e);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1003, node, port, request, (unsigned)at,
	    "IPCR IMS supported messages", answer, answer_size);
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
	if (value != NULL && size >= 2) {
		unsigned bytes = value[0] | (unsigned)value[1] << 8;
		unsigned covered = size - 2 < bytes ? (unsigned)(size - 2) : bytes;

		hakios_tl("pcie2: cellular: IMS: the IMS service's supported bitmap is %u bytes (%u message ids): 0x3c %u, 0x3d %u, 0x48 %u, 0x4a %u, 0x8f %u, 0x90 %u, 0x98 %u\n",
		    bytes, bytes * 8, mhi_ims_supported(value, size, 0x3c),
		    mhi_ims_supported(value, size, 0x3d),
		    mhi_ims_supported(value, size, 0x48),
		    mhi_ims_supported(value, size, 0x4a),
		    mhi_ims_supported(value, size, 0x8f),
		    mhi_ims_supported(value, size, 0x90),
		    mhi_ims_supported(value, size, 0x98));
		/* The console cuts a log line at about 190 characters, so the
		 * bitmap bytes go eleven to a line. */
		for (unsigned half = 0; half < 2; half++) {
			char text[3 * 11 + 1] = "";
			size_t used = 0;

			for (unsigned i = half * 11;
			    i < covered && i < half * 11 + 11; i++) {
				used += (size_t)snprintf(text + used,
				    sizeof(text) - used, "%02x ", value[2 + i]);
			}
			hakios_tl("pcie2: cellular: IMS: the supported bitmap, bytes %u-%u: %s\n",
			    half * 11, half * 11 + 10, text);
		}
	} else {
		hakios_tl("pcie2: cellular: IMS: the IMS service did not report its supported messages\n");
	}

	/* The six-message "qcril set-up" (0x54/0x01/0x03/0x20/0x5e/0x69) is
	 * gone: run 093352 sent it three times to the vendor port and to the
	 * stack's own port, every attempt answered nothing there (six 5 s
	 * timeouts each, about 2200 log lines), and the trace's client sends
	 * the same six to DMS as its own start-up -- they were never IMS
	 * traffic.  The set-up that matters for the vendor service is
	 * mhi_ims_vendor_prelude (0x3d/0x3c) below. */

	/* Android's client reads the vendor state (0x4a) first and only then
	 * reads the policy (0x48) and writes the service configuration; its
	 * 0x48 is answered result 0 right after the state turns 4 (capture
	 * 06:17:33.955 -> 06:17:33.970).  HakiOS's 0x4a to the 0x12 entry is
	 * refused (run 071041: error 3), so when it is, the probe asks every
	 * announced service and the rest of the vendor sequence follows the
	 * one that answers.  One round here: a refusal is not a state that
	 * polling would change. */
	vendor_state = mhi_ims_vendor_state(out, in, answer, answer_size,
	    vendor_client, node, port, 1);
	if (vendor_state != 4 && !mhi_ims_scan_done) {
		unsigned wnode, wport;

		mhi_ims_scan_done = true;
		if (mhi_ims_vendor_scan(out, in, node, &wnode, &wport)) {
			vendor_node = wnode;
			vendor_port = wport;
			/* The probe's own client: it is the one the name
			 * service introduced to the vendor service. */
			vendor_client = mhi_ims_winner.client;
			vendor_state = mhi_ims_vendor_state(out, in, answer,
			    answer_size, vendor_client, vendor_node, vendor_port,
			    5);
		}
	} else if (vendor_state != 4 && mhi_ims_winner_found) {
		/* A later call: the probe already named the service. */
		vendor_node = mhi_ims_winner.node;
		vendor_port = mhi_ims_winner.port;
		vendor_client = mhi_ims_winner.client;
		vendor_state = mhi_ims_vendor_state(out, in, answer,
		    answer_size, vendor_client, vendor_node, vendor_port, 5);
	}

	/* The IMS stack's own port -- the peer the stack's 0x21 bind came
	 * from (mhi_imsdcm) -- is where Android's settings client does all
	 * its vendor traffic: imscap2 fd 60 polls 0x4a there, reads state 4
	 * about 1.5 s after the address handoff, then reads 0x48 and writes
	 * 0x3d/0x3c.  Run 052215's probe asked only the announced services
	 * (WMS answered its transport word, never state 4) while the stack's
	 * port, 3:0x39, sat known from the bind.  When the stack has bound,
	 * the vendor sequence follows its port; the client is the one the
	 * stack itself addressed (mhi_imsdcm_from).  The stack is given the
	 * scan's own few seconds first, which is what usually lets it bind. */
	if (mhi_imsdcm_port != 0) {
		unsigned stack_state;

		stack_state = mhi_ims_vendor_state(out, in, answer, answer_size,
		    mhi_imsdcm_from, mhi_imsdcm_node, mhi_imsdcm_port, 5);
		if (stack_state != ~0u) {
			on_stack = 1;
			vendor_client = mhi_imsdcm_from;
			vendor_node = mhi_imsdcm_node;
			vendor_port = mhi_imsdcm_port;
			vendor_state = stack_state;
		} else {
			hakios_tl("pcie2: cellular: IMS: the stack's own port %u:%#x named no 0x4a state; the vendor sequence stays on %u:%#x\n",
			    mhi_imsdcm_node, mhi_imsdcm_port, vendor_node,
			    vendor_port);
		}
	}

	/* Android's WMS client runs its set-up sequence (imscap2 06:17:33,
	 * service 0x05) before its 0x4a can turn 4; nothing else does, and a
	 * service that was never set up answers the state read with a state
	 * that never moves.  Replayed at most three times a boot, then the
	 * state is read again.  Only for WMS: the messages are WMS's, the
	 * IMSDCM service's own message ids differ (its 0x47 is a profile
	 * write, not WMS's configuration message), and the stack's own port
	 * needs no set-up at all -- on Android nothing host-side writes to it
	 * before its state turns 4 by itself. */
	if (vendor_state != 4 && !on_stack && mhi_ims_winner_found &&
	    mhi_ims_winner.service == 0x05) {
		mhi_ims_vendor_prelude(out, in, answer, answer_size, vendor_client,
		    vendor_node, vendor_port);
		vendor_state = mhi_ims_vendor_state(out, in, answer, answer_size,
		    vendor_client, vendor_node, vendor_port, 2);
	}

	/* Get IMS Policy Manager Settings (0x0048) before setting anything.
	 * Run 065846 reached the IMS service for the first time (bind result
	 * 0) and both the Set and the Get of the enabled services answered
	 * error 70 (InvalidOperation) -- the shape of a modem whose IMS
	 * policy is not configured, and that policy names the service mask
	 * (TLV 0x18) the IMS stack is allowed to run.  This read says which
	 * priority source and mask the firmware carries, so the next boot can
	 * either set the policy (0x0047) or look elsewhere. */
	if (!on_stack || vendor_state == 4) {
		at = mhi_qmi_open(request, 0xbf, 0x0048);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, vendor_client, vendor_node, vendor_port,
		    request, (unsigned)at, "IPCR IMS policy read", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x18, &size) : NULL;
		if (value != NULL && size >= 8) {
			uint32_t mask[2];

			memcpy(mask, value, sizeof(mask));
			hakios_tl("pcie2: cellular: IMS: the policy read says result %u, error %u; its service mask is 0x%x%08x\n",
			    result, error, mask[1], mask[0]);
		} else {
			hakios_tl("pcie2: cellular: IMS: the policy read says result %u, error %u; no service mask is given\n",
			    result, error);
		}
		/* Android's 0x48 answer carries TLV 0x10 = 01 and TLV 0x11 = 00 01;
		 * this read says whether ours carries them once the state is 4. */
		{
			const uint8_t *flag;
			unsigned flag10 = ~0u, flag11 = ~0u;

			flag = got != 0 ?
			    mhi_qmi_answer_tlv(answer, got, 0x10, &size) : NULL;
			if (flag != NULL && size >= 1) {
				flag10 = flag[0];
			}
			flag = got != 0 ?
			    mhi_qmi_answer_tlv(answer, got, 0x11, &size) : NULL;
			if (flag != NULL && size >= 2) {
				flag11 = flag[0] | (unsigned)flag[1] << 8;
			}
			hakios_tl("pcie2: cellular: IMS: the policy read's vendor flags: 0x10 %u, 0x11 %u\n",
			    flag10, flag11);
		}
	} else {
		hakios_tl("pcie2: cellular: IMS: the policy read waits for the stack's own state 4 (now %u): Android reads 0x48 only after it\n",
		    vendor_state);
	}

	/* Android's vendor client writes these only once 0x4a answered
	 * state 4: 0x3d with TLV 0x01 = 01 and a 37-byte TLV 0x10 whose
	 * bytes are the capture's verbatim (a count of 7 and seven five-byte
	 * entries), then 0x3c with TLV 0x01 = 01 01; both answered result 0
	 * (06:17:34.023 and 06:17:36.336).  Its other 0x3d shape
	 * (TLV 0x01 = 00, TLV 0x11, 27 bytes) is refused error 25 and is
	 * not replayed. */
	if (vendor_state == 4) {
		(void)mhi_ims_vendor_configure(out, in, answer, answer_size,
		    vendor_client, vendor_node, vendor_port);
	} else {
		hakios_tl("pcie2: cellular: IMS: the vendor configuration is withheld: the 0x4a state is %u, not 4 (asked %u:%#x)\n",
		    vendor_state, vendor_node, vendor_port);
	}

	/* The enable the stock IMS client actually sends (imscap2 06:17:31,
	 * service 0x12 at 3:0x4b): two vendor config reads (0x1f, config ids
	 * 0x8f and 0xa2), then 0x2a with four one-byte flags 0x11, 0x1f,
	 * 0x2d, 0x31 -- all answered result 0, where the libqmi Set 0x8f
	 * below has only ever been refused error 70. */
	{
		static const uint8_t id8f[2] = { 0x8f, 0x00 };
		static const uint8_t ida2[2] = { 0xa2, 0x00 };

		at = mhi_qmi_open(request, 0xd1, 0x001f);
		at = mhi_qmi_put(request, at, 0x01, id8f, sizeof(id8f));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1003, node, port, request,
		    (unsigned)at, "IPCR IMS vendor config read 0x8f", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the vendor config read (0x1f, id 0x8f) says result %u, error %u\n",
		    result, error);

		at = mhi_qmi_open(request, 0xd2, 0x001f);
		at = mhi_qmi_put(request, at, 0x01, ida2, sizeof(ida2));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1003, node, port, request,
		    (unsigned)at, "IPCR IMS vendor config read 0xa2", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the vendor config read (0x1f, id 0xa2) says result %u, error %u\n",
		    result, error);

		at = mhi_qmi_open(request, 0xd3, 0x002a);
		at = mhi_qmi_put(request, at, 0x11, &on, 1);
		at = mhi_qmi_put(request, at, 0x1f, &on, 1);
		at = mhi_qmi_put(request, at, 0x2d, &on, 1);
		at = mhi_qmi_put(request, at, 0x31, &on, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1003, node, port, request,
		    (unsigned)at, "IPCR IMS vendor set config", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: THE VENDOR SET CONFIG (0x2a) SAYS result %u, error %u\n",
		    result, error);
	}

	/* The Set's input is taken to mirror the Get's output field ids
	 * (0x11 voice, 0x12 video telephony, 0x15 voice over Wi-Fi, 0x18
	 * IMS registration, 0x1a SMS, 0x1c USSD); the earlier single 0x10
	 * ("VoLTE") is no field this firmware names.  Run 063523's Set was
	 * refused InvalidOperation while it sat before the IMS PDN -- this
	 * shape goes before and after the PDN (see the call sites).  When
	 * it is refused too, the old three-flag shape goes once more, so
	 * one run tells the two apart. */
	{
		uint8_t off = 0x00;

		at = mhi_qmi_open(request, 0xc4, 0x008f);
		at = mhi_qmi_put(request, at, 0x11, &on, 1);
		at = mhi_qmi_put(request, at, 0x12, &off, 1);
		at = mhi_qmi_put(request, at, 0x15, &off, 1);
		at = mhi_qmi_put(request, at, 0x18, &on, 1);
		at = mhi_qmi_put(request, at, 0x1a, &on, 1);
		at = mhi_qmi_put(request, at, 0x1c, &off, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1003, node, port, request,
		    (unsigned)at, "IPCR DSD settings on", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: THE SET WITH THE GET'S FIELD IDS SAYS result %u, error %u\n",
		    result, error);
		if (result != 0 || error != 0) {
			at = mhi_qmi_open(request, 0xc8, 0x008f);
			at = mhi_qmi_put(request, at, 0x10, &on, 1);
			at = mhi_qmi_put(request, at, 0x18, &on, 1);
			at = mhi_qmi_put(request, at, 0x1a, &on, 1);
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, 0x1003, node, port,
			    request, (unsigned)at, "IPCR DSD settings on old shape",
			    answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: IMS: THE OLD THREE-FLAG SHAPE SAYS result %u, error %u\n",
			    result, error);
		}
	}

	at = mhi_qmi_open(request, 0xc5, 0x0090);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1003, node, port, request, (unsigned)at,
	    "IPCR DSD settings read back", answer, answer_size);
	/* libqmi 1.36's qmi-service-ims.json for 0x0090: 0x11 voice, 0x12
	 * video telephony, 0x15 voice over Wi-Fi, 0x18 registration, 0x19
	 * UT, 0x1a SMS, 0x1c USSD.  An earlier read of 0x1b was a guess at
	 * the SMS flag and no such field exists. */
	value = got != 0 ? mhi_qmi_answer_tlv(answer, got, 0x1a, &size) : NULL;
	hakios_tl("pcie2: cellular: IMS: the IMS SMS setting reads back %s\n",
	    value != NULL && size >= 1 ? (value[0] ? "on" : "off") : "not given");
	/* One enabler-state sample per call: this function runs before and
	 * after the ims PDN, so the two lines say whether the PDN moves it. */
	mhi_imsp_state(out, in, answer, answer_size);
	return 1;
}

/* The IMSA indications, as imscap2's Android capture of the same modem
 * decodes them: message 0x23 Registration Status Changed (TLV 0x01 a u8
 * flag, 0x10 the error u16, 0x11 the registration state u32, 0x13 the
 * technology u32, 0x14 the URIs: 02 <len> "sip:..." 12 <len> "tel:...")
 * and 0x24 Services Status Changed (TLV 0x10 SMS, 0x11 voice).  Android
 * does not read these fields through a getter -- its 0x20/0x21 are
 * refused error 70 as ours are -- the modem pushes them, and that push is
 * how its IMS stack hears the registration (state 2 about a second after
 * the IMSDCM address handoff).  The frame's source port is IMSA's.
 *
 * The type was 0x04, an indication: run 085605 filtered with
 * (qmi[0] & 0x06) != 0x02 and dropped every one of them, so the run saw
 * no indication at all. */
static void
mhi_imsa_note(const uint8_t *frame, size_t length, const char *what)
{
	const uint8_t *qmi, *value;
	size_t size;
	unsigned header, source, message, state, reg_error, tech, sms, voice;

	if (mhi_imsa_port == 0) {
		return;
	}
	if (length >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		source = mhi_qrtr_word(frame, 12);
	} else if (length >= 16) {
		header = 16;
		source = frame[10] | (unsigned)frame[11] << 8;
	} else {
		return;
	}
	if (source != mhi_imsa_port || length < header + 7) {
		return;
	}
	qmi = frame + header;
	if ((qmi[0] & 0x06) != 0x04) {
		return;		/* an answer, which the caller reads */
	}
	message = qmi[3] | (unsigned)qmi[4] << 8;
	if (message == 0x0023) {
		state = mhi_qmi_u32(frame, length, 0x11);
		tech = mhi_qmi_u32(frame, length, 0x13);
		reg_error = mhi_qmi_u16(frame, length, 0x10);
		mhi_imsa_ind_state = state;
		hakios_tl("pcie2: cellular: IMS: the modem's own IMSA indication (%s): the registration is %u, technology %u, error %u\n",
		    what, state, tech, reg_error);
		value = mhi_qmi_answer_tlv(frame, length, 0x14, &size);
		while (value != NULL && size >= 2) {
			/* 02 <len> sip-uri, 12 <len> tel-uri */
			unsigned tag = value[0], text = value[1];

			if (size < 2 + (size_t)text) {
				break;
			}
			hakios_tl("pcie2: cellular: IMS: the IMSA indication's URI %u is \"%.*s\"\n",
			    tag, (int)text, (const char *)value + 2);
			value += 2 + text;
			size -= 2 + text;
		}
		/* libqmi 1.38's IMSA IDL (qmi-service-imsa.json) makes TLV 0x12
		 * the registration error's message string.  No run has caught
		 * this firmware sending it yet; log it when it does. */
		value = mhi_qmi_answer_tlv(frame, length, 0x12, &size);
		if (value != NULL && size > 0) {
			char text[48];
			size_t n = size > sizeof(text) - 1 ? sizeof(text) - 1 : size;

			for (size_t i = 0; i < n; i++) {
				unsigned char c = value[i];

				text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
			}
			text[n] = 0;
			hakios_tl("pcie2: cellular: IMS: the IMSA indication's error message is \"%s\"\n",
			    text);
		}
		/* Error 808 (0x0328) has no public meaning: dump every TLV of
		 * the failed indication so the next run's packet is readable
		 * without guessing which ids this vendor firmware added. */
		if (reg_error != 0 && length > header + 7) {
			size_t shown = length - header;

			if (shown > 64) {
				shown = 64;
			}
			for (size_t dump = 0; dump < shown; dump += 32) {
				char hex[3 * 32 + 1];
				size_t n = shown - dump > 32 ? 32 : shown - dump;

				for (size_t i = 0; i < n; i++) {
					snprintf(hex + i * 3, 4, " %02x",
					    qmi[dump + i]);
				}
				hex[n * 3] = 0;
				hakios_tl("pcie2: cellular: IMS: the failed indication's bytes at %zu:%s\n",
				    dump, hex);
			}
		}
	} else if (message == 0x0024) {
		/* This modem's services indication carries the SMS word in
		 * TLV 0x16 and voice in 0x17 (run 123850's packet on the wire:
		 * 16[4]=2, 17[4]=1, exactly Android's 09:16.793 one); the
		 * 0x10/0x11 pair appears only in Android's later 0x24.  Read
		 * both shapes, as mhi_imsa_wait's 0x21 answer already does. */
		sms = mhi_qmi_u32(frame, length, 0x16);
		voice = mhi_qmi_u32(frame, length, 0x17);
		if (sms == ~0u) {
			sms = mhi_qmi_u32(frame, length, 0x10);
		}
		if (voice == ~0u) {
			voice = mhi_qmi_u32(frame, length, 0x11);
		}
		mhi_imsa_ind_sms = sms;
		mhi_imsa_ind_voice = voice;
		hakios_tl("pcie2: cellular: IMS: the modem's own IMSA services indication (%s): SMS %u, voice %u\n",
		    what, sms, voice);
	} else {
		/* Android's capture (imscap2 06:17:21.620 and 06:17:32.578, port
		 * 0x49, type 4) has the modem sending 0x26 right after the IMS
		 * data service's ready and address indications -- the stack's
		 * echo of them, TLV 0x01 the flag (0, then 1).  This driver
		 * reads 0x23 and 0x24 only, so every other id is logged here:
		 * one run then says whether this firmware pushes 0x26 too.
		 * The payload is dumped as well: run 140914's two 28-byte 0x39
		 * indications (14:20:05, TLV 0x01 = 0 then 1) were the only
		 * traffic between the instance's destroy and the re-ask, and
		 * their bytes were never written to the log. */
		hakios_tl("pcie2: cellular: IMS: the modem's own IMSA indication (%s): message %#x is one this driver does not read (%zu bytes)\n",
		    what, message, length);
		if (length > header + 7) {
			size_t shown = length - header;

			if (shown > 64) {
				shown = 64;
			}
			for (size_t dump = 0; dump < shown; dump += 32) {
				char hex[3 * 32 + 1];
				size_t n = shown - dump > 32 ? 32 : shown - dump;

				for (size_t i = 0; i < n; i++) {
					snprintf(hex + i * 3, 4, " %02x",
					    qmi[dump + i]);
				}
				hex[n * 3] = 0;
				hakios_tl("pcie2: cellular: IMS: the unread message's bytes at %zu:%s\n",
				    dump, hex);
			}
		}
	}
}

/* IMSA service 0x21: Bind (0x33), then Get IMS Registration Status (0x20:
 * TLV 0x12 the state, 0x14 the technology) and Get IMS Services Status
 * (0x21: TLV 0x10 the SMS service, 0x11 voice, 0x13 the SMS technology)
 * every 15 s until the registration is registered (2) and SMS available
 * (2), or the given rounds are spent (6 = 90 s).  Returns the registration
 * state (~0u when the service or an answer is missing); *sms_service takes
 * the last SMS service value (~0u when not given). */
static unsigned
mhi_imsa_wait(struct mhi_channel *out, struct mhi_channel *in, uint8_t *answer,
    size_t answer_size, unsigned rounds, unsigned *sms_service)
{
	static const char *const registration[] = {
		"not registered", "registering", "registered",
		"limited registration"
	};
	static const char *const service[] = {
		"unavailable", "limited", "available"
	};
	/* 64, not 32: the 0x22 indication registration carries eight one-byte
	 * TLVs and the header, 7 + 8 * 4 = 39 bytes; run 084442's 32-byte
	 * buffer put the eighth TLV past its end and fortified memcpy
	 * panicked in this function. */
	uint8_t request[64];
	size_t got, at, size;
	const uint8_t *value;
	unsigned node, port, result = ~0u, error = ~0u, state = ~0u, tech = ~0u;
	unsigned sms = ~0u, voice = ~0u, reg_error = ~0u;
	/* Android binds IMSA with TLV 0x10 = 0 (qcrild.trace 08:17:49 on
	 * 3:0x49, payload 10 04 00 00 00 00 00); 092358 bound with 1 and
	 * every IMSA getter after it came back refused error 70. */
	uint32_t binding = 0;

	if (!mhi_qrtr_service(0x21, &node, &port)) {
		hakios_tl("pcie2: cellular: IMS: the modem has not announced IMSA; the registration is unknown\n");
		return ~0u;
	}
	if (!mhi_imsa_bound || node != mhi_imsa_node || port != mhi_imsa_port) {
		at = mhi_qmi_open(request, 0xc6, 0x0033);
		at = mhi_qmi_put(request, at, 0x10, &binding, sizeof(binding));
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1004, node, port, request,
		    (unsigned)at, "IPCR IMSA bind", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the IMSA bind says result %u, error %u\n",
		    result, error);
		if (result == 0 && error == 0) {
			mhi_imsa_bound = true;
		}
	}

	/* The getters below are refused error 70 -- in Android too -- and the
	 * registration word is the modem's own indications; Android's client
	 * registers them with 0x22 (imscap2 06:17:30): eight one-byte enable
	 * flags, then TLV 0x18.  Once per boot; the note hook reads them. */
	if (!mhi_imsa_indications) {
		static const uint8_t enable[8] = {
			0x10, 0x11, 0x12, 0x13, 0x19, 0x1b, 0x1e, 0x1f
		};
		static const uint8_t on = 0x01;

		mhi_imsa_node = node;
		mhi_imsa_port = port;
		at = mhi_qmi_open(request, 0xd4, 0x0022);
		for (unsigned i = 0; i < sizeof(enable); i++) {
			at = mhi_qmi_put(request, at, enable[i], &on, 1);
		}
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1004, node, port, request,
		    (unsigned)at, "IPCR IMSA register indications", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the IMSA indication registration says result %u, error %u\n",
		    result, error);
		at = mhi_qmi_open(request, 0xd5, 0x0022);
		at = mhi_qmi_put(request, at, 0x18, &on, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1004, node, port, request,
		    (unsigned)at, "IPCR IMSA register the 0x18 indication",
		    answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: IMS: the second IMSA registration (0x18) says result %u, error %u\n",
		    result, error);
		if (result == 0 && error == 0) {
			mhi_imsa_indications = true;
		}
	}

	for (unsigned round = 0; round < rounds; round++) {
		state = ~0u;
		tech = ~0u;
		sms = ~0u;
		voice = ~0u;
		at = mhi_qmi_open(request, 0xc7 + round, 0x0020);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1004, node, port, request, (unsigned)at,
		    "IPCR IMSA registration status", answer, answer_size);
		reg_error = ~0u;
		if (got != 0) {
			state = mhi_qmi_u32(answer, got, 0x12);
			tech = mhi_qmi_u32(answer, got, 0x14);
			reg_error = mhi_qmi_u16(answer, got, 0x11);
			/* The registration error message (TLV 0x13, a string)
			 * says what a non-registered state is waiting on. */
			value = mhi_qmi_answer_tlv(answer, got, 0x13, &size);
			if (value != NULL && size > 0) {
				hakios_tl("pcie2: cellular: IMS: the registration error message is \"%.*s\"\n",
				    (int)(size > 80 ? 80 : size), (const char *)value);
			}
		}

		at = mhi_qmi_open(request, 0xd7 + round, 0x0021);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1004, node, port, request, (unsigned)at,
		    "IPCR IMSA services status", answer, answer_size);
		if (got != 0) {
			/* Android's 0x21 answer carries the SMS word in TLV 0x16
			 * and voice in 0x17 (qcrild.trace: 16[4]=2, 17[4]=1);
			 * 0x10/0x11, read here through run 120804, hold nothing
			 * and the log always said "not given". */
			sms = mhi_qmi_u32(answer, got, 0x16);
			voice = mhi_qmi_u32(answer, got, 0x17);
			if (sms == ~0u) {
				sms = mhi_qmi_u32(answer, got, 0x10);
			}
			if (voice == ~0u) {
				voice = mhi_qmi_u32(answer, got, 0x11);
			}
		}
		/* The getters are refused while the modem brings the stack up;
		 * the indications may already have said. */
		if (state == ~0u && mhi_imsa_ind_state != ~0u) {
			state = mhi_imsa_ind_state;
		}
		if (sms == ~0u && mhi_imsa_ind_sms != ~0u) {
			sms = mhi_imsa_ind_sms;
		}
		if (voice == ~0u && mhi_imsa_ind_voice != ~0u) {
			voice = mhi_imsa_ind_voice;
		}
		hakios_tl("pcie2: cellular: IMS: the registration is %u (%s), SMS %u (%s), voice %u (%s), technology %u, error %u\n",
		    state, state < 4 ? registration[state] : "not given",
		    sms, sms < 3 ? service[sms] : "not given",
		    voice, voice < 3 ? service[voice] : "not given", tech,
		    reg_error);
		if (state == 2 && sms == 2) {
			hakios_tl("pcie2: cellular: IMS: THE IMS IS REGISTERED AND ITS SMS SERVICE IS AVAILABLE\n");
			break;
		}
		delay(15000000);
	}
	if (sms_service != NULL) {
		*sms_service = sms;
	}
	return state;
}

/* An SMS-DELIVER PDU, decoded to the log: the sender and the text, UCS2 or
 * GSM 7-bit as its data coding scheme says. */
static void
mhi_sms_deliver(const uint8_t *pdu, size_t size)
{
	char sender[32];
	char text[160];
	size_t at = 0, n = 0;
	unsigned sca, fo, oa_len, oa_type, dcs, udl;

	if (size < 2) {
		return;
	}
	sca = pdu[at++];
	if (at + sca >= size) {
		return;
	}
	at += sca;
	fo = pdu[at++];
	if ((fo & 0x03) != 0x00) {
		hakios_tl("pcie2: cellular: SMS: a %s PDU (%zu bytes), not an SMS-DELIVER; left undecoded\n",
		    (fo & 3) == 2 ? "status report" : "not a deliver", size);
		return;
	}
	if (at + 2 > size) {
		return;
	}
	oa_len = pdu[at++];
	oa_type = pdu[at++];
	sender[0] = 0;
	if ((oa_type & 0x70) == 0x50) {
		/* alphanumeric: GSM 7-bit over (len+1)/2 octets */
		unsigned septets = oa_len * 4 / 7, bits = 0, value = 0;

		sender[n++] = '"';
		for (size_t i = 0; i < (oa_len + 1) / 2 && at < size && n + 2 < sizeof(sender); i++) {
			bits += 8;
			value |= (unsigned)pdu[at++] << (bits - 8);
			while (bits >= 7 && septets-- > 0) {
				unsigned septet = value & 0x7f;

				sender[n++] = mhi_sms_gsm7[septet];
				value >>= 7;
				bits -= 7;
			}
		}
		sender[n++] = '"';
	} else {
		if (((oa_type >> 4) & 0x07) == 0x01) {
			sender[n++] = '+';
		}
		n += mhi_sms_bcd(sender + n, sizeof(sender) - n, pdu + at, (oa_len + 1) / 2);
		at += (oa_len + 1) / 2;
	}
	sender[n] = 0;
	if (at + 2 + 7 + 1 > size) {
		return;
	}
	at++;			/* protocol identifier */
	dcs = pdu[at++];
	at += 7;		/* the service centre's timestamp */
	udl = pdu[at++];
	if ((dcs & 0x0c) == 0x08) {
		/* UCS2: UTF-16BE, two bytes a character: ASCII kept, the
		 * rest as '?' (the terminal's channel has no UTF-8) */
		size_t octets = udl;

		if (at + octets > size) {
			octets = size - at;
		}
		for (size_t i = 0; i + 1 < octets && n + 1 < sizeof(text); i += 2) {
			unsigned c = (unsigned)pdu[at + i] << 8 | pdu[at + i + 1];

			if (c == '\n' || c == '\r') {
				text[n++] = (char)c;
			} else if (c >= 0x20 && c < 0x7f) {
				text[n++] = (char)c;
			} else {
				text[n++] = '?';
			}
		}
	} else {
		/* GSM 7-bit, septets packed low bits first */
		unsigned septets = udl, bits = 0, value = 0;
		size_t i = at;

		while (septets-- > 0 && n + 1 < sizeof(text)) {
			while (bits < 7 && i < size) {
				value |= (unsigned)pdu[i++] << bits;
				bits += 8;
			}
			if (bits < 7) {
				break;
			}
			{
				unsigned septet = value & 0x7f;

				value >>= 7;
				bits -= 7;
				if (septet == 0x1b) {
					continue;	/* the extension table's prefix */
				}
				text[n++] = mhi_sms_gsm7[septet];
			}
		}
	}
	text[n] = 0;
	hakios_tl("pcie2: cellular: SMS: AN MT MESSAGE FROM %s (%zu bytes, DCS %#x): \"%s\"\n",
	    sender, size, dcs, text);
	snprintf(cellular_sms, sizeof(cellular_sms), "%s: %s", sender, text);
}

/* WMS Raw Read: TLV 1 {storage type, memory index u32}, TLV 0x10 mode 1
 * (GSM/WCDMA).  The answer's TLV 1 {tag, format, u16 length, PDU}. */
static void
mhi_sms_raw_read(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint8_t *answer, size_t answer_size)
{
	uint8_t request[7 + 8 + 4];
	size_t at = 7, got;
	unsigned result = ~0u, error = ~0u, format = 0;
	const uint8_t *raw = NULL;
	size_t raw_size = 0;

	memset(request, 0, sizeof(request));
	request[0] = 0x00;
	request[1] = 0xb4;
	request[2] = 0x00;
	request[3] = 0x22;	/* WMS raw read */
	request[4] = 0x00;
	request[at++] = 0x01;
	request[at++] = 0x05;
	request[at++] = 0x00;
	request[at++] = (uint8_t)sms_state.storage;
	request[at++] = (uint8_t)sms_state.index;
	request[at++] = (uint8_t)(sms_state.index >> 8);
	request[at++] = (uint8_t)(sms_state.index >> 16);
	request[at++] = (uint8_t)(sms_state.index >> 24);
	request[at++] = 0x10;
	request[at++] = 0x01;
	request[at++] = 0x00;
	request[at++] = 0x01;	/* GSM/WCDMA */
	request[5] = (uint8_t)(at - 7);
	request[6] = (uint8_t)((at - 7) >> 8);
	got = mhi_qmi_call(out, in, 0x1001, node, port, request, (unsigned)at,
	    "IPCR WMS raw read (the message the modem announced)", answer, answer_size);
	sms_state.pending = false;
	if (got < 16) {
		return;
	}
	{
		unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;
		const uint8_t *qmi = answer + header;
		size_t body = got - header;

		if (body < 7) {
			return;
		}
		for (at = 7; at + 3 <= body; ) {
			unsigned type = qmi[at];
			unsigned size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

			if (at + 3 + size > body) {
				break;
			}
			if (type == 2 && size >= 4) {
				result = qmi[at + 3] | (unsigned)qmi[at + 4] << 8;
				error = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
			} else if (type == 0x01 && size >= 7) {
				format = qmi[at + 4];
				raw_size = qmi[at + 5] | (unsigned)qmi[at + 6] << 8;
				raw = qmi + at + 7;
				if (raw_size > size - 4) {
					raw = NULL;
				}
			}
			at += 3 + size;
		}
	}
	if (raw == NULL || result != 0) {
		hakios_tl("pcie2: cellular: SMS: the raw read of storage %u index %u: result %u, error %u\n",
		    sms_state.storage, sms_state.index, result, error);
		return;
	}
	if (format == 0x06) {
		mhi_sms_deliver(raw, raw_size);
	} else {
		hakios_tl("pcie2: cellular: SMS: the message at storage %u index %u is format %u, not GSM point-to-point\n",
		    sms_state.storage, sms_state.index, format);
	}
}

/* One QMI payload from the modem: when it is a WMS Event Report indication
 * (type 4, message 0x0001), the new message is taken down -- the storage and
 * index it names, for the next cycle's Raw Read, or the raw PDU it already
 * carries, decoded here. */
static void
mhi_sms_note(const uint8_t *qmi, size_t body, const char *what)
{
	unsigned message, storage = 0, mode = 0, at;
	uint32_t index = 0;
	bool have_location = false;
	const uint8_t *raw = NULL;
	size_t raw_size = 0;
	unsigned format = 0;
	unsigned ack = 0x01;	/* QmiWmsAckIndicator: 0 = an ack is sent */
	uint32_t transaction = 0;
	bool have_route = false, ims = false;

	if (body < 7 || (qmi[0] & 0x06) != 0x04) {
		return;
	}
	message = qmi[3] | (unsigned)qmi[4] << 8;
	if (message != 0x0001) {
		return;
	}
	for (at = 7; at + 3 <= body; ) {
		unsigned type = qmi[at];
		unsigned size = qmi[at + 1] | (unsigned)qmi[at + 2] << 8;

		if (at + 3 + size > body) {
			break;
		}
		if (type == 0x10 && size >= 5) {
			storage = qmi[at + 3];
			index = qmi[at + 4] | (uint32_t)qmi[at + 5] << 8 |
			    (uint32_t)qmi[at + 6] << 16 | (uint32_t)qmi[at + 7] << 24;
			have_location = true;
		} else if (type == 0x11 && size >= 11) {
			/* ack u8, transaction u32, format u8, u16 length, PDU */
			ack = qmi[at + 3];
			transaction = qmi[at + 4] | (uint32_t)qmi[at + 5] << 8 |
			    (uint32_t)qmi[at + 6] << 16 |
			    (uint32_t)qmi[at + 7] << 24;
			have_route = true;
			format = qmi[at + 8];
			raw_size = qmi[at + 9] | (unsigned)qmi[at + 10] << 8;
			raw = qmi + at + 11;
			if (raw_size == 0 || raw_size > size - 8) {
				raw = NULL;
			}
		} else if (type == 0x12 && size >= 1) {
			mode = qmi[at + 3];
		} else if (type == 0x16 && size >= 1) {
			ims = qmi[at + 3] != 0;
		}
		at += 3 + size;
	}
	hakios_tl("pcie2: cellular: SMS: THE MODEM REPORTS A NEW MESSAGE while %s (storage %u, index %u, mode %u, %zu-byte PDU at hand)\n",
	    what, storage, index, mode, raw != NULL ? raw_size : 0);
	if (have_route && ack == 0 && ims) {
		sms_state.ack_pending = true;
		sms_state.ack_txn = transaction;
		hakios_tl("pcie2: cellular: SMS: the message asks for an IMS ack (transaction %#x); it goes out from the next cycle\n",
		    transaction);
	}
	if (raw != NULL) {
		if (format == 0x06) {
			mhi_sms_deliver(raw, raw_size);
		} else {
			char hex[3 * 24 + 1];
			size_t shown = raw_size > 24 ? 24 : raw_size;

			for (size_t i = 0; i < shown; i++) {
				snprintf(hex + i * 3, 4, " %02x", raw[i]);
			}
			hex[shown * 3] = 0;
			hakios_tl("pcie2: cellular: SMS: the delivered message is format %u %zu bytes:%s\n",
			    format, raw_size, hex);
		}
		return;
	}
	if (have_location && storage != 0xff && !sms_state.pending) {
		sms_state.storage = storage;
		sms_state.index = index;
		sms_state.pending = true;
		sms_state.reads = 0;
	}
}

/* WMS Send Ack (0x0037): the RP acknowledgement of an MT message that came
 * over IMS.  The Event Report's transfer route (TLV 0x11) carries the
 * transaction id and an ack indicator (0 = an ack is to be sent), and TLV
 * 0x16 marks the message SMS on IMS; this request hands the ack back with
 * TLV 0x01 {transaction id u32, protocol u8 (WCDMA 1), success u8} and TLV
 * 0x12 SMS on IMS.  It is sent from the milestone, never from inside a
 * wait: the note that records it may run while another request's answer is
 * awaited. */
static void
mhi_sms_ack(struct mhi_channel *out, struct mhi_channel *in, unsigned node,
    unsigned port, uint32_t transaction, uint8_t *answer, size_t answer_size)
{
	uint8_t request[32];
	uint8_t info[6];
	uint8_t on = 0x01;
	size_t at, got;
	unsigned result = ~0u, error = ~0u;

	info[0] = (uint8_t)transaction;
	info[1] = (uint8_t)(transaction >> 8);
	info[2] = (uint8_t)(transaction >> 16);
	info[3] = (uint8_t)(transaction >> 24);
	info[4] = 0x01;		/* WCDMA */
	info[5] = 0x01;		/* success */
	at = mhi_qmi_open(request, 0xb6, 0x0037);
	at = mhi_qmi_put(request, at, 0x01, info, sizeof(info));
	at = mhi_qmi_put(request, at, 0x12, &on, 1);
	mhi_qmi_seal(request, at);
	got = mhi_qmi_call(out, in, 0x1001, node, port, request, (unsigned)at,
	    "IPCR WMS send ack (the IMS message received)", answer, answer_size);
	if (got == 0) {
		hakios_tl("pcie2: cellular: SMS: the ack of transaction %#x went unanswered\n",
		    transaction);
		return;
	}
	mhi_qmi_outcome(answer, got, &result, &error);
	if (result == 0) {
		hakios_tl("pcie2: cellular: SMS: THE ACK OF THE IMS MESSAGE WAS SENT (transaction %#x)\n",
		    transaction);
	} else {
		hakios_tl("pcie2: cellular: SMS: the ack of transaction %#x says result %u, error %u\n",
		    transaction, result, error);
	}
}

/* The IMS envelope is brought up once the NAS reports the card registered,
 * even when WMS has not been announced: this network has no CS domain, so
 * the IMS path is the only SMS transport there can be (run 091313: 6718
 * cycles without WMS hid every IMS line).  The SMS stage later reuses what
 * this saw instead of poking the modem a second time. */
static struct {
	unsigned brought;		/* the WMS-less stage ran */
	unsigned enable;		/* the 0x12 settings were sent */
	unsigned imsa;			/* its registration state */
	unsigned sms_service;		/* its IMS SMS service */
} ims_state;

static void
mhi_ims_standalone(struct mhi_channel *out, struct mhi_channel *in,
    uint8_t *answer, size_t answer_size)
{
	unsigned imsa;

	ims_state.brought = 1;
	/* Look before poking: the registration may come by itself even
	 * when WMS has not been announced. */
	imsa = mhi_imsa_wait(out, in, answer, answer_size, 1,
	    &ims_state.sms_service);
	if (imsa == 2) {
		ims_state.imsa = imsa;
		hakios_tl("pcie2: cellular: IMS: the registration is already up even with WMS unannounced\n");
		return;
	}
	hakios_tl("pcie2: cellular: IMS: WMS is late; the IMS path is brought up without it so the run still carries the evidence\n");
	/* The IMS stack is turned on before the PDN is asked for: run
	 * 101216's start was refused with the settings never sent.  The
	 * return names whether the settings could be sent at all (run
	 * 124850 skipped them; the SMS loop retries when 0x12 appears). */
	ims_state.enable = mhi_ims_enable(out, in, answer, answer_size);
	if (mhi_wds_ims_up(out, in, answer, answer_size)) {
		ims_state.imsa = mhi_imsa_wait(out, in, answer, answer_size,
		    6, &ims_state.sms_service);
	} else {
		ims_state.imsa = imsa;
	}
	hakios_tl("pcie2: cellular: IMS: the WMS-less bring-up ends with the registration %u, the IMS SMS service %u\n",
	    ims_state.imsa, ims_state.sms_service);
}

/* ---- the call experiment: QMI VOICE (0x09) ----
 *
 * From libqmi's qmi-service-voice.json: Indication Register 0x0003 (TLV
 * 0x13 = 1, call notification events), Dial Call 0x0020 (TLV 0x01 the
 * calling number, answer TLV 0x10 the call ID), End Call 0x0021 (TLV 0x01
 * the call ID), and the All Call Status indication 0x002e that
 * mhi_voice_note decodes.  A live Android probe answered the voice
 * service's 0x0003 with result 0; the dial itself is this run's
 * experiment.  Armed by the pcie2_call boot argument, which names the
 * number to call ("pcie2_call=10099"); without it nothing is sent.  China
 * Broadnet has no CS domain, so the call can only ride the IMS
 * registration -- which is why the dial goes out after the IMS stage and
 * its result is quoted with the registration state that preceded it. */
static struct {
	unsigned tried;		/* the experiment ran */
	unsigned retried;	/* the late-registration attempt ran */
	unsigned registered;	/* the attempt was placed registered */
} voice_state;

/* The QMI protocol errors libqmi names (qmi-errors.h), for the voice
 * experiment's log lines. */
static const char *
mhi_qmi_error_name(unsigned error)
{
	switch (error) {
	case 0: return "none";
	case 1: return "malformed message";
	case 3: return "internal";
	case 5: return "client ids exhausted";
	case 7: return "invalid client id";
	case 9: return "invalid handle";
	case 13: return "no network found";
	case 14: return "call failed";
	case 15: return "out of call";
	case 16: return "not provisioned";
	case 17: return "missing argument";
	case 19: return "argument too long";
	case 23: return "device in use";
	case 24: return "network unsupported";
	case 25: return "device unsupported";
	case 26: return "no effect";
	case 28: return "invalid pdp type";
	case 29: return "invalid technology preference";
	case 37: return "uim uninitialized";
	case 42: return "requested number unsupported";
	case 43: return "interface not found";
	case 46: return "general error";
	case 47: return "unknown error";
	case 48: return "invalid argument";
	case 50: return "no entry";
	case 52: return "device not ready";
	case 53: return "network not ready";
	case 54: return "wms cause code";
	case 55: return "message not sent";
	case 56: return "message delivery failure";
	case 57: return "invalid message id";
	case 58: return "wms encoding";
	case 60: return "invalid transition";
	case 68: return "insufficient resources";
	case 69: return "disabled";
	case 70: return "invalid operation";
	case 71: return "invalid qmi command";
	case 72: return "wms t-pdu type";
	case 73: return "smsc address";
	default: return "error";
	}
}

/* The first bytes of a QMI frame, for the log: the voice service's answers
 * have been refused before and the raw TLV bytes are what tells why. */
static void
mhi_hex_line(const char *what, const uint8_t *data, unsigned size)
{
	char text[3 * 128 + 1];
	size_t at = 0;
	unsigned show = size > 128 ? 128 : size;

	for (unsigned i = 0; i < show; i++) {
		at += (size_t)snprintf(text + at, sizeof(text) - at, "%02x ", data[i]);
	}
	text[at != 0 ? at - 1 : 0] = 0;
	hakios_tl("pcie2: AMSS: %s: %u bytes: %s\n", what, size, text);
}

static void
mhi_voice_dial(struct mhi_channel *out, struct mhi_channel *in, uint8_t *answer,
    size_t answer_size)
{
	static char number[24];
	/* GobiAPI's VoiceCallOriginate request has a Call Type TLV (0x10):
	 * 0 voice automatic selection, 2 VoiceOverIP, 9 emergency.  Without
	 * it run 190153's dial was refused "invalid argument" although the
	 * modem still made a call indication.  Android's own dial of 10099
	 * (capture 06:29:24, voice service 3:0x30, msg 0x20) carried 0 --
	 * automatic -- beside TLV 0x16[4] = 1, 0x18[8] = 3, 0x19[8] = 0,
	 * 0x22[1] = 0 and 0x23[4] = 0, and the modem answered result 0 with
	 * call id 1; the dial here used to carry 2 and none of those. */
	static unsigned call_type = 0;
	static int parsed;
	uint8_t request[96];
	const uint8_t *value;
	size_t got, at, size, number_len;
	unsigned node = 0, port = 0, result = ~0u, error = ~0u, call = ~0u;

	if (!parsed) {
		char type_text[8];

		parsed = 1;
		/* The underscore makes bootargs.c copy the string (see the note
		 * below); the digits are read here, not by argnumcpy. */
		if (PE_parse_boot_argn("_pcie2_calltype", type_text, sizeof(type_text))) {
			unsigned parsed_type = 0;
			int digits = type_text[0] != 0;

			for (unsigned i = 0; type_text[i] != 0 && i < 4; i++) {
				if (type_text[i] < '0' || type_text[i] > '9') {
					digits = 0;
					break;
				}
				parsed_type = parsed_type * 10 +
				    (unsigned)(type_text[i] - '0');
			}
			if (digits && parsed_type <= 12) {
				call_type = parsed_type;
			}
		}
		/* The name begins with '_' on purpose: PE_parse_boot_argn
		 * copies a string only for such a name (bootargs.c).  Without
		 * the underscore "10099" is read as a number and its binary
		 * value lands in the buffer -- run 182052 dialled the two
		 * bytes 73 27 ("s'"), 10099's little-endian image, and the
		 * modem answered invalid argument. */
		if (!PE_parse_boot_argn("_pcie2_call", number, sizeof(number))) {
			number[0] = 0;
		}
		if (number[0] != 0) {
			hakios_tl("pcie2: cellular: VOICE: the call experiment is armed for %s, call type %u (_pcie2_call, _pcie2_calltype)\n",
			    number, call_type);
		} else {
			hakios_tl("pcie2: cellular: VOICE: the call experiment is off (no _pcie2_call)\n");
		}
	}
	if (number[0] == 0) {
		return;
	}
	if (!mhi_qrtr_service(0x09, &node, &port)) {
		hakios_tl("pcie2: cellular: VOICE: the modem has not announced the voice service; THE CALL CANNOT BE PLACED\n");
		return;
	}
	hakios_tl("pcie2: cellular: VOICE: the voice service is at %u:%#x; the IMS registration before the call is %u, the IMS SMS service %u\n",
	    node, port, ims_state.imsa, ims_state.sms_service);

	{
		uint8_t on = 1;

		at = mhi_qmi_open(request, 0x95, 0x0003);
		at = mhi_qmi_put(request, at, 0x13, &on, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1010, node, port, request,
		    (unsigned)at, "IPCR VOICE indication register", answer,
		    answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: VOICE: the call-notification registration says result %u, error %u%s\n",
		    result, error, got == 0 ? " (no answer)" : "");
	}

	number_len = strlen(number);
	at = mhi_qmi_open(request, 0x96, 0x0020);
	at = mhi_qmi_put(request, at, 0x01, number, number_len);
	{
		uint8_t type = (uint8_t)call_type;
		uint32_t service = 1, zero4 = 0;
		uint64_t attributes = 3, prl = 0;
		uint8_t zero1 = 0;

		at = mhi_qmi_put(request, at, 0x10, &type, 1);
		at = mhi_qmi_put(request, at, 0x16, &service, 4);
		at = mhi_qmi_put(request, at, 0x18, &attributes, 8);
		at = mhi_qmi_put(request, at, 0x19, &prl, 8);
		at = mhi_qmi_put(request, at, 0x22, &zero1, 1);
		at = mhi_qmi_put(request, at, 0x23, &zero4, 4);
	}
	mhi_qmi_seal(request, at);
	/* What this attempt is quoted against, and what the retry reads. */
	voice_call_progressed = false;
	voice_state.registered = ims_state.imsa == 2;
	mhi_hex_line("the dial request", request, (unsigned)at);
	got = mhi_qmi_call_slow(out, in, 0x1010, node, port, request,
	    (unsigned)at, "IPCR VOICE dial", answer, answer_size);
	mhi_qmi_outcome(answer, got, &result, &error);
	if (got != 0) {
		unsigned header = got >= 32 && mhi_qrtr_word(answer, 0) == 1 ? 32 : 16;

		if (got > header) {
			mhi_hex_line("the dial answer", answer + header,
			    (unsigned)(got - header));
		}
		value = mhi_qmi_answer_tlv(answer, got, 0x10, &size);
		if (value != NULL && size >= 1) {
			call = value[0];
		}
	}
	if (call == ~0u) {
		hakios_tl("pcie2: cellular: VOICE: THE DIAL TO %s says result %u, error %u (%s); no call id came back\n",
		    number, result, error, mhi_qmi_error_name(error));
	} else {
		hakios_tl("pcie2: cellular: VOICE: THE DIAL TO %s says result %u, error %u (%s), THE CALL ID IS %u\n",
		    number, result, error, mhi_qmi_error_name(error), call);
	}
	if (got == 0 || result != 0) {
		hakios_tl("pcie2: cellular: VOICE: the modem did not take the call in this form\n");
		/* An indication may have created the call before (or beside)
		 * the refused answer: the modem has it in CC set-up and the
		 * network has yet to answer.  Run 093352's 12 s ended the call
		 * before ALERTING could arrive; watch a minute now -- the
		 * states and a disconnect's reason are the point -- and end
		 * the call when the watch is over, or it would outlive the
		 * run.  A call the modem itself ends is left ended. */
		if (voice_indication_call != ~0u) {
			for (unsigned waited = 0; waited < 60000; waited += 3000) {
				(void)mhi_qrtr_pump(out, in, 3000, "during the refused call");
				if (voice_indication_call == ~0u) {
					break;
				}
			}
			hakios_tl("pcie2: cellular: VOICE: the refused call's watch is over: it %s; the call %s\n",
			    voice_call_progressed ? "reached ringing, talking or hold" :
			    "stayed in CC set-up",
			    voice_indication_call == ~0u ? "has ended" : "is still up");
		}
		if (voice_indication_call != ~0u) {
			uint8_t id = (uint8_t)voice_indication_call;

			at = mhi_qmi_open(request, 0x97, 0x0021);
			at = mhi_qmi_put(request, at, 0x01, &id, 1);
			mhi_qmi_seal(request, at);
			got = mhi_qmi_call(out, in, 0x1010, node, port, request,
			    (unsigned)at, "IPCR VOICE end call", answer, answer_size);
			mhi_qmi_outcome(answer, got, &result, &error);
			hakios_tl("pcie2: cellular: VOICE: THE END CALL OF %u (from the indication) says result %u, error %u (%s)\n",
			    voice_indication_call, result, error, mhi_qmi_error_name(error));
			voice_indication_call = ~0u;
		}
		return;
	}

	/* Thirty seconds of listening: the call's states come as All Call
	 * Status indications (decoded by mhi_voice_note) and the SMS poll's
	 * rhythm is not needed meanwhile. */
	for (unsigned waited = 0; waited < 30000; waited += 3000) {
		(void)mhi_qrtr_pump(out, in, 3000, "during the call");
	}
	if (voice_indication_call != ~0u) {
		call = voice_indication_call;
	}
	if (call != ~0u) {
		uint8_t id = (uint8_t)call;

		at = mhi_qmi_open(request, 0x97, 0x0021);
		at = mhi_qmi_put(request, at, 0x01, &id, 1);
		mhi_qmi_seal(request, at);
		got = mhi_qmi_call(out, in, 0x1010, node, port, request,
		    (unsigned)at, "IPCR VOICE end call", answer, answer_size);
		mhi_qmi_outcome(answer, got, &result, &error);
		hakios_tl("pcie2: cellular: VOICE: THE END CALL OF %u says result %u, error %u (%s)\n",
		    call, result, error, mhi_qmi_error_name(error));
		voice_indication_call = ~0u;
	}
}

/* Whether the boot armed the call experiment.  Parsed on its own so the
 * retry can tell without the dial's own parse having run -- a boot without
 * WMS leaves the milestone before its dial step, and the retry is the only
 * place a call would come from there. */
static bool
mhi_voice_armed(void)
{
	static int parsed;
	static bool armed;
	char text[24];

	if (!parsed) {
		parsed = 1;
		armed = PE_parse_boot_argn("_pcie2_call", text, sizeof(text)) &&
		    text[0] != 0;
	}
	return armed;
}

/* The call experiment's later attempt.  The milestone's dial goes out right
 * after the IMS stage, before the modem's own IMS stack has necessarily
 * registered -- its vendor state has to reach 4 and the settings follow, and
 * that can land after the milestone pass (or the milestone never reaches its
 * dial at all when WMS is unannounced).  Once the modem's 0x23 indication
 * says registered (2), the registration is confirmed with one getter and
 * the call is placed from that state; a first attempt that got as far as
 * ringing or talking is left alone.  The retry also takes the milestone's
 * place (tried = 1) so the number is never dialled twice in one pass. */
static void
mhi_voice_retry(struct mhi_channel *out, struct mhi_channel *in)
{
	uint8_t answer[512];
	unsigned imsa, sms_service = ~0u;

	if (!mhi_voice_armed() || voice_state.retried) {
		return;
	}
	if (voice_state.tried == 1 &&
	    (voice_state.registered || voice_call_progressed)) {
		return;
	}
	if (mhi_imsa_ind_state != 2) {
		return;
	}
	imsa = mhi_imsa_wait(out, in, answer, sizeof(answer), 1, &sms_service);
	if (imsa != 2) {
		hakios_tl("pcie2: cellular: VOICE: the late registration gate saw %u, not 2; the call waits\n",
		    imsa);
		return;
	}
	ims_state.imsa = imsa;
	ims_state.sms_service = sms_service;
	voice_state.retried = 1;
	voice_state.tried = 1;
	hakios_tl("pcie2: cellular: VOICE: the IMS registration arrived after the first call; THE CALL IS PLACED AGAIN\n");
	mhi_voice_dial(out, in, answer, sizeof(answer));
}

/* One step of the SMS milestone per cellular cycle, after the NAS poll: the
 * card's number and the WMS setup once registration is seen, then the read
 * of a message the modem announced.  Every step is skipped once done, so
 * the poll keeps its rhythm otherwise. */
static void
mhi_sms_milestone(struct mhi_channel *out, struct mhi_channel *in)
{
	/* 1536: a Get Profile Settings answer for a full profile (696-752
	 * bytes in run 121242) did not fit the 512 there used to be, and
	 * every "settings went unanswered" was really mhi_qrtr_answer
	 * refusing to truncate it. */
	uint8_t answer[1536];
	unsigned node, port, uim_node, uim_port;
	unsigned late_node, late_port;

	/* The paced handoff (run 154832's burst never reached the stack): an
	 * address the ask could not hand over yet leaves here, on a cycle of
	 * its own, only once the stack has bound and asked -- Android's
	 * daemon sends it to the stack 0.53 s after the answer to the ask,
	 * never inside it.  The ask path hands it over itself when the PDN
	 * already has it; this is the fallback for the other order.  Quiet
	 * when it cannot go yet; the gate inside mhi_imsdcm_indicate is the
	 * same rule. */
	if (mhi_imsdcm_pending &&
	    mhi_imsdcm_indicate(out, "between the cellular polls")) {
		mhi_imsdcm_pending = false;
	}
	if (!cellular_nas.registered) {
		/* Silent until now: run 203752's NAS attach landed ~12
		 * minutes after boot and this gate logged nothing in
		 * between, so a late attach and a stalled thread looked
		 * the same.  A line a minute tells them apart. */
		static unsigned nas_waits;

		if (++nas_waits % 30 == 0) {
			hakios_tl("pcie2: cellular: SMS: the NAS is still not registered (%u looks); the IMS/SMS stage waits\n",
			    nas_waits);
		}
		/* The modem's own IMSA is the only witness of what its stack
		 * does while the NAS has not registered: run 052215's
		 * indications moved 1 -> 0 with error 808 after the network
		 * attach, and nothing here was reading them until the NAS
		 * said registered.  A poll every ~30 looks keeps that
		 * visible; only once a handoff has gone out. */
		if (mhi_imsdcm_indicated && nas_waits % 30 == 15) {
			static unsigned late_last = ~0u;
			unsigned late_sms = ~0u;
			unsigned late_state = mhi_imsa_wait(out, in, answer,
			    sizeof(answer), 1, &late_sms);

			if (late_state != late_last) {
				late_last = late_state;
				hakios_tl("pcie2: cellular: SMS: with the NAS not registered the modem's own IMSA registration is %u (SMS %u)\n",
				    late_state, late_sms);
			}
		}
		return;
	}
	/* The handoff watch: run 140914 handed the modem's IMS stack its
	 * address and then heard nothing for four minutes.  Once a handoff
	 * has gone out -- again after a late bind; the counter restarts the
	 * clock, since a re-indication is the moment that matters -- read the
	 * modem's own enabler layer (IMSP 0x24: 1 uninitialized, 2
	 * initialized, 3 airplane, 4 registered) every ~10 s and the IMSA
	 * registration every ~30 s, for two minutes.  Run 203752's NAS attach
	 * came 12 minutes after boot and its handoff at 20:50:21; the old
	 * four-minute watch then outlived the 900 s run and the dial never
	 * ran -- two minutes keeps the evidence and leaves the dial its
	 * window. */
	if (mhi_imsdcm_indicated) {
		static unsigned seen_handoffs, watch_looks, watch_imsa;

		if (seen_handoffs != mhi_imsdcm_handoffs) {
			seen_handoffs = mhi_imsdcm_handoffs;
			watch_looks = 0;
			watch_imsa = 0;
			hakios_tl("pcie2: cellular: IMS: the handoff watch starts (%u handoff(s) so far): the modem's enabler state and registration are read for two minutes\n",
			    seen_handoffs);
		}
		if (watch_looks < 60) {
			watch_looks++;
			if (watch_looks % 5 == 0) {
				mhi_imsp_state(out, in, answer, sizeof(answer));
				if (++watch_imsa % 3 == 0) {
					unsigned watch_sms = ~0u;

					ims_state.imsa = mhi_imsa_wait(out, in,
					    answer, sizeof(answer), 1, &watch_sms);
					ims_state.sms_service = watch_sms;
				}
			}
		}
	}
	if (!mhi_qrtr_service(0x05, &node, &port)) {
		static unsigned asked;

		/* The name service answers a hello with its whole table
		 * (net/qrtr/ns.c: ctrl_cmd_hello -> announce_servers); the
		 * pump remembers what comes back.  Every ~20 s while WMS is
		 * missing, so a late registration is learned without waiting
		 * for the next run. */
		if (++asked >= 10) {
			asked = 0;
			if (mhi_send(out, mhi_qrtr_hello,
			    sizeof(mhi_qrtr_hello)) == 0) {
				hakios_tl("pcie2: cellular: SMS: WMS is still not announced; the modem's name service was asked for its table again\n");
			}
		}
		if (!sms_state.started) {
			hakios_tl("pcie2: cellular: SMS: the modem has not announced WMS yet\n");
		}
		/* WMS being late must not hide the IMS evidence; a few cycles
		 * of quiet first, so a registration coming by itself is seen
		 * rather than raced. */
		if (!ims_state.brought && asked >= 3) {
			mhi_ims_standalone(out, in, answer, sizeof(answer));
		} else if (ims_state.brought && !ims_state.enable &&
		    mhi_qrtr_service(0x12, &late_node, &late_port)) {
			/* Run 124850: the WMS-less bring-up reached the IMS
			 * settings before the modem had announced 0x12, and
			 * the announcement came only later.  Now that 0x12 is
			 * here, send the settings and ask for the PDN again. */
			hakios_tl("pcie2: cellular: IMS: the IMS settings service appeared late; the settings and the PDN are tried again\n");
			ims_state.enable = mhi_ims_enable(out, in, answer,
			    sizeof(answer));
			if (mhi_wds_ims_up(out, in, answer, sizeof(answer))) {
				/* The settings again, now that the ims PDN is
				 * up: run 063523 sent them only before it, and
				 * every answer was InvalidOperation. */
				ims_state.enable = mhi_ims_enable(out, in, answer,
				    sizeof(answer));
				ims_state.imsa = mhi_imsa_wait(out, in, answer,
				    sizeof(answer), 6, &ims_state.sms_service);
			} else {
				ims_state.imsa = mhi_imsa_wait(out, in, answer,
				    sizeof(answer), 1, &ims_state.sms_service);
			}
		}
		return;
	}
	/* WMS came early this boot: run the IMS/WDS stage before the
	 * milestone.  In runs 142914, 144447 and 151919 the milestone's
	 * three 0x4a transport probes (10 s apart) came first and the
	 * modem's endpoint was gone 19 s after the third, before WDS was
	 * ever touched.  Run 141245 reached this WMS state only after its
	 * IMS stage, surveyed all six profiles, and kept its link to the
	 * end.  The survey is the evidence this work needs, so it goes
	 * first; the old order stays when WDS is not announced yet. */
	if (!sms_state.started && !ims_state.brought) {
		unsigned wds_node, wds_port;

		if (mhi_qrtr_service(0x01, &wds_node, &wds_port)) {
			ims_state.brought = 1;
			ims_state.enable = mhi_ims_enable(out, in, answer,
			    sizeof(answer));
			if (mhi_wds_ims_up(out, in, answer, sizeof(answer))) {
				/* Run 063523's settings, sent before the PDN,
				 * were all InvalidOperation; again now. */
				ims_state.enable = mhi_ims_enable(out, in, answer,
				    sizeof(answer));
				ims_state.imsa = mhi_imsa_wait(out, in, answer,
				    sizeof(answer), 6, &ims_state.sms_service);
			}
			return;
		}
	}
	if (sms_state.ack_pending) {
		sms_state.ack_pending = false;
		mhi_sms_ack(out, in, node, port, sms_state.ack_txn, answer,
		    sizeof(answer));
	}
	if (!sms_state.started) {
		if (!mhi_qrtr_service(0x0b, &uim_node, &uim_port)) {
			hakios_tl("pcie2: cellular: SMS: the modem has not announced UIM yet\n");
			return;
		}
		sms_state.started = 1;
		hakios_tl("pcie2: cellular: SMS: THE MILESTONE STARTS: the card's own number, the new-message report, a message to self\n");
		mhi_sms_read_msisdn(out, in, uim_node, uim_port, answer, sizeof(answer));
		if (sms_state.own_digits == 0) {
			/* The AP's UIM client was refused (error 3): ask the
			 * modem's own DMS service for the same EF_MSISDN. */
			mhi_sms_dms_msisdn(out, in, answer, sizeof(answer));
		}
#ifdef HAKIOS_SMS_TARGET
		if (sms_state.own_digits == 0) {
			/* Neither the card nor the modem named the line; the
			 * out-of-tree header carries it (docs/cellular.md). */
			(void)mhi_sms_take_number(HAKIOS_SMS_TARGET,
			    "the build's out-of-tree target");
		}
#endif
		mhi_sms_event_report(out, in, node, port, answer, sizeof(answer));
		/* A submit can only go anywhere once the SMS stack has a
		 * transport; this network has no CS domain, so that transport
		 * is the IMS registration, and run 081226's three submits all
		 * failed "temporary" (error 56, failure type 0) with it still
		 * being had.  Wait a little for it to come by itself -- the
		 * modem may register on its own once the NAS attach settles --
		 * and bring the IMS path up when it has not. */
		{
			unsigned transport = ~0u, imsa = ~0u, sms_service = ~0u;
			int over_ims;

			if (ims_state.brought) {
				/* The WMS-less stage already poked this run; a
				 * fresh look says where it stands now. */
				imsa = mhi_imsa_wait(out, in, answer,
				    sizeof(answer), 1, &sms_service);
				ims_state.imsa = imsa;
				ims_state.sms_service = sms_service;
				transport = mhi_sms_transport(out, in, node,
				    port, answer, sizeof(answer));
			} else {
				for (unsigned waited = 0; waited < 30000; waited += 10000) {
					transport = mhi_sms_transport(out, in, node, port,
					    answer, sizeof(answer));
					if (transport == 4) {
						break;
					}
					delay(10000000);
				}
				if (transport != 4) {
					hakios_tl("pcie2: cellular: IMS: the SMS transport is not up by itself; the IMS path is brought up\n");
					ims_state.enable = mhi_ims_enable(out, in, answer,
					    sizeof(answer));
					if (mhi_wds_ims_up(out, in, answer, sizeof(answer))) {
						/* The settings once more with the
						 * PDN up: before it every answer
						 * was InvalidOperation. */
						ims_state.enable = mhi_ims_enable(out, in, answer,
						    sizeof(answer));
						imsa = mhi_imsa_wait(out, in, answer,
						    sizeof(answer), 6, &sms_service);
						transport = mhi_sms_transport(out, in, node,
						    port, answer, sizeof(answer));
					}
				} else {
					/* The transport came by itself; one quick IMSA
					 * look says whether it is the IMS registration,
					 * and so whether the submit is marked SMS on
					 * IMS the way qcril marks it. */
					imsa = mhi_imsa_wait(out, in, answer,
					    sizeof(answer), 1, &sms_service);
				}
			}
			/* The marker rides only on evidence: registered, with the
			 * SMS service available or not reported. */
			over_ims = imsa == 2 &&
			    (sms_service == ~0u || sms_service == 2);
			hakios_tl("pcie2: cellular: IMS: the transport before the submit is %u, the registration %u, the IMS SMS service %u, the SMS-on-IMS marker %s\n",
			    transport, imsa, sms_service,
			    over_ims ? "set" : "clear");
			/* The call experiment goes first: run 163801 put the SMS
			 * submit ahead of the dial and three unanswered attempts
			 * (~100 s each) starved the dial out of the run.  The
			 * IMS stage above has just been read, so the dial's
			 * result is quoted against the registration state it
			 * saw.  One attempt per boot. */
			if (!voice_state.tried) {
				voice_state.tried = 1;
				mhi_voice_dial(out, in, answer, sizeof(answer));
			}
			mhi_sms_send(out, in, node, port, over_ims, answer,
			    sizeof(answer));
		}
		return;
	}
	if (sms_state.pending && sms_state.reads < 4) {
		sms_state.reads++;
		mhi_sms_raw_read(out, in, node, port, answer, sizeof(answer));
	}
}

static void
mhi_cellular_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct mhi_channel *out = &mhi_channels[4], *in = &mhi_channels[5];
	uint8_t answer[512];
	unsigned cycles = 0, steer_tries = 0;

	while (cellular_wanted) {
		/* The boot's attempt may have run before WDS was announced; the
		 * modem's own attach lands within the first poll cycles, so the
		 * remaining tries go out before the first poll and each cycle
		 * after it (see mhi_wds_early). */
		if (!mhi_wds_steered && steer_tries < 6) {
			steer_tries++;
			mhi_wds_steered = mhi_wds_early("after the boot", out, in,
			    answer, sizeof(answer)) != 0;
			if (mhi_wds_steered) {
				hakios_tl("pcie2: cellular: early: the steering was accepted on try %u\n",
				    steer_tries);
			}
		}
		mhi_cellular_poll(out, in);
		mhi_sms_milestone(out, in);
		/* The vendor state turns 4 only once the modem's IMS stack is
		 * up, which takes longer than one enable call waits; the last
		 * step of the vendor sequence (0x3d/0x3c) is taken here when
		 * it does. */
		mhi_ims_vendor_catchup(out, in);
		mhi_voice_retry(out, in);
		(void)mhi_qrtr_pump(out, in, 1500, "between the cellular polls");
		for (unsigned ms = 0; ms < 400 && cellular_wanted; ms += 100) {
			delay(100000);
		}
		cycles++;
	}
	hakios_tl("pcie2: cellular: the NAS poll stopped after %u cycles\n", cycles);
	thread_terminate(current_thread());
}

static void
mhi_cellular_start(void)
{
	thread_t thread;

	if (cellular_wanted) {
		return;
	}
	cellular_wanted = true;
	if (kernel_thread_start(mhi_cellular_thread, NULL, &thread) != KERN_SUCCESS) {
		cellular_wanted = false;
		return;
	}
	thread_deallocate(thread);
	hakios_tl("pcie2: cellular: the NAS state is polled every two seconds; kern.hakios.cellular_* are live\n");
}

/* ---- CommCenter bridge (pcie2_mhi=2): IP_HW0 QMAP data plane + QRTR/QMUX loop ---- */

static uint8_t mhi_bb_txframe[MHI_PACKET];
static uint8_t mhi_bb_rxdata[MHI_PACKET];
static uint8_t mhi_bb_txdata[MHI_PACKET];
static uint8_t mhi_bb_ipcr_rx[MHI_PACKET];

int
umi_bb_announce(uint32_t service, uint32_t version, uint32_t instance, uint32_t port)
{
	mhi_bb_host_svc[0] = 4u;
	mhi_bb_host_svc[1] = service;
	mhi_bb_host_svc[2] = (version & 0xffu) | (instance << 8);
	mhi_bb_host_svc[3] = 1u;
	mhi_bb_host_svc[4] = port;
	mhi_bb_host_known = true;
	if (mhi_channels[4].ring != NULL) {
		uint8_t frame[40];
		size_t length = mhi_qrtr_put3(frame, 4, 0xfffe, mhi_bb_modem_node, 0xfffe,
		    mhi_bb_host_svc, sizeof(mhi_bb_host_svc));

		return mhi_send(&mhi_channels[4], frame, length);
	}
	return 0;
}

static int
mhi_bb_qmap_next(const uint8_t *frame, size_t length, size_t *at, uint8_t *mux,
    const uint8_t **payload, size_t *payload_length)
{
	size_t p, n, pad;

	if (!frame || !at || *at > length || length - *at < 4) {
		return -1;
	}
	p = *at;
	pad = frame[p] & 0x3f;
	n = ((size_t)frame[p + 2] << 8) | frame[p + 3];
	if (!n || pad > n || n > length - p - 4) {
		return -1;
	}
	if (mux) {
		*mux = frame[p + 1];
	}
	if (payload) {
		*payload = frame + p + 4;
	}
	if (payload_length) {
		*payload_length = n - pad;
	}
	*at = p + 4 + n;
	return 0;
}

static int
mhi_bb_data_poll(void)
{
	struct mhi_channel *tx = &mhi_channels[8], *rx = &mhi_channels[9];
	int error;

	if (!mhi_bb_data_open) {
		return 0;
	}
	if (tx->tx_done) {
		tx->tx_done = false;
		mhi_bb_data_tx++;
	}
	if (rx->rx_ready) {
		size_t rx_len = rx->rx_length, at = 0;

		if (rx_len > sizeof(mhi_bb_rxdata)) {
			rx_len = sizeof(mhi_bb_rxdata);
		}
		memcpy(mhi_bb_rxdata, (const void *)rx->buffer, rx_len);
		rx->rx_ready = false;
		error = mhi_post(rx, MHI_PACKET);
		if (error) {
			return error;
		}
		while (at < rx_len) {
			const uint8_t *ip;
			size_t n, start = at;
			uint8_t mux;

			if (mhi_bb_qmap_next(mhi_bb_rxdata, rx_len, &at, &mux, &ip, &n)) {
				mhi_bb_data_drop++;
				break;
			}
			if (!(mhi_bb_rxdata[start] & 0xc0)) {
				if (umi_bb_data_input(mux, ip, (uint32_t)n)) {
					mhi_bb_data_rx++;
				} else {
					mhi_bb_data_drop++;
				}
				mhi_bb_active_at = mhi_now_us();
			}
		}
	}
	if (!tx->tx_pending) {
		uint8_t mux = 1;
		uint32_t n = umi_bb_data_take(&mux, mhi_bb_txdata + 4, MHI_PACKET - 4);

		if (n > 0) {
			mhi_bb_active_at = mhi_now_us();
			mhi_bb_txdata[0] = 0;
			mhi_bb_txdata[1] = mux;
			mhi_bb_txdata[2] = (uint8_t)(n >> 8);
			mhi_bb_txdata[3] = (uint8_t)n;
			memcpy((void *)tx->buffer, mhi_bb_txdata, n + 4);
			mhi_publish();
			error = mhi_post(tx, n + 4);
			if (error) {
				return error;
			}
		}
	}
	return 0;
}

static int
mhi_bb_data_link(struct mhi_channel *out, struct mhi_channel *in, unsigned wda_node,
    unsigned wda_port, uint32_t *dl_max_out)
{
	/* WDA Set Data Format (0x0020): Raw IP (2), UL/DL QMAP (5), max 32 pkts / 32768 B, EP PCIe/4 */
	static const uint8_t wda_req[] = {
		0x00, 0x01, 0x00, 0x20, 0x00, 0x2e, 0x00,
		0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x12, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
		0x13, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
		0x15, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00,
		0x16, 0x04, 0x00, 0x00, 0x80, 0x00, 0x00,
		0x17, 0x08, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	};
	uint8_t answer[256];
	size_t got;
	unsigned header, result = ~0u, qerr = ~0u;
	uint32_t proto = 0, ul = 0, dl = 0, dl_max = 0;
	int error;

	error = mhi_enable_hw_events();
	if (error) {
		hakios_tl("pcie2: bridge: IP_HW0 event rings failed (%d)\n", error);
		return error;
	}
	error = mhi_start_channel(8, 100, 4);
	if (error) {
		hakios_tl("pcie2: bridge: IP_HW0 channel 100 failed (%d)\n", error);
		return error;
	}
	error = mhi_start_channel(9, 101, 5);
	if (error) {
		hakios_tl("pcie2: bridge: IP_HW0 channel 101 failed (%d)\n", error);
		return error;
	}
	mhi_bb_data_open = true;
	mhi_bb_log_at = mhi_now_us() + 60000000ull;

	got = mhi_qmi_call(out, in, 0x1004, wda_node, wda_port, wda_req, sizeof(wda_req),
	    "WDA set data format (IP_HW0 QMAP)", answer, sizeof(answer));
	if (got == 0) {
		hakios_tl("pcie2: bridge: WDA set data format timed out\n");
		return ETIMEDOUT;
	}
	mhi_qmi_outcome(answer, got, &result, &qerr);
	if (result != 0 || qerr != 0) {
		hakios_tl("pcie2: bridge: WDA set data format refused result=%u err=%u\n", result, qerr);
		return EIO;
	}
	header = (got >= 32 && mhi_qrtr_word(answer, 0) == 1) ? 32 : 16;
	got = mhi_qrtr_payload(answer, got, header);
	if (got >= header + 7) {
		const uint8_t *qmi = answer + header;
		size_t body = got - header;

		for (size_t at = 7; at + 3 <= body; ) {
			unsigned t = qmi[at];
			unsigned l = qmi[at + 1] | ((unsigned)qmi[at + 2] << 8);

			if (at + 3 + l > body) {
				break;
			}
			if (l == 4) {
				uint32_t v = mhi_qrtr_word(qmi, (unsigned)(at + 3));

				if (t == 0x11) proto = v;
				else if (t == 0x12) ul = v;
				else if (t == 0x13) dl = v;
				else if (t == 0x16) dl_max = v;
			}
			at += 3 + l;
		}
	}
	hakios_tl("pcie2: bridge: WDA format proto=%u ul=%u dl=%u dl_max=%u\n", proto, ul, dl, dl_max);
	if (proto != 2 || ul != 5 || dl != 5 || dl_max == 0 || dl_max > MHI_PACKET) {
		return EPROTONOSUPPORT;
	}
	*dl_max_out = dl_max;
	return 0;
}

static void
mhi_bb_send(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length)
{
	if (length > MHI_PACKET - 20) {
		hakios_tl("pcie2: bridge: send port %#x dropped (too large %u)\n", local_port, length);
		return;
	}
	size_t framed = mhi_qrtr_put3_frame(mhi_bb_txframe, 1,
	    local_port == 0xfffffffe ? 0xfffe : local_port,
	    node == 0xffffffff ? 0xffff : node,
	    port == 0xfffffffe ? 0xfffe : port,
	    message, length);
	int error = mhi_send(&mhi_channels[4], mhi_bb_txframe, framed);

	if (error) {
		hakios_tl("pcie2: bridge: send port %#x bytes %u failed (%d)\n", local_port, length, error);
	}
}

static void
mhi_bb_release(uint32_t local_port)
{
	uint32_t del[5] = { 6, 1, local_port, 0, 0 };
	uint8_t frame[40];
	size_t framed = mhi_qrtr_put3_frame(frame, 6, local_port, 0xffff, 0xfffe, del, sizeof(del));

	(void)mhi_send(&mhi_channels[4], frame, framed);
}

static void
mhi_bb_qrtr_rx(struct mhi_channel *out, const uint8_t *frame, size_t got, unsigned *modem)
{
	unsigned header, type, src_node, src_port, dst_port, declared;
	bool confirm;

	if (got >= 32 && mhi_qrtr_word(frame, 0) == 1) {
		header = 32;
		type = mhi_qrtr_word(frame, 4);
		src_node = mhi_qrtr_word(frame, 8);
		src_port = mhi_qrtr_word(frame, 12);
		confirm = mhi_qrtr_word(frame, 16) != 0;
		declared = mhi_qrtr_word(frame, 20);
		dst_port = mhi_qrtr_word(frame, 28);
	} else if (got >= 16) {
		type = frame[1];
		confirm = (frame[2] & 1) != 0;
		header = 16u + frame[3];
		declared = mhi_qrtr_word(frame, 4);
		src_node = frame[8] | ((unsigned)frame[9] << 8);
		src_port = frame[10] | ((unsigned)frame[11] << 8);
		dst_port = frame[14] | ((unsigned)frame[15] << 8);
		if (src_port == 0xfffe) src_port = 0xfffffffe;
		if (dst_port == 0xfffe) dst_port = 0xfffffffe;
	} else {
		return;
	}
	if (header > got || declared > got - header) {
		return;
	}
	const uint8_t *payload = frame + header;
	if (confirm) {
		mhi_qrtr_resume(out, dst_port == 0xfffffffe ? 0xfffe : dst_port,
		    src_node, src_port == 0xfffffffe ? 0xfffe : src_port);
	}
	if (type == 4 && declared >= 20 && mhi_qrtr_word(payload, 0) == 4) {
		mhi_qrtr_learn(mhi_qrtr_word(payload, 4), mhi_qrtr_word(payload, 8),
		    mhi_qrtr_word(payload, 12), mhi_qrtr_word(payload, 16));
	} else if (type == 5 && declared >= 20 && mhi_qrtr_word(payload, 0) == 5) {
		umi_bb_del_server(mhi_qrtr_word(payload, 4),
		    mhi_qrtr_word(payload, 12), mhi_qrtr_word(payload, 16));
	} else if (type == 2 && declared >= 4) {
		uint32_t hello[5] = { 2, 0, 0, 0, 0 };
		uint8_t hframe[40];
		size_t hlen;

		*modem = src_node;
		mhi_bb_modem_node = src_node;
		hlen = mhi_qrtr_put3(hframe, 2, 0xfffe, src_node, 0xfffe, hello, sizeof(hello));
		(void)mhi_send(out, hframe, hlen);
		mhi_qrtr_announce(out, src_node, ~0u, "bridge hello");
	} else if (type == 10 && declared >= 20 && mhi_qrtr_word(payload, 0) == 10) {
		unsigned service = mhi_qrtr_word(payload, 4);

		mhi_qrtr_announce(out, src_node, service, "bridge lookup");
		mhi_qrtr_announce_end(out, src_node);
	} else if (type == 1) {
		int hsvc = mhi_host_service(dst_port);

		if (hsvc >= 0 && mhi_host_services[hsvc].service == 4096) {
			mhi_tftp_request(out, frame, got, header, "bridge");
			return;
		}
		if (hsvc >= 0 && mhi_host_services[hsvc].service == 64 && declared >= 7) {
			mhi_servreg_answer(out, frame, got, header, "bridge");
			return;
		}
		if (hsvc < 0 && mhi_tftp_ack(out, frame, got, header, dst_port, "bridge")) {
			return;
		}
		if (umi_bb_qrtr(dst_port, src_node, src_port, payload, declared)) {
			mhi_bb_active_at = mhi_now_us();
			return;
		}
		static unsigned unknown_rx;
		if (unknown_rx++ < 16) {
			if (dst_port != 0x1007u && dst_port != 0x100eu && dst_port != 0x100fu)
					hakios_tl("pcie2: bridge: unhandled QRTR data dst=%#x from %u:%#x bytes=%u\n",
			    dst_port, src_node, src_port, declared);
		}
	}
}

static int
mhi_bb_bridge_run(struct mhi_channel *out, struct mhi_channel *in, unsigned modem)
{
	uint64_t quiet_at = mhi_now_us();
	bool data_attempted = false;
	unsigned wda_node = 0, wda_port = 0;
	int error = 0;

	mhi_stage = "bridge";
	mhi_bb_modem_node = modem;
	mhi_bb_active_at = quiet_at;
	mhi_bb_log_at = quiet_at + 60000000ull;

	if (mhi_qrtr_service(0x1a, &wda_node, &wda_port)) {
		uint32_t dl_max = 0;
		int derr;

		data_attempted = true;
		derr = mhi_bb_data_link(out, in, wda_node, wda_port, &dl_max);
		umi_bb_link(derr == 0 ? dl_max : 0);
	}
	{
		static uint8_t dsd_answer[1024];

		/* Arm Qualcomm DSD (0x2a) before CommCenter starts WDS: on dual-SIM
		 * X55M with physical slot 2 switched to logical slot 1, DSD 0x0065
		 * (DDS=1) and 0x0069 (UI data enabled) clear CM_ACL_FAILURE (3/1042). */
		mhi_dsd_settings_client(out, in, dsd_answer, sizeof(dsd_answer));
		mhi_dsd_arm(out, in, dsd_answer, sizeof(dsd_answer));
	}
	__atomic_store_n(&mhi_bb_live, 1, __ATOMIC_RELEASE);
	umi_bb_modem_ready();
	hakios_tl("pcie2: bridge: CommCenter bridge live (primary_gw %#x, secondary_gw %#x, QMI/QMAP owned by CommCenter)\n",
	    mhi_uim_state.primary_gw, mhi_uim_state.secondary_gw);

	for (;;) {
		error = mhi_poll();
		if (error) {
			hakios_tl("pcie2: bridge: MHI poll failed (%d); leaving bridge loop\n", error);
			break;
		}
		if (!data_attempted && mhi_qrtr_service(0x1a, &wda_node, &wda_port)) {
			uint32_t dl_max = 0;
			int derr;

			data_attempted = true;
			derr = mhi_bb_data_link(out, in, wda_node, wda_port, &dl_max);
			umi_bb_link(derr == 0 ? dl_max : 0);
		}
		if (umi_bb_poll(mhi_bb_send, mhi_bb_release)) {
			mhi_bb_active_at = mhi_now_us();
		}
		if (mhi_channels[MHI_EFS_IN_SLOT].rx_ready || mhi_efs_pos < mhi_efs_size) {
			(void)mhi_efs_service();
			quiet_at = mhi_now_us();
		}
		if (in->rx_ready) {
			size_t got = in->rx_length;

			if (got > sizeof(mhi_bb_ipcr_rx)) {
				got = sizeof(mhi_bb_ipcr_rx);
			}
			memcpy(mhi_bb_ipcr_rx, (const void *)in->buffer, got);
			in->rx_ready = false;
			error = mhi_post(in, MHI_PACKET);
			if (error) {
				hakios_tl("pcie2: bridge: IPCR RX repost failed (%d)\n", error);
				break;
			}
			mhi_bb_qrtr_rx(out, mhi_bb_ipcr_rx, got, &modem);
			quiet_at = mhi_now_us();
			continue;
		}
		uint64_t now = mhi_now_us();
		if (now >= mhi_bb_log_at) {
			hakios_tl("pcie2: bridge: IP_HW0 tx=%llu rx=%llu drop=%llu\n",
			    (unsigned long long)mhi_bb_data_tx, (unsigned long long)mhi_bb_data_rx,
			    (unsigned long long)mhi_bb_data_drop);
			umi_bb_report((unsigned)(now / 1000000ull));
			mhi_bb_log_at = now + 60000000ull;
		}
		mhi_bb_idle((now - quiet_at < 2000000ull || now - mhi_bb_active_at < 2000000ull) ? 1 : 50);
	}
	mhi_bb_data_open = false;
	__atomic_store_n(&mhi_bb_live, 0, __ATOMIC_RELEASE);
	umi_bb_modem_lost();
	return error;
}

static int
mhi_amss_ipcr(void)
{
	struct mhi_channel *out = &mhi_channels[4], *in = &mhi_channels[5];
	uint8_t answer[512], frame[64] = { 0 }, control[20];
	size_t length, got;
	unsigned modem = 2, uim_node = 0, uim_port = 0, rounds = 0;
	unsigned early_node = 0, early_port = 0;
	unsigned announced = 0, touched = 0;
	int error;

	mhi_dms_warmed = false;	/* one warm-up per AMSS boot */
	mhi_wds_steered = false;	/* and one attach steering */
	mhi_qrtr_hello_heard = false;
	error = mhi_start_channel(4, 20, 2);
	hakios_tl("pcie2: AMSS: IPCR channel 20 (out) %s\n", error == 0 ? "open" : "did not open");
	if (error) {
		return EIO;
	}
	error = mhi_start_channel(5, 21, 2);
	hakios_tl("pcie2: AMSS: IPCR channel 21 (in) %s\n", error == 0 ? "open" : "did not open");
	if (error) {
		return EIO;
	}
	/* The mission-mode EFS pair, exactly as umi's proven host opens it:
	 * channels 10 and 11 with IPCR, sharing its own event ring.  The modem
	 * then opens a Sahara memory-debug session on them and offers its EFS
	 * image (see the EFS section above).  A failure here is not fatal to
	 * the IPCR conversation, so it is logged and the probe goes on. */
	error = mhi_start_channel(MHI_EFS_OUT_SLOT, MHI_EFS_OUT_ID, MHI_EFS_ER);
	hakios_tl("pcie2: AMSS: EFS channel 10 (out) %s\n", error == 0 ? "open" : "did not open");
	if (error == 0) {
		error = mhi_start_channel(MHI_EFS_IN_SLOT, MHI_EFS_IN_ID, MHI_EFS_ER);
		hakios_tl("pcie2: AMSS: EFS channel 11 (in) %s\n", error == 0 ? "open" : "did not open");
	}
	/* A hello makes the nameservice announce its whole server table to the
	 * sender (net/qrtr/ns.c: ctrl_cmd_hello -> announce_servers), and the
	 * table grows while the modem's subsystems come up: run 032040 heard
	 * thirteen services but no UIM, and run 033152's round 0 heard 22 --
	 * UIM (0x0b) was the 22nd, at node 3 port 0x3f.  The probe therefore
	 * keeps asking (every few seconds, up to 12 rounds) until UIM is
	 * announced; round 0 goes out in the version 1 shape that works, every
	 * round after it in the version 3 shape the modem itself speaks, sent
	 * straight at its nameservice.  UIM arriving does not end the round:
	 * the announcements still queued behind it must be drained first, or
	 * the card-status answer (which follows them) would be cut off. */
	for (rounds = 0; rounds < 12 && uim_port == 0; rounds++) {
		unsigned quiet = 0, heard = 0;

		if (rounds == 0) {
			error = mhi_send(out, mhi_qrtr_hello, sizeof(mhi_qrtr_hello));
		} else {
			mhi_qrtr_control(control, 2, 0);
			length = mhi_qrtr_put3(frame, 2, 0xfffe, modem, 0xfffe, control,
			    sizeof(control));
			error = mhi_send(out, frame, length);
		}
		hakios_tl("pcie2: AMSS: IPCR hello %u: TX %s\n", rounds,
		    error == 0 ? "completed" : "no completion");
		if (error) {
			break;
		}
		if (rounds == 0) {
			/* The proven host sends a NEW_LOOKUP for the modem's own
			 * nameservice right behind its hello (umi_modem_mhi.inc:
			 * hello {2,0,0,0,0}, lookup {10,0,0,0,0}, both as transport
			 * type 2 and the lookup as type 10, to the broadcast node at
			 * the control port).  Run 062843 sent the lookup as a type 2
			 * frame straight at node 2 -- not what the reference does;
			 * the modem's name service sorts packets by type. */
			mhi_qrtr_control(control, 10, 0);
			length = mhi_qrtr_put3(frame, 10, 0xfffe, 0xffff, 0xfffe,
			    control, sizeof(control));
			hakios_tl("pcie2: AMSS: IPCR lookup: TX %s\n",
			    mhi_send(out, frame, length) == 0 ? "completed" : "no completion");
		}
		/* 48 cut run 044127's round 0 short: the modem queued 51
		 * host-service requests before the round's traffic stopped.
		 * 240 lets a round serve everything the boot asks; the two
		 * empty reads still end it once the queue is drained. */
		while (quiet < 2 && heard < 240) {
			size_t got = sizeof(answer);
			int result;

			(void)mhi_efs_service();
			result = mhi_receive(in, answer, &got,
			    heard == 0 && rounds == 0 ? 3000 : 1200);

			if (result) {
				quiet++;
				continue;
			}
			quiet = 0;
			heard++;
			/* The modem's boot asks node 1 for the registry's
			 * settings and TFTP's files while the rounds run;
			 * serve them the moment they are heard, as the pump
			 * does.  Run 044127's round 0 carried 51 such
			 * requests and the drain dropped them all. */
			if (!mhi_host_request(out, answer, got, "while the hello rounds run")) {
				mhi_qrtr_note(answer, got, rounds, &modem, &uim_node, &uim_port);
			}
			/* Stock's kernel announces the host's servers the
			 * moment the modem's hello names it, and the modem's
			 * boot waits on them.  Run 043045 announced only
			 * after its first round went quiet (16 s in), and the
			 * modem did not publish DMS until 33 s. */
			if (!announced && mhi_qrtr_hello_heard) {
				mhi_qrtr_announce(out, modem, ~0u, "after the hello");
				mhi_qrtr_tftp_lookup(out);
				announced = 1;
			}
			/* And question DMS the moment its announcement is
			 * parsed, not when the round's drain goes quiet: run
			 * 043045's round 3 drained for 13.7 s after its first
			 * frame. */
			if (!touched && mhi_qrtr_service(0x02, &early_node, &early_port)) {
				touched = 1;
				hakios_tl("pcie2: AMSS: DMS: first contact in hello round %u, "
				    "before the card\n", rounds);
				(void)mhi_dms_early(out, in, early_node, early_port, answer,
				    sizeof(answer));
			}
		}
		hakios_tl("pcie2: AMSS: IPCR hello %u: %u frames, UIM %s\n", rounds, heard,
		    uim_port != 0 ? "announced" : "not yet");
		/* The modem's boot waits for the host's services, and stock's
		 * kernel announces them the moment it hears the modem's hello:
		 * announce in the first round, not once every UIM round has
		 * run.  The TFTP lookup behind it has its own comment at
		 * mhi_qrtr_tftp_lookup. */
		if (!announced) {
			mhi_qrtr_announce(out, modem, ~0u, "after the hello");
			mhi_qrtr_tftp_lookup(out);
			announced = 1;
		}
		/* The first contact and the host's announcement now go out
		 * from inside the drain, the moment the hello or the DMS
		 * announcement is heard; only the settling pump is left
		 * here. */
		if (touched) {
			/* What the early contact draws -- TFTP reads, EFS
			 * traffic -- is served while the UIM rounds go on. */
			(void)mhi_qrtr_pump(out, in, 800, "while the boot settles");
		}
	}
	/* Tell the modem the host's services exist.  Stock's kernel answers the
	 * modem's hello with every local server it has (net/qrtr/ns.c,
	 * ctrl_cmd_hello -> announce_servers), and the external modem's boot
	 * then asks those servers -- the service registry, TFTP, RMTFS -- for
	 * what it needs before it comes up.  This host never announced
	 * anything: the modem's clients could not address node 1, its boot
	 * wait ran out, and about a minute after the AMSS came up the firmware
	 * died with every MHI register reading 0xffffffff (runs 043256 and
	 * 045244).  Phone #1's history is the same shape (docs/ru/cellular.md):
	 * DMS refused ONLINE with 52/60 until rmtfs, pd-mapper and tqftpserv
	 * were served.  The requests these announcements draw are collected by
	 * mhi_qrtr_pump below, not guessed at. */
	if (!announced) {
		/* Only a round that never reached the announce (the hello
		 * itself failed) arrives here. */
		mhi_qrtr_announce(out, modem, ~0u, "after the hello");
		mhi_qrtr_tftp_lookup(out);
	}
	/* DMS before the card is touched: read, and then ask for ONLINE the
	 * way umi's proven host does.  Its finding on this same modem: the
	 * radio goes online only after the modem has read its carrier
	 * configuration over TFTP; until then DMS answers ONLINE with error 52
	 * (DEVICE_NOT_READY), and the request is retried every couple of
	 * seconds -- with the channel pumped between tries, which is what
	 * serves the modem's TFTP/EFS/registry traffic -- until it is
	 * accepted.  Runs 042512 and 045244 asked too early (no EFS pair, no
	 * announcements) and were refused 52; run 043256 repeated the request
	 * onto a boot that had already given up.  This probe now has the EFS
	 * channels and the service hosts in place before it asks. */
	{
		unsigned dms_node, dms_port;

		if (mhi_qrtr_service(0x02, &dms_node, &dms_port)) {
			uint8_t get_mode[7] = {
				0x00, 0x50, 0x00, 0x2d, 0x00, 0x00, 0x00
			};
			/* The QMI header is type, transaction u16, message u16,
			 * length u16, all little-endian (the GET above is
			 * 00 50 | 00 2d | 00 00).  The set request's TLV is
			 * type 1, length 1, value 0 (ONLINE) -- four bytes, and
			 * the length field says 4.  The modem's own Get
			 * Operating Mode answer carries the same TLV the other
			 * way: its eleven-byte message ends 01 01 00 03, type 1,
			 * length 1, value 3 (OFFLINE), with the header's length
			 * 0x0b counting every one.  Runs 055654 and 060739 broke
			 * the header (message 0x042e, length 0x0100/0x0104);
			 * run 062843 then sent 01 01 00 as the whole message,
			 * length 3, and the value byte the TLV names fell
			 * outside the message -- every ONLINE request since has
			 * been answered FAILURE (1) by a modem that could not
			 * read the mode it was sent.  Run 112752's TFTP, UIM and
			 * NAS all worked; only this request was still wrong.
			 * The request and the low-power detour live in
			 * mhi_dms_online. */
			unsigned mode = ~0u;
			size_t got_mode;
			int online = 0;

			got_mode = mhi_qmi_call(out, in, 0x1001, dms_node, dms_port,
			    get_mode, sizeof(get_mode),
			    "IPCR DMS get operating mode (before the card)", answer,
			    sizeof(answer));
			if (got_mode != 0) {
				mode = mhi_dms_mode(answer, got_mode);
			}
			online = mode == 0;
			/* The card touch does not go out here.  Run 015246 put
			 * the switch 0.94 s before the dig and the modem read no
			 * config; run 021748 added a Set ONLINE right after the
			 * dig and the modem still read none.  In every scanned
			 * run the load begins in the same batch as the *second*
			 * card-status query -- the one sent after the switch --
			 * and with that batch: 121008 at tl 192.84 s, 123149 at
			 * 192.66 s, 133235 at 195.14 s, all three the txn 68
			 * status.  So the status, the switch and one more status
			 * go out in the dig window itself, below. */
			/* Android's DMS client is configured, not just opened, before
			 * its mode change (mhi_dms_warmup: the stock sequence captured
			 * from qcril's socket).  Run once per boot, here, before any
			 * ONLINE request -- every run so far asked a modem whose DMS
			 * client had only been opened, and run 164316's answer was
			 * still 52. */
			if (!online && !mhi_dms_warmed) {
				unsigned warmed;

				mhi_dms_warmed = true;
				warmed = mhi_dms_warmup(out, in, dms_node,
				    dms_port, answer, sizeof(answer));
				if (warmed != ~0u) {
					mode = warmed;
					online = mode == 0;
				}
			}
			/* The mode change before the card is timed, not early: the
			 * mcfg dig opens a window of about two seconds in which a
			 * Set Operating Mode ONLINE request starts the carrier-
			 * config load.  Runs 121008, 123149 and 133235 each sent
			 * one within 1.8 s of mbn_sw.dig's last ACK and the modem
			 * read oem_sw_j11 at once; 164316's first request came
			 * 3.1 s late and nothing was ever read; 005928 and 015246
			 * never asked (the warm-up above runs inside the window).
			 * The pump below runs in quarter-second slices so the
			 * request leaves the host within 0.25 s of the ACK, and
			 * the pumps stay whatever happens -- they serve the
			 * modem's TFTP, EFS and registry traffic. */
			{
				unsigned slice;
				unsigned error2 = ~0u;
				int asked = 0;

				/* Run 024138: its dig ACK came at tl 204.1 s,
				 * after a ten-second slice budget had already
				 * expired, so the sequence below never went out
				 * and the run was a control.  The budget is
				 * ninety seconds now; the branch leaves it.
				 * The loop no longer stops when the modem is
				 * online: the card work below still has to go
				 * out at the dig, which is what starts the
				 * carrier-config load; only the mode request
				 * itself is skipped then. */
				for (slice = 0; slice < (pcie2_mhi_mode == 2 ? 80u : 360u); slice++) {
					if (asked == 0 && mhi_mcfg_dig_done != 0) {
						uint8_t set[11] = {
							0x00, 0x58, 0x00, 0x2e, 0x00, 0x04, 0x00,
							0x01, 0x01, 0x00, 0x00
						};

						asked = 1;
						/* The card work the scanned runs
						 * did at the dig: a fresh status,
						 * the switch, and the second
						 * status -- the one whose batch
						 * the carrier load began in.  The
						 * scanning runs sent that second
						 * status 1.4 to 4.4 s after the
						 * switch and its answer read both
						 * slots ABSENT: the card was still
						 * initializing, and that answer is
						 * where the load started.  Keep
						 * the gap, or the status reads the
						 * card still present. */
						if (uim_port != 0) {
							int described;
							int switched = 0;

							described = mhi_uim_ask_status(out,
							    in, uim_node, uim_port, 0x70, 1,
							    answer, sizeof(answer));
							if (described &&
							    mhi_uim_state.primary_gw == 0xffff &&
							    mhi_uim_state.logical_aid_len == 0 &&
							    mhi_uim_state.ready_slot > 1) {
								switched = mhi_uim_switch_slot(out,
								    in, uim_node, uim_port, 0x71,
								    mhi_uim_state.ready_slot);
							}
							if (switched) {
								unsigned poll;

								mhi_uim_dig_switched = 1;
								/* The scanning runs read
								 * both slots ABSENT in the
								 * status answered 1.4 to
								 * 4.4 s after the switch.
								 * Run 025336's fixed 1.5 s
								 * wait read the card already
								 * DETECTED on slot 1 and no
								 * load followed, so ask until
								 * an answer reads no card. */
								for (poll = 0; poll < 8; poll++) {
									delay(700000);
									(void)mhi_uim_ask_status(out,
									    in, uim_node, uim_port,
									    0x72 + poll, 1, answer,
									    sizeof(answer));
									if (mhi_uim_state.card_present == 0 &&
									    mhi_uim_state.logical_aid_len == 0) {
										break;
									}
								}
							} else {
								(void)mhi_uim_ask_status(out, in,
								    uim_node, uim_port, 0x72, 1,
								    answer, sizeof(answer));
							}
						}
						/* Then the Set ONLINE frame the
						 * scanned runs sent with it --
						 * unless the first contact already
						 * had the mode change accepted. */
						if (!online) {
							(void)mhi_qmi_call(out, in, 0x1001,
							    dms_node, dms_port, set, sizeof(set),
							    "IPCR DMS set operating mode ONLINE (at the dig)",
							    answer, sizeof(answer));
							(void)mhi_dms_online(out, in, dms_node,
							    dms_port, 0x59,
							    "IPCR DMS set operating mode ONLINE (at the dig, detour)",
							    answer, sizeof(answer), &error2);
						}
						/* The card must come up again for
						 * the card block; do not sit out
						 * the rest of a long budget. */
						break;
					}
					mhi_qrtr_pump(out, in, 250, "before the card");
				}
			}
			for (unsigned t = 0; !online && t < 3; t++) {
				get_mode[1] = (uint8_t)(0x50 + t);
				got_mode = mhi_qmi_call(out, in, 0x1001, dms_node,
				    dms_port, get_mode, sizeof(get_mode),
				    "IPCR DMS get operating mode (before the card)", answer,
				    sizeof(answer));
				if (got_mode != 0) {
					mode = mhi_dms_mode(answer, got_mode);
					online = mode == 0;
				}
			}
			mhi_amss_dump("DMS: after the boot-time pumps");
			hakios_tl("pcie2: AMSS: DMS: before the card the modem is %s; the mode change waits for the boot to settle\n",
			    online ? "ONLINE" : "still offline");
		} else {
			hakios_tl("pcie2: AMSS: DMS: not announced yet; the mode stays for later\n");
		}
	}
	/* The attach steering, first stage: the modem has read its carrier
	 * configuration and DMS has been asked for ONLINE; the card work and
	 * the EFS sync are next.  Run 234850 accepted the set before the
	 * register and the attach still read {6}, so every stage that
	 * precedes the register sets the list and reads it back (see
	 * mhi_wds_early). */
	if (pcie2_mhi_mode != 2) {
		mhi_wds_steered = mhi_wds_early("before the card work", out, in, answer,
		    sizeof(answer)) != 0;
	}
	/* The SIM itself, over the port UIM named.  GET_CARD_STATUS (0x2f)
	 * with no TLVs, framed the way libqmi frames it: the size field is the
	 * message's own seven bytes, and the frame is padded to a multiple of
	 * four -- run 041016's breakthrough.  A size of 8 (the pad byte
	 * counted) is answered result 1, error 2 (NO_MEMORY) on every message
	 * this AMSS gets, while size 7 returns the whole card: primary GW
	 * application 0xffff (none: the subscription is bound to nothing),
	 * slot 1 ERROR, slot 2 a READY USIM -- phone #1's situation exactly
	 * (drivers/modem/hakios.mpss.c, runs 30-33), whose two remedies are
	 * replayed here: point logical slot 1 at the slot with the ready card
	 * (UIM Switch Slot 0x46), then bind that card's USIM to the primary GW
	 * session (UIM Change Provisioning Session 0x38).  DMS was asked
	 * before this block, and NAS is asked for the registration after it. */
	if (uim_port != 0) {
		int described, switched = 0, provisioned = 0;

		described = mhi_uim_ask_status(out, in, uim_node, uim_port, 0x40, 3,
		    answer, sizeof(answer));
		if (described) {
			if (mhi_uim_state.primary_gw == 0xffff &&
			    mhi_uim_state.logical_aid_len == 0 &&
			    mhi_uim_state.ready_slot > 1 &&
			    mhi_uim_dig_switched == 0) {
				switched = mhi_uim_switch_slot(out, in, uim_node, uim_port,
				    0x43, mhi_uim_state.ready_slot);
				/* Run 123149: the switch was answered result 0 while
				 * the modem was still reading its carrier
				 * configuration; two seconds later both logical slots
				 * were ABSENT (the card initializing again), and that
				 * was the last status read before the EFS sync.  The
				 * wait asks until the card comes back; the EFS watch
				 * below keeps asking after that. */
				/* With the switch off the card stays in its own
				 * slot and nothing comes to logical slot 1, so the
				 * wait would only spend its six tries. */
				if (mhi_uim_switch_allowed()) {
					described = mhi_uim_wait_ready(out, in,
					    uim_node, uim_port, 0x44, 6, answer,
					    sizeof(answer));
				}
			} else if (mhi_uim_state.logical_aid_len == 0 &&
			    mhi_uim_dig_switched != 0) {
				/* The dig-window sequence already switched; the
				 * card is still initializing.  Wait for it to
				 * come back instead of switching it again. */
				switched = 1;
				described = mhi_uim_wait_ready(out, in, uim_node, uim_port,
				    0x45, 6, answer, sizeof(answer));
			} else if (mhi_uim_state.logical_aid_len != 0) {
				hakios_tl("pcie2: AMSS: UIM: the card is already ready in logical slot 1\n");
			}
			if (described && mhi_uim_state.primary_gw == 0xffff &&
			    mhi_uim_state.logical_aid_len != 0) {
				provisioned = mhi_uim_provision(out, in, uim_node,
				    uim_port, 0x4b, answer, sizeof(answer));
				delay(2000000);
				described = mhi_uim_ask_status(out, in, uim_node, uim_port,
				    0x4c, 2, answer, sizeof(answer));
				if (described && mhi_uim_state.primary_gw != 0xffff) {
					hakios_tl("pcie2: AMSS: UIM: THE PROVISIONING TOOK: primary GW application %u (slot %u)\n",
					    mhi_uim_state.primary_gw & 0xff,
					    mhi_uim_state.primary_gw >> 8);
				}
			}
			hakios_tl("pcie2: AMSS: UIM: after the switch (%d) and the provisioning (%d): primary GW application %u\n",
			    switched, provisioned, mhi_uim_state.primary_gw);
		} else {
			/* The card was not described: ask the services the nameservice
			 * announced a bare GET_VERSION -- is UIM alone sick, or does
			 * the whole QMI path refuse? */
			mhi_qmi_get_version(out, in, 0x1001, uim_node, uim_port,
			    "IPCR UIM version", answer, sizeof(answer));
			for (unsigned i = 0; i < mhi_qrtr_server_count; i++) {
				char what[32];

				if (mhi_qrtr_servers[i].service == 0x0b) {
					continue;	/* UIM was asked already */
				}
				snprintf(what, sizeof(what), "IPCR %s version",
				    mhi_qrtr_name(mhi_qrtr_servers[i].service));
				mhi_qmi_get_version(out, in, 0x1001,
				    mhi_qrtr_servers[i].node, mhi_qrtr_servers[i].port,
				    what, answer, sizeof(answer));
			}
		}
		if (pcie2_mhi_mode == 2) {
			unsigned dn, dp;

			for (unsigned w = 0; w < 20 &&
			    (!mhi_qrtr_service(0x03, &dn, &dp) ||
			    !mhi_qrtr_service(0x01, &dn, &dp) ||
			    !mhi_qrtr_service(0x1a, &dn, &dp)); w++) {
				(void)mhi_qrtr_pump(out, in, 500, "before bridge handoff");
			}
			return mhi_bb_bridge_run(out, in, modem);
		}
		/* The attach steering, second stage: the card work above is
		 * done, the EFS sync and the NAS register are next, and the
		 * modem builds its attach from whatever the list holds then.
		 * The set and its read-back here bracket the card work. */
		mhi_wds_steered = mhi_wds_early("after the card work", out, in,
		    answer, sizeof(answer)) != 0;
		/* DMS after the card work: read the mode, do not change it.  The
		 * card work above is what the boot needs; a mode change this
		 * early has been refused in every run (see the block before the
		 * card), and run 233725's vendor pair stopped the carrier-config
		 * scan.  The pumps here keep serving the modem's TFTP and EFS
		 * traffic while the scan runs; the mode change goes out once,
		 * late, from the watch after the EFS sync. */
		{
			unsigned dms_node, dms_port;

			if (mhi_qrtr_service(0x02, &dms_node, &dms_port)) {
				uint8_t get_mode[7] = {
					0x00, 0x5a, 0x00, 0x2d, 0x00, 0x00, 0x00
				};
				/* Header layout and length as in the before-card
				 * request: 01 01 00 00 is the whole TLV, 4 bytes,
				 * value 0 (ONLINE), and the length field says 4.
				 * The request and the low-power detour live in
				 * mhi_dms_online. */
				unsigned mode = ~0u;
				size_t got_mode;
				int online;

				got_mode = mhi_qmi_call(out, in, 0x1001,
				    dms_node, dms_port, get_mode, sizeof(get_mode),
				    "IPCR DMS get operating mode (after the card)",
				    answer, sizeof(answer));
				if (got_mode != 0) {
					mode = mhi_dms_mode(answer, got_mode);
				}
				online = mode == 0;
				for (unsigned t = 0; !online && t < 3; t++) {
					mhi_qrtr_pump(out, in, 2500, "after the card");
					get_mode[1] = (uint8_t)(0x5a + t + 1);
					got_mode = mhi_qmi_call(out, in, 0x1001, dms_node,
					    dms_port, get_mode, sizeof(get_mode),
					    "IPCR DMS get operating mode (after the card)",
					    answer, sizeof(answer));
					if (got_mode != 0) {
						mode = mhi_dms_mode(answer, got_mode);
						online = mode == 0;
					}
				}
				hakios_tl("pcie2: AMSS: DMS: after the card the modem is %s; the mode change still waits\n",
				    online ? "ONLINE" : "still offline");
			}
		}
		/* The attach the network builds is steered before the registration
		 * is asked for: run 225123 registered at 22:52:02 with the boot
		 * list {6}, "ims", and the IMS phase's set to {1} came 55 s too
		 * late for the bearer the CM judged every Start against (see
		 * mhi_wds_early).  Run 234850's set was accepted and the attach
		 * still read {6}, so this last stage runs unconditionally: its
		 * read-back is the value right before the register. */
		mhi_wds_steered = mhi_wds_early("before the registration", out,
		    in, answer, sizeof(answer)) != 0;
		/* The IMS PDN is brought up here, before the NAS register and
		 * before the modem's IMS stack asks for it: Android's capture
		 * (imscap, 04:09) has the address indication 0.44 s after the
		 * stack's APN request (09:17.304 -> 09:17.746) and the
		 * registration state 1 35 ms after that (09:17.781, state 2 at
		 * 09:18.446).  Run 123850's stack asked during the NAS register
		 * wait while the PDN was only started two minutes later in the
		 * SMS stage; by the time the address came the stack had stopped
		 * waiting, and it never registered.  With the PDN already up,
		 * mhi_imsdcm_answer answers the request with the address at
		 * once; if the settings or the PDN cannot start here, the SMS
		 * stage keeps its own attempt (ims_state.brought stays 0). */
		if (!ims_state.brought && mhi_qrtr_service(0x12, &early_node,
		    &early_port)) {
			int pdn;

			/* Run 131620: the stack asked for the APN at 13:17:02
			 * and the vendor scan inside mhi_ims_enable held this
			 * stage until 13:18:50, so the address went out 108 s
			 * after the ask and the stack never registered -- on
			 * Android the daemon hands the address 0.53 s after
			 * the ask (imscap2 06:17:32.02 -> .56).  The PDN is
			 * started first now, so it is up when the stack asks;
			 * the enable scan follows it, and only a PDN that did
			 * not come up is tried once more after the enable. */
			/* Bind IMSA and turn its indications on before the
			 * PDN and the vendor settings: Android's daemon is
			 * subscribed from startup, long before the address
			 * handoff (imscap2 binds on fd 60, and the 0x23
			 * indications narrate every attempt), while this
			 * driver bound only after the handoff and its
			 * getters answer 808 whether or not an attempt is
			 * in flight.  Rounds 0: the bind and the register
			 * only. */
			(void)mhi_imsa_wait(out, in, answer, sizeof(answer), 0,
			    NULL);
			pdn = mhi_wds_ims_up(out, in, answer, sizeof(answer));
			ims_state.enable = mhi_ims_enable(out, in, answer,
			    sizeof(answer));
			if (!pdn && ims_state.enable) {
				pdn = mhi_wds_ims_up(out, in, answer,
				    sizeof(answer));
			}
			if (pdn) {
				ims_state.brought = 1;
				hakios_tl("pcie2: AMSS: IPCR IMS: the PDN is up before the registration; the stack's APN request is answered with the address at once\n");
				ims_state.imsa = mhi_imsa_wait(out, in, answer,
				    sizeof(answer), 1, &ims_state.sms_service);
			} else {
				hakios_tl("pcie2: AMSS: IPCR IMS: the PDN did not come up before the registration (settings %s); the SMS stage keeps its own attempt\n",
				    ims_state.enable ? "sent" : "skipped");
			}
		}
		/* NAS: the selection preference, the registration itself (TLV 1
		 * action 1, automatic), and the system info.  Error 37 on the
		 * registration is UIM_UNINITIALIZED: phone #1's run 31 saw it
		 * while the card initialized after the slot switch -- ten
		 * seconds, then the card and the registration asked again. */
		{
			unsigned nas_node, nas_port;

			if (mhi_qrtr_service(0x03, &nas_node, &nas_port)) {
				static const uint8_t selection[7] = {
					0x00, 0x60, 0x00, 0x34, 0x00, 0x00, 0x00
				};
				uint8_t reg[11] = {
					0x00, 0x61, 0x00, 0x22, 0x00, 0x04, 0x00,
					0x01, 0x01, 0x00, 0x01
				};
				static const uint8_t info[7] = {
					0x00, 0x62, 0x00, 0x4d, 0x00, 0x00, 0x00
				};
				unsigned result, error2;
				size_t got_nas;

				(void)mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
				    selection, sizeof(selection),
				    "IPCR NAS selection preference", answer, sizeof(answer));
				delay(1000000);
				got_nas = mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
				    reg, sizeof(reg), "IPCR NAS initiate register", answer,
				    sizeof(answer));
				mhi_qmi_outcome(answer, got_nas, &result, &error2);
				if (got_nas == 0 || (result != 0 && error2 == 37)) {
					hakios_tl("pcie2: AMSS: NAS: the registration %s; ten seconds, then again\n",
					    got_nas == 0 ? "was not answered" :
					    "is refused while UIM initializes (error 37)");
					mhi_qrtr_pump(out, in, 10000, "while the card initializes");
					(void)mhi_uim_ask_status(out, in, uim_node, uim_port,
					    0x49, 1, answer, sizeof(answer));
					reg[1] = 0x63;
					(void)mhi_qmi_call(out, in, 0x1001, nas_node,
					    nas_port, reg, sizeof(reg),
					    "IPCR NAS initiate register (again)", answer,
					    sizeof(answer));
				}
				delay(3000000);
				(void)mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
				    info, sizeof(info), "IPCR NAS system info", answer,
				    sizeof(answer));
			} else {
				hakios_tl("pcie2: AMSS: NAS was not announced; the registration is not asked\n");
			}
		}
	} else {
		hakios_tl("pcie2: AMSS: IPCR: no UIM after %u hellos, %u services seen\n",
		    rounds, mhi_qrtr_seen_count);
	}
	/* Watch the channel a while longer.  The AMSS used to die about a
	 * minute after it came up; if the announcements were what its boot
	 * waited for, the requests they draw appear in this window, and the
	 * dump after it says whether the firmware is still there. */
	(void)mhi_qrtr_pump(out, in, 30000, "after NAS");
	/* Run 115721 answered the question this block was written for: with the
	 * EFS handshake done, eight more ONLINE requests were answered 52 and
	 * the modem stayed offline -- the EFS session is not what holds "device
	 * ready".  What every request since 112752 has lacked is the detour:
	 * offline -> LOW POWER -> ONLINE (see mhi_dms_online).  Ask once more
	 * through it, and if the radio comes on, register NAS, which refused
	 * error 37 while the modem was offline. */
	{
		unsigned dms_node, dms_port;

		if (mhi_qrtr_service(0x02, &dms_node, &dms_port)) {
			uint8_t get_mode[7] = {
				0x00, 0xc0, 0x00, 0x2d, 0x00, 0x00, 0x00
			};
			unsigned mode = ~0u, asked = 0, last_error = ~0u;
			unsigned card_settled = 0, late_switches = 0, late_provisions = 0;
			unsigned last_result = ~0u, last_nas_error = ~0u;
			uint64_t now_ticks = 0, quiet_from = 0, last_ask = 0;
			size_t got_mode;
			int online, asking = 0;

			got_mode = mhi_qmi_call(out, in, 0x1001, dms_node, dms_port, get_mode,
			    sizeof(get_mode),
			    "IPCR DMS get operating mode (after the EFS sync)", answer,
			    sizeof(answer));
			if (got_mode != 0) {
				mode = mhi_dms_mode(answer, got_mode);
			}
			online = mode == 0;
			/* Run 121008: the carrier-config scan (267 entries in
			 * mbn_sw.txt) was still going when eight attempts ran out.
			 * Run 133235: the scan was still in SEA/XTL when the
			 * requests ran out, and every request sent while it ran --
			 * and after it -- was answered 60.  Run 233725: the vendor
			 * pair sent into the scan stopped it dead after mbn_sw.dig.
			 * So the pumps run alone first, the way stock's client
			 * leaves the modem's boot alone; the first mode change goes
			 * out after 150 s of quiet, and from then on one attempt
			 * every thirty seconds -- a storm is not what stock does.
			 * The exit is the modem's own silence (thirty quiet rounds,
			 * but only once the asking has begun; run 125953 wrote off an
			 * exit on five silent rounds and left a working modem
			 * unanswered), and the hard cap stays as the scene's
			 * backstop. */
			unsigned idle = 0;

			clock_get_uptime(&quiet_from);
			for (unsigned t = 0; !online && t < 900; t++) {
				unsigned error2 = ~0u;
				unsigned heard;
				uint64_t elapsed_ns = 0, since_ns = 0;
				unsigned long long quiet_s = 0, since_s = ~0ull;

				clock_get_uptime(&now_ticks);
				if (now_ticks > quiet_from) {
					absolutetime_to_nanoseconds(now_ticks - quiet_from,
					    &elapsed_ns);
					quiet_s = elapsed_ns / 1000000000ull;
				}
				if (asking == 0 && quiet_s >= 150) {
					asking = 1;
					idle = 0;	/* the grace is not silence to exit on */
					last_ask = now_ticks;
					hakios_tl("pcie2: AMSS: DMS: %llu s of quiet after the EFS sync; the first mode change goes out now\n",
					    quiet_s);
				}
				if (asking != 0 && now_ticks > last_ask) {
					absolutetime_to_nanoseconds(now_ticks - last_ask,
					    &since_ns);
					since_s = since_ns / 1000000000ull;
				}
				if (asking != 0 && since_s >= 30) {
					last_ask = now_ticks;
					asked++;
					online = mhi_dms_online(out, in, dms_node, dms_port,
					    (unsigned)(0xc1 + (t & 0x0f)),
					    "IPCR DMS set operating mode ONLINE (after the EFS sync)",
					    answer, sizeof(answer), &error2);
					if (online) {
						break;
					}
					if (error2 != last_error) {
						last_error = error2;
						hakios_tl("pcie2: AMSS: DMS: ONLINE after the EFS sync "
						    "answered error %u\n", error2);
					}
				}
				heard = mhi_qrtr_pump(out, in, 3000, "after the EFS sync");
				idle = heard == 0 ? idle + 1 : 0;
				if ((t & 31) == 31) {
					hakios_tl("pcie2: AMSS: DMS: round %u after the "
					    "EFS sync: %u frames heard, still offline\n",
					    t + 1, heard);
				}
				/* Thirty quiet rounds after the asking began, and at
				 * least twenty paced requests sent: run 005928's counter
				 * carried the silent grace into the asking and cut the
				 * watch after its first ONLINE request. */
				if (asking != 0 && idle >= 30 && asked >= 20) {
					break;
				}
				if ((t & 7) == 0) {
					get_mode[1] = (uint8_t)(0xc0 + (t & 0x0f) + 1);
					got_mode = mhi_qmi_call(out, in, 0x1001, dms_node,
					    dms_port, get_mode, sizeof(get_mode),
					    "IPCR DMS get operating mode (after the EFS sync)",
					    answer, sizeof(answer));
					if (got_mode != 0) {
						mode = mhi_dms_mode(answer, got_mode);
						online = mode == 0;
					}
				}
				/* The registration, while the modem finishes its scan.
				 * Run 125953: once the card's subscription stood, NAS
				 * answered error 3 (INTERNAL) where it had been 37
				 * (UIM_UNINITIALIZED) -- the card is not what still
				 * refuses.  Asked only after ONLINE, that change would
				 * never be seen; one round in sixteen, ask it, and log
				 * the answer when it changes. */
				if ((t & 15) == 8) {
					unsigned nas_node3, nas_port3;

					if (mhi_qrtr_service(0x03, &nas_node3, &nas_port3)) {
						uint8_t reg3[11] = {
							0x00, (uint8_t)(0xa0 + (t & 0x0f)), 0x00,
							0x22, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00,
							0x01
						};
						unsigned result3 = ~0u, error3 = ~0u;
						size_t got_reg3;

						got_reg3 = mhi_qmi_call(out, in, 0x1001,
						    nas_node3, nas_port3, reg3, sizeof(reg3),
						    "IPCR NAS initiate register (during the watch)",
						    answer, sizeof(answer));
						mhi_qmi_outcome(answer, got_reg3, &result3, &error3);
						if (result3 != last_result || error3 != last_nas_error) {
							last_result = result3;
							last_nas_error = error3;
							hakios_tl("pcie2: AMSS: NAS: the registration "
							    "during the watch: %s, result %u, error %u\n",
							    got_reg3 == 0 ? "not answered" : "answered",
							    result3, error3);
						}
					}
				}
				/* The card, while the modem finishes its boot.  The
				 * switch went out before the carrier-config scan and
				 * this EFS sync were done, and run 123149 shows the
				 * card still not described in logical slot 1 a minute
				 * on, the status never asked again.  One round in
				 * eight, ask it; when an application shows up, the
				 * provisioning goes out from here, and if the mapping
				 * has reverted the switch does.  The registration is
				 * asked again once the subscription stands. */
				if (uim_port != 0 && (t & 7) == 4 && !card_settled) {
					uint8_t card[7] = {
						0x00, (uint8_t)(0x80 + (t & 0x0f)), 0x00,
						0x2f, 0x00, 0x00, 0x00
					};
					size_t got_card;

					got_card = mhi_qmi_call(out, in, 0x1001, uim_node,
					    uim_port, card, sizeof(card),
					    "IPCR UIM card status (after the EFS sync)",
					    answer, sizeof(answer));
					if (got_card != 0 &&
					    mhi_uim_card_status(answer, got_card)) {
						if (mhi_uim_state.primary_gw != 0xffff) {
							card_settled = 1;
							hakios_tl("pcie2: AMSS: UIM: after the EFS sync the subscription stands on primary GW application %u (slot %u)\n",
							    mhi_uim_state.primary_gw & 0xff,
							    mhi_uim_state.primary_gw >> 8);
						} else if (mhi_uim_state.logical_aid_len != 0 &&
						    late_provisions < 3) {
							late_provisions++;
							(void)mhi_uim_provision(out, in,
							    uim_node, uim_port, 0x90, answer,
							    sizeof(answer));
							delay(2000000);
							card[1] = 0x91;
							got_card = mhi_qmi_call(out, in, 0x1001,
							    uim_node, uim_port, card, sizeof(card),
							    "IPCR UIM card status (after the late provisioning)",
							    answer, sizeof(answer));
							if (got_card != 0 &&
							    mhi_uim_card_status(answer, got_card) &&
							    mhi_uim_state.primary_gw != 0xffff) {
								unsigned nas_node2, nas_port2;

								card_settled = 1;
								hakios_tl("pcie2: AMSS: UIM: THE LATE PROVISIONING TOOK: primary GW application %u (slot %u)\n",
								    mhi_uim_state.primary_gw & 0xff,
								    mhi_uim_state.primary_gw >> 8);
								if (mhi_qrtr_service(0x03, &nas_node2, &nas_port2)) {
									uint8_t reg2[11] = {
										0x00, 0x92, 0x00, 0x22, 0x00, 0x04, 0x00,
										0x01, 0x01, 0x00, 0x01
									};

									(void)mhi_qmi_call(out, in, 0x1001,
									    nas_node2, nas_port2, reg2, sizeof(reg2),
									    "IPCR NAS initiate register (after the late provisioning)",
									    answer, sizeof(answer));
								}
							}
						} else if (mhi_uim_state.ready_slot > 1 &&
						    late_switches < 2) {
							late_switches++;
							hakios_tl("pcie2: AMSS: UIM: the card is back in slot %u and logical slot 1 does not see it; the switch goes again (%u)\n",
							    mhi_uim_state.ready_slot, late_switches);
							(void)mhi_uim_switch_slot(out, in, uim_node,
							    uim_port, (unsigned)(0x93 + late_switches),
							    mhi_uim_state.ready_slot);
						}
					}
				}
			}
			hakios_tl("pcie2: AMSS: DMS: after the EFS sync, the modem is %s after %u "
			    "ONLINE requests\n", online ? "ONLINE" : "still offline", asked);
			if (online) {
				unsigned nas_node, nas_port;

				if (mhi_qrtr_service(0x03, &nas_node, &nas_port)) {
					uint8_t reg[11] = {
						0x00, 0xc8, 0x00, 0x22, 0x00, 0x04, 0x00,
						0x01, 0x01, 0x00, 0x01
					};
					static const uint8_t info[7] = {
						0x00, 0xc9, 0x00, 0x4d, 0x00, 0x00, 0x00
					};

					(void)mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
					    reg, sizeof(reg),
					    "IPCR NAS initiate register (online)", answer,
					    sizeof(answer));
					mhi_qrtr_pump(out, in, 5000, "after registering online");
					(void)mhi_qmi_call(out, in, 0x1001, nas_node, nas_port,
					    info, sizeof(info), "IPCR NAS system info (online)",
					    answer, sizeof(answer));
				}
			}
		}
	}
	mhi_log_channel(21);
	mhi_log_channel(10);
	mhi_log_channel(11);
	mhi_amss_dump("after IPCR");
	hakios_tl("pcie2: AMSS: EFS: offered %d, acknowledged %d, %u regions, %llu bytes\n",
	    mhi_efs_offered, mhi_efs_done, mhi_efs_regions,
	    (unsigned long long)mhi_efs_bytes);
	return 0;
}

/*
 * The vendor's ready transition, replayed after the AMSS comes up: the three
 * context bases, the two address windows, MHICFG's ring counts, every event
 * ring's pointers in its context, their doorbells, and M0.  Run 025442 read
 * the registers first: CCABAP/ECABAP/CRCBAP still held the host's own values
 * and MHICFG still read the host's own 0x2060b80, so the AMSS had not
 * re-initialized the register side at all -- this replay changed nothing
 * there.  What the AMSS did do was write channel 14's context itself: state
 * 2 (RUNNING) after START_CHAN, and a read pointer of 0x100000001, which is
 * not an address the host ever put in, and which never moved.  The replay
 * stays because it is the vendor's own sequence and costs nothing; what the
 * device answered is in mhi_amss_dump().  The engine's reset was tried once
 * in the same run and the whole register block then read all ones, so the
 * AMSS does not survive a host reset and it is not written again.
 */
static int
mhi_amss_replay(void)
{
	uint32_t cfg;
	int error;

	wr(MHI_BAR + 0x58, (uint32_t)mhi_chctx_iova);
	wr(MHI_BAR + 0x5c, (uint32_t)(mhi_chctx_iova >> 32));
	wr(MHI_BAR + 0x60, (uint32_t)mhi_erctx_iova);
	wr(MHI_BAR + 0x64, (uint32_t)(mhi_erctx_iova >> 32));
	wr(MHI_BAR + 0x68, (uint32_t)mhi_cmdctx_iova);
	wr(MHI_BAR + 0x6c, (uint32_t)(mhi_cmdctx_iova >> 32));
	wr(MHI_BAR + 0x80, mhi_win_base);
	wr(MHI_BAR + 0x84, 0);
	wr(MHI_BAR + 0x88, mhi_win_stop);
	wr(MHI_BAR + 0x8c, 0);
	wr(MHI_BAR + 0x98, mhi_win_base);
	wr(MHI_BAR + 0x9c, 0);
	wr(MHI_BAR + 0xa0, mhi_win_stop);
	wr(MHI_BAR + 0xa4, 0);
	cfg = rd(MHI_BAR + 0x10);
	if (cfg == UINT32_MAX) {
		hakios_tl("pcie2: AMSS: replay: MHICFG does not read\n");
		return EIO;
	}
	wr(MHI_BAR + 0x10, (cfg & 0xffff) | (MHI_NER << 16) | (2u << 24));
	hakios_tl("pcie2: AMSS: replay: MHICFG %#x -> %#x; the bases %#llx/%#llx/%#llx\n",
	    cfg, (cfg & 0xffff) | (MHI_NER << 16) | (2u << 24),
	    (unsigned long long)mhi_chctx_iova, (unsigned long long)mhi_erctx_iova,
	    (unsigned long long)mhi_cmdctx_iova);
	mhi_hw_events_ready = false;
	for (unsigned e = 0; e < MHI_NER; e++) {
		struct mhi_event_ring *ring = &mhi_events[e];
		volatile uint32_t *c = mhi_erctx + e * 11;

		ring->consumer = 0;
		ring->wp = e < 4 ? ring->count - 1 : 0;
		mhi_set64(c, 3, ring->base);
		mhi_set64(c, 5, (uint64_t)ring->count * 16);
		mhi_set64(c, 7, ring->base);
		mhi_set64(c, 9, ring->base + (uint64_t)ring->wp * 16);
	}
	mhi_publish();
	for (unsigned e = 0; e < 4; e++) {
		mhi_doorbell(mhi_erdb + e * 8, mhi_events[e].base + (uint64_t)mhi_events[e].wp * 16);
	}
	error = update(MHI_BAR + 0x38, 0xff00, 0x200);
	if (error) {
		hakios_tl("pcie2: AMSS: replay: MHICTRL does not read (%d)\n", error);
		return error;
	}
	error = wait_bits(MHI_BAR + 0x48, 0xff04, 0x200, 20000);
	hakios_tl("pcie2: AMSS: replay done: MHISTATUS %#x, bases read back %#x/%#x/%#x%s\n",
	    rd(MHI_BAR + 0x48), rd(MHI_BAR + 0x58), rd(MHI_BAR + 0x60), rd(MHI_BAR + 0x68),
	    error ? " (M0 not reached)" : "");
	return error;
}

/* The QMI host sequence: version, sync, a UIM client, and the card status. */
static int
mhi_qmi_sequence(void)
{
	static const uint8_t get_version[12] = {
		0x01, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x21, 0x00, 0x00, 0x00
	};
	static const uint8_t sync[12] = {
		0x01, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x27, 0x00, 0x00, 0x00
	};
	static const uint8_t allocate[16] = {
		0x01, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x22, 0x00, 0x04, 0x00,
		0x01, 0x01, 0x00, 0x0b		/* TLV 1: service UIM */
	};
	int error = mhi_qmi_roundtrip(get_version, sizeof(get_version), "CTL GET_VERSION", 3);

	if (error) {
		return error;
	}
	error = mhi_qmi_roundtrip(sync, sizeof(sync), "CTL SYNC", 1);
	if (error) {
		return error;
	}
	error = mhi_qmi_roundtrip(allocate, sizeof(allocate), "CTL ALLOCATE_CID UIM", 1);
	if (error) {
		return error;
	}
	{
		unsigned cid = 0, off = 12;

		while (off + 3 <= mhi_qmi_answer_length) {
			unsigned type = mhi_qmi_answer[off];
			unsigned len = mhi_qmi_answer[off + 1] |
			    ((unsigned)mhi_qmi_answer[off + 2] << 8);

			if (off + 3 + len > mhi_qmi_answer_length) {
				break;
			}
			if (type == 0x01 && len >= 2) {
				cid = mhi_qmi_answer[off + 4];
				break;
			}
			off += 3 + len;
		}
		hakios_tl("pcie2: AMSS: UIM client id %u\n", cid);
		if (cid == 0) {
			return ETIMEDOUT;
		}
		{
			uint8_t card_status[13] = {
				0x01, 0x0c, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x04, 0x00, 0x2f, 0x00,
				0x00, 0x00
			};

			card_status[5] = (uint8_t)cid;
			error = mhi_qmi_roundtrip(card_status, sizeof(card_status),
			    "UIM GET_CARD_STATUS", 2);
		}
	}
	return error;
}

/*
 * The AMSS's MHI: after BHI_EXECENV 2 the modem's own firmware owns the
 * engine, and the host's next move is what Android's host does next -- open
 * the channels the modem's services sit on.  Which channels those are comes
 * from the vendor's own table (kona-mhi.dtsi): SAHARA is 2/3 and belongs to
 * the SBL (mhi,ee 0x2), while the AMSS's channels are QMI0 on 14 (out, event
 * ring 1) and 15 (in, event ring 2), QMI1 on 16/17, IPCR on 20/21 (the QRTR
 * packets the kernel's ipc router carries).  Android's
 * /dev/mhi_0306_02.01.00_pipe_14 is owned by radio, so userspace QMI rides
 * QMI0 -- that pair is the first to open here.  Run 021924 sent a bare QMI
 * header to the SBL's SAHARA pair, where after the handover no client
 * listens; 022525's retries never moved that ring.  The sequence that follows
 * is the one a QMI host runs: GET_VERSION_INFO, SYNC, then a client id for
 * UIM and the card-status request -- the first question that is about the SIM
 * itself.  Everything is announced before it is attempted; the loader's log
 * keeps only its newest megabyte.
 */
static void
mhi_amss_start(void)
{
	uint32_t cfg = rd(MHI_BAR + 0x10), chdb = rd(MHI_BAR + 0x18), erdb = rd(MHI_BAR + 0x20);
	uint32_t ctrl = rd(MHI_BAR + 0x38), status = rd(MHI_BAR + 0x48);
	int error;

	mhi_stage = "AMSS-MHI";
	hakios_tl("pcie2: AMSS: the handover's registers: MHICFG %#x, CHDBOFF %#x, ERDBOFF %#x, "
	    "MHICTRL %#x, MHISTATUS %#x\n", cfg, chdb, erdb, ctrl, status);
	if (((status >> 8) & 0xff) != 2) {
		hakios_tl("pcie2: AMSS: not M0 (state %u); requesting it\n", (status >> 8) & 0xff);
		wr(MHI_BAR + 0x38, (ctrl & ~0xff00u) | 0x200);
		for (unsigned ms = 0; ms < 5000; ms++) {
			status = rd(MHI_BAR + 0x48);
			if (((status >> 8) & 0xff) == 2) {
				break;
			}
			delay(1000);
		}
		hakios_tl("pcie2: AMSS: after the M0 request: MHISTATUS %#x\n", status);
	}
	if (chdb >= 0xa8 && chdb < 0x1000 - 1024 && (chdb & 7) == 0 &&
	    erdb >= 0xa8 && erdb < 0x1000 - MHI_NER * 8 && (erdb & 7) == 0 &&
	    (chdb != mhi_chdb || erdb != mhi_erdb)) {
		hakios_tl("pcie2: AMSS: the doorbells moved: CHDB %#x -> %#x, ERDB %#x -> %#x\n",
		    mhi_chdb, chdb, mhi_erdb, erdb);
		mhi_chdb = chdb;
		mhi_erdb = erdb;
	}
	/* The AMSS left the engine configured for itself: its MHICFG (six event
	 * rings, its own channel counts) and a state already past the READY
	 * window the vendor's host programs in.  The registers the host owns are
	 * written back first and read into the log; the channels are then started
	 * and their contexts read back, so the ring is only rung after the log
	 * has said what the device should see. */
	hakios_tl("pcie2: AMSS: the AMSS left: CCABAP %#x/%#x, ECABAP %#x/%#x, CRCBAP %#x/%#x, "
	    "CTRL %#x/%#x..%#x/%#x, DATA %#x/%#x..%#x/%#x\n",
	    rd(MHI_BAR + 0x58), rd(MHI_BAR + 0x5c), rd(MHI_BAR + 0x60), rd(MHI_BAR + 0x64),
	    rd(MHI_BAR + 0x68), rd(MHI_BAR + 0x6c), rd(MHI_BAR + 0x80), rd(MHI_BAR + 0x84),
	    rd(MHI_BAR + 0x88), rd(MHI_BAR + 0x8c), rd(MHI_BAR + 0x98), rd(MHI_BAR + 0x9c),
	    rd(MHI_BAR + 0xa0), rd(MHI_BAR + 0xa4));
	error = mhi_amss_replay();
	if (error == 0) {
		int qmi_error = EOPNOTSUPP;

		if (pcie2_mhi_mode != 2) {
			qmi_error = mhi_start_channel(2, 14, 1);
			hakios_tl("pcie2: AMSS: channel 14 (QMI0 out) %s\n",
			    qmi_error == 0 ? "open" : "did not open");
			if (qmi_error == 0) {
				mhi_log_channel(14);
				qmi_error = mhi_start_channel(3, 15, 2);
				hakios_tl("pcie2: AMSS: channel 15 (QMI0 in) %s\n",
				    qmi_error == 0 ? "open" : "did not open");
			}
		}
		/* The QRTR pair first.  Stock's host questions DMS 2.2 s after
		 * the AMSS takes over and changes its mode while the modem is
		 * still SHUTTING_DOWN.  This firmware never answers QMI0 (run
		 * 030228: all three CTL GET_VERSION tries left the TX ring's
		 * read pointer at 0x100000001, each costing 5 s of transfer
		 * wait plus 5 s of answer wait), and asking it first pushed
		 * the first DMS contact 55 s past Sahara -- into OFFLINE,
		 * where LOW POWER is refused 60 and ONLINE 52.  IPCR is where
		 * this modem speaks; it goes first, and mhi_amss_ipcr runs the
		 * whole QRTR bring-up: hello, announcements, the first DMS
		 * contact, UIM, NAS, EFS.  The QMI0 pair's own state must not
		 * gate it. */
		error = mhi_amss_ipcr();
		if (error == 0 && pcie2_mhi_mode != 2) {
			/* The modem is the system's now: NAS is polled from
			 * here on, and kern.hakios.cellular_* stays current
			 * for the UI. */
			mhi_cellular_start();
		}
		if (error != 0) {
			/* Run 025442: the engine's reset was written once and
			 * the whole register block then read all ones -- the
			 * AMSS does not survive a host reset, so it is not
			 * tried again.  What the device's own memory says is
			 * logged instead, and QMI0 -- the pair the first
			 * design used -- is the fallback for a QRTR pair that
			 * could not itself be opened. */
			mhi_amss_dump("after the IPCR pair failed");
			if (qmi_error == 0) {
				error = mhi_qmi_sequence();
				if (error != 0) {
					mhi_amss_dump("after the QMI tries");
				}
			}
		}
	}
	hakios_tl("pcie2: AMSS: the stage ends: MHISTATUS %#x, BHI_EXECENV %#x\n",
	    rd(MHI_BAR + 0x48), rd(mhi_bhi + 0x28));
}

/*
 * Which environment answers after a relink, and for how long: BHI_EXECENV
 * and MHISTATUS sampled every 200 ms with only the transitions logged. The
 * far side may be the PBL waiting for an image (EE 0) or the modem's own
 * engine coming up after the handover (EE 2); it may also leave the bus
 * again mid-window, and all ones is a state like any other here. Returns
 * the last environment read; 2 is the AMSS.
 */
static uint32_t
mhi_execenv_watch(unsigned window_ms)
{
	uint32_t execenv = rd(mhi_bhi + 0x28), status = rd(MHI_BAR + 0x48);

	hakios_tl("pcie2: MHI: the relink's endpoint: BHI_EXECENV %#x, MHISTATUS %#x\n", execenv, status);
	for (unsigned ms = 0; ms < window_ms; ms += 200) {
		uint32_t next_execenv, next_status;

		delay(200000);
		next_execenv = rd(mhi_bhi + 0x28);
		next_status = rd(MHI_BAR + 0x48);
		if (next_execenv != execenv || next_status != status) {
			hakios_tl("pcie2: MHI: the relink's endpoint at %u ms: BHI_EXECENV %#x, MHISTATUS %#x\n",
			    ms + 200, next_execenv, next_status);
			execenv = next_execenv;
			status = next_status;
		}
		if (execenv == 2) {
			break;
		}
	}
	return execenv;
}

/*
 * Run 232234: the prompt warm restart asserted PERST while the modem was
 * still holding MDM2AP_STATUS high -- its own reset had not finished -- and
 * the endpoint never trained (LTSSM 0x1 for 3 s). The one warm restart that
 * did train (run 230144) ran when the line was already low. So before a
 * restart, give the line the time it takes to fall, up to the window, and
 * log when it does. Returns the last MDM2AP_STATUS read; 0 is the modem's
 * own reset done.
 */
static uint32_t
mhi_status_settle(unsigned window_ms)
{
	uint32_t status = pcie2_pin_in(3);

	hakios_tl("pcie2: MHI: the modem's own reset: MDM2AP_STATUS %u, ERRFATAL %u\n",
	    status, pcie2_pin_in(1));
	for (unsigned ms = 0; ms < window_ms && status != 0; ms += 100) {
		uint32_t next;

		delay(100000);
		next = pcie2_pin_in(3);
		if (next != status) {
			hakios_tl("pcie2: MHI: MDM2AP_STATUS %u at %u ms, ERRFATAL %u\n",
			    next, ms + 100, pcie2_pin_in(1));
			status = next;
		}
	}
	return status;
}

static void pcie2_bhi_try(void);
static void pcie2_pin_output(unsigned pin, unsigned high);
static void pcie2_pin_input(unsigned pin);

/*
 * Run 224136: after the SBL's last image its endpoint leaves the bus -- the
 * Sahara wait's own fault check caught the bus dying about 17 s after the
 * last DONE (the SBL quiet all that time), and the watch then saw
 * MDM2AP_STATUS fall 1.8 s later. Run 230144, the first build to relink,
 * found a PBL whose BHI refused sbl1 (result 3, errcode 0xef120700) -- but
 * that relink ran ~80 s after the list, long past anything the modem waited
 * for. The vendor treats a dead link as the start of a full restart
 * (umi_modem.c's md_bringup: md_link_reset, then md_pcie from PERST and
 * AP_STATUS low through the rails, the clocks and a fresh train) and expects
 * the PBL (BHI_EXECENV 0) on the far side of it before it reloads the
 * images. UMI's DT has no soft-reset GPIO and its md_pcie writes no PMIC
 * PON, so the vendor's shape is the lines low, PERST asserted, the rails,
 * the clocks, AP2MDM_STATUS high, the link.
 *
 * `cold` adds the other restart shape: the PMIC's modem reset at the point
 * the cold boot pulses it (pcie2_run's bhi >= 2 path), because run 230144's
 * warm PBL refused the very image the cold boot's PBL accepts.
 *
 * Returns 0 with the link up and the endpoint's config space answering; the
 * caller reads BHI_EXECENV to see which environment the modem came back in.
 */
static int
mhi_relink(bool cold)
{
	uint32_t id;

	pcie2_pin_output(56, 0); /* AP2MDM_STATUS low: the restart's first state */
	pcie2_pin_output(57, 0);
	pcie2_pin_input(1);
	pcie2_pin_input(3);
	if (cold) {
		/* The cold boot's own order: the pulse after the lines are low,
		 * a 150 ms pause, then AP2MDM_STATUS high, the rails, the clocks. */
		hakios_tl("pcie2: MHI: relink: the PMIC's modem warm reset, as the cold boot: %d\n",
		    hakios_pmic_pon_modem_reset());
		delay(150000);
		pcie2_pin_output(56, 1);
		delay(10000);
	} else {
		hakios_tl("pcie2: MHI: relink: the endpoint left the bus; the vendor's restart follows\n");
		pcie2_perst(0);
		delay(150000); /* the vendor's pause after the power handshake is pulled */
	}
	if (umi_rpmh_wifi_power() != 0) {
		hakios_tl("pcie2: MHI: relink: the rails did not come back\n");
		return EIO;
	}
	delay(10000);
	if (!cold) {
		pcie2_pin_output(56, 1);
		delay(10000);
	}
	hakios_tl("pcie2: MHI: relink: AP2MDM_STATUS high, MDM2AP_STATUS %u; the clocks and the link follow\n",
	    pcie2_pin_in(3));
	if (clocks() != 0) {
		hakios_tl("pcie2: MHI: relink: the clocks failed\n");
		return EIO;
	}
	if (link_train() != 0) {
		hakios_tl("pcie2: MHI: relink: no link\n");
		return EIO;
	}
	iatu_config(0, 0x04, CONF, CONF + 0xfff, 1u << 24);
	id = rd(CONF);
	hakios_tl("pcie2: MHI: relink: the link is up, endpoint id %#x\n", id);
	if (id == 0xffffffffu || id == UINT32_MAX) {
		return ENODEV;
	}
	/* BAR0 may be unassigned again after the endpoint's reset: give it the
	 * phone's own address and re-point the window before any MHI read. */
	mhi_peek();
	return 0;
}

/*
 * The whole stage after BHI's result 2, entered with MHISTATUS.READY set and
 * the SBL in execution environment 1. pcie2_mhi=1 turns it on; every step
 * logs where it stands, and the last line says what BHI_EXECENV became --
 * 2 is the AMSS running, and nothing less is claimed.
 */
static void
pcie2_mhi_sahara(uint32_t bhi)
{
	int error;
	uint32_t execenv;

	mhi_round++;
	mhi_sahara_images = 0; /* per round: the watch says how many this one served */
	mhi_bhi = bhi;
	hakios_tl("pcie2: MHI: the SBL is in its execution environment; setting up the engine\n");
	error = mhi_pool_setup();
	if (error == 0) {
		error = mhi_contexts();
	}
	if (error == 0) {
		error = mhi_sahara();
	}
	if (mhi_sahara_images) {
		uint32_t id = rd(CONF), link = rd(DBI + 0x80) & 0x20000000u;

		/* After its last image the SBL goes quiet and the bus follows it
		 * down seconds later (run 224136: ~17 s of quiet, then the fault
		 * check caught all ones, and MDM2AP_STATUS fell 1.8 s after that).
		 * While the link is still up the old watch says what the endpoint
		 * does; once it is gone the restart below waits for the modem's
		 * own reset to finish (run 232234: PERST before that does not
		 * train). */
		if (rd(mhi_bhi + 0x28) == 2 || (id != UINT32_MAX && link != 0)) {
			mhi_amss_watch();
		} else {
			hakios_tl("pcie2: MHI: the endpoint left with the list; the restart waits for the modem's own reset\n");
		}
	}
	execenv = rd(mhi_bhi + 0x28);
	/*
	 * Run 224136: after the complete image list the endpoint leaves the bus
	 * (link down, config space all ones). The vendor's model for that is its
	 * restart loop; one restart, then one more image list -- never a loop,
	 * so a fault cannot cost more than two rounds.
	 */
	if (execenv != 2 && mhi_round == 1 && (rd(CONF) == 0xffffffffu || (rd(DBI + 0x80) & 0x20000000u) == 0)) {
		uint32_t modem_status = mhi_status_settle(20000);
		bool linked;

		/*
		 * Run 230144: the warm restart (PERST, rails, clocks, train) came
		 * back with a PBL in EE 0 whose BHI then refused sbl1 (result 3,
		 * errcode 0xef120700) -- but that restart ran ~80 s after the list.
		 * Run 232234: run at once instead, it asserted PERST while the
		 * modem still held MDM2AP_STATUS high, and the endpoint never
		 * trained (ELBI 0x1, LTSSM 0x1 for 3 s). So the warm restart waits
		 * for the line to fall first, and a modem that never lowers it gets
		 * the cold restart -- the one sequence that has always brought the
		 * link up, and the one that returns the endpoint to the PBL.
		 */
		if (modem_status == 0) {
			linked = mhi_relink(false) == 0;
		} else {
			hakios_tl("pcie2: MHI: the status never fell; the cold restart is the only restart left\n");
			linked = false;
		}
		if (!linked) {
			hakios_tl("pcie2: MHI: no warm link; the cold restart follows\n");
			linked = mhi_relink(true) == 0;
		}
		if (linked) {
			uint32_t bhioff = rd(MHI_BAR + 0x28), bhi_now = MHI_BAR + bhioff;

			mhi_bhi = bhi_now; /* the block's offset may have moved */
			/*
			 * The environment the endpoint comes back in is the answer to
			 * what the modem waits for: EE 2 is the modem's own engine
			 * after the handover; an endpoint that stays in the PBL gets
			 * another cold restart, the state the cold boot's BHI load is
			 * known to accept, and then the image list runs once more.
			 */
			execenv = mhi_execenv_watch(15000);
			if (execenv == 0) {
				hakios_tl("pcie2: MHI: the endpoint stayed in the PBL; the cold restart follows\n");
				if (mhi_relink(true) == 0) {
					bhioff = rd(MHI_BAR + 0x28);
					mhi_bhi = MHI_BAR + bhioff;
					execenv = mhi_execenv_watch(2000);
				} else {
					execenv = 0xffffffffu;
				}
			}
			if (execenv == 0) {
				uint32_t ready;

				/* Run 225537: the PBL's engine takes the doorbell before it
				 * ever sets MHISTATUS.READY -- the cold boot loaded sbl1
				 * with MHISTATUS 0 (`BHI trace: 0 ms`, `:2543`) and only the
				 * SBL left 0x101 behind. After the relink the endpoint is the
				 * PBL's id 0x30617cb with MHISTATUS 0, the cold boot's exact
				 * pre-load state, so the gate here is BHI_EXECENV 0, not the
				 * READY bit. */
				ready = rd(MHI_BAR + 0x48) & 1;
				bhioff = rd(MHI_BAR + 0x28);
				bhi_now = MHI_BAR + bhioff;
				hakios_tl("pcie2: MHI: after the relink: BHI_EXECENV %#x, MHISTATUS %#x (ready %u), "
				    "BHI_STATUS %#x, BHIEOFF %#x\n", execenv, rd(MHI_BAR + 0x48), ready,
				    rd(bhi_now + 0x2c), bhioff);
				hakios_tl("pcie2: MHI: the PBL is back; the image list runs a second time\n");
				hakios_tl("pcie2: MHI: ---- second round ----\n");
				pcie2_bhi_try();
				execenv = rd(mhi_bhi + 0x28); /* the nested round's end state */
			} else if (execenv == 2) {
				hakios_tl("pcie2: MHI: the AMSS has taken BHI over after the relink (BHI_EXECENV 2)\n");
			}
		}
	}
	hakios_tl("pcie2: MHI: %s ends with %d: BHI_EXECENV %#x, MHISTATUS %#x, BHI_STATUS %#x\n", mhi_stage,
	    error, execenv, rd(MHI_BAR + 0x48), rd(mhi_bhi + 0x2c));
	modem_context_fsr(false);
	if (execenv == 2) {
		hakios_tl("pcie2: MHI: the AMSS has taken BHI over (BHI_EXECENV 2)\n");
		mhi_amss_start();
	}
}

/*
 * The PBL's boot interface, the vendor's mhi_fw_load_sbl: the device sits in
 * MHI execution environment PBL (BHI_EXECENV 0) with version 1.2, waiting for
 * sdx55m/sbl1.mbn. The host takes the MHI core through RESET and M0
 * (mhi_set_mhi_state, mhi_ready_state_transition), programs the window the
 * device may read (MHICTRL and MHIDATA base/limit), hands the PBL the image's
 * physical address and size, and rings BHI_IMGTXDB; BHI_STATUS's top two bits
 * become 2 on success, 3 on error. Gated by pcie2_bhi=1: it writes to the
 * modem, which the read-only runs do not.
 */
static void
pcie2_bhi_try(void)
{
	uint32_t on = 0, bad = 0;
	size_t size = 0;
	vm_offset_t memory = 0;
	uint64_t phys;
	uint32_t bhioff, bhi, status = 0, mhistatus = 0, i, eetrace = 0, traced = 0;

	if (!PE_parse_boot_argn("pcie2_bhi", &on, sizeof(on)) || on < 1) {
		return;
	}
	/* The 32 MB io_submap every driver shares: an ml_io_map with none of it
	 * left blocks in kmem_alloc on the kernel map's lock and takes the boot
	 * with it (two probe runs ended that way). The number says how close the
	 * boot is before this driver maps anything. */
	hakios_tl("pcie2: io submap free before the probe: %llu bytes\n", (unsigned long long)umi_camera_iomap_free());
	/* A deliberately wrong image needs no file and no userland: send zeroed
	 * bytes from the SLPI heap. If the refusal's error code is the same as
	 * the real image's, the PBL rejects the transfer before it can be about
	 * the image's content; if it changes, the content is being read. */
	PE_parse_boot_argn("pcie2_bhi_bad", &bad, sizeof(bad));
	if (bad == 1 || bad == 3) {
		memory = ml_io_map(MODEM_IMAGE_PA, 0x1000);
		if (memory == 0) {
			hakios_tl("pcie2: BHI: cannot map the bad-image window\n");
			return;
		}
		for (unsigned j = 0; j < 0x400; j++) {
			((volatile uint32_t *)memory)[j] = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		size = bad == 1 ? 0x1000 : 4;
		phys = MODEM_IMAGE_PA;
		hakios_tl("pcie2: BHI: bad image mode %u: %zu zero bytes at %#llx, the file not read\n", bad, size,
		    (unsigned long long)phys);
		goto image_ready;
	}
	/*
	 * Where the image lives for the PBL. pcie2_img=1 (the default) puts it
	 * in the SLPI's reserved heap at 0xc0000000 -- below 4 GB, reserved
	 * away from the kernel and unused when the sensor DSP is not started,
	 * ml_io_map'ed so it is uncached. pcie2_img=2 puts it in ordinary
	 * kernel memory (physically contiguous, anywhere): the heap is a
	 * carveout the modem's SID may not be allowed to read, while the
	 * kernel's own pages are what the MHI host uses on Android. The heap
	 * read is byte-perfect (word sum 0x92f1f39) and the SMMU records no
	 * fault, so the PBL's error is something between the SMMU and memory.
	 */
	{
		uint32_t iova = 0;

		/* pcie2_iova=N: where in the DT's pool the modem finds the image.
		 * The stock kernel's first 1 MB holds the MHI context rings and its
		 * dma_alloc_coherent buffer lands at 0x20100000; every probe so far
		 * put the image at the pool's base instead. */
		if (PE_parse_boot_argn("pcie2_iova", &iova, sizeof(iova)) && iova != 0) {
			smmu_iova = iova;
			hakios_tl("pcie2: BHI: the probe's image IOVA %#x, not %#llx\n", iova,
			    (unsigned long long)SMMU_IOVA);
		}
	}
	{
		uint32_t img = 1;

		PE_parse_boot_argn("pcie2_img", &img, sizeof(img));
		if (img == 5) {
			/* the image copied straight into a physical address inside the MHI
			 * window, through a write-combining mapping: 0x20000000 by
			 * default, or pcie2_addr if the probe names another (the window's
			 * base answered reads, but takes no writes from the AP). */
			uint32_t pool = 0x20000000u;

			PE_parse_boot_argn("pcie2_addr", &pool, sizeof(pool));
			memory = ml_io_map_wcomb(pool, MODEM_IMAGE_MAP);
			if (memory == 0) {
				hakios_tl("pcie2: BHI: cannot map %#x\n", pool);
				return;
			}
			hakios_tl("pcie2: BHI: %#x mapped at %#x; loading the file into it\n", pool, (uint32_t)memory);
			for (i = 0; i < 30; i++) {
				if (root_file_into("modem/sbl1.mbn", memory, MODEM_IMAGE_MAP, &size) == 0) {
					break;
				}
				delay(1000000);
			}
			if (i == 30) {
				hakios_tl("pcie2: BHI: no /redwood/fw/modem/sbl1.mbn after 30 s\n");
				return;
			}
			phys = pool;
			__asm__ volatile("dsb sy" ::: "memory");
			hakios_tl("pcie2: BHI: the image copied into %#x, %zu bytes\n", pool, size);
			goto image_ready;
		}
		if (img == 4) {
			/* the kernel's own BSS, below 4 GB with the kernel image */
			memory = (vm_offset_t)modem_image_buf;
			for (i = 0; i < 30; i++) {
				if (root_file_into("modem/sbl1.mbn", memory, sizeof(modem_image_buf), &size) == 0) {
					break;
				}
				delay(1000000);
			}
			if (i == 30) {
				hakios_tl("pcie2: BHI: no /redwood/fw/modem/sbl1.mbn after 30 s\n");
				return;
			}
			phys = kvtophys(memory);
			flush_dcache(memory, (unsigned)size, 0);
			hakios_tl("pcie2: BHI: the kernel's own buffer at %#x, physical %#llx\n", (uint32_t)memory,
			    (unsigned long long)phys);
			goto image_ready;
		}
		if (img == 2 || img == 3) {
			vm_size_t bytes = 0x100000; /* 1 MB; sbl1.mbn is 548,056 */
			/* img=3 asks for the allocation below 4 GB: the PBL's reads
			 * have turned out to be physical and to deliver only in the DRAM
			 * this master may read -- the heap at 0xc0000000 is not, and the
			 * flexible allocation lands above 4 GB. 0x40000 pages of 16 KB
			 * is 4 GB, and max_pnum is exclusive. */
			ppnum_t max_pnum = (img == 3) ? 0x40000 : 0;

			if (kmem_alloc_contig(kernel_map, &memory, bytes, PAGE_MASK, max_pnum, 0,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
				for (i = 0; i < 30; i++) {
					if (root_file_into("modem/sbl1.mbn", memory, bytes, &size) == 0) {
						break;
					}
					delay(1000000);
				}
				if (i == 30) {
					hakios_tl("pcie2: BHI: no /redwood/fw/modem/sbl1.mbn after 30 s\n");
					return;
				}
				phys = kvtophys(memory);
				flush_dcache(memory, (unsigned)size, 0);
				goto image_ready;
			}
			hakios_tl("pcie2: BHI: no 1 MB of contiguous kernel memory; the SLPI heap instead\n");
		}
		memory = ml_io_map(MODEM_IMAGE_PA, MODEM_IMAGE_MAP);
		if (memory == 0) {
			hakios_tl("pcie2: BHI: cannot map the image window\n");
			return;
		}
		for (i = 0; i < 30; i++) {
			if (root_file_into("modem/sbl1.mbn", memory, MODEM_IMAGE_MAP, &size) == 0) {
				break;
			}
			delay(1000000);
		}
		if (i == 30) {
			hakios_tl("pcie2: BHI: no /redwood/fw/modem/sbl1.mbn after 30 s\n");
			return;
		}
		phys = MODEM_IMAGE_PA;
	}
image_ready:
	{
		volatile uint32_t *words = (volatile uint32_t *)memory;
		uint32_t sum = 0;

		for (size_t j = 0; j + 4 <= size; j += 4) {
			sum += words[j / 4];
		}
		hakios_tl("pcie2: BHI: sbl1.mbn %zu bytes at %#x, physical %#llx, first words %#x %#x, word sum %#x\n",
		    size, (uint32_t)memory, (unsigned long long)phys, words[0], words[1], sum);
	}

	/* pcie2_bhi_bad=2: zero the real image in place, to keep the file's
	 * address and size while its content is wrong. 1 and 3 were handled
	 * above, without the file. */
	if (bad == 2) {
		volatile uint32_t *words = (volatile uint32_t *)memory;

		for (size_t j = 0; j < size / 4; j++) {
			words[j] = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		hakios_tl("pcie2: BHI: bad image mode 2: the whole %zu-byte image zeroed\n", size);
	}
	if (bad == 4 && size > 0x8000) {
		/* one byte flipped deep inside the real image: whether the PBL's
		 * verdict changes says whether its check covers the image's body
		 * (signature) or only looks at the header */
		volatile uint8_t *bytes = (volatile uint8_t *)memory;
		uint8_t old = bytes[0x8000];

		bytes[0x8000] = (uint8_t)(old ^ 0x5a);
		__asm__ volatile("dsb sy" ::: "memory");
		hakios_tl("pcie2: BHI: bad image mode 4: byte 0x8000 %#x -> %#x\n", old, bytes[0x8000]);
	}

	bhioff = rd(MHI_BAR + 0x28);
	bhi = MHI_BAR + bhioff;

	/*
	 * The vendor's mhi_init_mmio writes the three MHI context bases before
	 * the SBL is loaded (CCABAP/ECABAP/CRCBAP, 0x58-0x6c). They are not
	 * used until MHI is up, but they are part of the state Android leaves
	 * behind; the image's own IOVA stands in for all three until there are
	 * real context structures.
	 *
	 * The MHICTRL reset handshake (mhi_set_mhi_state(MHI_STATE_RESET)) is
	 * NOT part of the vendor's pre-load path -- its SBL load never touches
	 * MHICTRL -- and this driver's write left the reset bit set (the device
	 * never cleared it), a state Android never has when the doorbell rings.
	 * It is off by default now and pcie2_reset=1 asks for it.
	 */
	{
		/* pcie2_mhiraw=1: leave the MHI registers exactly as Android does
		 * before the load. The vendor writes these only after the SBL has
		 * run, so the PBL may well see them as foreign state. */
		uint32_t mhiraw = 0;

		PE_parse_boot_argn("pcie2_mhiraw", &mhiraw, sizeof(mhiraw));
		if (mhiraw == 0) {
			wr(MHI_BAR + 0x58, (uint32_t)(phys & ~0xfffu));
			wr(MHI_BAR + 0x5c, (uint32_t)(phys >> 32));
			wr(MHI_BAR + 0x60, (uint32_t)(phys & ~0xfffu));
			wr(MHI_BAR + 0x64, (uint32_t)(phys >> 32));
			wr(MHI_BAR + 0x68, (uint32_t)(phys & ~0xfffu));
			wr(MHI_BAR + 0x6c, (uint32_t)(phys >> 32));
		} else {
			hakios_tl("pcie2: MHI: the context bases are left as they were (pcie2_mhiraw=1)\n");
		}
	}
	{
		uint32_t reset = 0, control = rd(MHI_BAR + 0x38);

		PE_parse_boot_argn("pcie2_reset", &reset, sizeof(reset));
		if (reset) {
			if (!(control & 0x2)) {
				wr(MHI_BAR + 0x38, control | 0x2);
				for (i = 0; i < 100 && (rd(MHI_BAR + 0x38) & 0x2); i++) {
					delay(1000);
				}
				hakios_tl("pcie2: MHI: reset bit %s (control %#x, status %#x)\n",
				    (rd(MHI_BAR + 0x38) & 0x2) ? "did not clear" : "cleared", rd(MHI_BAR + 0x38),
				    rd(MHI_BAR + 0x48));
			} else {
				hakios_tl("pcie2: MHI: already in reset (control %#x)\n", control);
			}
		} else if (control & 0x2) {
			/* an earlier run's reset write may still be sitting there */
			hakios_tl("pcie2: MHI: control %#x still has the reset bit; writing 0\n", control);
			wr(MHI_BAR + 0x38, 0);
		}
	}

	/* the vendor's mhi_async_power_up writes the boot interrupt vector
	 * before anything else; without it the PBL ignores the doorbell. */
	wr(bhi + 0x20, 0);          /* BHI_INTVEC */
	hakios_tl("pcie2: BHI: writes land? INTVEC %#x, before: imgaddr %#x/%#x size %#x\n", rd(bhi + 0x20),
	    rd(bhi + 0x0c), rd(bhi + 0x08), rd(bhi + 0x10));
	/*
	 * The PBL's identity and trust registers (mhi_internal.h 0x40-0x80): the
	 * serial number, the anti-rollback revision, the segment count, the four
	 * MSM hardware-ID words and the OEM public-key hash.  An all-zero hash is
	 * an unfused (open) part and would let any image through; a fused one is
	 * the root the image's signer has to match, and its words say whether
	 * that is even possible for the sbl1.mbn this phone carries.
	 */
	hakios_tl("pcie2: BHI: serial %#x, antirollback %#x, numseg %#x, HWID %#x %#x %#x %#x\n", rd(bhi + 0x40),
	    rd(bhi + 0x44), rd(bhi + 0x48), rd(bhi + 0x4c), rd(bhi + 0x50), rd(bhi + 0x54), rd(bhi + 0x58));
	hakios_tl("pcie2: BHI: OEM PK hash %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(bhi + 0x64), rd(bhi + 0x68),
	    rd(bhi + 0x6c), rd(bhi + 0x70), rd(bhi + 0x74), rd(bhi + 0x78), rd(bhi + 0x7c), rd(bhi + 0x80));

	/* the endpoint as a bus master the SMMU lets through (its own boot arg:
	 * the first attempt at it took the phone down with no log at all, so it
	 * is switched on by pcie2_sid=1 while that is worked out; 5 is the
	 * Android path, a stage-1 context with our tables and an IOVA) */
	{
		uint32_t sid = 0;

		PE_parse_boot_argn("pcie2_sid", &sid, sizeof(sid));
		if (sid == 5) {
			uint64_t iova;

			if (mhi_sid_ready && phys == mhi_sid_pbase) {
				/* The second round after a relink: the same image at the
				 * same physical base is already behind the context whose
				 * stream the endpoint's SID matched. Mapping it again
				 * would take a second context for the same SID, and the
				 * match register's first entry would still win. */
				phys = mhi_sid_iova;
				hakios_tl("pcie2: BHI: the image keeps its IOVA %#llx for the second round\n",
				    (unsigned long long)phys);
			} else {
				mhi_sid_pbase = phys;       /* what the second round's reuse test compares */
				modem_sid_setup(2);         /* the BDF-to-SID table only */
				iova = modem_smmu_map(memory, size);
				if (iova != 0) {
					phys = iova;
				} else {
					hakios_tl("pcie2: BHI: no SMMU context; the physical address stays\n");
				}
				/* pcie2_msi=1: the vendor's md_enable_dma order -- the GIC
				 * MBI page in the same context, then the endpoint's MSI.
				 * The page is identity-mapped with the vendor's own
				 * descriptor 0x0060000000000e4b: UXN/PXN set, AttrIdx 2
				 * (MAIR0's 0x04, Device-nGnRE) -- the device writes it. */
				PE_parse_boot_argn("pcie2_msi", &pcie2_msi, sizeof(pcie2_msi));
				if (pcie2_msi) {
					if (iova == 0) {
						hakios_tl("pcie2: MSI: pcie2_msi=1 without a context; not enabled\n");
					} else if (!smmu_enter_page(0x17a10000, 0x17a10000, 0x0060000000000e4bull)) {
						hakios_tl("pcie2: MSI: the GIC MBI page is not mapped\n");
					} else {
						int msi_error = pcie2_msi_enable();

						hakios_tl("pcie2: MSI: the GIC MBI page 0x17a10000 is in context %u, enable %d\n",
						    modem_space.context, msi_error);
					}
				}
				mhi_sid_iova = phys;
				mhi_sid_ready = 1;
			}
		} else if (sid != 0) {
			modem_sid_setup(sid);
		}
		/*
		 * SM8250 defaults the BDF-to-SID table to bypass: a table, and even
		 * a matching SMR, does nothing until the configuration register's
		 * bit 0 is clear. The wifi's umi_pcie_enable_dma clears exactly this
		 * bit and sets the root port's command to memory+bus master before
		 * its stream works; neither write was in this driver, which is why
		 * no SMMU context ever saw the modem's traffic.
		 */
		if (sid != 0) {
			hakios_tl("pcie2: SID: BDF-to-SID configuration before %#x\n", rd(PARF + 0x2c00));
			update(PARF + 0x2c00, 1, 0);
			hakios_tl("pcie2: SID: BDF-to-SID configuration after %#x\n", rd(PARF + 0x2c00));
		}
	}

	/* What the device may read. The vendor's mhi_init_mmio gives both
	 * windows the DT's DMA pool (the mhi_0_iommu_group's
	 * qcom,iommu-dma-addr-pool = <0x20000000 0x1fffffff>), not the image:
	 * MHICTRLBASE/LIMIT and MHIDATABASE/LIMIT are the IOVA range MHI may
	 * use, and the image is placed inside it.  The live stock kernel's
	 * success trace wrote base 0x0 / limit 0x3fffffff; the defaults here
	 * keep the pool, and pcie2_win_base/stop reproduce the stock pair. */
#define MODEM_IOVA_BASE 0x20000000u
#define MODEM_IOVA_STOP 0x3ffffffeu
	{
		uint32_t mhiraw = 0, win_base = MODEM_IOVA_BASE, win_stop = MODEM_IOVA_STOP;

		PE_parse_boot_argn("pcie2_mhiraw", &mhiraw, sizeof(mhiraw));
		PE_parse_boot_argn("pcie2_win_base", &win_base, sizeof(win_base));
		PE_parse_boot_argn("pcie2_win_stop", &win_stop, sizeof(win_stop));
		if (mhiraw == 0) {
			mhi_win_base = win_base;
			mhi_win_stop = win_stop;
			wr(MHI_BAR + 0x80, win_base);
			wr(MHI_BAR + 0x84, 0);
			wr(MHI_BAR + 0x88, win_stop);
			wr(MHI_BAR + 0x8c, 0);
			wr(MHI_BAR + 0x98, win_base);
			wr(MHI_BAR + 0x9c, 0);
			wr(MHI_BAR + 0xa0, win_stop);
			wr(MHI_BAR + 0xa4, 0);
			hakios_tl("pcie2: MHI: windows base %#x stop %#x, image at %#llx size %zu\n", win_base,
			    win_stop, (unsigned long long)phys, size);
		} else {
			hakios_tl("pcie2: MHI: the windows are left as they were (pcie2_mhiraw=1), image at %#llx\n",
			    (unsigned long long)phys);
		}
	}

	/* the image, and the doorbell (the mapping is uncached: no flush needed) */
	{
		/* clear the SMMU's recorded faults so the ones after the doorbell
		 * are the endpoint's */
		vm_offset_t smmu = (vm_offset_t)ml_io_map(SMMU_BASE, 0x1000);

		if (smmu != 0) {
			((volatile uint32_t *)smmu)[0x48 / 4] = 0xffffffff;
			__asm__ volatile("dsb sy" ::: "memory");
		}
		/* ... and our own context's, whose boot-chain state is not this
		 * transfer's fault */
		if (modem_space.on) {
			modem_context_fsr(true);
			hakios_tl("pcie2: SMMU: before the doorbell:\n");
			modem_context_fsr(false);
		}
	}
	/*
	 * ... and the root complex's AER status, which the boot chain's own
	 * traffic (Android boots this modem often) leaves behind. Writing 1 to a
	 * status bit clears it; the read-back says whether the registers are
	 * writable at all, so the read after the doorbell is this transfer's and
	 * not an earlier boot's.
	 */
	wr(DBI + 0x104, 0xffffffff);
	wr(DBI + 0x110, 0xffffffff);
	wr(DBI + 0x130, 0xffffffff);
	__asm__ volatile("dsb sy" ::: "memory");
	hakios_tl("pcie2: AER: before the doorbell: uncorr %#x corr %#x root %#x\n", rd(DBI + 0x104),
	    rd(DBI + 0x110), rd(DBI + 0x130));
	/* ... and the apps SMMU's non-secure global fault status (+0x400 alias,
	 * nsGFSR 0x448): cleared so a fault after the doorbell is this read's,
	 * and read after it to name the stream when nothing matches it. */
	{
		volatile uint32_t *smmu = (volatile uint32_t *)smmu_window();

		if (smmu != 0) {
			smmu[0x448 / 4] = 0xffffffff;
			__asm__ volatile("dsb sy" ::: "memory");
			hakios_tl("pcie2: SMMU: NS GFSR cleared: %#x (SCR0 at 0x0 %#x, at 0x400 %#x)\n", smmu[0x448 / 4],
			    smmu[0x000 / 4], smmu[0x400 / 4]);
		}
	}
	/*
	 * pcie2_inb=1: an inbound window for the image's address range (region
	 * 2, ctrl1 0x01 = inbound memory; the outbound regions 0 and 1 are the
	 * other direction). The vendor driver programs no inbound region, but the
	 * PBL's read of host memory is exactly an inbound TLP, and the AER's
	 * advisory event after the doorbell says the read reaches the root
	 * complex's error path. If the core's inbound routing is what terminates
	 * it, this is where the answer changes; it touches only TLPs from the
	 * endpoint to this address range, never the host's own accesses to the
	 * modem.
	 */
	{
		uint32_t inb = 0;

		PE_parse_boot_argn("pcie2_inb", &inb, sizeof(inb));
		if (inb != 0 && (phys >> 32) == 0) {
			uint32_t base = (uint32_t)phys & ~0xfffu;

			iatu_config(2, 0x01, base, base + 0x7fffff, base);
			hakios_tl("pcie2: iATU 2: inbound %#x..%#x -> %#x, ctrl1 %#x ctrl2 %#x\n", base, base + 0x7fffff,
			    base, rd(IATU + 2 * 0x200), rd(IATU + 2 * 0x200 + 0x4));
		}
	}
	/*
	 * pcie2_addr=N: hand the PBL an address other than the file's. The file
	 * stays where it is; only the doorbell's address changes. Reading a
	 * different address and watching the AER says whether a read completes
	 * anywhere: the advisory event with the file's address and none with
	 * another is the difference between a path that refuses everything and
	 * one that refuses only this region.
	 */
	{
		uint32_t addr = 0;

		if (PE_parse_boot_argn("pcie2_addr", &addr, sizeof(addr))) {
			hakios_tl("pcie2: BHI: the probe's image address %#x, not the file's %#llx\n", addr,
			    (unsigned long long)phys);
			phys = addr;
		}
	}
	/*
	 * pcie2_imgsz=N: hand the PBL a size other than the file's. With N=0 the
	 * doorbell is rung with nothing to read at all -- if the refusal and the
	 * AER advisory event are the same, neither is about reading host memory.
	 */
	{
		uint32_t imgsz = 0;

		if (PE_parse_boot_argn("pcie2_imgsz", &imgsz, sizeof(imgsz))) {
			hakios_tl("pcie2: BHI: the probe's image size %#x, not the file's %zu\n", imgsz, size);
			size = imgsz;
		}
	}
	/*
	 * pcie2_txdb=N: the value written to BHI_IMGTXDB. The sentinel 0x1234567
	 * was only ever a way to see the doorbell answer; the PBL reads that word
	 * as a request, and its fields (the image type among them) are what the
	 * next experiments vary: mainline's mhi writes its own image request
	 * there, and a wrong type could be what is refused after the transfer.
	 */
	{
		uint32_t txdb = 0x1234567;

		if (PE_parse_boot_argn("pcie2_txdb", &txdb, sizeof(txdb))) {
			hakios_tl("pcie2: BHI: the probe's doorbell %#x, not the sentinel\n", txdb);
		}
		wr(bhi + 0x2c, 0);              /* BHI_STATUS */
		wr(bhi + 0x0c, (uint32_t)(phys >> 32));
		wr(bhi + 0x08, (uint32_t)phys);
		wr(bhi + 0x10, (uint32_t)size); /* BHI_IMGSIZE */
		wr(bhi + 0x18, txdb);           /* BHI_IMGTXDB */
		hakios_tl("pcie2: BHI: wrote imgaddr %#x/%#x size %#x doorbell %#x; read back %#x/%#x size %#x, txdb %#x, status %#x\n",
		    (uint32_t)(phys >> 32), (uint32_t)phys, (uint32_t)size, txdb, rd(bhi + 0x0c), rd(bhi + 0x08),
		    rd(bhi + 0x10), rd(bhi + 0x18), rd(bhi + 0x2c));
	}
	/*
	 * pcie2_eetrace=1: sample BHI_EXECENV and BHI_STATUS every millisecond
	 * for the first 500 ms after the doorbell, logging every change.  The
	 * umi port's working stack waits for EXECENV==1 before it touches MHI
	 * (umi_modem_mhi.inc:1156): the SBL writes 1 when it starts, AMSS 2
	 * later.  This probe has only ever read the word once, after the
	 * refusal, so it cannot say whether the PBL jumped and the SBL died or
	 * the PBL never loaded the ELF.  The trace only reads; the normal wait
	 * follows it unchanged, and the error registers are captured at the
	 * moment the refusal appears in case the PBL clears them later.
	 */
	PE_parse_boot_argn("pcie2_eetrace", &eetrace, sizeof(eetrace));
	if (eetrace) {
		uint32_t last_ee = ~0u, last_result = ~0u;
		unsigned ms = 0;

		while (ms < 500) {
			uint32_t ee = rd(bhi + 0x28), raw = rd(bhi + 0x2c), result = (raw >> 30) & 3;

			if (ee != last_ee || result != last_result) {
				hakios_tl("pcie2: BHI trace: %u ms: execenv %#x, status %#x (result %u), MHISTATUS %#x\n", ms,
				    ee, raw, result, rd(MHI_BAR + 0x48));
				if (result == 3 && last_result != 3) {
					hakios_tl("pcie2: BHI trace: %u ms: the refusal is set: errcode %#x, dbg %#x %#x %#x\n",
					    ms, rd(bhi + 0x30), rd(bhi + 0x34), rd(bhi + 0x38), rd(bhi + 0x3c));
				}
				last_ee = ee;
				last_result = result;
			}
			ms++;
			if (result == 2) {
				break; /* success: hand back to the normal path at once */
			}
			delay(1000);
		}
		traced = ms;
		hakios_tl("pcie2: BHI trace: %u ms window done: execenv %#x, status %#x\n", ms, rd(bhi + 0x28),
		    rd(bhi + 0x2c));
	} else {
		delay(10000);
	}
	modem_context_fsr(false);
	for (i = 0; i < 1000; i++) {
		status = rd(bhi + 0x2c);
		if (((status >> 30) & 3) == 2 || ((status >> 30) & 3) == 3) {
			break;
		}
		if (i % 100 == 99) {
			hakios_tl("pcie2: BHI: still %#x at %u ms (execenv %#x, MHISTATUS %#x)\n", status, traced + (i + 1) * 10,
			    rd(bhi + 0x28), rd(MHI_BAR + 0x48));
		}
		delay(10000);
	}
	hakios_tl("pcie2: BHI: status %#x (result %u), errcode %#x, dbg %#x %#x %#x, execenv %#x, MHISTATUS %#x after %u ms\n",
	    status, (status >> 30) & 3, rd(bhi + 0x30), rd(bhi + 0x34), rd(bhi + 0x38), rd(bhi + 0x3c),
	    rd(bhi + 0x28), rd(MHI_BAR + 0x48), traced + i * 10);
	/* what the root complex saw of it: interrupt status and the two error
	 * registers (whether the endpoint's reads of host memory completed) */
	hakios_tl("pcie2: PARF: int status %#x, status %#x, core errors %#x\n", rd(PARF + 0x224), rd(PARF + 0x230),
	    rd(PARF + 0x3c0));
	/*
	 * The root complex's own AER view (the DBI window is its config space):
	 * a completer abort or an unsupported request for the endpoint's reads of
	 * host memory lands in the status registers; the mask and severity words
	 * say how each class is treated (a masked, non-fatal uncorrectable error
	 * shows up as the correctable status's Advisory Non-Fatal bit, 0x2000),
	 * and the header log, when the class logs one, holds the TLP that did it.
	 * The four vendor offsets are from drivers/pci/controller/pci-msm.c
	 * (PCIE20_AER_UNCORR_ERR_STATUS_REG 0x104, PCIE20_AER_CORR_ERR_STATUS_REG
	 * 0x110, PCIE20_AER_ROOT_ERR_STATUS_REG 0x130, PCIE20_AER_ERR_SRC_ID_REG
	 * 0x134); the mask, severity and header log are the standard AER layout
	 * beside them (0x108/0x114, 0x10c, 0x118-0x124).
	 */
	hakios_tl("pcie2: AER: uncorr %#x (mask %#x, severity %#x), corr %#x (mask %#x), root %#x src %#x\n",
	    rd(DBI + 0x104), rd(DBI + 0x108), rd(DBI + 0x10c), rd(DBI + 0x110), rd(DBI + 0x114), rd(DBI + 0x130),
	    rd(DBI + 0x134));
	hakios_tl("pcie2: AER: header log %#x %#x %#x %#x\n", rd(DBI + 0x118), rd(DBI + 0x11c), rd(DBI + 0x120),
	    rd(DBI + 0x124));
	/* the stream the SMMU refused, if it refused one: SGFSYNR1 is the ID;
	 * a translation fault inside a context lands in that context's FSR */
	vm_offset_t smmu_win = smmu_window();
	{
		volatile uint32_t *regs = (volatile uint32_t *)smmu_win;

		if (smmu_win != 0) {
			uint32_t idr1 = regs[0x024 / 4];
			uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
			uint32_t pages = 2u << ((idr1 >> 28) & 7);
			uint32_t cb0 = pages * page / 4;

			hakios_tl("pcie2: SMMU: SGFSR %#x, SGFSYNR0 %#x, SGFSYNR1 %#x (the stream), CB0 FSR %#x FAR %#x\n",
			    regs[0x48 / 4], regs[0x50 / 4], regs[0x54 / 4], regs[cb0 + 0x58 / 4], regs[cb0 + 0x60 / 4]);
			/* the non-secure aliases: Qualcomm's SMMU splits secure and NS,
			 * and nsGFSR is 0x448, nsGFSYNR0 0x450 (the vendor's
			 * arm-smmu.c: "secure aliases ... nsCR0: 0x400, nsGFSR: 0x448,
			 * nsGFSYNR0: 0x450"). The 0x48 read above is the secure copy the
			 * AP may not see; if the endpoint's stream matches no SMR, this
			 * is where its ID is recorded. */
			hakios_tl("pcie2: SMMU: NS GFSR %#x, GFSYNR0 %#x, GFSYNR1 %#x, GFSYNR2 %#x (SCR0 at 0x0 %#x, at 0x400 %#x)\n",
			    regs[0x448 / 4], regs[0x450 / 4], regs[0x454 / 4], regs[0x458 / 4], regs[0x000 / 4],
			    regs[0x400 / 4]);
			if (modem_space.on) {
				uint32_t our = (pages + modem_space.context) * page / 4;

				hakios_tl("pcie2: SMMU: our context %u: FSR %#x FAR %#x, TTBR0 %#x%08x, TCR %#x, SCTLR %#x\n",
				    modem_space.context, regs[our + 0x58 / 4], regs[our + 0x60 / 4], regs[our + 0x24 / 4],
				    regs[our + 0x20 / 4], regs[our + 0x30 / 4], regs[our + 0x00 / 4]);
			}
			/* Every enabled context's fault state, not only this driver's:
			 * the boot chain's SMR for the modem's stream points at a context
			 * of its own (3 on this phone) with Android's tables, and a read
			 * translated there faults there, not in SGFSR. */
			{
				uint32_t nctx = idr1 & 0xff;

				for (uint32_t cx = 0; cx < nctx; cx++) {
					uint32_t at = (pages + cx) * page / 4;
					uint32_t sctlr = regs[at + SMMU_CB_SCTLR / 4];
					uint64_t ttbr0 = ((uint64_t)regs[at + SMMU_CB_TTBR0 / 4 + 1] << 32) |
					    regs[at + SMMU_CB_TTBR0 / 4];

					if ((sctlr & 1) || regs[at + SMMU_CB_FSR / 4]) {
						hakios_tl("pcie2: SMMU: context %u: SCTLR %#x FSR %#x FAR %#x TTBR0 %#llx TCR %#x\n", cx,
						    sctlr, regs[at + SMMU_CB_FSR / 4], regs[at + 0x60 / 4], (unsigned long long)ttbr0,
						    regs[at + SMMU_CB_TCR / 4]);
					}
					if (sctlr & 1) {
						/* what the PBL's two addresses resolve to in this context */
						mmu_walk(cx, ttbr0, regs[at + SMMU_CB_TCR / 4], phys);
						if (phys != 0x20000000u) {
							mmu_walk(cx, ttbr0, regs[at + SMMU_CB_TCR / 4], 0x20000000u);
						}
					}
				}
			}
		}
	}
	/* the iATU's other regions, as the bootloader left them: an enabled
	 * inbound region over our image address would translate the PBL's
	 * reads somewhere else (the outbound 0 and 1 are ours). */
	for (uint32_t r = 0; r < 8; r++) {
		uint32_t b = IATU + r * 0x200;

		hakios_tl("pcie2: iATU %u: ctrl1 %#x ctrl2 %#x bar %#x/%#x lar %#x tar %#x/%#x\n", r, rd(b + 0x0),
		    rd(b + 0x4), rd(b + 0xc), rd(b + 0x8), rd(b + 0x10), rd(b + 0x18), rd(b + 0x14));
	}
	pcie2_compare_rc0();

	/* The apps SMMU's streams for the modem: the RC's BDF-to-SID table is
	 * not programmed here, so the endpoint's DMAs arrive with whatever SID
	 * the table's default gives, and an SMR that matches it with a context
	 * turns our physical addresses into translations. The block above already
	 * mapped the block; a second ml_io_map of the same range is refused, and
	 * that silence was this block's for one run. */
	{
		volatile uint32_t *regs = (volatile uint32_t *)smmu_win;

		if (smmu_win != 0) {
			uint32_t scr0 = regs[0x000 / 4], idr0 = regs[0x020 / 4];
			uint32_t matches = idr0 & 0xff, wide = scr0 & (1u << 3);

			hakios_tl("pcie2: apps SMMU: SCR0 %#x (unmatched streams %s), %u SMRs\n", scr0,
			    scr0 & (1u << 10) ? "fault" : "pass untranslated", matches);
			for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
				uint32_t smr = regs[(0x800 + 4 * n) / 4], s2cr = regs[(0xc00 + 4 * n) / 4];
				uint32_t used = wide ? (s2cr & (1u << 10)) : (smr >> 31);
				uint32_t stream, mask;

				if (!used) {
					continue;
				}
				stream = smr & (wide ? 0xffff : 0x7fff);
				mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);
				/* the modem's two SIDs, SID 0 and the wifi's two -- the
				 * comparison says which streams the SMMU actually matches */
				if (((0x1d00 ^ stream) & ~mask) != 0 && ((0x1d01 ^ stream) & ~mask) != 0 &&
				    ((0x0000 ^ stream) & ~mask) != 0 && ((0x1c00 ^ stream) & ~mask) != 0 &&
				    ((0x1c01 ^ stream) & ~mask) != 0) {
					continue;
				}
				hakios_tl("pcie2: apps SMMU: SMR %u stream %#x mask %#x s2cr %#x (kind %u context %u)\n", n,
				    stream, mask, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
			}
		}
	}
	if (((status >> 30) & 3) == 2) {
		uint32_t mhi = 0;

		/* the SBL has run: the vendor's mhi_ready_state_transition then
		 * waits for MHISTATUS.READY, and an SBL that offers BHIE bumps
		 * BHIEOFF. */
		for (i = 0; i < 1000; i++) {
			mhistatus = rd(MHI_BAR + 0x48);
			if (mhistatus & 1) {
				break;
			}
			delay(1000);
		}
		hakios_tl("pcie2: BHI: after the SBL: MHISTATUS %#x, execenv %#x, BHIEOFF %#x after %u ms\n",
		    mhistatus, rd(bhi + 0x28), rd(MHI_BAR + 0x2c), i);
		/* The engine is the next stage and only ever runs on this result:
		 * pcie2_mhi=1 with the device in READY and the SBL in environment
		 * 1, exactly what the vendor's md_probe then drives. */
		PE_parse_boot_argn("pcie2_mhi", &mhi, sizeof(mhi));
		if (mhi && (mhistatus & 1) && rd(bhi + 0x28) == 1) {
			pcie2_mhi_sahara(bhi);
		} else if (mhi) {
			hakios_tl("pcie2: MHI: not started: MHISTATUS %#x, execenv %#x after %u ms\n", mhistatus,
			    rd(bhi + 0x28), i);
		}
	}
}

static void pcie2_pin_output(unsigned pin, unsigned high);

static void
endpoint(void)
{
	uint32_t id, classreg, bar0;

	/* 0002:01:00.0: bus 1, device 0, function 0. The vendor does NOT use
	 * an ECAM address (bus << 20): msm_pcie_cfg_bdf re-points iATU region 0
	 * at the target BDF, so the CONF resource's FIRST 4 KB *are* that
	 * device's config space and a register is read at CONF + offset.
	 * Reading CONF + (1 << 20) misses the iATU window and answers all
	 * ones -- the two runs that did. */
	iatu_config(0, 0x04, CONF, CONF + 0xfff, 1u << 24);
	id = rd(CONF);
	classreg = rd(CONF + 8);
	bar0 = rd(CONF + 0x10);
	hakios_tl("pcie2: the endpoint says id %#x (vendor %#x, device %#x), class %#x, BAR0 %#x\n", id, id & 0xffff,
	    id >> 16, classreg, bar0);
	if (id != 0xffffffffu && id != UINT32_MAX) {
		mhi_peek();
		/* pcie2_pon_late: the vendor's order -- the link is up first (the
		 * controller driver proves the endpoint), then the ESOC power-on
		 * resets the modem through its own PON and raises AP2MDM_STATUS,
		 * and the MHI driver loads the SBL immediately after. */
		if (pcie2_pon_late) {
			hakios_tl("pcie2: the PMIC's modem reset after the link: %d\n", hakios_pmic_pon_modem_reset());
			delay(150000);
			pcie2_pin_output(56, 1);
			hakios_tl("pcie2: AP2MDM_STATUS high after the late reset; the doorbell follows\n");
			delay(10000);
		}
		pcie2_bhi_try();
	}
}

#define TLMM_PAGE(pin) (0x0f500000u + (pin) * 0x1000) /* Kona SOUTH tile: +0x500000 */

/* The ext-sdx55m's lines: ap2mdm-status GPIO 56, mdm2ap-status GPIO 3, the errfatal pair 57/1
 * (the phone's DT qcom,ext-sdx55m). A read-only survey first: their configuration word and
 * the pad's input bit say whether the modem is powered and has signalled. */
static void
pcie2_lines(void)
{
	static const unsigned pins[] = { 1, 3, 56, 57 };

	/*
	 * Kona's TLMM is one 16 MB aperture whose pins are grouped in tiles:
	 * WEST at +0x100000, SOUTH at +0x500000, NORTH at +0x900000, and a
	 * pin's page is tile + pin * 0x1000 (CTL at +0, IO at +4; the vendor
	 * pinctrl-kona.c's PINGROUP). All four ext-sdx55m lines are SOUTH
	 * pins, and the Wi-Fi's known-good pins 79-81 are NORTH -- checked
	 * against the driver's own PINGROUP table, which is also why the
	 * first survey's reads at base + pin * 0x1000 were wrong: they fell
	 * in the hole below the west tile, not on the modem's pads. The gaps
	 * between tiles are unmapped and a read there stops the bus, so only
	 * pages the pin tables place in a tile are touched.
	 */
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t page = TLMM_PAGE(pins[i]);
		uint32_t cfg = rd(page);
		uint32_t io = rd(page + 4);

		hakios_tl("pcie2: gpio%u at %#x: config %#x (mux %u, pull %u, drive %u, oe %u), io %#x (in %u, out %u)\n",
		    pins[i], page, cfg, (cfg >> 2) & 3, cfg & 3, (cfg >> 6) & 7, (cfg >> 9) & 1, io, io & 1,
		    (io >> 1) & 1);
	}
}

/*
 * The DT's mdm_active state, written into the pads themselves: every line is
 * GPIO function with the bias off; the two AP outputs at 16 mA and the two
 * modem outputs at 8 mA (the DT's drive-strength 16 and 8 are the values 7 and
 * 3 in CTL bits 8:6). A line is set by writing its output latch before the
 * output enable, as the Wi-Fi's gpio_output does.
 */
#define TLMM_MUX_GPIO     0u
#define TLMM_PULL_DISABLE 3u
#define TLMM_DRIVE_8MA    3u
#define TLMM_DRIVE_16MA   7u
#define TLMM_OE           BIT(9)
#define TLMM_OUT          BIT(1)

static void
pcie2_pin_input(unsigned pin)
{
	wr(TLMM_PAGE(pin), TLMM_MUX_GPIO | TLMM_PULL_DISABLE | (TLMM_DRIVE_8MA << 6));
}

static void
pcie2_pin_output(unsigned pin, unsigned high)
{
	wr(TLMM_PAGE(pin) + 4, high ? TLMM_OUT : 0);
	wr(TLMM_PAGE(pin), TLMM_MUX_GPIO | TLMM_PULL_DISABLE | (TLMM_DRIVE_16MA << 6) | TLMM_OE);
}

static unsigned
pcie2_pin_in(unsigned pin)
{
	return rd(TLMM_PAGE(pin) + 4) & 1;
}

/*
 * GPIO 85, the endpoint's PERST: the DT's pcie2_perst_default is GPIO
 * function, 2 mA, pull-down, and the line is active low, so "asserted" is
 * the output latch at 0. The vendor's msm_pcie_enable drives it low before
 * any PCIe register access and releases it only after the PHY is ready.
 */
static void
pcie2_perst(unsigned high)
{
	wr(TLMM_PAGE(85) + 4, high ? TLMM_OUT : 0);
	wr(TLMM_PAGE(85), 1u | (0u << 2) | (0u << 6) | TLMM_OE); /* pull-down, 2 mA, output */
}

/*
 * The vendor's first power-on, less the PMIC soft reset (that is a PON regmap
 * sequence XNU does not have yet): apply the DT's pad state, hold the AP lines
 * low, then raise AP2MDM_STATUS and watch MDM2AP_STATUS. The vendor's PBLRDY
 * poll is skipped because this phone's DT has no pblrdy line. No PCIe register
 * is touched -- the link is the step after this one answers.
 */
static void
pcie2_power(void)
{
	unsigned status, waited = 0;

	hakios_tl("pcie2: power: configuring the four lines, no PCIe access\n");
	pcie2_pin_input(1);      /* MDM2AP_ERRFATAL */
	hakios_tl("pcie2: power: gpio1 configured\n");
	pcie2_pin_input(3);      /* MDM2AP_STATUS */
	hakios_tl("pcie2: power: gpio3 configured\n");
	pcie2_pin_output(56, 0); /* AP2MDM_STATUS */
	hakios_tl("pcie2: power: gpio56 low\n");
	pcie2_pin_output(57, 0); /* AP2MDM_ERRFATAL */
	hakios_tl("pcie2: power: gpio57 low\n");
	delay(150000);           /* the vendor's pause after its PMIC reset */
	hakios_tl("pcie2: power: pause done\n");

	status = pcie2_pin_in(3);
	hakios_tl("pcie2: lines configured (56/57 out low); before the wake: status %u, errfatal %u\n",
	    status, pcie2_pin_in(1));

	pcie2_pin_output(56, 1); /* AP2MDM_STATUS high: the modem's power-on handshake */
	for (unsigned i = 0; i < 400; i++) {
		delay(5000);
		status = pcie2_pin_in(3);
		if (status) {
			waited = (i + 1) * 5;
			break;
		}
	}
	hakios_tl("pcie2: AP2MDM_STATUS high; MDM2AP_STATUS %s, MDM2AP_ERRFATAL %u, waited %u ms\n",
	    status ? "high" : "stays low", pcie2_pin_in(1), waited);
	delay(200000); /* the vendor's pause before the POWER_ON request */
	hakios_tl("pcie2: after the pause: status %u, errfatal %u; gpio56 ctl %#x io %#x, gpio57 ctl %#x, gpio1 io %#x, gpio3 io %#x\n",
	    pcie2_pin_in(3), pcie2_pin_in(1), rd(TLMM_PAGE(56)), rd(TLMM_PAGE(56) + 4),
	    rd(TLMM_PAGE(57)), rd(TLMM_PAGE(1) + 4), rd(TLMM_PAGE(3) + 4));
}

/*
 * power=2: the wake above, then the link, for the case where the modem answers.
 * The vendor's POWER_ON request is what starts the client's (MHI's) bring-up;
 * here the PCIe2 link follows directly, with no MHI yet.
 */
static void
pcie2_power_link(void)
{
	unsigned status;

	pcie2_pin_input(1);
	pcie2_pin_input(3);
	pcie2_pin_output(56, 0);
	pcie2_pin_output(57, 0);
	delay(150000);
	pcie2_pin_output(56, 1);
	delay(200000);

	status = pcie2_pin_in(3);
	hakios_tl("pcie2: power=2: AP2MDM_STATUS high, MDM2AP_STATUS %u; the link follows\n", status);
	if (umi_rpmh_wifi_power() != 0) {
		hakios_tl("pcie2: the PCIe rails did not come up\n");
		return;
	}
	delay(1000);
	if (clocks() != 0) {
		hakios_tl("pcie2: power and clocks failed\n");
		return;
	}
	hakios_tl("pcie2: power and clocks ready (GDSC %#x)\n", rd(GCC + 0x6004));
	if (link_train() != 0) {
		return;
	}
	hakios_tl("pcie2: link status %#x\n", rd(DBI + 0x80));
	endpoint();
}

static void
pcie2_run(void)
{
	uint32_t survey = 0;
	uint32_t power = 0;
	uint32_t bhi = 0;

	hakios_tl("pcie2: the modem's link, on the Wi-Fi's PCIe0 sequence and the phone's own registers\n");
	for (unsigned i = 0; i < sizeof(pcie2_regions) / sizeof(pcie2_regions[0]); i++) {
		pcie2_regions[i].mapping = ml_io_map(pcie2_regions[i].address, pcie2_regions[i].size);
		if (pcie2_regions[i].mapping == 0) {
			hakios_tl("pcie2: cannot map %#x\n", pcie2_regions[i].address);
			return;
		}
	}
	pcie2_lines();
	PE_parse_boot_argn("pcie2_survey", &survey, sizeof(survey));
	(void)PE_parse_boot_argn("pcie2_perst", &pcie2_perst_on, sizeof(pcie2_perst_on));
	(void)PE_parse_boot_argn("pcie2_perst_early", &pcie2_perst_early, sizeof(pcie2_perst_early));
	(void)PE_parse_boot_argn("pcie2_pon_late", &pcie2_pon_late, sizeof(pcie2_pon_late));
	if (!pcie2_perst_on) {
		hakios_tl("pcie2: pcie2_perst=0: PERST is not asserted before the link\n");
	}
	if (pcie2_pon_late) {
		hakios_tl("pcie2: pcie2_pon_late=1: link first, then the PMIC reset\n");
	}
	PE_parse_boot_argn("pcie2_power", &power, sizeof(power));
	PE_parse_boot_argn("pcie2_bhi", &bhi, sizeof(bhi));
	PE_parse_boot_argn("pcie2_keep", &pcie2_keep, sizeof(pcie2_keep));
	hakios_tl("pcie2: args: survey %u, power %u, bhi %u, keep %u\n", survey, power, bhi, pcie2_keep);
	if (survey == 1) {
		hakios_tl("pcie2: survey only; the link is not touched\n");
		return;
	}
	if (power == 2) {
		pcie2_power_link();
		return;
	}
	if (power == 1) {
		pcie2_power();
		return;
	}
	/* pcie2_bhi=2 is the vendor's first power-on before the link: the PMIC
	 * warm reset (qpnp_pon_modem_pwr_off(1), now that the PMIC bus has a
	 * write path), a pause, and then the AP2MDM_STATUS handshake below. */
	if (bhi >= 2 && !pcie2_keep && !pcie2_pon_late) {
		/* the vendor's ESOC probe leaves AP2MDM_STATUS low until its
		 * power-on; drive it low here so the PON pulse meets the same
		 * level the stock reset does. */
		pcie2_pin_output(56, 0);
		pcie2_pin_output(57, 0); /* the vendor's AP2MDM_ERRFATAL low, before the pulse */
		hakios_tl("pcie2: the PMIC's modem warm reset: %d\n", hakios_pmic_pon_modem_reset());
		hakios_tl("pcie2: PON right after the pulse:\n");
		hakios_pmic_pon_report();
		delay(150000);
		hakios_tl("pcie2: PON 150 ms later:\n");
		hakios_pmic_pon_report();
	}
	if (pcie2_perst_early) {
		/* umi's order: the two quiet lines, then PERST asserted, all
		 * before AP2MDM_STATUS goes high and before the rails
		 * (umi_modem_pcie.inc:53-66: GPIO 86 as function 3 with its
		 * pull-up, GPIO 87 as function 3).  link_train then finds the
		 * line already held and releases it 100 ms past PHY ready. */
		wr(TLMM_PAGE(86), 7); /* CLKREQ: function 3, pull-up */
		wr(TLMM_PAGE(87), 3); /* WAKE: function 3, no pull */
		pcie2_perst(0);
		hakios_tl("pcie2: pcie2_perst_early=1: GPIO86/87 as umi, PERST asserted before AP2MDM_STATUS and the rails\n");
	}
	/* the default path: the four ext-sdx55m lines in the modem's active
	 * state -- mdm2ap status and errfatal as inputs, ap2mdm status high,
	 * ap2mdm errfatal low -- without a reset pulse, because the modem was
	 * already running under Android when this kernel was booted. */
	pcie2_pin_input(1);
	pcie2_pin_input(3);
	pcie2_pin_output(57, 0);
	pcie2_pin_output(56, pcie2_pon_late ? 0 : 1);
	/* after a PMIC reset the PBL's boot window is short: the vendor goes
	 * from the reset's 150 ms straight to AP2MDM_STATUS and the MHI load,
	 * so the pauses here are milliseconds, not seconds, when bhi >= 2.
	 * The vendor's mdm4x_do_first_power_on then watches MDM2AP_STATUS for
	 * up to 100 ms and gives the modem another 200 ms before the MHI/PCIe
	 * probe touches the link; mirror that tail when the reset ran. */
	/* The vendor's mdm4x_do_first_power_on does its 200 ms wait only on the
	 * pblrdy branch; this phone's DT has no pblrdy line, so stock queues the
	 * image request right after AP2MDM_STATUS and the MHI load follows.  The
	 * reset-to-doorbell gap stays short, as stock's is (~250 ms), not the
	 * 200 ms status wait that branch adds. */
	delay(bhi >= 2 ? 10000 : 1000000);
	hakios_tl("pcie2: modem lines: mdm2ap status %u, errfatal %u\n", pcie2_pin_in(3), pcie2_pin_in(1));
	if (umi_rpmh_wifi_power() != 0) {
		hakios_tl("pcie2: the PCIe rails did not come up\n");
		return;
	}
	delay(bhi >= 2 ? 10000 : 1000000);
	if (clocks() != 0) {
		hakios_tl("pcie2: power and clocks failed\n");
		return;
	}
	hakios_tl("pcie2: power and clocks ready (GDSC %#x)\n", rd(GCC + 0x6004));
	if (link_train() != 0) {
		return;
	}
	hakios_tl("pcie2: link status %#x\n", rd(DBI + 0x80));
	endpoint();
}

static void
pcie2_thread(__unused void *parameter, __unused wait_result_t result)
{
	__atomic_store_n(&mhi_bb_thread_alive, 1, __ATOMIC_RELEASE);
	delay(3000);
	if (pcie2_mhi_mode == 2) {
		/* Publish AppleBaseband, PCI channels, Mav PDP manager, AppleIPAppender and pdp_ip0..3
		 * before the modem finishes Sahara/QRTR so CommCenter finds them immediately at launch. */
		umi_bb_publish();
	}
	pcie2_run();
	__atomic_store_n(&mhi_bb_live, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&mhi_bb_thread_alive, 0, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}

void
hakios_thyme_pcie2_test(void)
{
	uint32_t on = 0, cc = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("pcie2_test", &on, sizeof(on)) || on != 1) {
		return;
	}
	(void)PE_parse_boot_argn("pcie2_mhi", &pcie2_mhi_mode, sizeof(pcie2_mhi_mode));
	(void)PE_parse_boot_argn("pcie2_cc", &cc, sizeof(cc));
	if (cc == 1 && pcie2_mhi_mode < 2) {
		pcie2_mhi_mode = 2;
	}
	if (kernel_thread_start(pcie2_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
