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

#include "env_internal.h"

#include "spdk/pci_ids.h"

static struct rte_pci_id virtio_pci_driver_id[] = {
	{ RTE_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_SCSI_MODERN) },
	{ RTE_PCI_DEVICE(SPDK_PCI_VID_VIRTIO, PCI_DEVICE_ID_VIRTIO_BLK_MODERN) },
	{ .vendor_id = 0, /* sentinel */ },
};

static struct spdk_pci_enum_ctx g_virtio_pci_drv = {
	.driver = {
		.drv_flags	= RTE_PCI_DRV_NEED_MAPPING,
		.id_table	= virtio_pci_driver_id,
#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
		.probe		= spdk_pci_device_init,
		.remove		= spdk_pci_device_fini,
		.driver.name	= "spdk_virtio",
#else
		.devinit	= spdk_pci_device_init,
		.devuninit	= spdk_pci_device_fini,
		.name		= "spdk_virtio",
#endif
	},

	.cb_fn = NULL,
	.cb_arg = NULL,
	.mtx = PTHREAD_MUTEX_INITIALIZER,
	.is_registered = false,
};

int
spdk_pci_virtio_device_attach(spdk_pci_enum_cb enum_cb,
			      void *enum_ctx, struct spdk_pci_addr *pci_address)
{
	return spdk_pci_device_attach(&g_virtio_pci_drv, enum_cb, enum_ctx, pci_address);
}

int
spdk_pci_virtio_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return spdk_pci_enumerate(&g_virtio_pci_drv, enum_cb, enum_ctx);
}
