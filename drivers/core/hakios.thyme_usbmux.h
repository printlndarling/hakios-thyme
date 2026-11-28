#ifndef HAKIOS_THYME_USBMUX_H
#define HAKIOS_THYME_USBMUX_H

#include <stdbool.h>
#include <stdint.h>

#define THYME_LOCKDOWN_PORT 62078u
#define THYME_LOCKDOWN_MESSAGE_MAX 32768u
#define THYME_LOCKDOWN_NODE "/dev/hakios-lockdown"

#define THYME_LOCKDOWN_TLS_RECORD 0x80000000u
#define THYME_LOCKDOWN_START_TLS  0x40000000u

/* The native-endian transaction ID precedes a framed plist or, when
 * TLS_RECORD is set, one TLS record. Echo the ID on write. A TLS write
 * can contain zero or more outbound TLS records. Set START_TLS on a
 * plist reply to upgrade this stream once the complete reply is sent. */
struct thyme_lockdown_message {
	uint32_t transaction;
	uint8_t data[THYME_LOCKDOWN_MESSAGE_MAX];
};

void thyme_usbmux_reset(void);
void thyme_usbmux_service(bool online);
int thyme_usbmux_feed(const uint8_t *input, unsigned length, uint8_t *reply);
int thyme_usbmux_request(struct thyme_lockdown_message *message, unsigned capacity);
bool thyme_usbmux_submit(const struct thyme_lockdown_message *message, unsigned length);
bool thyme_usbmux_pending(void);
int thyme_usbmux_complete(uint8_t *reply, bool timeout);

#endif
