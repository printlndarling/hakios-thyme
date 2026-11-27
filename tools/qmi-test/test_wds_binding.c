#include "hakios.wds_binding.h"

#include <assert.h>
#include <stdio.h>

#define WDS_START_NETWORK 0x0020u

int
main(void)
{
	assert(wds_bind_action_for(WDS_UNBOUND, WDS_START_NETWORK) == WDS_BIND_BEFORE);
	assert(wds_bind_action_for(WDS_UNBOUND, WDS_BIND_MUX_DATA_PORT) == WDS_BIND_AFTER_MUX);
	assert(wds_bind_action_for(WDS_FALLBACK, WDS_BIND_MUX_DATA_PORT) == WDS_BIND_AFTER_MUX);
	assert(wds_bind_action_for(WDS_FALLBACK, WDS_START_NETWORK) == WDS_BIND_NONE);

	assert(wds_bind_action_for(WDS_UNBOUND, WDS_BIND_SUBSCRIPTION) == WDS_RECORD_EXPLICIT);
	assert(wds_bind_action_for(WDS_FALLBACK, WDS_BIND_SUBSCRIPTION) == WDS_RECORD_EXPLICIT);
	assert(wds_bind_action_for(WDS_EXPLICIT, WDS_BIND_SUBSCRIPTION) == WDS_RECORD_EXPLICIT);
	assert(wds_bind_action_for(WDS_EXPLICIT, WDS_BIND_MUX_DATA_PORT) == WDS_BIND_NONE);
	assert(wds_bind_action_for(WDS_EXPLICIT, WDS_START_NETWORK) == WDS_BIND_NONE);
	assert(wds_bind_action_for(WDS_UNBOUND, 0) == WDS_BIND_NONE);
	puts("WDS preserves explicit subscription selection across mux rebinds");
	return 0;
}
