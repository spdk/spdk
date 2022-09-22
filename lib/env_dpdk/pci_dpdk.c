/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include <rte_config.h>
#include <rte_version.h>
#include "pci_dpdk.h"
#include "spdk/log.h"

extern struct dpdk_fn_table fn_table_2207;

static struct dpdk_fn_table *g_dpdk_fn_table;

int
dpdk_pci_init(void)
{
	uint32_t year;
	uint32_t month;
	uint32_t minor;
	int count;

	count = sscanf(rte_version(), "DPDK %u.%u.%u", &year, &month, &minor);
	if (count != 3) {
		SPDK_ERRLOG("Unrecognized DPDK version format '%s'\n", rte_version());
		return -EINVAL;
	}

	/* Anything 23.x or higher is not supported. */
	if (year > 22) {
		SPDK_ERRLOG("DPDK version %d.%02d.%d not supported.\n", year, month, minor);
		return -EINVAL;
	}

	/* Anything greater than 22.07 is not supported. */
	if (year == 22 && month > 7) {
		SPDK_ERRLOG("DPDK version %d.%02d.%d not supported.\n", year, month, minor);
		return -EINVAL;
	}

	/* Everything else we use the 22.07 implementation. */
	g_dpdk_fn_table = &fn_table_2207;
	return 0;
}

uint64_t
dpdk_pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr)
{
	return g_dpdk_fn_table->pci_device_vtophys(dev, vaddr);
}

const char *
dpdk_pci_device_get_name(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_name(rte_dev);
}

struct rte_devargs *
dpdk_pci_device_get_devargs(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_devargs(rte_dev);
}

void
dpdk_pci_device_copy_identifiers(struct rte_pci_device *_dev, struct spdk_pci_device *dev)
{
	g_dpdk_fn_table->pci_device_copy_identifiers(_dev, dev);
}

int
dpdk_pci_device_map_bar(struct rte_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	return g_dpdk_fn_table->pci_device_map_bar(dev, bar, mapped_addr, phys_addr, size);
}

int
dpdk_pci_device_read_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return g_dpdk_fn_table->pci_device_read_config(dev, value, len, offset);
}

int
dpdk_pci_device_write_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return g_dpdk_fn_table->pci_device_write_config(dev, value, len, offset);
}

int
dpdk_pci_driver_register(struct spdk_pci_driver *driver,
			 int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			 int (*remove_fn)(struct rte_pci_device *device))

{
	return g_dpdk_fn_table->pci_driver_register(driver, probe_fn, remove_fn);
}

int
dpdk_pci_device_enable_interrupt(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_enable_interrupt(rte_dev);
}

int
dpdk_pci_device_disable_interrupt(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_disable_interrupt(rte_dev);
}

int
dpdk_pci_device_get_interrupt_efd(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_interrupt_efd(rte_dev);
}

int
dpdk_bus_probe(void)
{
	return g_dpdk_fn_table->bus_probe();
}

void
dpdk_bus_scan(void)
{
	g_dpdk_fn_table->bus_scan();
}

struct rte_devargs *
dpdk_device_get_devargs(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_get_devargs(dev);
}

void
dpdk_device_set_devargs(struct rte_device *dev, struct rte_devargs *devargs)
{
	g_dpdk_fn_table->device_set_devargs(dev, devargs);
}

const char *
dpdk_device_get_name(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_get_name(dev);
}

bool
dpdk_device_scan_allowed(struct rte_device *dev)
{
	return g_dpdk_fn_table->device_scan_allowed(dev);
}
