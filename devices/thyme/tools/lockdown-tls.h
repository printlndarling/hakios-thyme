#ifndef HAKIOS_LOCKDOWN_TLS_H
#define HAKIOS_LOCKDOWN_TLS_H

#include "lockdown-pairing.h"
#include "hakios.thyme_usbmux.h"
#include <openssl/ssl.h>

struct lockdown_tls {
	SSL *connection;
	X509 *client;
	bool root_client;
	CFStringRef id;
	size_t plaintext_length, plaintext_capacity;
	uint8_t *plaintext;
};

bool lockdown_tls_start(struct lockdown_tls *, const struct lockdown_certificates *, SecKeyRef, size_t);
void lockdown_tls_clear(struct lockdown_tls *);
bool lockdown_tls_receive(struct lockdown_tls *, const uint8_t *, size_t);
int lockdown_tls_output(struct lockdown_tls *, uint8_t *, size_t);

#endif
