/* Link with hakios.thyme_usbmux.c to test the same parser used by XNU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hakios.thyme_usbmux.h"

static const uint8_t version[] = {
	0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t reply_version[] = {
	0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t syn[] = {
	0, 0, 0, 6, 0, 0, 0, 28, 0xc0, 0, 0xf2, 0x7e,
	0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0x50, 2, 0x02, 0, 0, 0, 0, 0,
};
static const uint8_t reset[] = {
	0, 0, 0, 6, 0, 0, 0, 28, 0xf2, 0x7e, 0xc0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0x50, 0x14, 0, 0, 0, 0, 0, 0,
};

static void
open_lockdown(uint8_t *reply)
{
	assert(thyme_usbmux_feed(version, sizeof(version), reply) == 20);
	assert(thyme_usbmux_feed(syn, sizeof(syn), reply) == 28);
	assert(reply[21] == 0x12 && reply[15] == 0 && reply[19] == 0);
}

static void
put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = value >> (24 - 8 * i);
	}
}

static int
data(const uint8_t *payload, unsigned length, uint32_t sequence, uint32_t acknowledgement, uint8_t *reply)
{
	uint8_t packet[512];
	assert(length <= sizeof(packet) - sizeof(syn));
	memcpy(packet, syn, sizeof(syn));
	put32(packet + 4, sizeof(syn) + length);
	put32(packet + 12, sequence);
	put32(packet + 16, acknowledgement);
	packet[21] = length ? 0x18 : 0x10;
	if (length) {
		memcpy(packet + sizeof(syn), payload, length);
	}
	return thyme_usbmux_feed(packet, sizeof(syn) + length, reply);
}

static void
lockdown_mailbox(uint8_t *reply)
{
	const uint8_t body[] = {0, 0, 0, 1, 'x'};
	struct thyme_lockdown_message message;
	thyme_usbmux_reset();
	thyme_usbmux_service(true);
	open_lockdown(reply);
	assert(data(body, 2, 1, 1, reply) < 0); /* out of sequence */
	assert(data(body, 2, 0, 1, reply) == 28);
	assert(reply[21] == 0x10 && reply[19] == 2);
	assert(!thyme_usbmux_pending());
	assert(thyme_usbmux_request(&message, sizeof(message.data)) == 0);
	assert(data(body + 2, 3, 2, 1, reply) == 0);
	assert(thyme_usbmux_pending());
	assert(thyme_usbmux_request(&message, sizeof(message.data)) == sizeof(body));
	assert(memcmp(message.data, body, sizeof(body)) == 0);
	assert(thyme_usbmux_request(&message, sizeof(message.data)) == 0);
	assert(!thyme_usbmux_submit(&message, sizeof(body) - 1));
	assert(thyme_usbmux_complete(reply, false) == 0);

	thyme_usbmux_reset();
	open_lockdown(reply);
	assert(data(body, sizeof(body), 0, 1, reply) == 0);
	assert(!thyme_usbmux_submit(&message, sizeof(body)));
	uint32_t old = message.transaction;
	assert(thyme_usbmux_request(&message, sizeof(message.data)) == sizeof(body));
	assert(old != message.transaction);
	message.data[4] = 'y';
	assert(thyme_usbmux_submit(&message, sizeof(body)));
	assert(thyme_usbmux_complete(reply, false) == 28 + sizeof(body));
	assert(reply[21] == 0x18 && reply[15] == 1 && reply[19] == 5);
	assert(reply[32] == 'y');

	assert(data(body, sizeof(body), 5, 6, reply) == 0);
	assert(thyme_usbmux_complete(reply, true) == 28 && reply[21] == 0x14);
	assert(!thyme_usbmux_pending());
	assert(!thyme_usbmux_submit(&message, sizeof(body)));
	open_lockdown(reply);
	assert(data(body, sizeof(body), 0, 1, reply) == 0);
	thyme_usbmux_service(false);
	assert(thyme_usbmux_complete(reply, false) == 28 && reply[21] == 0x14);
	assert(thyme_usbmux_feed(syn, sizeof(syn), reply) == 28 && reply[21] == 0x14);
}

