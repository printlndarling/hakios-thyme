/* Card Status TLV of QMI UIM Get Card Status (0x002f). */
#ifndef _HAKIOS_UIM_STATUS_H_
#define _HAKIOS_UIM_STATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UIM_NO_SESSION 0xffffu

struct uim_inventory {
	unsigned primary_gw;
	unsigned secondary_gw;
	unsigned slots;
	unsigned present;
	unsigned ready;
};

static inline bool
uim_status_inventory(const uint8_t *status, size_t size, struct uim_inventory *inventory)
{
	if (size < 9) {
		return false;
	}
	struct uim_inventory parsed = {
		status[0] | (unsigned)status[1] << 8,
		status[4] | (unsigned)status[5] << 8,
		status[8], 0, 0,
	};
	size_t at = 9;

	for (unsigned slot = 0; slot < parsed.slots; slot++) {
		if (size - at < 6) {
			return false;
		}
		bool present = status[at] == 1, ready = false;
		unsigned apps = status[at + 5];

		at += 6;
		for (unsigned app = 0; app < apps; app++) {
			if (size - at < 14 || status[at + 6] > size - at - 14) {
				return false;
			}
			ready |= (status[at] == 1 || status[at] == 2) && status[at + 1] == 7;
			at += 14 + status[at + 6];
		}
		parsed.present += present;
		parsed.ready += present && ready;
	}

	*inventory = parsed;
	return true;
}

#endif
