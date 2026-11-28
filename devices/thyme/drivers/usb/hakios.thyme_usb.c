/*
 * THYME's SM8250 DWC_usb31 peripheral controller. The loader stops its
 * EFI USB driver before handing over this core. Keep the firmware's
 * Qualcomm wrapper/PHY configuration and take over the device registers.
 */
#include <arm/caches_internal.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void hakios_battery_guard_state(int *, int *, int *, int *) __attribute__((weak));
void hakios_thyme_usb_quiesce(void);

#define MS_VENDOR_CODE          0x21
#define DWC3_PA                 0x0a60c000UL
#define QSCRATCH_PAGE           0x0a6f8000UL
#define QSCRATCH_HS_PHY_CTRL    0x810
#define HS_VBUS_VALID           (1u << 20)
#define HS_SESSION_FROM_SW      (1u << 28)
#define HS_SESSION_MASK         (HS_VBUS_VALID | HS_SESSION_FROM_SW)
#define GCTL                    0x110
#define GSNPSID                 0x120
#define GUSB2PHYCFG             0x200
#define GEVNTADRLO              0x400
#define GEVNTADRHI              0x404
#define GEVNTSIZ                0x408
#define GEVNTCOUNT              0x40c
#define DCFG                    0x700
#define DCTL                    0x704
#define DEVTEN                  0x708
#define DSTS                    0x70c
#define DALEPENA                0x720
#define DEPCMD_BASE             0x800

#define DCTL_RUN                (1u << 31)
#define DCTL_RESET              (1u << 30)
#define DSTS_HALTED             (1u << 22)
#define DEV_ADDR_MASK           (0x7fu << 3)
#define GEVNT_INTMASK           (1u << 31)
#define GEVNT_COUNT_MASK        0xfffcu
#define DEPCMD_ACTIVE           (1u << 10)
#define DEPCMD_STATUS           (0xfu << 12)
#define DEPCMD_FORCE            (1u << 11)

enum {
	DEP_CONFIGURE = 1, DEP_RESERVE = 2, DEP_STALL = 4,
	DEP_START = 6, DEP_END = 8, DEP_START_CONFIG = 9,
};

enum {
	TRB_SETUP = 2, TRB_STATUS2 = 3, TRB_STATUS3 = 4, TRB_DATA = 5,
};

enum {
	TRANSFER_COMPLETE = 1, TRANSFER_NOT_READY = 3,
	USB_DISCONNECT = 0, USB_RESET = 1, USB_CONNECT_DONE = 2, USB_OVERFLOW = 11,
};

enum usb_phase { USB_SETUP, USB_DATA_IN, USB_STATUS_IN, USB_STATUS_OUT };
enum usb_stage { USB_OFF, USB_OWNED, USB_RUNNING, USB_FAILED, USB_STOPPED };

struct usb_trb {
	uint32_t lo, hi, count, control;
};

/* Device-written and CPU-written areas never share a cache line. */
static struct {
	uint32_t events[64];
	struct usb_trb trb[2] __attribute__((aligned(64)));
	uint8_t setup[64] __attribute__((aligned(64)));
	uint8_t reply[64] __attribute__((aligned(64)));
} dma __attribute__((aligned(4096)));

static vm_offset_t base;
static volatile uint32_t *hs_phy;
static uint32_t old_session;
static bool session_owned;
static int vbus_override;
static uint64_t dma_pa;
static thread_call_t event_call;
static unsigned event_slot, configuration, active_ep, resource;
static enum usb_phase phase;
static bool have_transfer;
static int stage, last_command, setup_count, reset_count, connect_count, disconnect_count;
static int address_seen, configured, stalled, event_count, hw_status, hw_control;
static int last_setup, last_index, session_changes;

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, thyme_usb, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "thyme USB gadget");
#define USB_STAT(name, var, desc) \
	SYSCTL_INT(_kern_hakios_thyme_usb, OID_AUTO, name, CTLFLAG_RD | CTLFLAG_LOCKED, &var, 0, desc)
