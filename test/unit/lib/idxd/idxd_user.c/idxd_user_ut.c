/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/idxd.h"
#include "common/lib/test_env.c"

#include "idxd/idxd_user.c"

#define FAKE_REG_SIZE 0x1000
#define GRP_CFG_OFFSET (0x800 / IDXD_TABLE_OFFSET_MULT)
#define MAX_TOKENS 0x40
#define MAX_ARRAY_SIZE 0x20

SPDK_LOG_REGISTER_COMPONENT(idxd);

DEFINE_STUB(spdk_pci_idxd_get_driver, struct spdk_pci_driver *, (void), NULL);
DEFINE_STUB_V(idxd_impl_register, (struct spdk_idxd_impl *impl));
DEFINE_STUB_V(spdk_pci_device_detach, (struct spdk_pci_device *device));
DEFINE_STUB(spdk_pci_device_claim, int, (struct spdk_pci_device *dev), 0);
DEFINE_STUB(spdk_pci_device_get_device_id, uint16_t, (struct spdk_pci_device *dev), 0);
DEFINE_STUB(spdk_pci_device_get_vendor_id, uint16_t, (struct spdk_pci_device *dev), 0);

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr;

	memset(&pci_addr, 0, sizeof(pci_addr));
	return pci_addr;
}

int
spdk_pci_enumerate(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return -1;
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	*mapped_addr = NULL;
	*phys_addr = 0;
	*size = 0;
	return 0;
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
	return 0;
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value,
			   uint32_t offset)
{
	*value = 0xFFFFFFFFu;
	return 0;
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value,
			    uint32_t offset)
{
	return 0;
}

#define WQ_CFG_OFFSET (0x800 / IDXD_TABLE_OFFSET_MULT)
#define TOTAL_WQE_SIZE 0x40
static int
test_idxd_wq_config(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	uint32_t wq_size, i, j;
	int rc;
	union idxd_wqcfg *wqcfg;

	user_idxd.registers = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.registers != NULL);

	user_idxd.registers->wqcap.total_wq_size = TOTAL_WQE_SIZE;
	user_idxd.registers->wqcap.num_wqs = 1;
	user_idxd.registers->gencap.max_batch_shift = LOG2_WQ_MAX_BATCH;
	user_idxd.registers->gencap.max_xfer_shift = LOG2_WQ_MAX_XFER;
	user_idxd.registers->offsets.wqcfg = WQ_CFG_OFFSET;
	wq_size = user_idxd.registers->wqcap.total_wq_size;

	wqcfg = (union idxd_wqcfg *)((uint8_t *)user_idxd.registers +
				     (user_idxd.registers->offsets.wqcfg * IDXD_TABLE_OFFSET_MULT));

	rc = idxd_wq_config(&user_idxd);
	CU_ASSERT(rc == 0);
	CU_ASSERT(wqcfg->wq_size == wq_size);
	CU_ASSERT(wqcfg->mode == WQ_MODE_DEDICATED);
	CU_ASSERT(wqcfg->max_batch_shift == LOG2_WQ_MAX_BATCH);
	CU_ASSERT(wqcfg->max_xfer_shift == LOG2_WQ_MAX_XFER);
	CU_ASSERT(wqcfg->wq_state == WQ_ENABLED);
	CU_ASSERT(wqcfg->priority == WQ_PRIORITY_1);

	for (i = 1; i < user_idxd.registers->wqcap.num_wqs; i++) {
		for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
			CU_ASSERT(spdk_mmio_read_4(&wqcfg->raw[j]) == 0);
		}
	}

	free(user_idxd.registers);

	return 0;
}

static int
test_idxd_group_config(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	uint64_t wqs[MAX_ARRAY_SIZE] = {};
	uint64_t engines[MAX_ARRAY_SIZE] = {};
	union idxd_group_flags flags[MAX_ARRAY_SIZE] = {};
	int rc, i;
	struct idxd_grptbl *grptbl;

	user_idxd.registers = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.registers != NULL);

	user_idxd.registers->groupcap.num_groups = 1;
	user_idxd.registers->enginecap.num_engines = 4;
	user_idxd.registers->wqcap.num_wqs = 1;
	user_idxd.registers->groupcap.read_bufs = MAX_TOKENS;
	user_idxd.registers->offsets.grpcfg = GRP_CFG_OFFSET;

	grptbl = (struct idxd_grptbl *)((uint8_t *)user_idxd.registers +
					(user_idxd.registers->offsets.grpcfg * IDXD_TABLE_OFFSET_MULT));

	rc = idxd_group_config(&user_idxd);
	CU_ASSERT(rc == 0);
	for (i = 0 ; i < user_idxd.registers->groupcap.num_groups; i++) {
		wqs[i] = spdk_mmio_read_8(&grptbl->group[i].wqs[0]);
		engines[i] = spdk_mmio_read_8(&grptbl->group[i].engines);
		flags[i].raw = spdk_mmio_read_4(&grptbl->group[i].flags.raw);
	}
	/* wqe and engine arrays are indexed by group id and are bitmaps of assigned elements. */
	CU_ASSERT(wqs[0] == 0x1);
	CU_ASSERT(engines[0] == 0xf);
	CU_ASSERT(flags[0].read_buffers_allowed == MAX_TOKENS);

	/* groups allocated by code under test. */
	free(user_idxd.registers);

	return 0;
}

static int
test_idxd_reset_dev(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	union idxd_cmdsts_register *fake_cmd_status_reg;
	int rc;

	user_idxd.registers = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.registers != NULL);
	fake_cmd_status_reg = &user_idxd.registers->cmdsts;

	/* Test happy path */
	rc = idxd_reset_dev(&user_idxd);
	CU_ASSERT(rc == 0);

	/* Test error reported path */
	fake_cmd_status_reg->err = 1;
	rc = idxd_reset_dev(&user_idxd);
	CU_ASSERT(rc == -EINVAL);

	free(user_idxd.registers);

	return 0;
}

static int
test_idxd_wait_cmd(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	int timeout = 1;
	union idxd_cmdsts_register *fake_cmd_status_reg;
	int rc;

	user_idxd.registers = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.registers != NULL);
	fake_cmd_status_reg = &user_idxd.registers->cmdsts;

	/* Test happy path. */
	rc = idxd_wait_cmd(&user_idxd, timeout);
	CU_ASSERT(rc == 0);

	/* Setup up our fake register to set the error bit. */
	fake_cmd_status_reg->err = 1;
	rc = idxd_wait_cmd(&user_idxd, timeout);
	CU_ASSERT(rc == -EINVAL);
	fake_cmd_status_reg->err = 0;

	/* Setup up our fake register to set the active bit. */
	fake_cmd_status_reg->active = 1;
	rc = idxd_wait_cmd(&user_idxd, timeout);
	CU_ASSERT(rc == -EBUSY);

	free(user_idxd.registers);

	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("idxd_user", NULL, NULL);

	CU_ADD_TEST(suite, test_idxd_wait_cmd);
	CU_ADD_TEST(suite, test_idxd_reset_dev);
	CU_ADD_TEST(suite, test_idxd_group_config);
	CU_ADD_TEST(suite, test_idxd_wq_config);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
