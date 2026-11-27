#include "hakios.ufs_archive.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PART 3
#define FIRST (2042368ULL * 4096)
#define END (5242875ULL * 4096)

static const char *signatures[2];
static int fail_at, reads;

static int
read_signature(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	const uint64_t expected[] = { FIRST + 1024, END - 1024 };

	assert(part == PART && len == 2 && reads < 2);
	assert(offset == expected[reads]);
	if (reads++ == fail_at) {
		return EIO;
	}
	memcpy(dst, signatures[reads - 1], 2);
	return 0;
}

static void
check(const char *primary, const char *backup, int failure, int want, int read_count)
{
	signatures[0] = primary;
	signatures[1] = backup;
	fail_at = failure;
	reads = 0;
	assert(ufs_archive_region_check(read_signature, PART, FIRST, END) == want);
	assert(reads == read_count);
}

int
main(void)
{
	check("\0\0", "\0\0", -1, 0, 2);
	check("H+", "\0\0", -1, EBUSY, 1);
	check("\0\0", "HX", -1, EBUSY, 2);
	check("BD", "\0\0", -1, EBUSY, 1);
	check("\0\0", "\0\0", 0, EIO, 1);
	check("\0\0", "\0\0", 1, EIO, 2);
	assert(ufs_archive_region_check(read_signature, PART, END, FIRST) == EINVAL);
	assert(ufs_archive_region_check(read_signature, PART, FIRST, FIRST + 1024) == EINVAL);
	puts("UFS archive guard protects primary and alternate HFS headers and fails closed on read errors");
	return 0;
}
