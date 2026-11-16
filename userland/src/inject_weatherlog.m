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
 * connection returns is where that gate shows itself -- except this
 * Foundation has no NSXPCConnection, so the Swift XPC layer's libxpc calls
 * are interposed instead (the __DATA,__interpose table): which Mach service,
 * and every message and reply, on the C API WeatherDaemon actually uses.
 *
 * /dev/console, not NSLog: NSLog from these apps does not reach the run's
 * log, while the prefs and tweaks libraries' console lines do.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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
/* This Foundation has no NSXPCConnection -- the app's WeatherDaemon client is
 * Swift and goes through libswiftXPC on the libxpc C API.  These are dyld
 * interposings (__DATA,__interpose): a process's connections to Mach services
 * are named, and every message and reply is described, whatever Swift layer
 * sent it.  The originals are looked up with dlsym so the wrappers do not
 * recurse into themselves. */
typedef void *wl_xpc_object;
typedef void *wl_xpc_connection;
typedef void (^wl_xpc_handler)(wl_xpc_object);

extern wl_xpc_connection xpc_connection_create(const char *, dispatch_queue_t);
extern wl_xpc_connection xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_send_message(wl_xpc_connection, wl_xpc_object);
extern void xpc_connection_send_message_with_reply(wl_xpc_connection, wl_xpc_object, dispatch_queue_t, wl_xpc_handler);
extern wl_xpc_object xpc_connection_send_message_with_reply_sync(wl_xpc_connection, wl_xpc_object);
extern char *xpc_copy_description(wl_xpc_object);

#define WL_MAX_CONN 64
static struct {
	wl_xpc_connection connection;
	char name[96];
} wl_connections[WL_MAX_CONN];
static pthread_mutex_t wl_connections_lock = PTHREAD_MUTEX_INITIALIZER;

static void
wl_remember(wl_xpc_connection connection, const char *name)
{
	pthread_mutex_lock(&wl_connections_lock);
	for (int i = 0; i < WL_MAX_CONN; i++) {
		if (wl_connections[i].connection == NULL || wl_connections[i].connection == connection) {
			wl_connections[i].connection = connection;
			snprintf(wl_connections[i].name, sizeof(wl_connections[i].name), "%s", name);
			break;
		}
	}
	pthread_mutex_unlock(&wl_connections_lock);
}

static const char *
wl_name(wl_xpc_connection connection)
{
	static __thread char name[96];

	name[0] = '\0';
	pthread_mutex_lock(&wl_connections_lock);
	for (int i = 0; i < WL_MAX_CONN; i++) {
		if (wl_connections[i].connection == connection) {
			snprintf(name, sizeof(name), "%s", wl_connections[i].name);
			break;
		}
	}
	pthread_mutex_unlock(&wl_connections_lock);
	return name;
}

/* xpc_copy_description writes several lines; the run's log is read a line at a
 * time, so the newlines become spaces and the whole message is one line. */
static void
wl_flatten(char *text)
{
	for (char *p = text; *p != '\0'; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t') {
			*p = ' ';
		}
	}
}

/* Not everything a process says over XPC belongs in the run's log: the
 * Weather app and widget both poll cfprefsd in tight synchronous loops (898
 * and 1058 messages in two runs), and those enough were to rotate the rest
 * out of the kernel's 1 MB log (logs/154426-run.log.live,
 * logs/155121-run.log.live).  Preference traffic is skipped; everything else
 * is said, because the auth and data calls are what this probe exists for. */
static int
wl_interesting(const char *connection_name, const char *description)
{
	(void)connection_name;
	if (description != NULL && strcasestr(description, "CFPreferences") != NULL) {
		return 0;
	}
	return 1;
}

