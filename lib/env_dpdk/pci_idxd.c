/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "env_internal.h"

#include "spdk/pci_ids.h"

#define SPDK_IDXD_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
static struct spdk_pci_id idxd_driver_id[] = {
	{SPDK_IDXD_PCI_DEVICE(PCI_DEVICE_ID_INTEL_DSA)},
	{SPDK_IDXD_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IAA)},
	{ .vendor_id = 0, /* sentinel */ },
};

struct spdk_pci_driver *
spdk_pci_idxd_get_driver(void)
{
	return spdk_pci_get_driver("idxd");
}

SPDK_PCI_DRIVER_REGISTER(idxd, idxd_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
