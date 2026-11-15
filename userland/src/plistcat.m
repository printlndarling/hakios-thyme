/*
 * plistcat: print a plist as XML on stdout, for the console/kernel log.
 *
 *   plistcat /var/mobile/Library/Caches/locationd/clients.plist
 *
 * locationd keeps its per-client authorizations in a binary plist there; the
 * console log keeps text, so the binary is not readable in a run's log as it
 * is.  This is a bring-up aid, nothing else.
 */
#import <Foundation/Foundation.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSData *data;
		id plist;
		NSData *xml;
		BOOL is_directory = NO;
		NSString *path;

		if (argc < 2) {
			fprintf(stderr, "usage: plistcat FILE|DIRECTORY\n");
			return 2;
		}
		path = [NSString stringWithUTF8String:argv[1]];
		if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&is_directory] &&
		    is_directory) {
			NSArray<NSString *> *items;

			items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:NULL];
			if (items == nil) {
				fprintf(stderr, "plistcat: %s: cannot list\n", argv[1]);
				return 1;
			}
			for (NSString *item in [items sortedArrayUsingSelector:@selector(compare:)]) {
				printf("%s/%s\n", argv[1], item.UTF8String);
			}
			return 0;
		}
		data = [NSData dataWithContentsOfFile:path];
		if (data == nil) {
			fprintf(stderr, "plistcat: %s: cannot read\n", argv[1]);
			return 1;
		}
		plist = [NSPropertyListSerialization propertyListWithData:data
		    options:NSPropertyListImmutable format:NULL error:NULL];
		if (plist == nil) {
			fprintf(stderr, "plistcat: %s: not a plist (%lu bytes)\n", argv[1],
			    (unsigned long)data.length);
			return 1;
		}
		xml = [NSPropertyListSerialization dataWithPropertyList:plist
		    format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
		if (xml == nil) {
			fprintf(stderr, "plistcat: %s: cannot make XML\n", argv[1]);
			return 1;
		}
		fwrite(xml.bytes, 1, xml.length, stdout);
		fflush(stdout);
	}
	return 0;
}
