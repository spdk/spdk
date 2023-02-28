/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "ioat/ioat.c"

#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"

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
ioat_state_check(void)
{
	/*
	 * CHANSTS's STATUS field is 3 bits (8 possible values), but only has 5 valid states:
	 * ACTIVE	0x0
	 * IDLE		0x1
	 * SUSPENDED	0x2
	 * HALTED	0x3
	 * ARMED	0x4
	 */

	CU_ASSERT(is_ioat_active(0) == 1); /* ACTIVE */
	CU_ASSERT(is_ioat_active(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_active(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_active(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_active(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_active(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_active(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_active(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_idle(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_idle(1) == 1); /* IDLE */
	CU_ASSERT(is_ioat_idle(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_idle(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_idle(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_idle(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_idle(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_idle(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_suspended(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_suspended(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_suspended(2) == 1); /* SUSPENDED */
	CU_ASSERT(is_ioat_suspended(3) == 0); /* HALTED */
	CU_ASSERT(is_ioat_suspended(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_suspended(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_suspended(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_suspended(7) == 0); /* reserved */

	CU_ASSERT(is_ioat_halted(0) == 0); /* ACTIVE */
	CU_ASSERT(is_ioat_halted(1) == 0); /* IDLE */
	CU_ASSERT(is_ioat_halted(2) == 0); /* SUSPENDED */
	CU_ASSERT(is_ioat_halted(3) == 1); /* HALTED */
	CU_ASSERT(is_ioat_halted(4) == 0); /* ARMED */
	CU_ASSERT(is_ioat_halted(5) == 0); /* reserved */
	CU_ASSERT(is_ioat_halted(6) == 0); /* reserved */
	CU_ASSERT(is_ioat_halted(7) == 0); /* reserved */
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ioat", NULL, NULL);

	CU_ADD_TEST(suite, ioat_state_check);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
