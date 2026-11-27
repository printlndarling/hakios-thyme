/* One bounded NFC-A passive discovery session over thyme's NCI device. */
#include "mini.h"

#define NCI_PACKET_MAX 258
#define NCI_COMMAND_WAIT 12
#define NCI_DISCOVERY_WAIT 40

/* Darwin's error numbers, as returned by /dev/hakios-nfc. */
#define NFC_IO_ERROR 5
#define NFC_BUSY 16
#define NFC_AGAIN 35

static int
next_packet(int fd, uint8_t packet[NCI_PACKET_MAX], unsigned attempts)
{
	for (unsigned i = 0; i < attempts; i++) {
		ssize_t n = read(fd, packet, NCI_PACKET_MAX);

		if (n < 0 && (errno == NFC_AGAIN || errno == NFC_BUSY)) {
			if (errno == NFC_BUSY) {
				usleep(20000);
			}
			continue;
		}
		if (n < 0) {
			printf("nfc_poll: read failed (errno %d)\n", errno);
			return -1;
		}
		if (n < 3 || n != 3 + packet[2]) {
			printf("nfc_poll: invalid NCI packet size %ld\n", (long)n);
			return -1;
		}
		return (int)n;
	}
	return 0;
}

static unsigned
expect_packet(int fd, uint8_t group, uint8_t opcode, uint8_t packet[NCI_PACKET_MAX])
{
	/* A notification may arrive while a command response is outstanding. */
	for (unsigned i = 0; i < 8; i++) {
		int len = next_packet(fd, packet, NCI_COMMAND_WAIT);

		if (len <= 0) {
			return 0;
		}
		if (packet[0] == group && packet[1] == opcode) {
			return (unsigned)len;
		}
	}
	return 0;
}

static int
command(int fd, const char *name, const uint8_t *cmd, unsigned size)
{
	uint8_t response[NCI_PACKET_MAX];
	ssize_t written = write(fd, cmd, size);

	/* The NXP controller can NACK the first write when waking from standby. */
	if (written < 0 && (errno == NFC_IO_ERROR || errno == NFC_BUSY)) {
		usleep(110000);
		written = write(fd, cmd, size);
	}
	if (written != (ssize_t)size) {
		printf("nfc_poll: %s write failed (errno %d)\n", name, errno);
		return 0;
	}
	if (expect_packet(fd, (uint8_t)(0x40 | (cmd[0] & 0x0f)), cmd[1], response) < 4) {
		printf("nfc_poll: %s response missing\n", name);
		return 0;
	}
	if (response[3] != 0) {
		printf("nfc_poll: %s rejected (NCI status %#x)\n", name, response[3]);
		return 0;
	}
	printf("nfc_poll: %s accepted\n", name);
	return 1;
}

int
main(__attribute__((unused)) int argc, __attribute__((unused)) char **argv, __attribute__((unused)) char **envp)
{
	static const uint8_t reset[] = { 0x20, 0x00, 0x01, 0x01 };
	static const uint8_t init[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
	/* Type 2 Tag -> Frame, ISO-DEP -> ISO-DEP, both enabled only for polling. */
	static const uint8_t mapping[] = { 0x21, 0x00, 0x07, 2, 0x02, 0x01, 0x01, 0x04, 0x01, 0x02 };
	static const uint8_t discover[] = { 0x21, 0x03, 0x03, 1, 0x00, 1 };
	static const uint8_t deactivate[] = { 0x21, 0x06, 0x01, 0x00 };
	uint8_t packet[NCI_PACKET_MAX];
	int fd = open("/dev/hakios-nfc", O_RDWR, 0);

	if (fd < 0) {
		printf("nfc_poll: /dev/hakios-nfc (errno %d)\n", errno);
		return 1;
	}
	int ok = command(fd, "CORE_RESET", reset, sizeof(reset));

	if (ok) {
		unsigned len = expect_packet(fd, 0x60, 0x00, packet);

		ok = len >= 6 && packet[5] >= 0x20;
		if (!ok) {
			puts("nfc_poll: NCI 2.x reset notification missing");
		}
	}
	if (ok) {
		ok = command(fd, "CORE_INIT", init, sizeof(init)) &&
		    command(fd, "RF_DISCOVER_MAP", mapping, sizeof(mapping));
	}
	if (ok) {
		unsigned detected = 0;

		ok = command(fd, "RF_DISCOVER", discover, sizeof(discover));
		for (unsigned i = 0; ok && i < NCI_DISCOVERY_WAIT; i++) {
			int len = next_packet(fd, packet, 1);

			if (len < 0) {
				ok = 0;
				break;
			}

			if (len >= 8 && packet[0] == 0x61 && packet[1] == 0x05) {
				printf("nfc_poll: RF interface activated (interface %#x protocol %#x mode %#x)\n",
				    packet[4], packet[5], packet[6]);
				detected++;
				break;
			}
			if (len >= 8 && packet[0] == 0x61 && packet[1] == 0x03) {
				printf("nfc_poll: RF target discovered (protocol %#x mode %#x)\n", packet[4], packet[5]);
				detected++;
			}
		}
		printf("nfc_poll: %u target notifications in this session\n", detected);
		ok = command(fd, "RF_DEACTIVATE", deactivate, sizeof(deactivate)) && ok;
	}
	close(fd);
	return ok ? 0 : 1;
}
