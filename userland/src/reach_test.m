/*
 * reach_test: what SystemConfiguration thinks of the network, next to what the sockets do.
 *
 * locationd skips its ALS (Wi-Fi positioning) query with "@AlsFlow, query, skip, connectivity"
 * on this port although the phone's Wi-Fi carries HTTPS -- so the question is whether
 * SystemConfiguration (whose reachability configd answers) agrees with the sockets.  This asks
 * SCNetworkReachability for a host and for the default address, and then makes the same
 * connection by hand with getaddrinfo and connect, so a disagreement is visible in one line
 * each.  (SCDynamicStore's API is marked unavailable on iOS, so whether configd itself runs is
 * left to /bin/ps beside this in the scene.)
 *
 *   reach_test        (RC_JOBS: a line /redwood/bin/reach_test, after the Wi-Fi link is up)
 */
#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void
show_reach(const char *what, SCNetworkReachabilityRef reach)
{
	SCNetworkReachabilityFlags flags = 0;
	Boolean ok;

	if (reach == NULL) {
		printf("reach_test: %s: no reachability ref\n", what);
		return;
	}
	ok = SCNetworkReachabilityGetFlags(reach, &flags);
	printf("reach_test: %s: flags %s %#x (reachable %d, direct %d, wwan %d, on-demand %d)\n",
	    what, ok ? "ok" : "FAILED", (unsigned)flags,
	    !!(flags & kSCNetworkReachabilityFlagsReachable),
	    !!(flags & kSCNetworkReachabilityFlagsIsDirect),
	    !!(flags & kSCNetworkReachabilityFlagsIsWWAN),
	    !!(flags & kSCNetworkReachabilityFlagsConnectionOnDemand));
}

static void
try_connect(const char *host, const char *port)
{
	struct addrinfo hints, *list = NULL;
	int error = getaddrinfo(host, port, &(struct addrinfo){ .ai_family = AF_UNSPEC,
	    .ai_socktype = SOCK_STREAM }, &list);

	if (error != 0 || list == NULL) {
		printf("reach_test: connect %s:%s: getaddrinfo: %s\n", host, port, gai_strerror(error));
		return;
	}
	int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
	struct timeval timeout = { .tv_sec = 8 };

	if (fd < 0) {
		printf("reach_test: connect %s:%s: socket: %s\n", host, port, strerror(errno));
	} else {
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		int r = connect(fd, list->ai_addr, list->ai_addrlen);
		char address[64] = "?";

		if (list->ai_family == AF_INET) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)list->ai_addr)->sin_addr, address, sizeof(address));
		} else if (list->ai_family == AF_INET6) {
			inet_ntop(AF_INET6, &((struct sockaddr_in6 *)list->ai_addr)->sin6_addr, address, sizeof(address));
		}
		printf("reach_test: connect %s:%s (%s): %s\n", host, port, address,
		    r == 0 ? "connected" : strerror(errno));
		close(fd);
	}
	freeaddrinfo(list);
}

int
main(void)
{
	@autoreleasepool {
		SCNetworkReachabilityRef host = SCNetworkReachabilityCreateWithName(NULL, "gs-loc.apple.com");
		struct sockaddr_in any = { .sin_len = sizeof(any), .sin_family = AF_INET, .sin_addr.s_addr = 0 };
		SCNetworkReachabilityRef def = SCNetworkReachabilityCreateWithAddress(NULL,
		    (const struct sockaddr *)&any);

		show_reach("gs-loc.apple.com", host);
		show_reach("0.0.0.0", def);
		try_connect("gs-loc.apple.com", "443");
		try_connect("apt.procurs.us", "443");
		fflush(stdout);
	}
	return 0;
}
