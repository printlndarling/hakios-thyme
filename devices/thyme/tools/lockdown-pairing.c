/* Volatile Lockdown approval for the explicitly enabled USB development profile. */
#include "lockdown-pairing.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

#define APPROVAL_PATH "/private/var/tmp/hakios-lockdown-approval.plist"
#define CERTIFICATE_MAX 8192
enum { PAIR_HOST_ID, PAIR_BUID, PAIR_ROOT, PAIR_HOST, PAIR_DEVICE, PAIR_PARTS };

static CFDictionaryRef
read_approval(void)
{
	int fd = open(APPROVAL_PATH, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	struct statfs fs;
	uint8_t bytes[4096];
	size_t length = 0;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
	    (st.st_mode & 077) == 0 && st.st_size > 0 && st.st_size <= (off_t)sizeof(bytes) &&
	    fstatfs(fd, &fs) == 0 && strcmp(fs.f_fstypename, "ramfs") == 0) {
		while (length < (size_t)st.st_size) {
			ssize_t n = read(fd, bytes + length, (size_t)st.st_size - length);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				length = 0;
				break;
			}
			length += (size_t)n;
		}
	}
	close(fd);
	if (!length) {
		return NULL;
	}
	CFDataRef contents = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)length);
	if (!contents) {
		return NULL;
	}
	CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, contents,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(contents);
	if (list && CFGetTypeID(list) != CFDictionaryGetTypeID()) {
		CFRelease(list);
		return NULL;
	}
	return list;
}

static bool
uuid_matches(CFTypeRef proposed, CFTypeRef approved)
{
	char text[37], pin[37];
	uuid_t requested_uuid, approved_uuid;
	return proposed && CFGetTypeID(proposed) == CFStringGetTypeID() &&
	    CFStringGetLength(proposed) == 36 && approved && CFGetTypeID(approved) == CFStringGetTypeID() &&
	    CFStringGetLength(approved) == 36 &&
	    CFStringGetCString(proposed, text, sizeof(text), kCFStringEncodingASCII) &&
	    CFStringGetCString(approved, pin, sizeof(pin), kCFStringEncodingASCII) &&
	    uuid_parse(text, requested_uuid) == 0 && uuid_parse(pin, approved_uuid) == 0 &&
	    uuid_compare(requested_uuid, approved_uuid) == 0 && !uuid_is_null(requested_uuid);
}

static SecCertificateRef
parse_certificate(CFTypeRef value)
{
	if (!value || CFGetTypeID(value) != CFDataGetTypeID()) {
		return NULL;
	}
	CFIndex length = CFDataGetLength(value);
	if (length <= 0 || length > CERTIFICATE_MAX) {
		return NULL;
	}
	const uint8_t *bytes = CFDataGetBytePtr(value);
	if (bytes[0] == 0x30) {
		return SecCertificateCreateWithData(kCFAllocatorDefault, value);
	}
	const char begin[] = "-----BEGIN CERTIFICATE-----";
	const char end[] = "-----END CERTIFICATE-----";
	const size_t prefix = sizeof(begin) - 1;
	if ((size_t)length <= prefix || memcmp(bytes, begin, prefix) != 0 ||
	    (bytes[prefix] != '\r' && bytes[prefix] != '\n') || memchr(bytes, '\0', (size_t)length)) {
		return NULL;
	}
	char pem[CERTIFICATE_MAX + 1];
	memcpy(pem, bytes, (size_t)length);
	pem[length] = '\0';
	char *footer = strstr(pem + prefix, end);
	if (!footer) {
		return NULL;
	}
	const char *tail = footer + sizeof(end) - 1;
	if (tail[strspn(tail, " \r\n\t")] != '\0') {
		return NULL;
	}
	*footer = '\0';
	uint8_t der[CERTIFICATE_MAX * 3 / 4];
	int decoded = b64_pton(pem + prefix, der, sizeof(der));
	if (decoded <= 0) {
		return NULL;
	}
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, der, decoded);
	SecCertificateRef certificate = data ? SecCertificateCreateWithData(kCFAllocatorDefault, data) : NULL;
	if (data) {
		CFRelease(data);
	}
	return certificate;
}

