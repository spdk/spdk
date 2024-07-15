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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("net", NULL, NULL);

	CU_ADD_TEST(suite, get_interface_name);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
