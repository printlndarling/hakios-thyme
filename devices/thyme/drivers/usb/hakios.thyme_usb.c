/*
 * THYME's SM8250 DWC_usb31 peripheral controller. The loader stops its
 * EFI USB driver before handing over this core. Keep the firmware's
 * Qualcomm wrapper/PHY configuration and take over the device registers.
 */
#include <arm/caches_internal.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <os/atomic_private.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include "hakios.thyme_usbmux.h"

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
#define TRB_OWNED               1u
#define TRB_RESIDUAL            0x00ffffffu
#define FULL_SPEED_PACKET       64u
#define HIGH_SPEED_PACKET       512u
#define BULK_OUT                2u
#define BULK_IN                 3u
#define BULK_ENDPOINTS          ((1u << BULK_OUT) | (1u << BULK_IN))

enum {
	DEP_CONFIGURE = 1, DEP_RESERVE = 2, DEP_STALL = 4,
	DEP_START = 6, DEP_END = 8, DEP_START_CONFIG = 9,
};

enum {
	TRB_NORMAL = 1, TRB_SETUP = 2, TRB_STATUS2 = 3, TRB_STATUS3 = 4, TRB_DATA = 5,
};

enum {
	TRANSFER_COMPLETE = 1, TRANSFER_NOT_READY = 3,
	USB_DISCONNECT = 0, USB_RESET = 1, USB_CONNECT_DONE = 2, USB_OVERFLOW = 11,
};

enum usb_phase { USB_SETUP, USB_DATA_IN, USB_STATUS_IN, USB_STATUS_OUT };
enum usb_stage { USB_OFF, USB_OWNED, USB_RUNNING, USB_FAILED, USB_STOPPED, USB_RECONNECTING };

struct usb_trb {
	uint32_t lo, hi, count, control;
} __attribute__((aligned(64)));

#define INTERFACE_PROPERTY u"DeviceInterfaceGUID"
#define INTERFACE_GUID u"{A4083620-32F7-5F01-8504-C2BEFA7DF2A7}"

/* Device-written and CPU-written areas never share a cache line. */
static struct {
	uint32_t events[64];
	struct usb_trb trb[4];
	uint8_t setup[64] __attribute__((aligned(64)));
	uint8_t reply[256] __attribute__((aligned(64)));
	uint8_t echo[HIGH_SPEED_PACKET] __attribute__((aligned(64)));
} dma __attribute__((aligned(4096)));
_Static_assert(sizeof(dma) <= 4096, "the DMA buffers must fit in one page");

static vm_offset_t base;
static volatile uint32_t *hs_phy;
static uint32_t old_session;
static bool session_owned;
static int vbus_override;
static uint64_t dma_pa;
static thread_call_t event_call;
static unsigned event_slot, configuration, active_ep, resource;
static enum usb_phase phase;
static unsigned bulk_ep, bulk_resource, echo_length;
static unsigned bulk_packet = HIGH_SPEED_PACKET;
static int pending_configuration = -1;
static int bulk_out_packets, bulk_in_packets, bulk_out_bytes, bulk_in_bytes;
static int reconnect_requested, reconnect_count;
static int profile, mux_versions, mux_refused, mux_invalid, mux_connections, mux_replies;
static LCK_GRP_DECLARE(mux_locks, "thyme-usbmux");
static LCK_MTX_DECLARE(mux_lock, &mux_locks);
static bool lockdown_open, lockdown_registered;
static uint64_t lockdown_deadline;
static bool have_transfer;
static int stage, last_command, setup_count, reset_count, connect_count, disconnect_count;
static int address_seen, configured, stalled, event_count, hw_status, hw_control;
static int last_setup, last_index, session_changes;
static bool take_controller(void);

static int
lockdown_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *proc)
{
	int error = ENXIO;
	lck_mtx_lock(&mux_lock);
	if (os_atomic_load(&stage, relaxed) == USB_RUNNING) {
		error = EBUSY;
		if (!lockdown_open && !thyme_usbmux_pending()) {
			lockdown_open = true;
			thyme_usbmux_service(true);
			error = 0;
		}
	}
	lck_mtx_unlock(&mux_lock);
	return error;
}

static int
lockdown_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *proc)
{
	lck_mtx_lock(&mux_lock);
	lockdown_open = false;
	thyme_usbmux_service(false);
	wakeup(&lockdown_open);
	lck_mtx_unlock(&mux_lock);
	return 0;
}

