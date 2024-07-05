/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "util/file.c"

static void
_read_sysfs_attribute(void)
{
	/* Don't try to use real sysfs paths for the unit test. Instead
	 * simulate sysfs attributes with some temporary files.
	 */
	const char *path = "/tmp/spdk_file_ut_2024";
	const char *setup = "spdk_unit_tests\n";
	char *attr = NULL;
	FILE *f;
	int rc;

	f = fopen(path, "w");
	SPDK_CU_ASSERT_FATAL(f != NULL);

	rc = fwrite(setup, strlen(setup) + 1, 1, f);
	CU_ASSERT(rc == 1);

	rc = fclose(f);
	CU_ASSERT(rc == 0);

	rc = spdk_read_sysfs_attribute(&attr, "/tmp/spdk_file_ut_%d", 2024);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(attr != NULL);
	CU_ASSERT(strncmp(setup, attr, strlen(setup) - 1) == 0);
	free(attr);

	rc = spdk_read_sysfs_attribute(&attr, "/tmp/some_non_existent_file");
	CU_ASSERT(rc == -ENOENT);
}

static void
read_sysfs_attribute_uint32(void)
{
	/* Don't try to use real sysfs paths for the unit test. Instead
	 * simulate sysfs attributes with some temporary files.
	 */
	const char *path = "/tmp/spdk_file_ut_2024";
	const char *setup = "111\n";
	uint32_t attr;
	FILE *f;
	int rc;

	f = fopen(path, "w");
	SPDK_CU_ASSERT_FATAL(f != NULL);
	rc = fwrite(setup, strlen(setup) + 1, 1, f);
	CU_ASSERT(rc == 1);
	rc = fclose(f);
	CU_ASSERT(rc == 0);

	rc = spdk_read_sysfs_attribute_uint32(&attr, "/tmp/spdk_file_ut_%d", 2024);
	CU_ASSERT(rc == 0);
	CU_ASSERT(attr == 111);

	setup = "0xFFFFFFFF\n";
	f = fopen(path, "w");
	SPDK_CU_ASSERT_FATAL(f != NULL);
	rc = fwrite(setup, strlen(setup) + 1, 1, f);
	CU_ASSERT(rc == 1);
	rc = fclose(f);
	CU_ASSERT(rc == 0);

	rc = spdk_read_sysfs_attribute_uint32(&attr, "/tmp/spdk_file_ut_%d", 2024);
	CU_ASSERT(rc == 0);
	CU_ASSERT(attr == UINT32_MAX);

	/* Write a value larger than UINT32_MAX */
	setup = "0x100000000\n";
	f = fopen(path, "w");
	SPDK_CU_ASSERT_FATAL(f != NULL);
	rc = fwrite(setup, strlen(setup) + 1, 1, f);
	CU_ASSERT(rc == 1);
	rc = fclose(f);
	CU_ASSERT(rc == 0);

	rc = spdk_read_sysfs_attribute_uint32(&attr, "/tmp/spdk_file_ut_%d", 2024);
	CU_ASSERT(rc == -EINVAL);

	/* Write a negative number */
	setup = "-1\n";
	f = fopen(path, "w");
	SPDK_CU_ASSERT_FATAL(f != NULL);
	rc = fwrite(setup, strlen(setup) + 1, 1, f);
	CU_ASSERT(rc == 1);
	rc = fclose(f);
	CU_ASSERT(rc == 0);

	rc = spdk_read_sysfs_attribute_uint32(&attr, "/tmp/spdk_file_ut_%d", 2024);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_read_sysfs_attribute_uint32(&attr, "/tmp/some_non_existent_file");
	CU_ASSERT(rc == -ENOENT);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("file", NULL, NULL);

	CU_ADD_TEST(suite, _read_sysfs_attribute);
	CU_ADD_TEST(suite, read_sysfs_attribute_uint32);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
