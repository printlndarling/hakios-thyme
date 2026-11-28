#ifndef HAKIOS_LOCKDOWN_PAIRING_H
#define HAKIOS_LOCKDOWN_PAIRING_H

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdbool.h>

struct lockdown_pairing {
	CFArrayRef accepted;
};

struct lockdown_certificates {
	CFDataRef root, host, device;
};

/* Recheck the active RAM approval. The returned certificates borrow state. */
bool lockdown_pair_authorized(struct lockdown_pairing *, CFDictionaryRef, struct lockdown_certificates *);

/* Return a protocol error, or NULL on acceptance. */
CFStringRef lockdown_pair_request(struct lockdown_pairing *, CFDictionaryRef, SecKeyRef, bool);
void lockdown_pair_clear(struct lockdown_pairing *);

#endif
