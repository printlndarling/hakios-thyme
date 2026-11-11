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
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_wifi_power(void);
extern int hakios_pmic_pon_modem_reset(void);
extern kern_return_t thread_terminate(thread_t thread);
extern uint64_t kvtophys(vm_offset_t va);

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
	{ 0x0f000000, 0xa00000, 0 }, /* TLMM: west +0x100000, south +0x500000, north +0x900000 */
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
	if (pcie2_keep) {
		hakios_tl("pcie2: PERST left alone (pcie2_keep=1)\n");
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
	delay(10000);                             /* qcom,ep-latency, 10 ms */

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

extern lck_mtx_t hakios_smmu_lock;

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

static struct {
	vm_offset_t tables;              /* the root, then the levels it needs */
	uint32_t count;
	uint32_t context;
	bool on;
} modem_space;

static vm_offset_t smmu_va;

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
		uint64_t at = SMMU_IOVA + offset;
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
		smmu_va = ml_io_map(SMMU_BASE, 0x100000);
	}
	if (smmu_va == 0) {
		hakios_tl("pcie2: SMMU: cannot map the block\n");
		return 0;
	}
	regs = (volatile uint32_t *)smmu_va;
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
	return SMMU_IOVA;
}

/*
 * The modem's streams in the apps SMMU. The RC's BDF-to-SID table is not
 * programmed here, so the endpoint's DMA arrives as SID 0 -- and the phone's
 * boot chain leaves SMR 0 (stream 0, mask 0) in S2CR type FAULT, with
 * SCR0's unmatched-stream policy also FAULT. Every read of the PBL's boot
 * engine was therefore faulted in the SMMU, which is the BHI error
 * (0xef120700) that neither the address nor the image explained.
 *
 * Two writes fix that the way the DT intends: the BDF-to-SID table gets BDF
 * 1:0.0 (0x0100) -> the DT SID's offset in every entry, so the hash the RC
 * computes cannot miss it; and two free SMRs are given the table offset (1)
 * and the full SID (0x1d01) with S2CR type BYPASS, like the SLPI's FastRPC
 * stream is given one. SID 0's fault entry is left as the boot chain made
 * it -- writing that SMR was the one change followed by a log-less reset.
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
		 * bypass, the stream's reads pass and the global fault stays 0 */
		smmu = (vm_offset_t)ml_io_map(0x15000000, 0x100000);
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
			regs[(0xc00 + 4 * n) / 4] = wide ? ((1u << 10) | 0x20000) : 0x20000;
			hakios_tl("pcie2: SID: SMR %u stream 0x1d01, s2cr read back %#x (type %u)\n", n,
			    regs[(0xc00 + 4 * n) / 4], (regs[(0xc00 + 4 * n) / 4] >> 16) & 3);
			break;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	if (mode == 4) {
		/* a probe: 0x1d01 matched but pointed at context 0, which is off
		 * -- if the endpoint's reads reach the SMMU at all, this makes
		 * the SMMU record the fault and the stream */
		smmu = (vm_offset_t)ml_io_map(0x15000000, 0x100000);
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

	smmu = (vm_offset_t)ml_io_map(0x15000000, 0x100000);
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
	uint32_t bhioff, bhi, status = 0, mhistatus = 0, i;

	if (!PE_parse_boot_argn("pcie2_bhi", &on, sizeof(on)) || on < 1) {
		return;
	}
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
		uint32_t img = 1;

		PE_parse_boot_argn("pcie2_img", &img, sizeof(img));
		if (img == 2) {
			vm_size_t bytes = 0x100000; /* 1 MB; sbl1.mbn is 548,056 */

			if (kmem_alloc_contig(kernel_map, &memory, bytes, PAGE_MASK, 0, 0,
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
		memory = ml_io_map(MODEM_IMAGE_PA, MODEM_IMAGE_SIZE);
		if (memory == 0) {
			hakios_tl("pcie2: BHI: cannot map the image window\n");
			return;
		}
		for (i = 0; i < 30; i++) {
			if (root_file_into("modem/sbl1.mbn", memory, MODEM_IMAGE_SIZE, &size) == 0) {
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

	bhioff = rd(MHI_BAR + 0x28);
	bhi = MHI_BAR + bhioff;

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

			modem_sid_setup(2);         /* the BDF-to-SID table only */
			iova = modem_smmu_map(memory, size);
			if (iova != 0) {
				phys = iova;
			} else {
				hakios_tl("pcie2: BHI: no SMMU context; the physical address stays\n");
			}
		} else if (sid != 0) {
			modem_sid_setup(sid);
		}
	}

	/* what the device may read: the image's own range, in both windows */
	wr(MHI_BAR + 0x80, (uint32_t)phys);
	wr(MHI_BAR + 0x84, (uint32_t)(phys >> 32));
	wr(MHI_BAR + 0x88, (uint32_t)(phys + size - 1));
	wr(MHI_BAR + 0x8c, (uint32_t)((phys + size - 1) >> 32));
	wr(MHI_BAR + 0x98, (uint32_t)phys);
	wr(MHI_BAR + 0x9c, (uint32_t)(phys >> 32));
	wr(MHI_BAR + 0xa0, (uint32_t)(phys + size - 1));
	wr(MHI_BAR + 0xa4, (uint32_t)((phys + size - 1) >> 32));

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
	wr(bhi + 0x2c, 0);                  /* BHI_STATUS */
	wr(bhi + 0x0c, (uint32_t)(phys >> 32));
	wr(bhi + 0x08, (uint32_t)phys);
	wr(bhi + 0x10, (uint32_t)size);     /* BHI_IMGSIZE */
	wr(bhi + 0x18, 0x1234567);          /* BHI_IMGTXDB */
	hakios_tl("pcie2: BHI: wrote imgaddr %#x/%#x size %#x; read back %#x/%#x size %#x, txdb %#x, status %#x\n",
	    (uint32_t)(phys >> 32), (uint32_t)phys, (uint32_t)size, rd(bhi + 0x0c), rd(bhi + 0x08), rd(bhi + 0x10),
	    rd(bhi + 0x18), rd(bhi + 0x2c));
	delay(10000);
	modem_context_fsr(false);
	for (i = 0; i < 1000; i++) {
		status = rd(bhi + 0x2c);
		if (((status >> 30) & 3) == 2 || ((status >> 30) & 3) == 3) {
			break;
		}
		if (i % 100 == 99) {
			hakios_tl("pcie2: BHI: still %#x at %u ms (execenv %#x, MHISTATUS %#x)\n", status, (i + 1) * 10,
			    rd(bhi + 0x28), rd(MHI_BAR + 0x48));
		}
		delay(10000);
	}
	hakios_tl("pcie2: BHI: status %#x (result %u), errcode %#x, dbg %#x %#x %#x, execenv %#x, MHISTATUS %#x after %u ms\n",
	    status, (status >> 30) & 3, rd(bhi + 0x30), rd(bhi + 0x34), rd(bhi + 0x38), rd(bhi + 0x3c),
	    rd(bhi + 0x28), rd(MHI_BAR + 0x48), i * 10);
	/* what the root complex saw of it: interrupt status and the two error
	 * registers (whether the endpoint's reads of host memory completed) */
	hakios_tl("pcie2: PARF: int status %#x, status %#x, core errors %#x\n", rd(PARF + 0x224), rd(PARF + 0x230),
	    rd(PARF + 0x3c0));
	/* the stream the SMMU refused, if it refused one: SGFSYNR1 is the ID;
	 * a translation fault inside a context lands in that context's FSR */
	{
		vm_offset_t smmu = (vm_offset_t)ml_io_map(0x15000000, 0x100000);

		if (smmu != 0) {
			volatile uint32_t *regs = (volatile uint32_t *)smmu;
			uint32_t idr1 = regs[0x024 / 4];
			uint32_t page = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
			uint32_t pages = 2u << ((idr1 >> 28) & 7);
			uint32_t cb0 = pages * page / 4;

			hakios_tl("pcie2: SMMU: SGFSR %#x, SGFSYNR0 %#x, SGFSYNR1 %#x (the stream), CB0 FSR %#x FAR %#x\n",
			    regs[0x48 / 4], regs[0x50 / 4], regs[0x54 / 4], regs[cb0 + 0x58 / 4], regs[cb0 + 0x60 / 4]);
			if (modem_space.on) {
				uint32_t our = (pages + modem_space.context) * page / 4;

				hakios_tl("pcie2: SMMU: our context %u: FSR %#x FAR %#x, TTBR0 %#x%08x, TCR %#x, SCTLR %#x\n",
				    modem_space.context, regs[our + 0x58 / 4], regs[our + 0x60 / 4], regs[our + 0x24 / 4],
				    regs[our + 0x20 / 4], regs[our + 0x30 / 4], regs[our + 0x00 / 4]);
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

	/* The apps SMMU's streams for the modem: the RC's BDF-to-SID table is
	 * not programmed here, so the endpoint's DMAs arrive with whatever SID
	 * the table's default gives, and an SMR that matches it with a context
	 * turns our physical addresses into translations. */
	{
		vm_offset_t smmu = (vm_offset_t)ml_io_map(0x15000000, 0x100000);

		if (smmu != 0) {
			volatile uint32_t *regs = (volatile uint32_t *)smmu;
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
				/* the modem's two SIDs, and SID 0, are what matter */
				if (((0x1d00 ^ stream) & ~mask) != 0 && ((0x1d01 ^ stream) & ~mask) != 0 &&
				    ((0x0000 ^ stream) & ~mask) != 0) {
					continue;
				}
				hakios_tl("pcie2: apps SMMU: SMR %u stream %#x mask %#x s2cr %#x (kind %u context %u)\n", n,
				    stream, mask, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
			}
		}
	}
	if (((status >> 30) & 3) == 2) {
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
	}
}

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
	if (bhi >= 2 && !pcie2_keep) {
		hakios_tl("pcie2: the PMIC's modem warm reset: %d\n", hakios_pmic_pon_modem_reset());
		delay(150000);
	}
	/* the default path: the four ext-sdx55m lines in the modem's active
	 * state -- mdm2ap status and errfatal as inputs, ap2mdm status high,
	 * ap2mdm errfatal low -- without a reset pulse, because the modem was
	 * already running under Android when this kernel was booted. */
	pcie2_pin_input(1);
	pcie2_pin_input(3);
	pcie2_pin_output(57, 0);
	pcie2_pin_output(56, 1);
	/* after a PMIC reset the PBL's boot window is short: the vendor goes
	 * from the reset's 150 ms straight to AP2MDM_STATUS and the MHI load,
	 * so the pauses here are milliseconds, not seconds, when bhi >= 2 */
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
	delay(3000);
	pcie2_run();
	thread_terminate(current_thread());
}

void
hakios_thyme_pcie2_test(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("pcie2_test", &on, sizeof(on)) || on != 1) {
		return;
	}
	if (kernel_thread_start(pcie2_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
