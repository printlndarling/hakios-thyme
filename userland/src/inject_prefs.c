/*
 * libhakios.prefs.dylib: the Location Services row, put back into Settings.
 *
 * The Privacy & Security page is PUIPrivacyController with Privacy.plist's
 * items; this port's Preferences filters the plist's LOCATION specifier out
 * before it is shown, whatever the row's detail class, getter, label or icon
 * says (devices/thyme/CHECKLIST.md, 2026-11-15).  Rather than find that filter,
 * this library adds the row itself when the page loads: a PSListListCell that
 * pushes PUILocationServicesListController -- the real Location Services page,
 * master switch and per-app list -- with the identifier LOCATION so that
 * prefs:root=Privacy&path=LOCATION (the URL the system's "Turn On Location
 * Services" alert opens) finds it.
 *
 * Injected by userland/build.sh's INJECT (a line naming Preferences).
 *
 * Diagnostics go to /dev/console as well as stderr: Settings' stderr is not in
 * the kernel log, and the console is (that is how the window server's and the
 * tweak loader's messages are seen).  Every list controller that loads or
 * appears is named there, so the class the Privacy page really uses -- and
 * whether these hooks run in it at all -- can be read out of a run's log.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static IMP original_viewDidLoad;
static IMP original_viewWillAppear;
static IMP original_viewDidAppear;
static IMP original_reloadSpecifiers;
static int console_fd = -1;
static int appearances_logged;
static int inside_reload;

static void
say(const char *format, ...)
{
	char buffer[512];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(buffer, sizeof(buffer), format, arguments);
	va_end(arguments);
	if (length <= 0) {
		return;
	}
	if ((size_t)length > sizeof(buffer) - 1) {
		length = (int)(sizeof(buffer) - 1);
	}
	if (console_fd < 0) {
		console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (console_fd >= 0) {
		(void)write(console_fd, buffer, (size_t)length);
	}
	(void)write(2, buffer, (size_t)length);
}

static const char *
class_name_of(id object)
{
	const char *name;

	if (object == NULL) {
		return "?";
	}
	name = object_getClassName(object);
	return name != NULL ? name : "?";
}

static BOOL
name_has_privacy(const char *name)
{
	/* The page, not every page with the word in it: PUILocationServices-
	 * PrivacyAlertsLevelController is a subpage of Location Services, and a
	 * Location row in it is not ours to add. */
	return name != NULL && strstr(name, "PrivacyController") != NULL;
}

static BOOL
is_privacy_controller(id self)
{
	Class cls = objc_getClass("PUIPrivacyController");
	const char *name = class_name_of(self);

	if (name_has_privacy(name)) {
		return YES;             /* PUIPrivacyController, or whatever the page calls itself here */
	}
	if (cls != NULL &&
	    ((BOOL (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("isKindOfClass:"), cls)) {
		return YES;
	}
	return NO;
}

/*
 * Add the row unless the controller already has a specifier with our
 * identifier.  Called after the page's own load and on every appearance, so a
 * reload does not lose it.
 */
static void
ensure_location_row(id self)
{
	Class specifier_class = objc_getClass("PSSpecifier");
	Class detail_class = objc_getClass("PUILocationServicesListController");
	id specifiers;
	unsigned long count;

	if (specifier_class == NULL || detail_class == NULL) {
		say("hakios-prefs: %s: specifier %s, location page %s\n", class_name_of(self),
		    specifier_class != NULL ? "yes" : "missing",
		    detail_class != NULL ? "yes" : "missing");
		return;
	}
	specifiers = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("specifiers"));
	if (specifiers == NULL) {
		say("hakios-prefs: %s: no specifiers\n", class_name_of(self));
		return;
	}
	count = ((unsigned long (*)(id, SEL))objc_msgSend)(specifiers, sel_registerName("count"));
	if (count == 0) {
		/* The page has not built its list yet: inserting now would be lost,
		 * and an index into an empty list is the exception that failed the
		 * first navigation.  A later appearance or reload comes back. */
		say("hakios-prefs: %s: empty list, will add later\n", class_name_of(self));
		return;
	}
	for (unsigned long i = 0; i < count; i++) {
		id specifier = ((id (*)(id, SEL, unsigned long))objc_msgSend)(specifiers,
		    sel_registerName("objectAtIndex:"), i);
		id identifier = ((id (*)(id, SEL))objc_msgSend)(specifier, sel_registerName("identifier"));

		if (identifier != NULL &&
		    ((BOOL (*)(id, SEL, id))objc_msgSend)(identifier,
			sel_registerName("isEqualToString:"), (id)CFSTR("LOCATION"))) {
			return;
		}
	}
	/* The page's first object is its group header; insert after it, but
	 * never past the end (an empty list is why a navigation into this
	 * page failed before). */
	{
		unsigned long index = count > 1 ? 1 : count;

		if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(self,
			sel_registerName("respondsToSelector:"),
			sel_registerName("insertSpecifier:atIndex:animated:"))) {
			say("hakios-prefs: %s: no insertSpecifier (count %lu)\n",
			    class_name_of(self), count);
			return;
		}
		{
			id specifier = ((id (*)(id, SEL, id, id, SEL, SEL, id, long, id))objc_msgSend)(
			    specifier_class,
			    sel_registerName("preferenceSpecifierNamed:target:set:get:detail:cell:edit:"),
			    (id)CFSTR("Location Services"), self, (SEL)0, (SEL)0, detail_class,
			    2, (id)0);

			if (specifier == NULL) {
				say("hakios-prefs: %s: could not make the specifier\n",
				    class_name_of(self));
				return;
			}
			((void (*)(id, SEL, id, id))objc_msgSend)(specifier,
			    sel_registerName("setProperty:forKey:"), (id)CFSTR("Location"),
			    (id)CFSTR("iconCache"));
			((void (*)(id, SEL, id))objc_msgSend)(specifier,
			    sel_registerName("setIdentifier:"), (id)CFSTR("LOCATION"));
			((void (*)(id, SEL, id, unsigned long, BOOL))objc_msgSend)(self,
			    sel_registerName("insertSpecifier:atIndex:animated:"), specifier, index, NO);
			say("hakios-prefs: %s: Location row added at %lu of %lu\n",
			    class_name_of(self), index, count);
		}
	}
}

