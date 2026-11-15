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
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>

static IMP original_viewDidLoad;
static IMP original_viewWillAppear;

static void
say(const char *text)
{
	fprintf(stderr, "hakios-prefs: %s\n", text);
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

	if (specifier_class == NULL || detail_class == NULL) {
		return;
	}
	specifiers = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("specifiers"));
	if (specifiers == NULL) {
		return;
	}
	{
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(specifiers,
		    sel_registerName("count"));

		for (unsigned long i = 0; i < count; i++) {
			id specifier = ((id (*)(id, SEL, unsigned long))objc_msgSend)(specifiers,
			    sel_registerName("objectAtIndex:"), i);
			id identifier = ((id (*)(id, SEL))objc_msgSend)(specifier,
			    sel_registerName("identifier"));

			if (identifier != NULL &&
			    ((BOOL (*)(id, SEL, id))objc_msgSend)(identifier,
				sel_registerName("isEqualToString:"), (id)CFSTR("LOCATION"))) {
				return;
			}
		}
	}
	{
		id specifier = ((id (*)(id, SEL, id, id, SEL, SEL, id, long, id))objc_msgSend)(
		    specifier_class, sel_registerName("preferenceSpecifierNamed:target:set:get:detail:cell:edit:"),
		    (id)CFSTR("Location"), self, (SEL)0, (SEL)0, detail_class, 2, (id)0);

		if (specifier == NULL) {
			say("could not make the Location specifier");
			return;
		}
		((void (*)(id, SEL, id, id))objc_msgSend)(specifier,
		    sel_registerName("setProperty:forKey:"), (id)CFSTR("Location"),
		    (id)CFSTR("iconCache"));
		((void (*)(id, SEL, id))objc_msgSend)(specifier, sel_registerName("setIdentifier:"),
		    (id)CFSTR("LOCATION"));
		((void (*)(id, SEL, id, long, BOOL))objc_msgSend)(self,
		    sel_registerName("insertSpecifier:atIndex:animated:"), specifier, 1, NO);
		say("Location row added");
	}
}

static BOOL
is_privacy_controller(id self)
{
	Class cls = objc_getClass("PUIPrivacyController");

	return cls != NULL &&
	    ((BOOL (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("isKindOfClass:"), cls);
}

/*
 * PSListController's own methods, replaced for every list controller in
 * Settings but acted on only when the instance is PUIPrivacyController.  The
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
	if (is_privacy_controller(self)) {
		ensure_location_row(self);
	}
}

__attribute__((constructor)) static void
prefs_init(void)
{
	Class cls = objc_getClass("PSListController");
	Method method;

	if (cls == NULL) {
		say("PSListController not found");
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
	say("hooked PSListController");
}
