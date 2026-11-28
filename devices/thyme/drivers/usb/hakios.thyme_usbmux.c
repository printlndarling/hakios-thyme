/* The initial usbmux exchange uses an 8-byte, big-endian header. */
#include "hakios.thyme_usbmux.h"
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define MUX_HEADER_SIZE 8u
#define MUX_VERSION_SIZE 20u
#define MUX_TCP_HEADER 20u
#define MUX_FRAME_MAX 512u

enum { MUX_VERSION = 0, MUX_TCP = 6 };
enum { TCP_FIN = 1, TCP_SYN = 2, TCP_RST = 4, TCP_PSH = 8, TCP_ACK = 16 };
enum { LOCKDOWN_HEADER = 4, TCP_FRAME_HEADER = MUX_HEADER_SIZE + MUX_TCP_HEADER };
/* End each high-speed transfer with a short packet. */
#define MUX_SEGMENT_MAX 480u
_Static_assert(TCP_FRAME_HEADER + MUX_SEGMENT_MAX < MUX_FRAME_MAX, "reply must fit in one short high-speed packet");
_Static_assert(THYME_LOCKDOWN_MESSAGE_MAX <= 65535u, "receive window is sixteen bits");

static uint8_t stream[2 * MUX_FRAME_MAX];
static unsigned used;
static bool negotiated, online;
static uint16_t peer_port;
static uint32_t peer_next, device_next, next_transaction;
static struct thyme_lockdown_message request;
static unsigned request_length, response_length, response_offset;
static bool request_complete, request_read;
static uint8_t response[THYME_LOCKDOWN_MESSAGE_MAX];

static uint16_t
be16(const uint8_t *bytes)
{
	return (uint16_t)((bytes[0] << 8) | bytes[1]);
}

static void
put_be16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

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

static void
close_stream(void)
{
	peer_port = 0;
	request_length = response_length = response_offset = 0;
	request_complete = request_read = false;
}

void
thyme_usbmux_reset(void)
{
	used = 0;
	negotiated = false;
	close_stream();
}

void
thyme_usbmux_service(bool enabled)
{
	online = enabled;
	if (!online && !request_complete) {
		close_stream();
	}
}

int
thyme_usbmux_request(struct thyme_lockdown_message *message, unsigned capacity)
{
	if (!request_complete || request_read) {
		return 0;
	}
	if (request_length > capacity) {
		return -1;
	}
	message->transaction = request.transaction;
	memcpy(message->data, request.data, request_length);
	request_read = true;
	return (int)request_length;
}

bool
thyme_usbmux_submit(const struct thyme_lockdown_message *message, unsigned length)
{
	if (!request_complete || !request_read || response_length || message->transaction != request.transaction ||
	    length <= LOCKDOWN_HEADER || length > sizeof(response) || be32(message->data) != length - LOCKDOWN_HEADER) {
		return false;
	}
	memcpy(response, message->data, length);
	response_length = length;
	return true;
}

bool
thyme_usbmux_pending(void)
{
	return request_complete;
}

static int
tcp_reply(uint8_t *reply, uint16_t source, uint16_t dest, uint32_t seq,
    uint32_t ack, uint8_t flags, const uint8_t *data, unsigned size)
{
	unsigned length = TCP_FRAME_HEADER + size;
	memset(reply, 0, TCP_FRAME_HEADER);
	put_be32(reply, MUX_TCP);
	put_be32(reply + 4, length);
	uint8_t *tcp = reply + MUX_HEADER_SIZE;
	put_be16(tcp, source);
	put_be16(tcp + 2, dest);
	put_be32(tcp + 4, seq);
	put_be32(tcp + 8, ack);
	tcp[12] = (MUX_TCP_HEADER / 4) << 4;
	tcp[13] = flags;
	if ((flags & TCP_RST) == 0) {
		put_be16(tcp + 14, THYME_LOCKDOWN_MESSAGE_MAX);
	}
	if (size != 0) {
		memcpy(reply + TCP_FRAME_HEADER, data, size);
	}
	return (int)length;
}

