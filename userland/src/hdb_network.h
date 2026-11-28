#ifndef HAKIOS_HDB_NETWORK_H
#define HAKIOS_HDB_NETWORK_H

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HDB_DIR "/redwood/etc/hdb"

static inline bool
hdb_private_ipv4(struct in_addr addr)
{
	uint32_t ip = ntohl(addr.s_addr);

	return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
}

static inline bool
hdb_wifi_address(struct in_addr peer, char *text, size_t capacity)
{
	struct ifaddrs *interfaces;
	bool ready = false;

	if (getifaddrs(&interfaces) < 0) {
		return false;
	}
	for (const struct ifaddrs *it = interfaces; it != NULL; it = it->ifa_next) {
		if (strcmp(it->ifa_name, "en0") != 0 || !(it->ifa_flags & IFF_UP) ||
		    it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || it->ifa_netmask == NULL) {
			continue;
		}
		struct in_addr local = ((const struct sockaddr_in *)it->ifa_addr)->sin_addr;
		uint32_t mask = ((const struct sockaddr_in *)it->ifa_netmask)->sin_addr.s_addr;

		if (mask != 0 && hdb_private_ipv4(local) && local.s_addr != peer.s_addr &&
		    ((local.s_addr ^ peer.s_addr) & mask) == 0) {
			ready = inet_ntop(AF_INET, &local, text, (socklen_t)capacity) != NULL;
			break;
		}
	}
	freeifaddrs(interfaces);
	return ready;
}

static inline bool
hdb_read_peer(struct in_addr *peer, char *text, size_t capacity)
{
	FILE *file = fopen(HDB_DIR "/peer", "r");
	if (file == NULL) {
		return false;
	}
	bool read = fgets(text, (int)capacity, file) != NULL;
	fclose(file);
	if (!read) {
		return false;
	}
	text[strcspn(text, "\r\n")] = 0;
	return inet_pton(AF_INET, text, peer) == 1 && hdb_private_ipv4(*peer);
}

#endif
