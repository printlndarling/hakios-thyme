/*
 * libhakios.prefs.dylib: the Location Services row put back into Settings, and
 * the stock cellular and Bluetooth rows let through.
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
#include <sys/sysctl.h>
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

/* ---- the cellular capability: the stock row, unfiltered -------------------- *
 *
 * The stock Settings root list (PreferencesUI's Settings.plist) has always had
 * a cellular row -- label "CELLULAR", iconCache "CellularData", isController,
 * pushing Apple's own SettingsCellular bundle page.  It is not shown on this
 * port because the row carries requiredCapabilities = ("cellular-data"), and
 * Preferences' plist-to-specifier conversion drops every row whose capability
 * answers no (SystemHasCapabilities in Preferences, which asks
 * -[PSCapabilityManager hasCapabilities:], whose string path asks
 * -capabilityAnswer: for that name).  The answer reaches CommCenter through
 * MobileGestalt; without CommCenter it is nil, which the filter reads as NO.
 *
 * -capabilityAnswer: consults its own overrides dictionary before anything
 * else, so answering "cellular-data" = true there makes the stock row pass and
 * the stock SettingsCellular page opens behind it.  No row, page or class of
 * ours is added: the plist row is the original one.
 *
 * The dictionary starts out nil (capabilityAnswer: checks for that), and
 * -setOverrideValue:forKey: writes into it through -overrides, which would be
 * a message to nil; so when it is nil a dictionary is made and handed over with
 * -setOverrides:.  Whatever is already in it stays: the whole thing must not
 * be replaced.
 *
 * Bluetooth is the same case: Settings.plist's BLUETOOTH row (cell
 * PSLinkListCell, detail BTSDevicesController) requires "bluetooth", and
 * without that answer the row -- and the whole Bluetooth page behind it -- is
 * filtered out of Settings.  Both names are answered true here; the rows, the
 * policies and the pages are Apple's own, none of ours are added.
 */
static void
ensure_capability_override(void)
{
	static int said;
	Class manager = objc_getClass("PSCapabilityManager");
	Class dict_class = objc_getClass("NSMutableDictionary");
	id shared, overrides;
	const char *const granted[] = { "cellular-data", "bluetooth" };

	if (manager == NULL || dict_class == NULL) {
		if (!said) {
			said = 1;
			say("hakios-prefs: PSCapabilityManager is not loaded; the stock cellular and Bluetooth rows stay filtered\n");
		}
		return;
	}
	shared = ((id (*)(id, SEL))objc_msgSend)(manager, sel_registerName("sharedManager"));
	if (shared == NULL || !((BOOL (*)(id, SEL, SEL))objc_msgSend)(shared,
	    sel_registerName("respondsToSelector:"), sel_registerName("setOverrides:"))) {
		if (!said) {
			said = 1;
			say("hakios-prefs: PSCapabilityManager cannot take overrides\n");
		}
		return;
	}
	overrides = ((id (*)(id, SEL))objc_msgSend)(shared, sel_registerName("overrides"));
	if (overrides == NULL) {
		overrides = ((id (*)(id, SEL))objc_msgSend)(dict_class, sel_registerName("dictionary"));
		if (overrides != NULL) {
			((void (*)(id, SEL, id))objc_msgSend)(shared, sel_registerName("setOverrides:"), overrides);
		}
	}
	if (overrides == NULL) {
		if (!said) {
			said = 1;
			say("hakios-prefs: no dictionary for the capability answers\n");
		}
		return;
	}
	for (unsigned i = 0; i < sizeof(granted) / sizeof(granted[0]); i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, granted[i], kCFStringEncodingUTF8);

		if (key != NULL) {
			((void (*)(id, SEL, id, id))objc_msgSend)(overrides,
			    sel_registerName("setObject:forKeyedSubscript:"), (id)kCFBooleanTrue, (id)key);
			CFRelease(key);
		}
	}
	if (!said) {
		const char *const names[] = { "cellular-data", "bluetooth", "wifi" };

		said = 1;
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
			id answer = name != NULL ?
			    ((id (*)(id, SEL, id))objc_msgSend)(shared, sel_registerName("capabilityAnswer:"),
				(id)name) : NULL;
			BOOL value = answer != NULL && ((BOOL (*)(id, SEL))objc_msgSend)(answer,
			    sel_registerName("boolValue"));

			say("hakios-prefs: capability %s answers %s\n", names[i],
			    answer == NULL ? "nil" : value ? "true" : "false");
			if (name != NULL) {
				CFRelease(name);
			}
		}
		say("hakios-prefs: cellular-data and bluetooth answered true: the stock CELLULAR and Bluetooth rows are not filtered\n");
	}
}