static int
lockdown_dev_read(__unused dev_t dev, struct uio *uio, __unused int flags)
{
	user_ssize_t capacity = uio_resid(uio);
	if (capacity <= (user_ssize_t)sizeof(uint32_t)) {
		return EMSGSIZE;
	}
	struct thyme_lockdown_message *message = kalloc_data(sizeof(*message), Z_WAITOK);
	if (message == NULL) {
		return ENOMEM;
	}
	unsigned payload = capacity >= (user_ssize_t)sizeof(*message) ?
	    sizeof(message->data) : (unsigned)capacity - sizeof(message->transaction);
	int error = 0, length = 0;
	lck_mtx_lock(&mux_lock);
	while (lockdown_open && error == 0) {
		int current_stage = os_atomic_load(&stage, relaxed);
		if (current_stage != USB_RUNNING && current_stage != USB_RECONNECTING) {
			break;
		}
		length = thyme_usbmux_request(message, payload);
		if (length != 0) {
			break;
		}
		const struct timespec interval = { .tv_sec = 1 };
		error = msleep(&lockdown_open, &mux_lock, PZERO | PCATCH, "hakios-lockdown", &interval);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
	}
	lck_mtx_unlock(&mux_lock);
	if (length > 0) {
		error = uiomove((char *)message, (int)sizeof(message->transaction) + length, uio);
	} else if (length < 0) {
		error = EMSGSIZE;
	} else if (!error) {
		error = ENXIO;
	}
	kfree_data(message, sizeof(*message));
	return error;
}

static int
lockdown_dev_write(__unused dev_t dev, struct uio *uio, __unused int flags)
{
	user_ssize_t size = uio_resid(uio);
	if (size < (user_ssize_t)sizeof(uint32_t) || size > (user_ssize_t)sizeof(struct thyme_lockdown_message)) {
		return EMSGSIZE;
	}
	struct thyme_lockdown_message *message = kalloc_data(sizeof(*message), Z_WAITOK);
	if (message == NULL) {
		return ENOMEM;
	}
	int error = uiomove((char *)message, (int)size, uio);
	if (!error) {
		lck_mtx_lock(&mux_lock);
		bool accepted = lockdown_open &&
		    thyme_usbmux_submit(message, (unsigned)size - sizeof(message->transaction));
		lck_mtx_unlock(&mux_lock);
		error = accepted ? 0 : ESTALE;
	}
	kfree_data(message, sizeof(*message));
	return error;
}

static const struct cdevsw lockdown_cdevsw = {
	.d_open = lockdown_dev_open, .d_close = lockdown_dev_close,
	.d_read = lockdown_dev_read, .d_write = lockdown_dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0,
};

static void
reset_mux(void)
{
	lck_mtx_lock(&mux_lock);
	thyme_usbmux_reset();
	lockdown_deadline = 0;
	lck_mtx_unlock(&mux_lock);
}

static int
request_reconnect SYSCTL_HANDLER_ARGS
{
	int request = 0;
	int error = sysctl_handle_int(oidp, &request, 0, req);
	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (request != 1) {
		return EINVAL;
	}
	if (os_atomic_load(&stage, relaxed) != USB_RUNNING ||
	    !os_atomic_cmpxchg(&reconnect_requested, 0, 1, release)) {
		return EBUSY;
	}
	return 0;
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, thyme_usb, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "thyme USB gadget");
#define USB_STAT(name, var, desc) \
	SYSCTL_INT(_kern_hakios_thyme_usb, OID_AUTO, name, CTLFLAG_RD | CTLFLAG_LOCKED, &var, 0, desc)
