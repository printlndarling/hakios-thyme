/*
 * mcm_test ID: ask containermanagerd's MobileContainerManager for an app container, the way
 * TrollStore does when it installs an IPA ("MCMAppContainer containerWithIdentifier:
 * createIfNecessary:existed:error:").  This says whether containers exist on this port at all,
 * which is what TrollStore's install path stands on; the class is private and has no headers, so
 * it is called through the runtime.  Everything goes to stdout, which a scene's run log carries.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>

typedef id (*ContainerFn)(id, SEL, NSString *, BOOL, BOOL *, NSError **);

static void
describe_class(Class cls)
{
	unsigned count = 0;
	Method *methods = class_copyMethodList(object_getClass(cls), &count);

	printf("class methods of %s (%u):\n", class_getName(cls), count);
	for (unsigned i = 0; i < count && i < 40; i++) {
		printf("  +%s\n", sel_getName(method_getName(methods[i])));
	}
	free(methods);
}

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		NSString *identifier = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : @"com.hakios.mcmtest";
		Class cls = objc_getClass("MCMAppContainer");
		SEL selector = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");

		printf("mcm_test: identifier %s\n", identifier.UTF8String);
		if (cls == NULL) {
			/* The class is only in the process once its image is loaded; a program that
			 * merely looks it up has not linked the private framework, so load it by path. */
			const char *path = "/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager";
			void *handle = dlopen(path, RTLD_NOW);

			printf("mcm_test: dlopen %s -> %s\n", path, handle != NULL ? "ok" : dlerror());
			cls = objc_getClass("MCMAppContainer");
		}
		if (cls == NULL) {
			printf("mcm_test: MCMAppContainer is not there\n");
			return 1;
		}
		printf("mcm_test: MCMAppContainer found\n");
		if (!class_respondsToSelector(object_getClass((id)cls), selector)) {
			printf("mcm_test: the class method is not there\n");
			describe_class(cls);
			return 2;
		}

		BOOL existed = NO;
		NSError *error = nil;
		ContainerFn fn = (ContainerFn)[(id)cls methodForSelector:selector];
		id container = fn((id)cls, selector, identifier, YES, &existed, &error);

		printf("mcm_test: container %s (existed %d)\n", container != nil ? "created" : "nil", existed);
		if (error != nil) {
			printf("mcm_test: error: %s\n", error.description.UTF8String);
		}
		if (container != nil) {
			printf("mcm_test: object: %s\n", [container description].UTF8String);
			SEL urlSelector = sel_registerName("url");

			if ([container respondsToSelector:urlSelector]) {
				id url = ((id (*)(id, SEL))[container methodForSelector:urlSelector])(container, urlSelector);

				printf("mcm_test: url: %s\n", [url description].UTF8String);
			} else {
				printf("mcm_test: -url is not there\n");
			}
			return 0;
		}
	}
	return 3;
}
