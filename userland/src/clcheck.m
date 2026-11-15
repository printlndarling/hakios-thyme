/*
 * clcheck: whether the class Settings names in its privacy plist can be found in this
 * userland at all, and whether a bundle load of the framework that carries it works the
 * way Settings would do it.  Preferences drops a privacy row whose detail controller class
 * does not resolve, and the Location Services row is the one row the Privacy & Security
 * page leaves out -- the others in the same plist are shown.  The framework lives only in
 * the dyld shared cache here: its executable is not on the system volume, so this asks
 * both ways -- dlopen by path and [NSBundle load] as a settings bundle would -- and says
 * whether the executable file exists (a NSBundle that insists on the file would fail
 * where a dlopen does not).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>

int
main(void)
{
	@autoreleasepool {
		const char *framework = "/System/Library/PrivateFrameworks/Settings/PrivacySettingsUI.framework";
		const char *path = "/System/Library/PrivateFrameworks/Settings/PrivacySettingsUI.framework/PrivacySettingsUI";
		const char *names[] = {
			"PUILocationServicesListController",
			"PUILocationServicesCell",
			"PUILocationServicesAuthLevelCell",
			"PUITCCAccessController",
			"PUITrackersController",
		};
		NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:framework]];
		NSError *error = nil;
		BOOL loaded;

		if (bundle == nil) {
			printf("clcheck: bundleWithPath: nil\n");
		} else {
			NSString *executable = [bundle executablePath];

			printf("clcheck: NSBundle executable: %s\n",
			    executable != nil ? executable.UTF8String : "(none)");
			printf("clcheck: NSBundle executable file: %s\n",
			    executable != nil && [[NSFileManager defaultManager] fileExistsAtPath:executable]
			    ? "present" : "MISSING");
			loaded = [bundle loadAndReturnError:&error];
			printf("clcheck: NSBundle load: %d%s%s\n", loaded,
			    loaded || error == nil ? "" : " ",
			    loaded || error == nil ? "" : error.localizedDescription.UTF8String);
		}
		void *h = dlopen(path, RTLD_NOW);

		printf("clcheck: dlopen PrivacySettingsUI: %s\n", h != NULL ? "ok" : dlerror());
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			Class c = NSClassFromString([NSString stringWithUTF8String:names[i]]);

			printf("clcheck: %s: %s\n", names[i], c != NULL ? "found" : "nil");
		}
		fflush(stdout);
	}
	return 0;
}
