/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#define ALLOW_INTERNAL_API
#include <rte_config.h>
#include <rte_version.h>
#include "pci_dpdk.h"
#include "22.11/bus_pci_driver.h"
#include "22.11/bus_driver.h"
#include "22.11/rte_bus_pci.h"
#include "spdk/assert.h"

SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver_buf) == 0, "driver_buf must be first");
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver) >= sizeof(struct rte_pci_driver),
		   "driver_buf not big enough");

/* Following API was added in versions later than DPDK 22.11.
 * It is unused right now, if this changes a new pci_dpdk_* should be added.
 */
#define rte_pci_mmio_read(...) SPDK_STATIC_ASSERT(false, "rte_pci_mmio_read requires new pci_dpdk_2307 compat layer")
#define rte_pci_mmio_write(...) SPDK_STATIC_ASSERT(false, "rte_pci_mmio_write requires new pci_dpdk_2307 compat layer")
#define rte_pci_pasid_set_state(...) SPDK_STATIC_ASSERT(false, "rte_pci_pasid_set_state requires new pci_dpdk_2307 compat layer")

static struct rte_mem_resource *
pci_device_get_mem_resource_2211(struct rte_pci_device *dev, uint32_t bar)
{
	if (bar >= PCI_MAX_RESOURCE) {
		assert(false);
		return NULL;
	}

	return &dev->mem_resource[bar];
}

static const char *
pci_device_get_name_2211(struct rte_pci_device *rte_dev)
{
	return rte_dev->name;
}

static struct rte_devargs *
pci_device_get_devargs_2211(struct rte_pci_device *rte_dev)
{
	return rte_dev->device.devargs;
}

static struct rte_pci_addr *
pci_device_get_addr_2211(struct rte_pci_device *_dev)
{
	return &_dev->addr;
}

static struct rte_pci_id *
pci_device_get_id_2211(struct rte_pci_device *_dev)
{
	return &_dev->id;
}

static int
pci_device_get_numa_node_2211(struct rte_pci_device *_dev)
{
	return _dev->device.numa_node;
}

static int
pci_device_read_config_2211(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

	rc = rte_pci_read_config(dev, value, len, offset);

	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

static int
pci_device_write_config_2211(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

	rc = rte_pci_write_config(dev, value, len, offset);

#ifdef __FreeBSD__
	/* DPDK returns 0 on success and -1 on failure */
	return rc;
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

/* translate spdk_pci_driver to an rte_pci_driver and register it to dpdk */
static int
pci_driver_register_2211(struct spdk_pci_driver *driver,
			 int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			 int (*remove_fn)(struct rte_pci_device *device))

{
	unsigned pci_id_count = 0;
	struct rte_pci_id *rte_id_table;
	char *rte_name;
	size_t rte_name_len;
	uint32_t rte_flags;

	assert(driver->id_table);
	while (driver->id_table[pci_id_count].vendor_id) {
		pci_id_count++;
	}
	assert(pci_id_count > 0);

	rte_id_table = calloc(pci_id_count + 1, sizeof(*rte_id_table));
	if (!rte_id_table) {
		return -ENOMEM;
	}

	while (pci_id_count > 0) {
		struct rte_pci_id *rte_id = &rte_id_table[pci_id_count - 1];
		const struct spdk_pci_id *spdk_id = &driver->id_table[pci_id_count - 1];

		rte_id->class_id = spdk_id->class_id;
		rte_id->vendor_id = spdk_id->vendor_id;
		rte_id->device_id = spdk_id->device_id;
		rte_id->subsystem_vendor_id = spdk_id->subvendor_id;
		rte_id->subsystem_device_id = spdk_id->subdevice_id;
		pci_id_count--;
	}

	assert(driver->name);
	rte_name_len = strlen(driver->name) + strlen("spdk_") + 1;
	rte_name = calloc(rte_name_len, 1);
	if (!rte_name) {
		free(rte_id_table);
		return -ENOMEM;
	}

	snprintf(rte_name, rte_name_len, "spdk_%s", driver->name);
	driver->driver->driver.name = rte_name;
	driver->driver->id_table = rte_id_table;

	rte_flags = 0;
	if (driver->drv_flags & SPDK_PCI_DRIVER_NEED_MAPPING) {
		rte_flags |= RTE_PCI_DRV_NEED_MAPPING;
	}
	if (driver->drv_flags & SPDK_PCI_DRIVER_WC_ACTIVATE) {
		rte_flags |= RTE_PCI_DRV_WC_ACTIVATE;
	}
	driver->driver->drv_flags = rte_flags;

	driver->driver->probe = probe_fn;
	driver->driver->remove = remove_fn;

	rte_pci_register(driver->driver);
	return 0;
}

static int
pci_device_enable_interrupt_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_enable(rte_dev->intr_handle);
}

static int
pci_device_disable_interrupt_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_disable(rte_dev->intr_handle);
}

