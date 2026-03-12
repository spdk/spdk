/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"

#include "util/cpuset.c"
#include "env_dpdk/init.c"

/* DPDK stubs */
DEFINE_STUB(rte_eal_init, int, (int argc, char **argv), 0);
DEFINE_STUB(rte_eal_cleanup, int, (void), 0);
DEFINE_STUB(rte_vfio_noiommu_is_enabled, int, (void), 0);
DEFINE_STUB(rte_version, const char *, (void), "DPDK test");

__thread int per_lcore__rte_errno;

/* env_internal stubs */
DEFINE_STUB(pci_env_init, int, (void), 0);
DEFINE_STUB_V(pci_env_reinit, (void));
DEFINE_STUB_V(pci_env_fini, (void));
DEFINE_STUB(mem_map_init, int, (bool legacy_mem), 0);
DEFINE_STUB_V(mem_map_fini, (void));
DEFINE_STUB(vtophys_init, int, (void), 0);
DEFINE_STUB_V(vtophys_fini, (void));
DEFINE_STUB_V(mem_disable_huge_pages, (void));
DEFINE_STUB_V(mem_disable_vtophys, (void));
DEFINE_STUB_V(mem_enforce_numa, (void));

/* OpenSSL stubs */
DEFINE_STUB(OPENSSL_INIT_new, OPENSSL_INIT_SETTINGS *, (void), NULL);
DEFINE_STUB_V(OPENSSL_INIT_free, (OPENSSL_INIT_SETTINGS *settings));
DEFINE_STUB(OPENSSL_init_ssl, int, (uint64_t opts, const OPENSSL_INIT_SETTINGS *settings), 1);
DEFINE_STUB_V(ERR_print_errors_fp, (FILE *fp));
#if OPENSSL_VERSION_NUMBER >= 0x30000000
DEFINE_STUB_V(OPENSSL_INIT_set_config_file_flags, (OPENSSL_INIT_SETTINGS *settings,
		unsigned long flags));
#endif

static void
test_coremask_to_corelist(void)
{
	char *list;

	/* Single core: "0x1" -> "0" */
	list = coremask_to_corelist("0x1");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0");
	free(list);

	/* First 4 cores: "0xF" -> "0-3" */
	list = coremask_to_corelist("0xF");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0-3");
	free(list);

	/* Cores 4-7: "0xF0" -> "4-7" */
	list = coremask_to_corelist("0xF0");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "4-7");
	free(list);

	/* Sparse cores: "0x15" (bits 0,2,4) -> "0,2,4" */
	list = coremask_to_corelist("0x15");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0,2,4");
	free(list);

	/* Default core mask: "0x1" -> "0" */
	list = coremask_to_corelist(SPDK_ENV_DPDK_DEFAULT_CORE_MASK);
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0");
	free(list);

	/* Large contiguous range: "0xFF00" -> "8-15" */
	list = coremask_to_corelist("0xFF00");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "8-15");
	free(list);

	/* Mixed ranges and singles: "0xFF0F" -> "0-3,8-15" */
	list = coremask_to_corelist("0xFF0F");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0-3,8-15");
	free(list);

	/* Two-core range: "0x3" -> "0-1" */
	list = coremask_to_corelist("0x3");
	SPDK_CU_ASSERT_FATAL(list != NULL);
	CU_ASSERT_STRING_EQUAL(list, "0-1");
	free(list);

	/* Invalid mask: NULL input */
	list = coremask_to_corelist(NULL);
	CU_ASSERT(list == NULL);

	/* Invalid mask: empty string */
	list = coremask_to_corelist("");
	CU_ASSERT(list == NULL);

	/* Invalid mask: not a hex number */
	list = coremask_to_corelist("xyz");
	CU_ASSERT(list == NULL);

	/* Zero mask: "0x0" -> NULL (no cores set) */
	list = coremask_to_corelist("0x0");
	CU_ASSERT(list == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("init", NULL, NULL);

	CU_ADD_TEST(suite, test_coremask_to_corelist);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
