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

#include <rte_alarm.h>
#include "spdk/env.h"

#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"

#define PCI_CFG_SIZE		256
#define PCI_EXT_CAP_ID_SN	0x03

/* DPDK 18.11+ hotplug isn't robust. Multiple apps starting at the same time
 * might cause the internal IPC to misbehave. Just retry in such case.
 */
#define DPDK_HOTPLUG_RETRY_COUNT 4

static pthread_mutex_t g_pci_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);
static TAILQ_HEAD(, spdk_pci_driver) g_pci_drivers = TAILQ_HEAD_INITIALIZER(g_pci_drivers);

static int
spdk_map_bar_rte(struct spdk_pci_device *device, uint32_t bar,
		 void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct rte_pci_device *dev = device->dev_handle;

	*mapped_addr = dev->mem_resource[bar].addr;
	*phys_addr = (uint64_t)dev->mem_resource[bar].phys_addr;
	*size = (uint64_t)dev->mem_resource[bar].len;

	return 0;
}

static int
spdk_unmap_bar_rte(struct spdk_pci_device *device, uint32_t bar, void *addr)
{
	return 0;
}

static int
spdk_cfg_read_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

	rc = rte_pci_read_config(dev->dev_handle, value, len, offset);

#if defined(__FreeBSD__) && RTE_VERSION < RTE_VERSION_NUM(18, 11, 0, 0)
	/* Older DPDKs return 0 on success and -1 on failure */
	return rc;
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

static int
spdk_cfg_write_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

	rc = rte_pci_write_config(dev->dev_handle, value, len, offset);

#ifdef __FreeBSD__
	/* DPDK returns 0 on success and -1 on failure */
	return rc;
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

static void
spdk_detach_rte_cb(void *_dev)
{
	struct rte_pci_device *rte_dev = _dev;

#if RTE_VERSION >= RTE_VERSION_NUM(18, 11, 0, 0)
	char bdf[32];
	int i = 0, rc;

	snprintf(bdf, sizeof(bdf), "%s", rte_dev->device.name);
	do {
		rc = rte_eal_hotplug_remove("pci", bdf);
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);
#else
	rte_eal_dev_detach(&rte_dev->device);
#endif
}

static void
spdk_detach_rte(struct spdk_pci_device *dev)
{
	/* The device was already marked as available and could be attached
	 * again while we go asynchronous, so we explicitly forbid that.
	 */
	dev->internal.pending_removal = true;
	if (spdk_process_is_primary()) {
		rte_eal_alarm_set(10, spdk_detach_rte_cb, dev->dev_handle);
	} else {
		spdk_detach_rte_cb(dev->dev_handle);
	}
}

void
spdk_pci_driver_register(struct spdk_pci_driver *driver)
{
	TAILQ_INSERT_TAIL(&g_pci_drivers, driver, tailq);
}

#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
static void
spdk_pci_device_rte_hotremove(const char *device_name,
			      enum rte_dev_event_type event,
			      void *cb_arg)
{
	struct spdk_pci_device *dev;

	if (event != RTE_DEV_EVENT_REMOVE) {
		return;
	}

	pthread_mutex_lock(&g_pci_mutex);
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		struct rte_pci_device *rte_dev = dev->dev_handle;

		if (strcmp(rte_dev->name, device_name) == 0) {
			if (!dev->internal.pending_removal &&
			    !dev->internal.attached) {
				/* if device is not attached, we
				 * can remove it right away.
				 */
				spdk_detach_rte(dev);
			} else {
				/* otherwise we let the upper layers
				 * detach it first.
				 */
				dev->internal.pending_removal = true;
			}
			break;
		}
	}
	pthread_mutex_unlock(&g_pci_mutex);
}
#endif

void
spdk_pci_init(void)
{
#if RTE_VERSION >= RTE_VERSION_NUM(18, 11, 0, 0)
	struct spdk_pci_driver *driver;

	/* We need to pre-register pci drivers for the pci devices to be
	 * attachable in multi-process with DPDK 18.11+.
	 *
	 * DPDK 18.11+ does its best to ensure all devices are equally
	 * attached or detached in all processes within a shared memory group.
	 * For SPDK it means that if a device is hotplugged in the primary,
	 * then DPDK will automatically send an IPC hotplug request to all other
	 * processes. Those other processes may not have the same SPDK PCI
	 * driver registered and may fail to attach the device. DPDK will send
	 * back the failure status, and the the primary process will also fail
	 * to hotplug the device. To prevent that, we need to pre-register the
	 * pci drivers here.
	 */
	TAILQ_FOREACH(driver, &g_pci_drivers, tailq) {
		assert(!driver->is_registered);
		driver->is_registered = true;
		rte_pci_register(&driver->driver);
	}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
	/* Register a single hotremove callback for all devices. */
	if (spdk_process_is_primary()) {
		rte_dev_event_callback_register(NULL, spdk_pci_device_rte_hotremove, NULL);
	}
#endif
}

