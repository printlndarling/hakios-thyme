/* The initial usbmux exchange uses an 8-byte, big-endian header. */
#include <stdint.h>
#include <stdbool.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

void thyme_usbmux_reset(void);
int thyme_usbmux_feed(const uint8_t *input, unsigned length, uint8_t *reply);

#define MUX_HEADER_SIZE 8u
#define MUX_VERSION_SIZE 20u
#define MUX_TCP_HEADER 20u
#define MUX_FRAME_MAX 512u

enum { MUX_VERSION = 0, MUX_TCP = 6 };
enum { TCP_SYN = 0x02, TCP_RST_ACK = 0x14 };

static uint8_t stream[2 * MUX_FRAME_MAX];
static unsigned used;
static bool negotiated;

static uint32_t
be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	    ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void
put_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

void
thyme_usbmux_reset(void)
{
	used = 0;
	negotiated = false;
}

static int
respond(const uint8_t *frame, unsigned length, uint8_t *reply)
{
	uint32_t protocol = be32(frame);
	if (protocol == MUX_VERSION && length == MUX_VERSION_SIZE) {
		uint32_t major = be32(frame + MUX_HEADER_SIZE);
		if ((major != 1 && major != 2) || be32(frame + 12) != 0) {
			return -1;
		}
		memset(reply, 0, MUX_VERSION_SIZE);
		put_be32(reply + 4, MUX_VERSION_SIZE);
		put_be32(reply + MUX_HEADER_SIZE, 1);
		negotiated = true;
		return MUX_VERSION_SIZE;
	}
	if (!negotiated || protocol != MUX_TCP || length < MUX_HEADER_SIZE + MUX_TCP_HEADER) {
		return -1;
	}
	const uint8_t *tcp = frame + MUX_HEADER_SIZE;
	unsigned header = (tcp[12] >> 4) * 4;
	if (tcp[13] != TCP_SYN || header < MUX_TCP_HEADER || length != MUX_HEADER_SIZE + header) {
		return -1;
	}
	/* No service is listening in this profile. Return a reset acknowledging the SYN. */
	unsigned response_size = MUX_HEADER_SIZE + MUX_TCP_HEADER;
	memset(reply, 0, response_size);
	put_be32(reply, MUX_TCP);
	put_be32(reply + 4, response_size);
	uint8_t *reset = reply + MUX_HEADER_SIZE;
	memcpy(reset, tcp + 2, 2);
	memcpy(reset + 2, tcp, 2);
	put_be32(reset + 8, be32(tcp + 4) + 1);
	reset[12] = (MUX_TCP_HEADER / 4) << 4;
	reset[13] = TCP_RST_ACK;
	return (int)response_size;
}

/* One USB receive may cover a fragment or several small frames. Keep any remainder
 * while the reply is in flight. A zero-length call drains another buffered frame. */
int
thyme_usbmux_feed(const uint8_t *input, unsigned length, uint8_t *reply)
{
	if (length > MUX_FRAME_MAX || length > sizeof(stream) - used) {
		used = 0;
		return -1;
	}
	if (length != 0) {
		memcpy(stream + used, input, length);
		used += length;
	}
	if (used < MUX_HEADER_SIZE) {
		return 0;
	}
	uint32_t frame_size = be32(stream + 4);
	if (frame_size < MUX_HEADER_SIZE || frame_size > MUX_FRAME_MAX) {
		used = 0;
		return -1;
	}
	if (frame_size > used) {
		return 0;
	}
	int result = respond(stream, frame_size, reply);
	used -= frame_size;
	memmove(stream, stream + frame_size, used);
	return result;
}
