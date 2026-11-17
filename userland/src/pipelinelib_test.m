/*
 * pipelinelib_test: what this Darwin's Metal does with a pipeline library it has no compiled pipelines for.
 *
 * Maps draws nothing because VectorKit builds its render pipelines through
 * /System/Library/PrivateFrameworks/VectorKit.framework/iphoneos.pipelinelib, and the library has no
 * compiled pipeline file for this GPU (`pipelines.Adreno(hakios-metal,chip0x6050002)`), its `functions`
 * archive is 0 bytes, and Metal refuses the whole library ("Could not open function cache file") before
 * any pipeline is asked for (logs/061230-run.log).  The plug-in answers that by giving the mirror an
 * empty LPPM function cache (HakiosMetalCA.m, -newPipelineLibraryWithFilePath:error:); this asks Metal
 * what it makes of the result, one step at a time, so that a nil library can be told from a library
 * whose pipelines are nil, and from a pipeline whose compilation is refused.
 *
 * It is a program, not part of any daemon: the window server's boot must not depend on a diagnostic.
 *
 *   usage: pipelinelib_test
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <stdio.h>

#define VK_PIPELINELIB "/System/Library/PrivateFrameworks/VectorKit.framework/iphoneos.pipelinelib"
#define VK_METALLIB    "/redwood/share/ca-metallibs/System/Library/PrivateFrameworks/VectorKit.framework/default.metallib"

/* the same private interfaces the plug-in uses (HakiosMetalCA.m) */
@interface _MTLDevice : NSObject
@end

