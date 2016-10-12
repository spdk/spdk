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

/*
 * NVMe over PCIe transport
 */

#include "nvme_internal.h"

static int
nvme_pcie_ctrlr_get_pci_id(struct spdk_nvme_ctrlr *ctrlr, struct pci_id *pci_id)
{
	struct spdk_pci_device *pci_dev;

	assert(ctrlr != NULL);
	assert(pci_id != NULL);

	pci_dev = ctrlr->devhandle;
	assert(pci_dev != NULL);

	pci_id->vendor_id = spdk_pci_device_get_vendor_id(pci_dev);
	pci_id->dev_id = spdk_pci_device_get_device_id(pci_dev);
	pci_id->sub_vendor_id = spdk_pci_device_get_subvendor_id(pci_dev);
	pci_id->sub_dev_id = spdk_pci_device_get_subdevice_id(pci_dev);

	return 0;
}

const struct spdk_nvme_transport spdk_nvme_transport_pcie = {
	.ctrlr_get_pci_id = nvme_pcie_ctrlr_get_pci_id,
};
