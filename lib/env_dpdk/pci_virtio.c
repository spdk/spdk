/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "env_internal.h"

#include "spdk/pci_ids.h"

static struct spdk_pci_id virtio_pci_driver_id[] = {
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_SCSI_MODERN) },
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_BLK_MODERN) },
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_SCSI_LEGACY) },
	{ SPDK_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_BLK_LEGACY) },
	{ .vendor_id = 0, /* sentinel */ },
};

struct spdk_pci_driver *
spdk_pci_virtio_get_driver(void)
{
	return spdk_pci_get_driver("virtio");
}

SPDK_PCI_DRIVER_REGISTER(virtio, virtio_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);
