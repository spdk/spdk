/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_PCI_DPDK_H
#define SPDK_PCI_DPDK_H

#include "spdk/env.h"

#if RTE_VERSION < RTE_VERSION_NUM(21, 11, 0, 0)
#define RTE_TAILQ_HEAD(name, type) TAILQ_HEAD(name, type)
#define RTE_TAILQ_ENTRY(type) TAILQ_ENTRY(type)
#define RTE_TAILQ_FOREACH(var, head, field) TAILQ_FOREACH(var, head, field)
#endif

struct spdk_pci_driver {
	uint8_t				driver_buf[256];
	struct rte_pci_driver		*driver;

	const char                      *name;
	const struct spdk_pci_id	*id_table;
	uint32_t			drv_flags;

	spdk_pci_enum_cb		cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(spdk_pci_driver)	tailq;
};

struct rte_pci_device;
struct rte_pci_driver;
struct rte_device;

struct dpdk_fn_table {
	struct rte_mem_resource *(*pci_device_get_mem_resource)(struct rte_pci_device *dev, uint32_t bar);
	const char *(*pci_device_get_name)(struct rte_pci_device *);
	struct rte_devargs *(*pci_device_get_devargs)(struct rte_pci_device *);
	struct rte_pci_addr *(*pci_device_get_addr)(struct rte_pci_device *);
	struct rte_pci_id *(*pci_device_get_id)(struct rte_pci_device *);
	int (*pci_device_get_numa_node)(struct rte_pci_device *_dev);
	int (*pci_device_read_config)(struct rte_pci_device *dev, void *value, uint32_t len,
				      uint32_t offset);
	int (*pci_device_write_config)(struct rte_pci_device *dev, void *value, uint32_t len,
				       uint32_t offset);
	int (*pci_driver_register)(struct spdk_pci_driver *driver,
				   int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
				   int (*remove_fn)(struct rte_pci_device *device));
	int (*pci_device_enable_interrupt)(struct rte_pci_device *rte_dev);
	int (*pci_device_disable_interrupt)(struct rte_pci_device *rte_dev);
	int (*pci_device_get_interrupt_efd)(struct rte_pci_device *rte_dev);
	void (*bus_scan)(void);
	int (*bus_probe)(void);
	struct rte_devargs *(*device_get_devargs)(struct rte_device *dev);
	void (*device_set_devargs)(struct rte_device *dev, struct rte_devargs *devargs);
	const char *(*device_get_name)(struct rte_device *dev);
	bool (*device_scan_allowed)(struct rte_device *dev);
};

int dpdk_pci_init(void);

struct rte_mem_resource *dpdk_pci_device_get_mem_resource(struct rte_pci_device *dev, uint32_t bar);
uint64_t dpdk_pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr, size_t len);
const char *dpdk_pci_device_get_name(struct rte_pci_device *);
struct rte_devargs *dpdk_pci_device_get_devargs(struct rte_pci_device *);
struct rte_pci_addr *dpdk_pci_device_get_addr(struct rte_pci_device *);
struct rte_pci_id *dpdk_pci_device_get_id(struct rte_pci_device *);
int dpdk_pci_device_get_numa_node(struct rte_pci_device *_dev);
int dpdk_pci_device_read_config(struct rte_pci_device *dev, void *value, uint32_t len,
				uint32_t offset);
int dpdk_pci_device_write_config(struct rte_pci_device *dev, void *value, uint32_t len,
				 uint32_t offset);
int dpdk_pci_driver_register(struct spdk_pci_driver *driver,
			     int (*probe_fn)(struct rte_pci_driver *driver, struct rte_pci_device *device),
			     int (*remove_fn)(struct rte_pci_device *device));
int dpdk_pci_device_enable_interrupt(struct rte_pci_device *rte_dev);
int dpdk_pci_device_disable_interrupt(struct rte_pci_device *rte_dev);
int dpdk_pci_device_get_interrupt_efd(struct rte_pci_device *rte_dev);
void dpdk_bus_scan(void);
int dpdk_bus_probe(void);
struct rte_devargs *dpdk_device_get_devargs(struct rte_device *dev);
void dpdk_device_set_devargs(struct rte_device *dev, struct rte_devargs *devargs);
const char *dpdk_device_get_name(struct rte_device *dev);
bool dpdk_device_scan_allowed(struct rte_device *dev);

#endif /* ifndef SPDK_PCI_DPDK_H */