static bool
pin_matches(CFDataRef der, CFTypeRef approved)
{
	if (!der || !approved || CFGetTypeID(approved) != CFDataGetTypeID() ||
	    CFDataGetLength(approved) != CC_SHA256_DIGEST_LENGTH) {
		return false;
	}
	uint8_t digest[CC_SHA256_DIGEST_LENGTH];
	return CC_SHA256(CFDataGetBytePtr(der), (CC_LONG)CFDataGetLength(der), digest) &&
	    memcmp(digest, CFDataGetBytePtr(approved), sizeof(digest)) == 0;
}

static bool
matches_device_key(SecCertificateRef certificate, SecKeyRef key)
{
	SecKeyRef proposed = SecCertificateCopyKey(certificate);
	SecKeyRef actual = SecKeyCopyPublicKey(key);
	CFDataRef a = proposed ? SecKeyCopyExternalRepresentation(proposed, NULL) : NULL;
	CFDataRef b = actual ? SecKeyCopyExternalRepresentation(actual, NULL) : NULL;
	bool matches = a && b && CFEqual(a, b);
	if (a) CFRelease(a);
	if (b) CFRelease(b);
	if (proposed) CFRelease(proposed);
	if (actual) CFRelease(actual);
	return matches;
}

static bool
chain_valid(SecCertificateRef leaf, SecCertificateRef root)
{
	const void *anchors[] = { root };
	CFArrayRef pin = CFArrayCreate(kCFAllocatorDefault, anchors, 1, &kCFTypeArrayCallBacks);
	SecPolicyRef policy = SecPolicyCreateBasicX509();
	SecTrustRef trust = NULL;
	CFErrorRef failure = NULL;
	bool verified = pin && policy &&
	    SecTrustCreateWithCertificates(leaf, policy, &trust) == errSecSuccess &&
	    SecTrustSetAnchorCertificates(trust, pin) == errSecSuccess &&
	    SecTrustSetAnchorCertificatesOnly(trust, true) == errSecSuccess &&
	    SecTrustSetNetworkFetchAllowed(trust, false) == errSecSuccess &&
	    SecTrustEvaluateWithError(trust, &failure);
	if (failure) {
		fprintf(stderr, "Lockdown certificate validation failed (%ld)\n", (long)CFErrorGetCode(failure));
		CFRelease(failure);
	}
	if (trust) CFRelease(trust);
	if (policy) CFRelease(policy);
	if (pin) CFRelease(pin);
	return verified;
}

void
lockdown_pair_clear(struct lockdown_pairing *state)
{
	if (state->accepted) {
		CFRelease(state->accepted);
		state->accepted = NULL;
	}
}

bool
lockdown_pair_authorized(struct lockdown_pairing *state, CFDictionaryRef request, struct lockdown_certificates *certs)
{
	if (!state->accepted) {
		return false;
	}
	CFDictionaryRef approved = read_approval();
	CFArrayRef record = state->accepted;
	CFTypeRef host_id = CFArrayGetValueAtIndex(record, PAIR_HOST_ID);
	CFTypeRef buid = CFArrayGetValueAtIndex(record, PAIR_BUID);
	CFDataRef root = CFArrayGetValueAtIndex(record, PAIR_ROOT);
	CFDataRef host = CFArrayGetValueAtIndex(record, PAIR_HOST);
	CFTypeRef role = approved ? CFDictionaryGetValue(approved, CFSTR("TLSClientCertificate")) : NULL;
	bool root_client = role && CFEqual(role, CFSTR("RootCertificate"));
	bool permitted = approved && (!role || root_client || CFEqual(role, CFSTR("HostCertificate"))) &&
	    uuid_matches(host_id, CFDictionaryGetValue(approved, CFSTR("HostID"))) &&
	    uuid_matches(buid, CFDictionaryGetValue(approved, CFSTR("SystemBUID"))) &&
	    pin_matches(root, CFDictionaryGetValue(approved, CFSTR("RootCertificateSHA256"))) &&
	    pin_matches(host, CFDictionaryGetValue(approved, CFSTR("HostCertificateSHA256")));
	if (approved) CFRelease(approved);
	if (!permitted) {
		lockdown_pair_clear(state);
		return false;
	}
	if (request && (!uuid_matches(CFDictionaryGetValue(request, CFSTR("HostID")), host_id) ||
	    !uuid_matches(CFDictionaryGetValue(request, CFSTR("SystemBUID")), buid))) {
		return false;
	}
	if (certs) {
		*certs = (struct lockdown_certificates){
			.root = root, .client = root_client ? root : host,
			.device = CFArrayGetValueAtIndex(record, PAIR_DEVICE), .root_client = root_client,
		};
	}
	return true;
}

