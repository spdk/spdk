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

#ifndef SPDK_ENV_INTERNAL_H
#define SPDK_ENV_INTERNAL_H

#include "spdk/stdinc.h"

#define spdk_pci_device rte_pci_device

#include "spdk/env.h"

#include <rte_config.h>
#include <rte_version.h>
#include <rte_eal.h>
#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 0)
#include <rte_bus.h>
extern struct rte_pci_bus rte_pci_bus;
#endif
#include <rte_pci.h>
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 1)
#include <rte_bus_pci.h>
#endif
#include <rte_dev.h>

/* x86-64 and ARM userspace virtual addresses use only the low 48 bits [0..47],
 * which is enough to cover 256 TB.
 */
#define SHIFT_256TB	48 /* (1 << 48) == 256 TB */
#define MASK_256TB	((1ULL << SHIFT_256TB) - 1)

#define SHIFT_1GB	30 /* (1 << 30) == 1 GB */
#define MASK_1GB	((1ULL << SHIFT_1GB) - 1)

#define SHIFT_2MB	21 /* (1 << 21) == 2MB */
#define MASK_2MB	((1ULL << SHIFT_2MB) - 1)
#define VALUE_2MB	(1 << SHIFT_2MB)

#define SHIFT_4KB	12 /* (1 << 12) == 4KB */
#define MASK_4KB	((1ULL << SHIFT_4KB) - 1)
#define VALUE_4KB	(1 << SHIFT_4KB)

struct spdk_pci_enum_ctx {
	struct rte_pci_driver	driver;
	spdk_pci_enum_cb	cb_fn;
	void			*cb_arg;
	pthread_mutex_t		mtx;
	bool			is_registered;
};

int spdk_pci_device_init(struct rte_pci_driver *driver, struct rte_pci_device *device);
int spdk_pci_device_fini(struct rte_pci_device *device);

int spdk_pci_enumerate(struct spdk_pci_enum_ctx *ctx, spdk_pci_enum_cb enum_cb, void *enum_ctx);
int spdk_pci_device_attach(struct spdk_pci_enum_ctx *ctx, spdk_pci_enum_cb enum_cb, void *enum_ctx,
			   struct spdk_pci_addr *pci_address);

int spdk_mem_map_init(void);
int spdk_vtophys_init(void);

/**
 * Report a DMA-capable PCI device to the vtophys translation code.
 * Increases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called after a `rte_pci_device` is created.
 */
void spdk_vtophys_pci_device_added(struct rte_pci_device *pci_device);

/**
 * Report the removal of a DMA-capable PCI device to the vtophys translation code.
 * Decreases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called before a `rte_pci_device` is destroyed.
 */
void spdk_vtophys_pci_device_removed(struct rte_pci_device *pci_device);

#endif
