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

#define FAKE_REG_SIZE 0x800
#define NUM_GROUPS 4
#define NUM_WQ_PER_GROUP 1
#define NUM_ENGINES_PER_GROUP 1
#define TOTAL_WQS (NUM_GROUPS * NUM_WQ_PER_GROUP)
#define TOTAL_ENGINES (NUM_GROUPS * NUM_ENGINES_PER_GROUP)

DEFINE_STUB(spdk_pci_idxd_get_driver, struct spdk_pci_driver *, (void), NULL);

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

#define movdir64b mock_movdir64b
static inline void
mock_movdir64b(void *dst, const void *src)
{
	return;
}

#include "idxd/idxd.c"

#define WQ_CFG_OFFSET 0x500
#define TOTAL_WQE_SIZE 0x40
static int
test_idxd_wq_config(void)
{
	struct spdk_idxd_device idxd = {};
	union idxd_wqcfg wqcfg = {};
	uint32_t expected[8] = {0x10, 0, 0x11, 0x9e, 0, 0, 0x40000000, 0};
	uint32_t wq_size;
	int rc, i, j;

	idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(idxd.reg_base != NULL);

	g_dev_cfg = &g_dev_cfg0;
	idxd.registers.wqcap.total_wq_size = TOTAL_WQE_SIZE;
	idxd.registers.wqcap.num_wqs = TOTAL_WQS;
	idxd.registers.gencap.max_batch_shift = LOG2_WQ_MAX_BATCH;
	idxd.registers.gencap.max_xfer_shift = LOG2_WQ_MAX_XFER;
	idxd.wqcfg_offset = WQ_CFG_OFFSET;
	wq_size = idxd.registers.wqcap.total_wq_size / g_dev_cfg->total_wqs;

	rc = idxd_wq_config(&idxd);
	CU_ASSERT(rc == 0);
	for (i = 0; i < g_dev_cfg->total_wqs; i++) {
		CU_ASSERT(idxd.queues[i].wqcfg.wq_size == wq_size);
		CU_ASSERT(idxd.queues[i].wqcfg.mode == WQ_MODE_DEDICATED);
		CU_ASSERT(idxd.queues[i].wqcfg.max_batch_shift == LOG2_WQ_MAX_BATCH);
		CU_ASSERT(idxd.queues[i].wqcfg.max_xfer_shift == LOG2_WQ_MAX_XFER);
		CU_ASSERT(idxd.queues[i].wqcfg.wq_state == WQ_ENABLED);
		CU_ASSERT(idxd.queues[i].wqcfg.priority == WQ_PRIORITY_1);
		CU_ASSERT(idxd.queues[i].idxd == &idxd);
		CU_ASSERT(idxd.queues[i].group == &idxd.groups[i % g_dev_cfg->num_groups]);
	}

	for (i = 0 ; i < idxd.registers.wqcap.num_wqs; i++) {
		for (j = 0 ; j < WQCFG_NUM_DWORDS; j++) {
			wqcfg.raw[j] = spdk_mmio_read_4((uint32_t *)(idxd.reg_base + idxd.wqcfg_offset + i * 32 + j *
							4));
			CU_ASSERT(wqcfg.raw[j] == expected[j]);
		}
	}

	free(idxd.queues);
	free(idxd.reg_base);

	return 0;
}

