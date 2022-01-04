/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/idxd.h"
#include "common/lib/test_env.c"

#include "idxd/idxd.h"
#include "idxd/idxd_user.c"

#define FAKE_REG_SIZE 0x800
#define GRP_CFG_OFFSET 0x400
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

#define WQ_CFG_OFFSET 0x500
#define TOTAL_WQE_SIZE 0x40
static int
test_idxd_wq_config(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	struct spdk_idxd_device *idxd = &user_idxd.idxd;
	union idxd_wqcfg wqcfg = {};
	uint32_t expected[8] = {0x40, 0, 0x11, 0xbe, 0, 0, 0x40000000, 0};
	uint32_t wq_size, i, j;
	uint32_t wqcap_size = 32;
	int rc;

	user_idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.reg_base != NULL);

	SPDK_CU_ASSERT_FATAL(g_user_dev_cfg.num_groups <= MAX_ARRAY_SIZE);
	idxd->groups = calloc(g_user_dev_cfg.num_groups, sizeof(struct idxd_group));
	SPDK_CU_ASSERT_FATAL(idxd->groups != NULL);

	user_idxd.registers.wqcap.total_wq_size = TOTAL_WQE_SIZE;
	user_idxd.registers.wqcap.num_wqs = g_user_dev_cfg.total_wqs;
	user_idxd.registers.gencap.max_batch_shift = LOG2_WQ_MAX_BATCH;
	user_idxd.registers.gencap.max_xfer_shift = LOG2_WQ_MAX_XFER;
	user_idxd.wqcfg_offset = WQ_CFG_OFFSET;
	wq_size = user_idxd.registers.wqcap.total_wq_size / g_user_dev_cfg.total_wqs;

	rc = idxd_wq_config(&user_idxd);
	CU_ASSERT(rc == 0);
	for (i = 0; i < g_user_dev_cfg.total_wqs; i++) {
		CU_ASSERT(idxd->queues[i].wqcfg.wq_size == wq_size);
		CU_ASSERT(idxd->queues[i].wqcfg.mode == WQ_MODE_DEDICATED);
		CU_ASSERT(idxd->queues[i].wqcfg.max_batch_shift == LOG2_WQ_MAX_BATCH);
		CU_ASSERT(idxd->queues[i].wqcfg.max_xfer_shift == LOG2_WQ_MAX_XFER);
		CU_ASSERT(idxd->queues[i].wqcfg.wq_state == WQ_ENABLED);
		CU_ASSERT(idxd->queues[i].wqcfg.priority == WQ_PRIORITY_1);
		CU_ASSERT(idxd->queues[i].idxd == idxd);
		CU_ASSERT(idxd->queues[i].group == &idxd->groups[i % g_user_dev_cfg.num_groups]);
	}

	for (i = 0 ; i < user_idxd.registers.wqcap.num_wqs; i++) {
		for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
			wqcfg.raw[j] = spdk_mmio_read_4((uint32_t *)(user_idxd.reg_base +
							user_idxd.wqcfg_offset + i * wqcap_size + j * sizeof(uint32_t)));
			CU_ASSERT(wqcfg.raw[j] == expected[j]);
		}
	}

	free(idxd->queues);
	free(user_idxd.reg_base);
	free(idxd->groups);

	return 0;
}

static int
test_idxd_group_config(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	struct spdk_idxd_device *idxd = &user_idxd.idxd;
	uint64_t wqs[MAX_ARRAY_SIZE] = {};
	uint64_t engines[MAX_ARRAY_SIZE] = {};
	union idxd_group_flags flags[MAX_ARRAY_SIZE] = {};
	int rc, i;
	uint64_t base_offset;

	user_idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.reg_base != NULL);

	SPDK_CU_ASSERT_FATAL(g_user_dev_cfg.num_groups <= MAX_ARRAY_SIZE);
	user_idxd.registers.groupcap.num_groups = g_user_dev_cfg.num_groups;
	user_idxd.registers.enginecap.num_engines = g_user_dev_cfg.total_engines;
	user_idxd.registers.wqcap.num_wqs = g_user_dev_cfg.total_wqs;
	user_idxd.registers.groupcap.read_bufs = MAX_TOKENS;
	user_idxd.grpcfg_offset = GRP_CFG_OFFSET;

	rc = idxd_group_config(idxd);
	CU_ASSERT(rc == 0);
	for (i = 0 ; i < user_idxd.registers.groupcap.num_groups; i++) {
		base_offset = user_idxd.grpcfg_offset + i * 64;

		wqs[i] = spdk_mmio_read_8((uint64_t *)(user_idxd.reg_base + base_offset));
		engines[i] = spdk_mmio_read_8((uint64_t *)(user_idxd.reg_base + base_offset + CFG_ENGINE_OFFSET));
		flags[i].raw = spdk_mmio_read_8((uint64_t *)(user_idxd.reg_base + base_offset + CFG_FLAG_OFFSET));
	}
	/* wqe and engine arrays are indexed by group id and are bitmaps of assigned elements. */
	CU_ASSERT(wqs[0] == 0x1);
	CU_ASSERT(engines[0] == 0xf);
	CU_ASSERT(flags[0].tokens_allowed == MAX_TOKENS / g_user_dev_cfg.num_groups);

	/* groups allocated by code under test. */
	free(idxd->groups);
	free(user_idxd.reg_base);

	return 0;
}

static int
test_idxd_reset_dev(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	union idxd_cmdsts_reg *fake_cmd_status_reg;
	int rc;

	user_idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.reg_base != NULL);
	fake_cmd_status_reg = user_idxd.reg_base + IDXD_CMDSTS_OFFSET;

	/* Test happy path */
	rc = idxd_reset_dev(&user_idxd.idxd);
	CU_ASSERT(rc == 0);

	/* Test error reported path */
	fake_cmd_status_reg->err = 1;
	rc = idxd_reset_dev(&user_idxd.idxd);
	CU_ASSERT(rc == -EINVAL);

	free(user_idxd.reg_base);

	return 0;
}

static int
test_idxd_wait_cmd(void)
{
	struct spdk_user_idxd_device user_idxd = {};
	int timeout = 1;
	union idxd_cmdsts_reg *fake_cmd_status_reg;
	int rc;

	user_idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(user_idxd.reg_base != NULL);
	fake_cmd_status_reg = user_idxd.reg_base + IDXD_CMDSTS_OFFSET;

	/* Test happy path. */
	rc = idxd_wait_cmd(&user_idxd.idxd, timeout);
	CU_ASSERT(rc == 0);

	/* Setup up our fake register to set the error bit. */
	fake_cmd_status_reg->err = 1;
	rc = idxd_wait_cmd(&user_idxd.idxd, timeout);
	CU_ASSERT(rc == -EINVAL);
	fake_cmd_status_reg->err = 0;

	/* Setup up our fake register to set the active bit. */
	fake_cmd_status_reg->active = 1;
	rc = idxd_wait_cmd(&user_idxd.idxd, timeout);
	CU_ASSERT(rc == -EBUSY);

	free(user_idxd.reg_base);

	return 0;
}

static int
test_setup(void)
{
	g_user_dev_cfg.config_num = 0;
	g_user_dev_cfg.num_groups = 1;
	g_user_dev_cfg.total_wqs = 1;
	g_user_dev_cfg.total_engines = 4;

	return 0;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("idxd_user", test_setup, NULL);

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
