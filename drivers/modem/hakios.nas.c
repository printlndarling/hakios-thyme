/*
 * Small QMI NAS parser. Wire IDs and TLVs follow libqmi's qmi-service-nas.json:
 * Serving System (0x24), Get Signal Info (0x4f), Signal Info indication (0x51).
 * It only consumes reports. It never changes modem registration or radio power.
 */
#include <sys/hakios.nas.h>
#include <sys/systm.h>
#include <sys/hakios.qrtr.h>

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint8_t
bars_for(int16_t dbm)
{
	if (dbm < -120) return 1;
	if (dbm < -110) return 2;
	if (dbm < -100) return 3;
	return 4;
}

/* The network's description of the PLMN (serving system TLV 0x12) can be
 * UTF-16LE -- China Broadnet's is 2d 4e fd 56 7f 5e 35 75, "中国广电" --
 * while an all-ASCII name arrives as plain bytes.  The former is decoded
 * to UTF-8; the latter is copied as it comes. */
static int
nas_utf16_plausible(const uint8_t *p, unsigned n)
{
	unsigned i, units = n / 2, good = 0;

	if (units == 0) return 0;
	for (i = 0; i < units; i++) {
		uint16_t u = (uint16_t)(p[i * 2] | p[i * 2 + 1] << 8);

		if ((u >= 0x20 && u <= 0x7e) || (u >= 0x4e00 && u <= 0x9fff) ||
		    (u >= 0x3000 && u <= 0x30ff) || (u >= 0xac00 && u <= 0xd7af))
			good++;
	}
	return good * 4 >= units * 3;
}

static unsigned
nas_utf8(char *out, size_t cap, unsigned at, uint32_t cp)
{
	if (cp < 0x80) {
		if (at + 1 < cap) out[at] = (char)cp;
		return at + 1;
	}
	if (cp < 0x800) {
		if (at + 2 < cap) {
			out[at] = (char)(0xc0 | cp >> 6);
			out[at + 1] = (char)(0x80 | (cp & 0x3f));
		}
		return at + 2;
	}
	if (at + 3 < cap) {
		out[at] = (char)(0xe0 | cp >> 12);
		out[at + 1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[at + 2] = (char)(0x80 | (cp & 0x3f));
	}
	return at + 3;
}

int
hakios_nas_apply(struct hakios_nas_state *state, const void *frame, size_t length)
{
	const uint8_t *data = frame;
	struct hakios_nas_state next;
	uint16_t message, payload_size;
	size_t at;
	bool serving = false, signal = false, success = false, result_seen = false;
	int16_t level = 0;
	uint8_t radio = 0;

	if (state == NULL || data == NULL || length < 7) return -1;
	if (data[0] != 2 && data[0] != 4) return 0;
	message = le16(data + 3);
	if (message != 0x24 && message != 0x4f && message != 0x51) return 0;
	if ((message == 0x4f && data[0] != 2) || (message == 0x51 && data[0] != 4)) return 0;
	payload_size = le16(data + 5);
	if ((size_t)payload_size != length - 7) return -1;
	next = *state;
	for (at = 7; at < length;) {
		uint8_t type;
		uint16_t size;
		const uint8_t *value;
		if (length - at < 3) return -1;
		type = data[at];
		size = le16(data + at + 1);
		at += 3;
		if (size > length - at) return -1;
		value = data + at;
		at += size;
		if (type == 2 && data[0] == 2) {
			if (size != 4) return -1;
			result_seen = true;
			success = le16(value) == 0;
		} else if (message == 0x24 && type == 1) {
			static unsigned shown;

			if (size < 5 || size < (size_t)5 + value[4]) return -1;
			if (shown++ < 4) {
				printf("nas: serving system: registration %u (0 none, 1 registered, 2 searching, 3 denied, 4 unknown), "
				    "CS %u, PS %u, network %u, %u radios%s%u\n", value[0], value[1], value[2], value[3], value[4],
				    value[4] ? ", the first " : "", value[4] ? value[5] : 0);
			}
			/*
			 * The modem answers the serving system and the signal strength in separate messages, and
			 * on this phone the two alternate ("registered, no reading" / "not registered, -86 dBm",
			 * cycles 39 to 41). The last reading is kept across a "not registered" sample, so that
			 * the bars come back with the registration instead of waiting for the next reading:
			 * bars only while registered, from the last reading there is.
			 */
			next.registered = value[0] == 1;
			next.bars = next.registered && next.has_signal ? bars_for(next.dbm) : 0;
			serving = true;
		} else if (message == 0x24 && type == 0x12 && size >= 5 && size >= (size_t)5 + value[4]) {
			/* the current PLMN (TLV 0x12; 0x10 is the roaming
			 * indicator, one byte): MCC, MNC (16 bits each), the
			 * description's length and bytes */
			unsigned n = value[4] < sizeof(next.operator_name) - 1 ? value[4] : sizeof(next.operator_name) - 1;

			next.mcc = le16(value);
			next.mnc = le16(value + 2);
			if (n >= 2 && (n % 2) == 0 && nas_utf16_plausible(value + 5, n)) {
				unsigned at = 0, i;

				for (i = 0; i + 1 < n; i += 2) {
					uint16_t u = (uint16_t)(value[5 + i] | value[6 + i] << 8);

					if (u == 0) break;
					at = nas_utf8(next.operator_name, sizeof(next.operator_name), at, u);
				}
				next.operator_name[at < sizeof(next.operator_name) ? at : sizeof(next.operator_name) - 1] = 0;
			} else {
				memcpy(next.operator_name, value + 5, n);
				next.operator_name[n] = 0;
			}
		} else if (message == 0x4f || message == 0x51) {
			/*
			 * Each TLV is one radio's reading, and the modem fills
			 * the other radios' TLVs with zeros: run 051024's LTE
			 * answer carried TLV 0x14 (RSRP -102 dBm) beside
			 * TLV 0x17 = 00 00 00 00.  The old "0x17 seen, radio
			 * 12" took the zero-valued NR TLV as the reading and
			 * its radio!=12 guard then hid the LTE one, so a real
			 * -102 dBm was dropped.  A value outside the dBm range
			 * is no reading at all.  Priority: NR, LTE, UMTS, GSM.
			 */
			int16_t candidate = 0;
			uint8_t which = 0;

			if (type == 0x17 && size >= 4) {
				candidate = (int16_t)le16(value); which = 12;
			} else if (type == 0x14 && size >= 6) {
				candidate = (int16_t)le16(value + 2); which = 8;
			} else if (type == 0x13 && size >= 3) {
				candidate = (int8_t)value[0]; which = 5;
			} else if (type == 0x12 && size >= 1) {
				candidate = (int8_t)value[0]; which = 4;
			}
			if (which != 0 && candidate >= -150 && candidate <= -30 &&
			    (radio == 0 || which > radio)) {
				level = candidate; radio = which; signal = true;
			}
		}
	}
	if (data[0] == 2 && (!result_seen || !success)) return 0;
	if (signal && level >= -150 && level <= -30) {
		next.has_signal = true;
		next.dbm = level;
		next.radio_interface = radio;
		next.bars = next.registered ? bars_for(level) : 0;
	}
	if (!serving && !signal) return 0;
	if (next.registered == state->registered && next.has_signal == state->has_signal && next.mcc == state->mcc && next.mnc == state->mnc &&
	    strcmp(next.operator_name, state->operator_name) == 0 &&
	    next.radio_interface == state->radio_interface && next.dbm == state->dbm && next.bars == state->bars) return 0;
	*state = next;
	return 1;
}

static void
nas_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct hakios_nas_client *client = context;

	if (!client->connected || node != client->peer_node || port != client->peer_port) return;
	client->answers++;
	if (hakios_nas_apply(&client->state, data, length) == 1 && client->changed != NULL)
		client->changed(client->context, &client->state);
}