CFStringRef
lockdown_pair_request(struct lockdown_pairing *state, CFDictionaryRef request, SecKeyRef key, bool validate)
{
	CFDictionaryRef approved = read_approval();
	if (!approved) {
		lockdown_pair_clear(state);
		return validate ? CFSTR("InvalidHostID") : CFSTR("InvalidRequest");
	}
	CFTypeRef record = CFDictionaryGetValue(request, CFSTR("PairRecord"));
	CFStringRef error = validate ? CFSTR("InvalidHostID") : CFSTR("InvalidPairRecord");
	if (!key || !record || CFGetTypeID(record) != CFDictionaryGetTypeID()) {
		CFRelease(approved);
		return error;
	}
	CFTypeRef host_id = CFDictionaryGetValue(record, CFSTR("HostID"));
	CFTypeRef buid = CFDictionaryGetValue(record, CFSTR("SystemBUID"));
	if (!uuid_matches(host_id, CFDictionaryGetValue(approved, CFSTR("HostID"))) ||
	    !uuid_matches(buid, CFDictionaryGetValue(approved, CFSTR("SystemBUID")))) {
		CFRelease(approved);
		return CFSTR("InvalidHostID");
	}
	SecCertificateRef root = parse_certificate(CFDictionaryGetValue(record, CFSTR("RootCertificate")));
	SecCertificateRef host = parse_certificate(CFDictionaryGetValue(record, CFSTR("HostCertificate")));
	SecCertificateRef device = parse_certificate(CFDictionaryGetValue(record, CFSTR("DeviceCertificate")));
	CFDataRef root_der = root ? SecCertificateCopyData(root) : NULL;
	CFDataRef host_der = host ? SecCertificateCopyData(host) : NULL;
	CFDataRef device_der = device ? SecCertificateCopyData(device) : NULL;
	if (pin_matches(root_der, CFDictionaryGetValue(approved, CFSTR("RootCertificateSHA256"))) &&
	    pin_matches(host_der, CFDictionaryGetValue(approved, CFSTR("HostCertificateSHA256"))) &&
	    device_der && !CFEqual(host_der, root_der) && !CFEqual(device_der, root_der) &&
	    !CFEqual(device_der, host_der) && matches_device_key(device, key)) {
		const void *parts[] = { host_id, buid, root_der, host_der, device_der };
		CFArrayRef normalized = CFArrayCreate(kCFAllocatorDefault, parts, PAIR_PARTS, &kCFTypeArrayCallBacks);
		if (normalized && (!validate || (state->accepted && CFEqual(state->accepted, normalized))) &&
		    chain_valid(host, root) && chain_valid(device, root)) {
			if (!validate) {
				lockdown_pair_clear(state);
				state->accepted = CFRetain(normalized);
			}
			error = NULL;
		}
		if (normalized) CFRelease(normalized);
	}
	if (device_der) CFRelease(device_der);
	if (host_der) CFRelease(host_der);
	if (root_der) CFRelease(root_der);
	if (device) CFRelease(device);
	if (host) CFRelease(host);
	if (root) CFRelease(root);
	CFRelease(approved);
	return error;
}
