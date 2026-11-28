#ifndef HAKIOS_AFC_RAM_H
#define HAKIOS_AFC_RAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AFC_HEADER_SIZE 40u
#define AFC_REQUEST_MAX (1024u * 1024u + AFC_HEADER_SIZE + 8u)

struct afc_handle {
	int fd;
	uint64_t id;
};

struct afc_ram {
	int root;
	struct afc_handle files[16];
	uint64_t next_handle;
	uint64_t bytes_written;
};

bool afc_ram_open(struct afc_ram *);
void afc_ram_close(struct afc_ram *);
/* Call with at least AFC_HEADER_SIZE bytes. Zero indicates an invalid header. */
size_t afc_request_size(const uint8_t *);
size_t afc_ram_reply(struct afc_ram *, const uint8_t *, size_t, uint8_t *, size_t);

#endif
