/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#include "env_internal.h"

#include "spdk/pci_ids.h"

/*
 * AMD EPYC 4th-generation Non-Transparent Bridge driver registration.
 *
 * The driver name is "ntb_amd" (rather than the bare "ntb") because "ntb"
 * is a generic device category — Intel and PLX/Avago also ship NTB
 * silicon, and any future SPDK NTB driver will want to register its own
 * (vid, did) table under its own name.
 *
 * The "ntb_amd" / "ntb_intel" / "ntb_plx" naming is intentional: it
 * matches the existing NTB-framework convention in Linux
 * (drivers/ntb/hw/{amd,intel,idt}/ntb_hw_<vendor>.c) and DPDK
 * (dpdk/drivers/raw/ntb/ntb_hw_<vendor>.c), and it follows SPDK's
 * <category>_<implementation> pattern (bdev_nvme, accel_ioat, etc.).
 * It also keeps every NTB driver visible together in `git grep ntb_*`
 * and matches the library name (libspdk_ntb_amd / lib/ntb_amd).
 */

#define SPDK_NTB_AMD_PCI_DEVICE(DEVICE_ID) SPDK_PCI_DEVICE(SPDK_PCI_VID_AMD, DEVICE_ID)

static struct spdk_pci_id ntb_amd_driver_id[] = {
	{SPDK_NTB_AMD_PCI_DEVICE(PCI_DEVICE_ID_AMD_NTB_PRIMARY_EPYC9XX4)},
	{SPDK_NTB_AMD_PCI_DEVICE(PCI_DEVICE_ID_AMD_NTB_SECONDARY_EPYC9XX4)},
	{ .vendor_id = 0, /* sentinel */ },
};

struct spdk_pci_driver *
spdk_pci_ntb_amd_get_driver(void)
{
	return spdk_pci_get_driver("ntb_amd");
}

SPDK_PCI_DRIVER_REGISTER(ntb_amd, ntb_amd_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
