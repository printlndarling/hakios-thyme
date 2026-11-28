/* Lockdown development endpoint on the private thyme USB profile. */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "hakios.thyme_usbmux.h"
#include "hdb_network.h"
#include "lockdown-tls.h"
#include "afc-ram.h"

static SecKeyRef
create_device_key(void)
{
	const int bits = 2048;
	CFNumberRef size = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits);
	if (!size) {
		return NULL;
	}
	const void *keys[] = { kSecAttrKeyType, kSecAttrKeySizeInBits, kSecAttrIsPermanent };
	const void *values[] = { kSecAttrKeyTypeRSA, size, kCFBooleanFalse };
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(size);
	if (!attrs) {
		return NULL;
	}
	SecKeyRef key = SecKeyCreateRandomKey(attrs, NULL);
	CFRelease(attrs);
	return key;
}

static CFDataRef
export_public_key(SecKeyRef key)
{
	SecKeyRef public = key ? SecKeyCopyPublicKey(key) : NULL;
	if (!public) {
		return NULL;
	}
	CFDataRef der = SecKeyCopyExternalRepresentation(public, NULL);
	CFRelease(public);
	if (!der) {
		return NULL;
	}
	char encoded[512];
	int length = b64_ntop(CFDataGetBytePtr(der), (size_t)CFDataGetLength(der), encoded, sizeof(encoded));
	CFRelease(der);
	if (length <= 0) {
		return NULL;
	}
	CFMutableDataRef pem = CFDataCreateMutable(kCFAllocatorDefault, 0);
	if (!pem) {
		return NULL;
	}
	const UInt8 begin[] = "-----BEGIN RSA PUBLIC KEY-----\n";
	const UInt8 end[] = "-----END RSA PUBLIC KEY-----\n";
	CFDataAppendBytes(pem, begin, sizeof(begin) - 1);
	for (int at = 0; at < length; at += 64) {
		int count = length - at < 64 ? length - at : 64;
		CFDataAppendBytes(pem, (const UInt8 *)encoded + at, count);
		CFDataAppendBytes(pem, (const UInt8 *)"\n", 1);
	}
	CFDataAppendBytes(pem, end, sizeof(end) - 1);
	return pem;
}

/* Compatibility fields describe the live system image. The HakiOS and
 * Xiaomi identity is provided independently of Apple's image metadata. */
static void
read_system_version(CFMutableDictionaryRef info)
{
	FILE *file = fopen("/System/Library/CoreServices/SystemVersion.plist", "rb");
	if (!file) return;
	uint8_t buffer[4096];
	size_t length = fread(buffer, 1, sizeof(buffer), file);
	bool complete = length > 0 && feof(file) && !ferror(file);
	fclose(file);
	if (!complete) return;
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, buffer, (CFIndex)length);
	if (!data) return;
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		CFTypeRef version = CFDictionaryGetValue(plist, CFSTR("ProductVersion"));
		CFTypeRef build = CFDictionaryGetValue(plist, CFSTR("ProductBuildVersion"));
		if (version && CFGetTypeID(version) == CFStringGetTypeID() && CFStringGetLength(version) > 0) {
			CFDictionarySetValue(info, CFSTR("ProductVersion"), version);
		}
		if (build && CFGetTypeID(build) == CFStringGetTypeID() && CFStringGetLength(build) > 0) {
			CFDictionarySetValue(info, CFSTR("BuildVersion"), build);
		}
	}
	if (plist) CFRelease(plist);
}

static CFDictionaryRef
identity(void)
{
	CFMutableDictionaryRef info = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!info) return NULL;
	CFDictionarySetValue(info, CFSTR("DeviceName"), CFSTR("Xiaomi Mi 10S"));
	CFDictionarySetValue(info, CFSTR("HardwareModel"), CFSTR("thyme"));
	CFDictionarySetValue(info, CFSTR("HardwarePlatform"), CFSTR("SM8250-AC"));
	CFDictionarySetValue(info, CFSTR("ProductName"), CFSTR("HakiOS"));
	read_system_version(info);
	return info;
}