/*
 * PSListController's own methods, replaced for every list controller in
 * Settings but acted on only when the instance is the privacy page.  The
 * privacy framework is not loaded when this library is inserted (it is a
 * dlopen of the page's own push), so hooking the class by name at constructor
 * time misses it; PSListController is always there.
 */
static void
prefs_viewDidLoad(id self, SEL _cmd)
{
	if (original_viewDidLoad != NULL) {
		((void (*)(id, SEL))original_viewDidLoad)(self, _cmd);
	}
	if (appearances_logged < 64) {
		appearances_logged++;
		say("hakios-prefs: load %s\n", class_name_of(self));
	}
	if (is_privacy_controller(self)) {
		ensure_location_row(self);
	}
}

static void
prefs_viewWillAppear(id self, SEL _cmd, BOOL animated)
{
	if (original_viewWillAppear != NULL) {
		((void (*)(id, SEL, BOOL))original_viewWillAppear)(self, _cmd, animated);
	}
	if (appearances_logged < 64) {
		appearances_logged++;
		say("hakios-prefs: appear %s\n", class_name_of(self));
	}
	if (is_privacy_controller(self)) {
		ensure_location_row(self);
	}
}

static void
prefs_viewDidAppear(id self, SEL _cmd, BOOL animated)
{
	if (original_viewDidAppear != NULL) {
		((void (*)(id, SEL, BOOL))original_viewDidAppear)(self, _cmd, animated);
	}
	if (is_privacy_controller(self)) {
		ensure_location_row(self);
	}
}

/*
 * Preferences reloads a page's specifiers when something it shows changes
 * (a location switch, a profile, a notification); the reload builds the list
 * from the plist again and would drop a row we added earlier.  Add it back
 * after every reload of the privacy page.  The guard keeps our own insert from
 * coming back through here.
 */
static void
prefs_reloadSpecifiers(id self, SEL _cmd)
{
	if (original_reloadSpecifiers != NULL) {
		((void (*)(id, SEL))original_reloadSpecifiers)(self, _cmd);
	}
	if (!inside_reload && is_privacy_controller(self)) {
		inside_reload = 1;
		ensure_location_row(self);
		inside_reload = 0;
	}
}

__attribute__((constructor)) static void
prefs_init(void)
{
	Class cls = objc_getClass("PSListController");
	Method method;

	say("hakios-prefs: library loaded (pid %d)\n", (int)getpid());
	if (cls == NULL) {
		say("hakios-prefs: PSListController not found\n");
		return;
	}
	method = class_getInstanceMethod(cls, sel_registerName("viewDidLoad"));
	if (method != NULL) {
		original_viewDidLoad = method_setImplementation(method, (IMP)prefs_viewDidLoad);
	}
	method = class_getInstanceMethod(cls, sel_registerName("viewWillAppear:"));
	if (method != NULL) {
		original_viewWillAppear = method_setImplementation(method, (IMP)prefs_viewWillAppear);
	}
	method = class_getInstanceMethod(cls, sel_registerName("viewDidAppear:"));
	if (method != NULL) {
		original_viewDidAppear = method_setImplementation(method, (IMP)prefs_viewDidAppear);
	}
	method = class_getInstanceMethod(cls, sel_registerName("reloadSpecifiers"));
	if (method != NULL) {
		original_reloadSpecifiers = method_setImplementation(method, (IMP)prefs_reloadSpecifiers);
	}
	say("hakios-prefs: hooked PSListController\n");
}
