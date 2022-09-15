/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include <rte_config.h>
#include <rte_version.h>
#include "pci_dpdk.h"
#include <rte_bus_pci.h>
#include "spdk/assert.h"

SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver_buf) == 0, "driver_buf must be first");
SPDK_STATIC_ASSERT(offsetof(struct spdk_pci_driver, driver) >= sizeof(struct rte_pci_driver),
		   "driver_buf not big enough");

uint64_t
dpdk_pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr)
{
	struct rte_mem_resource *res;
	uint64_t paddr;
	unsigned r;

	for (r = 0; r < PCI_MAX_RESOURCE; r++) {
		res = &dev->mem_resource[r];
		if (res->phys_addr && vaddr >= (uint64_t)res->addr &&
		    vaddr < (uint64_t)res->addr + res->len) {
			paddr = res->phys_addr + (vaddr - (uint64_t)res->addr);
			return paddr;
		}
	}

	return SPDK_VTOPHYS_ERROR;
}

const char *
dpdk_pci_device_get_name(struct rte_pci_device *rte_dev)
{
	return rte_dev->name;
}

struct rte_devargs *
dpdk_pci_device_get_devargs(struct rte_pci_device *rte_dev)
{
	return rte_dev->device.devargs;
}

void
dpdk_pci_device_copy_identifiers(struct rte_pci_device *_dev, struct spdk_pci_device *dev)
{
	dev->addr.domain = _dev->addr.domain;
	dev->addr.bus = _dev->addr.bus;
	dev->addr.dev = _dev->addr.devid;
	dev->addr.func = _dev->addr.function;
	dev->id.class_id = _dev->id.class_id;
	dev->id.vendor_id = _dev->id.vendor_id;
	dev->id.device_id = _dev->id.device_id;
	dev->id.subvendor_id = _dev->id.subsystem_vendor_id;
	dev->id.subdevice_id = _dev->id.subsystem_device_id;
	dev->socket_id = _dev->device.numa_node;
}

int
dpdk_pci_device_map_bar(struct rte_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	*mapped_addr = dev->mem_resource[bar].addr;
	*phys_addr = (uint64_t)dev->mem_resource[bar].phys_addr;
	*size = (uint64_t)dev->mem_resource[bar].len;

	return 0;
}

int
dpdk_pci_device_read_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

	rc = rte_pci_read_config(dev, value, len, offset);

	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

int
dpdk_pci_device_write_config(struct rte_pci_device *dev, void *value, uint32_t len, uint32_t offset)
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
int
dpdk_pci_driver_register(struct spdk_pci_driver *driver,
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

int
dpdk_pci_device_enable_interrupt(struct rte_pci_device *rte_dev)
{
#if RTE_VERSION < RTE_VERSION_NUM(21, 11, 0, 0)
	return rte_intr_enable(&rte_dev->intr_handle);
#else
	return rte_intr_enable(rte_dev->intr_handle);
#endif
}

int
dpdk_pci_device_disable_interrupt(struct rte_pci_device *rte_dev)
{
#if RTE_VERSION < RTE_VERSION_NUM(21, 11, 0, 0)
	return rte_intr_disable(&rte_dev->intr_handle);
#else
	return rte_intr_disable(rte_dev->intr_handle);
#endif
}

int
dpdk_pci_device_get_interrupt_efd(struct rte_pci_device *rte_dev)
{
#if RTE_VERSION < RTE_VERSION_NUM(21, 11, 0, 0)
	return rte_dev->intr_handle.fd;
#else
	return rte_intr_fd_get(rte_dev->intr_handle);
#endif
}

int
dpdk_bus_probe(void)
{
	return rte_bus_probe();
}

void
dpdk_bus_scan(void)
{
	rte_bus_scan();
}

struct rte_devargs *
dpdk_device_get_devargs(struct rte_device *dev)
{
	return dev->devargs;
}

void
dpdk_device_set_devargs(struct rte_device *dev, struct rte_devargs *devargs)
{
	dev->devargs = devargs;
}

const char *
dpdk_device_get_name(struct rte_device *dev)
{
	return dev->name;
}

bool
dpdk_device_scan_allowed(struct rte_device *dev)
{
	return dev->bus->conf.scan_mode == RTE_BUS_SCAN_ALLOWLIST;
}
