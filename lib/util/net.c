/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/net.h"
#include "spdk/log.h"

int
spdk_net_get_interface_name(const char *ip, char *ifc, size_t len)
{
	struct ifaddrs *addrs, *iap;
	struct sockaddr_in *sa;
	char buf[32];
	int rc = -ENODEV;

	getifaddrs(&addrs);
	for (iap = addrs; iap != NULL; iap = iap->ifa_next) {
		if (!(iap->ifa_addr && (iap->ifa_flags & IFF_UP) && iap->ifa_addr->sa_family == AF_INET)) {
			continue;
		}
		sa = (struct sockaddr_in *)(iap->ifa_addr);
		inet_ntop(iap->ifa_addr->sa_family, &sa->sin_addr, buf, sizeof(buf));
		if (strcmp(ip, buf) != 0) {
			continue;
		}
		if (strnlen(iap->ifa_name, len) == len) {
			rc = -ENOMEM;
			goto ret;
		}
		snprintf(ifc, len, "%s", iap->ifa_name);
		rc = 0;
		break;
	}
ret:
	freeifaddrs(addrs);
	return rc;
}

int
spdk_net_get_address_string(struct sockaddr *sa, char *addr, size_t len)
{
	const char *result = NULL;

	if (sa == NULL || addr == NULL) {
		return -EINVAL;
	}

	switch (sa->sa_family) {
	case AF_INET:
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   addr, len);
		break;
	case AF_INET6:
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   addr, len);
		break;
	default:
		break;
	}

	if (result == NULL) {
		return -errno;
	}

	return 0;
}

bool
spdk_net_is_loopback(int fd)
{
	struct ifaddrs *addrs, *tmp;
	struct sockaddr_storage sa = {};
	socklen_t salen;
	struct ifreq ifr = {};
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	bool is_loopback = false;

	salen = sizeof(sa);
	rc = getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	memset(ip_addr, 0, sizeof(ip_addr));
	rc = spdk_net_get_address_string((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	rc = getifaddrs(&addrs);
	if (rc != 0) {
		return is_loopback;
	}

	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = spdk_net_get_address_string(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				ioctl(fd, SIOCGIFFLAGS, &ifr);
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	freeifaddrs(addrs);
	return is_loopback;
}

int
spdk_net_getaddr(int fd, char *laddr, int llen, uint16_t *lport,
		 char *paddr, int plen, uint16_t *pport)
{
	struct sockaddr_storage sa;
	int val;
	socklen_t len;
	int rc;

	memset(&sa, 0, sizeof(sa));
	len = sizeof(sa);
	rc = getsockname(fd, (struct sockaddr *)&sa, &len);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		errno = EINVAL;
		return -1;
	}

	if (laddr) {
		rc = spdk_net_get_address_string((struct sockaddr *)&sa, laddr, llen);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_net_get_address_string() failed (errno=%d)\n", abs(rc));
			errno = abs(rc);
			return -1;
		}
	}

	if (lport) {
		if (sa.ss_family == AF_INET) {
			*lport = ntohs(((struct sockaddr_in *)&sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*lport = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
		}
	}

	len = sizeof(val);
	rc = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len);
	if (rc == 0 && val == 1) {
		/* It is an error to getaddr for a peer address on a listen socket. */
		if (paddr != NULL || pport != NULL) {
			SPDK_ERRLOG("paddr, pport not valid on listen sockets\n");
			errno = EINVAL;
			return -1;
		}
		return 0;
	}

	if (paddr || pport) {
		memset(&sa, 0, sizeof(sa));
		len = sizeof(sa);
		rc = getpeername(fd, (struct sockaddr *)&sa, &len);
		if (rc != 0) {
			SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
			return -1;
		}
	}

	if (paddr) {
		rc = spdk_net_get_address_string((struct sockaddr *)&sa, paddr, plen);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_net_get_address_string() failed (errno=%d)\n", abs(rc));
			errno = abs(rc);
			return -1;
		}
	}

	if (pport) {
		if (sa.ss_family == AF_INET) {
			*pport = ntohs(((struct sockaddr_in *)&sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*pport = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
		}
	}

	return 0;
}
