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
#include "lockdown-pairing.h"

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
    struct lockdown_pairing *pairing, SecKeyRef key)
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
	} else if (CFEqual(name, CFSTR("Pair")) || CFEqual(name, CFSTR("ValidatePair"))) {
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

static CFDataRef
reply_data(const struct thyme_lockdown_message *message, size_t length, CFDictionaryRef info, CFDataRef public_key,
    struct lockdown_pairing *pairing, SecKeyRef key)
{
	CFDataRef bytes = CFDataCreate(kCFAllocatorDefault, message->data + sizeof(uint32_t), length);
	if (!bytes) {
		return NULL;
	}
	CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, bytes, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(bytes);
	CFMutableDictionaryRef reply = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!reply) {
		if (list) {
			CFRelease(list);
		}
		return NULL;
	}
	dispatch(list && CFGetTypeID(list) == CFDictionaryGetTypeID() ? list : NULL, reply, info, public_key, pairing, key);
	if (list) {
		CFRelease(list);
	}
	CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, reply, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	CFRelease(reply);
	return data;
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
	struct thyme_lockdown_message message;
	const size_t header = sizeof(message.transaction) + sizeof(uint32_t);
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
		uint32_t size;
		memcpy(&size, message.data, sizeof(size));
		if (ntohl(size) != (size_t)n - header) {
			fputs("invalid Lockdown frame length\n", stderr);
			status = 1;
			break;
		}
		CFDataRef data = reply_data(&message, (size_t)n - header, info, public_key, &pairing, key);
		if (!data || CFDataGetLength(data) > THYME_LOCKDOWN_MESSAGE_MAX - sizeof(size)) {
			if (data) {
				CFRelease(data);
			}
			fputs("cannot encode a bounded Lockdown reply\n", stderr);
			status = 1;
			break;
		}
		size_t length = (size_t)CFDataGetLength(data);
		size = htonl((uint32_t)length);
		memcpy(message.data, &size, sizeof(size));
		memcpy(message.data + sizeof(size), CFDataGetBytePtr(data), length);
		CFRelease(data);
		do {
			n = write(fd, &message, header + length);
		} while (n < 0 && errno == EINTR);
		if (n < 0 && errno == ESTALE) {
			continue; /* USB disconnected during the user-space request. */
		}
		if (n != (ssize_t)(header + length)) {
			perror("cannot send Lockdown reply");
			status = 1;
			break;
		}
	}
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