static void
wl_say_xpc(const char *what, wl_xpc_connection connection, wl_xpc_object object)
{
	char *description = object != NULL ? xpc_copy_description(object) : NULL;
	const char *name = wl_name(connection);

	if (!wl_interesting(name, description)) {
		free(description);
		return;
	}
	if (description != NULL) {
		wl_flatten(description);
	}
	weather_say([NSString stringWithFormat:@"xpc %s %s: %.700s", what, name,
	    description != NULL ? description : "(none)"]);
	free(description);
}

static wl_xpc_connection
wl_xpc_connection_create(const char *name, dispatch_queue_t queue)
{
	static wl_xpc_connection (*real)(const char *, dispatch_queue_t);
	wl_xpc_connection connection;

	if (real == NULL) {
		real = (void *)dlsym(RTLD_NEXT, "xpc_connection_create");
	}
	weather_say([NSString stringWithFormat:@"xpc connection %s", name != NULL ? name : "(peer)"]);
	connection = real != NULL ? real(name, queue) : NULL;
	if (connection != NULL && name != NULL) {
		wl_remember(connection, name);
	}
	return connection;
}

static wl_xpc_connection
wl_xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
	static wl_xpc_connection (*real)(const char *, dispatch_queue_t, uint64_t);
	wl_xpc_connection connection;

	if (real == NULL) {
		real = (void *)dlsym(RTLD_NEXT, "xpc_connection_create_mach_service");
	}
	weather_say([NSString stringWithFormat:@"xpc service %s (flags 0x%llx)", name, (unsigned long long)flags]);
	connection = real != NULL ? real(name, queue, flags) : NULL;
	if (connection != NULL) {
		wl_remember(connection, name);
	}
	return connection;
}

static void
wl_xpc_connection_send_message(wl_xpc_connection connection, wl_xpc_object message)
{
	static void (*real)(wl_xpc_connection, wl_xpc_object);

	if (real == NULL) {
		real = (void *)dlsym(RTLD_NEXT, "xpc_connection_send_message");
	}
	wl_say_xpc("send", connection, message);
	if (real != NULL) {
		real(connection, message);
	}
}

static void
wl_xpc_connection_send_message_with_reply(wl_xpc_connection connection, wl_xpc_object message,
    dispatch_queue_t queue, wl_xpc_handler handler)
{
	static void (*real)(wl_xpc_connection, wl_xpc_object, dispatch_queue_t, wl_xpc_handler);

	if (real == NULL) {
		real = (void *)dlsym(RTLD_NEXT, "xpc_connection_send_message_with_reply");
	}
	wl_say_xpc("send (reply awaited)", connection, message);
	if (real == NULL) {
		return;
	}
	real(connection, message, queue, ^(wl_xpc_object reply) {
		wl_say_xpc("reply", connection, reply);
		if (handler != nil) {
			handler(reply);
		}
	});
}

static wl_xpc_object
wl_xpc_connection_send_message_with_reply_sync(wl_xpc_connection connection, wl_xpc_object message)
{
	static wl_xpc_object (*real)(wl_xpc_connection, wl_xpc_object);
	wl_xpc_object reply;

	if (real == NULL) {
		real = (void *)dlsym(RTLD_NEXT, "xpc_connection_send_message_with_reply_sync");
	}
	wl_say_xpc("send (sync reply)", connection, message);
	reply = real != NULL ? real(connection, message) : NULL;
	wl_say_xpc("sync reply", connection, reply);
	return reply;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct {
	const void *replacement;
	const void *original;
} wl_xpc_interposes[] = {
	{ (const void *)wl_xpc_connection_create, (const void *)xpc_connection_create },
	{ (const void *)wl_xpc_connection_create_mach_service, (const void *)xpc_connection_create_mach_service },
	{ (const void *)wl_xpc_connection_send_message, (const void *)xpc_connection_send_message },
	{ (const void *)wl_xpc_connection_send_message_with_reply, (const void *)xpc_connection_send_message_with_reply },
	{ (const void *)wl_xpc_connection_send_message_with_reply_sync, (const void *)xpc_connection_send_message_with_reply_sync },
};

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
