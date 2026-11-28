/* Link with hakios.thyme_usbmux.c to test the same parser used by XNU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void thyme_usbmux_reset(void);
int thyme_usbmux_feed(const uint8_t *, unsigned, uint8_t *);

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

int
main(void)
{
	uint8_t reply[32], combined[sizeof(version) + sizeof(syn)];
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
	puts("usbmux v1 wire replay passed (fragmentation, coalescing, invalid frames and closed ports)");
	return 0;
}
