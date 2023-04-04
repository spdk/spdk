/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <rte_config.h>
#include <rte_version.h>
#include "pci_dpdk.h"
#include "spdk/log.h"

extern struct dpdk_fn_table fn_table_2207;
extern struct dpdk_fn_table fn_table_2211;

static struct dpdk_fn_table *g_dpdk_fn_table;

int
dpdk_pci_init(void)
{
	uint32_t year;
	uint32_t month;
	uint32_t minor;
	char release[32] = {0}; /* Max size of DPDK version string */
	int count;

	count = sscanf(rte_version(), "DPDK %u.%u.%u%s", &year, &month, &minor, release);
	if (count != 3 && count != 4) {
		SPDK_ERRLOG("Unrecognized DPDK version format '%s'\n", rte_version());
		return -EINVAL;
	}

	/* Add support for DPDK main branch.
	 * Only DPDK in development has additional suffix past minor version.
	 */
	if (strlen(release) != 0) {
		if (year == 23 && month == 7 && minor == 0) {
			g_dpdk_fn_table = &fn_table_2211;
			SPDK_NOTICELOG("DPDK version 23.07.0 not supported yet. Enabled only for validation.\n");
			return 0;
		}
	}

	/* Anything 24.x or higher is not supported. */
	if (year > 23) {
		SPDK_ERRLOG("DPDK version %d.%02d.%d not supported.\n", year, month, minor);
		return -EINVAL;
	}

	if (year == 22 && month == 11) {
		if (minor > 1) {
			/* It is possible that LTS minor release changed private ABI, so we
			 * cannot assume fn_table_2211 works for minor releases.  As 22.11
			 * minor releases occur, this will need to be updated to either affirm
			 * no ABI changes for the minor release, or add new header files and
			 * pci_dpdk_xxx.c implementation for the new minor release.
			 */
			SPDK_ERRLOG("DPDK LTS version 22.11.%d not supported.\n", minor);
			return -EINVAL;
		}
		g_dpdk_fn_table = &fn_table_2211;
	} else if (year == 23) {
		/* Only 23.03.0 is supported */
		if (month != 3 || minor != 0) {
			SPDK_ERRLOG("DPDK version 23.%02d.%d is not supported.\n", month, minor);
			return -EINVAL;
		}
		/* There were no changes between 22.11 and 23.03, so use the 22.11 implementation */
		g_dpdk_fn_table = &fn_table_2211;
	} else {
		/* Everything else we use the 22.07 implementation. */
		g_dpdk_fn_table = &fn_table_2207;
	}
	return 0;
}

struct rte_mem_resource *
dpdk_pci_device_get_mem_resource(struct rte_pci_device *dev, uint32_t bar)
{
	return g_dpdk_fn_table->pci_device_get_mem_resource(dev, bar);
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

struct rte_pci_addr *
dpdk_pci_device_get_addr(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_addr(rte_dev);
}

struct rte_pci_id *
dpdk_pci_device_get_id(struct rte_pci_device *rte_dev)
{
	return g_dpdk_fn_table->pci_device_get_id(rte_dev);
}

int
dpdk_pci_device_get_numa_node(struct rte_pci_device *_dev)
{
	return g_dpdk_fn_table->pci_device_get_numa_node(_dev);
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
