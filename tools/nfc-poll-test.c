/* Replay the production scanner against a stand-in packet device. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

int nfc_poll_main(int argc, char **argv, char **envp);
int nfc_test_errno;

static unsigned written, packets, closes, retries;
static int reject_discovery, corrupt_notification, standby_nack;

int
nfc_test_open(const char *path, int flags, int mode)
{
	assert(strcmp(path, "/dev/hakios-nfc") == 0);
	assert(flags == 2 && mode == 0);
	return 3;
}

ssize_t
nfc_test_write(int fd, const void *buffer, size_t size)
{
	static const uint8_t groups[] = { 0x20, 0x20, 0x21, 0x21, 0x21 };
	static const uint8_t opcodes[] = { 0, 1, 0, 3, 6 };
	const uint8_t *command = buffer;

	assert(fd == 3 && written < sizeof(opcodes));
	assert(size == 3u + command[2]);
	assert(command[0] == groups[written] && command[1] == opcodes[written]);
	if (written == 3) {
		assert(size == 6 && command[3] == 1 && command[4] == 0 && command[5] == 1);
	}
	if (written == 4 && standby_nack && retries == 0) {
		nfc_test_errno = 5;
		return -1;
	}
	written++;
	return (ssize_t)size;
}

ssize_t
nfc_test_read(int fd, void *buffer, size_t size)
{
	static const uint8_t responses[][14] = {
		{ 0x40, 0x00, 1, 0 },
		{ 0x60, 0x00, 3, 1, 1, 0x20 },
		{ 0x40, 0x01, 1, 0 },
		{ 0x41, 0x00, 1, 0 },
		{ 0x41, 0x03, 1, 0 },
		{ 0x61, 0x05, 11, 1, 1, 2, 0, 32, 1, 0, 0, 0, 0, 0 },
		{ 0x41, 0x06, 1, 0 },
	};
	unsigned index = packets++;
	uint8_t *packet = buffer;

	assert(fd == 3 && size == 258 && index < 7);
	if (reject_discovery && index == 5) {
		index++;
	}
	memcpy(packet, responses[index], sizeof(responses[index]));
	if (index == 4 && reject_discovery) {
		packet[3] = 1;
	}
	if (index == 5 && corrupt_notification) {
		return 7;
	}
	return 3 + packet[2];
}

int
nfc_test_close(int fd)
{
	assert(fd == 3);
	closes++;
	return 0;
}

int
nfc_test_usleep(unsigned delay)
{
	assert(delay == 110000);
	retries++;
	return 0;
}

static void
check(int rejected, int corrupt, int standby, int result, unsigned expected_packets)
{
	written = packets = closes = retries = 0;
	reject_discovery = rejected;
	corrupt_notification = corrupt;
	standby_nack = standby;
	assert(nfc_poll_main(0, NULL, NULL) == result);
	assert(written == 5 && packets == expected_packets && closes == 1);
	assert(retries == (unsigned)standby);
}

int
main(void)
{
	check(0, 0, 0, 0, 7);
	check(1, 0, 0, 1, 6);
	check(0, 1, 0, 1, 7);
	check(0, 0, 1, 0, 7);
	puts("NCI polling startup and cleanup passed");
	return 0;
}
