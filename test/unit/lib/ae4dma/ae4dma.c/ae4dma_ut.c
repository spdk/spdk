/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 AMD Corporation.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "ae4dma/ae4dma.c"

#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "ae4dma/ae4dma_internal.h"

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

static void
ae4dma_queue_full_check(void)
{
	uint8_t i;

	/* Checking cmdq full */
	for (i = 0; i < 28; i++)
		CU_ASSERT(ae4dma_desc_cmdq_full(i) == false)

		CU_ASSERT(ae4dma_desc_cmdq_full(28) == true)
		CU_ASSERT(ae4dma_desc_cmdq_full(29) == true)
		CU_ASSERT(ae4dma_desc_cmdq_full(30) == true)
	}

static void
ae4dma_max_queue_config_check(void)
{
	/* Checking for max queues allowed to configure per device */
	for (uint8_t i = 0; i < 16; i++) {
		CU_ASSERT(ae4dma_config_queues_per_device(i) == 0);
	}

	CU_ASSERT(ae4dma_config_queues_per_device(17) == 1);
	CU_ASSERT(ae4dma_config_queues_per_device(-1) == 1);
}

int
main(int argc, char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("ae4dma", NULL, NULL);

	CU_ADD_TEST(suite, ae4dma_queue_full_check);
	CU_ADD_TEST(suite, ae4dma_max_queue_config_check);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
