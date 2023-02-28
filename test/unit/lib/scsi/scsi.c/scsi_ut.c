/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/scsi.h"

#include "spdk_cunit.h"

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

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("scsi_suite", NULL, NULL);

	CU_ADD_TEST(suite, scsi_init);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