USB_STAT(stage, stage, "0 off, 1 claimed, 2 running, 3 failed, 4 stopped");
USB_STAT(command, last_command, "last failed endpoint command and status");
USB_STAT(setup, setup_count, "received USB control requests");
USB_STAT(reset, reset_count, "USB bus resets");
USB_STAT(connect, connect_count, "USB connect-done events");
USB_STAT(disconnect, disconnect_count, "USB disconnect events");
USB_STAT(address, address_seen, "USB device address");
USB_STAT(configuration, configured, "active USB configuration");
USB_STAT(stall, stalled, "stalled control requests");
USB_STAT(events, event_count, "processed USB events");
USB_STAT(dsts, hw_status, "latest device status");
USB_STAT(dctl, hw_control, "latest device control");
USB_STAT(last_setup, last_setup, "last handled control request type, code and value");
USB_STAT(last_index, last_index, "last control request wIndex");
USB_STAT(vbus_changes, session_changes, "changes to the USB2 session-valid override");
USB_STAT(vbus_override, vbus_override, "USB2 session valid from charger telemetry");

static uint32_t
rd(unsigned reg)
{
	return *(volatile uint32_t *)(base + reg);
}

static void
wr(unsigned reg, uint32_t value)
{
	*(volatile uint32_t *)(base + reg) = value;
}

/* The peripheral role needs a valid USB2 session when the wrapper is under software control. */
static void
update_vbus(void)
{
	int soc, present, charger, charging;
	hakios_battery_guard_state(&soc, &present, &charger, &charging);
	uint32_t valid = (soc >= 0 && charger != 0) ? HS_VBUS_VALID : 0;
	uint32_t previous = *hs_phy;
	uint32_t updated = (previous & ~HS_SESSION_MASK) | HS_SESSION_FROM_SW | valid;
	if (previous != updated) {
		*hs_phy = updated;
		(void)*hs_phy;
		session_changes++;
	}
	vbus_override = valid != 0;
}

static uint64_t
usb_pa(const void *ptr)
{
	return dma_pa + ((uintptr_t)ptr - (uintptr_t)&dma);
}

static bool
wait_register(unsigned reg, uint32_t mask, uint32_t want)
{
	for (unsigned i = 0; i < 5000; i++) {
		if ((rd(reg) & mask) == want) {
			return true;
		}
		delay(1);
	}
	return false;
}

static bool
endpoint_command(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, unsigned *index)
{
	unsigned offset = DEPCMD_BASE + 0x10u * ep;
	wr(offset, 0);
	wr(offset + 4, p1);
	wr(offset + 8, p0);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(offset + 12, cmd | DEPCMD_ACTIVE);
	bool completed = wait_register(offset + 12, DEPCMD_ACTIVE, 0);
	uint32_t result = rd(offset + 12);
	if (!completed || (result & DEPCMD_STATUS) != 0) {
		last_command = (int)((ep << 24) | (cmd << 16) | (result & 0xffffu));
		return false;
	}
	if (index != NULL) {
		*index = (result >> 16) & 0x7fu;
	}
	return true;
}

static bool
queue_control(unsigned ep, unsigned type, const void *buffer, unsigned length)
{
	struct usb_trb *trb = &dma.trb[ep];
	uint64_t buffer_pa = usb_pa(buffer);
	uint64_t trb_pa = usb_pa(trb);

	*trb = (struct usb_trb){
		.lo = (uint32_t)buffer_pa, .hi = (uint32_t)(buffer_pa >> 32),
		.count = length, .control = (type << 4) | (1u << 11) | 3u,
	};
	flush_dcache64((uintptr_t)trb, sizeof(*trb), 0);
	if (!endpoint_command(ep, DEP_START, (uint32_t)(trb_pa >> 32), (uint32_t)trb_pa, &resource)) {
		return false;
	}
	active_ep = ep;
	have_transfer = true;
	return true;
}

static bool
queue_setup(void)
{
	phase = USB_SETUP;
	flush_dcache64((uintptr_t)dma.setup, sizeof(dma.setup), 0);
	return queue_control(0, TRB_SETUP, dma.setup, 8);
}

static bool
stall_setup(void)
{
	stalled++;
	return endpoint_command(0, DEP_STALL, 0, 0, NULL) && queue_setup();
}