void
spdk_pci_fini(void)
{
	struct spdk_pci_device *dev;
	char bdf[32];

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->internal.attached) {
			spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->addr);
			fprintf(stderr, "Device %s is still attached at shutdown!\n", bdf);
		}
	}

#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
	if (spdk_process_is_primary()) {
		rte_dev_event_callback_unregister(NULL, spdk_pci_device_rte_hotremove, NULL);
	}
#endif
}

int
spdk_pci_device_init(struct rte_pci_driver *_drv,
		     struct rte_pci_device *_dev)
{
	struct spdk_pci_driver *driver = (struct spdk_pci_driver *)_drv;
	struct spdk_pci_device *dev;
	int rc;

#if RTE_VERSION < RTE_VERSION_NUM(18, 11, 0, 0)
	if (!driver->cb_fn) {
		/* Return a positive value to indicate that this device does
		 * not belong to this driver, but this isn't an error.
		 */
		return 1;
	}
#endif

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -1;
	}

	dev->dev_handle = _dev;

	dev->addr.domain = _dev->addr.domain;
	dev->addr.bus = _dev->addr.bus;
	dev->addr.dev = _dev->addr.devid;
	dev->addr.func = _dev->addr.function;
	dev->id.vendor_id = _dev->id.vendor_id;
	dev->id.device_id = _dev->id.device_id;
	dev->id.subvendor_id = _dev->id.subsystem_vendor_id;
	dev->id.subdevice_id = _dev->id.subsystem_device_id;
	dev->socket_id = _dev->device.numa_node;

	dev->map_bar = spdk_map_bar_rte;
	dev->unmap_bar = spdk_unmap_bar_rte;
	dev->cfg_read = spdk_cfg_read_rte;
	dev->cfg_write = spdk_cfg_write_rte;
	dev->detach = spdk_detach_rte;

	dev->internal.driver = driver;

	if (driver->cb_fn != NULL) {
		rc = driver->cb_fn(driver->cb_arg, dev);
		if (rc != 0) {
			free(dev);
			return rc;
		}
		dev->internal.attached = true;
	}

	TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);
	spdk_vtophys_pci_device_added(dev->dev_handle);
	return 0;
}

int
spdk_pci_device_fini(struct rte_pci_device *_dev)
{
	struct spdk_pci_device *dev;

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->dev_handle == _dev) {
			break;
		}
	}

	if (dev == NULL || dev->internal.attached) {
		/* The device might be still referenced somewhere in SPDK. */
		return -1;
	}

	spdk_vtophys_pci_device_removed(dev->dev_handle);
	TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
	free(dev);
	return 0;

}

void
spdk_pci_device_detach(struct spdk_pci_device *dev)
{
	assert(dev->internal.attached);
	dev->internal.attached = false;
	dev->detach(dev);
}

int
spdk_pci_device_attach(struct spdk_pci_driver *driver,
		       spdk_pci_enum_cb enum_cb,
		       void *enum_ctx, struct spdk_pci_addr *pci_address)
{
	struct spdk_pci_device *dev;
	int rc;
	char bdf[32];

	spdk_pci_addr_fmt(bdf, sizeof(bdf), pci_address);

	pthread_mutex_lock(&g_pci_mutex);

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (spdk_pci_addr_compare(&dev->addr, pci_address) == 0) {
			break;
		}
	}

	if (dev != NULL && dev->internal.driver == driver) {
		if (dev->internal.attached || dev->internal.pending_removal) {
			pthread_mutex_unlock(&g_pci_mutex);
			return -1;
		}

		rc = enum_cb(enum_ctx, dev);
		if (rc == 0) {
			dev->internal.attached = true;
		}
		pthread_mutex_unlock(&g_pci_mutex);
		return rc;
	}

	if (!driver->is_registered) {
		driver->is_registered = true;
		rte_pci_register(&driver->driver);
	}

	driver->cb_fn = enum_cb;
	driver->cb_arg = enum_ctx;

#if RTE_VERSION >= RTE_VERSION_NUM(18, 11, 0, 0)
	int i = 0;

	do {
		rc = rte_eal_hotplug_add("pci", bdf, "");
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);

	if (i > 1 && rc == -EEXIST) {
		/* Even though the previous request timed out, the device
		 * was attached successfully.
		 */
		rc = 0;
	}
