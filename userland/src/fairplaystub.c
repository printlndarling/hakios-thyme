/*
 * fairplaystub: the Mach services com.apple.fairplayd and .versioned, answered
 * with empty XPC replies and described on the console.
 *
 * The Weather app (and WeatherKit in it) carries the fairplay-client right and
 * asks fairplayd for the device's FairPlay identity at startup; this port has
 * no fairplayd -- none of the SEP hardware it needs -- so launchd answers "No
 * such process" and the app retries once a minute instead of asking weatherd
 * for forecasts (logs/131642-run.log).  This stand-in owns the two names so
 * that the lookups succeed and what the client sends can be read: the reply is
 * an empty dictionary, which is honest about what this stub knows.
 *
 * The XPC headers are not in the Theos SDK; the four calls and two type objects
 * used here are declared as libxpc exports them (the same shape as xpc/xpc.h).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection;
extern const struct _xpc_type_s _xpc_type_dictionary;

extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, void (^handler)(xpc_object_t event));
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern int xpc_connection_get_pid(xpc_connection_t connection);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern char *xpc_copy_description(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);

#define XPC_CONNECTION_MACH_SERVICE_LISTENER 1

static void
serve(const char *name)
{
	xpc_connection_t listener = xpc_connection_create_mach_service(name, dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		printf("fairplaystub: %s: no listener\n", name);
		return;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
		if (xpc_get_type(event) != &_xpc_type_connection) {
			char *d = xpc_copy_description(event);
			printf("fairplaystub: %s: listener event %s\n", name, d != NULL ? d : "?");
			free(d);
			return;
		}
		xpc_connection_t peer = (xpc_connection_t)event;
		printf("fairplaystub: %s: connection from pid %d\n", name, xpc_connection_get_pid(peer));
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			char *d = xpc_copy_description(message);
			printf("fairplaystub: %s: message from pid %d: %s\n", name, xpc_connection_get_pid(peer),
			    d != NULL ? d : "(none)");
			fflush(stdout);
			free(d);
			if (xpc_get_type(message) == &_xpc_type_dictionary) {
				xpc_object_t reply = xpc_dictionary_create_reply(message);
				if (reply != NULL) {
					xpc_connection_send_message(peer, reply);
				}
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	printf("fairplaystub: listening on %s\n", name);
	fflush(stdout);
}

int
main(void)
{
	/* a launchd job's standard output is /dev/null unless its plist says otherwise; the console is where
	 * the kernel log is, and the messages are the point of this program */
	int console = open("/dev/console", O_WRONLY);

	if (console >= 0) {
		dup2(console, 1);
		dup2(console, 2);
		close(console);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* (the SDK has no libxpc to link against: the four calls are resolved from the system's own library,
	 * which this image's dyld cache carries as /usr/lib/system/libxpc.dylib) */
	if (dlopen("/usr/lib/system/libxpc.dylib", RTLD_LAZY) == NULL) {
		printf("fairplaystub: no libxpc: %s\n", dlerror());
		return 1;
	}
	serve("com.apple.fairplayd");
	serve("com.apple.fairplayd.versioned");
	dispatch_main();
	return 0;
}
