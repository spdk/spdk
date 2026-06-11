/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/test_env.c"
#include "spdk/stdinc.h"
#include "spdk/dma.h"

/*
 * PCI stubs so ntb_amd.c can be linked into this TU without env_dpdk.
 *
 * spdk_pci_ntb_amd_get_driver() is provided by the shared
 * common/lib/test_env.c (alongside the nvme/ioat/ae4dma/virtio
 * counterparts), so we don't redefine it here.
 *
 * The current tests exercise pure argument-validation paths and never
 * dispatch through these stubs; the unit binary only needs the symbols to
 * resolve. DEFINE_STUB / DEFINE_STUB_V also expose ut_<fn>_mocked hooks if
 * future tests want to override return values.
 *
 * Note: DEFINE_STUB does not write to caller-provided OUT parameters
 * (e.g. spdk_pci_device_map_bar's mapped_addr/phys_addr/size). That is
 * fine here because no test currently follows a path that consumes them;
 * if a future test does, mock the return value to the failure case so the
 * caller bails before reading the OUT params.
 */
DEFINE_STUB(spdk_pci_enumerate, int,
	    (struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx), 0)
DEFINE_STUB(spdk_pci_device_map_bar, int,
	    (struct spdk_pci_device *dev, uint32_t bar, void **mapped_addr,
	     uint64_t *phys_addr, uint64_t *size), -1)
DEFINE_STUB(spdk_pci_device_unmap_bar, int,
	    (struct spdk_pci_device *dev, uint32_t bar, void *addr), 0)
DEFINE_STUB(spdk_pci_device_cfg_read8, int,
	    (struct spdk_pci_device *dev, uint8_t *value, uint32_t offset), 0)
DEFINE_STUB(spdk_pci_device_cfg_read32, int,
	    (struct spdk_pci_device *dev, uint32_t *value, uint32_t offset), 0)
DEFINE_STUB(spdk_pci_device_cfg_write32, int,
	    (struct spdk_pci_device *dev, uint32_t value, uint32_t offset), 0)
DEFINE_STUB_V(spdk_pci_for_each_device,
	      (void *ctx, void (*fn)(void *ctx, struct spdk_pci_device *dev)))
DEFINE_STUB(spdk_pci_device_get_addr, struct spdk_pci_addr,
(struct spdk_pci_device *dev), ((struct spdk_pci_addr) {0}))
DEFINE_STUB_V(spdk_pci_device_detach, (struct spdk_pci_device *dev))
DEFINE_STUB(spdk_memory_domain_create, int,
	    (struct spdk_memory_domain **domain, enum spdk_dma_device_type type,
	     struct spdk_memory_domain_ctx *ctx, const char *id), 0)
DEFINE_STUB_V(spdk_memory_domain_destroy, (struct spdk_memory_domain *domain))

#include "ntb_amd/ntb_amd.c"

static void
ntb_set_active_mw_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};

	ntb.mw_cnt = 4;

	CU_ASSERT_EQUAL(spdk_ntb_amd_set_active_mw(NULL, 0), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_set_active_mw(&ntb, 4), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_set_active_mw(&ntb, 200), -EINVAL);

	CU_ASSERT_EQUAL(spdk_ntb_amd_set_active_mw(&ntb, 0), 0);
	CU_ASSERT_EQUAL(ntb.active_mw_idx, 0);
	CU_ASSERT_EQUAL(spdk_ntb_amd_set_active_mw(&ntb, 3), 0);
	CU_ASSERT_EQUAL(ntb.active_mw_idx, 3);
}

static void
ntb_attr_get_invalid_args_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t v;

	CU_ASSERT_EQUAL(spdk_ntb_amd_attr_get(NULL, SPDK_NTB_TOPO_NAME, &v), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_attr_get(&ntb, NULL, &v), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_attr_get(&ntb, SPDK_NTB_TOPO_NAME, NULL), -EINVAL);
}

static void
ntb_attr_get_topo_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t v;

	ntb.topo = NTB_TOPO_SEC;
	CU_ASSERT_EQUAL(spdk_ntb_amd_attr_get(&ntb, SPDK_NTB_TOPO_NAME, &v), 0);
	CU_ASSERT_EQUAL(v, NTB_TOPO_SEC);
}

