/*
 * The sensor DSP (SLPI) of the Xiaomi Mi 10S (thyme, SM8250), started, and the link with it:
 * the first step of the sensors.
 *
 * The accelerometer, gyroscope, ambient-light and proximity sensors are not on the application
 * processor's I2C buses. They are behind the SLPI -- Qualcomm's Sensor Low Power Island, a
 * Hexagon of its own -- which stays in reset until somebody loads it. The phone's signed image,
 * slpi.mdt and slpi.b00 to slpi.b20 from /vendor/firmware_mnt/image of its Android (~7.8 MB),
 * goes into the 0x2f00000-byte carveout at 0x8e800000, TrustZone authenticates and starts it,
 * and from there everything is conversation through the shared memory: SMP2P for "up", "given
 * up", "stopped"; GLINK for channels; QRTR on the channel "IPCRTR" for the services the DSP
 * offers, among them the SNS client the sensor protocol uses.
 *
 * This file is a copy of the audio DSP's bring-up (devices/thyme/drivers/audio/
 * hakios.thyme_adsp.c) with the SLPI's numbers:
 *   - processor 3 in the shared memory (dt: qcom,smp2p-dsps, qcom,glink/dsps); its SMP2P items
 *     481 (its) and 430 (ours), its last words in item 424; GLINK's three items in our partition
 *     with it (descriptor 478, our queue 479, its 480)
 *   - its doorbells are IPCC client 4: signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 12 (dt: qcom,pas-id = 0x0c); its memory is 0x8e800000, 47 MB
 *     (dt: pil_slpi_region@8e800000)
 *   - the always-on processor is told of it as "slpi"
 *
 * Boot-arg slpi_step=N performs steps 1 to N and stops; slpi=1 is all of them, on a thread beside
 * the system's start, and leaves the DSP up. slpi_keep=1 keeps it up when the steps end;
 * slpi_wait=S, slpi_listen=S, slpi_pause=MS and slpi_new=K are the audio DSP's knobs with the
 * same meanings. What is started is stopped on the way back to the loader.
 *
 * Steps 1-8 are the boot; step 9 takes up GLINK (the IPCRTR channel); step 10 listens to QRTR
 * and writes down the services the DSP tells of. The sensor protocol (SNS over QMI) is the next
 * layer, above QRTR.
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the Wi-Fi's DSP and this one both use (hakios.wifi.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the other DSP's driver */
extern lck_mtx_t hakios_smmu_lock;           /* the apps SMMU's match registers (hakios.wifi.c) */
extern void *hakios_slpi_dma_alloc(size_t size, uint64_t *physical);
extern void hakios_slpi_dma_free(void *address, size_t size);

static LCK_GRP_DECLARE(slpi_lock_group, "hakios-slpi");

