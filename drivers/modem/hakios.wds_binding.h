/* Keep the bridge's primary WDS fallback subordinate to a client's own bind. */
#ifndef _HAKIOS_WDS_BINDING_H_
#define _HAKIOS_WDS_BINDING_H_

#include <stdint.h>

enum wds_binding {
	WDS_UNBOUND,
	WDS_FALLBACK,
	WDS_EXPLICIT,
};

enum wds_bind_action {
	WDS_BIND_NONE,
	WDS_BIND_BEFORE,
	WDS_BIND_AFTER_MUX,
	WDS_RECORD_EXPLICIT,
};

#define WDS_BIND_MUX_DATA_PORT 0x00a2u
#define WDS_BIND_SUBSCRIPTION  0x00afu

static inline enum wds_bind_action
wds_bind_action_for(enum wds_binding binding, uint32_t message)
{
	if (message == WDS_BIND_SUBSCRIPTION) {
		return WDS_RECORD_EXPLICIT;
	}
	if (binding == WDS_EXPLICIT || message == 0) {
		return WDS_BIND_NONE;
	}
	if (message == WDS_BIND_MUX_DATA_PORT) {
		return WDS_BIND_AFTER_MUX;
	}
	return binding == WDS_UNBOUND ? WDS_BIND_BEFORE : WDS_BIND_NONE;
}

#endif