static void
ntb_get_peer_mw_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t sizes[2] = { 4096, 8192 };
	uint64_t dummy_window;
	struct iovec iov = {};
	void *vaddr = NULL;

	ntb.mw_cnt = 2;
	ntb.mw_size = sizes;
	ntb.mw_vaddr[1] = &dummy_window;
	ntb.mw_paddr[1] = 0x1000;

	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(&ntb, 1, &iov, NULL, &vaddr), 0);
	CU_ASSERT_EQUAL(iov.iov_len, 8192);
	CU_ASSERT_EQUAL((uint64_t)(uintptr_t)iov.iov_base, 0x1000);
	CU_ASSERT_PTR_EQUAL(vaddr, &dummy_window);
}

static void
ntb_get_peer_mw_bad_args_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t sizes[2] = { 1, 2 };
	uint64_t dummy_window;
	struct iovec iov = {};

	ntb.mw_cnt = 2;
	ntb.mw_size = sizes;

	/* NULL args. */
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(NULL, 0, &iov, NULL, NULL), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(&ntb, 0, NULL, NULL, NULL), -EINVAL);
	/* Index past the window count. */
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(&ntb, 2, &iov, NULL, NULL), -EINVAL);
	/* Valid index but window not mapped. */
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(&ntb, 0, &iov, NULL, NULL), -ENODEV);
	/* Once mapped, it succeeds. */
	ntb.mw_vaddr[0] = &dummy_window;
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw(&ntb, 0, &iov, NULL, NULL), 0);
}

static void
ntb_attr_unknown_name_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t v;

	CU_ASSERT_EQUAL(spdk_ntb_amd_attr_get(&ntb, "no_such_attribute", &v), -EINVAL);
}

static void
ntb_spad_read_bounds_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	char mmio[4096];
	uint32_t value = 0xdeadbeef;

	memset(mmio, 0, sizeof(mmio));
	ntb.hw_addr = mmio;
	ntb.spad_cnt = 4;
	ntb.self_spad = 0;
	ntb.peer_spad = 128;

	/*
	 * spdk_ntb_amd_spad_read() returns 0/negative-errno and writes the
	 * register value to the out-param, so the error path is unambiguous.
	 */
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(NULL, 0, false, &value), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, 0, false, NULL), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, -1, false, &value), -EINVAL);

	/*
	 * spad == spad_cnt is the exact off-by-one boundary: the valid range
	 * is 0..spad_cnt-1, so index 4 (== spad_cnt) must be rejected. This
	 * guards the `spad >= spad_cnt` bounds check directly.
	 */
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, 4, false, &value), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, 5, false, &value), -EINVAL);

	/* In-range indices succeed and populate the out-param. */
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, 0, false, &value), 0);
	CU_ASSERT_EQUAL(value, 0);
	CU_ASSERT_EQUAL(spdk_ntb_amd_spad_read(&ntb, 3, false, &value), 0);
}

/*
 * Doorbell helpers should reject NULL devices, masks that include bits past
 * the 16-bit AMD_DBSTAT register width, and out-of-range peer doorbell
 * indices. We can't easily test the MMIO write side-effect from the unit
 * test (no mmio backing store wired up), but argument validation is
 * pure logic and the part we care most about for regression catching.
 *
 * AMD_DBREQ/AMD_DBSTAT/AMD_DBMASK live around offset 0x450; size the fake
 * BAR generously so a successful path can write at those offsets without
 * walking off the end of the buffer.
 */
#define UT_FAKE_BAR_SIZE 0x1000

static void
ntb_db_clear_arg_validation_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint8_t mmio[UT_FAKE_BAR_SIZE] = {0};

	CU_ASSERT_EQUAL(spdk_ntb_amd_db_clear(NULL, 0x1), -EINVAL);

	ntb.self_mmio = NULL;
	CU_ASSERT_EQUAL(spdk_ntb_amd_db_clear(&ntb, 0x1), -EINVAL);

	ntb.self_mmio = mmio;
	CU_ASSERT_EQUAL(spdk_ntb_amd_db_clear(&ntb, 0x1ULL << 17), -EINVAL); /* > 16 bits */
	CU_ASSERT_EQUAL(spdk_ntb_amd_db_clear(&ntb, 0xFFFFULL),    0);       /* full mask */
}

