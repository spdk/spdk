/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/config.h"
#include "spdk/env.h"

#ifndef SPDK_CONFIG_VMD
struct spdk_pci_driver *
spdk_pci_vmd_get_driver(void)
{
	return NULL;
}
#endif