USB_STAT(stage, stage, "0 off, 1 claimed, 2 running, 3 failed, 4 stopped, 5 reconnecting");
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
USB_STAT(bulk_out_packets, bulk_out_packets, "completed bulk OUT transfers");
USB_STAT(bulk_in_packets, bulk_in_packets, "completed bulk IN transfers");
USB_STAT(bulk_out_bytes, bulk_out_bytes, "bytes received on bulk OUT");
USB_STAT(bulk_in_bytes, bulk_in_bytes, "bytes completed on bulk IN");
USB_STAT(reconnects, reconnect_count, "completed software reattachments");
USB_STAT(profile, profile, "1 bulk echo, 2 usbmux");
USB_STAT(mux_versions, mux_versions, "usbmux version replies queued");
USB_STAT(mux_refused, mux_refused, "usbmux closed port resets queued");
USB_STAT(mux_invalid, mux_invalid, "invalid or unsupported usbmux frames");
USB_STAT(mux_connections, mux_connections, "usbmux Lockdown SYN/ACK replies queued");
USB_STAT(mux_replies, mux_replies, "usbmux Lockdown data replies queued");
SYSCTL_PROC(_kern_hakios_thyme_usb, OID_AUTO, reconnect, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, request_reconnect, "I", "write 1 as root to detach and reattach the RAM Boot USB test");

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
start_transfer(unsigned ep, unsigned type, const void *buffer, unsigned length, unsigned *index)
{
	struct usb_trb *trb = &dma.trb[ep];
	uint64_t buffer_pa = usb_pa(buffer);
	uint64_t trb_pa = usb_pa(trb);

	*trb = (struct usb_trb){
		.lo = (uint32_t)buffer_pa, .hi = (uint32_t)(buffer_pa >> 32),
		.count = length, .control = (type << 4) | (1u << 11) | 3u,
	};
	flush_dcache64((uintptr_t)trb, sizeof(*trb), 0);
	return endpoint_command(ep, DEP_START, (uint32_t)(trb_pa >> 32), (uint32_t)trb_pa, index);
}

static bool
queue_control(unsigned ep, unsigned type, const void *buffer, unsigned length)
{
	if (!start_transfer(ep, type, buffer, length, &resource)) {
		return false;
	}
	active_ep = ep;
	have_transfer = true;
	return true;
}

static bool
queue_bulk(unsigned ep, unsigned length)
{
	flush_dcache64((uintptr_t)dma.echo, sizeof(dma.echo), 0);
	if (!start_transfer(ep, TRB_NORMAL, dma.echo, length, &bulk_resource)) {
		return false;
	}
	bulk_ep = ep;
	return true;
}

static bool
stop_bulk(void)
{
	bool ended = bulk_ep == 0 ||
	    endpoint_command(bulk_ep, DEP_END | DEPCMD_FORCE | (bulk_resource << 16), 0, 0, NULL);
	wr(DALEPENA, rd(DALEPENA) & ~BULK_ENDPOINTS);
	bulk_ep = 0;
	echo_length = 0;
	return ended;
}

static bool
configure_bulk(unsigned config)
{
	if (profile == 2) {
		reset_mux();
	}
	if (!stop_bulk()) {
		return false;
	}
	if (config == 0) {
		return true;
	}
	for (unsigned ep = BULK_OUT; ep <= BULK_IN; ep++) {
		uint32_t params = (bulk_packet << 3) | (2u << 1);
		if (ep == BULK_IN) {
			params |= 1u << 17; /* TX FIFO 1 */
		}
		if (!endpoint_command(ep, DEP_CONFIGURE, params, (ep << 25) | (1u << 8), NULL)) {
			return false;
		}
	}
	wr(DALEPENA, rd(DALEPENA) | BULK_ENDPOINTS);
	return queue_bulk(BULK_OUT, bulk_packet);
}

static bool
send_mux(unsigned response)
{
	echo_length = response;
	if (!queue_bulk(BULK_IN, echo_length)) {
		return false;
	}
	if (dma.echo[3] == 0) {
		mux_versions++;
	} else if (dma.echo[21] & 0x04) { /* TCP RST */
		mux_refused++;
	} else if (dma.echo[21] == 0x12) { /* TCP SYN/ACK */
		mux_connections++;
	} else if (response > 28) {
		mux_replies++;
	}
	return true;
}

static bool
pump_usbmux(unsigned received)
{
	lck_mtx_lock(&mux_lock);
	int response = thyme_usbmux_feed(dma.echo, received, dma.echo);
	/* If an unsupported complete frame was consumed, check for another in the same transfer. */
	while (response < 0) {
		mux_invalid++;
		response = thyme_usbmux_feed(NULL, 0, dma.echo);
	}
	bool pending = thyme_usbmux_pending();
	if (pending) {
		clock_interval_to_deadline(2, NSEC_PER_SEC, &lockdown_deadline);
		wakeup(&lockdown_open);
	}
	lck_mtx_unlock(&mux_lock);
	if (response > 0) {
		return send_mux((unsigned)response);
	}
	/* Keep the OUT pipe idle until this request is answered. */
	return pending || queue_bulk(BULK_OUT, bulk_packet);
}

