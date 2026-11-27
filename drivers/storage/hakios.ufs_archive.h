/* Keep a raw /var archive out of the overlapping HFS data volume. */
#ifndef _HAKIOS_UFS_ARCHIVE_H_
#define _HAKIOS_UFS_ARCHIVE_H_

#include <stdint.h>
#include <sys/errno.h>

/*
 * HFS Plus and HFSX place a header 1024 bytes from each end of the volume.
 * Recognize the classic HFS wrapper too. Either signature is enough to
 * reserve the volume, including when its other header needs recovery.
 */
static inline int
ufs_archive_region_check(int (*read)(uint32_t, uint64_t, uint8_t *, uint64_t),
    uint32_t part, uint64_t first, uint64_t end)
{
	uint8_t signature[2];

	if (end < first || end - first < 4096) {
		return EINVAL;
	}
	const uint64_t headers[] = { first + 1024, end - 1024 };

	for (unsigned i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
		int error = read(part, headers[i], signature, sizeof(signature));

		if (error != 0) {
			return error;
		}
		if ((signature[0] == 'H' && (signature[1] == '+' || signature[1] == 'X')) ||
		    (signature[0] == 'B' && signature[1] == 'D')) {
			return EBUSY;
		}
	}
	return 0;
}

#endif
