/*
 * libhakios.sydexc.dylib: an uncaught exception's stack, on the console.
 *
 * Injected into a daemon that dies of an Objective-C exception before anything
 * reaches the log (syncdefaultsd, for one: -[NSURL initFileURLWithPath:
 * isDirectory:] with a nil string, then SIGABRT, and the kernel's userabort
 * line has only the libc++abi frames).  The handler runs on the throwing
 * thread before the process aborts, so the stack still has the thrower.
 *
 * The exception's name, reason and -[NSThread callStackSymbols] go to
 * /dev/console as "hakios-exc:" lines; the return addresses are printed raw
 * as well, for a binary that carries no symbol names.
 *
 * Injected by userland/build.sh's INJECT (a line naming the program).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int console_fd = -1;

static void
say(const char *text)
{
	size_t length = strlen(text);

	if (console_fd < 0) {
		console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (console_fd >= 0) {
		(void)write(console_fd, text, length);
	}
	(void)write(2, text, length);
}

static void
handler(NSException *exception)
{
	@autoreleasepool {
		NSMutableString *text = [NSMutableString string];
		const char *name = exception.name != nil ? exception.name.UTF8String : "?";
		const char *reason = exception.reason != nil ? exception.reason.UTF8String : "?";

		[text appendFormat:@"hakios-exc: %s: %s\n", name, reason];
		for (NSString *line in [NSThread callStackSymbols]) {
			[text appendFormat:@"hakios-exc: %s\n", line.UTF8String];
		}
		for (NSNumber *number in [NSThread callStackReturnAddresses]) {
			[text appendFormat:@"hakios-exc: pc %#lx\n",
			    (unsigned long)number.unsignedLongLongValue];
		}
		say(text.UTF8String);
	}
}

/*
 * The uncaught handler above runs after libdispatch has caught the exception
 * out of its block and called terminate, so its stack has the abort path, not
 * the thrower.  -[NSException raise] is called while the thrower's frames are
 * still on the stack; hooking it prints the stack the question is about.  A
 * cap keeps a framework that raises for control flow from flooding the log.
 */
static IMP original_raise;
static int raises_logged;

static void
raise_hook(id self, SEL _cmd)
{
	@autoreleasepool {
		if (raises_logged < 32) {
			NSException *exception = self;
			NSMutableString *text = [NSMutableString string];
			const char *name = exception.name != nil ? exception.name.UTF8String : "?";
			const char *reason = exception.reason != nil ? exception.reason.UTF8String : "?";
			int frames = 0;

			raises_logged++;
			[text appendFormat:@"hakios-exc: raise %s: %s\n", name, reason];
			for (NSString *line in [NSThread callStackSymbols]) {
				[text appendFormat:@"hakios-exc: %s\n", line.UTF8String];
				if (++frames >= 24) {
					break;
				}
			}
			say(text.UTF8String);
		}
	}
	if (original_raise != NULL) {
		((void (*)(id, SEL))original_raise)(self, _cmd);
	}
}

/* The exception itself is thrown from inside the shared cache, but the method
 * it is thrown out of is an Objective-C method like any other: swizzle NSURL's
 * initializer, and when the path is nil the stack is the caller's. */
static IMP original_init_file_url;
static int nil_urls_logged;

static id
init_file_url_hook(id self, SEL _cmd, NSString *path, BOOL is_directory)
{
	if (path == nil && nil_urls_logged < 8) {
		@autoreleasepool {
			NSMutableString *text = [NSMutableString string];
			int frames = 0;

			nil_urls_logged++;
			[text appendString:@"hakios-exc: NSURL initFileURLWithPath: nil\n"];
			for (NSString *line in [NSThread callStackSymbols]) {
				[text appendFormat:@"hakios-exc: %s\n", line.UTF8String];
				if (++frames >= 26) {
					break;
				}
			}
			say(text.UTF8String);
		}
	}
	if (original_init_file_url != NULL) {
		return ((id (*)(id, SEL, NSString *, BOOL))original_init_file_url)(self, _cmd,
		    path, is_directory);
	}
	return self;
}

/* The URL hook shows the string is nil, not what the string was made from.
 * The container path comes through +[NSString stringWithUTF8String:]; when
 * that returns nil, print the bytes it was given. */
static IMP original_string_utf8;
static int nil_strings_logged;

static id
string_utf8_hook(id self, SEL _cmd, const char *cstring)
{
	id result = ((id (*)(id, SEL, const char *))original_string_utf8)(self, _cmd, cstring);

	if (result == nil && cstring != NULL && nil_strings_logged < 4) {
		char line[1200];
		int used = snprintf(line, sizeof(line),
		    "hakios-exc: stringWithUTF8String: nil for bytes:");
		int i;

		nil_strings_logged++;
		for (i = 0; i < 256 && used + 4 < (int)sizeof(line); i++) {
			used += snprintf(line + used, sizeof(line) - used, " %02x",
			    (unsigned char)cstring[i]);
		}
		snprintf(line + used, sizeof(line) - used, "\n");
		say(line);
	}
	return result;
}

__attribute__((constructor)) static void
hakios_exc_init(void)
{
	@autoreleasepool {
		char line[128];
		Class cls = objc_getClass("NSException");
		Method method;

		snprintf(line, sizeof(line), "hakios-exc: watching exceptions (pid %d)\n",
		    (int)getpid());
		say(line);
		NSSetUncaughtExceptionHandler(handler);
		if (cls != NULL) {
			method = class_getInstanceMethod(cls, sel_registerName("raise"));
			if (method != NULL) {
				original_raise = method_setImplementation(method, (IMP)raise_hook);
			}
		}
		say(original_raise != NULL ? "hakios-exc: hooked -[NSException raise]\n"
		    : "hakios-exc: no -[NSException raise] to hook\n");
		cls = objc_getClass("NSURL");
		if (cls != NULL) {
			method = class_getInstanceMethod(cls,
			    sel_registerName("initFileURLWithPath:isDirectory:"));
			if (method != NULL) {
				original_init_file_url = method_setImplementation(method,
				    (IMP)init_file_url_hook);
			}
		}
		say(original_init_file_url != NULL ? "hakios-exc: hooked -[NSURL initFileURLWithPath:isDirectory:]\n"
		    : "hakios-exc: no NSURL initializer to hook\n");
		cls = objc_getClass("NSString");
		if (cls != NULL) {
			method = class_getClassMethod(cls, sel_registerName("stringWithUTF8String:"));
			if (method != NULL) {
				original_string_utf8 = method_setImplementation(method,
				    (IMP)string_utf8_hook);
			}
		}
		say(original_string_utf8 != NULL ? "hakios-exc: hooked +[NSString stringWithUTF8String:]\n"
		    : "hakios-exc: no stringWithUTF8String to hook\n");
	}
}
