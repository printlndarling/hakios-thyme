/*
 * LocProbe: the smallest real application that asks CoreLocation for a position.  A command
 * line program is refused before TCC is consulted ("This app has attempted to access
 * privacy-sensitive data without a usage description", kCLErrorDenied), so the question
 * "does locationd have a position at all" needs an application with a bundle identifier and
 * NSLocationWhenInUseUsageDescription.  It writes what happens where a scene can read it,
 * /var/jb/locprobe.log, because an application's NSLog does not reach the console.
 *
 *   built as /var/jb/Applications/LocProbe.app (userland/build.sh), registered by uicache
 *   launched by the scene with "uiopen hakiosloc://"
 */
#import <UIKit/UIKit.h>
#import <CoreLocation/CoreLocation.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void
note(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *line = [[NSString alloc] initWithFormat:format arguments:args];
	FILE *log;
	time_t now;
	struct tm tm;
	char stamp[32];

	va_end(args);
	NSLog(@"LocProbe: %@", line);
	/* the scene makes /var/jb/locprobe world-writable before the launch; the container's
	 * caches directory is the fallback a sandbox definitely allows */
	log = fopen("/var/jb/locprobe/locprobe.log", "a");
	if (log == NULL) {
		NSString *caches = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES) firstObject];

		if (caches != nil) {
			NSString *path = [caches stringByAppendingPathComponent:@"locprobe.log"];

			log = fopen(path.UTF8String, "a");
		}
	}
	if (log != NULL) {
		now = time(NULL);
		localtime_r(&now, &tm);
		strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
		fprintf(log, "%s %s\n", stamp, line.UTF8String);
		fclose(log);
	}
}

@interface LocProbeDelegate : UIResponder <UIApplicationDelegate, CLLocationManagerDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) CLLocationManager *manager;
@property (nonatomic, strong) NSTimer *ticker;
@end

@implementation LocProbeDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	UIViewController *controller = [[UIViewController alloc] init];
	UILabel *label;

	controller.view.backgroundColor = [UIColor systemBackgroundColor];
	label = [[UILabel alloc] initWithFrame:controller.view.bounds];
	label.text = @"HakiOS LocProbe";
	label.textAlignment = NSTextAlignmentCenter;
	label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[controller.view addSubview:label];
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = controller;
	[self.window makeKeyAndVisible];

	note(@"launched: services %d, authorization %ld, home %@", (int)[CLLocationManager locationServicesEnabled],
	    (long)[CLLocationManager authorizationStatus], NSHomeDirectory());
	self.manager = [[CLLocationManager alloc] init];
	self.manager.delegate = self;
	[self.manager requestWhenInUseAuthorization];
	[self.manager startUpdatingLocation];
	self.ticker = [NSTimer scheduledTimerWithTimeInterval:15.0 repeats:YES block:^(NSTimer *timer) {
		note(@"waiting: authorization %ld", (long)[CLLocationManager authorizationStatus]);
	}];
	return YES;
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
	note(@"authorization %ld", (long)manager.authorizationStatus);
	if (manager.authorizationStatus == kCLAuthorizationStatusAuthorizedWhenInUse ||
	    manager.authorizationStatus == kCLAuthorizationStatusAuthorizedAlways) {
		[manager startUpdatingLocation];
	}
}

- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	for (CLLocation *location in locations) {
		note(@"location %.6f %.6f +/- %.1f m age %.1f s", location.coordinate.latitude,
		    location.coordinate.longitude, location.horizontalAccuracy,
		    -[location.timestamp timeIntervalSinceNow]);
	}
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	note(@"failed: %@ %ld", error.domain, (long)error.code);
}

@end

int
main(int argc, char **argv)
{
	return UIApplicationMain(argc, argv, nil, @"LocProbeDelegate");
}