static void
nas_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port)
{
	struct hakios_nas_client *client = context;
	(void)version;
	(void)instance;
	if (service != 3) return; /* QMI NAS */
	if (arrived && !client->connected) {
		client->connected = true;
		client->peer_node = node;
		client->peer_port = port;
		if (client->port != NULL) (void)hakios_nas_client_poll(client);
	} else if (!arrived && client->connected && node == client->peer_node && port == client->peer_port) {
		client->connected = false;
		client->state = (struct hakios_nas_state){0};
		if (client->changed != NULL) client->changed(client->context, &client->state);
	}
}

int
hakios_nas_client_open(struct hakios_nas_client *client, const struct hakios_qrtr_ops *transport,
    void (*changed)(void *, const struct hakios_nas_state *), void *context)
{
	int error;
	if (client == NULL || transport == NULL || transport->open == NULL || transport->lookup == NULL ||
	    transport->send == NULL || transport->close == NULL) return -1;
	*client = (struct hakios_nas_client){0};
	client->transport = transport;
	client->changed = changed;
	client->context = context;
	client->port = transport->open(nas_receive, nas_server, client);
	if (client->port == NULL) return -1;
	error = transport->lookup(client->port, 3, 0, 0);
	if (error != 0) {
		hakios_nas_client_close(client);
		return error;
	}
	return 0;
}

int
hakios_nas_client_poll(struct hakios_nas_client *client)
{
	uint8_t request[7] = {0};
	const uint16_t messages[2] = {0x24, 0x4f};
	int error;
	if (client == NULL || client->port == NULL || !client->connected) return -1;
	for (unsigned i = 0; i < 2; i++) {
		client->transaction++;
		if (client->transaction == 0) client->transaction++;
		request[1] = (uint8_t)client->transaction;
		request[2] = (uint8_t)(client->transaction >> 8);
		request[3] = (uint8_t)messages[i];
		request[4] = (uint8_t)(messages[i] >> 8);
		error = client->transport->send(client->port, client->peer_node, client->peer_port, request, sizeof(request));
		if (error != 0) return error;
	}
	return 0;
}

void
hakios_nas_client_close(struct hakios_nas_client *client)
{
	if (client != NULL && client->port != NULL) {
		client->transport->close(client->port);
		client->port = NULL;
		client->connected = false;
	}
}
