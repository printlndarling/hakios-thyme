/*
 * hk_download URL PATH [SHA256]: fetch one URL into a file, the smallest useful downloader for
 * this port.
 *
 * Why it exists: the root image travels beside the kernel in the loader's payload, which tops out
 * around 50 MB -- a 5.5 MB IPA added to the daily image failed to upload ("xl: upload failed",
 * 2026-11-18).  The JB prefix in the system image has no wget or curl either, so a scene that
 * must get an IPA onto the phone (TrollApps from a machine on the LAN, say) has no tool.  This is
 * that tool: NSURLSession, one URL, one file, the HTTP status and the byte count printed.
 *
 * The optional third word is the digest the file must have (the value devices/thyme/ipas.md
 * records for each app): the tool prints what it computed and returns 4 on a mismatch, so an
 * install scene stops before handing a wrong or truncated file to TrollStore's helper.
 *
 * usage: hk_download URL PATH [SHA256]
 */
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		if (argc < 3) {
			printf("usage: hk_download URL PATH [SHA256]\n");
			return 2;
		}
		NSString *address = [NSString stringWithUTF8String:argv[1]];
		NSString *path = [NSString stringWithUTF8String:argv[2]];
		NSString *expected = argc > 3 ? [NSString stringWithUTF8String:argv[3]].lowercaseString : nil;
		NSURL *url = [NSURL URLWithString:address];
		__block BOOL done = NO;
		__block long status = 0;
		__block unsigned long long written = 0;
		__block NSString *failure = nil;
		__block NSString *digest = nil;
		__block BOOL mismatch = NO;

		if (url == nil) {
			printf("hk_download: bad URL %s\n", argv[1]);
			return 2;
		}
		NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithURL:url
		    completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
			if (error != nil) {
				failure = error.localizedDescription;
			} else if (data != nil) {
				unsigned char md[CC_SHA256_DIGEST_LENGTH];
				NSMutableString *hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];

				CC_SHA256(data.bytes, (CC_LONG)data.length, md);
				for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
					[hex appendFormat:@"%02x", md[i]];
				}
				digest = hex;
				if (expected != nil && ![digest isEqualToString:expected]) {
					mismatch = YES;
				}
				if ([data writeToFile:path atomically:YES]) {
					written = data.length;
				} else {
					failure = @"cannot write the file";
				}
				if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
					status = [(NSHTTPURLResponse *)response statusCode];
				}
			}
			done = YES;
		}];

		[task resume];
		while (!done) {
			[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
			    beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
		}
		if (failure != nil) {
			printf("hk_download: %s: %s\n", address.UTF8String, failure.UTF8String);
			return 3;
		}
		printf("hk_download: %s -> %s, %llu bytes (http %ld)\n", address.UTF8String,
		    path.UTF8String, written, status);
		if (expected != nil) {
			printf("hk_download: sha256 %s (expected %s)%s\n", digest.UTF8String ?: "-",
			    expected.UTF8String, mismatch ? " MISMATCH" : " ok");
		}
		if (mismatch) {
			return 4;
		}
	}
	return 0;
}
