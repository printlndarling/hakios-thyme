/*
 * loc_test: what CoreLocation itself says -- whether location services are enabled and what
 * this process authorization is -- and the private switch Settings uses to turn them on.
 *
 *   loc_test           one line per question, then exit
 *   loc_test spi       call CoreLocation private CLInternalSetLocationServicesEnabled(YES)
 *                      (the SPI the Settings privacy pane uses -- its failure string,
 *                      "CLInternalSetLocationServicesEnabled failed", is in the cache), then
 *                      ask again
 *   loc_test enable    become mobile and write LocationServicesEnabledIn8.0 = true (and the
 *                      legacy key) into com.apple.locationd, read them back; with "enablenum"
 *                      the value is the number 1 instead of a boolean
 *   loc_test watch N   become mobile, ask CoreLocation for updates for N seconds and print
 *                      what locationd delivers (or the error), then the authorization status
 *
 * locationd reads the switch from the mobile user's own preferences; a process running as
 * root ("current user" = /var/root) writes a file nobody reads.  Hence the setuid(501).
 */
#import <CoreLocation/CoreLocation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface LocWatch : NSObject <CLLocationManagerDelegate>
@end

@implementation LocWatch
- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	CLLocation *last = [locations lastObject];

	printf("loc_test: location %.6f %.6f +/- %.1f m (age %.1f s)\n",
	    last.coordinate.latitude, last.coordinate.longitude, last.horizontalAccuracy,
	    -[last.timestamp timeIntervalSinceNow]);
	fflush(stdout);
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	printf("loc_test: update failed: %s %ld\n", error.domain.UTF8String, (long)error.code);
	fflush(stdout);
}

- (void)locationManager:(CLLocationManager *)manager didChangeAuthorizationStatus:(CLAuthorizationStatus)status
{
	printf("loc_test: authorization changed to %d\n", (int)status);
	fflush(stdout);
}
@end

static void
write_read(CFStringRef user, CFStringRef host, const char *name, CFStringRef key,
    CFPropertyListRef value)
{
	Boolean synced;
	const char *type = "nothing";

	CFPreferencesSetValue(key, value, CFSTR("com.apple.locationd"), user, host);
	synced = CFPreferencesAppSynchronize(CFSTR("com.apple.locationd"));
	CFPropertyListRef got = CFPreferencesCopyValue(key, CFSTR("com.apple.locationd"), user, host);
	if (got != NULL) {
		if (CFGetTypeID(got) == CFBooleanGetTypeID()) {
			type = CFBooleanGetValue((CFBooleanRef)got) ? "boolean true" : "boolean false";
		} else if (CFGetTypeID(got) == CFNumberGetTypeID()) {
			int n = 0;
			CFNumberGetValue((CFNumberRef)got, kCFNumberSInt32Type, &n);
			type = n ? "number 1" : "number 0";
		} else {
			type = "other type";
		}
		CFRelease(got);
	}
	printf("loc_test: %s: write %s, read back %s\n", name, synced ? "ok" : "FAILED", type);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc > 1 && strcmp(argv[1], "spi") == 0) {
			typedef Boolean (*set_fn)(Boolean);
			set_fn set = (set_fn)dlsym(RTLD_DEFAULT, "CLInternalSetLocationServicesEnabled");

			if (set == NULL) {
				printf("loc_test: no CLInternalSetLocationServicesEnabled in CoreLocation\n");
				return 1;
			}
			Boolean ok = set(true);
			printf("loc_test: CLInternalSetLocationServicesEnabled(YES) -> %d\n", (int)ok);
			printf("loc_test: locationServicesEnabled = %d\n", (int)[CLLocationManager locationServicesEnabled]);
			return ok ? 0 : 1;
		}
		if (argc > 1 && (strcmp(argv[1], "enable") == 0 || strcmp(argv[1], "enablenum") == 0)) {
			int one = 1;
			int as_number = strcmp(argv[1], "enablenum") == 0;
			CFPropertyListRef value;

			/* locationd runs as mobile (501) and reads this key from its own home,
			 * /var/mobile/Library/Preferences/com.apple.locationd.plist.  As root,
			 * "current user" would be /var/root and the write would be invisible. */
			if (geteuid() == 0) {
				if (setgid(501) != 0 || setuid(501) != 0) {
					perror("loc_test: setgid/setuid(501)");
					return 1;
				}
			}
			printf("loc_test: writing as uid %d gid %d (%s)\n", (int)geteuid(),
			    (int)getegid(), as_number ? "number" : "boolean");
			value = as_number ? (CFPropertyListRef)CFNumberCreate(NULL, kCFNumberSInt32Type, &one)
			    : (CFPropertyListRef)kCFBooleanTrue;
			write_read(kCFPreferencesCurrentUser, kCFPreferencesAnyHost, "current user, any host", CFSTR("LocationServicesEnabledIn8.0"), value);
			write_read(kCFPreferencesAnyUser, kCFPreferencesAnyHost, "any user, any host", CFSTR("LocationServicesEnabledIn8.0"), value);
			write_read(kCFPreferencesCurrentUser, kCFPreferencesAnyHost, "current user, any host (legacy)", CFSTR("LocationServicesEnabled"), value);
			if (as_number) {
				CFRelease(value);
			}
			return 0;
		}
		if (argc > 1 && strcmp(argv[1], "watch") == 0) {
			int secs = argc > 2 ? atoi(argv[2]) : 30;
			CLLocationManager *manager;
			LocWatch *watch;
			NSDate *end;

			/* a client of locationd is a mobile process on this system */
			if (geteuid() == 0 && (setgid(501) != 0 || setuid(501) != 0)) {
				perror("loc_test: setgid/setuid(501)");
				return 1;
			}
			manager = [[CLLocationManager alloc] init];
			watch = [[LocWatch alloc] init];
			end = [NSDate dateWithTimeIntervalSinceNow:secs];
			manager.delegate = watch;
			printf("loc_test: watch %d s as uid %d: services %d, authorization %ld\n", secs,
			    (int)geteuid(), (int)[CLLocationManager locationServicesEnabled],
			    (long)[CLLocationManager authorizationStatus]);
			@try {
				[manager requestWhenInUseAuthorization];
				[manager startUpdatingLocation];
			} @catch (NSException *e) {
				printf("loc_test: exception: %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
				return 1;
			}
			while ([end timeIntervalSinceNow] > 0) {
				@autoreleasepool {
					[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
					    beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
				}
			}
			[manager stopUpdatingLocation];
			printf("loc_test: watch done, authorization %ld\n",
			    (long)[CLLocationManager authorizationStatus]);
			fflush(stdout);
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
