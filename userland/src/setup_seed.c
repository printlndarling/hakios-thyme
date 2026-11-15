/*
 * setup_seed: the preferences Setup Assistant writes before it hands a new phone over, which
 * this port never runs -- "the device is set up" is what a Settings pane is allowed to ask.
 *
 * The Location Services row of Privacy & Security is the one row Preferences leaves out on this
 * port; the plist row and every class it names are there, the switch is on, and taking the row's
 * getter out changes nothing (CHECKLIST, 2026-11-15 night).  What a real phone has and a fresh
 * /var/mobile does not is the purplebuddy completion state (and the first-time location-consent
 * marker), so this writes them as the mobile user and says what it wrote.  It is a test, not a
 * daily boot step: whether it should be is what the run opens Settings to see.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <unistd.h>

static void
set(const char *domain, const char *key, CFPropertyListRef value)
{
	CFStringRef app = CFStringCreateWithCString(NULL, domain, kCFStringEncodingUTF8);
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	Boolean synced;

	CFPreferencesSetValue(name, value, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	synced = CFPreferencesAppSynchronize(app);
	printf("setup_seed: %s %s: %s\n", domain, key, synced ? "written" : "FAILED");
	CFRelease(name);
	CFRelease(app);
}

int
main(void)
{
	int one = 1;
	CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &one);

	if (geteuid() == 0 && (setgid(501) != 0 || setuid(501) != 0)) {
		perror("setup_seed: setgid/setuid(501)");
		return 1;
	}
	set("com.apple.purplebuddy", "SetupDone", kCFBooleanTrue);
	set("com.apple.purplebuddy", "SetupFinishedAllSteps", kCFBooleanTrue);
	set("com.apple.purplebuddy", "SetupLastExit", number);
	set("com.apple.purplebuddy", "UserHasFinishedSetup", kCFBooleanTrue);
	set("com.apple.Preferences", "PSDidShowFirstTimeLocationConsent", kCFBooleanTrue);
	CFRelease(number);
	fflush(stdout);
	return 0;
}
