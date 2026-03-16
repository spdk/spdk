/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "util/net.c"

static void
get_interface_name(void)
{
	char ifc[32];
	int rc;

	rc = spdk_net_get_interface_name("127.0.0.1", ifc, sizeof(ifc));
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(ifc, "lo") == 0);

	/* Verify that an invalid IP address returns -ENODEV. */
	rc = spdk_net_get_interface_name("99.99.99.99", ifc, sizeof(ifc));
	CU_ASSERT(rc == -ENODEV);

	/* Verify that an insufficient output string length returns -ENOMEM. */
	rc = spdk_net_get_interface_name("127.0.0.1", ifc, 2);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_spdk_net_compare_ip_address(void)
{
	int cmp;

	/* Test invalid input */
	CU_ASSERT(spdk_net_compare_address(AF_INET, NULL, NULL, &cmp) == -EINVAL);
	CU_ASSERT(spdk_net_compare_address(AF_INET, "127.0.0.1", "127.0.0.1", NULL) == -EINVAL);
	CU_ASSERT(spdk_net_compare_address(AF_INET, "invalid", "invalid", &cmp) == -EINVAL);
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "invalid", "invalid", &cmp) == -EINVAL);
	CU_ASSERT(spdk_net_compare_address(AF_UNIX, "127.0.0.1", "127.0.0.1", &cmp) == -EAFNOSUPPORT);

	/* Test IPv4 addresses */
	CU_ASSERT(spdk_net_compare_address(AF_INET, "127.0.0.1", "127.0.0.1", &cmp) == 0);
	CU_ASSERT(cmp == 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET, "127.0.0.1", "127.0.0.2", &cmp) == 0);
	CU_ASSERT(cmp < 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET, "127.0.0.2", "127.0.0.1", &cmp) == 0);
	CU_ASSERT(cmp > 0);

	/* Test IPv6 addresses */
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "::1", "::1", &cmp) == 0);
	CU_ASSERT(cmp == 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "0:0:0:0:0:0:0:1", "::1", &cmp) == 0);
	CU_ASSERT(cmp == 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "0000:0000:0000:0000:0000:0000:0000:0001",
					   "::1", &cmp) == 0);
	CU_ASSERT(cmp == 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "::1", "::2", &cmp) == 0);
	CU_ASSERT(cmp < 0);
	CU_ASSERT(spdk_net_compare_address(AF_INET6, "::2", "::1", &cmp) == 0);
	CU_ASSERT(cmp > 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("net", NULL, NULL);

	CU_ADD_TEST(suite, get_interface_name);
	CU_ADD_TEST(suite, test_spdk_net_compare_ip_address);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
