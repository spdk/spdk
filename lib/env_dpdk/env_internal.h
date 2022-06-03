/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_ENV_INTERNAL_H
#define SPDK_ENV_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/env.h"

#include <rte_config.h>
#include <rte_version.h>
#include <rte_eal.h>
#include <rte_bus.h>
#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_dev.h>

#if RTE_VERSION < RTE_VERSION_NUM(19, 11, 0, 0)
#error RTE_VERSION is too old! Minimum 19.11 is required.
#endif

/* x86-64 and ARM userspace virtual addresses use only the low 48 bits [0..47],
 * which is enough to cover 256 TB.
 */
#define SHIFT_256TB	48 /* (1 << 48) == 256 TB */
#define MASK_256TB	((1ULL << SHIFT_256TB) - 1)

#define SHIFT_1GB	30 /* (1 << 30) == 1 GB */
#define MASK_1GB	((1ULL << SHIFT_1GB) - 1)

#define SPDK_PCI_DRIVER_MAX_NAME_LEN 32
struct spdk_pci_driver {
	struct rte_pci_driver		driver;

	const char                      *name;
	const struct spdk_pci_id	*id_table;
	uint32_t			drv_flags;

	spdk_pci_enum_cb		cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(spdk_pci_driver)	tailq;
};

int pci_device_init(struct rte_pci_driver *driver, struct rte_pci_device *device);
int pci_device_fini(struct rte_pci_device *device);

void pci_env_init(void);
void pci_env_reinit(void);
void pci_env_fini(void);
int mem_map_init(bool legacy_mem);
int vtophys_init(void);

/**
 * Report a DMA-capable PCI device to the vtophys translation code.
 * Increases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called after a `rte_pci_device` is created.
 */
void vtophys_pci_device_added(struct rte_pci_device *pci_device);

/**
 * Report the removal of a DMA-capable PCI device to the vtophys translation code.
 * Decreases the refcount of active DMA-capable devices managed by SPDK.
 * This must be called before a `rte_pci_device` is destroyed.
 */
void vtophys_pci_device_removed(struct rte_pci_device *pci_device);

#endif