static unsigned
descriptor(uint16_t value, uint16_t language)
{
	/* 1209:0001 is pid.codes' non-unique private test ID. Keep it off in distributed use. */
	static const uint8_t device[] = {
		18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 1, 0, 1, 1, 1, 2, 0, 1,
	};
	static const uint8_t config[] = {
		9, 2, 18, 0, 1, 1, 0, 0x80, 50,
		9, 4, 0, 0, 0, 0xff, 0, 0, 0,
	};
	static const uint8_t qualifier[] = { 10, 6, 0, 2, 0, 0, 0, 64, 1, 0 };
	unsigned index = value & 0xffu;
	switch (value >> 8) {
	case 1:
		if (index != 0) return 0;
		memcpy(dma.reply, device, sizeof(device));
		return sizeof(device);
	case 2:
	case 7:
		if (index != 0) return 0;
		memcpy(dma.reply, config, sizeof(config));
		dma.reply[1] = (uint8_t)(value >> 8);
		return sizeof(config);
	case 6:
		if (index != 0) return 0;
		memcpy(dma.reply, qualifier, sizeof(qualifier));
		return sizeof(qualifier);
	case 3:
		if (index == 0) {
			static const uint8_t english[] = { 4, 3, 9, 4 };
			memcpy(dma.reply, english, sizeof(english));
			return sizeof(english);
		}
		if (index == 0xee) {
			static const uint8_t os[] = {
				18, 3, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, MS_VENDOR_CODE, 0,
			};
			memcpy(dma.reply, os, sizeof(os));
			return sizeof(os);
		}
		if (language != 0x0409 || index > 2) {
			return 0;
		}
		const char *string = index == 1 ? "HakiOS" : "HakiOS thyme USB test";
		unsigned length = (unsigned)strlen(string);
		dma.reply[0] = (uint8_t)(length * 2 + 2);
		dma.reply[1] = 3;
		for (unsigned i = 0; i < length; i++) {
			dma.reply[2 + i * 2] = (uint8_t)string[i];
			dma.reply[3 + i * 2] = 0;
		}
		return dma.reply[0];
	default:
		return 0;
	}
}

static void
set_address(unsigned value)
{
	wr(DCFG, (rd(DCFG) & ~DEV_ADDR_MASK) | (value << 3));
	address_seen = (int)value;
}

static bool
handle_setup(void)
{
	flush_dcache64((uintptr_t)dma.setup, sizeof(dma.setup), 0);
	const uint8_t *setup = dma.setup;
	uint16_t value = setup[2] | (setup[3] << 8);
	uint16_t index = setup[4] | (setup[5] << 8);
	uint16_t length = setup[6] | (setup[7] << 8);
	unsigned size = 0;

	setup_count++;
	last_setup = (int)((uint32_t)setup[0] << 24 | (uint32_t)setup[1] << 16 | value);
	last_index = index;
	switch ((setup[0] << 8) | setup[1]) {
	case 0x0005: /* SET_ADDRESS */
		if (index != 0 || length != 0 || value > 127 || configuration != 0) return stall_setup();
		set_address(value);
		phase = USB_STATUS_IN;
		return true;
	case 0x0009: /* SET_CONFIGURATION */
		if (index != 0 || length != 0 || value > 1 || address_seen == 0) return stall_setup();
		configuration = value;
		configured = (int)value;
		phase = USB_STATUS_IN;
		return true;
	case 0x8006: /* GET_DESCRIPTOR */
		size = descriptor(value, index);
		if (size == 0) return stall_setup();
		break;
	case 0xc000 | MS_VENDOR_CODE: /* Microsoft extended compatible ID */
		if (value != 0 || index != 4) return stall_setup();
		static const uint8_t compatible_id[40] = {
			[0] = 40, [5] = 1, [6] = 4, [8] = 1,
			[17] = 1, [18] = 'W', 'I', 'N', 'U', 'S', 'B',
		};
		memcpy(dma.reply, compatible_id, sizeof(compatible_id));
		size = sizeof(compatible_id);
		break;
	case 0x8008: /* GET_CONFIGURATION */
		if (value != 0 || index != 0 || length != 1) return stall_setup();
		dma.reply[0] = (uint8_t)configuration;
		size = 1;
		break;
	case 0x8000: /* GET_STATUS (device) */
	case 0x8100: /* GET_STATUS (interface) */
	case 0x8200: /* GET_STATUS (endpoint) */
		if (value != 0 || length != 2 ||
		    (index != 0 && !(setup[0] == 0x82 && index == 0x80))) return stall_setup();
		dma.reply[0] = dma.reply[1] = 0;
		size = 2;
		break;
	case 0x810a: /* GET_INTERFACE */
		if (configuration == 0 || value != 0 || index != 0 || length != 1) return stall_setup();
		dma.reply[0] = 0;
		size = 1;
		break;
	case 0x010b: /* SET_INTERFACE */
		if (configuration == 0 || value != 0 || index != 0 || length != 0) return stall_setup();
		phase = USB_STATUS_IN;
		return true;
	default:
		return stall_setup();
	}
	if (length == 0) {
		phase = USB_STATUS_IN;
		return true;
	}
	if (length < size) {
		size = length;
	}
	flush_dcache64((uintptr_t)dma.reply, sizeof(dma.reply), 0);
	phase = USB_DATA_IN;
	return queue_control(1, TRB_DATA, dma.reply, size);
}

