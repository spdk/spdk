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
#include "idxd/idxd.c"

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

#define FAKE_REG_SIZE 1024
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

	chan.ring_ctrl.ring_slots = spdk_bit_array_create(test_ring_size);
	chan.ring_ctrl.ring_size = test_ring_size;
	chan.ring_ctrl.completions = spdk_zmalloc(test_ring_size * sizeof(struct idxd_hw_desc), 0, NULL,
				     SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	SPDK_CU_ASSERT_FATAL(chan.ring_ctrl.completions != NULL);

	rc = spdk_idxd_reconfigure_chan(&chan, num_channels);
	CU_ASSERT(rc == 0);
	CU_ASSERT(chan.ring_ctrl.max_ring_slots == test_ring_size / num_channels);

	spdk_bit_array_free(&chan.ring_ctrl.ring_slots);
	spdk_free(chan.ring_ctrl.completions);
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

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