/* The address is a discovery hint. Authorization belongs to OpenSSH. */
static CFStringRef
debug_address(void)
{
	struct in_addr peer;
	char peer_text[INET_ADDRSTRLEN + 2], local[INET_ADDRSTRLEN];
	if (!hdb_read_peer(&peer, peer_text, sizeof(peer_text)) ||
	    access(HDB_DIR "/authorized_keys", R_OK) != 0 ||
	    access(HDB_DIR "/host_key", R_OK) != 0 ||
	    !hdb_wifi_address(peer, local, sizeof(local))) {
		return NULL;
	}
	return CFStringCreateWithCString(kCFAllocatorDefault, local, kCFStringEncodingASCII);
}

/* The current USB carrier allows one socket at a time. Keep the grant
 * across Lockdown teardown until the new service proves the same identity. */
struct service_offer {
	CFArrayRef record;
	bool root_client;
	int port;
	time_t deadline;
};

static void
clear_offer(struct service_offer *offer)
{
	if (offer->record) CFRelease(offer->record);
	*offer = (struct service_offer){0};
}

static bool
open_service(struct service_offer *offer, struct lockdown_pairing *pairing,
    struct lockdown_tls *tls, SecKeyRef key, struct afc_ram *afc, int port)
{
	lockdown_tls_clear(tls);
	afc_ram_close(afc);
	struct timespec now;
	struct lockdown_certificates certs;
	bool allowed = offer->record && offer->port == port && clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
	    now.tv_sec < offer->deadline && lockdown_pair_authorized(pairing, NULL, &certs) &&
	    certs.root_client == offer->root_client && CFEqual(offer->record, pairing->accepted);
	bool ready = allowed && lockdown_tls_start(tls, &certs, key, port == THYME_AFC_PORT ? AFC_REQUEST_MAX : THYME_LOCKDOWN_MESSAGE_MAX);
	clear_offer(offer);
	return ready;
}

static bool
dispatch_diagnostics(CFDictionaryRef request, CFMutableDictionaryRef reply, CFDictionaryRef info)
{
	CFTypeRef verb = request ? CFDictionaryGetValue(request, CFSTR("Request")) : NULL;
	if (verb && CFEqual(verb, CFSTR("Goodbye"))) {
		CFDictionarySetValue(reply, CFSTR("Status"), CFSTR("Success"));
		return true;
	}
	if (!verb || !CFEqual(verb, CFSTR("All"))) {
		CFDictionarySetValue(reply, CFSTR("Status"), CFSTR("UnknownRequest"));
		return false;
	}
	CFMutableDictionaryRef usb = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!usb) return false;
	const struct { CFStringRef field; const char *sysctl; } counters[] = {
		{ CFSTR("Stage"), "kern.hakios.thyme_usb.stage" },
		{ CFSTR("Configuration"), "kern.hakios.thyme_usb.configuration" },
		{ CFSTR("BusResets"), "kern.hakios.thyme_usb.reset" },
		{ CFSTR("Reconnects"), "kern.hakios.thyme_usb.reconnects" },
		{ CFSTR("InvalidMuxFrames"), "kern.hakios.thyme_usb.mux_invalid" },
	};
	for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
		uint32_t value;
		size_t size = sizeof(value);
		if (!sysctlbyname(counters[i].sysctl, &value, &size, NULL, 0) && size == sizeof(value)) {
			int64_t count = value;
			CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &count);
			if (number) {
				CFDictionarySetValue(usb, counters[i].field, number);
				CFRelease(number);
			}
		}
	}
	const void *keys[] = { CFSTR("HakiOS"), CFSTR("USB") };
	const void *values[] = { info, usb };
	CFDictionaryRef snapshot = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(usb);
	if (snapshot) {
		CFDictionarySetValue(reply, CFSTR("Status"), CFSTR("Success"));
		CFDictionarySetValue(reply, CFSTR("Diagnostics"), snapshot);
		CFRelease(snapshot);
	}
	return false;
}

