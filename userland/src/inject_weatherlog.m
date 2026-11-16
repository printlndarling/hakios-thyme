/*
 * weatherlog: say every URL request the Weather app makes and every answer it
 * gets.
 *
 * The app's forecast path needs a FairPlay device identity this port has none
 * of (CHECKLIST, "Weather: the app's own fairplayd gate"), and a stand-in for
 * fairplayd changes nothing.  Before the data source can be replaced in the
 * app's process (devices/thyme/docs/weather-data-shim.md) the seam has to be
 * found: does the app, or WeatherKit inside it, ever reach URLSession, and
 * with what host, path and headers?  This library answers by logging the
 * dataTask entry points the Swift URLSession API funnels through, and the
 * answers when they come.  It is injected into Weather alone (WEATHERLOG=1 in
 * userland/build.sh's INJECT).
 *
 * NSLog, not /dev/console: the Weather app is sandboxed and may not open the
 * console, while its unified-log lines reach the same log the rest of the run
 * is read from.
 */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

/* The console, as the prefs and tweaks libraries do it: their lines from
 * Preferences are in the run's log, so the app may open it and NSLog's output
 * is what did not arrive. */
static void
weather_say(NSString *message)
{
	char line[900];
	int n = snprintf(line, sizeof(line), "hakios: weatherlog: %s\n", [message UTF8String]);
	int fd = open("/dev/console", O_WRONLY);

	if (fd >= 0) {
		write(fd, line, (size_t)n);
		close(fd);
	}
}

typedef void (^weather_completion)(NSData *, NSURLResponse *, NSError *);

@implementation NSURLSession (WeatherLog)
- (NSURLSessionDataTask *)wl_dataTaskWithRequest:(NSURLRequest *)request completionHandler:(weather_completion)completion
{
	NSString *auth = [request valueForHTTPHeaderField:@"Authorization"];
	weather_say([NSString stringWithFormat:@"request %@ %@ (Authorization: %@)", request.HTTPMethod,
	    request.URL.absoluteString, auth != nil ? @"yes" : @"no"]);
	weather_completion wrapped = nil;
	if (completion != nil) {
		wrapped = ^(NSData *data, NSURLResponse *response, NSError *error) {
			NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
			    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;
			weather_say([NSString stringWithFormat:@"answer %ld from %@, %lu bytes, %@", (long)status,
			    request.URL.absoluteString.lastPathComponent, (unsigned long)data.length,
			    error != nil ? error.localizedDescription : @"no error"]);
			completion(data, response, error);
		};
	}
	return [self wl_dataTaskWithRequest:request completionHandler:wrapped];
}

- (NSURLSessionDataTask *)wl_dataTaskWithURL:(NSURL *)url completionHandler:(weather_completion)completion
{
	weather_say([NSString stringWithFormat:@"request (URL only) %@", url.absoluteString]);
	return [self wl_dataTaskWithURL:url completionHandler:completion];
}
@end

static void
exchange(const char *from, const char *to)
{
	Method a = class_getInstanceMethod([NSURLSession class], sel_registerName(from));
	Method b = class_getInstanceMethod([NSURLSession class], sel_registerName(to));

	if (a != NULL && b != NULL) {
		method_exchangeImplementations(a, b);
		weather_say([NSString stringWithFormat:@"interposed %s", from]);
	} else {
		weather_say([NSString stringWithFormat:@"could not interpose %s", from]);
	}
}

__attribute__((constructor)) static void
weatherlog_init(void)
{
	weather_say([NSString stringWithFormat:@"loaded into %@", [[NSProcessInfo processInfo] processName]]);
	exchange("dataTaskWithRequest:completionHandler:", "wl_dataTaskWithRequest:completionHandler:");
	exchange("dataTaskWithURL:completionHandler:", "wl_dataTaskWithURL:completionHandler:");
}
