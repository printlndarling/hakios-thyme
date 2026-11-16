/*
 * wifipw_seed: keep the Wi-Fi network's password where wifid's Auto-Join looks
 * for it, at every boot.
 *
 * The small image carries the network's name and passphrase in fw/network
 * (neither in git, neither printed).  The kernel's Wi-Fi driver joins with
 * them, but iOS's own Wi-Fi daemon does not see them: its keychain item
 * ("AirPort", account = network name) is missing, so about a hundred seconds
 * into a boot it puts up "Enter the password for ..." -- a sheet over whatever
 * is on screen.  WiFiSecuritySetPassword() is what Settings itself calls when
 * a person joins a network; this calls it with the file's passphrase, waits for
 * the keychain to be up, and reads the password back to be sure (lengths only
 * on the console, never the password).
 *
 *   wifipw_seed [FILE]     default /redwood/fw/network: line 1 name, line 2 password
 *
 * Signed with wifipw_test.entitlements (wifid's name and the system keychain).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Boolean (*set_password)(CFStringRef ssid, CFStringRef password);
static CFStringRef (*copy_password)(CFStringRef ssid);

static CFStringRef
read_line(FILE *f)
{
	char buffer[512];
	size_t length;

	if (fgets(buffer, sizeof(buffer), f) == NULL) {
		return NULL;
	}
	length = strlen(buffer);
	while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
		buffer[--length] = '\0';
	}
	return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/redwood/fw/network";
	FILE *f;
	CFStringRef ssid, password;
	void *mw;
	int attempt;

	/* The keychain's daemon is not there ten seconds into a boot: a call then
	 * waits inside the Security framework (the job's first run printed its
	 * sizes and never came back).  Wait for the boot to settle first; the
	 * password sheet appears at about 100 s, so this is still early enough. */
	if (argc > 2) {
		int delay = atoi(argv[2]);

		if (delay > 0) {
			printf("wifipw_seed: waiting %d s for the keychain\n", delay);
			sleep((unsigned)delay);
		}
	}
	f = fopen(path, "r");
	if (f == NULL) {
		printf("wifipw_seed: no %s\n", path);
		return 1;
	}
	ssid = read_line(f);
	password = read_line(f);
	fclose(f);
	if (ssid == NULL || password == NULL) {
		printf("wifipw_seed: %s needs two lines\n", path);
		return 1;
	}
	printf("wifipw_seed: %zu-byte network name, %zu-byte password\n",
	    (size_t)CFStringGetLength(ssid), (size_t)CFStringGetLength(password));
	mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	set_password = mw ? dlsym(mw, "WiFiSecuritySetPassword") : NULL;
	copy_password = mw ? dlsym(mw, "WiFiSecurityCopyPassword") : NULL;
	if (set_password == NULL || copy_password == NULL) {
		printf("wifipw_seed: MobileWiFi not available: %s\n", dlerror());
		return 1;
	}

	/* the keychain's daemon comes up as launchd does; try for a minute */
	for (attempt = 0; attempt < 30; attempt++) {
		CFStringRef back;

		if (!set_password(ssid, password)) {
			sleep(2);
			continue;
		}
		back = copy_password(ssid);
		if (back != NULL) {
			Boolean same = CFEqual(back, password);

			printf("wifipw_seed: stored; read back %s\n", same ? "the same password" : "another password");
			CFRelease(back);
			return same ? 0 : 1;
		}
		sleep(2);
	}
	printf("wifipw_seed: the keychain did not take it in 60 s\n");
	return 1;
}