static void
dispatch(CFDictionaryRef request, CFMutableDictionaryRef reply, CFDictionaryRef info, CFDataRef public_key,
    struct lockdown_pairing *pairing, SecKeyRef key, struct lockdown_tls *tls, bool encrypted,
    struct service_offer *offer)
{
	CFTypeRef name = request ? CFDictionaryGetValue(request, CFSTR("Request")) : NULL;
	if (!name || CFGetTypeID(name) != CFStringGetTypeID()) {
		CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidRequest"));
		return;
	}
	if (CFEqual(name, CFSTR("QueryType"))) {
		CFDictionarySetValue(reply, CFSTR("Request"), CFSTR("QueryType"));
		CFDictionarySetValue(reply, CFSTR("Type"), CFSTR("com.apple.mobile.lockdown"));
	} else if (CFEqual(name, CFSTR("GetValue"))) {
		CFTypeRef key = CFDictionaryGetValue(request, CFSTR("Key"));
		CFTypeRef domain = CFDictionaryGetValue(request, CFSTR("Domain"));
		CFTypeRef value = NULL;
		CFStringRef address = NULL;
		CFDictionarySetValue(reply, CFSTR("Request"), CFSTR("GetValue"));
		if (!domain) {
			if (!key) {
				value = info;
			} else if (CFEqual(key, CFSTR("DevicePublicKey"))) {
				value = public_key;
			} else if (CFGetTypeID(key) == CFStringGetTypeID()) {
				value = CFDictionaryGetValue(info, key);
			}
		} else if (key && CFEqual(domain, CFSTR("com.hakios.debug")) &&
		    CFEqual(key, CFSTR("SSHAddress"))) {
			address = debug_address();
			value = address;
		}
		if (value) {
			CFDictionarySetValue(reply, CFSTR("Value"), value);
		} else {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("MissingValue"));
		}
		if (address) {
			CFRelease(address);
		}
	} else if (!encrypted && CFEqual(name, CFSTR("StartSession"))) {
		CFDictionarySetValue(reply, CFSTR("Request"), name);
		struct lockdown_certificates certs;
		if (!lockdown_pair_authorized(pairing, request, &certs)) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidHostID"));
		} else if (!lockdown_tls_start(tls, &certs, key, THYME_LOCKDOWN_MESSAGE_MAX)) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("SessionFailed"));
		} else {
			CFDictionarySetValue(reply, CFSTR("SessionID"), tls->id);
			CFDictionarySetValue(reply, CFSTR("EnableSessionSSL"), kCFBooleanTrue);
		}
	} else if (CFEqual(name, CFSTR("StartService"))) {
		CFDictionarySetValue(reply, CFSTR("Request"), name);
		CFTypeRef service = CFDictionaryGetValue(request, CFSTR("Service"));
		const int port = service && CFEqual(service, CFSTR("com.apple.mobile.diagnostics_relay")) ? THYME_DIAGNOSTICS_PORT :
		    service && CFEqual(service, CFSTR("com.apple.afc")) ? THYME_AFC_PORT : 0;
		if (!encrypted) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("SessionInactive"));
		} else if (!port) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidService"));
		} else {
			CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &port);
			struct timespec now;
			if (number && clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
				clear_offer(offer);
				offer->record = CFRetain(pairing->accepted);
				offer->root_client = tls->root_client;
				offer->port = port;
				offer->deadline = now.tv_sec + 15;
				CFDictionarySetValue(reply, CFSTR("Port"), number);
				CFDictionarySetValue(reply, CFSTR("EnableServiceSSL"), kCFBooleanTrue);
			} else {
				CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidService"));
			}
			if (number) CFRelease(number);
		}
	} else if (encrypted && CFEqual(name, CFSTR("StopSession"))) {
		CFDictionarySetValue(reply, CFSTR("Request"), name);
		CFTypeRef id = CFDictionaryGetValue(request, CFSTR("SessionID"));
		if (!id || !CFEqual(id, tls->id)) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidSessionID"));
		}
	} else if (!encrypted && (CFEqual(name, CFSTR("Pair")) || CFEqual(name, CFSTR("ValidatePair")))) {
		CFDictionarySetValue(reply, CFSTR("Request"), name);
		CFStringRef error = lockdown_pair_request(pairing, request, key, CFEqual(name, CFSTR("ValidatePair")));
		if (error) {
			CFDictionarySetValue(reply, CFSTR("Error"), error);
		} else {
			CFDictionarySetValue(reply, CFSTR("Result"), CFSTR("Success"));
		}
	} else if (CFEqual(name, CFSTR("Goodbye"))) {
		CFDictionarySetValue(reply, CFSTR("Request"), CFSTR("Goodbye"));
	} else {
		/* Echo a bounded verb so libimobiledevice can classify the error. */
		if (CFStringGetLength(name) <= 32) {
			CFDictionarySetValue(reply, CFSTR("Request"), name);
		}
		CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("InvalidRequest"));
	}
}

