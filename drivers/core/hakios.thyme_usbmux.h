#ifndef HAKIOS_THYME_USBMUX_H
#define HAKIOS_THYME_USBMUX_H

#include <stdbool.h>
#include <stdint.h>

#define THYME_LOCKDOWN_PORT 62078u
#define THYME_LOCKDOWN_MESSAGE_MAX 32768u
#define THYME_LOCKDOWN_NODE "/dev/hakios-lockdown"

/* Device node reads a native-endian transaction ID and a framed plist.
 * A write must return that ID and exactly one framed response. */
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