#else
	rc = rte_eal_dev_attach(bdf, "");
#endif

	driver->cb_arg = NULL;
	driver->cb_fn = NULL;
	pthread_mutex_unlock(&g_pci_mutex);

	return rc == 0 ? 0 : -1;
}

/* Note: You can call spdk_pci_enumerate from more than one thread
 *       simultaneously safely, but you cannot call spdk_pci_enumerate
 *       and rte_eal_pci_probe simultaneously.
 */
int
spdk_pci_enumerate(struct spdk_pci_driver *driver,
		   spdk_pci_enum_cb enum_cb,
		   void *enum_ctx)
{
	struct spdk_pci_device *dev;
	int rc;

	pthread_mutex_lock(&g_pci_mutex);

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->internal.attached ||
		    dev->internal.driver != driver ||
		    dev->internal.pending_removal) {
			continue;
		}

		rc = enum_cb(enum_ctx, dev);
		if (rc == 0) {
			dev->internal.attached = true;
		} else if (rc < 0) {
			pthread_mutex_unlock(&g_pci_mutex);
			return -1;
		}
	}

	if (!driver->is_registered) {
		driver->is_registered = true;
		rte_pci_register(&driver->driver);
	}

	driver->cb_fn = enum_cb;
	driver->cb_arg = enum_ctx;

	if (rte_bus_scan() != 0 || rte_bus_probe() != 0) {
		driver->cb_arg = NULL;
		driver->cb_fn = NULL;
		pthread_mutex_unlock(&g_pci_mutex);
		return -1;
	}

	driver->cb_arg = NULL;
	driver->cb_fn = NULL;
	pthread_mutex_unlock(&g_pci_mutex);

	return 0;
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	return dev->map_bar(dev, bar, mapped_addr, phys_addr, size);
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
	return dev->unmap_bar(dev, bar, addr);
}

uint32_t
spdk_pci_device_get_domain(struct spdk_pci_device *dev)
{
	return dev->addr.domain;
}

uint8_t
spdk_pci_device_get_bus(struct spdk_pci_device *dev)
{
	return dev->addr.bus;
}

uint8_t
spdk_pci_device_get_dev(struct spdk_pci_device *dev)
{
	return dev->addr.dev;
}

uint8_t
spdk_pci_device_get_func(struct spdk_pci_device *dev)
{
	return dev->addr.func;
}

uint16_t
spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev)
{
	return dev->id.vendor_id;
}

uint16_t
spdk_pci_device_get_device_id(struct spdk_pci_device *dev)
{
	return dev->id.device_id;
}

uint16_t
spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev)
{
	return dev->id.subvendor_id;
}

uint16_t
spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev)
{
	return dev->id.subdevice_id;
}

struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *dev)
{
	return dev->id;
}

int
spdk_pci_device_get_socket_id(struct spdk_pci_device *dev)
{
	return dev->socket_id;
}

int
spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return dev->cfg_read(dev, value, len, offset);
}

int
spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return dev->cfg_write(dev, value, len, offset);
}

int
spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset)
{
	return spdk_pci_device_cfg_read(dev, value, 1, offset);
}

int
spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset)
{
	return spdk_pci_device_cfg_write(dev, &value, 1, offset);
}

int
spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset)
{
	return spdk_pci_device_cfg_read(dev, value, 2, offset);
}

int
spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset)
{
	return spdk_pci_device_cfg_write(dev, &value, 2, offset);
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset)
{
	return spdk_pci_device_cfg_read(dev, value, 4, offset);
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset)
{
	return spdk_pci_device_cfg_write(dev, &value, 4, offset);
}

int
spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len)
{
	int err;
	uint32_t pos, header = 0;
	uint32_t i, buf[2];

	if (len < 17) {
		return -1;
	}

	err = spdk_pci_device_cfg_read32(dev, &header, PCI_CFG_SIZE);
	if (err || !header) {
		return -1;
	}

	pos = PCI_CFG_SIZE;
	while (1) {
		if ((header & 0x0000ffff) == PCI_EXT_CAP_ID_SN) {
			if (pos) {
				/* skip the header */
				pos += 4;
				for (i = 0; i < 2; i++) {
					err = spdk_pci_device_cfg_read32(dev, &buf[i], pos + 4 * i);
					if (err) {
						return -1;
					}
				}
				snprintf(sn, len, "%08x%08x", buf[1], buf[0]);
				return 0;
			}
		}
		pos = (header >> 20) & 0xffc;
		/* 0 if no other items exist */
		if (pos < PCI_CFG_SIZE) {
			return -1;
		}
		err = spdk_pci_device_cfg_read32(dev, &header, pos);
		if (err) {
			return -1;
		}
	}
	return -1;
}

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *dev)
{
	return dev->addr;
}

