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
 * The GCC registers are not in any local table: they were read out of the
 * phone's own kernel (docs/cellular.md has the method and the numbers) --
 * GDSC 0x6004, CBCRs 0x6010-0x602c, refgen 0x6038, branch votes at 0x52010
 * (bits 10-15), core reset 0x6000. The PHY sequence is the phone's DTB
 * qcom,phy-sequence of qcom,pcie@1c10000 (136 triples).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <mach/vm_param.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_wifi_power(void);
extern kern_return_t thread_terminate(thread_t thread);

#define GCC  0x00100000u
#define PARF 0x01c10000u
#define PHY  0x01c16000u
#define DBI  0x64000000u
#define CONF 0x64100000u
#define BIT(n) (1u << (n))

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
	{ 0x00100000, 0xa0000, 0 },  /* GCC */
	{ 0x01c10000, 0x3000, 0 },   /* PARF */
	{ 0x01c16000, 0x2000, 0 },   /* QMP PHY */
	{ 0x64000000, 0x1000, 0 },   /* DBI + ELBI */
	{ 0x64100000, 0x100000, 0 }, /* endpoint configuration (ECAM, bus 1) */
};

static vm_offset_t
pcie2_register(uint32_t address)
{
	for (unsigned i = 0; i < sizeof(pcie2_regions) / sizeof(pcie2_regions[0]); i++) {
		uint32_t base = pcie2_regions[i].address;

		if (address >= base && address - base <= pcie2_regions[i].size - 4) {
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
	error = source_xo(GCC + 0x6038);
	if (error) {
		return error;
	}
	/* the five halt-polled branches, and the votes that enable them */
	error = update(GCC + 0x52010, 0, 0x7c00);
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
	/* the core reset: set, a millisecond, clear */
	error = update(GCC + 0x6000, 0, 1);
	if (error) {
		return error;
	}
	delay(1000);
	return update(GCC + 0x6000, 1, 0);
}

static int
link_train(void)
{
	int error;
	uint32_t control;

	/* root complex, per the Wi-Fi's PCIe0 sequence, at this controller's PARF */
	wr(PARF + 0x1000, 4);
	error = update(PARF + 0x40, 1, 0);
	if (error) {
		return error;
	}
	wr(PARF + 0x350, 0);
	wr(PARF, 0x365e);
	error = update(PARF + 0x174, 0, BIT(4));
	if (error) {
		return error;
	}
	wr(PARF + 0x22c, 0);
	wr(PARF + 0x358, 0x04000000);
	error = update(PARF + 0x1a8, 0, BIT(31));
	if (error) {
		return error;
	}
	hakios_tl("pcie2: PARF programmed; PARF+0x350 %#x, +0x358 %#x\n", rd(PARF + 0x350), rd(PARF + 0x358));
	for (unsigned i = 0; i < sizeof(pcie2_phy) / sizeof(pcie2_phy[0]); i++) {
		wr(PHY + pcie2_phy[i].offset, pcie2_phy[i].value);
		if (pcie2_phy[i].delay_us) {
			delay(pcie2_phy[i].delay_us);
		}
	}
	delay(1000);
	error = update(GCC + 0x52010, 0, 0x8000);       /* the pipe clock's vote */
	if (error) {
		return error;
	}
	hakios_tl("pcie2: PHY sequence written; PHY+0x814 %#x\n", rd(PHY + 0x814));
	error = wait_bits(PHY + 0x814, BIT(6), 0, 10000);
	if (error) {
		hakios_tl("pcie2: the PHY does not report ready: %#x\n", rd(PHY + 0x814));
		return error;
	}
	delay(10000);
	error = update(DBI + 0x80c, 0x1f00, 0x100);
	if (error) {
		return error;
	}
	wr(DBI + 0x8a8, 0x20);
	error = update(DBI + 0x890, BIT(0), 0);
	if (error) {
		return error;
	}
	error = update(DBI + 0x8bc, 0, BIT(0));
	if (error) {
		return error;
	}
	wr(DBI + 0x154, 0x55555555);
	error = update(DBI + 0x8bc, BIT(0), 0);
	if (error) {
		return error;
	}
	error = update(DBI + 0xa0, 0xf, 2);             /* Gen2, as the Wi-Fi's sequence */
	if (error) {
		return error;
	}
	control = rd(DBI + 0x78);
	if (control == UINT32_MAX) {
		return EIO;
	}
	wr(DBI + 0x78, (control & 0xffff) & ~0x7000u);
	error = update(PARF + 0x1b0, 0, BIT(8));
	if (error) {
		return error;
	}
	hakios_tl("pcie2: link requests done; LTSSM %#x, link status %#x\n", rd(DBI + 0xf28), rd(DBI + 0x80));
	error = wait_bits(DBI + 0xf28, BIT(10), BIT(10), 5000);
	if (error) {
		hakios_tl("pcie2: no link training done: %#x\n", rd(DBI + 0xf28));
		return error;
	}
	error = wait_bits(DBI + 0x80, BIT(29), BIT(29), 5000);
	if (error) {
		hakios_tl("pcie2: the link does not come up: %#x\n", rd(DBI + 0x80));
		return error;
	}
	return 0;
}

static void
endpoint(void)
{
	uint32_t id, classreg, bar0;

	/* 0002:01:00.0: bus 1, device 0, function 0 in the ECAM at CONF */
	id = rd(CONF + (1 << 20));
	classreg = rd(CONF + (1 << 20) + 8);
	bar0 = rd(CONF + (1 << 20) + 0x10);
	hakios_tl("pcie2: the endpoint says id %#x (vendor %#x, device %#x), class %#x, BAR0 %#x\n", id, id & 0xffff,
	    id >> 16, classreg, bar0);
}

static void
pcie2_run(void)
{
	hakios_tl("pcie2: the modem's link, on the Wi-Fi's PCIe0 sequence and the phone's own registers\n");
	for (unsigned i = 0; i < sizeof(pcie2_regions) / sizeof(pcie2_regions[0]); i++) {
		pcie2_regions[i].mapping = ml_io_map(pcie2_regions[i].address, pcie2_regions[i].size);
		if (pcie2_regions[i].mapping == 0) {
			hakios_tl("pcie2: cannot map %#x\n", pcie2_regions[i].address);
			return;
		}
	}
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
