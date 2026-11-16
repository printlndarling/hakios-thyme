/*
 * https_test: one URL over NSURLSession, with the answer, the error's domain
 * and code, and how long it took.
 *
 * The Weather app's fetches to api.open-meteo.com time out
 * (NSURLErrorDomain -1001) while mDNSResponder resolves the name in 0.01 s
 * and an IPv4 TCP connect to port 443 takes 0.26 s (net_test, logs/
 * 201124-run.log): the stall is above TCP.  This asks the same question from
 * a plain process, outside the application's sandbox and without the crash
 * logger's URLSessionTask swizzle, so the next scene can say whether the
 * stall is the system's TLS or the app's.
 *
 *   https_test URL [seconds]
 */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		NSString *url;
		double timeout;
		NSURLRequest *request;
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block NSString *result = @"no answer";
		NSDate *start = [NSDate date];
		NSURLSessionDataTask *task;

		if (argc < 2) {
			fprintf(stderr, "usage: https_test URL [seconds]\n");
			return 2;
		}
		url = [NSString stringWithUTF8String:argv[1]];
		timeout = argc > 2 ? atof(argv[2]) : 10.0;
		request = [NSURLRequest requestWithURL:[NSURL URLWithString:url]
		    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:timeout];
		task = [[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData *data,
		    NSURLResponse *response, NSError *error) {
			NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
			    (NSInteger)[(NSHTTPURLResponse *)response statusCode] : -1;

			result = [NSString stringWithFormat:@"status %ld, %lu bytes, domain %@ code %ld",
			    (long)status, (unsigned long)data.length,
			    error != nil ? error.domain : @"none", error != nil ? (long)error.code : 0];
			dispatch_semaphore_signal(done);
		}];
		[task resume];
		dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)((timeout + 5.0) * NSEC_PER_SEC)));
		printf("https_test: %s -> %s (%.1f s)\n", [url UTF8String], [result UTF8String],
		    -[start timeIntervalSinceNow]);
	}
	return 0;
}
