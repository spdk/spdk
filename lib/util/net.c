/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/net.h"

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