static bool
poll_mux_reply(void)
{
	if (!lockdown_deadline || bulk_ep != 0) {
		return true;
	}
	lck_mtx_lock(&mux_lock);
	int response = thyme_usbmux_complete(dma.echo, mach_absolute_time() >= lockdown_deadline);
	if (response > 0) {
		lockdown_deadline = 0;
	}
	lck_mtx_unlock(&mux_lock);
	return response == 0 || send_mux((unsigned)response);
}

static bool
complete_bulk(unsigned ep)
{
	struct usb_trb *trb = &dma.trb[ep];
	flush_dcache64((uintptr_t)trb, sizeof(*trb), 0);
	if (trb->control & TRB_OWNED) {
		return true; /* completion left over from the previous bus state */
	}
	unsigned residual = trb->count & TRB_RESIDUAL;
	bulk_ep = 0;
	if (ep == BULK_OUT) {
		if (residual > bulk_packet) {
			return false;
		}
		/* Discard cache lines fetched while the controller owned the OUT buffer. */
		flush_dcache64((uintptr_t)dma.echo, sizeof(dma.echo), 0);
		echo_length = bulk_packet - residual;
		bulk_out_packets++;
		bulk_out_bytes += (int)echo_length;
		return profile == 2 ? pump_usbmux(echo_length) : queue_bulk(BULK_IN, echo_length);
	}
	if (residual != 0) {
		return false;
	}
	bulk_in_packets++;
	bulk_in_bytes += (int)echo_length;
	return profile == 2 ? pump_usbmux(0) : queue_bulk(BULK_OUT, bulk_packet);
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
	pending_configuration = -1;
	stalled++;
	return endpoint_command(0, DEP_STALL, 0, 0, NULL) && queue_setup();
}

