/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/scsi.h"

#include "spdk_internal/cunit.h"

#include "scsi/scsi.c"

static void
scsi_init(void)
{
	int rc;

	rc = spdk_scsi_init();
	CU_ASSERT_EQUAL(rc, 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("scsi_suite", NULL, NULL);

	CU_ADD_TEST(suite, scsi_init);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