static bool
parse_size(const uint8_t *frame, size_t *size)
{
	uint32_t wire;
	memcpy(&wire, frame, sizeof(wire));
	*size = ntohl(wire);
	return *size > 0 && *size <= THYME_LOCKDOWN_MESSAGE_MAX - sizeof(wire);
}

/* Input and output can alias after CF has copied the incoming plist. */
static size_t
frame_reply(const uint8_t *payload, size_t length, uint8_t *output, size_t capacity,
    CFDictionaryRef info, CFDataRef public_key, struct lockdown_pairing *pairing,
    SecKeyRef key, struct lockdown_tls *tls, bool encrypted, bool diagnostics,
    struct service_offer *offer, bool *stop, bool *offered)
{
	CFDataRef bytes = CFDataCreate(kCFAllocatorDefault, payload, (CFIndex)length);
	if (!bytes) return 0;
	CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, bytes, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(bytes);
	CFMutableDictionaryRef reply = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!reply) {
		if (list) CFRelease(list);
		return 0;
	}
	CFDictionaryRef request = list && CFGetTypeID(list) == CFDictionaryGetTypeID() ? list : NULL;
	*offered = false;
	if (diagnostics) {
		*stop = dispatch_diagnostics(request, reply, info);
	} else {
		dispatch(request, reply, info, public_key, pairing, key, tls, encrypted, offer);
		CFTypeRef verb = CFDictionaryGetValue(reply, CFSTR("Request"));
		*stop = encrypted && verb && CFEqual(verb, CFSTR("StopSession")) &&
		    !CFDictionaryContainsKey(reply, CFSTR("Error"));
		*offered = encrypted && CFDictionaryContainsKey(reply, CFSTR("Port"));
	}
	if (list) CFRelease(list);
	CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, reply, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	CFRelease(reply);
	size_t written = 0;
	if (data) {
		size_t encoded = (size_t)CFDataGetLength(data);
		if (capacity >= sizeof(uint32_t) && encoded <= capacity - sizeof(uint32_t)) {
			uint32_t wire = htonl((uint32_t)encoded);
			memcpy(output, &wire, sizeof(wire));
			memcpy(output + sizeof(wire), CFDataGetBytePtr(data), encoded);
			written = sizeof(wire) + encoded;
		}
		CFRelease(data);
	}
	return written;
}

static int
process_tls(struct lockdown_tls *tls, struct thyme_lockdown_message *message, size_t length,
    CFDictionaryRef info, CFDataRef public_key, struct lockdown_pairing *pairing, SecKeyRef key,
    struct service_offer *offer, struct afc_ram *afc)
{
	bool diagnostics = (message->transaction & THYME_LOCKDOWN_DIAGNOSTICS) != 0;
	bool file_service = (message->transaction & THYME_LOCKDOWN_AFC) != 0;
	int port = file_service ? THYME_AFC_PORT : diagnostics ? THYME_DIAGNOSTICS_PORT : THYME_LOCKDOWN_PORT;
	if ((message->transaction & THYME_LOCKDOWN_NEW_SERVICE) && !open_service(offer, pairing, tls, key, afc, port)) {
		message->transaction |= THYME_LOCKDOWN_ABORT;
		return 0;
	}
	bool publish_offer = false;
	bool keep = lockdown_tls_receive(tls, message->data, length);
	if (keep && SSL_is_init_finished(tls->connection)) {
		struct lockdown_certificates certs;
		keep = lockdown_pair_authorized(pairing, NULL, &certs) && certs.root_client == tls->root_client;
		if (keep && file_service && afc->root < 0) keep = afc_ram_open(afc);
		if (!keep) SSL_shutdown(tls->connection);
	}
	size_t header = file_service ? AFC_HEADER_SIZE : sizeof(uint32_t);
	while (keep && tls->plaintext_length >= header) {
		size_t frame_size = 0;
		if (file_service) {
			frame_size = afc_request_size(tls->plaintext);
		} else {
			size_t size;
			if (parse_size(tls->plaintext, &size)) frame_size = header + size;
		}
		if (!frame_size) {
			SSL_shutdown(tls->connection);
			keep = false;
			break;
		}
		if (tls->plaintext_length < frame_size) break;
		bool stop = false, offered = false;
		size_t reply;
		if (file_service) {
			reply = afc_ram_reply(afc, tls->plaintext, frame_size, message->data, sizeof(message->data));
		} else {
			reply = frame_reply(tls->plaintext + header, frame_size - header, message->data,
			    sizeof(message->data), info, public_key, pairing, key, tls, true, diagnostics, offer, &stop, &offered);
		}
		size_t written = 0;
		if (!reply || !SSL_write_ex(tls->connection, message->data, reply, &written) || written != reply) {
			keep = false;
			break;
		}
		publish_offer |= offered;
		tls->plaintext_length -= frame_size;
		memmove(tls->plaintext, tls->plaintext + frame_size, tls->plaintext_length);
		OPENSSL_cleanse(tls->plaintext + tls->plaintext_length, frame_size);
		if (stop) {
			SSL_shutdown(tls->connection);
			keep = false;
		}
	}
	int output = lockdown_tls_output(tls, message->data, sizeof(message->data));
	if (!keep || output < 0) {
		lockdown_tls_clear(tls);
		afc_ram_close(afc);
	}
	if (publish_offer && output > 0) {
		message->transaction |= offer->port == THYME_AFC_PORT ?
		    THYME_LOCKDOWN_OFFER_AFC : THYME_LOCKDOWN_OFFER_DIAGNOSTICS;
	}
	return output < 0 ? 0 : output;
}