static void
end_transfer(void)
{
	if (have_transfer) {
		endpoint_command(active_ep, DEP_END | DEPCMD_FORCE | (resource << 16), 0, 0, NULL);
		have_transfer = false;
	}
}

static bool
process_event(uint32_t event)
{
	if (event & 1u) {
		switch ((event >> 8) & 0xfu) {
		case USB_DISCONNECT:
			disconnect_count++;
			return true;
		case USB_RESET:
			reset_count++;
			set_address(0);
			configuration = 0;
			configured = 0;
			if (phase == USB_SETUP && have_transfer) {
				return true;
			}
			end_transfer();
			return queue_setup();
		case USB_CONNECT_DONE:
			connect_count++;
			return true;
		case USB_OVERFLOW:
			return false;
		default:
			return true;
		}
	}
	unsigned ep = (event >> 1) & 0x1fu;
	unsigned kind = (event >> 6) & 0xfu;
	unsigned status = (event >> 12) & 0xfu;
	if (kind == TRANSFER_NOT_READY && !have_transfer && status == 2) {
		if (phase == USB_STATUS_IN && ep == 1) {
			return queue_control(1, TRB_STATUS2, dma.reply, 0);
		}
		if (phase == USB_STATUS_OUT && ep == 0) {
			return queue_control(0, TRB_STATUS3, dma.reply, 0);
		}
	}
	if (kind != TRANSFER_COMPLETE || !have_transfer || ep != active_ep) {
		return true;
	}
	have_transfer = false;
	flush_dcache64((uintptr_t)&dma.trb[ep], sizeof(dma.trb[ep]), 0);
	/* The hardware completes a queued TRB early when a new setup packet arrives. */
	if (((dma.trb[ep].count >> 28) & 0xfu) == 2) {
		return queue_setup();
	}
	switch (phase) {
	case USB_SETUP:
		return handle_setup();
	case USB_DATA_IN:
		phase = USB_STATUS_OUT;
		return true;
	case USB_STATUS_IN:
	case USB_STATUS_OUT:
		return queue_setup();
	}
	return false;
}

void
hakios_thyme_usb_quiesce(void)
{
	if (stage != USB_OWNED && stage != USB_RUNNING) {
		return;
	}
	/* On loader return the other CPUs are stopped. The timer will not run again. */
	stage = USB_STOPPED;
	end_transfer();
	wr(DCTL, rd(DCTL) & ~DCTL_RUN);
	for (unsigned i = 0; i < 5000 && !(rd(DSTS) & DSTS_HALTED); i++) {
		wr(GEVNTCOUNT, rd(GEVNTCOUNT) & GEVNT_COUNT_MASK);
		delay(1);
	}
	hw_status = (int)rd(DSTS);
	if ((hw_status & DSTS_HALTED) != 0) {
		wr(DEVTEN, 0);
		wr(GEVNTSIZ, GEVNT_INTMASK);
	}
	if (session_owned) {
		*hs_phy = (*hs_phy & ~HS_SESSION_MASK) | old_session;
		(void)*hs_phy;
		session_owned = false;
		vbus_override = 0;
	}
	/* The DMA page stays reserved until the EFI loader cold-resets. */
}

static void
poll_events(__unused thread_call_param_t first, __unused thread_call_param_t second)
{
	if (stage != USB_RUNNING) {
		return;
	}
	update_vbus();
	hw_control = (int)rd(DCTL);
	hw_status = (int)rd(DSTS);
	unsigned bytes = rd(GEVNTCOUNT) & GEVNT_COUNT_MASK;
	if (bytes > sizeof(dma.events)) {
		hakios_thyme_usb_quiesce();
		stage = USB_FAILED;
		return;
	}
	for (unsigned i = 0; i < bytes / 4; i++) {
		uint32_t *slot = &dma.events[event_slot];
		flush_dcache64((uintptr_t)slot, sizeof(*slot), 0);
		uint32_t event = *slot;
		event_slot = (event_slot + 1) % (sizeof(dma.events) / sizeof(*slot));
		wr(GEVNTCOUNT, 4);
		event_count++;
		if (!process_event(event)) {
			hakios_thyme_usb_quiesce();
			stage = USB_FAILED;
			return;
		}
	}
	uint64_t deadline;
	clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(event_call, deadline);
}

