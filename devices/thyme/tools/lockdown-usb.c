/* Lockdown development endpoint on the private thyme USB profile. */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "hakios.thyme_usbmux.h"
#include "hdb_network.h"
#include "lockdown-tls.h"

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

static CFDictionaryRef
identity(void)
{
	const void *keys[] = {
		CFSTR("DeviceName"), CFSTR("HardwareModel"), CFSTR("HardwarePlatform"), CFSTR("ProductName"),
	};
	const void *values[] = {
		CFSTR("Xiaomi Mi 10S"), CFSTR("thyme"), CFSTR("SM8250-AC"), CFSTR("HakiOS"),
	};
	return CFDictionaryCreate(kCFAllocatorDefault, keys, values, sizeof(keys) / sizeof(keys[0]),
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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

static void
dispatch(CFDictionaryRef request, CFMutableDictionaryRef reply, CFDictionaryRef info, CFDataRef public_key,
    struct lockdown_pairing *pairing, SecKeyRef key, struct lockdown_tls *tls, bool encrypted)
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
		} else if (!lockdown_tls_start(tls, &certs, key)) {
			CFDictionarySetValue(reply, CFSTR("Error"), CFSTR("SessionFailed"));
		} else {
			CFDictionarySetValue(reply, CFSTR("SessionID"), tls->id);
			CFDictionarySetValue(reply, CFSTR("EnableSessionSSL"), kCFBooleanTrue);
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
    SecKeyRef key, struct lockdown_tls *tls, bool encrypted, bool *stop)
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
	dispatch(list && CFGetTypeID(list) == CFDictionaryGetTypeID() ? list : NULL,
	    reply, info, public_key, pairing, key, tls, encrypted);
	if (list) CFRelease(list);
	CFTypeRef request = CFDictionaryGetValue(reply, CFSTR("Request"));
	*stop = encrypted && request && CFEqual(request, CFSTR("StopSession")) &&
	    !CFDictionaryContainsKey(reply, CFSTR("Error"));
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
    CFDictionaryRef info, CFDataRef public_key, struct lockdown_pairing *pairing, SecKeyRef key)
{
	bool keep = lockdown_tls_receive(tls, message->data, length);
	if (keep && SSL_is_init_finished(tls->connection)) {
		keep = lockdown_pair_authorized(pairing, NULL, NULL);
		if (!keep) SSL_shutdown(tls->connection);
	}
	while (keep && tls->plaintext_length >= sizeof(uint32_t)) {
		size_t size;
		if (!parse_size(tls->plaintext, &size)) {
			SSL_shutdown(tls->connection);
			keep = false;
			break;
		}
		size_t frame_size = sizeof(uint32_t) + size;
		if (tls->plaintext_length < frame_size) break;
		bool stop;
		size_t reply = frame_reply(tls->plaintext + sizeof(uint32_t), size, message->data,
		    sizeof(message->data), info, public_key, pairing, key, tls, true, &stop);
		size_t written = 0;
		if (!reply || !SSL_write_ex(tls->connection, message->data, reply, &written) || written != reply) {
			keep = false;
			break;
		}
		tls->plaintext_length -= frame_size;
		memmove(tls->plaintext, tls->plaintext + frame_size, tls->plaintext_length);
		OPENSSL_cleanse(tls->plaintext + tls->plaintext_length, frame_size);
		if (stop) {
			SSL_shutdown(tls->connection);
			keep = false;
		}
	}
	int output = lockdown_tls_output(tls, message->data, sizeof(message->data));
	if (!keep || output < 0) lockdown_tls_clear(tls);
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
			length = (size_t)process_tls(&tls, &message, length, info, public_key, &pairing, key);
		} else {
			lockdown_tls_clear(&tls);
			size_t size;
			if (length <= sizeof(uint32_t) || !parse_size(message.data, &size) || length != sizeof(uint32_t) + size) {
				fputs("invalid Lockdown frame length\n", stderr);
				status = 1;
				break;
			}
			bool stop;
			length = frame_reply(message.data + sizeof(uint32_t), size, message.data, sizeof(message.data),
			    info, public_key, &pairing, key, &tls, false, &stop);
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
			continue; /* USB disconnected during the user-space request. */
		}
		if (n != (ssize_t)(header + length)) {
			perror("cannot send Lockdown reply");
			status = 1;
			break;
		}
	}
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