static void
large_record(uint8_t *reply)
{
	static uint8_t body[THYME_LOCKDOWN_MESSAGE_MAX];
	static struct thyme_lockdown_message message;
	thyme_usbmux_reset();
	thyme_usbmux_service(true);
	open_lockdown(reply);
	put32(body, sizeof(body) - 4);
	for (unsigned at = 4; at < sizeof(body); at++) {
		body[at] = (uint8_t)(at ^ (at >> 8));
	}
	for (unsigned offset = 0; offset < sizeof(body);) {
		unsigned remaining = sizeof(body) - offset;
		unsigned size = remaining > 484 ? 484 : remaining;
		int length = data(body + offset, size, offset, 1, reply);
		assert(length == (size == remaining ? 0 : 28));
		offset += size;
	}
	assert(thyme_usbmux_request(&message, 1024) == -1);
	assert(thyme_usbmux_request(&message, sizeof(message.data)) == sizeof(body));
	assert(memcmp(message.data, body, sizeof(body)) == 0);
	assert(thyme_usbmux_submit(&message, sizeof(body)));
	int length = thyme_usbmux_complete(reply, false);
	for (unsigned offset = 0;;) {
		assert(length > 28 && length <= 508);
		unsigned size = (unsigned)length - 28;
		assert(size <= sizeof(body) - offset);
		assert(memcmp(reply + 28, body + offset, size) == 0);
		offset += size;
		if (offset == sizeof(body)) {
			assert(reply[21] == 0x18);
			break;
		}
		assert(reply[21] == 0x10);
		assert(data(NULL, 0, sizeof(body), offset, reply) < 0); /* reject a stale ack */
		length = data(NULL, 0, sizeof(body), offset + 1, reply);
	}
	uint8_t oversized[4];
	put32(oversized, sizeof(body) - 3);
	assert(data(oversized, sizeof(oversized), sizeof(body), sizeof(body) + 1, reply) == 28);
	assert(reply[21] == 0x14);
	open_lockdown(reply);
	thyme_usbmux_reset();
	thyme_usbmux_service(false);
}

int
main(void)
{
	uint8_t reply[512], combined[sizeof(version) + sizeof(syn)];
	for (unsigned split = 1; split < sizeof(version); split++) {
		thyme_usbmux_reset();
		assert(thyme_usbmux_feed(version, split, reply) == 0);
		assert(thyme_usbmux_feed(version + split, sizeof(version) - split, reply) == 20);
		assert(memcmp(reply, reply_version, sizeof(reply_version)) == 0);
	}
	thyme_usbmux_reset();
	assert(thyme_usbmux_feed(syn, sizeof(syn), reply) < 0);
	memcpy(combined, version, sizeof(version));
	memcpy(combined + sizeof(version), syn, sizeof(syn));
	assert(thyme_usbmux_feed(combined, sizeof(combined), reply) == 20);
	assert(memcmp(reply, reply_version, sizeof(reply_version)) == 0);
	assert(thyme_usbmux_feed(NULL, 0, reply) == 28);
	assert(memcmp(reply, reset, sizeof(reset)) == 0);
	assert(thyme_usbmux_feed(NULL, 0, reply) == 0);

	uint8_t bad[sizeof(syn)];
	memcpy(bad, syn, sizeof(bad));
	bad[20] = 0x40; /* TCP header shorter than 20 bytes */
	assert(thyme_usbmux_feed(bad, sizeof(bad), reply) < 0);
	memcpy(bad, syn, sizeof(bad));
	bad[7] = 7;
	assert(thyme_usbmux_feed(bad, sizeof(bad), reply) < 0);
	memcpy(bad, syn, sizeof(bad));
	bad[6] = 2; /* advertised wire length larger than 512 bytes */
	assert(thyme_usbmux_feed(bad, sizeof(bad), reply) < 0);
	assert(thyme_usbmux_feed(version, sizeof(version), reply) == 20);
	memcpy(bad, version, sizeof(version));
	bad[11] = 9;
	assert(thyme_usbmux_feed(bad, sizeof(version), reply) < 0);
	assert(thyme_usbmux_feed(syn, sizeof(syn), reply) == 28);
	thyme_usbmux_reset();
	assert(thyme_usbmux_feed(syn, sizeof(syn), reply) < 0);
	lockdown_mailbox(reply);
	large_record(reply);
	puts("usbmux v1 replay passed (small and 32 KiB records, stale ACKs and closed ports)");
	return 0;
}