static bool
take_controller(void)
{
	uint32_t id = rd(GSNPSID);
	hw_status = (int)rd(DSTS);
	if ((id >> 16) != 0x3331u || !(hw_status & DSTS_HALTED) || (rd(DCTL) & DCTL_RUN)) {
		return false;
	}
	stage = USB_OWNED;
	wr(DCTL, DCTL_RESET);
	if (!wait_register(DCTL, DCTL_RESET, 0)) {
		return false;
	}
	wr(GCTL, (rd(GCTL) & ~(3u << 12)) | (2u << 12));
	/* Keep UTMI clocks awake for EP0 commands. Limit this probe to USB 2.0. */
	wr(GUSB2PHYCFG, rd(GUSB2PHYCFG) & ~((1u << 6) | (1u << 8)));
	wr(DCFG, rd(DCFG) & ~(7u | DEV_ADDR_MASK));
	uint64_t ring = usb_pa(dma.events);
	flush_dcache64((uintptr_t)&dma, sizeof(dma), 0);
	wr(GEVNTSIZ, GEVNT_INTMASK);
	wr(GEVNTCOUNT, rd(GEVNTCOUNT) & GEVNT_COUNT_MASK);
	wr(GEVNTADRLO, (uint32_t)ring);
	wr(GEVNTADRHI, (uint32_t)(ring >> 32));
	wr(GEVNTSIZ, GEVNT_INTMASK | sizeof(dma.events));
	wr(DEVTEN, (1u << USB_DISCONNECT) | (1u << USB_RESET) | (1u << USB_CONNECT_DONE) |
	    (1u << USB_OVERFLOW));
	wr(DALEPENA, 0);
	if (!endpoint_command(0, DEP_START_CONFIG, 0, 0, NULL)) {
		return false;
	}
	for (unsigned ep = 0; ep < 2; ep++) {
		if (!endpoint_command(ep, DEP_CONFIGURE, 64u << 3,
		    (ep << 25) | (1u << 8) | (1u << 10), NULL) ||
		    !endpoint_command(ep, DEP_RESERVE, 1, 0, NULL)) {
			return false;
		}
	}
	wr(DALEPENA, 3);
	if (!queue_setup()) {
		return false;
	}
	old_session = *hs_phy & HS_SESSION_MASK;
	session_owned = true;
	update_vbus();
	wr(DCTL, rd(DCTL) | DCTL_RUN);
	return wait_register(DSTS, DSTS_HALTED, 0);
}

__startup_func
static void
thyme_usb_start(void)
{
	DTEntry chosen;
	const void *handoff;
	unsigned size;
	uint32_t enable = 0;
	if (!PE_parse_boot_argn("usb_gadget", &enable, sizeof(enable)) || enable != 1 ||
	    SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-usb-handoff", &handoff, &size) != kSuccess ||
	    size != sizeof(uint32_t) || *(const uint32_t *)handoff != 1) {
		return;
	}
	vm_offset_t scratch = ml_io_map(QSCRATCH_PAGE, 0x1000);
	if (scratch == 0 || hakios_battery_guard_state == NULL) {
		stage = USB_FAILED;
		return;
	}
	hs_phy = (volatile uint32_t *)(scratch + QSCRATCH_HS_PHY_CTRL);
	base = ml_io_map(DWC3_PA, 0x1000);
	dma_pa = kvtophys((vm_offset_t)&dma);
	event_call = thread_call_allocate_with_options(poll_events, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (base == 0 || dma_pa == 0 || event_call == NULL || !take_controller()) {
		hakios_thyme_usb_quiesce();
		stage = USB_FAILED;
		printf("thyme USB: start failed, DSTS=%#x command=%#x\n", hw_status, last_command);
		return;
	}
	stage = USB_RUNNING;
	printf("thyme USB: USB 2.0 EP0 probe started, DSTS=%#x\n", rd(DSTS));
	thread_call_enter(event_call);
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, thyme_usb_start);
