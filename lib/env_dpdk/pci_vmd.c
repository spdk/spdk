/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "env_internal.h"

#include "spdk/pci_ids.h"

static struct spdk_pci_id vmd_pci_driver_id[] = {
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, PCI_DEVICE_ID_INTEL_VMD_SKX) },
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_INTEL, PCI_DEVICE_ID_INTEL_VMD_ICX) },
	{ .vendor_id = 0, /* sentinel */ },
};

struct spdk_pci_driver *
spdk_pci_vmd_get_driver(void)
{
	return spdk_pci_get_driver("vmd");
}

SPDK_PCI_DRIVER_REGISTER(vmd, vmd_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);