static int
pci_device_get_interrupt_efd_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_fd_get(rte_dev->intr_handle);
}

static int
pci_device_create_interrupt_efds_2211(struct rte_pci_device *rte_dev, uint32_t count)
{
	return rte_intr_efd_enable(rte_dev->intr_handle, count);
}

static void
pci_device_delete_interrupt_efds_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_efd_disable(rte_dev->intr_handle);
}

static int
pci_device_get_interrupt_efd_by_index_2211(struct rte_pci_device *rte_dev, uint32_t index)
{
	return rte_intr_efds_index_get(rte_dev->intr_handle, index);
}

static int
pci_device_interrupt_cap_multi_2211(struct rte_pci_device *rte_dev)
{
	return rte_intr_cap_multiple(rte_dev->intr_handle);
}

static int
bus_probe_2211(void)
{
	return rte_bus_probe();
}

static void
bus_scan_2211(void)
{
	rte_bus_scan();
}

static struct rte_devargs *
device_get_devargs_2211(struct rte_device *dev)
{
	return dev->devargs;
}

static void
device_set_devargs_2211(struct rte_device *dev, struct rte_devargs *devargs)
{
	dev->devargs = devargs;
}

static const char *
device_get_name_2211(struct rte_device *dev)
{
	return dev->name;
}

static bool
device_scan_allowed_2211(struct rte_device *dev)
{
	return dev->bus->conf.scan_mode == RTE_BUS_SCAN_ALLOWLIST;
}

struct dpdk_fn_table fn_table_2211 = {
	.pci_device_get_mem_resource	= pci_device_get_mem_resource_2211,
	.pci_device_get_name		= pci_device_get_name_2211,
	.pci_device_get_devargs		= pci_device_get_devargs_2211,
	.pci_device_get_addr		= pci_device_get_addr_2211,
	.pci_device_get_id		= pci_device_get_id_2211,
	.pci_device_get_numa_node	= pci_device_get_numa_node_2211,
	.pci_device_read_config		= pci_device_read_config_2211,
	.pci_device_write_config	= pci_device_write_config_2211,
	.pci_driver_register		= pci_driver_register_2211,
	.pci_device_enable_interrupt	= pci_device_enable_interrupt_2211,
	.pci_device_disable_interrupt	= pci_device_disable_interrupt_2211,
	.pci_device_get_interrupt_efd	= pci_device_get_interrupt_efd_2211,
	.pci_device_create_interrupt_efds = pci_device_create_interrupt_efds_2211,
	.pci_device_delete_interrupt_efds = pci_device_delete_interrupt_efds_2211,
	.pci_device_get_interrupt_efd_by_index = pci_device_get_interrupt_efd_by_index_2211,
	.pci_device_interrupt_cap_multi	= pci_device_interrupt_cap_multi_2211,
	.bus_scan			= bus_scan_2211,
	.bus_probe			= bus_probe_2211,
	.device_get_devargs		= device_get_devargs_2211,
	.device_set_devargs		= device_set_devargs_2211,
	.device_get_name		= device_get_name_2211,
	.device_scan_allowed		= device_scan_allowed_2211,
};
