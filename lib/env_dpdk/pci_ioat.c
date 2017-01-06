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

#define SPDK_IOAT_PCI_DEVICE(DEVICE_ID) RTE_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
static struct rte_pci_id ioat_driver_id[] = {
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB9)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW9)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX9)},
	{ .vendor_id = 0, /* sentinel */ },
};

static struct spdk_pci_enum_ctx g_ioat_pci_drv = {
	.driver = {
		.drv_flags	= RTE_PCI_DRV_NEED_MAPPING,
		.id_table	= ioat_driver_id,
#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
		.probe		= spdk_pci_device_init,
		.remove		= spdk_pci_device_fini,
#else
		.devinit	= spdk_pci_device_init,
		.devuninit	= spdk_pci_device_fini,
		.name		= "spdk_ioat",
#endif
	},

	.cb_fn = NULL,
	.cb_arg = NULL,
	.mtx = PTHREAD_MUTEX_INITIALIZER,
};

#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
RTE_PMD_REGISTER_PCI(spdk_ioat, g_ioat_pci_drv.driver);
#else
static int
spdk_ioat_drv_register(const char *name __rte_unused, const char *params __rte_unused)
{
	rte_eal_pci_register(&g_ioat_pci_drv.driver);

	return 0;
}

static struct rte_driver g_ioat_drv = {
	.type = PMD_PDEV,
	.init = spdk_ioat_drv_register,
};

#if RTE_VERSION >= RTE_VERSION_NUM(16, 7, 0, 0)
PMD_REGISTER_DRIVER(g_ioat_drv, spdk_ioat);
#else
PMD_REGISTER_DRIVER(g_ioat_drv);
#endif
#endif

int
spdk_pci_ioat_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
			    struct spdk_pci_addr *pci_address)
{
	return spdk_pci_device_attach(&g_ioat_pci_drv, enum_cb, enum_ctx, pci_address);
}

int
spdk_pci_ioat_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return spdk_pci_enumerate(&g_ioat_pci_drv, enum_cb, enum_ctx);
}