@interface _MTLDevice (HakiosPipelineTest)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error;
@end

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* the pipeline library's own descriptions: section 1 is the pipeline names, each entry a 16-byte header and
 * a NUL-terminated string (HakiosMetalCA.m reads CoreAnimation's the same way) */
static NSArray<NSString *> *
desc_names(const char *path)
{
	NSData *desc = [NSData dataWithContentsOfFile:@(path)];
	const uint8_t *d = [desc bytes];
	NSUInteger n = [desc length];
	NSMutableArray *names = [NSMutableArray array];

	printf("pipelinelib_test: pipelines.desc %s (%lu bytes)\n", path, (unsigned long)n);
	if (desc == nil || n < 8) {
		return names;
	}
	uint32_t sections = le32(d);
	if (sections > 16 || 4 + 4 * sections > n) {
		return names;
	}
	uint32_t off1 = le32(d + 4 + 4);
	if (off1 + 8 > n) {
		return names;
	}
	uint32_t count = le32(d + off1);
	printf("pipelinelib_test: %u sections, section 1 names %u pipelines\n", sections, count);
	for (uint32_t i = 0; i < count && off1 + 8 + 4 * i <= n; i++) {
		uint32_t r = le32(d + off1 + 4 + 4 * i);
		if (off1 + r + 16 < n) {
			const char *s = (const char *)d + off1 + r + 16;
			size_t len = strnlen(s, n - off1 - r - 16);
			NSString *name = [[NSString alloc] initWithBytes:s length:len encoding:NSUTF8StringEncoding];

			[names addObject:name != nil ? name : @""];
		}
	}
	return names;
}

static void
stat_file(const char *path)
{
	NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:@(path) error:NULL];

	printf("pipelinelib_test: %s: %s%s\n", path, attrs != nil ? "present" : "MISSING",
	       attrs != nil ? [[NSString stringWithFormat:@", %@ bytes", attrs[NSFileSize]] UTF8String] : "");
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	printf("pipelinelib_test: device %s\n", device != nil ? [[device name] UTF8String] : "(nil)");
	if (device == nil) {
		return 1;
	}
	stat_file(VK_PIPELINELIB "/pipelines.desc");
	stat_file(VK_PIPELINELIB "/functions");
	stat_file(VK_METALLIB);

	NSError *error = nil;
	id library = [(id)device newPipelineLibraryWithFilePath:@VK_PIPELINELIB error:&error];
	printf("pipelinelib_test: newPipelineLibraryWithFilePath = %s%s\n", library != nil ? "a library" : "NIL",
	       error != nil ? [[NSString stringWithFormat:@" (%@)", [error localizedDescription]] UTF8String] : "");

	NSArray *names = desc_names(VK_PIPELINELIB "/pipelines.desc");
	for (NSUInteger i = 0; i < [names count] && i < 6; i++) {
		printf("pipelinelib_test:   name %lu: %s\n", (unsigned long)i, [names[i] UTF8String]);
	}
	if (library != nil && [names count] > 0) {
		SEL byName = sel_registerName("newRenderPipelineStateWithName:options:reflection:error:");
		NSArray *pipelineNames = nil;
		if ([library respondsToSelector:sel_registerName("pipelineNames")]) {
			pipelineNames = ((id (*)(id, SEL))objc_msgSend)(library, sel_registerName("pipelineNames"));
		}
		printf("pipelinelib_test: the library is %s, %s newRenderPipelineStateWithName:options:reflection:error:, "
		       "and pipelineNames says %lu\n", object_getClassName(library),
		       [library respondsToSelector:byName] ? "answers" : "does NOT answer",
		       (unsigned long)[pipelineNames count]);
		for (NSUInteger i = 0; i < [pipelineNames count] && i < 8; i++) {
			NSString *name = pipelineNames[i];

			printf("pipelinelib_test:   pipelineNames[%lu] = %s\n", (unsigned long)i,
			       [name isKindOfClass:[NSString class]] ? [name UTF8String] : "<not a string>");
		}
		for (NSUInteger i = 0; i < [pipelineNames count] && i < 6 && [library respondsToSelector:byName]; i++) {
			NSString *name = [pipelineNames[i] isKindOfClass:[NSString class]] ? pipelineNames[i] : nil;
			NSError *why = nil;
			id pipeline;

			if (name == nil) {
				continue;
			}
			pipeline = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))objc_msgSend)(library, byName,
			    name, 0, NULL, &why);
			printf("pipelinelib_test:   pipeline %s = %s%s\n", [name UTF8String],
			       pipeline != nil ? object_getClassName(pipeline) : "NIL",
			       why != nil ? [[NSString stringWithFormat:@" (%@)", [why localizedDescription]] UTF8String] : "");
		}
		/* one name the descriptions do list, so that the plug-in's answer to the library's own lookups is
		 * exercised even when pipelineNames is empty (HakiosMetalCA.m, hm_vk_new_pipeline) */
		if ([library respondsToSelector:byName] && [names count] > 0) {
			NSError *why = nil;
			id pipeline = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))objc_msgSend)(library, byName,
			    names[0], 0, NULL, &why);

			printf("pipelinelib_test:   by description name %s = %s%s\n", [names[0] UTF8String],
			       pipeline != nil ? object_getClassName(pipeline) : "NIL",
			       why != nil ? [[NSString stringWithFormat:@" (%@)", [why localizedDescription]] UTF8String] : "");
		}
	}

	NSError *libError = nil;
	id<MTLLibrary> functions = [device newLibraryWithFile:@VK_METALLIB error:&libError];
	printf("pipelinelib_test: VectorKit's default.metallib as a library = %s%s\n",
	       functions != nil ? object_getClassName(functions) : "NIL",
	       libError != nil ? [[NSString stringWithFormat:@" (%@)", [libError localizedDescription]] UTF8String] : "");
	if (functions != nil) {
		static const char *const wanted[] = { "DaVinci::ground_base_vertex", "Grid::grid_fragment", "DaVinci::ground_fragment" };
		for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
			id function = [functions newFunctionWithName:@(wanted[i])];
			printf("pipelinelib_test:   function %s = %s", wanted[i], function != nil ? "found" : "NOT FOUND");
			if (function != nil && [function respondsToSelector:sel_registerName("bitcodeData")]) {
				NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, sel_registerName("bitcodeData"));
				printf(", %lu bytes of AIR", (unsigned long)[air length]);
			}
			printf("\n");
		}
		/* would a pipeline built from the library's own functions reach this runtime's compiler at all: a grid
		 * pipeline with nothing special (no vertex descriptor, one BGRA8 target, no depth) */
		id vertex = [functions newFunctionWithName:@"Grid::grid_vertex"];
		id fragment = [functions newFunctionWithName:@"Grid::grid_fragment"];
		if (vertex != nil && fragment != nil) {
			MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
			NSError *pipelineError = nil;
			id pipeline;

			descriptor.vertexFunction = vertex;
			descriptor.fragmentFunction = fragment;
			descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			descriptor.label = @"pipelinelib_test grid";
			pipeline = [device newRenderPipelineStateWithDescriptor:descriptor options:0 reflection:NULL error:&pipelineError];
			printf("pipelinelib_test: a Grid pipeline from the functions = %s%s\n",
			       pipeline != nil ? object_getClassName(pipeline) : "NIL",
			       pipelineError != nil ? [[NSString stringWithFormat:@" (%@)", [pipelineError localizedDescription]] UTF8String] : "");
		}
		/* and Metal's own private lookup, the one the pipeline-library builder takes (tools/m2v/adrenometal-ios16.patch):
		 * with an empty function cache, does it still answer from the library? */
		SEL withCache = sel_registerName("newFunctionWithName:constantValues:functionCache:error:");
		if ([functions respondsToSelector:withCache]) {
			@try {
				MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
				NSError *cacheError = nil;
				id function = ((id (*)(id, SEL, id, id, id, NSError **))objc_msgSend)(functions, withCache,
				    @"Grid::grid_vertex", values, nil, &cacheError);

				printf("pipelinelib_test: newFunctionWithName:constantValues:functionCache:error: (cache nil) = %s%s\n",
				       function != nil ? "a function" : "NIL",
				       cacheError != nil ? [[NSString stringWithFormat:@" (%@)", [cacheError localizedDescription]] UTF8String] : "");
			} @catch (NSException *exception) {
				printf("pipelinelib_test: newFunctionWithName:constantValues:functionCache:error: raised %s: %s\n",
				       [[exception name] UTF8String], [[exception reason] UTF8String]);
			}
		} else {
			printf("pipelinelib_test: the library does not answer newFunctionWithName:constantValues:functionCache:error:\n");
		}
	}
	printf("pipelinelib_test: done\n");
	return 0;
}