static unsigned
descriptor(uint16_t value, uint16_t language)
{
	/* 1209:0001 is pid.codes' non-unique private test ID. Keep it off in distributed use. */
	static const uint8_t device[] = {
		18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 1, 0, 2, 1, 1, 2, 0, 1,
	};
	static const uint8_t config[] = {
		9, 2, 32, 0, 1, 1, 0, 0x80, 50,
		9, 4, 0, 0, 2, 0xff, 0, 0, 0,
		7, 5, 0x01, 2, 0, 0, 0,
		7, 5, 0x81, 2, 0, 0, 0,
	};
	static const uint8_t qualifier[] = { 10, 6, 0, 2, 0, 0, 0, 64, 1, 0 };
	unsigned index = value & 0xffu;
	switch (value >> 8) {
	case 1:
		if (index != 0) return 0;
		memcpy(dma.reply, device, sizeof(device));
		if (profile == 2) {
			dma.reply[12] = 3;
		}
		return sizeof(device);
	case 2:
	case 7:
		if (index != 0) return 0;
		memcpy(dma.reply, config, sizeof(config));
		bool other_speed = (value >> 8) == 7;
		unsigned packet = bulk_packet;
		if (other_speed) {
			packet = packet == HIGH_SPEED_PACKET ? FULL_SPEED_PACKET : HIGH_SPEED_PACKET;
		}
		dma.reply[1] = (uint8_t)(value >> 8);
		if (profile == 2) {
			dma.reply[15] = 0xfe;
			dma.reply[16] = 2;
		}
		dma.reply[22] = dma.reply[29] = (uint8_t)packet;
		dma.reply[23] = dma.reply[30] = (uint8_t)(packet >> 8);
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
		const char *product = profile == 2 ? "HakiOS thyme usbmux test" : "HakiOS thyme USB test";
		const char *string = index == 1 ? "HakiOS" : product;
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

static unsigned
winusb_properties(void)
{
	/* The USB wire and the arm64 target both use little-endian integers. */
	static const struct {
		uint32_t size;
		uint16_t version, index, count;
		uint32_t property_size, type;
		uint16_t name_length, name[sizeof(INTERFACE_PROPERTY) / sizeof(uint16_t)];
		uint32_t data_length;
		uint16_t data[sizeof(INTERFACE_GUID) / sizeof(uint16_t)];
	} __attribute__((packed)) properties = {
		.size = 24 + sizeof(INTERFACE_PROPERTY) + sizeof(INTERFACE_GUID),
		.version = 0x0100, .index = 5, .count = 1,
		.property_size = 14 + sizeof(INTERFACE_PROPERTY) + sizeof(INTERFACE_GUID),
		.type = 1, /* REG_SZ */
		.name_length = sizeof(INTERFACE_PROPERTY), .name = INTERFACE_PROPERTY,
		.data_length = sizeof(INTERFACE_GUID), .data = INTERFACE_GUID,
	};
	_Static_assert(sizeof(properties) <= sizeof(dma.reply), "WinUSB descriptor buffer");
	memcpy(dma.reply, &properties, sizeof(properties));
	return sizeof(properties);
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
		pending_configuration = value;
		phase = USB_STATUS_IN;
		return true;
	case 0x8006: /* GET_DESCRIPTOR */
		size = descriptor(value, index);
		if (size == 0) return stall_setup();
		break;
	case 0xc000 | MS_VENDOR_CODE: /* Microsoft extended compatible ID */
		if (value != 0) return stall_setup();
		if (index == 5) {
			size = winusb_properties();
			break;
		}
		if (index != 4) return stall_setup();
		static const uint8_t compatible_id[40] = {
			[0] = 40, [5] = 1, [6] = 4, [8] = 1,
			[17] = 1, [18] = 'W', 'I', 'N', 'U', 'S', 'B',
		};
		memcpy(dma.reply, compatible_id, sizeof(compatible_id));
		size = sizeof(compatible_id);
		break;
	case 0xc100 | MS_VENDOR_CODE:
		if (value != 0 || index != 5) return stall_setup();
		size = winusb_properties();
		break;
	case 0x8008: /* GET_CONFIGURATION */
		if (value != 0 || index != 0 || length != 1) return stall_setup();
		dma.reply[0] = (uint8_t)configuration;
		size = 1;
		break;
	case 0x8000: /* GET_STATUS (device) */
	case 0x8100: /* GET_STATUS (interface) */
	case 0x8200: /* GET_STATUS (endpoint) */
		if (value != 0 || length != 2) return stall_setup();
		if (index != 0 && (setup[0] != 0x82 || (index != 0x80 &&
		    (configuration == 0 || (index != 0x01 && index != 0x81))))) return stall_setup();
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
reset_link(void)
{
	if (profile == 2) {
		reset_mux();
	}
	pending_configuration = -1;
	configuration = 0;
	configured = 0;
	if (!stop_bulk()) {
		return false;
	}
	set_address(0);
	if (phase == USB_SETUP && have_transfer) {
		return true;
	}
	end_transfer();
	return queue_setup();
}

static bool
process_event(uint32_t event)
{
	if (event & 1u) {
		switch ((event >> 8) & 0xfu) {
		case USB_DISCONNECT:
			disconnect_count++;
			return reset_link();
		case USB_RESET:
			reset_count++;
			return reset_link();
		case USB_CONNECT_DONE:
			connect_count++;
			bulk_packet = (rd(DSTS) & 7u) == 0 ? HIGH_SPEED_PACKET : FULL_SPEED_PACKET;
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
	if (ep >= BULK_OUT) {
		return kind != TRANSFER_COMPLETE || ep != bulk_ep || complete_bulk(ep);
	}
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
		pending_configuration = -1;
		return queue_setup();
	}
	switch (phase) {
	case USB_SETUP:
		return handle_setup();
	case USB_DATA_IN:
		phase = USB_STATUS_OUT;
		return true;
	case USB_STATUS_IN:
		if (pending_configuration >= 0) {
			if (!configure_bulk((unsigned)pending_configuration)) {
				return false;
			}
			configuration = (unsigned)pending_configuration;
			configured = pending_configuration;
			pending_configuration = -1;
		}
		return queue_setup();
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
	os_atomic_store(&stage, USB_STOPPED, relaxed);
	stop_bulk();
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
schedule_events(unsigned delay_ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(delay_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(event_call, deadline);
}

static void
poll_events(__unused thread_call_param_t first, __unused thread_call_param_t second)
{
	if (stage == USB_RECONNECTING) {
		if (!take_controller()) {
			hakios_thyme_usb_quiesce();
			os_atomic_store(&stage, USB_FAILED, relaxed);
			return;
		}
		os_atomic_store(&stage, USB_RUNNING, relaxed);
		reconnect_count++;
	}
	if (stage != USB_RUNNING) {
		return;
	}
	if (profile == 2 && !lockdown_registered) {
		vnode_t root = vfs_rootvnode();
		if (root != NULL) {
			vnode_put(root);
			lockdown_registered = true;
			int major = cdevsw_add(-1, &lockdown_cdevsw);
			if (major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600,
			    "%s", THYME_LOCKDOWN_NODE + sizeof("/dev/") - 1) == NULL) {
				cdevsw_remove(major, &lockdown_cdevsw);
				major = -1;
			}
			if (major < 0) {
				printf("thyme USB: cannot register the Lockdown service device\n");
			}
		}
	}
	if (os_atomic_xchg(&reconnect_requested, 0, acquire)) {
		if (profile == 2) {
			reset_mux();
		}
		hakios_thyme_usb_quiesce();
		if (!(hw_status & DSTS_HALTED)) {
			os_atomic_store(&stage, USB_FAILED, relaxed);
			return;
		}
		os_atomic_store(&stage, USB_RECONNECTING, relaxed);
		schedule_events(750);
		return;
	}
	update_vbus();
	hw_control = (int)rd(DCTL);
	hw_status = (int)rd(DSTS);
	unsigned bytes = rd(GEVNTCOUNT) & GEVNT_COUNT_MASK;
	if (bytes > sizeof(dma.events)) {
		hakios_thyme_usb_quiesce();
		os_atomic_store(&stage, USB_FAILED, relaxed);
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
			os_atomic_store(&stage, USB_FAILED, relaxed);
			return;
		}
	}
	if (profile == 2 && !poll_mux_reply()) {
		hakios_thyme_usb_quiesce();
		os_atomic_store(&stage, USB_FAILED, relaxed);
		return;
	}
	schedule_events(1);
}

static bool
take_controller(void)
{
	uint32_t id = rd(GSNPSID);
	hw_status = (int)rd(DSTS);
	if ((id >> 16) != 0x3331u || !(hw_status & DSTS_HALTED) || (rd(DCTL) & DCTL_RUN)) {
		return false;
	}
	os_atomic_store(&stage, USB_OWNED, relaxed);
	wr(DCTL, DCTL_RESET);
	if (!wait_register(DCTL, DCTL_RESET, 0)) {
		return false;
	}
	wr(GCTL, (rd(GCTL) & ~(3u << 12)) | (2u << 12));
	/* Keep UTMI clocks awake for EP0 commands. Limit this probe to USB 2.0. */
	wr(GUSB2PHYCFG, rd(GUSB2PHYCFG) & ~((1u << 6) | (1u << 8)));
	wr(DCFG, rd(DCFG) & ~(7u | DEV_ADDR_MASK));
	address_seen = 0;
	configured = 0;
	configuration = 0;
	pending_configuration = -1;
	event_slot = 0;
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
	for (unsigned ep = 0; ep < 4; ep++) {
		if (ep < BULK_OUT && !endpoint_command(ep, DEP_CONFIGURE, 64u << 3,
		    (ep << 25) | (1u << 8) | (1u << 10), NULL)) {
			return false;
		}
		if (!endpoint_command(ep, DEP_RESERVE, 1, 0, NULL)) {
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
	if (!PE_parse_boot_argn("usb_gadget", &enable, sizeof(enable)) || (enable != 1 && enable != 2) ||
	    SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-usb-handoff", &handoff, &size) != kSuccess ||
	    size != sizeof(uint32_t) || *(const uint32_t *)handoff != 1) {
		return;
	}
	vm_offset_t scratch = ml_io_map(QSCRATCH_PAGE, 0x1000);
	if (scratch == 0 || hakios_battery_guard_state == NULL) {
		os_atomic_store(&stage, USB_FAILED, relaxed);
		return;
	}
	profile = (int)enable;
	hs_phy = (volatile uint32_t *)(scratch + QSCRATCH_HS_PHY_CTRL);
	base = ml_io_map(DWC3_PA, 0x1000);
	dma_pa = kvtophys((vm_offset_t)&dma);
	event_call = thread_call_allocate_with_options(poll_events, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (base == 0 || dma_pa == 0 || event_call == NULL || !take_controller()) {
		hakios_thyme_usb_quiesce();
		os_atomic_store(&stage, USB_FAILED, relaxed);
		printf("thyme USB: start failed, DSTS=%#x command=%#x\n", hw_status, last_command);
		return;
	}
	os_atomic_store(&stage, USB_RUNNING, relaxed);
	printf("thyme USB: USB 2.0 development profile %d started, DSTS=%#x\n", profile, rd(DSTS));
	thread_call_enter(event_call);
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, thyme_usb_start);
