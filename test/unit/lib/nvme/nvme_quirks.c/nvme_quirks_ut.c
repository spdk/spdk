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

int main(int argc, char **argv)
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
