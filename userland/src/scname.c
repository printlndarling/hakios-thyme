#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define HAKIOS_NAME_KEY CFSTR("Setup:/System")
#define HAKIOS_NAME_FIELD CFSTR("ComputerName")
#define HAKIOS_ENCODING_FIELD CFSTR("ComputerNameEncoding")

typedef SCDynamicStoreRef (*store_create_fn)(CFAllocatorRef, CFStringRef, SCDynamicStoreCallBack, SCDynamicStoreContext *);
typedef CFPropertyListRef (*store_copy_fn)(SCDynamicStoreRef, CFStringRef);
typedef Boolean (*store_set_fn)(SCDynamicStoreRef, CFStringRef, CFPropertyListRef);
typedef CFStringRef (*copy_name_fn)(SCDynamicStoreRef, CFStringEncoding *);
typedef SCPreferencesRef (*prefs_create_fn)(CFAllocatorRef, CFStringRef, CFStringRef);
typedef Boolean (*prefs_lock_fn)(SCPreferencesRef, Boolean);
typedef Boolean (*prefs_set_name_fn)(SCPreferencesRef, CFStringRef, CFStringEncoding);
typedef Boolean (*prefs_commit_fn)(SCPreferencesRef);
typedef Boolean (*prefs_apply_fn)(SCPreferencesRef);
typedef Boolean (*prefs_unlock_fn)(SCPreferencesRef);

static store_create_fn store_create;
static store_copy_fn store_copy;
static store_set_fn store_set;
static copy_name_fn copy_name;

static void
show_name(void)
{
	CFStringEncoding encoding = kCFStringEncodingInvalidId;
	SCDynamicStoreRef store = store_create(NULL, CFSTR("hakios-scname-read"), NULL, NULL);
	CFStringRef name = store != NULL ? copy_name(store, &encoding) : NULL;
	char text[128] = "{(null)}";

	if (name != NULL) {
		if (!CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8)) {
			strcpy(text, "{not-utf8}");
		}
		CFRelease(name);
	}
	printf("scname: computer name \"%s\" encoding=%#x\n", text, encoding);
	if (store != NULL) {
		CFRelease(store);
	}
}

static Boolean
set_dynamic_name(CFStringRef name)
{
	SCDynamicStoreRef store = store_create(NULL, CFSTR("hakios-scname"), NULL, NULL);
	CFPropertyListRef current = store != NULL ? store_copy(store, HAKIOS_NAME_KEY) : NULL;
	CFMutableDictionaryRef system = current != NULL && CFGetTypeID(current) == CFDictionaryGetTypeID()
	    ? CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)current)
	    : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int32_t encoding = (int32_t)kCFStringEncodingUTF8;
	CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &encoding);
	Boolean result = false;

	if (store != NULL && system != NULL && number != NULL) {
		CFDictionarySetValue(system, HAKIOS_NAME_FIELD, name);
		CFDictionarySetValue(system, HAKIOS_ENCODING_FIELD, number);
		result = store_set(store, HAKIOS_NAME_KEY, system);
	}
	if (number != NULL) CFRelease(number);
	if (system != NULL) CFRelease(system);
	if (current != NULL) CFRelease(current);
	if (store != NULL) CFRelease(store);
	return result;
}

int
main(int argc, char **argv)
{
	store_create = (store_create_fn)dlsym(RTLD_DEFAULT, "SCDynamicStoreCreate");
	store_copy = (store_copy_fn)dlsym(RTLD_DEFAULT, "SCDynamicStoreCopyValue");
	store_set = (store_set_fn)dlsym(RTLD_DEFAULT, "SCDynamicStoreSetValue");
	copy_name = (copy_name_fn)dlsym(RTLD_DEFAULT, "SCDynamicStoreCopyComputerName");
	if (!store_create || !store_copy || !store_set || !copy_name) {
		fprintf(stderr, "scname: SystemConfiguration dynamic-store symbols unavailable\n");
		return 1;
	}
	show_name();
	if (argc <= 1) {
		return 0;
	}

	char wanted[128] = "";
	for (int i = 1; i < argc; i++) {
		size_t used = strlen(wanted);
		if (used != 0 && used + 1 < sizeof(wanted)) wanted[used++] = ' ';
		if (used < sizeof(wanted)) snprintf(wanted + used, sizeof(wanted) - used, "%s", argv[i]);
	}
	CFStringRef name = CFStringCreateWithCString(NULL, wanted, kCFStringEncodingUTF8);
	prefs_create_fn prefs_create = (prefs_create_fn)dlsym(RTLD_DEFAULT, "SCPreferencesCreate");
	prefs_lock_fn prefs_lock = (prefs_lock_fn)dlsym(RTLD_DEFAULT, "SCPreferencesLock");
	prefs_set_name_fn prefs_set_name = (prefs_set_name_fn)dlsym(RTLD_DEFAULT, "SCPreferencesSetComputerName");
	prefs_commit_fn prefs_commit = (prefs_commit_fn)dlsym(RTLD_DEFAULT, "SCPreferencesCommitChanges");
	prefs_apply_fn prefs_apply = (prefs_apply_fn)dlsym(RTLD_DEFAULT, "SCPreferencesApplyChanges");
	prefs_unlock_fn prefs_unlock = (prefs_unlock_fn)dlsym(RTLD_DEFAULT, "SCPreferencesUnlock");
	Boolean preference_ok = false;
	SCPreferencesRef preferences = NULL;

	if (name != NULL && prefs_create && prefs_set_name && prefs_commit && prefs_apply) {
		preferences = prefs_create(NULL, CFSTR("hakios-scname"), NULL);
		if (preferences != NULL) {
			Boolean locked = prefs_lock ? prefs_lock(preferences, true) : true;
			Boolean set = prefs_set_name(preferences, name, kCFStringEncodingUTF8);
			Boolean committed = set ? prefs_commit(preferences) : false;
			Boolean applied = committed ? prefs_apply(preferences) : false;
			preference_ok = locked && set && committed && applied;
			printf("scname: preferences lock=%d set=%d commit=%d apply=%d\n", locked, set, committed, applied);
			if (locked && prefs_unlock) prefs_unlock(preferences);
			CFRelease(preferences);
		}
	}
	Boolean dynamic_ok = name != NULL ? set_dynamic_name(name) : false;
	printf("scname: write \"%s\" preferences=%d dynamic=%d\n", wanted, preference_ok, dynamic_ok);
	if (name != NULL) CFRelease(name);
	return preference_ok || dynamic_ok ? 0 : 1;
}