static void
ntb_db_set_mask_arg_validation_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint8_t mmio[UT_FAKE_BAR_SIZE] = {0};

	CU_ASSERT_EQUAL(spdk_ntb_amd_db_set_mask(NULL, 0x1), -EINVAL);

	ntb.self_mmio = mmio;
	ntb.db_valid_mask = 0xFFFFULL;
	CU_ASSERT_EQUAL(spdk_ntb_amd_db_set_mask(&ntb, 0x1ULL << 20), -EINVAL); /* outside valid */
	CU_ASSERT_EQUAL(spdk_ntb_amd_db_set_mask(&ntb, 0xFFFFULL), 0);
	CU_ASSERT_EQUAL(ntb.db_mask & 0xFFFFULL, 0xFFFFULL);
}

static void
ntb_peer_db_set_bounds_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint8_t mmio[UT_FAKE_BAR_SIZE] = {0};

	CU_ASSERT_EQUAL(spdk_ntb_amd_peer_db_set(NULL, 0), -EINVAL);

	ntb.hw_addr = (char *)mmio;
	ntb.db_cnt = AMD_DB_COUNT;
	ntb.db_valid_mask = (1ULL << AMD_DB_COUNT) - 1;

	CU_ASSERT_EQUAL(spdk_ntb_amd_peer_db_set(&ntb, AMD_DB_COUNT), -EINVAL);     /* off-by-one */
	CU_ASSERT_EQUAL(spdk_ntb_amd_peer_db_set(&ntb, AMD_DB_COUNT + 4), -EINVAL); /* clearly out */
	CU_ASSERT_EQUAL(spdk_ntb_amd_peer_db_set(&ntb, 0), 0);
	CU_ASSERT_EQUAL(spdk_ntb_amd_peer_db_set(&ntb, AMD_DB_COUNT - 1), 0);
}

/*
 * spdk_ntb_amd_mw_set_trans() combines argument validation with cached BAR lookup.
 * Make sure invalid mw_idx, NULL ntb, and unmapped windows all fail without
 * touching MMIO -- the production code path used to take an EINVAL by
 * dereferencing an unmapped BAR pointer.
 */
static void
ntb_mw_set_trans_arg_validation_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};

	ntb.mw_cnt = 2;

	CU_ASSERT_EQUAL(spdk_ntb_amd_mw_set_trans(NULL,  0, 0, 0), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_mw_set_trans(&ntb, -1, 0, 0), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_mw_set_trans(&ntb,  2, 0, 0), -EINVAL);

	/* Valid mw_idx but BAR not mapped -> ENODEV (not a deref). */
	ntb.mw_vaddr[0] = NULL;
	CU_ASSERT_EQUAL(spdk_ntb_amd_mw_set_trans(&ntb, 0, 0, 4096), -ENODEV);
}

static void
ntb_get_peer_mw_addr_validation_test(void)
{
	struct spdk_ntb_amd_dev ntb = {};
	uint64_t out = 0xdeadbeef;
	char fake_bar[64] = {0};

	ntb.mw_cnt = 2;

	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(NULL, 0, &out), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(&ntb, 0, NULL), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(&ntb, -1, &out), -EINVAL);
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(&ntb, 2, &out), -EINVAL);

	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(&ntb, 0, &out), -ENODEV);

	ntb.mw_vaddr[0] = fake_bar;
	CU_ASSERT_EQUAL(spdk_ntb_amd_get_peer_mw_addr(&ntb, 0, &out), 0);
	CU_ASSERT_EQUAL(out, (uint64_t)(uintptr_t)fake_bar);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("ntb_amd", NULL, NULL);

	CU_ADD_TEST(suite, ntb_set_active_mw_test);
	CU_ADD_TEST(suite, ntb_attr_get_invalid_args_test);
	CU_ADD_TEST(suite, ntb_attr_get_topo_test);
	CU_ADD_TEST(suite, ntb_get_peer_mw_test);
	CU_ADD_TEST(suite, ntb_get_peer_mw_bad_args_test);
	CU_ADD_TEST(suite, ntb_attr_unknown_name_test);
	CU_ADD_TEST(suite, ntb_spad_read_bounds_test);
	CU_ADD_TEST(suite, ntb_db_clear_arg_validation_test);
	CU_ADD_TEST(suite, ntb_db_set_mask_arg_validation_test);
	CU_ADD_TEST(suite, ntb_peer_db_set_bounds_test);
	CU_ADD_TEST(suite, ntb_mw_set_trans_arg_validation_test);
	CU_ADD_TEST(suite, ntb_get_peer_mw_addr_validation_test);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
