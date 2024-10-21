/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#include "env_internal.h"

#include "spdk/pci_ids.h"

#define SPDK_AE4DMA_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_AMD, DEVICE_ID)
static struct spdk_pci_id ae4dma_driver_id[] = {
	{SPDK_AE4DMA_PCI_DEVICE(PCI_DEVICE_ID_AMD_AE4DMA_3E)},
	{SPDK_AE4DMA_PCI_DEVICE(PCI_DEVICE_ID_AMD_AE4DMA_4E)},
	{ .vendor_id = 0, /* sentinel */ },
};

struct spdk_pci_driver *
spdk_pci_ae4dma_get_driver(void)
{
	return spdk_pci_get_driver("ae4dma");
}

SPDK_PCI_DRIVER_REGISTER(ae4dma, ae4dma_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
