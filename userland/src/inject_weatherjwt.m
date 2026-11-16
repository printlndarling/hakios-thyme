/*
 * weatherjwt: in weatherd, answer a JWT token request with a local token.
 *
 * WeatherDaemon's WDSJWTAuthenticatorService mints WeatherKit's token by
 * signing a request with Mescal/FairPlay (WDSJWTMescalSigner) and asking
 * Apple's token service; this port has no SEP and no FairPlay identity, so
 * that can never succeed here (CHECKLIST, "Weather: the app's own fairplayd
 * gate").  The token is only what the app's WeatherKit client needs before it
 * will ask for a forecast -- the forecast itself is going to be answered by
 * the data shim, not by Apple -- so the service's entry point is replaced
 * with one that completes at once.
 *
 * The method is ObjC-visible: WeatherDaemon.framework's selector table has
 * `generateTokenWithUrl:completion:`, typed (NSURL *, block(NSString *,
 * NSError *)).  The class is the service, not the client proxy; it is found
 * by its Swift runtime name and, failing that, by scanning for a class whose
 * name contains WDSJWTAuthenticatorService and which implements the selector.
 *
 * Enabled by WEATHERJWT=1 in userland/build.sh; injected into weatherd only.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

typedef void (^weatherjwt_completion)(NSString *, NSError *);

static void
say(NSString *message)
{
	char line[700];
	int n = snprintf(line, sizeof(line), "hakios: weatherjwt: %s\n", [message UTF8String]);
	int fd = open("/dev/console", O_WRONLY);

	if (fd >= 0) {
		write(fd, line, (size_t)n);
		close(fd);
	}
}

/* A JWT-shaped string (three dot-separated base64url parts).  Nothing here
 * validates it; a real one would have to be signed by Apple, which is the
 * wall this stands in front of. */
static NSString *
local_token(void)
{
	return @"eyJhbGciOiJFUzI1NiJ9.eyJpc3MiOiJoYWtpb3MiLCJzdWIiOiJ3ZWF0aGVyIn0.c2lnbmF0dXJl";
}

static void
weatherjwt_generate(id self, SEL _cmd, NSURL *url, weatherjwt_completion completion)
{
	say([NSString stringWithFormat:@"token request answered locally (url %@)", url]);
	if (completion != nil) {
		completion(local_token(), nil);
	}
}

static Class
weatherjwt_class(SEL selector)
{
	Class service = objc_getClass("_TtC13WeatherDaemon26WDSJWTAuthenticatorService");

	if (service != NULL && class_getInstanceMethod(service, selector) != NULL) {
		return service;
	}
	unsigned int count = 0;
	Class *classes = objc_copyClassList(&count);
	Class found = Nil;

	for (unsigned int i = 0; i < count; i++) {
		const char *name = class_getName(classes[i]);
		if (strstr(name, "WDSJWTAuthenticatorService") == NULL ||
		    class_getInstanceMethod(classes[i], selector) == NULL) {
			continue;
		}
		if (strstr(name, "Proxy") != NULL || strstr(name, "Listener") != NULL) {
			continue;
		}
		found = classes[i];
		break;
	}
	free(classes);
	return found;
}

__attribute__((constructor)) static void
weatherjwt_init(void)
{
	SEL selector = sel_registerName("generateTokenWithUrl:completion:");
	Class service = weatherjwt_class(selector);

	say(@"loaded");
	if (service == Nil) {
		say(@"no WDSJWTAuthenticatorService implements generateTokenWithUrl:completion:");
		return;
	}
	Method method = class_getInstanceMethod(service, selector);
	class_replaceMethod(service, selector, (IMP)weatherjwt_generate, method_getTypeEncoding(method));
	say([NSString stringWithFormat:@"replaced generateTokenWithUrl:completion: on %s",
	    class_getName(service)]);
}
