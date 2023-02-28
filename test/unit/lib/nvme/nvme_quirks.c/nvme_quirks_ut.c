/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "nvme/nvme_quirks.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

static void
test_nvme_quirks_striping(void)
{
	struct spdk_pci_id pci_id = {};
	uint64_t quirks = 0;

	/* Non-Intel device should not have striping enabled */
	quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT((quirks & NVME_INTEL_QUIRK_STRIPING) == 0);

	/* Set the vendor id to Intel, but no device id. No striping. */
	pci_id.class_id = SPDK_PCI_CLASS_NVME;
	pci_id.vendor_id = SPDK_PCI_VID_INTEL;
	quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT((quirks & NVME_INTEL_QUIRK_STRIPING) == 0);

	/* Device ID 0x0953 should have striping enabled */
	pci_id.device_id = 0x0953;
	quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT((quirks & NVME_INTEL_QUIRK_STRIPING) != 0);

	/* Even if specific subvendor/subdevice ids are set,
	 * striping should be enabled.
	 */
	pci_id.subvendor_id = SPDK_PCI_VID_INTEL;
	pci_id.subdevice_id = 0x3704;
	quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT((quirks & NVME_INTEL_QUIRK_STRIPING) != 0);

	pci_id.subvendor_id = 1234;
	pci_id.subdevice_id = 42;
	quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT((quirks & NVME_INTEL_QUIRK_STRIPING) != 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_quirks", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_quirks_striping);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
