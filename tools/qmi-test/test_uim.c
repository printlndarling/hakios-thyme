#include "hakios.uim_status.h"

#include <assert.h>
#include <stdio.h>

int
main(void)
{
	/* Two present cards, with independent GW records. Card 1 also has an ISIM. */
	const uint8_t dual[] = {
		0, 0, 0xff, 0xff, 0, 1, 0xff, 0xff, 2,
		1, 0, 0, 0, 0, 2,
		2, 7, 0, 0, 0, 0, 2, 0xa0, 0x01, 0, 0, 0, 0, 0, 0, 0,
		5, 7, 0, 0, 0, 0, 1, 0xa1, 0, 0, 0, 0, 0, 0, 0,
		1, 0, 0, 0, 0, 1,
		2, 1, 0, 0, 0, 0, 1, 0xa2, 0, 0, 0, 0, 0, 0, 0,
	};
	const uint8_t one[] = {
		0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2,
		1, 0, 0, 0, 0, 1,
		2, 7, 0, 0, 0, 0, 2, 0xa0, 0x01, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0,
	};
	struct uim_inventory inventory;

	assert(uim_status_inventory(one, sizeof(one), &inventory));
	assert(inventory.slots == 2 && inventory.present == 1 && inventory.ready == 1);
	assert(inventory.primary_gw == 0 && inventory.secondary_gw == UIM_NO_SESSION);
	assert(uim_status_inventory(dual, sizeof(dual), &inventory));
	assert(inventory.slots == 2 && inventory.present == 2 && inventory.ready == 1);
	assert(inventory.primary_gw == 0 && inventory.secondary_gw == 0x0100);
	for (size_t n = 0; n < sizeof(dual); n++) {
		assert(!uim_status_inventory(dual, n, &inventory));
	}
	assert(inventory.present == 2 && inventory.secondary_gw == 0x0100);
	puts("UIM inventory distinguishes the two GW records and rejects truncated card status");
	return 0;
}
