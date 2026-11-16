/*
 * weatherlog: say every URL request the Weather data path makes and every
 * answer it gets.
 *
 * The app's forecast path needs a FairPlay device identity this port has none
 * of (CHECKLIST, "Weather: the app's own fairplayd gate"), and a stand-in for
 * fairplayd changes nothing.  Before the data source can be replaced in the
 * app's process (devices/thyme/docs/weather-data-shim.md) the seam has to be
 * found: who fetches, over which URLSession entry point, with what host, path
 * and Authorization header.
 *
 * Three APIs matter.  WeatherKit's client in the app uses the
 * completion-handler data tasks: logs/151121-run.log has its PUT to
 * weather-edge.apple.com/v1/configs and GET to c.apple.news through them.
 * The delegate-based dataTaskWithRequest: is what the app's App Store bag
 * fetch and WeatherDaemon's WDSURLSession use (logs/152821-run.log.live), so
 * that entry point is interposed as well.  And because the app then sits on
 * com.apple.fairplayd lookups while weatherd stays silent, NSXPCConnection is
 * interposed too: which service a process connects to and what error the
 * connection returns is where that gate shows itself.
 *
 * /dev/console, not NSLog: NSLog from these apps does not reach the run's
 * log, while the prefs and tweaks libraries' console lines do.
 */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

static void
weather_say(NSString *message)
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "hakios: weatherlog: %s: %s\n",
	    [[[NSProcessInfo processInfo] processName] UTF8String], [message UTF8String]);
	int fd = open("/dev/console", O_WRONLY);

	if (fd >= 0) {
		write(fd, line, (size_t)n);
		close(fd);
	}
}

/* "no", "yes", or "jwt" when the value looks like a JSON Web Token (the
 * WeatherKit client's Authorization scheme: three base64url parts). */
static NSString *
weather_auth(NSString *value)
{
	if (value == nil || value.length == 0) {
		return @"no";
	}
	if ([value hasPrefix:@"eyJ"] || [value rangeOfString:@"."].location != NSNotFound) {
		return @"jwt";
	}
	return @"yes";
}

static NSString *
weather_describe(NSURLRequest *request, NSString *kind)
{
	NSString *auth = [request valueForHTTPHeaderField:@"Authorization"];

	return [NSString stringWithFormat:@"%@ %@ %@ (Authorization: %@)", kind, request.HTTPMethod,
	    request.URL.absoluteString, weather_auth(auth)];
}

typedef void (^weather_completion)(NSData *, NSURLResponse *, NSError *);

@implementation NSURLSession (WeatherLog)
- (NSURLSessionDataTask *)wl_dataTaskWithRequest:(NSURLRequest *)request completionHandler:(weather_completion)completion
{
	weather_say(weather_describe(request, @"request"));
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

/* The delegate-based entry point: the task's callbacks go to the session's
 * delegate, so only the request is said.  (This Foundation has no
 * dataTaskWithRequest:delegate:; the app's own WeatherKit client and
 * WeatherDaemon's WDSURLSession both go through this one.) */
- (NSURLSessionDataTask *)wl_dataTaskWithRequestPlain:(NSURLRequest *)request
{
	weather_say(weather_describe(request, @"request (delegate task, session delegate)"));
	return [self wl_dataTaskWithRequestPlain:request];
}
@end

/* The auth gate is on the other side of XPC: the app looks fairplayd up and
 * retries, and weatherd (the on-demand daemon) is spawned but silent.  These
 * say which services the process connects to, asks for a proxy on, and what
 * error the connection hands back, so the gate's place is visible. */
@implementation NSXPCConnection (WeatherLog)
- (instancetype)wl_initWithMachServiceName:(NSString *)name options:(NSXPCConnectionOptions)options
{
	weather_say([NSString stringWithFormat:@"xpc connect to %@", name]);
	return [self wl_initWithMachServiceName:name options:options];
}

- (id)wl_remoteObjectProxyWithErrorHandler:(void (^)(NSError *))handler
{
	weather_say(@"xpc proxy requested");
	void (^wrapped)(NSError *) = ^(NSError *error) {
		weather_say([NSString stringWithFormat:@"xpc error: %@ (%ld)",
		    error.localizedDescription, (long)error.code]);
		if (handler != nil) {
			handler(error);
		}
	};
	return [self wl_remoteObjectProxyWithErrorHandler:wrapped];
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
	weather_say(@"loaded");
	exchange("dataTaskWithRequest:completionHandler:", "wl_dataTaskWithRequest:completionHandler:");
	exchange("dataTaskWithURL:completionHandler:", "wl_dataTaskWithURL:completionHandler:");
	exchange("dataTaskWithRequest:", "wl_dataTaskWithRequestPlain:");
	exchange("initWithMachServiceName:options:", "wl_initWithMachServiceName:options:");
	exchange("remoteObjectProxyWithErrorHandler:", "wl_remoteObjectProxyWithErrorHandler:");
}