#define GRP_CFG_OFFSET 0x400
#define MAX_TOKENS 0x40
static int
test_idxd_group_config(void)
{
	struct spdk_idxd_device idxd = {};
	uint64_t wqs[NUM_GROUPS] = {};
	uint64_t engines[NUM_GROUPS] = {};
	union idxd_group_flags flags[NUM_GROUPS] = {};
	int rc, i;
	uint64_t base_offset;

	idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(idxd.reg_base != NULL);

	g_dev_cfg = &g_dev_cfg0;
	idxd.registers.groupcap.num_groups = NUM_GROUPS;
	idxd.registers.enginecap.num_engines = TOTAL_ENGINES;
	idxd.registers.wqcap.num_wqs = TOTAL_WQS;
	idxd.registers.groupcap.total_tokens = MAX_TOKENS;
	idxd.grpcfg_offset = GRP_CFG_OFFSET;

	rc = idxd_group_config(&idxd);
	CU_ASSERT(rc == 0);
	for (i = 0 ; i < idxd.registers.groupcap.num_groups; i++) {
		base_offset = idxd.grpcfg_offset + i * 64;

		wqs[i] = spdk_mmio_read_8((uint64_t *)(idxd.reg_base + base_offset));
		engines[i] = spdk_mmio_read_8((uint64_t *)(idxd.reg_base + base_offset + CFG_ENGINE_OFFSET));
		flags[i].raw = spdk_mmio_read_8((uint64_t *)(idxd.reg_base + base_offset + CFG_FLAG_OFFSET));
	}
	/* wqe and engine arrays are indexed by group id and are bitmaps of assigned elements. */
	CU_ASSERT(wqs[0] == 0x1);
	CU_ASSERT(engines[0] == 0x1);
	CU_ASSERT(wqs[1] == 0x2);
	CU_ASSERT(engines[1] == 0x2);
	CU_ASSERT(flags[0].tokens_allowed == MAX_TOKENS / NUM_GROUPS);
	CU_ASSERT(flags[1].tokens_allowed == MAX_TOKENS / NUM_GROUPS);

	/* groups allocated by code under test. */
	free(idxd.groups);
	free(idxd.reg_base);

	return 0;
}

static int
test_idxd_reset_dev(void)
{
	struct spdk_idxd_device idxd = {};
	union idxd_cmdsts_reg *fake_cmd_status_reg;
	int rc;

	idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(idxd.reg_base != NULL);
	fake_cmd_status_reg = idxd.reg_base + IDXD_CMDSTS_OFFSET;

	/* Test happy path */
	rc = idxd_reset_dev(&idxd);
	CU_ASSERT(rc == 0);

	/* Test error reported path */
	fake_cmd_status_reg->err = 1;
	rc = idxd_reset_dev(&idxd);
	CU_ASSERT(rc == -EINVAL);

	free(idxd.reg_base);

	return 0;
}

static int
test_idxd_wait_cmd(void)
{
	struct spdk_idxd_device idxd = {};
	int timeout = 1;
	union idxd_cmdsts_reg *fake_cmd_status_reg;
	int rc;

	idxd.reg_base = calloc(1, FAKE_REG_SIZE);
	SPDK_CU_ASSERT_FATAL(idxd.reg_base != NULL);
	fake_cmd_status_reg = idxd.reg_base + IDXD_CMDSTS_OFFSET;

	/* Test happy path. */
	rc = idxd_wait_cmd(&idxd, timeout);
	CU_ASSERT(rc == 0);

	/* Setup up our fake register to set the error bit. */
	fake_cmd_status_reg->err = 1;
	rc = idxd_wait_cmd(&idxd, timeout);
	CU_ASSERT(rc == -EINVAL);
	fake_cmd_status_reg->err = 0;

	/* Setup up our fake register to set the active bit. */
	fake_cmd_status_reg->active = 1;
	rc = idxd_wait_cmd(&idxd, timeout);
	CU_ASSERT(rc == -EBUSY);

	free(idxd.reg_base);

	return 0;
}

static int
test_spdk_idxd_set_config(void)
{

	g_dev_cfg = NULL;
	spdk_idxd_set_config(0);
	SPDK_CU_ASSERT_FATAL(g_dev_cfg != NULL);
	CU_ASSERT(memcmp(&g_dev_cfg0, g_dev_cfg, sizeof(struct device_config)) == 0);

	return 0;
}

static int
test_spdk_idxd_reconfigure_chan(void)
{
	struct spdk_idxd_io_channel chan = {};
	int rc;
	uint32_t test_ring_size = 8;
	uint32_t num_channels = 2;

	chan.ring_slots = spdk_bit_array_create(test_ring_size);
	chan.ring_size = test_ring_size;
	chan.completions = spdk_zmalloc(test_ring_size * sizeof(struct idxd_hw_desc), 0, NULL,
					SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	SPDK_CU_ASSERT_FATAL(chan.completions != NULL);

	rc = spdk_idxd_reconfigure_chan(&chan, num_channels);
	CU_ASSERT(rc == 0);
	CU_ASSERT(chan.max_ring_slots == test_ring_size / num_channels);

	spdk_bit_array_free(&chan.ring_slots);
	spdk_free(chan.completions);
	return 0;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("idxd", NULL, NULL);

	CU_ADD_TEST(suite, test_spdk_idxd_reconfigure_chan);
	CU_ADD_TEST(suite, test_spdk_idxd_set_config);
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
