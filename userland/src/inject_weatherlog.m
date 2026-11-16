/*
 * weatherlog: say every URL request the Weather app makes and every answer it
 * gets, on the console.
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
 */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static void
weather_say(NSString *format, ...)
{
	va_list ap;
	char line[900];
	int n = snprintf(line, sizeof(line), "hakios: weatherlog: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, [format UTF8String], ap);
	va_end(ap);
	if (n < (int)sizeof(line) - 1) {
		line[n++] = '\n';
	}
	line[n] = 0;
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
	weather_say(@"request %@ %@ (Authorization: %@)", request.HTTPMethod, request.URL.absoluteString,
	    auth != nil ? @"yes" : @"no");
	weather_completion wrapped = nil;
	if (completion != nil) {
		wrapped = ^(NSData *data, NSURLResponse *response, NSError *error) {
			NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
			    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;
			weather_say(@"answer %ld from %@, %lu bytes, %@", (long)status,
			    request.URL.absoluteString.lastPathComponent, (unsigned long)data.length,
			    error != nil ? error.localizedDescription : @"no error");
			completion(data, response, error);
		};
	}
	return [self wl_dataTaskWithRequest:request completionHandler:wrapped];
}

- (NSURLSessionDataTask *)wl_dataTaskWithURL:(NSURL *)url completionHandler:(weather_completion)completion
{
	weather_say(@"request (URL only) %@", url.absoluteString);
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
		weather_say(@"interposed %s", from);
	} else {
		weather_say(@"could not interpose %s", from);
	}
}

__attribute__((constructor)) static void
weatherlog_init(void)
{
	exchange("dataTaskWithRequest:completionHandler:", "wl_dataTaskWithRequest:completionHandler:");
	exchange("dataTaskWithURL:completionHandler:", "wl_dataTaskWithURL:completionHandler:");
}