#define STEPS                   10
#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios.wifi.c);
 * with slpi=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &slpi_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in hakios.wifi.c) ------------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("slpi: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("slpi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("slpi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (hakios.wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_SLPI               3               /* dt: qcom,remote-pid of qcom,smp2p-dsps and qcom,glink/dsps */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-dsps, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         430
#define ITEM_SMP2P_THEIRS       481
#define ITEM_CRASH_REASON       424
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		/*
		 * The DSP can write this header: padding larger than the item would make the size below wrap to about 4 GB,
		 * and an item that ends beyond its partition would send the readers out of the shared memory. Stop at it.
		 */
		if (entry.padding_data > entry.size ||
		    (uint64_t)at + sizeof(entry) + entry.padding_header + entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		/* (padding larger than the item would make the size below wrap to about 4 GB) */
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at ||
		    entry.padding_data > entry.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_SLPI }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no partition for us and the sensor DSP (0 and 3)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("slpi: %u: the partition for us and the sensor DSP (0 and 3): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("slpi: %u:   items (bytes):%s\n", step, line);
	}
}

/* A diagnostic: the global item table, where SMP2P and GLINK's items may be instead of in our partition. */
static void
show_global(void)
{
	static struct smem_item found[512];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0;
	uint32_t partition = smem_partition(SMEM_GLOBAL, SMEM_GLOBAL, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no global partition\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 512);
	printf("slpi: %u: the global partition at %#lx, %#x bytes; %u items\n", step, SMEM_PHYS + partition, size, count);
	for (unsigned i = 0; i < count && i < 512;) {
		int at = 0;

		for (unsigned n = 0; n < 16 && i < count && i < 512; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("slpi: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-dsps; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("slpi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("slpi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("slpi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("slpi: %u: THE SENSOR DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (hakios.wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_SLPI              4u              /* dt: mboxes of qcom,smp2p-dsps and qcom,glink/dsps */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The sensor DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_SLPI) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("slpi: doorbell: +%llu.%03llu ms: signal %u of the sensor DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as hakios.wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("slpi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	/* (box and size come from the always-on processor: "box + size" could wrap in 32 bits and pass, so it is compared as a difference) */
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box >= aoss.size || size > aoss.size - box || (box & 3)) {
		printf("slpi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("slpi: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("slpi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("slpi: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("slpi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("slpi: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The sensor DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("slpi: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("slpi: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("slpi: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("slpi: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	/* (on a panic's way back to the loader no lock is taken: the other processors are stopped, and one of them may hold this one for good) */
	if (!leaving) {
		lck_mtx_lock(&hakios_rsc_lock);
	}
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("slpi: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		if (!leaving) {
			lck_mtx_unlock(&hakios_rsc_lock);
		}
		return false;
	}
	printf("slpi: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the sensor DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("slpi: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	if (!leaving) {
		lck_mtx_unlock(&hakios_rsc_lock);
	}
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the sensor DSP in the shared memory (0x80900000), and its items");
	show_partition();
	show_global();
	if (dsp_words(&words)) {
		show_words("the DSP's item (481) is there: it has run since the phone was started", &words);
	} else {
		printf("slpi: %u: the DSP's SMP2P item (481) is not there: it has not run since the phone was started\n", step);
	}
	printf("slpi: %u: our SMP2P item (430): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("slpi: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_SLPI, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define HAKIOS_SLPI_MEMORY     0x8e800000u     /* THYME: reserved-memory pil_slpi_region@8e800000 */
#define HAKIOS_SLPI_SIZE       0x02f00000u
#define UMI_SLPI_MEMORY         0x8e800000u     /* Mi 10: reserved-memory pil_slpi_region@8bb00000 */
#define UMI_SLPI_SIZE           0x02f00000u
#define PAS_SLPI                12               /* dt: qcom,pas-id */

/* The SLPI's PAS carveout (dt: pil_slpi_region@8e800000). */
static uint32_t slpi_memory = HAKIOS_SLPI_MEMORY;
static uint32_t slpi_memory_size = HAKIOS_SLPI_SIZE;


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-slpi/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only slpi=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > slpi_memory_size) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("slpi: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* For the sound (hakios.sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
const uint8_t *
hakios_slpi_fw_file(const char *name, uint64_t *size)
{
	return root_file(name, size);
}

void
hakios_slpi_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/* A file of fw/ in the small image (SLPI=1 userland/build.sh puts fw-slpi/ there), else of /fw on the root (above). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios.image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			return data;
		}
	}
	printf("slpi: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-slpi/ and run SYSTEM=1 userland/build.sh (or SLPI=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MDT_RELOCATABLE         (1u << 27)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size, segment_base;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0, highest = 0;
	uint32_t count, hashes = 0, lowest = ~0u;
	unsigned loadable = 0;
	bool relocatable = false;
	uint8_t first[4] = { 0 };

	announce("find the sensor DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("slpi.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	/* (a file of 1 to 4 bytes is shorter than what is compared and printed: both look only at what is there) */
	memcpy(first, header, size < sizeof(first) ? size : sizeof(first));
	if (size < 5 || memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size ||
	    size > PAGE_SIZE) {
		printf("slpi: %u: fw/slpi.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, first[0],
		    first[1], first[2], first[3], count, size);
		goto fail;
	}
	/* Qualcomm's relocatable MDTs use segment addresses relative to their lowest segment. PAS
	 * memory setup relocates that span into the reserved carveout from the device tree. */
	for (uint32_t i = 0; i < count; i++) {
		uint64_t end;

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.file_size > segment.memory_size) {
			printf("slpi: %u: segment %u has %u file bytes but only %u memory bytes\n", step, i, segment.file_size,
			    segment.memory_size);
			goto fail;
		}
		relocatable |= (segment.flags & MDT_RELOCATABLE) != 0;
		end = (uint64_t)segment.paddr + segment.memory_size;
		if (end > UINT32_MAX) {
			printf("slpi: %u: segment %u wraps the firmware address space\n", step, i);
			goto fail;
		}
		if (segment.paddr < lowest) {
			lowest = segment.paddr;
		}
		if (end > highest) {
			highest = end;
		}
		loadable++;
	}
	image.segment_base = relocatable ? lowest : slpi_memory;
	if (loadable == 0 || lowest < image.segment_base || highest < image.segment_base ||
	    highest - image.segment_base > slpi_memory_size ||
	    ((highest - image.segment_base + 0xfff) & ~0xfffull) > slpi_memory_size) {
		printf("slpi: %u: the image span %#x..%#llx (%s) does not fit the DSP's %#x-byte carveout at %#x\n", step, lowest,
		    highest, relocatable ? "relocatable" : "fixed", slpi_memory_size, slpi_memory);
		goto fail;
	}
	image.size = (uint32_t)((highest - image.segment_base + 0xfff) & ~0xfffull);
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		unsigned s = image.segments++;
		image.segment[s].at = segment.paddr - image.segment_base;
		image.segment[s].file_size = segment.file_size;
		image.segment[s].memory_size = segment.memory_size;
		image.segment[s].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "slpi.b%02u", i);
			image.segment[s].file = image_file(name, &file_size);
			if (image.segment[s].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("slpi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segment[s].file_size = (uint32_t)file_size;
				goto fail;
			}
			files += file_size;
		}
	}
	if (image.segments != loadable || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("slpi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	printf("slpi: %u: the image is whole (%s, %s MDT): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, "
	    "span %#x from %#x relocated into the DSP's %#x-byte carveout at %#x\n", step,
	    from_root ? "from /fw on the root" : "from the small image", relocatable ? "relocatable" : "fixed", size, hashes,
	    image.segments, files, image.size, image.segment_base, slpi_memory_size, slpi_memory);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &slpi_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("slpi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("slpi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("slpi: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	/* (step_our_item() writes the whole struct there without a bounds check: it has to lie inside the SMEM window) */
	return at != 0 && size >= sizeof(struct smp2p) && (uint64_t)at + sizeof(struct smp2p) <= SMEM_SIZE ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("slpi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("slpi: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("slpi: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("slpi: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("slpi: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("slpi: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("slpi: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the sensor DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("slpi: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE SENSOR DSP ACKNOWLEDGES THE STOP" : "THE SENSOR DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);
static void fastrpc_smmu_restore(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("slpi: %u: %s: the sensor DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the sensor DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_SLPI, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT SHUT THE SENSOR DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("slpi: %u: THE SENSOR DSP IS SHUT DOWN\n", step);
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_SLPI << 16 | IPCC_SMP2P);
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: slpi, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	fastrpc_smmu_restore();
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an sensor DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("slpi_again")) {
			printf("slpi: %u: THE SENSOR DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (slpi_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("slpi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("slpi: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the sensor DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_SLPI, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("slpi: %u: TRUSTZONE ACCEPTS THE SENSOR DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_SLPI, slpi_memory, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("slpi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, slpi_memory);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static uint8_t
segment_byte(unsigned s, uint32_t at)
{
	return at < image.segment[s].file_size ? image.segment[s].file[at] : 0;
}

static bool
step_image(void)
{
	volatile uint8_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the sensor DSP's reserved memory, with zeroes where a segment is longer than its "
	    "file, and read them back");
	dsp_memory = (volatile uint8_t *)ml_io_map_unmappable(slpi_memory, slpi_memory_size, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, slpi_memory);
	for (unsigned s = 0; s < image.segments; s++) {
		uint32_t at = image.segment[s].at, i = 0, size = image.segment[s].memory_size;

		/* Keep exact segment boundaries. Most MDTs are word-aligned, but the UMI image ends
		 * one read-only segment on a four-byte boundary, before its separate BSS segment. */
		while (i < size && ((at + i) & 7)) {
			dsp_memory[at + i] = segment_byte(s, i);
			i++;
		}
		for (; i + 8 <= size; i += 8) {
			volatile uint64_t *to = (volatile uint64_t *)(dsp_memory + at + i);

			*to = segment_word(s, i);
		}
		for (; i < size; i++) {
			dsp_memory[at + i] = segment_byte(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		uint32_t at = image.segment[s].at, i = 0, size = image.segment[s].memory_size;

		while (i < size && ((at + i) & 7)) {
			wrong += dsp_memory[at + i] != segment_byte(s, i);
			i++;
		}
		for (; i + 8 <= size; i += 8) {
			volatile uint64_t *from = (volatile uint64_t *)(dsp_memory + at + i);

			wrong += *from != segment_word(s, i);
		}
		for (; i < size; i++) {
			wrong += dsp_memory[at + i] != segment_byte(s, i);
		}
	}
	printf("slpi: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, slpi_memory_size);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* An item made in the partition, as Linux's qcom_smem_alloc_private() makes one. */
static bool
smem_private_item_make(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("slpi: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock for item %u", step, item);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("slpi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("slpi: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			printf("slpi: %u: item %u appeared while the allocation lock was held: nothing is written\n", step, item);
			goto out;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("slpi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("slpi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("slpi: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

/* Our SMP2P item and GLINK's 16 KiB host-to-DSP queue are private items in this partition. */
static bool
our_item_make(uint32_t partition)
{
	return smem_private_item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p));
}

/* Repair a malformed old entry only when its allocation is exactly the requested size. */
static bool
smem_private_item_repair_padding(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, expected_padding = item_size - bytes;
	uint32_t owner = ~0u, at;
	bool repaired = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);
	need(&smem_lock);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("slpi: %u: cannot inspect item %u: shared-memory lock stays with processor %u\n", step, item, owner);
		goto out;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("slpi: %u: cannot inspect item %u: the partition's marks make no sense\n", step, item);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			if (entry.size != item_size || entry.padding_header != 0 || entry.padding_data > entry.size) {
				printf("slpi: %u: item %u cannot be repaired safely: allocation %u bytes, padding %u/%u\n", step, item,
				    entry.size, entry.padding_header, entry.padding_data);
				goto out;
			}
			if (entry.padding_data != expected_padding) {
				printf("slpi: %u: correcting item %u padding from %u to %u bytes; its allocation remains %u bytes\n", step,
				    item, entry.padding_data, expected_padding, entry.size);
				smem_poke(partition + at + 8, expected_padding | (uint32_t)entry.padding_header << 16);
				settle();
			}
			repaired = true;
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (!repaired) {
		printf("slpi: %u: item %u was not found in this private partition to repair\n", step, item);
	}
out:
	if (owner == LOCK_US) {
		wr(smem_lock.base + LOCK_OWNER, 0);
	}
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return repaired;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (430) in our partition with the sensor DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("slpi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("slpi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("slpi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_SLPI << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("slpi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	{
		uint32_t queue_size = 0, queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);

		if (queue != 0) {
			if (queue_size != 0x4000) {
				printf("slpi: %u: GLINK's host queue (item %u) reads as %u bytes; checking its private allocation\n", step,
				    ITEM_GLINK_OUR_QUEUE, queue_size);
				if (!smem_private_item_repair_padding(partition, ITEM_GLINK_OUR_QUEUE, 0x4000)) {
					return false;
				}
				queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);
				if (queue == 0 || queue_size != 0x4000) {
					printf("slpi: %u: GLINK's host queue (item %u) remains the wrong size (%u; expected 16384)\n", step,
					    ITEM_GLINK_OUR_QUEUE, queue_size);
					return false;
				}
			}
			printf("slpi: %u: GLINK's host queue (item %u) is already there, at %#lx, %u bytes\n", step,
			    ITEM_GLINK_OUR_QUEUE, SMEM_PHYS + queue, queue_size);
		} else {
			if (!smem_private_item_make(partition, ITEM_GLINK_OUR_QUEUE, 0x4000)) {
				return false;
			}
			queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);
			if (queue == 0 || queue_size != 0x4000) {
				printf("slpi: %u: GLINK's host queue (item %u) did not read back at 16384 bytes\n", step,
				    ITEM_GLINK_OUR_QUEUE);
				return false;
			}
			printf("slpi: %u: made GLINK's host queue (item %u), at %#lx, %u bytes\n", step,
			    ITEM_GLINK_OUR_QUEUE, SMEM_PHYS + queue, queue_size);
		}
	}
	{
		uint32_t descriptor_size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &descriptor_size);

		if (descriptor == 0) {
			if (!smem_private_item_make(partition, ITEM_GLINK_DESCRIPTOR, 32)) {
				return false;
			}
			descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &descriptor_size);
			if (descriptor == 0 || descriptor_size != 32) {
				printf("slpi: %u: GLINK's descriptor (item %u) did not read back at 32 bytes\n", step,
				    ITEM_GLINK_DESCRIPTOR);
				return false;
			}
			for (uint32_t at = 0; at < 32; at += 4) {
				smem_poke(descriptor + at, 0);
			}
			settle();
			printf("slpi: %u: made GLINK's descriptor (item %u), at %#lx, 32 bytes\n", step,
			    ITEM_GLINK_DESCRIPTOR, SMEM_PHYS + descriptor);
		} else if (descriptor_size != 32) {
			printf("slpi: %u: GLINK's descriptor (item %u) is %u bytes, not 32\n", step,
			    ITEM_GLINK_DESCRIPTOR, descriptor_size);
			return false;
		} else {
			printf("slpi: %u: GLINK's descriptor (item %u) is already there, at %#lx\n", step,
			    ITEM_GLINK_DESCRIPTOR, SMEM_PHYS + descriptor);
		}
	}
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the sensor services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("slpi: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("slpi_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the sensor DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_SLPI; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_SLPI << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_SLPI << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: slpi, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the sensor DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_SLPI, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT START THE SENSOR DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: slpi, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("slpi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE SENSOR DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("slpi: %u: THE SENSOR DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("slpi: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("slpi: %u: THE SENSOR DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("slpi: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (430) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("slpi: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("slpi: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("slpi: %u: %s\n", step, spoke ? "THE SENSOR DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE SENSOR DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (hakios.wifi.c; items 478 to 480 of our partition with the sensor DSP) ------------------- */

#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,slpi_qrtr), and apr_audio_svc, which
 * carries APR, the sensor services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNEL_FASTRPC, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &slpi_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_SLPI_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
		/* the DSP's FastRPC: the AP serves its registry files (dt: qcom,msm_fastrpc_rpmsg, intents 0x64 x 64) */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 4, .intents = { { 0x64, 24 }, { 0, 0 }, { 0, 0 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	/* (the descriptor and both queues are read and written without further checks: all three must lie inside the SMEM window) */
	if (glink.descriptor == 0 || size != 32 || (uint64_t)glink.descriptor + size > SMEM_SIZE) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7) &&
	       (uint64_t)glink.ours + glink.ours_size <= SMEM_SIZE && (uint64_t)glink.theirs + glink.theirs_size <= SMEM_SIZE;
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	/* (and inside the SMEM window: the words below are read and written without a bounds check) */
	if (descriptor == 0 || size != 32 || (uint64_t)descriptor + size > SMEM_SIZE) {
		printf("slpi: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("slpi: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("slpi: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("slpi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		/* (a buffer of another size from an earlier run is given back first: replacing the pointer would leak it) */
		if (channel->our_intents[i].data != NULL) {
			kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
			channel->our_intents[i].data = NULL;
		}
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length;
		/*
		 * The length is worked out in 64 bits: the DSP writes the fields it comes from, and in 32 bits a value such as
		 * 0xfffffff8 wrapped it to 0, so that the message was never taken out and this loop never ended (with glink_lock held).
		 */
		uint64_t wide = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			wide += message.param2;
		} else if (message.command == GLINK_INTENT) {
			wide += 8ull * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			wide = 16ull + words[0];
		}
		wide = (wide + 7) & ~7ull;
		if (wide > glink.theirs_size - 8) {
			printf("slpi: glink: A MESSAGE OF %llu BYTES (%s %u %#x): not believed, and nothing more is read\n", wide,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		length = (uint32_t)wide;
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("slpi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("slpi: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("slpi: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("slpi: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("slpi: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("slpi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("slpi: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (hakios.wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
static uint32_t sns_enabled;             /* sns=1: print every QRTR datagram while the SNS client runs */

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("slpi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("slpi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("slpi: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("slpi: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("slpi: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

#define SNS_OUR_PORT            0x100u
#define SNS_REG_PORT            0x10fu          /* the AP's Sensor Registry service, as the mainline sns-reg serves it */

static void sns_input(const uint8_t *data, uint32_t length);
static void sns_reg_input(uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("slpi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	/* (the datagram's size is the DSP's word: "at + size" could wrap in 32 bits and pass, so it is compared as a difference) */
	if (header.size == 0 || at > length || header.size > length - at) {
		printf("slpi: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (sns_enabled) {
		printf("slpi: qrtr: < type %u from %u:%u to %u:%u, %u bytes\n", header.type, header.from_node, header.from_port,
		    header.to_node, header.to_port, header.size);
	}
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port == SNS_OUR_PORT) {
		sns_input(data + at, header.size);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port == SNS_REG_PORT) {
		sns_reg_input(header.from_node, header.from_port, data + at, header.size);
		return;
	}
	printf("slpi: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- SNS: the sensor protocol above QRTR (a first client, for the accelerometer) --------------------- */

#define SNS_SERVICE             400u            /* QRTR service 400 (0x190): the SNS client */
#define SNS_MSG_SUID_REQ        512u            /* SNS_SUID_MSGID_SNS_SUID_REQ */
#define SNS_MSG_SENSOR_CONFIG   513u            /* SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_CONFIG */
#define SNS_MSG_SUID_EVENT      768u            /* SNS_SUID_MSGID_SNS_SUID_EVENT */
#define SNS_MSG_SENSOR_EVENT    1025u           /* SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT */

static uint8_t sns_suid[16];
static bool sns_have_suid, sns_have_registry;
static unsigned sns_txn = 1, sns_samples;
static uint8_t sns_client_byte;         /* the TLV 0x10 byte: 0 as sensh sends it, and what the service says to others */

static bool
sns_server(uint32_t *node, uint32_t *port)
{
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == SNS_SERVICE) {
			*node = qrtr_servers[i].node;
			*port = qrtr_servers[i].port;
			return true;
		}
	}
	return false;
}

static uint8_t *
sns_varint(uint8_t *p, uint64_t value)
{
	while (value >= 0x80) {
		*p++ = (uint8_t)(value | 0x80);
		value >>= 7;
	}
	*p++ = (uint8_t)value;
	return p;
}

static uint8_t *
sns_key(uint8_t *p, uint32_t field, uint32_t wire)
{
	return sns_varint(p, (uint64_t)field << 3 | wire);
}

static uint8_t *
sns_bytes(uint8_t *p, uint32_t field, const void *data, uint32_t length)
{
	p = sns_key(p, field, 2);
	p = sns_varint(p, length);
	memcpy(p, data, length);
	return p + length;
}

static uint8_t *
sns_fixed32(uint8_t *p, uint32_t field, uint32_t value)
{
	p = sns_key(p, field, 5);
	memcpy(p, &value, 4);
	return p + 4;
}

static uint8_t *
sns_fixed64(uint8_t *p, uint32_t field, uint64_t value)
{
	p = sns_key(p, field, 1);
	memcpy(p, &value, 8);
	return p + 8;
}

static uint8_t *
sns_float(uint8_t *p, uint32_t field, float value)
{
	p = sns_key(p, field, 5);
	memcpy(p, &value, 4);
	return p + 4;
}

static uint64_t
sns_read_varint(const uint8_t **at, const uint8_t *end)
{
	uint64_t value = 0;
	unsigned shift = 0;

	while (*at < end && shift < 64) {
		uint8_t byte = *(*at)++;

		value |= (uint64_t)(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0) {
			break;
		}
		shift += 7;
	}
	return value;
}

static uint32_t
sns_request(uint32_t msg_id, const uint8_t suid[16], const void *payload, uint32_t payload_length, uint8_t *out)
{
	uint8_t suid_bytes[32], pm[16], body[64];
	uint8_t *p = out, *q;
	uint32_t suid_length, pm_length, body_length;
	uint64_t low, high;

	memcpy(&low, suid, 8);
	memcpy(&high, suid + 8, 8);
	q = suid_bytes;
	q = sns_fixed64(q, 1, low);
	q = sns_fixed64(q, 2, high);
	suid_length = (uint32_t)(q - suid_bytes);
	q = pm;
	q = sns_varint(q, 0x08);        /* client_proc_type = 1 */
	q = sns_varint(q, 1);
	q = sns_varint(q, 0x10);        /* delivery_type = 0 */
	q = sns_varint(q, 0);
	pm_length = (uint32_t)(q - pm);
	q = body;
	if (payload != NULL && payload_length != 0) {
		q = sns_bytes(q, 2, payload, payload_length);
	}
	q = sns_varint(q, 0x18);        /* is_passive = 0 */
	q = sns_varint(q, 0);
	body_length = (uint32_t)(q - body);
	p = sns_bytes(p, 1, suid_bytes, suid_length);
	p = sns_fixed32(p, 2, msg_id);
	p = sns_bytes(p, 3, pm, pm_length);
	p = sns_bytes(p, 4, body, body_length);
	return (uint32_t)(p - out);
}

static int
sns_qmi_send(uint32_t msg_id, const uint8_t suid[16], const void *payload, uint32_t payload_length)
{
	uint8_t proto[512], qmi[600];
	uint32_t proto_length, qmi_length, node, port;
	uint8_t *p;

	if (!sns_server(&node, &port)) {
		printf("slpi: sns: the SNS client service (%u) is not in the DSP's list\n", SNS_SERVICE);
		return ENOENT;
	}
	proto_length = sns_request(msg_id, suid, payload, payload_length, proto);
	if (proto_length + 32 > sizeof(qmi)) {
		return EINVAL;
	}
	p = qmi;
	*p++ = 0;                       /* request */
	*p++ = (uint8_t)(sns_txn & 0xff);
	*p++ = (uint8_t)(sns_txn >> 8);
	sns_txn++;
	*p++ = 0x20;                    /* SNS_CLIENT_REQ_V01 */
	*p++ = 0;
	*p++ = 0;                       /* the QMI length, filled below */
	*p++ = 0;
	*p++ = 0x01;                    /* TLV 0x01: the protobuf, as the SSC array TLV (outer = inner + 2) */
	*p++ = (uint8_t)((proto_length + 2) & 0xff);
	*p++ = (uint8_t)((proto_length + 2) >> 8);
	*p++ = (uint8_t)(proto_length & 0xff);
	*p++ = (uint8_t)(proto_length >> 8);
	memcpy(p, proto, proto_length);
	p += proto_length;
	*p++ = 0x10;                    /* TLV 0x10: the required byte */
	*p++ = 1;
	*p++ = sns_client_byte;
	*p++ = 0;
	qmi_length = (uint32_t)(p - qmi);
	qmi[5] = (uint8_t)((qmi_length - 7) & 0xff);
	qmi[6] = (uint8_t)((qmi_length - 7) >> 8);
	printf("slpi: sns: > QMI %u, %u bytes, to node %u port %u, TLV 0x10=%u\n", msg_id, qmi_length, node, port,
	    sns_client_byte);
	return qrtr_out_packet(QRTR_DATA, SNS_OUR_PORT, node, port, qmi, qmi_length);
}

static void
sns_config(void)
{
	uint8_t inner[8];
	uint8_t *p = inner;

	p = sns_float(p, 1, 10.0f);     /* samples a second */
	sns_qmi_send(SNS_MSG_SENSOR_CONFIG, sns_suid, inner, (uint32_t)(p - inner));
	printf("slpi: sns: asked the accelerometer for 10 samples a second\n");
}

static void
sns_sensor_event(const uint8_t *data, uint32_t length)
{
	const uint8_t *at = data, *end = data + length;
	float values[8];
	unsigned count = 0;

	while (at < end) {
		uint64_t key = sns_read_varint(&at, end);
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);

		if (field == 1 && wire == 5 && at + 4 <= end && count < 8) {
			memcpy(&values[count], at, 4);
			count++;
			at += 4;
		} else if (wire == 0) {
			(void)sns_read_varint(&at, end);
		} else if (wire == 1) {
			at += 8;
		} else if (wire == 2) {
			uint64_t n = sns_read_varint(&at, end);

			if (n > (uint64_t)(end - at)) {
				break;
			}
			at += n;
		} else {
			break;
		}
	}
	if (count >= 3 && sns_samples < 20) {
		printf("slpi: sns: accelerometer %u: %.4f %.4f %.4f\n", ++sns_samples, values[0], values[1], values[2]);
	}
}

static void
sns_suid_event(const uint8_t *data, uint32_t length)
{
	const uint8_t *at = data, *end = data + length;
	uint8_t suid[16] = { 0 };
	char name[32] = "";

	while (at < end) {
		uint64_t key = sns_read_varint(&at, end);
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);

		if (field == 1 && wire == 2) {
			uint64_t n = sns_read_varint(&at, end);

			if (n >= sizeof(name)) {
				n = sizeof(name) - 1;
			}
			memcpy(name, at, n);
			name[n] = 0;
			at += n;
		} else if (field == 2 && wire == 2) {
			uint64_t n = sns_read_varint(&at, end);
			const uint8_t *send = at + n;

			while (at < send) {
				uint64_t k = sns_read_varint(&at, send);
				uint32_t f = (uint32_t)(k >> 3), w = (uint32_t)(k & 7);

				if (w == 1 && at + 8 <= send) {
					if (f == 1) {
						memcpy(suid, at, 8);
					} else if (f == 2) {
						memcpy(suid + 8, at, 8);
					}
					at += 8;
				} else {
					break;
				}
			}
			at = send;
		} else if (wire == 0) {
			(void)sns_read_varint(&at, end);
		} else if (wire == 1) {
			at += 8;
		} else if (wire == 2) {
			uint64_t n = sns_read_varint(&at, end);

			if (n > (uint64_t)(end - at)) {
				break;
			}
			at += n;
		} else {
			break;
		}
	}
	if (name[0] != 0) {
		printf("slpi: sns: \"%s\" has SUID %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n", name,
		    suid[15], suid[14], suid[13], suid[12], suid[11], suid[10], suid[9], suid[8], suid[7], suid[6], suid[5],
		    suid[4], suid[3], suid[2], suid[1], suid[0]);
		if (strcmp(name, "accel") == 0) {
			memcpy(sns_suid, suid, sizeof(sns_suid));
			sns_have_suid = true;
			sns_config();
		} else if (strcmp(name, "registry") == 0) {
			sns_have_registry = true;
		}
	}
}

static void
sns_event(const uint8_t *data, uint32_t length)
{
	const uint8_t *at = data, *end = data + length;

	while (at < end) {
		uint64_t key = sns_read_varint(&at, end);
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);

		if (field == 2 && wire == 2) {
			uint64_t n = sns_read_varint(&at, end);
			const uint8_t *e = at, *ee = at + n;
			uint32_t msg_id = 0;
			const uint8_t *payload = NULL;
			uint32_t payload_length = 0;

			while (e < ee) {
				uint64_t k = sns_read_varint(&e, ee);
				uint32_t f = (uint32_t)(k >> 3), w = (uint32_t)(k & 7);

				if (f == 1 && w == 5 && e + 4 <= ee) {
					memcpy(&msg_id, e, 4);
					e += 4;
				} else if (f == 2 && w == 1 && e + 8 <= ee) {
					e += 8;
				} else if (f == 3 && w == 2) {
					uint64_t l = sns_read_varint(&e, ee);

					if (l > (uint64_t)(ee - e)) {
						break;
					}
					payload = e;
					payload_length = (uint32_t)l;
					e += l;
				} else {
					break;
				}
			}
			if (msg_id == SNS_MSG_SUID_EVENT && payload != NULL) {
				sns_suid_event(payload, payload_length);
			} else if (msg_id == SNS_MSG_SENSOR_EVENT && payload != NULL) {
				sns_sensor_event(payload, payload_length);
			} else if (msg_id != 0) {
				printf("slpi: sns: event %u, %u bytes\n", msg_id, payload_length);
			}
			at += n;
		} else if (wire == 0) {
			(void)sns_read_varint(&at, end);
		} else if (wire == 1) {
			at += 8;
		} else if (wire == 2) {
			uint64_t n = sns_read_varint(&at, end);

			if (n > (uint64_t)(end - at)) {
				break;
			}
			at += n;
		} else {
			break;
		}
	}
}

static void
sns_input(const uint8_t *data, uint32_t length)
{
	uint16_t message, qmi_length;
	uint32_t at;

	if (length < 7) {
		return;
	}
	message = (uint16_t)(data[3] | data[4] << 8);
	qmi_length = (uint16_t)(data[5] | data[6] << 8);
	if (qmi_length > length - 7) {
		qmi_length = (uint16_t)(length - 7);
	}
	if (sns_enabled) {
		printf("slpi: sns: < QMI %u, %u bytes:", message, length);
		for (uint32_t i = 0; i < length && i < 40; i++) {
			printf(" %02x", data[i]);
		}
		printf("\n");
	}
	at = 7;
	while (at + 3 <= 7 + qmi_length) {
		uint8_t type = data[at];
		uint16_t l = (uint16_t)(data[at + 1] | data[at + 2] << 8);

		at += 3;
		if (at + l > 7 + qmi_length) {
			break;
		}
		if (type == 0x02 && message == 0x20 && l >= 4) {
			printf("slpi: sns: < QMI result %u, error %u\n", data[at] | data[at + 1] << 8,
			    data[at + 2] | data[at + 3] << 8);
		} else if (type == 0x02) {
			/* the SSC sends an array TLV: outer length, then inner length, then the payload */
			if (l >= 2 && (uint16_t)(data[at] | data[at + 1] << 8) + 2 == l) {
				sns_event(data + at + 2, (uint32_t)(l - 2));
			} else {
				sns_event(data + at, l);
			}
		} else if (type == 0x11 && message == 0x20) {
			uint16_t result = l >= 2 ? (uint16_t)(data[at] | data[at + 1] << 8) : 0xffff;

			printf("slpi: sns: < response: result %u\n", result);
		}
		at += l;
	}
}

static void
sns_ask(const char *type)
{
	uint8_t inner[32], suid[16];
	uint8_t *p = inner;

	memset(suid, 0xab, sizeof(suid));
	p = sns_bytes(p, 1, type, (uint32_t)strlen(type));
	p = sns_varint(p, 0x10);        /* unkfield_2 = 1 */
	p = sns_varint(p, 1);
	p = sns_varint(p, 0x18);        /* unkfield_3 = 0 */
	p = sns_varint(p, 0);
	sns_qmi_send(SNS_MSG_SUID_REQ, suid, inner, (uint32_t)(p - inner));
}

static void
sns_start(void)
{
	uint32_t node = 0, port = 0;

	sns_enabled = 1;
	/* register our port with the router for this service, so that its answers come back */
	if (sns_server(&node, &port)) {
		struct qrtr_control lookup = { .command = QRTR_NEW_LOOKUP, .words = { SNS_SERVICE, 1, 0 } };

		printf("slpi: sns: looking up service %u from our port %#x\n", SNS_SERVICE, SNS_OUR_PORT);
		qrtr_out_packet(QRTR_NEW_LOOKUP, SNS_OUR_PORT, node, QRTR_CONTROL, &lookup, sizeof(lookup));
		sleep_ms(50);
	}
	{
		static const uint8_t variants[] = { 0, 1, 2, 0xff };

		for (unsigned v = 0; v < sizeof(variants); v++) {
			sns_client_byte = variants[v];
			sns_ask("registry");
			sns_ask("accel");
			sleep_ms(200);
		}
		sns_client_byte = 0;
	}
	/* the sensor process may still be initialising at three seconds: ask again for a while */
	for (unsigned attempt = 0; attempt < 40 && !(sns_have_registry && sns_have_suid); attempt++) {
		sns_ask("registry");
		sns_ask("accel");
		sleep_ms(500);
	}
	printf("slpi: sns: registry %s, accelerometer %s\n", sns_have_registry ? "ANSWERED" : "silent",
	    sns_have_suid ? "ANSWERED" : "silent");
}

/* ---- the AP's Sensor Registry service (0x10f), as mainline's sns-reg serves it ----------------------- */

static void
sns_reg_advertise(void)
{
	struct qrtr_control server = { .command = QRTR_NEW_SERVER, .words = { 0x10f, 2, 1, SNS_REG_PORT } };

	if (qrtr_peer == 0) {
		return;
	}
	qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &server, sizeof(server));
	printf("slpi: qrtr: advertised the sensor registry service %#x at node 1 port %#x\n", 0x10f, SNS_REG_PORT);
}

static void
sns_reg_input(uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
	uint8_t out[32];
	uint8_t *p = out;
	uint16_t txn, msg, id = 0xffff;
	uint32_t at;

	if (length < 7) {
		return;
	}
	txn = (uint16_t)(data[1] | data[2] << 8);
	msg = (uint16_t)(data[3] | data[4] << 8);
	for (at = 7; at + 3 <= length;) {
		uint8_t type = data[at];
		uint16_t l = (uint16_t)(data[at + 1] | data[at + 2] << 8);

		at += 3;
		if (at + l > length) {
			break;
		}
		if (type == 0x01 && l >= 2) {
			id = (uint16_t)(data[at] | data[at + 1] << 8);
		}
		at += l;
	}
	printf("slpi: sns-reg: group %u requested by node %u port %u (QMI %u, %u bytes)\n", id, node, port, msg, length);
	p = out;
	*p++ = 2;                       /* response */
	*p++ = (uint8_t)(txn & 0xff);
	*p++ = (uint8_t)(txn >> 8);
	*p++ = 0x04;                    /* SNS_REG_GROUP_MSG_ID */
	*p++ = 0;
	*p++ = 0;                       /* length, below */
	*p++ = 0;
	*p++ = 0x02;                    /* result: failure (no registry map yet) */
	*p++ = 2;
	*p++ = 0;
	*p++ = 1;
	*p++ = 0;
	*p++ = 0x03;                    /* the group asked for */
	*p++ = 2;
	*p++ = 0;
	*p++ = (uint8_t)(id & 0xff);
	*p++ = (uint8_t)(id >> 8);
	{
		uint32_t l = (uint32_t)(p - out);

		out[5] = (uint8_t)((l - 7) & 0xff);
		out[6] = (uint8_t)((l - 7) >> 8);
	}
	qrtr_out_packet(QRTR_DATA, SNS_REG_PORT, node, port, out, (uint32_t)(p - out));
}

/* ---- FastRPC: the DSP's calls to the AP (the registry file service) ---------------------------------- */

struct fastrpc_msg {
	int32_t client_id;
	int32_t tid;
	uint64_t ctx;
	uint32_t handle;
	uint32_t sc;
	uint64_t addr;
	uint64_t size;
};

static volatile bool fastrpc_rsp_seen;
static uint64_t fastrpc_rsp_ctx;
static int32_t fastrpc_rsp_retval;

static void
fastrpc_input(const uint8_t *data, uint32_t length)
{
	printf("slpi: fastrpc: %u bytes:", length);
	for (uint32_t i = 0; i < length && i < 64; i++) {
		printf(" %02x", data[i]);
	}
	printf("\n");
	if (length >= 12 && length < sizeof(struct fastrpc_msg)) {
		/* the invoke response: a context and the DSP's return value */
		uint64_t ctx;
		int32_t retval;

		memcpy(&ctx, data, 8);
		memcpy(&retval, data + 8, 4);
		fastrpc_rsp_ctx = ctx;
		fastrpc_rsp_retval = retval;
		fastrpc_rsp_seen = true;
		printf("slpi: fastrpc: < response ctx %#llx retval %d\n", ctx, retval);
	} else if (length >= sizeof(struct fastrpc_msg)) {
		struct fastrpc_msg msg;

		memcpy(&msg, data, sizeof(msg));
		printf("slpi: fastrpc: client %d tid %d ctx %#llx handle %u sc %#x addr %#llx size %llu\n", msg.client_id, msg.tid,
		    msg.ctx, msg.handle, msg.sc, msg.addr, msg.size);
	}
}

struct fastrpc_remote_buf {
	uint64_t pv, len;
} __attribute__((packed));
struct fastrpc_invoke_buf {
	uint32_t num, pgidx;
} __attribute__((packed));
struct fastrpc_phy_page {
	uint64_t addr, size;
} __attribute__((packed));

#define FASTRPC_FDLIST_BYTES     (8u * 16)
#define FASTRPC_CRCLIST_BYTES    (4u * 64)

/* Lay out the metadata and the inline buffers as Linux's fastrpc_get_args does. Returns the page size to send. */
static uint32_t
fastrpc_build(uint8_t *buf, uint64_t pv_base, uint64_t physical, uint32_t count, const uint32_t *lengths,
    const void *const *sources)
{
	uint32_t meta = (16 + 8 + 16) * count + FASTRPC_FDLIST_BYTES + FASTRPC_CRCLIST_BYTES + 4;
	uint32_t at = (meta + 127) & ~127u;
	struct fastrpc_remote_buf *rpra = (struct fastrpc_remote_buf *)buf;
	struct fastrpc_invoke_buf *list = (struct fastrpc_invoke_buf *)(buf + 16 * count);
	struct fastrpc_phy_page *pages = (struct fastrpc_phy_page *)(buf + 24 * count);

	for (uint32_t i = 0; i < count; i++) {
		at = (at + 127) & ~127u;
		rpra[i].pv = lengths[i] != 0 ? pv_base + at : 0;
		rpra[i].len = lengths[i];
		list[i].num = lengths[i] != 0;
		list[i].pgidx = i;
		if (lengths[i] == 0) {
			/* as Linux leaves it: no page behind a buffer of no bytes */
			continue;
		}
		pages[i].addr = (physical + at) & ~0xfffull;
		pages[i].size = ((physical + at + lengths[i] - 1) & ~0xfffull) - pages[i].addr + 0x1000;
		if (sources[i] != NULL) {
			memcpy(buf + at, sources[i], lengths[i]);
		}
		at += lengths[i];
	}
	return (at + 0xfff) & ~0xfffu;
}

/*
 * A FastRPC argument packet in the shared memory the sensor DSP reads anyway (its GLINK queues are
 * there): 4 KB, in an item of ours in the partition for processor 0 and the DSP. The DSP has to
 * read the packet before it can answer, and this memory is proven reachable by the link itself.
 */
#define ITEM_FASTRPC_ARGS        621
#define ITEM_FASTRPC_OPEN        622

static uint8_t *
fastrpc_smem_item(uint32_t item, uint32_t *offset)
{
	uint32_t cacheline = 0, partition_size = 0, bytes = 0, at;
	uint32_t partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &partition_size);

	if (partition == 0) {
		printf("slpi: fastrpc: no partition with the sensor DSP for the argument packet\n");
		return NULL;
	}
	at = smem_item(item, &bytes);
	if (at == 0 || bytes < 4096) {
		smem_private_item_make(partition, item, 4096);
		at = smem_item(item, &bytes);
		if (at == 0 || bytes < 4096) {
			printf("slpi: fastrpc: the shared 4 KB buffer could not be made (%u bytes)\n", bytes);
			return NULL;
		}
	}
	*offset = at;
	return (uint8_t *)(uintptr_t)(smem + at);
}

static uint8_t *
fastrpc_smem_buffer(uint32_t *offset)
{
	return fastrpc_smem_item(ITEM_FASTRPC_ARGS, offset);
}

static void
fastrpc_smem_put(uint32_t offset, const uint8_t *packet, uint32_t size)
{
	memcpy((void *)(uintptr_t)(smem + offset), packet, size);
	settle();
}

static void
fastrpc_smem_get(uint32_t offset, uint8_t *to, uint32_t size)
{
	memcpy(to, (const void *)(uintptr_t)(smem + offset), size);
	settle();
}

static uint32_t fastrpc_call_tid = 1;    /* the tid of the call being sent */

static bool
fastrpc_send_async(uint32_t client_id, uint32_t handle, uint32_t sc, uint32_t context, uint64_t physical, uint32_t size)
{
	struct fastrpc_msg msg;

	if (!glink.channel[CHANNEL_FASTRPC].open) {
		return false;
	}
	bzero(&msg, sizeof(msg));
	msg.client_id = (int32_t)client_id;
	msg.tid = (int32_t)fastrpc_call_tid;
	msg.ctx = context;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = physical;
	msg.size = (size + 0xfff) & ~0xfffu;
	return glink_send(&glink.channel[CHANNEL_FASTRPC], &msg, (uint32_t)sizeof(msg)) == 0;
}

static bool
fastrpc_call(uint32_t client_id, uint32_t handle, uint32_t sc, uint32_t context, uint64_t physical, uint32_t size,
    uint32_t wait_ms, const char *what)
{
	struct fastrpc_msg msg;

	if (!glink.channel[CHANNEL_FASTRPC].open) {
		printf("slpi: fastrpc: the channel is not open\n");
		return false;
	}
	fastrpc_rsp_seen = false;
	fastrpc_rsp_ctx = 0;
	fastrpc_rsp_retval = 0;
	bzero(&msg, sizeof(msg));
	msg.client_id = (int32_t)client_id;
	msg.tid = (int32_t)fastrpc_call_tid;
	msg.ctx = context;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = physical;
	msg.size = (size + 0xfff) & ~0xfffu;
	if (glink_send(&glink.channel[CHANNEL_FASTRPC], &msg, (uint32_t)sizeof(msg)) != 0) {
		printf("slpi: fastrpc: %s: the channel did not take the call\n", what);
		return false;
	}
	printf("slpi: fastrpc: > %s client %u handle %u sc %#x ctx %#x addr %#llx size %u\n", what, client_id, handle, sc, context,
	    physical, size);
	for (unsigned ms = 0; ms < wait_ms && !(fastrpc_rsp_seen && fastrpc_rsp_ctx == context); ms++) {
		sleep_ms(1);
	}
	if (!(fastrpc_rsp_seen && fastrpc_rsp_ctx == context)) {
		printf("slpi: fastrpc: %s: no response in %u ms%s\n", what, wait_ms,
		    fastrpc_rsp_seen ? " (a response for another call came instead)" : "");
		return false;
	}
	printf("slpi: fastrpc: %s: ctx %#llx retval %d\n", what, fastrpc_rsp_ctx, fastrpc_rsp_retval);
	return true;
}

/*
 * One look at the reverse tunnel: adsp_listener_next2 (handle 3, method 4) hands us the next call
 * the DSP wants the AP to serve. Its inputs are the previous call's context and result and the
 * previous call's encoded output buffers; its outputs are the next call's context, handle, scalars
 * and input buffers. Nothing is served yet: this stage only shows what the sensor process asks for.
 */
static uint32_t fastrpc_pd = 2;          /* the PD the listener's calls are addressed to (boot-arg fastrpc_pd; the sensors PD) */
static uint32_t fastrpc_service = 3;     /* the listener's handle: the fixed one, or the open's (boot-arg fastrpc_lhandle) */
static uint32_t fastrpc_lhandle;         /* 0: the fixed 3; 1: the handle remotectl_open returned; else that value */
static uint32_t fastrpc_listener1;       /* boot-arg fastrpc_listener1: the modern adsp_listener1 interface (methods 5 and 6) */
static uint32_t fastrpc_init2_sc = 0x03000000;
static uint32_t fastrpc_next2_sc = 0x040202;
static uint32_t fastrpc_attach_late;     /* boot-arg fastrpc_attach_late: register the listener before the sensors PD starts */
static uint32_t fastrpc_restart;         /* boot-arg fastrpc_restart: release the sensors PD and attach it again */
static uint32_t fastrpc_delay;           /* boot-arg fastrpc_delay: seconds between the first attach and the listener */
static uint32_t fastrpc_bare;            /* boot-arg fastrpc_bare: the static listener first, the sensors PD after it */
static uint32_t fastrpc_attach_mid;      /* boot-arg fastrpc_attach_mid: register, attach the sensors PD, then the listener thread */
static uint32_t fastrpc_static_open;     /* boot-arg fastrpc_static_open: open the adsp_listener module for init2/next2 */
static uint32_t fastrpc_open_wait = 20000; /* boot-arg fastrpc_open_wait: ms to wait for that module load */
static uint32_t fastrpc_open_async;      /* boot-arg fastrpc_open_async: send the open, let the listener serve it */
static uint32_t fastrpc_open_after;      /* boot-arg fastrpc_open_after: send it after the listener is waiting */
static uint32_t fastrpc_open_pd = 0xffffffff; /* boot-arg fastrpc_open_pd: the PD of that open (unset: fastrpc_pd) */
static uint32_t fastrpc_attach0;         /* boot-arg fastrpc_attach0: the guest-OS attach (PD 0) before the listener */
static uint32_t fastrpc_attach_client;   /* boot-arg fastrpc_attach_client: the client session of the sensors attach */
static uint32_t fastrpc_attach_tid;      /* boot-arg fastrpc_attach_tid: the DSP thread the sensors attach runs on */

static void
fastrpc_listener_poll(uint64_t physical, uint32_t shared_offset, bool shared, uint8_t *packet, uint8_t *reply)
{
	uint8_t nested[2048];
	uint32_t sizes[4] = { 0, 0, 0, sizeof(nested) }, numbers[4] = { 0, 0, 0, 0 }, lengths[4], size;
	const void *sources[4];

	lengths[0] = sizeof(sizes);
	sources[0] = sizes;
	lengths[1] = 0;
	sources[1] = NULL;
	lengths[2] = sizeof(numbers);
	sources[2] = numbers;
	lengths[3] = sizeof(nested);
	sources[3] = nested;
	memset(packet, 0, 4096);
	size = fastrpc_build(packet, physical, physical, 4, lengths, sources);
	if (shared) {
		fastrpc_smem_put(shared_offset, packet, size);
	}
	if (!fastrpc_call(1, fastrpc_service, fastrpc_next2_sc, 0x500 | fastrpc_pd, physical, size, 10000,
	    "adsp_listener_next2")) {
		return;
	}
	if (shared) {
		struct fastrpc_remote_buf *rpra = (struct fastrpc_remote_buf *)reply;

		fastrpc_smem_get(shared_offset, reply, 4096);
		fastrpc_smem_get(shared_offset + (uint32_t)(rpra[2].pv - physical), (uint8_t *)numbers, sizeof(numbers));
		fastrpc_smem_get(shared_offset + (uint32_t)(rpra[3].pv - physical), nested, sizeof(nested));
	} else {
		struct fastrpc_remote_buf *rpra = (struct fastrpc_remote_buf *)packet;
		uint32_t *out = (uint32_t *)(uintptr_t)rpra[2].pv;
		uint8_t *in = (uint8_t *)(uintptr_t)rpra[3].pv;

		memcpy(numbers, out, sizeof(numbers));
		memcpy(nested, in, sizeof(nested));
	}
	printf("slpi: fastrpc: the DSP asks for handle %u sc %#x with %u bytes of inputs (ctx %#x)\n", numbers[1], numbers[2],
	    numbers[3], numbers[0]);
	for (uint32_t i = 0; i < numbers[3] && i < sizeof(nested); i++) {
		printf("%s%02x", i % 16 == 0 ? "\nslpi: fastrpc:   " : " ", nested[i]);
	}
	if (numbers[3] != 0) {
		printf("\n");
	}
}

/* ---- the reverse tunnel's apps_std: the sensor registry and configuration, served from fw/sensors ---- */

#define FASTRPC_FILE_SLOTS      32
#define FASTRPC_DIR_SLOTS       4
#define FASTRPC_DIR_NAMES       96
#define FASTRPC_DIR_NAME_LEN    96
#define FASTRPC_FILE_LIMIT      (512 * 1024)
#define FASTRPC_OUT_LIMIT       1024

#define AEE_EBADPARM            0x80000434u
#define AEE_EFAILED             0x80004005u
#define AEE_ENOMEMORY           0x80000002u

struct fastrpc_io {
	uint32_t size;
	uint8_t *data;
};

struct fastrpc_file_slot {
	bool used;
	uint32_t fd;
	uint8_t *data;
	uint32_t size, offset;
};

struct fastrpc_dir_slot {
	bool used;
	uint32_t fd;
	uint32_t count, at;
	char names[FASTRPC_DIR_NAMES][FASTRPC_DIR_NAME_LEN];
};

static struct fastrpc_file_slot fastrpc_files[FASTRPC_FILE_SLOTS];
static struct fastrpc_dir_slot fastrpc_dirs[FASTRPC_DIR_SLOTS];
static uint32_t fastrpc_next_fd = 1;

/* The DSP's paths, to the copies in the root image that userland/build.sh puts there from fw-sensors/. */
static const char *
fastrpc_path(const char *name, char *to, size_t length)
{
	static const struct {
		const char *from, *to;
	} map[] = {
		{ "/vendor/etc/sensors/sns_reg_config", "/fw/sensors/sns_reg_config" },
		{ "/vendor/etc/sensors/config/", "/fw/sensors/config/" },
		{ "/vendor/etc/sensors/config", "/fw/sensors/config" },
		{ "/etc/qcom/sns_reg.conf", "/fw/sensors/sns_reg_config" },
		{ "/etc/qcom/sensors.d/", "/fw/sensors/config/" },
		{ "/etc/qcom/sensors.d", "/fw/sensors/config" },
		{ "/var/lib/qcom/sensors/", "/fw/sensors/persist/" },
		{ "/var/lib/qcom/sensors", "/fw/sensors/persist" },
		{ "/mnt/vendor/persist/sensors/", "/fw/sensors/persist/" },
		{ "/mnt/vendor/persist/sensors", "/fw/sensors/persist" },
		{ "/usr/lib/qcom/adsp/avs/", "/fw/acdbdata/" },
		{ "/usr/lib/qcom/adsp/avs", "/fw/acdbdata" },
		{ "/usr/lib/qcom/adsp/", "/fw/sensors/adsp/" },
		{ "/usr/lib/qcom/adsp", "/fw/sensors/adsp" },
		{ "/vendor/dsp/adsp/", "/fw/sensors/adsp/" },
		{ "/sys/devices/soc0/", "/fw/sensors/soc0/" },
	};

	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		size_t n = strlen(map[i].from);

		if (strncmp(name, map[i].from, n) == 0) {
			snprintf(to, length, "%s%s", map[i].to, name + n);
			return to;
		}
	}
	return NULL;
}

/* A file of the root image, whole, up to a limit. */
static uint8_t *
fastrpc_root_read(const char *path, uint64_t limit, uint32_t *size)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP || vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size > limit ||
	    attributes.va_data_size > 0xffffffffull) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data((size_t)attributes.va_data_size + 1, Z_WAITOK | Z_ZERO);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	if (attributes.va_data_size != 0) {
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
		    vfs_context_ucred(context), &left, vfs_context_proc(context));
		if (error != 0 || left != 0) {
			kfree_data(data, (size_t)attributes.va_data_size + 1);
			vnode_put(vnode);
			return NULL;
		}
	}
	vnode_put(vnode);
	*size = (uint32_t)attributes.va_data_size;
	return data;
}

/* The entries of a directory of the root image, by name. */
static uint32_t
fastrpc_root_dir(const char *path, char names[][FASTRPC_DIR_NAME_LEN], uint32_t max)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint32_t count = 0;
	off_t offset = 0;
	bool eof = false;

	if (rootvnode == NULLVP || vnode_lookup(path, 0, &vnode, context) != 0) {
		return 0;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_mode);
	if (vnode_getattr(vnode, &attributes, context) != 0 || (attributes.va_mode & S_IFMT) != S_IFDIR) {
		vnode_put(vnode);
		return 0;
	}
	while (!eof && count < max) {
		char *buffer = kalloc_data(4096, Z_WAITOK | Z_ZERO);
		struct uio auio;
		int eofflag = 0, numdirent = 0;
		uint32_t used;
		char *at;

		if (buffer == NULL) {
			break;
		}
		uio_reset(&auio, offset, UIO_READ, UIO_SYSSPACE);
		uio_addiov(&auio, (uintptr_t)buffer, 4096);
		if (VNOP_READDIR(vnode, &auio, 0, &eofflag, &numdirent, context) != 0) {
			kfree_data(buffer, 4096);
			break;
		}
		used = 4096 - (uint32_t)auio.uio_resid_64;
		offset = auio.uio_offset;
		for (at = buffer; at + offsetof(struct dirent, d_name) + 1 <= buffer + used && count < max;) {
			struct dirent *entry = (struct dirent *)at;

			if (entry->d_reclen < offsetof(struct dirent, d_name) + 1 || at + entry->d_reclen > buffer + used) {
				break;
			}
			if (entry->d_namlen != 0 && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
				strlcpy(names[count++], entry->d_name, FASTRPC_DIR_NAME_LEN);
			}
			at += entry->d_reclen;
		}
		eof = eofflag != 0 || used == 0;
		kfree_data(buffer, 4096);
	}
	vnode_put(vnode);
	return count;
}

static struct fastrpc_file_slot *
fastrpc_file_slot(uint32_t fd)
{
	for (unsigned i = 0; i < FASTRPC_FILE_SLOTS; i++) {
		if (fastrpc_files[i].used && fastrpc_files[i].fd == fd) {
			return &fastrpc_files[i];
		}
	}
	return NULL;
}

static struct fastrpc_dir_slot *
fastrpc_dir_slot(uint32_t fd)
{
	for (unsigned i = 0; i < FASTRPC_DIR_SLOTS; i++) {
		if (fastrpc_dirs[i].used && fastrpc_dirs[i].fd == fd) {
			return &fastrpc_dirs[i];
		}
	}
	return NULL;
}

static const char *
fastrpc_string(const struct fastrpc_io *io)
{
	if (io->data == NULL || io->size == 0 || io->data[io->size - 1] != 0) {
		return NULL;
	}
	return (const char *)io->data;
}

/* The input buffers of a nested call, decoded: [size][padding][data], as the DSP encodes them. */
static int
fastrpc_decode(uint32_t sc, const uint8_t *blob, uint32_t length, struct fastrpc_io *io, uint32_t max)
{
	uint32_t count = (sc >> 16) & 0xff, at = 0, align = 0;

	if (count > max) {
		return -1;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t size, skip;

		if (at + 4 > length) {
			return -1;
		}
		memcpy(&size, blob + at, 4);
		at += 4;
		align = (align + 4) & 7;
		io[i].size = 0;
		io[i].data = NULL;
		if (size == 0) {
			continue;
		}
		skip = align ? 8 - align : 0;
		at += skip;
		align = (align + skip) & 7;
		if (at + size > length) {
			return -1;
		}
		io[i].size = size;
		io[i].data = __DECONST(uint8_t *, blob + at);
		at += size;
		align = (align + size) & 7;
	}
	return (int)count;
}

/* The output buffers of a nested call, encoded the same way, into the blob the next call returns. */
static uint32_t
fastrpc_encode(const struct fastrpc_io *io, uint32_t count, uint8_t *blob, uint32_t capacity)
{
	uint32_t at = 0, align = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t skip;

		if (at + 4 > capacity) {
			return 0;
		}
		memcpy(blob + at, &io[i].size, 4);
		at += 4;
		align = (align + 4) & 7;
		if (io[i].size == 0) {
			continue;
		}
		skip = align ? 8 - align : 0;
		if (at + skip + io[i].size > capacity) {
			return 0;
		}
		memset(blob + at, 0, skip);
		at += skip;
		align = (align + skip) & 7;
		memcpy(blob + at, io[i].data, io[i].size);
		at += io[i].size;
		align = (align + io[i].size) & 7;
	}
	return at;
}

/* apps_std's methods, as far as the sensor registry needs them: method, in numbers, in buffers, out numbers, out buffers. */
static const struct {
	uint8_t method, in_numbers, in_buffers, out_numbers, out_buffers;
} fastrpc_apps_std[] = {
	{ 1, 1, 0, 1, 1 },      /* freopen */
	{ 2, 8, 0, 0, 0 },      /* fflush */
	{ 3, 1, 0, 0, 0 },      /* fclose */
	{ 4, 1, 0, 2, 1 },      /* fread */
	{ 9, 3, 0, 0, 0 },      /* fseek */
	{ 19, 0, 4, 1, 0 },     /* fopen_with_env */
	{ 26, 0, 1, 2, 0 },     /* opendir */
	{ 27, 2, 0, 0, 0 },     /* closedir */
	{ 28, 2, 0, 66, 0 },    /* readdir */
	{ 31, 1, 1, 24, 0 },    /* stat */
};

static int32_t
fastrpc_serve(uint32_t handle, uint32_t sc, const uint8_t *blob, uint32_t length, struct fastrpc_io *out,
    uint32_t max_out, uint32_t *out_count)
{
	struct fastrpc_io in[8];
	uint32_t method = (sc >> 24) & 0x1f, def = 0, want, sizes[16];
	int32_t result = (int32_t)AEE_EBADPARM;
	uint32_t *numbers = NULL;

	*out_count = 0;
	if (handle != 1) {
		printf("slpi: fastrpc: a call for handle %u is not served\n", handle);
		return result;
	}
	while (def < sizeof(fastrpc_apps_std) / sizeof(fastrpc_apps_std[0]) && fastrpc_apps_std[def].method != method) {
		def++;
	}
	if (def == sizeof(fastrpc_apps_std) / sizeof(fastrpc_apps_std[0])) {
		printf("slpi: fastrpc: apps_std method %u is not served\n", method);
		return result;
	}
	if (fastrpc_decode(sc, blob, length, in, 8) < 0 || in[0].size < 4) {
		printf("slpi: fastrpc: apps_std %u: the arguments do not decode\n", method);
		return result;
	}
	memset(sizes, 0, sizeof(sizes));
	for (uint32_t i = 0; i < fastrpc_apps_std[def].in_numbers + fastrpc_apps_std[def].in_buffers +
	    fastrpc_apps_std[def].out_buffers && i < 16; i++) {
		if (4 * i + 4 <= in[0].size) {
			memcpy(&sizes[i], in[0].data + 4 * i, 4);
		}
	}
	want = (fastrpc_apps_std[def].out_numbers ? 1 : 0) + fastrpc_apps_std[def].out_buffers;
	if (want > max_out) {
		return result;
	}
	for (uint32_t i = 0; i < want; i++) {
		uint32_t size;

		if (i == 0 && fastrpc_apps_std[def].out_numbers) {
			size = 4 * fastrpc_apps_std[def].out_numbers;
		} else {
			size = sizes[fastrpc_apps_std[def].in_numbers + fastrpc_apps_std[def].in_buffers +
			    (fastrpc_apps_std[def].out_numbers ? i - 1 : i)];
		}
		if (size > FASTRPC_OUT_LIMIT) {
			size = FASTRPC_OUT_LIMIT;
		}
		out[i].size = size;
		out[i].data = kalloc_data(size ? size : 1, Z_WAITOK | Z_ZERO);
		if (out[i].data == NULL) {
			for (uint32_t n = 0; n < i; n++) {
				kfree_data(out[n].data, out[n].size ? out[n].size : 1);
				out[n].data = NULL;
				out[n].size = 0;
			}
			*out_count = 0;
			return (int32_t)AEE_ENOMEMORY;
		}
		*out_count = i + 1;
	}
	if (fastrpc_apps_std[def].out_numbers && want > 0) {
		numbers = (uint32_t *)out[0].data;
	}

	switch (method) {
	case 19: {                         /* fopen_with_env */
		const char *name = NULL, *mode;
		char path[160];
		uint32_t file_size = 0;
		uint8_t *data;
		int slot;

		if (in[3].size != 0) {
			name = fastrpc_string(&in[3]);
		}
		mode = in[4].size != 0 ? fastrpc_string(&in[4]) : NULL;
		if (name == NULL || mode == NULL || (mode[0] != 'r' && mode[0] != 'R')) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		if (fastrpc_path(name, path, sizeof(path)) == NULL) {
			const char *env = in[1].size != 0 ? fastrpc_string(&in[1]) : NULL;

			if (name[0] == '/') {
				snprintf(path, sizeof(path), "%s", name);
			} else if (env != NULL && strcmp(env, "ADSP_LIBRARY_PATH") == 0) {
				snprintf(path, sizeof(path), "/fw/sensors/adsp/%s", name);
			} else if (env != NULL && strcmp(env, "ADSP_AVS_CFG_PATH") == 0) {
				snprintf(path, sizeof(path), "/fw/acdbdata/%s", name);
			} else {
				snprintf(path, sizeof(path), "/fw/sensors/%s", name);
			}
		}
		data = fastrpc_root_read(path, FASTRPC_FILE_LIMIT, &file_size);
		if (data == NULL) {
			printf("slpi: fastrpc: apps_std: cannot read \"%s\"\n", path);
			result = (int32_t)AEE_EFAILED;
			break;
		}
		for (slot = 0; slot < FASTRPC_FILE_SLOTS && fastrpc_files[slot].used; slot++) {
			;
		}
		if (slot == FASTRPC_FILE_SLOTS) {
			kfree_data(data, file_size + 1);
			result = (int32_t)AEE_ENOMEMORY;
			break;
		}
		fastrpc_files[slot].used = true;
		fastrpc_files[slot].fd = fastrpc_next_fd++;
		fastrpc_files[slot].data = data;
		fastrpc_files[slot].size = file_size;
		fastrpc_files[slot].offset = 0;
		if (numbers != NULL) {
			numbers[0] = fastrpc_files[slot].fd;
		}
		printf("slpi: fastrpc: apps_std: opened %s (%u bytes) as %u\n", path, file_size, fastrpc_files[slot].fd);
		result = 0;
		break;
	}
	case 4: {                          /* fread */
		struct fastrpc_file_slot *file = fastrpc_file_slot(sizes[0]);
		uint32_t got;

		if (file == NULL || numbers == NULL || out[1].data == NULL) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		got = file->size - file->offset;
		if (got > sizes[1]) {
			got = sizes[1];
		}
		if (got > out[1].size) {
			got = out[1].size;
		}
		memcpy(out[1].data, file->data + file->offset, got);
		file->offset += got;
		numbers[0] = got;
		numbers[1] = file->offset >= file->size;
		result = 0;
		break;
	}
	case 9: {                          /* fseek */
		struct fastrpc_file_slot *file = fastrpc_file_slot(sizes[0]);

		if (file == NULL) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		if (sizes[2] == 0) {
			file->offset = sizes[1];
		} else if (sizes[2] == 1) {
			file->offset += sizes[1];
		} else {
			file->offset = (uint32_t)((int32_t)file->size + (int32_t)sizes[1]);
		}
		if (file->offset > file->size) {
			file->offset = file->size;
		}
		result = 0;
		break;
	}
	case 3: {                          /* fclose */
		struct fastrpc_file_slot *file = fastrpc_file_slot(sizes[0]);

		if (file == NULL) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		kfree_data(file->data, file->size + 1);
		bzero(file, sizeof(*file));
		result = 0;
		break;
	}
	case 26: {                         /* opendir */
		const char *name = in[1].size != 0 ? fastrpc_string(&in[1]) : NULL;
		char path[160];
		int slot;

		if (name == NULL || numbers == NULL) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		if (fastrpc_path(name, path, sizeof(path)) == NULL) {
			snprintf(path, sizeof(path), "%s", name);
		}
		for (slot = 0; slot < FASTRPC_DIR_SLOTS && fastrpc_dirs[slot].used; slot++) {
			;
		}
		if (slot == FASTRPC_DIR_SLOTS) {
			result = (int32_t)AEE_ENOMEMORY;
			break;
		}
		fastrpc_dirs[slot].used = true;
		fastrpc_dirs[slot].fd = fastrpc_next_fd++;
		fastrpc_dirs[slot].count = fastrpc_root_dir(path, fastrpc_dirs[slot].names, FASTRPC_DIR_NAMES);
		fastrpc_dirs[slot].at = 0;
		{
			uint64_t fd = fastrpc_dirs[slot].fd;

			memcpy(numbers, &fd, sizeof(fd));
		}
		printf("slpi: fastrpc: apps_std: opened %s, %u entries\n", path, fastrpc_dirs[slot].count);
		result = 0;
		break;
	}
	case 28: {                         /* readdir */
		uint64_t fd = 0;
		struct fastrpc_dir_slot *dir;
		struct {
			uint32_t inode;
			char name[255];
			uint32_t is_eof;
		} __attribute__((packed)) *entry = (void *)out[0].data;

		if (in[0].size >= 8) {
			memcpy(&fd, in[0].data, 8);
		}
		dir = fastrpc_dir_slot((uint32_t)fd);
		if (dir == NULL || entry == NULL || out[0].size < sizeof(*entry)) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		if (dir->at < dir->count) {
			strlcpy(entry->name, dir->names[dir->at++], sizeof(entry->name));
			entry->is_eof = 0;
		} else {
			entry->name[0] = 0;
			entry->is_eof = 1;
		}
		entry->inode = 0;
		result = 0;
		break;
	}
	case 27: {                         /* closedir */
		uint64_t fd = 0;
		struct fastrpc_dir_slot *dir;

		if (in[0].size >= 8) {
			memcpy(&fd, in[0].data, 8);
		}
		dir = fastrpc_dir_slot((uint32_t)fd);
		if (dir == NULL) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		bzero(dir, sizeof(*dir));
		result = 0;
		break;
	}
	case 31: {                         /* stat */
		char name[160], path[160];
		vfs_context_t context = vfs_context_kernel();
		vnode_t vnode = NULLVP;
		struct vnode_attr attributes;
		struct {
			uint64_t tsz, dev, ino;
			uint32_t mode, nlink;
			uint64_t rdev, size;
			int64_t atime, atimensec, mtime, mtimensec, ctime, ctimensec;
		} *stats = (void *)out[0].data;

		if (in[1].size == 0 || in[1].size >= sizeof(name) || stats == NULL || out[0].size < sizeof(*stats)) {
			result = (int32_t)AEE_EBADPARM;
			break;
		}
		memcpy(name, in[1].data, in[1].size);
		name[in[1].size] = 0;
		if (fastrpc_path(name, path, sizeof(path)) == NULL) {
			snprintf(path, sizeof(path), "%s", name);
		}
		if (vnode_lookup(path, 0, &vnode, context) != 0) {
			result = (int32_t)AEE_EFAILED;
			break;
		}
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_mode);
		VATTR_WANTED(&attributes, va_data_size);
		VATTR_WANTED(&attributes, va_fsid);
		VATTR_WANTED(&attributes, va_fileid);
		if (vnode_getattr(vnode, &attributes, context) == 0) {
			stats->dev = attributes.va_fsid;
			stats->ino = attributes.va_fileid;
			stats->mode = (uint32_t)attributes.va_mode;
			stats->nlink = 1;
			stats->size = attributes.va_data_size;
			result = 0;
		}
		vnode_put(vnode);
		break;
	}
	case 2:                            /* fflush */
		result = 0;
		break;
	default:
		printf("slpi: fastrpc: apps_std method %u: not served yet\n", method);
		result = (int32_t)AEE_ENOMEMORY;
		break;
	}
	return result;
}

/* The reverse tunnel kept open: each nested call the DSP makes is served from fw/sensors. */
static volatile bool fastrpc_listener_wanted;

static void
fastrpc_listener_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t *packet = kalloc_data(4096, Z_WAITOK | Z_ZERO), *reply = kalloc_data(4096, Z_WAITOK | Z_ZERO);
	uint8_t *returned = kalloc_data(4096, Z_WAITOK | Z_ZERO);
	uint32_t offset = 0, physical, rctx = 0, result_code = 0xffffffff, returned_length = 0, calls = 0;
	uint32_t sizes[4], numbers[4], lengths[4], size;
	uint8_t nested[2048];
	const void *sources[4];

	if (packet == NULL || reply == NULL || returned == NULL) {
		printf("slpi: fastrpc: the listener has no buffers\n");
		goto out;
	}
	if (fastrpc_smem_buffer(&offset) == NULL) {
		goto out;
	}
	physical = SMEM_PHYS + offset;
	while (fastrpc_listener_wanted) {
		sizes[0] = rctx;
		sizes[1] = result_code;
		sizes[2] = returned_length;
		sizes[3] = (uint32_t)sizeof(nested);
		memset(numbers, 0, sizeof(numbers));
		memset(nested, 0, sizeof(nested));
		lengths[0] = sizeof(sizes);
		sources[0] = sizes;
		lengths[1] = returned_length;
		sources[1] = returned_length != 0 ? returned : NULL;
		lengths[2] = sizeof(numbers);
		sources[2] = numbers;
		lengths[3] = (uint32_t)sizeof(nested);
		sources[3] = nested;
		memset(packet, 0, 4096);
		size = fastrpc_build(packet, physical, physical, 4, lengths, sources);
		if (size > 4096) {
			printf("slpi: fastrpc: the listener's packet would be %u bytes\n", size);
			break;
		}
		fastrpc_smem_put(offset, packet, size);
		if (!fastrpc_call(1, fastrpc_service, fastrpc_next2_sc, 0x600 | ((calls++ & 7) << 8) | fastrpc_pd, physical,
		    size, 120000, "adsp_listener_next2")) {
			if (!fastrpc_listener_wanted) {
				break;
			}
			sleep_ms(5000);
			continue;
		}
		if (fastrpc_rsp_retval != 0) {
			/* nothing is waiting in the DSP's listener yet */
			printf("slpi: fastrpc: the listener has nothing waiting (%d)\n", fastrpc_rsp_retval);
			sleep_ms(100);
			continue;
		}
		{
			struct fastrpc_remote_buf *rpra;

			fastrpc_smem_get(offset, reply, 4096);
			rpra = (struct fastrpc_remote_buf *)reply;
			fastrpc_smem_get(offset + (uint32_t)(rpra[2].pv - physical), (uint8_t *)numbers, sizeof(numbers));
			fastrpc_smem_get(offset + (uint32_t)(rpra[3].pv - physical), nested, sizeof(nested));
		}
		printf("slpi: fastrpc: the DSP asks for handle %u sc %#x with %u bytes of inputs (ctx %#x)\n", numbers[1], numbers[2],
		    numbers[3], numbers[0]);
		for (uint32_t i = 0; i < numbers[3] && i < sizeof(nested); i++) {
			printf("%s%02x", i % 16 == 0 ? "\nslpi: fastrpc:   " : " ", nested[i]);
		}
		if (numbers[3] != 0) {
			printf("\n");
		}
		{
			struct fastrpc_io out[4];
			uint32_t out_count = 0;
			int32_t served;

			/* the registry files are in the root image, which may be mounted a moment after this step */
			for (unsigned ms = 0; ms < 30000 && rootvnode == NULLVP; ms++) {
				sleep_ms(1);
			}
			memset(out, 0, sizeof(out));
			served = fastrpc_serve(numbers[1], numbers[2], nested,
			    numbers[3] <= sizeof(nested) ? numbers[3] : (uint32_t)sizeof(nested), out, 4, &out_count);
			returned_length = out_count != 0 ? fastrpc_encode(out, out_count, returned, 1024) : 0;
			if (served != 0) {
				printf("slpi: fastrpc: the call is answered with %#x\n", (uint32_t)served);
			}
			for (uint32_t i = 0; i < out_count; i++) {
				if (out[i].data != NULL) {
					kfree_data(out[i].data, out[i].size ? out[i].size : 1);
				}
			}
			rctx = numbers[0];
			result_code = (uint32_t)served;
		}
	}
out:
	if (packet != NULL) {
		kfree_data(packet, 4096);
	}
	if (reply != NULL) {
		kfree_data(reply, 4096);
	}
	if (returned != NULL) {
		kfree_data(returned, 4096);
	}
	thread_terminate(current_thread());
}

static void
fastrpc_listener_start(void)
{
	thread_t thread;

	if (fastrpc_listener_wanted) {
		return;
	}
	fastrpc_listener_wanted = true;
	if (kernel_thread_start(fastrpc_listener_thread, NULL, &thread) != KERN_SUCCESS) {
		fastrpc_listener_wanted = false;
		printf("slpi: fastrpc: the listener's thread did not start\n");
		return;
	}
	thread_deallocate(thread);
	printf("slpi: fastrpc: the listener's thread runs\n");
}

/* Which names the DSP's remotectl will open on this firmware, and with which handle and error. */
static uint32_t
fastrpc_open_name(const char *name, uint64_t physical, uint32_t shared_offset, uint8_t *packet, uint8_t *reply,
    uint32_t wait_ms)
{
	uint32_t lengths[4], size, handle = 0, handles[2] = { 0, 0 }, scalar[2];
	uint8_t error[256];
	const void *sources[4];

	memset(packet, 0, 4096);
	memset(error, 0, sizeof(error));
	scalar[0] = (uint32_t)strlen(name) + 1;
	scalar[1] = (uint32_t)sizeof(error);
	lengths[0] = 8;
	sources[0] = scalar;
	lengths[1] = scalar[0];
	sources[1] = name;
	lengths[2] = 8;
	sources[2] = handles;
	lengths[3] = (uint32_t)sizeof(error);
	sources[3] = error;
	size = fastrpc_build(packet, physical, physical, 4, lengths, sources);
	fastrpc_smem_put(shared_offset, packet, size);
	if (fastrpc_call(1, 0, 0x20200, 0x200 | fastrpc_pd, physical, size, wait_ms, name)) {
		struct fastrpc_remote_buf *rpra;

		fastrpc_smem_get(shared_offset, reply, 4096);
		rpra = (struct fastrpc_remote_buf *)reply;
		fastrpc_smem_get(shared_offset + (uint32_t)(rpra[2].pv - physical), (uint8_t *)handles, sizeof(handles));
		fastrpc_smem_get(shared_offset + (uint32_t)(rpra[3].pv - physical), error, sizeof(error) - 1);
		handle = handles[0];
		printf("slpi: fastrpc: opened \"%s\": handle %u dlret %d error \"%s\"\n", name, handles[0], (int32_t)handles[1], error);
	}
	return handle;
}

/* INIT_ATTACH, INIT_RELEASE: one buffer with the client id, on the fixed handle 1. */
static bool
fastrpc_pd_call(uint32_t msg_client, uint32_t sc, uint32_t context, uint64_t physical, uint32_t shared_offset,
    bool shared, uint8_t *packet, uint32_t client_id, const char *what)
{
	const void *sources[1];
	uint32_t lengths[1], size;

	uint32_t saved = fastrpc_call_tid;

	memset(packet, 0, 4096);
	lengths[0] = 4;
	sources[0] = &client_id;
	size = fastrpc_build(packet, physical, physical, 1, lengths, sources);
	if (shared) {
		fastrpc_smem_put(shared_offset, packet, size);
	}
	if (fastrpc_attach_tid != 0 && sc == 0x10000) {
		/* the sensors attach must not wait behind the listener's next2 on another thread */
		fastrpc_call_tid = fastrpc_attach_tid;
	}
	{
		bool sent = fastrpc_call(msg_client, 1, sc, context, physical, size, 2000, what);

		fastrpc_call_tid = saved;
		return sent;
	}
}

/* Send the module open without waiting: its file requests are for the listener to serve meanwhile. */
static bool
fastrpc_module_open_send(uint32_t pd)
{
	uint32_t offset = 0, lengths[4], size, scalar[2], handles[2] = { 0, 0 };
	uint8_t error[256] = { 0 }, *staging = kalloc_data(4096, Z_WAITOK | Z_ZERO);
	uint8_t *slot = fastrpc_smem_item(ITEM_FASTRPC_OPEN, &offset);
	const void *sources[4];
	uint64_t physical = SMEM_PHYS + offset;
	bool sent = false;

	if (staging != NULL && slot != NULL) {
		scalar[0] = (uint32_t)strlen("adsp_listener") + 1;
		scalar[1] = (uint32_t)sizeof(error);
		lengths[0] = 8;
		sources[0] = scalar;
		lengths[1] = scalar[0];
		sources[1] = "adsp_listener";
		lengths[2] = 8;
		sources[2] = handles;
		lengths[3] = (uint32_t)sizeof(error);
		sources[3] = error;
		size = fastrpc_build(staging, physical, physical, 4, lengths, sources);
		fastrpc_smem_put(offset, staging, size);
		sent = fastrpc_send_async(1, 0, 0x20200, 0x250 | pd, physical, size);
		printf("slpi: fastrpc: the adsp_listener open is sent to PD %u; the listener can serve it while it waits\n", pd);
	}
	if (staging != NULL) {
		kfree_data(staging, 4096);
	}
	return sent;
}

static void
fastrpc_first_calls(uint32_t stage)
{
	static const char listener[] = "adsp_default_listener";
	static const char listener_modern[] = "adsp_listener1";
	const char *open_name = fastrpc_listener1 ? listener_modern : listener;
	uint8_t *buf = NULL, *packet = NULL, *parsed, reply[4096];
	uint64_t physical = 0;
	uint32_t client = 1, lengths[4], name_length = (uint32_t)strlen(open_name) + 1;
	uint32_t shared_offset = 0, size, fastrpc_listener = 0;
	bool shared = stage >= 4;
	const void *sources[4];
	uint32_t scalar[2] = { 0, 256 };
	uint32_t handles[2] = { 0, 0 };
	uint8_t error[256];

	if (shared) {
		printf("slpi: fastrpc: stage %u: asking for a 4 KB packet in the shared memory\n", stage);
		buf = fastrpc_smem_buffer(&shared_offset);
		if (buf == NULL) {
			return;
		}
		physical = SMEM_PHYS + shared_offset;
		packet = kalloc_data(4096, Z_WAITOK | Z_ZERO);
		if (packet == NULL) {
			printf("slpi: fastrpc: no staging buffer\n");
			return;
		}
		printf("slpi: fastrpc: stage %u: the packet is at %#llx (%#x in the shared memory)\n", stage, physical, shared_offset);
	} else {
		printf("slpi: fastrpc: stage %u: asking for a physically contiguous 4 KB buffer\n", stage);
		buf = hakios_slpi_dma_alloc(4096, &physical);
		if (buf == NULL) {
			printf("slpi: fastrpc: no physically contiguous buffer\n");
			return;
		}
		packet = buf;
		printf("slpi: fastrpc: stage %u: the buffer is %p, physical %#llx\n", stage, buf, physical);
		if (stage == 1) {
			hakios_slpi_dma_free(buf, 4096);
			printf("slpi: fastrpc: stage 1: the buffer is given back\n");
			return;
		}
	}
	memset(packet, 0, 4096);
	lengths[0] = 4;
	sources[0] = &client;
	size = fastrpc_build(packet, physical, physical, 1, lengths, sources);
	if (shared) {
		fastrpc_smem_put(shared_offset, packet, size);
	}
	bool attached = fastrpc_attach_late || fastrpc_bare ||
	    fastrpc_call(0, 1, 0x10000, 0x102, physical, size, 2000, "INIT_ATTACH_SNS");
	if (attached && fastrpc_delay != 0) {
		/* the first registry read has no listener and fails: wait for the process to be restarted */
		printf("slpi: fastrpc: waiting %u s before the listener exists\n", fastrpc_delay);
		sleep_ms(fastrpc_delay * 1000);
	}
	if (fastrpc_restart == 2 && attached) {
		/* stop the sensor process the first attach started, so a fresh one can start later */
		fastrpc_pd_call(0, 0x01010000, 0x500 | fastrpc_pd, physical, shared_offset, shared, packet, client, "INIT_RELEASE");
		sleep_ms(500);
	}
	if (!fastrpc_bare && attached && (shared ? stage >= 5 : stage >= 3)) {
		memset(packet, 0, 4096);
		memset(error, 0, sizeof(error));
		scalar[0] = name_length;
		lengths[0] = 8;
		sources[0] = scalar;
		lengths[1] = name_length;
		sources[1] = open_name;
		lengths[2] = 8;
		sources[2] = handles;
		lengths[3] = (uint32_t)sizeof(error);
		sources[3] = error;
		size = fastrpc_build(packet, physical, physical, 4, lengths, sources);
		parsed = packet;
		if (shared) {
			fastrpc_smem_put(shared_offset, packet, size);
			fastrpc_smem_get(shared_offset, reply, sizeof(reply));
			parsed = reply;
		}
		if (fastrpc_call(1, 0, 0x20200, 0x200 | fastrpc_pd, physical, size, 2000, "remotectl_open")) {
			struct fastrpc_remote_buf *rpra;

			if (shared) {
				fastrpc_smem_get(shared_offset, reply, sizeof(reply));
				parsed = reply;
			}
			rpra = (struct fastrpc_remote_buf *)parsed;

			if (shared) {
				uint32_t out_offset = (uint32_t)(rpra[2].pv - physical);
				uint32_t why_offset = (uint32_t)(rpra[3].pv - physical);
				uint32_t out[2] = { 0, 0 };
				char why[64] = { 0 };

				fastrpc_smem_get(shared_offset + out_offset, (uint8_t *)out, sizeof(out));
				fastrpc_smem_get(shared_offset + why_offset, (uint8_t *)why, sizeof(why) - 1);
				printf("slpi: fastrpc: remotectl handle %u dlret %d error \"%s\"\n", out[0], (int32_t)out[1], why);
				fastrpc_listener = out[0];
			} else {
				uint32_t *out = (uint32_t *)(uintptr_t)rpra[2].pv;
				char *why = (char *)(uintptr_t)rpra[3].pv;

				printf("slpi: fastrpc: remotectl handle %u dlret %d error \"%.64s\"\n", out[0], (int32_t)out[1], why);
				fastrpc_listener = out[0];
			}
		}
	}
	if (fastrpc_bare && stage >= 6) {
		/*
		 * The way the phone's sscrpcd starts: the static listener is initialized and next2 waits
		 * before any sensors PD exists, and only then does the attach start the sensor process,
		 * whose one registry read finds the listener ready.
		 */
		if (fastrpc_attach0) {
			fastrpc_pd_call(0, 0x10000, 0x100, physical, shared_offset, shared, packet, client, "INIT_ATTACH (guest OS)");
		}
		if (fastrpc_attach_mid) {
			/*
			 * The order the phone uses with two processes: the AP is the default listener
			 * first, then the sensors PD starts, and only then does this side wait in next2.
			 * Requests the sensor process makes in between are for the registered listener.
			 */
			uint32_t dl = fastrpc_open_name("adsp_default_listener", physical, shared_offset, packet, reply, 5000);

			if (dl != 0) {
				memset(packet, 0, 4096);
				size = fastrpc_build(packet, physical, physical, 0, lengths, sources);
				if (shared) {
					fastrpc_smem_put(shared_offset, packet, size);
				}
				fastrpc_call(1, dl, 0, 0x300 | fastrpc_pd, physical, size, 5000, "adsp_default_listener_register");
			}
			{
				uint32_t ac = fastrpc_attach_client;
				uint32_t abuf = fastrpc_attach_client ? fastrpc_attach_client : client;

				fastrpc_pd_call(ac, 0x10000, 0x102, physical, shared_offset, shared, packet, abuf,
				    "INIT_ATTACH_SNS (mid)");
			}
		}
		if (fastrpc_static_open) {
			uint32_t opened = fastrpc_open_name("adsp_listener", physical, shared_offset, packet, reply, fastrpc_open_wait);

			if (opened != 0) {
				fastrpc_service = opened;
				printf("slpi: fastrpc: the adsp_listener service handle is %u\n", fastrpc_service);
			}
		}
		memset(packet, 0, 4096);
		size = fastrpc_build(packet, physical, physical, 0, lengths, sources);
		if (shared) {
			fastrpc_smem_put(shared_offset, packet, size);
		}
		if (fastrpc_call(1, fastrpc_service, fastrpc_init2_sc, 0x400 | fastrpc_pd, physical, size, 10000,
		    "adsp_listener_init2") && stage >= 8) {
			if (stage >= 9) {
				fastrpc_listener_start();
			} else {
				fastrpc_listener_poll(physical, shared_offset, shared, packet, reply);
			}
			sleep_ms(200);
			if (!fastrpc_attach_mid) {
				/* a session of its own, so the DSP processes it while the listener's next2 waits */
				uint32_t ac = fastrpc_attach_client;
				uint32_t abuf = fastrpc_attach_client ? fastrpc_attach_client : client;

				fastrpc_pd_call(ac, 0x10000, 0x102, physical, shared_offset, shared, packet, abuf,
				    "INIT_ATTACH_SNS (after the listener)");
			}
		}
	}
	if (!fastrpc_bare && fastrpc_listener != 0) {
		if (fastrpc_listener1) {
			/* the modern interface is opened by name and answered on its own handle */
			fastrpc_service = fastrpc_listener;
			fastrpc_init2_sc = 0x05000000;
			fastrpc_next2_sc = 0x060202;
		} else if (fastrpc_lhandle == 1) {
			fastrpc_service = fastrpc_listener;
		} else if (fastrpc_lhandle > 1) {
			fastrpc_service = fastrpc_lhandle;
		}
		printf("slpi: fastrpc: the listener's service handle is %u (%s)\n", fastrpc_service,
		    fastrpc_listener1 ? "adsp_listener1" : "the static adsp_listener");
	}
	if (fastrpc_listener != 0 && stage >= 6) {
		bool ready = true;

		if (!fastrpc_listener1) {
			/* the handle the open gave, no buffers: the DSP now knows this AP serves its default listener */
			memset(packet, 0, 4096);
			size = fastrpc_build(packet, physical, physical, 0, lengths, sources);
			if (shared) {
				fastrpc_smem_put(shared_offset, packet, size);
			}
			ready = fastrpc_call(1, fastrpc_listener, 0, 0x300 | fastrpc_pd, physical, size, 5000,
			    "adsp_default_listener_register");
			if (ready) {
				/* the reference closes the control handle after registering */
				memset(packet, 0, 4096);
				memset(error, 0, sizeof(error));
				scalar[0] = fastrpc_listener;
				scalar[1] = (uint32_t)sizeof(error);
				lengths[0] = 8;
				sources[0] = scalar;
				lengths[1] = 4;
				sources[1] = handles;
				lengths[2] = (uint32_t)sizeof(error);
				sources[2] = error;
				size = fastrpc_build(packet, physical, physical, 3, lengths, sources);
				if (shared) {
					fastrpc_smem_put(shared_offset, packet, size);
				}
				fastrpc_call(1, 0, 0x01010200, 0x310 | fastrpc_pd, physical, size, 2000, "remotectl_close");
			}
		}
		if (ready && fastrpc_static_open && fastrpc_open_async && !fastrpc_open_after) {
			/* the module load may ask the AP for the library: let the listener serve that request */
			fastrpc_module_open_send(fastrpc_open_pd != 0xffffffff ? fastrpc_open_pd : fastrpc_pd);
		} else if (ready && fastrpc_static_open) {
			/* the static interface's handle is the module opened by name, not a fixed number */
			uint32_t opened = fastrpc_open_name("adsp_listener", physical, shared_offset, packet, reply, fastrpc_open_wait);

			if (opened != 0) {
				fastrpc_service = opened;
				fastrpc_init2_sc = 0x03000000;
				fastrpc_next2_sc = 0x040202;
				printf("slpi: fastrpc: the adsp_listener service handle is %u\n", fastrpc_service);
			}
		}
		if (ready && stage >= 7) {
			/* method 3 on the static interface, method 5 on adsp_listener1: the reverse tunnel's first call */
			memset(packet, 0, 4096);
			size = fastrpc_build(packet, physical, physical, 0, lengths, sources);
			if (shared) {
				fastrpc_smem_put(shared_offset, packet, size);
			}
			if (fastrpc_call(1, fastrpc_service, fastrpc_init2_sc, 0x400 | fastrpc_pd, physical, size, 10000,
			    "adsp_listener_init2") && stage >= 8) {
				if (fastrpc_restart == 1) {
					/*
					 * The sensor process reads the registry when its PD starts, and this
					 * first attach started it before the listener existed. Release it and
					 * attach again now, before next2 waits: an outstanding next2 blocks the
					 * DSP's FastRPC thread, and the release would never be processed.
					 */
					fastrpc_pd_call(0, 0x01010000, 0x500 | fastrpc_pd, physical, shared_offset, shared, packet, client,
					    "INIT_RELEASE");
					sleep_ms(500);
					fastrpc_pd_call(0, 0x10000, 0x102, physical, shared_offset, shared, packet, client,
					    "INIT_ATTACH_SNS (again)");
				} else if (fastrpc_restart == 2) {
					/* the release after the first attach stopped the process; start a fresh one now */
					fastrpc_pd_call(0, 0x10000, 0x102, physical, shared_offset, shared, packet, client,
					    "INIT_ATTACH_SNS (again)");
				}
				if (fastrpc_attach_late) {
					/*
					 * The sensor process reads the registry once, when its PD starts: give it a
					 * listener with a next2 already waiting, and only then start the PD.
					 */
					if (stage >= 9) {
						fastrpc_listener_start();
					} else {
						fastrpc_listener_poll(physical, shared_offset, shared, packet, reply);
					}
					sleep_ms(200);
					memset(packet, 0, 4096);
					lengths[0] = 4;
					sources[0] = &client;
					size = fastrpc_build(packet, physical, physical, 1, lengths, sources);
					if (shared) {
						fastrpc_smem_put(shared_offset, packet, size);
					}
					fastrpc_call(0, 1, 0x10000, 0x102, physical, size, 2000, "INIT_ATTACH_SNS (late)");
				} else if (stage >= 9) {
					fastrpc_listener_start();
				} else {
					fastrpc_listener_poll(physical, shared_offset, shared, packet, reply);
				}
			}
		}
	if (fastrpc_static_open && fastrpc_open_async && fastrpc_open_after) {
		/* the listener is waiting now: the module load's file requests can be served */
		fastrpc_module_open_send(fastrpc_open_pd != 0xffffffff ? fastrpc_open_pd : fastrpc_pd);
	}
	if (stage >= 11 && shared) {
		static const char *const names[] = {
			"adsp_default_listener", "apps_std", "listener", "apps_remotectl", "sns_default_listener", "nonexistent",
		};

		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			fastrpc_open_name(names[i], physical, shared_offset, packet, reply, 2000);
		}
	}
	}
	if (shared) {
		kfree_data(packet, 4096);
	} else {
		hakios_slpi_dma_free(buf, 4096);
	}
	printf("slpi: fastrpc: stage %u: done\n", stage);
}

/* Read-only look at the apps SMMU for the SLPI's FastRPC stream (dt: sdsprpc-smd, iommus 0x541). */
static void
fastrpc_smmu_look(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x100000);
	uint32_t scr0, idr0, idr1, matches, contexts, page, pages, valid = 0, covering = 0;

	if (smmu == 0) {
		printf("slpi: fastrpc: the apps SMMU cannot be mapped\n");
		return;
	}
	scr0 = rd(smmu + 0x000);
	idr0 = rd(smmu + 0x020);
	idr1 = rd(smmu + 0x024);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	printf("slpi: fastrpc: apps SMMU %s; %u match registers, %u contexts, %#x-byte pages; unmatched streams %s\n",
	    scr0 & 1 ? "OFF" : "on", matches, contexts, page, scr0 & 1u << 10 ? "fault" : "pass untranslated");
	for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * n), s2cr = rd(smmu + 0xc00 + 4 * n);
		uint32_t wide = scr0 & 1u << 3, stream = smr & (wide ? 0xffff : 0x7fff);
		uint32_t mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			continue;
		}
		valid++;
		if (((stream ^ 0x541) & ~mask) != 0) {
			continue;
		}
		covering++;
		printf("slpi: fastrpc: SMR %u covers 0x541: stream %#x mask %#x, s2cr %#x, kind %u context %u\n", n, stream, mask,
		    s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		{
			uint32_t context = s2cr & 0xff;
			vm_offset_t cb = smmu + (pages + context) * page;

			printf("slpi: fastrpc: context %u: cbar %#x cba2r %#x sctlr %#x ttbr0 %#x fsr %#x far %#x\n", context,
			    rd(smmu + page + 4 * context), rd(smmu + page + 0x800 + 4 * context), rd(cb + 0x00), rd(cb + 0x20),
			    rd(cb + 0x58), rd(cb + 0x60));
		}
	}
	printf("slpi: fastrpc: %u of %u SMRs are in use; %u cover the SLPI's 0x541%s\n", valid, matches, covering,
	    covering ? "" : " (the boot chain set nothing up for it)");
}

static vm_offset_t fastrpc_smmu_base;
static uint32_t fastrpc_smmu_index, fastrpc_smmu_old_smr, fastrpc_smmu_old_s2cr;
static bool fastrpc_smmu_programmed;

/*
 * Give the SLPI's FastRPC stream a context of its own, bypassing translation: the DSP is trusted
 * firmware, and a first bring-up only needs it to reach a buffer whose physical address the AP
 * puts in fastrpc_msg.addr. The old match register is saved and put back when the DSP stops.
 */
static bool
fastrpc_smmu_bypass(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x100000);
	uint32_t scr0, idr0, idr1, matches, wide, n = 0, smr = 0, s2cr = 0;
	bool used = true;

	if (smmu == 0) {
		printf("slpi: fastrpc: the apps SMMU cannot be mapped for the bypass\n");
		return false;
	}
	scr0 = rd(smmu + 0x000);
	idr0 = rd(smmu + 0x020);
	idr1 = rd(smmu + 0x024);
	matches = idr0 & 0xff;
	wide = (scr0 & 1u << 3) != 0;
	lck_mtx_lock(&hakios_smmu_lock);
	for (n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		smr = rd(smmu + 0x800 + 4 * n);
		s2cr = rd(smmu + 0xc00 + 4 * n);
		used = wide ? (s2cr & 1u << 10) != 0 : (smr >> 31) != 0;
		if (!used) {
			break;
		}
	}
	if (!(!used && n < matches && 0x800 + 4 * n < 0xc00)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		printf("slpi: fastrpc: no free match register for the SLPI's 0x541\n");
		return false;
	}
	fastrpc_smmu_old_smr = smr;
	fastrpc_smmu_old_s2cr = s2cr;
	if (wide) {
		wr(smmu + 0x800 + 4 * n, 0x541);
		wr(smmu + 0xc00 + 4 * n, (1u << 10) | (1u << 16));
	} else {
		wr(smmu + 0x800 + 4 * n, (1u << 31) | 0x541);
		wr(smmu + 0xc00 + 4 * n, 1u << 16);
	}
	settle();
	smr = rd(smmu + 0x800 + 4 * n);
	s2cr = rd(smmu + 0xc00 + 4 * n);
	printf("slpi: fastrpc: SMR %u for 0x541: smr %#x s2cr %#x (bypass); old %#x/%#x\n", n, smr, s2cr,
	    fastrpc_smmu_old_smr, fastrpc_smmu_old_s2cr);
	fastrpc_smmu_base = smmu;
	fastrpc_smmu_index = n;
	fastrpc_smmu_programmed = ((s2cr >> 16 & 3) == 1) && (wide ? (s2cr & 1u << 10) != 0 : (smr >> 31) != 0);
	lck_mtx_unlock(&hakios_smmu_lock);
	return fastrpc_smmu_programmed;
}

static void
fastrpc_smmu_restore(void)
{
	if (!fastrpc_smmu_programmed) {
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	wr(fastrpc_smmu_base + 0x800 + 4 * fastrpc_smmu_index, fastrpc_smmu_old_smr);
	wr(fastrpc_smmu_base + 0xc00 + 4 * fastrpc_smmu_index, fastrpc_smmu_old_s2cr);
	settle();
	lck_mtx_unlock(&hakios_smmu_lock);
	printf("slpi: fastrpc: SMR %u put back to %#x/%#x\n", fastrpc_smmu_index, fastrpc_smmu_old_smr,
	    fastrpc_smmu_old_s2cr);
	fastrpc_smmu_programmed = false;
}

/* ---- APR: the sensor services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's sensor services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_SLPI_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_SLPI                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_SLPI_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_SLPI_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_SLPI_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the sensor services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static hakios_apr_events_t apr_events; /* the sound's (hakios.sound.c): what nobody waits for */
static hakios_apr_events_t apr_events_second;  /* the microphones' (hakios.mic.c) */
static LCK_MTX_DECLARE(apr_lock, &slpi_lock_group);     /* one question at a time */

/* A packet from the DSP's sensor services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		printf("slpi: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		printf("slpi: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	/*
	 * The packet's own size is the DSP's word: below the header it would make the payload wrap to about 4 GB, and the copies
	 * below would then read 256 bytes from beyond this packet. Such a packet is dropped.
	 */
	if ((header.size < length ? header.size : length) < header_length) {
		printf("slpi: apr: a packet of %u bytes with a header of %u: dropped\n", header.size < length ? header.size : length,
		    header_length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		hakios_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("slpi: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	/* (released: the thread that waits must not see "there" before the words and the token above, as with battmgr's answers) */
	os_atomic_store(&apr_answer.there, true, release);
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
hakios_slpi_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct hakios_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_SLPI;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("slpi: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (os_atomic_load(&apr_answer.there, acquire) && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
hakios_slpi_apr_events(hakios_apr_events_t handler)
{
	apr_events = handler;
}

void
hakios_slpi_apr_events_second(hakios_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return hakios_slpi_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_SLPI_APPS ------------------------------------------ */

/*
 * On this SoC the sensor DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_SLPI_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With slpi=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to drivers/power/hakios.battery.c (hakios_battery_report()), and from there to the power source
 * that iOS reads (drivers/power/hakios.battery.cpp). battmgr=2: every property once, for the log.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("slpi: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("slpi: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, 0 } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	battmgr.runs = true;
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("slpi: battmgr: the DSP has not opened PMIC_RTR_SLPI_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("slpi: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 12; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("slpi: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct hakios_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			hakios_battery_report(&b);
		}
		if (round < 3 || round % 6 == 0 || !good) {
			tell("slpi: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA)\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr);
			if (round % 30 == 0 && good) {
				tell("slpi: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		for (unsigned ms = 0; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(void)
{
	thread_t thread;

	uint32_t wanted = 1;            /* (on with slpi=1 unless battmgr=0) */

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	if (option("battery")) {
		/* (the loader's way, through Qualcomm's UEFI charger drivers: one way to the battery at a time) */
		tell("slpi: battmgr: not with battery=1: the battery is asked about the loader's way\n");
		return;
	}
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("slpi: link: THE SENSOR DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else if (from == CHANNEL_FASTRPC) {
				fastrpc_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	fastrpc_listener_wanted = false;
	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("slpi: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
		/*
		 * The buffers we offered the DSP are given back too, so that they do not stay allocated for good: only
		 * glink_pump() uses them, under this lock, and it does nothing while glink.up is false.
		 */
		for (unsigned i = 0; i < sizeof(channel->our_intents) / sizeof(channel->our_intents[0]); i++) {
			if (channel->our_intents[i].data != NULL) {
				kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
				channel->our_intents[i].data = NULL;
			}
			channel->our_intents[i].size = channel->our_intents[i].fill = 0;
		}
		channel->our_intent_count = 0;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the sensor DSP has put into its own");
	if (!glink_find()) {
		printf("slpi: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("slpi: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("slpi: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the sensor DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("slpi: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("slpi: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"apr_audio_svc\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	uint32_t sns_on = 0, fastrpc_smmu = 0, fastrpc_call = 0;

	PE_parse_boot_argn("sns", &sns_on, sizeof(sns_on));
	PE_parse_boot_argn("fastrpc_smmu", &fastrpc_smmu, sizeof(fastrpc_smmu));
	PE_parse_boot_argn("fastrpc_call", &fastrpc_call, sizeof(fastrpc_call));
	PE_parse_boot_argn("fastrpc_pd", &fastrpc_pd, sizeof(fastrpc_pd));
	PE_parse_boot_argn("fastrpc_lhandle", &fastrpc_lhandle, sizeof(fastrpc_lhandle));
	PE_parse_boot_argn("fastrpc_attach_late", &fastrpc_attach_late, sizeof(fastrpc_attach_late));
	PE_parse_boot_argn("fastrpc_restart", &fastrpc_restart, sizeof(fastrpc_restart));
	PE_parse_boot_argn("fastrpc_delay", &fastrpc_delay, sizeof(fastrpc_delay));
	PE_parse_boot_argn("fastrpc_listener1", &fastrpc_listener1, sizeof(fastrpc_listener1));
	PE_parse_boot_argn("fastrpc_bare", &fastrpc_bare, sizeof(fastrpc_bare));
	PE_parse_boot_argn("fastrpc_attach_mid", &fastrpc_attach_mid, sizeof(fastrpc_attach_mid));
	PE_parse_boot_argn("fastrpc_attach0", &fastrpc_attach0, sizeof(fastrpc_attach0));
	PE_parse_boot_argn("fastrpc_static_open", &fastrpc_static_open, sizeof(fastrpc_static_open));
	PE_parse_boot_argn("fastrpc_open_wait", &fastrpc_open_wait, sizeof(fastrpc_open_wait));
	PE_parse_boot_argn("fastrpc_open_async", &fastrpc_open_async, sizeof(fastrpc_open_async));
	PE_parse_boot_argn("fastrpc_open_after", &fastrpc_open_after, sizeof(fastrpc_open_after));
	PE_parse_boot_argn("fastrpc_open_pd", &fastrpc_open_pd, sizeof(fastrpc_open_pd));
	PE_parse_boot_argn("fastrpc_attach_client", &fastrpc_attach_client, sizeof(fastrpc_attach_client));
	PE_parse_boot_argn("fastrpc_attach_tid", &fastrpc_attach_tid, sizeof(fastrpc_attach_tid));
	PE_parse_boot_argn("slpi_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	if (sns_on) {
		sns_reg_advertise();
	}
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	if (sns_on) {
		sns_reg_advertise();
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("slpi: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there) {
			printf("slpi: %u: service %u (instance %u) at node %u port %u\n", step, qrtr_servers[i].service,
			    qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port);
		}
	}
	if (fastrpc_smmu) {
		fastrpc_smmu_look();
	}
	if (fastrpc_smmu >= 2) {
		fastrpc_smmu_bypass();
	}
	if (fastrpc_call && fastrpc_smmu >= 2) {
		fastrpc_first_calls(fastrpc_call);
	}
	if (sns_on) {
		sns_start();
	}
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	PE_parse_boot_argn("slpi_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the sensor DSP's core service over APR whether the sensor services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("slpi: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_SLPI_CORE, AVCS_CMD_SLPI_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_SLPI_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("slpi: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_SLPI_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("slpi: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("slpi: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (slpi_keep, slpi=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("slpi: THE SENSOR DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_slpi_stop(void)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("slpi: the sensor DSP is off: it had been up%s (node %u, %u services), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_slpi_quiesce(void)
{
		if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("slpi: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("slpi: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("slpi: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("slpi: THE SENSOR DSP IS UP AND LINKED, %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
	} else {
		tell("slpi: THE SENSOR DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (drivers/input/hakios.input.c), after the Wi-Fi's start has been set going. */
void
hakios_slpi_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("slpi_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("slpi", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		printf("slpi: slpi=1 and slpi_step=N are for the phone's sensor DSP, and this is not the phone\n");
		return;
	}
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("slpi: no thread: the sensor DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("slpi: the sensor DSP is being brought up beside the system's start (slpi=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("slpi_keep");
	new_from = last_step;
	PE_parse_boot_argn("slpi_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("slpi_pause", &pause_ms, sizeof(pause_ms));
	printf("slpi: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("slpi_return")) {
		printf("slpi: back to the loader (slpi_return)\n");
		loader_return();
	}
	printf("slpi: the system goes on starting\n");
}