/* ---- the stock cellular page when telephony has nothing to show ------------ *
 *
 * PSUICellularController's -specifiers builds its rows from CoreTelephony and
 * from its "no SIMs" layout; without CommCenter nothing answers, it stores an
 * empty list, and the page opens blank.  Its own fallback for a dead telephony
 * stack is -_deadTelephonySpecifiers, which builds Apple's "Carrier settings
 * update required" group and Learn More footer without asking telephony
 * anything.  The class's -specifiers is replaced the first time one of its
 * pages loads, and an empty result is answered with that fallback: still the
 * stock page and the stock rows.
 */
static IMP original_cell_page_specifiers;
static int cell_page_hooked;

static id
cell_page_specifiers(id self, SEL _cmd)
{
	id list = original_cell_page_specifiers != NULL ?
	    ((id (*)(id, SEL))original_cell_page_specifiers)(self, _cmd) : NULL;
	unsigned long count = list != NULL ?
	    ((unsigned long (*)(id, SEL))objc_msgSend)(list, sel_registerName("count")) : 0;

	if (count == 0 && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(self,
	    sel_registerName("respondsToSelector:"), sel_registerName("_deadTelephonySpecifiers"))) {
		id dead = ((id (*)(id, SEL))objc_msgSend)(self,
		    sel_registerName("_deadTelephonySpecifiers"));
		unsigned long dead_count = dead != NULL ?
		    ((unsigned long (*)(id, SEL))objc_msgSend)(dead, sel_registerName("count")) : 0;

		if (dead_count > 0) {
			say("hakios-prefs: the cellular page was empty; Apple's dead-telephony rows stand in (%lu)\n",
			    dead_count);
			return dead;
		}
	}
	return list;
}

static void
hook_cell_page(id self)
{
	Method method;
	Class cls;

	if (cell_page_hooked || strcmp(class_name_of(self), "PSUICellularController") != 0) {
		return;
	}
	cell_page_hooked = 1;
	cls = object_getClass(self);
	method = cls != NULL ? class_getInstanceMethod(cls, sel_registerName("specifiers")) : NULL;
	if (method == NULL) {
		say("hakios-prefs: PSUICellularController has no -specifiers\n");
		return;
	}
	original_cell_page_specifiers = method_getImplementation(method);
	method_setImplementation(method, (IMP)cell_page_specifiers);
	say("hakios-prefs: PSUICellularController -specifiers replaced (empty -> Apple's dead-telephony rows)\n");
}

/* The root list once: what is in it, so a missing row (Bluetooth, say) is a
 * fact from the log rather than a guess. */
static void
dump_root_once(id self)
{
	static int dumped;
	id specifiers;
	unsigned long count;

	if (dumped) {
		return;
	}
	specifiers = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("specifiers"));
	count = specifiers != NULL ?
	    ((unsigned long (*)(id, SEL))objc_msgSend)(specifiers, sel_registerName("count")) : 0;
	if (count == 0) {
		return;                 /* not built yet; a later load or appearance comes back */
	}
	dumped = 1;
	say("hakios-prefs: root list: %lu items\n", count);
	for (unsigned long i = 0; i < count && i < 64; i++) {
		id specifier = ((id (*)(id, SEL, unsigned long))objc_msgSend)(specifiers,
		    sel_registerName("objectAtIndex:"), i);
		id label = ((id (*)(id, SEL))objc_msgSend)(specifier, sel_registerName("name"));
		id identifier = ((id (*)(id, SEL))objc_msgSend)(specifier, sel_registerName("identifier"));
		const char *label_text = label != NULL ?
		    ((const char *(*)(id, SEL))objc_msgSend)(label, sel_registerName("UTF8String")) : NULL;
		const char *id_text = identifier != NULL ?
		    ((const char *(*)(id, SEL))objc_msgSend)(identifier, sel_registerName("UTF8String")) : NULL;

		say("hakios-prefs: root %lu: \"%s\" id \"%s\"\n", i,
		    label_text != NULL ? label_text : "?", id_text != NULL ? id_text : "?");
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
	ensure_capability_override();
	hook_cell_page(self);
	if (original_viewDidLoad != NULL) {
		((void (*)(id, SEL))original_viewDidLoad)(self, _cmd);
	}
	if (appearances_logged < 64) {
		appearances_logged++;
		say("hakios-prefs: load %s\n", class_name_of(self));
	}
	if (strcmp(class_name_of(self), "PSUIPrefsListController") == 0) {
		dump_root_once(self);
	}
	if (is_privacy_controller(self)) {
		ensure_location_row(self);
	}
}

static void
prefs_viewWillAppear(id self, SEL _cmd, BOOL animated)
{
	ensure_capability_override();
	hook_cell_page(self);
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
	ensure_capability_override();
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
	ensure_capability_override();
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
	ensure_capability_override();
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