static int
send_response_segment(uint8_t *reply)
{
	unsigned remaining = response_length - response_offset;
	unsigned size = remaining > MUX_SEGMENT_MAX ? MUX_SEGMENT_MAX : remaining;
	bool last = size == remaining;
	int length = tcp_reply(reply, THYME_LOCKDOWN_PORT, peer_port, device_next,
	    peer_next, last ? TCP_ACK | TCP_PSH : TCP_ACK, response + response_offset, size);
	device_next += size;
	response_offset += size;
	if (last) {
		response_offset = response_length = 0;
	}
	return length;
}

int
thyme_usbmux_complete(uint8_t *reply, bool timeout)
{
	if (!request_complete) {
		return 0;
	}
	if (timeout || !online) {
		int length = tcp_reply(reply, THYME_LOCKDOWN_PORT, peer_port,
		    device_next, peer_next, TCP_RST | TCP_ACK, NULL, 0);
		close_stream();
		return length;
	}
	if (!response_length) {
		return 0;
	}
	request_complete = request_read = false;
	request_length = 0;
	return send_response_segment(reply);
}

static int
tcp_input(const uint8_t *tcp, unsigned length, uint8_t *reply)
{
	unsigned header = (tcp[12] >> 4) * 4;
	uint16_t source = be16(tcp), dest = be16(tcp + 2);
	uint32_t seq = be32(tcp + 4), ack = be32(tcp + 8);
	uint8_t flags = tcp[13];
	if (header < MUX_TCP_HEADER || header > length || source == 0) {
		return -1;
	}
	unsigned size = length - header;
	if (flags == TCP_SYN && size == 0) {
		if (dest != THYME_LOCKDOWN_PORT || !online || peer_port) {
			return tcp_reply(reply, dest, source, 0, seq + 1, TCP_RST | TCP_ACK, NULL, 0);
		}
		peer_port = source;
		peer_next = seq + 1;
		device_next = 1;
		return tcp_reply(reply, dest, source, 0, peer_next, TCP_SYN | TCP_ACK, NULL, 0);
	}
	if (source != peer_port || dest != THYME_LOCKDOWN_PORT) {
		return -1;
	}
	if ((flags & TCP_RST) && seq == peer_next) {
		close_stream();
		return 0;
	}
	if (seq != peer_next || ack != device_next || request_complete) {
		return -1;
	}
	if (flags == (TCP_FIN | TCP_ACK) && size == 0) {
		close_stream();
		return tcp_reply(reply, dest, source, ack, seq + 1, TCP_FIN | TCP_ACK, NULL, 0);
	}
	if (response_length) {
		return flags == TCP_ACK && size == 0 ? send_response_segment(reply) : -1;
	}
	if ((flags & ~TCP_PSH) != TCP_ACK) {
		return -1;
	}
	if (size == 0) {
		return 0;
	}
	peer_next += size;
	if (size <= sizeof(request.data) - request_length) {
		memcpy(request.data + request_length, tcp + header, size);
		request_length += size;
		if (request_length < LOCKDOWN_HEADER) {
			return tcp_reply(reply, dest, source, device_next, peer_next, TCP_ACK, NULL, 0);
		}
		uint32_t expected = be32(request.data);
		if (expected > 0 && expected <= sizeof(request.data) - LOCKDOWN_HEADER &&
		    request_length <= expected + LOCKDOWN_HEADER) {
			if (request_length < expected + LOCKDOWN_HEADER) {
				return tcp_reply(reply, dest, source, device_next, peer_next, TCP_ACK, NULL, 0);
			}
			request.transaction = ++next_transaction;
			request_complete = true;
			return 0;
		}
	}
	close_stream();
	return tcp_reply(reply, dest, source, device_next, peer_next, TCP_RST | TCP_ACK, NULL, 0);
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
		close_stream();
		negotiated = true;
		return MUX_VERSION_SIZE;
	}
	if (!negotiated || protocol != MUX_TCP || length < MUX_HEADER_SIZE + MUX_TCP_HEADER) {
		return -1;
	}
	return tcp_input(frame + MUX_HEADER_SIZE, length - MUX_HEADER_SIZE, reply);
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
	while (used >= MUX_HEADER_SIZE && !request_complete) {
		uint32_t frame_size = be32(stream + 4);
		if (frame_size < MUX_HEADER_SIZE || frame_size > MUX_FRAME_MAX) {
			used = 0;
			return -1;
		}
		if (frame_size > used) {
			break;
		}
		int result = respond(stream, frame_size, reply);
		used -= frame_size;
		memmove(stream, stream + frame_size, used);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}
