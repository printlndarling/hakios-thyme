/*
 * weatherprefs: WeatherDaemon's switches, written into the mobile user's
 * preferences before weatherd starts.
 *
 * weatherd's WDSAuthenticatorFactory takes its isJWTEnabled from
 * "weatherdaemon.enableJWTTokenAuth" (a WeatherCore Setting in weatherd's own
 * defaults).  With it on, the JWT comes from com.apple.weatherkit.authservice,
 * which carries the fairplay-client entitlement: it needs fairplayd and the
 * SEP's FairPlay IO user client, and this port has neither, so the Weather app
 * asks for forecast data it can never authenticate (the wall in
 * docs/../CHECKLIST.md, "weatherd starts, and the numbers still do not come").
 * With the switch off, the factory builds its other authenticator.
 *
 * weatherd runs as mobile (501) and reads /var/mobile/Library/Preferences: a
 * root process's CFPreferences writes go to /var/root and nobody reads them,
 * hence the setuid(501) (the same reason as loc_test's enable).
 *
 *   weatherprefs          write the switches and print what reads back
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void
set_bool(CFStringRef domain, CFStringRef key, Boolean value)
{
	CFPreferencesSetValue(key, value ? kCFBooleanTrue : kCFBooleanFalse, domain, kCFPreferencesCurrentUser,
	    kCFPreferencesAnyHost);
}

static void
show(CFStringRef domain, CFStringRef key)
{
	CFPropertyListRef value = CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	Boolean on = false;

	if (value != NULL) {
		if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
			on = CFBooleanGetValue((CFBooleanRef)value);
		}
		CFRelease(value);
	}
	printf("weatherprefs: %s = %s\n", key == CFSTR("weatherdaemon.enableJWTTokenAuth") ? "enableJWTTokenAuth" :
	    key == CFSTR("weatherdaemon.fetchesWeatherData") ? "fetchesWeatherData" : "?", on ? "true" : "false");
}

int
main(void)
{
	if (geteuid() == 0 && (setgid(501) != 0 || setuid(501) != 0)) {
		perror("weatherprefs: setgid/setuid(501)");
		return 1;
	}
	printf("weatherprefs: writing as uid %d gid %d\n", (int)geteuid(), (int)getegid());
	/* The daemon's own domain; the app's too, in case it hands its setting to
	 * the daemon with the request.
	 */
	set_bool(CFSTR("com.apple.weatherd"), CFSTR("weatherdaemon.enableJWTTokenAuth"), false);
	set_bool(CFSTR("com.apple.weatherd"), CFSTR("weatherdaemon.fetchesWeatherData"), true);
	set_bool(CFSTR("com.apple.weather"), CFSTR("weatherdaemon.enableJWTTokenAuth"), false);
	CFPreferencesAppSynchronize(CFSTR("com.apple.weatherd"));
	CFPreferencesAppSynchronize(CFSTR("com.apple.weather"));
	show(CFSTR("com.apple.weatherd"), CFSTR("weatherdaemon.enableJWTTokenAuth"));
	show(CFSTR("com.apple.weatherd"), CFSTR("weatherdaemon.fetchesWeatherData"));
	show(CFSTR("com.apple.weather"), CFSTR("weatherdaemon.enableJWTTokenAuth"));
	return 0;
}
