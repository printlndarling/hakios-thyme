/*
 * loc_test: what CoreLocation itself says -- whether location services are enabled and what
 * this process authorization is -- and the switch that decides the first answer.
 *
 *   loc_test           one line per question, then exit
 *   loc_test enable    write LocationServicesEnabled = true into com.apple.locationd in every
 *                      preferences domain (current user / any user, any host / current host),
 *                      synchronise, read each back and say what took
 *
 * On a stock phone Setup Assistant sets the switch; this port /var/mobile starts empty every
 * boot, so CoreLocation answers 0 (off) and the Settings privacy page leaves Location Services
 * out. Which domain the framework and locationd read is what the read-back lines tell.
 */
#import <CoreLocation/CoreLocation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static void
write_read(CFStringRef user, CFStringRef host, const char *name)
{
	Boolean synced;

	CFPreferencesSetValue(CFSTR("LocationServicesEnabled"), kCFBooleanTrue,
	    CFSTR("com.apple.locationd"), user, host);
	synced = CFPreferencesAppSynchronize(CFSTR("com.apple.locationd"));
	CFPropertyListRef got = CFPreferencesCopyValue(CFSTR("LocationServicesEnabled"),
	    CFSTR("com.apple.locationd"), user, host);
	printf("loc_test: %s: write %s, read back %s\n", name, synced ? "ok" : "FAILED",
	    got == NULL ? "nothing" : (CFGetTypeID(got) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)got)) ? "true" : "not true");
	if (got != NULL) {
		CFRelease(got);
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc > 1 && strcmp(argv[1], "enable") == 0) {
			write_read(kCFPreferencesCurrentUser, kCFPreferencesAnyHost, "current user, any host");
			write_read(kCFPreferencesAnyUser, kCFPreferencesAnyHost, "any user, any host");
			write_read(kCFPreferencesCurrentUser, kCFPreferencesCurrentHost, "current user, this host");
			write_read(kCFPreferencesAnyUser, kCFPreferencesCurrentHost, "any user, this host");
			return 0;
		}
		printf("loc_test: locationServicesEnabled = %d\n", (int)[CLLocationManager locationServicesEnabled]);
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 140000
		printf("loc_test: authorizationStatus = %ld\n", (long)[CLLocationManager authorizationStatus]);
#endif
		CLLocationManager *manager = [[CLLocationManager alloc] init];

		printf("loc_test: manager made: %s\n", manager != nil ? "yes" : "no");
	}
	return 0;
}