bool
spdk_pci_device_is_removed(struct spdk_pci_device *dev)
{
	return dev->internal.pending_removal;
}

int
spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2)
{
	if (a1->domain > a2->domain) {
		return 1;
	} else if (a1->domain < a2->domain) {
		return -1;
	} else if (a1->bus > a2->bus) {
		return 1;
	} else if (a1->bus < a2->bus) {
		return -1;
	} else if (a1->dev > a2->dev) {
		return 1;
	} else if (a1->dev < a2->dev) {
		return -1;
	} else if (a1->func > a2->func) {
		return 1;
	} else if (a1->func < a2->func) {
		return -1;
	}

	return 0;
}

#ifdef __linux__
int
spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr)
{
	int dev_fd;
	char dev_name[64];
	int pid;
	void *dev_map;
	struct flock pcidev_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	snprintf(dev_name, sizeof(dev_name), "/tmp/spdk_pci_lock_%04x:%02x:%02x.%x", pci_addr->domain,
		 pci_addr->bus,
		 pci_addr->dev, pci_addr->func);

	dev_fd = open(dev_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (dev_fd == -1) {
		fprintf(stderr, "could not open %s\n", dev_name);
		return -1;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		fprintf(stderr, "could not truncate %s\n", dev_name);
		close(dev_fd);
		return -1;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	if (dev_map == MAP_FAILED) {
		fprintf(stderr, "could not mmap dev %s (%d)\n", dev_name, errno);
		close(dev_fd);
		return -1;
	}

	if (fcntl(dev_fd, F_SETLK, &pcidev_lock) != 0) {
		pid = *(int *)dev_map;
		fprintf(stderr, "Cannot create lock on device %s, probably"
			" process %d has claimed it\n", dev_name, pid);
		munmap(dev_map, sizeof(int));
		close(dev_fd);
		return -1;
	}

	*(int *)dev_map = (int)getpid();
	munmap(dev_map, sizeof(int));
	/* Keep dev_fd open to maintain the lock. */
	return dev_fd;
}
#endif /* __linux__ */

#ifdef __FreeBSD__
int
spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr)
{
	/* TODO */
	return 0;
}
#endif /* __FreeBSD__ */

int
spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf)
{
	unsigned domain, bus, dev, func;

	if (addr == NULL || bdf == NULL) {
		return -EINVAL;
	}

	if ((sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) ||
	    (sscanf(bdf, "%x.%x.%x.%x", &domain, &bus, &dev, &func) == 4)) {
		/* Matched a full address - all variables are initialized */
	} else if (sscanf(bdf, "%x:%x:%x", &domain, &bus, &dev) == 3) {
		func = 0;
	} else if ((sscanf(bdf, "%x:%x.%x", &bus, &dev, &func) == 3) ||
		   (sscanf(bdf, "%x.%x.%x", &bus, &dev, &func) == 3)) {
		domain = 0;
	} else if ((sscanf(bdf, "%x:%x", &bus, &dev) == 2) ||
		   (sscanf(bdf, "%x.%x", &bus, &dev) == 2)) {
		domain = 0;
		func = 0;
	} else {
		return -EINVAL;
	}

	if (bus > 0xFF || dev > 0x1F || func > 7) {
		return -EINVAL;
	}

	addr->domain = domain;
	addr->bus = bus;
	addr->dev = dev;
	addr->func = func;

	return 0;
}

int
spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr)
{
	int rc;

	rc = snprintf(bdf, sz, "%04x:%02x:%02x.%x",
		      addr->domain, addr->bus,
		      addr->dev, addr->func);

	if (rc > 0 && (size_t)rc < sz) {
		return 0;
	}

	return -1;
}

void
spdk_pci_hook_device(struct spdk_pci_driver *drv, struct spdk_pci_device *dev)
{
	assert(dev->map_bar != NULL);
	assert(dev->unmap_bar != NULL);
	assert(dev->cfg_read != NULL);
	assert(dev->cfg_write != NULL);
	assert(dev->detach != NULL);
	dev->internal.driver = drv;
	TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);
}

void
spdk_pci_unhook_device(struct spdk_pci_device *dev)
{
	assert(!dev->internal.attached);
	TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
}