int
main(void)
{
	int fd = open(THYME_LOCKDOWN_NODE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(THYME_LOCKDOWN_NODE);
		return 1;
	}
	CFDictionaryRef info = identity();
	if (!info) {
		close(fd);
		return 1;
	}
	/* The key stays in this process and is regenerated on each launch. */
	SecKeyRef key = create_device_key();
	CFDataRef public_key = export_public_key(key);
	if (!public_key) {
		fputs("Lockdown device public key unavailable\n", stderr);
	}
	struct lockdown_pairing pairing = {0};
	struct lockdown_tls tls = {0};
	struct service_offer offer = {0};
	struct afc_ram afc = {.root = -1};
	struct thyme_lockdown_message message;
	const size_t header = sizeof(message.transaction);
	int status = 0;
	for (;;) {
		ssize_t n = read(fd, &message, sizeof(message));
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && errno == ENXIO) {
			break;
		}
		if (n <= (ssize_t)header) {
			fputs("cannot read a framed Lockdown request\n", stderr);
			status = 1;
			break;
		}
		size_t length = (size_t)n - header;
		if (message.transaction & THYME_LOCKDOWN_TLS_RECORD) {
			length = (size_t)process_tls(&tls, &message, length, info, public_key, &pairing, key, &offer, &afc);
		} else {
			lockdown_tls_clear(&tls);
			afc_ram_close(&afc);
			size_t size;
			if (length <= sizeof(uint32_t) || !parse_size(message.data, &size) || length != sizeof(uint32_t) + size) {
				fputs("invalid Lockdown frame length\n", stderr);
				status = 1;
				break;
			}
			bool stop, offered;
			length = frame_reply(message.data + sizeof(uint32_t), size, message.data, sizeof(message.data),
			    info, public_key, &pairing, key, &tls, false, false, &offer, &stop, &offered);
			if (!length) {
				fputs("cannot encode a bounded Lockdown reply\n", stderr);
				status = 1;
				break;
			}
			if (tls.connection) message.transaction |= THYME_LOCKDOWN_START_TLS;
		}
		do {
			n = write(fd, &message, header + length);
		} while (n < 0 && errno == EINTR);
		if (n < 0 && errno == ESTALE) {
			lockdown_tls_clear(&tls);
			afc_ram_close(&afc);
			continue; /* USB disconnected during the user-space request. */
		}
		if (n != (ssize_t)(header + length)) {
			perror("cannot send Lockdown reply");
			status = 1;
			break;
		}
	}
	afc_ram_close(&afc);
	clear_offer(&offer);
	lockdown_tls_clear(&tls);
	lockdown_pair_clear(&pairing);
	if (public_key) {
		CFRelease(public_key);
	}
	if (key) {
		CFRelease(key);
	}
	CFRelease(info);
	close(fd);
	return status;
}
