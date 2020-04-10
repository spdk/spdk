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

#define SPDK_IDXD_PCI_DEVICE(DEVICE_ID) RTE_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
static struct rte_pci_id idxd_driver_id[] = {
	{SPDK_IDXD_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IDXD)},
	{ .vendor_id = 0, /* sentinel */ },
};

static struct spdk_pci_driver g_idxd_pci_drv = {
	.driver = {
		.drv_flags	= RTE_PCI_DRV_NEED_MAPPING,
		.id_table	= idxd_driver_id,
		.probe		= pci_device_init,
		.remove		= pci_device_fini,
		.driver.name	= "spdk_idxd",
	},

	.cb_fn = NULL,
	.cb_arg = NULL,
	.is_registered = false,
};

struct spdk_pci_driver *
spdk_pci_idxd_get_driver(void)
{
	return &g_idxd_pci_drv;
}

SPDK_PMD_REGISTER_PCI(g_idxd_pci_drv);
