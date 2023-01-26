/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#include "env_internal.h"
#include "pci_dpdk.h"

#include <rte_alarm.h>
#include <rte_devargs.h>
#include <rte_pci.h>
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/memory.h"

#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"

/* Compatibility for versions < 20.11 */
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 0, 0)
#define RTE_DEV_ALLOWED RTE_DEV_WHITELISTED
#define RTE_DEV_BLOCKED RTE_DEV_BLACKLISTED
#define RTE_BUS_SCAN_ALLOWLIST RTE_BUS_SCAN_WHITELIST
#endif

#define PCI_CFG_SIZE		256
#define PCI_EXT_CAP_ID_SN	0x03

/* DPDK 18.11+ hotplug isn't robust. Multiple apps starting at the same time
 * might cause the internal IPC to misbehave. Just retry in such case.
 */
#define DPDK_HOTPLUG_RETRY_COUNT 4

/* DPDK alarm/interrupt thread */
static pthread_mutex_t g_pci_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);
/* devices hotplugged on a dpdk thread */
static TAILQ_HEAD(, spdk_pci_device) g_pci_hotplugged_devices =
	TAILQ_HEAD_INITIALIZER(g_pci_hotplugged_devices);
static TAILQ_HEAD(, spdk_pci_driver) g_pci_drivers = TAILQ_HEAD_INITIALIZER(g_pci_drivers);
static TAILQ_HEAD(, spdk_pci_device_provider) g_pci_device_providers =
	TAILQ_HEAD_INITIALIZER(g_pci_device_providers);

int pci_device_init(struct rte_pci_driver *driver, struct rte_pci_device *device);
int pci_device_fini(struct rte_pci_device *device);

struct env_devargs {
	struct rte_bus	*bus;
	char		name[128];
	uint64_t	allowed_at;
	TAILQ_ENTRY(env_devargs) link;
};
static TAILQ_HEAD(, env_devargs) g_env_devargs = TAILQ_HEAD_INITIALIZER(g_env_devargs);

static struct env_devargs *
find_env_devargs(struct rte_bus *bus, const char *name)
{
	struct env_devargs *da;

	TAILQ_FOREACH(da, &g_env_devargs, link) {
		if (bus == da->bus && !strcmp(name, da->name)) {
			return da;
		}
	}

	return NULL;
}

static int
map_bar_rte(struct spdk_pci_device *device, uint32_t bar,
	    void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct rte_mem_resource *res;

	res = dpdk_pci_device_get_mem_resource(device->dev_handle, bar);
	*mapped_addr = res->addr;
	*phys_addr = (uint64_t)res->phys_addr;
	*size = (uint64_t)res->len;

	return 0;
}

static int
unmap_bar_rte(struct spdk_pci_device *device, uint32_t bar, void *addr)
{
	return 0;
}

static int
cfg_read_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return dpdk_pci_device_read_config(dev->dev_handle, value, len, offset);
}

static int
cfg_write_rte(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	return dpdk_pci_device_write_config(dev->dev_handle, value, len, offset);
}

static void
remove_rte_dev(struct rte_pci_device *rte_dev)
{
	char bdf[32];
	int i = 0, rc;

	snprintf(bdf, sizeof(bdf), "%s", dpdk_pci_device_get_name(rte_dev));
	do {
		rc = rte_eal_hotplug_remove("pci", bdf);
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);
}

static void
detach_rte_cb(void *_dev)
{
	remove_rte_dev(_dev);
}

/* if it's a physical device we need to deal with DPDK on
 * a different process and we can't just unset one flag
 * here. We also want to stop using any device resources
 * so that the device isn't "in use" by the userspace driver
 * once we detach it. This would allow attaching the device
 * to a different process, or to a kernel driver like nvme.
 */
static void
detach_rte(struct spdk_pci_device *dev)
{
	struct rte_pci_device *rte_dev = dev->dev_handle;
	int i;
	bool removed;

	if (!spdk_process_is_primary()) {
		return;
	}

	pthread_mutex_lock(&g_pci_mutex);
	dev->internal.attached = false;
	/* prevent the hotremove notification from removing this device */
	dev->internal.pending_removal = true;
	pthread_mutex_unlock(&g_pci_mutex);

	rte_eal_alarm_set(1, detach_rte_cb, rte_dev);

	/* wait up to 2s for the cb to execute */
	for (i = 2000; i > 0; i--) {

		spdk_delay_us(1000);
		pthread_mutex_lock(&g_pci_mutex);
		removed = dev->internal.removed;
		pthread_mutex_unlock(&g_pci_mutex);

		if (removed) {
			break;
		}
	}

	/* besides checking the removed flag, we also need to wait
	 * for the dpdk detach function to unwind, as it's doing some
	 * operations even after calling our detach callback. Simply
	 * cancel the alarm - if it started executing already, this
	 * call will block and wait for it to finish.
	 */
	rte_eal_alarm_cancel(detach_rte_cb, rte_dev);

	/* the device could have been finally removed, so just check
	 * it again.
	 */
	pthread_mutex_lock(&g_pci_mutex);
	removed = dev->internal.removed;
	pthread_mutex_unlock(&g_pci_mutex);
	if (!removed) {
		SPDK_ERRLOG("Timeout waiting for DPDK to remove PCI device %s.\n",
			    dpdk_pci_device_get_name(rte_dev));
		/* If we reach this state, then the device couldn't be removed and most likely
		   a subsequent hot add of a device in the same BDF will fail */
	}
}

void
spdk_pci_driver_register(const char *name, struct spdk_pci_id *id_table, uint32_t flags)
{
	struct spdk_pci_driver *driver;

	driver = calloc(1, sizeof(*driver));
	if (!driver) {
		/* we can't do any better than bailing atm */
		return;
	}

	driver->name = name;
	driver->id_table = id_table;
	driver->drv_flags = flags;
	driver->driver = (struct rte_pci_driver *)driver->driver_buf;
	TAILQ_INSERT_TAIL(&g_pci_drivers, driver, tailq);
}

struct spdk_pci_driver *
spdk_pci_nvme_get_driver(void)
{
	return spdk_pci_get_driver("nvme");
}

struct spdk_pci_driver *
spdk_pci_get_driver(const char *name)
{
	struct spdk_pci_driver *driver;

	TAILQ_FOREACH(driver, &g_pci_drivers, tailq) {
		if (strcmp(driver->name, name) == 0) {
			return driver;
		}
	}

	return NULL;
}

static void
pci_device_rte_dev_event(const char *device_name,
			 enum rte_dev_event_type event,
			 void *cb_arg)
{
	struct spdk_pci_device *dev;
	bool can_detach = false;

	switch (event) {
	default:
	case RTE_DEV_EVENT_ADD:
		/* Nothing to do here yet. */
		break;
	case RTE_DEV_EVENT_REMOVE:
		pthread_mutex_lock(&g_pci_mutex);
		TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
			struct rte_pci_device *rte_dev = dev->dev_handle;

			if (strcmp(dpdk_pci_device_get_name(rte_dev), device_name) == 0 &&
			    !dev->internal.pending_removal) {
				can_detach = !dev->internal.attached;
				/* prevent any further attaches */
				dev->internal.pending_removal = true;
				break;
			}
		}
		pthread_mutex_unlock(&g_pci_mutex);

		if (dev != NULL && can_detach) {
			/* if device is not attached we can remove it right away.
			 * Otherwise it will be removed at detach.
			 *
			 * Because the user's callback is invoked in eal interrupt
			 * callback, the interrupt callback need to be finished before
			 * it can be unregistered when detaching device. So finish
			 * callback soon and use a deferred removal to detach device
			 * is need. It is a workaround, once the device detaching be
			 * moved into the eal in the future, the deferred removal could
			 * be deleted.
			 */
			rte_eal_alarm_set(1, detach_rte_cb, dev->dev_handle);
		}
		break;
	}
}

static void
cleanup_pci_devices(void)
{
	struct spdk_pci_device *dev, *tmp;

	pthread_mutex_lock(&g_pci_mutex);
	/* cleanup removed devices */
	TAILQ_FOREACH_SAFE(dev, &g_pci_devices, internal.tailq, tmp) {
		if (!dev->internal.removed) {
			continue;
		}

		vtophys_pci_device_removed(dev->dev_handle);
		TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
		free(dev);
	}

	/* add newly-attached devices */
	TAILQ_FOREACH_SAFE(dev, &g_pci_hotplugged_devices, internal.tailq, tmp) {
		TAILQ_REMOVE(&g_pci_hotplugged_devices, dev, internal.tailq);
		TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);
		vtophys_pci_device_added(dev->dev_handle);
	}
	pthread_mutex_unlock(&g_pci_mutex);
}

static int scan_pci_bus(bool delay_init);

static inline void
_pci_env_init(void)
{
	/* We assume devices were present on the bus for more than 2 seconds
	 * before initializing SPDK and there's no need to wait more. We scan
	 * the bus, but we don't block any devices.
	 */
	scan_pci_bus(false);

	/* Register a single hotremove callback for all devices. */
	if (spdk_process_is_primary()) {
		rte_dev_event_callback_register(NULL, pci_device_rte_dev_event, NULL);
	}
}

int
pci_env_init(void)
{
	struct spdk_pci_driver *driver;
	int rc;

	rc = dpdk_pci_init();
	if (rc) {
		return rc;
	}

	TAILQ_FOREACH(driver, &g_pci_drivers, tailq) {
		dpdk_pci_driver_register(driver, pci_device_init, pci_device_fini);
	}

	_pci_env_init();
	return 0;
}

void
pci_env_reinit(void)
{
	/* There is no need to register pci drivers again, since they were
	 * already pre-registered in pci_env_init.
	 */

	_pci_env_init();
}

void
pci_env_fini(void)
{
	struct spdk_pci_device *dev;
	char bdf[32];

	cleanup_pci_devices();
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->internal.attached) {
			spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->addr);
			SPDK_ERRLOG("Device %s is still attached at shutdown!\n", bdf);
		}
	}

	if (spdk_process_is_primary()) {
		rte_dev_event_callback_unregister(NULL, pci_device_rte_dev_event, NULL);
	}
}

int
pci_device_init(struct rte_pci_driver *_drv,
		struct rte_pci_device *_dev)
{
	struct spdk_pci_driver *driver = (struct spdk_pci_driver *)_drv;
	struct spdk_pci_device *dev;
	struct rte_pci_addr *addr;
	struct rte_pci_id *id;
	int rc;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -1;
	}

	dev->dev_handle = _dev;

	addr = dpdk_pci_device_get_addr(_dev);
	dev->addr.domain = addr->domain;
	dev->addr.bus = addr->bus;
	dev->addr.dev = addr->devid;
	dev->addr.func = addr->function;

	id = dpdk_pci_device_get_id(_dev);
	dev->id.class_id = id->class_id;
	dev->id.vendor_id = id->vendor_id;
	dev->id.device_id = id->device_id;
	dev->id.subvendor_id = id->subsystem_vendor_id;
	dev->id.subdevice_id = id->subsystem_device_id;

	dev->socket_id = dpdk_pci_device_get_numa_node(_dev);
	dev->type = "pci";

	dev->map_bar = map_bar_rte;
	dev->unmap_bar = unmap_bar_rte;
	dev->cfg_read = cfg_read_rte;
	dev->cfg_write = cfg_write_rte;

	dev->internal.driver = driver;
	dev->internal.claim_fd = -1;

	if (driver->cb_fn != NULL) {
		rc = driver->cb_fn(driver->cb_arg, dev);
		if (rc != 0) {
			free(dev);
			return rc;
		}
		dev->internal.attached = true;
	}

	pthread_mutex_lock(&g_pci_mutex);
	TAILQ_INSERT_TAIL(&g_pci_hotplugged_devices, dev, internal.tailq);
	pthread_mutex_unlock(&g_pci_mutex);
	return 0;
}

static void
set_allowed_at(struct rte_devargs *rte_da, uint64_t tsc)
{
	struct env_devargs *env_da;

	env_da = find_env_devargs(rte_da->bus, rte_da->name);
	if (env_da == NULL) {
		env_da = calloc(1, sizeof(*env_da));
		if (env_da == NULL) {
			SPDK_ERRLOG("could not set_allowed_at for device %s\n", rte_da->name);
			return;
		}
		env_da->bus = rte_da->bus;
		spdk_strcpy_pad(env_da->name, rte_da->name, sizeof(env_da->name), 0);
		TAILQ_INSERT_TAIL(&g_env_devargs, env_da, link);
	}

	env_da->allowed_at = tsc;
}

static uint64_t
get_allowed_at(struct rte_devargs *rte_da)
{
	struct env_devargs *env_da;

	env_da = find_env_devargs(rte_da->bus, rte_da->name);
	if (env_da) {
		return env_da->allowed_at;
	} else {
		return 0;
	}
}

int
pci_device_fini(struct rte_pci_device *_dev)
{
	struct spdk_pci_device *dev;

	pthread_mutex_lock(&g_pci_mutex);
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (dev->dev_handle == _dev) {
			break;
		}
	}

	if (dev == NULL || dev->internal.attached) {
		/* The device might be still referenced somewhere in SPDK. */
		pthread_mutex_unlock(&g_pci_mutex);
		return -EBUSY;
	}

	/* remove our allowed_at option */
	if (dpdk_pci_device_get_devargs(_dev)) {
		set_allowed_at(dpdk_pci_device_get_devargs(_dev), 0);
	}

	/* It is possible that removed flag was already set when there is a race
	 * between the remove notification for this process, and another process
	 * that is also detaching from this same device (for example, when using
	 * nvme driver in multi-process mode.  So do not assert here.  See
	 * #2456 for additional details.
	 */
	dev->internal.removed = true;
	pthread_mutex_unlock(&g_pci_mutex);
	return 0;

}

void
spdk_pci_device_detach(struct spdk_pci_device *dev)
{
	struct spdk_pci_device_provider *provider;

	assert(dev->internal.attached);

	if (dev->internal.claim_fd >= 0) {
		spdk_pci_device_unclaim(dev);
	}

	TAILQ_FOREACH(provider, &g_pci_device_providers, tailq) {
		if (strcmp(dev->type, provider->name) == 0) {
			break;
		}
	}

	assert(provider != NULL);
	dev->internal.attached = false;
	provider->detach_cb(dev);

	cleanup_pci_devices();
}

static int
scan_pci_bus(bool delay_init)
{
	struct rte_dev_iterator it;
	struct rte_device *rte_dev;
	uint64_t now;

	dpdk_bus_scan();
	now = spdk_get_ticks();

	if (!TAILQ_FIRST(&g_pci_drivers)) {
		return 0;
	}

	RTE_DEV_FOREACH(rte_dev, "bus=pci", &it) {
		struct rte_devargs *da;

		da = dpdk_device_get_devargs(rte_dev);
		if (!da) {
			char devargs_str[128];

			/* the device was never blocked or allowed */
			da = calloc(1, sizeof(*da));
			if (!da) {
				return -1;
			}

			snprintf(devargs_str, sizeof(devargs_str), "pci:%s", dpdk_device_get_name(rte_dev));
			if (rte_devargs_parse(da, devargs_str) != 0) {
				free(da);
				return -1;
			}

			rte_devargs_insert(&da);
			dpdk_device_set_devargs(rte_dev, da);
		}

		if (get_allowed_at(da)) {
			uint64_t allowed_at = get_allowed_at(da);

			/* this device was seen by spdk before... */
			if (da->policy == RTE_DEV_BLOCKED && allowed_at <= now) {
				da->policy = RTE_DEV_ALLOWED;
			}
		} else if ((dpdk_device_scan_allowed(rte_dev) && da->policy == RTE_DEV_ALLOWED) ||
			   da->policy != RTE_DEV_BLOCKED) {
			/* override the policy only if not permanently blocked */

			if (delay_init) {
				da->policy = RTE_DEV_BLOCKED;
				set_allowed_at(da, now + 2 * spdk_get_ticks_hz());
			} else {
				da->policy = RTE_DEV_ALLOWED;
				set_allowed_at(da, now);
			}
		}
	}

	return 0;
}

static int
pci_attach_rte(const struct spdk_pci_addr *addr)
{
	char bdf[32];
	int rc, i = 0;

	spdk_pci_addr_fmt(bdf, sizeof(bdf), addr);

	do {
		rc = rte_eal_hotplug_add("pci", bdf, "");
	} while (rc == -ENOMSG && ++i <= DPDK_HOTPLUG_RETRY_COUNT);

	if (i > 1 && rc == -EEXIST) {
		/* Even though the previous request timed out, the device
		 * was attached successfully.
		 */
		rc = 0;
	}

	return rc;
}

static struct spdk_pci_device_provider g_pci_rte_provider = {
	.name = "pci",
	.attach_cb = pci_attach_rte,
	.detach_cb = detach_rte,
};

SPDK_PCI_REGISTER_DEVICE_PROVIDER(pci, &g_pci_rte_provider);

int
spdk_pci_device_attach(struct spdk_pci_driver *driver,
		       spdk_pci_enum_cb enum_cb,
		       void *enum_ctx, struct spdk_pci_addr *pci_address)
{
	struct spdk_pci_device *dev;
	struct spdk_pci_device_provider *provider;
	struct rte_pci_device *rte_dev;
	struct rte_devargs *da;
	int rc;

	cleanup_pci_devices();

	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (spdk_pci_addr_compare(&dev->addr, pci_address) == 0) {
			break;
		}
	}

	if (dev != NULL && dev->internal.driver == driver) {
		pthread_mutex_lock(&g_pci_mutex);
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

	driver->cb_fn = enum_cb;
	driver->cb_arg = enum_ctx;

	rc = -ENODEV;
	TAILQ_FOREACH(provider, &g_pci_device_providers, tailq) {
		rc = provider->attach_cb(pci_address);
		if (rc == 0) {
			break;
		}
	}

	driver->cb_arg = NULL;
	driver->cb_fn = NULL;

	cleanup_pci_devices();

	if (rc != 0) {
		return -1;
	}

	/* explicit attach ignores the allowlist, so if we blocked this
	 * device before let's enable it now - just for clarity.
	 */
	TAILQ_FOREACH(dev, &g_pci_devices, internal.tailq) {
		if (spdk_pci_addr_compare(&dev->addr, pci_address) == 0) {
			break;
		}
	}
	assert(dev != NULL);

	rte_dev = dev->dev_handle;
	if (rte_dev != NULL) {
		da = dpdk_pci_device_get_devargs(rte_dev);
		if (da && get_allowed_at(da)) {
			set_allowed_at(da, spdk_get_ticks());
			da->policy = RTE_DEV_ALLOWED;
		}
	}

	return 0;
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

	cleanup_pci_devices();

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
	pthread_mutex_unlock(&g_pci_mutex);

	if (scan_pci_bus(true) != 0) {
		return -1;
	}

	driver->cb_fn = enum_cb;
	driver->cb_arg = enum_ctx;

	if (dpdk_bus_probe() != 0) {
		driver->cb_arg = NULL;
		driver->cb_fn = NULL;
		return -1;
	}

	driver->cb_arg = NULL;
	driver->cb_fn = NULL;

	cleanup_pci_devices();
	return 0;
}

void
spdk_pci_for_each_device(void *ctx, void (*fn)(void *ctx, struct spdk_pci_device *dev))
{
	struct spdk_pci_device *dev, *tmp;

	pthread_mutex_lock(&g_pci_mutex);
	TAILQ_FOREACH_SAFE(dev, &g_pci_devices, internal.tailq, tmp) {
		fn(ctx, dev);
	}
	pthread_mutex_unlock(&g_pci_mutex);
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	int rc;

	rc = dev->map_bar(dev, bar, mapped_addr, phys_addr, size);
	if (rc) {
		return rc;
	}

#if VFIO_ENABLED
	/* Automatically map the BAR to the IOMMU */
	if (!spdk_iommu_is_enabled()) {
		return 0;
	}

	if (rte_eal_iova_mode() == RTE_IOVA_VA) {
		/* We'll use the virtual address as the iova to match DPDK. */
		rc = vtophys_iommu_map_dma_bar((uint64_t)(*mapped_addr), (uint64_t) * mapped_addr, *size);
		if (rc) {
			dev->unmap_bar(dev, bar, *mapped_addr);
			return -EFAULT;
		}

		*phys_addr = (uint64_t)(*mapped_addr);
	} else {
		/* We'll use the physical address as the iova to match DPDK. */
		rc = vtophys_iommu_map_dma_bar((uint64_t)(*mapped_addr), *phys_addr, *size);
		if (rc) {
			dev->unmap_bar(dev, bar, *mapped_addr);
			return -EFAULT;
		}
	}
#endif
	return rc;
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
#if VFIO_ENABLED
	int rc;

	if (spdk_iommu_is_enabled()) {
		rc = vtophys_iommu_unmap_dma_bar((uint64_t)addr);
		if (rc) {
			return -EFAULT;
		}
	}
#endif

	return dev->unmap_bar(dev, bar, addr);
}

int
spdk_pci_device_enable_interrupt(struct spdk_pci_device *dev)
{
	return dpdk_pci_device_enable_interrupt(dev->dev_handle);
}

int
spdk_pci_device_disable_interrupt(struct spdk_pci_device *dev)
{
	return dpdk_pci_device_disable_interrupt(dev->dev_handle);
}

int
spdk_pci_device_get_interrupt_efd(struct spdk_pci_device *dev)
{
	return dpdk_pci_device_get_interrupt_efd(dev->dev_handle);
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
spdk_pci_device_claim(struct spdk_pci_device *dev)
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

	snprintf(dev_name, sizeof(dev_name), "/var/tmp/spdk_pci_lock_%04x:%02x:%02x.%x",
		 dev->addr.domain, dev->addr.bus, dev->addr.dev, dev->addr.func);

	dev_fd = open(dev_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (dev_fd == -1) {
		SPDK_ERRLOG("could not open %s\n", dev_name);
		return -errno;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		SPDK_ERRLOG("could not truncate %s\n", dev_name);
		close(dev_fd);
		return -errno;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	if (dev_map == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap dev %s (%d)\n", dev_name, errno);
		close(dev_fd);
		return -errno;
	}

	if (fcntl(dev_fd, F_SETLK, &pcidev_lock) != 0) {
		pid = *(int *)dev_map;
		SPDK_ERRLOG("Cannot create lock on device %s, probably"
			    " process %d has claimed it\n", dev_name, pid);
		munmap(dev_map, sizeof(int));
		close(dev_fd);
		/* F_SETLK returns unspecified errnos, normalize them */
		return -EACCES;
	}

	*(int *)dev_map = (int)getpid();
	munmap(dev_map, sizeof(int));
	dev->internal.claim_fd = dev_fd;
	/* Keep dev_fd open to maintain the lock. */
	return 0;
}

void
spdk_pci_device_unclaim(struct spdk_pci_device *dev)
{
	char dev_name[64];

	snprintf(dev_name, sizeof(dev_name), "/var/tmp/spdk_pci_lock_%04x:%02x:%02x.%x",
		 dev->addr.domain, dev->addr.bus, dev->addr.dev, dev->addr.func);

	close(dev->internal.claim_fd);
	dev->internal.claim_fd = -1;
	unlink(dev_name);
}
#else /* !__linux__ */
int
spdk_pci_device_claim(struct spdk_pci_device *dev)
{
	/* TODO */
	return 0;
}

void
spdk_pci_device_unclaim(struct spdk_pci_device *dev)
{
	/* TODO */
}
#endif /* __linux__ */

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

int
spdk_pci_hook_device(struct spdk_pci_driver *drv, struct spdk_pci_device *dev)
{
	int rc;

	assert(dev->map_bar != NULL);
	assert(dev->unmap_bar != NULL);
	assert(dev->cfg_read != NULL);
	assert(dev->cfg_write != NULL);
	dev->internal.driver = drv;

	if (drv->cb_fn != NULL) {
		rc = drv->cb_fn(drv->cb_arg, dev);
		if (rc != 0) {
			return -ECANCELED;
		}

		dev->internal.attached = true;
	}

	TAILQ_INSERT_TAIL(&g_pci_devices, dev, internal.tailq);

	return 0;
}

void
spdk_pci_unhook_device(struct spdk_pci_device *dev)
{
	assert(!dev->internal.attached);
	TAILQ_REMOVE(&g_pci_devices, dev, internal.tailq);
}

void
spdk_pci_register_device_provider(struct spdk_pci_device_provider *provider)
{
	TAILQ_INSERT_TAIL(&g_pci_device_providers, provider, tailq);
}

const char *
spdk_pci_device_get_type(const struct spdk_pci_device *dev)
{
	return dev->type;
}

int
spdk_pci_device_allow(struct spdk_pci_addr *pci_addr)
{
	struct rte_devargs *da;
	char devargs_str[128];

	da = calloc(1, sizeof(*da));
	if (da == NULL) {
		SPDK_ERRLOG("could not allocate rte_devargs\n");
		return -ENOMEM;
	}

	snprintf(devargs_str, sizeof(devargs_str), "pci:%04x:%02x:%02x.%x",
		 pci_addr->domain, pci_addr->bus, pci_addr->dev, pci_addr->func);
	if (rte_devargs_parse(da, devargs_str) != 0) {
		SPDK_ERRLOG("rte_devargs_parse() failed on '%s'\n", devargs_str);
		free(da);
		return -EINVAL;
	}
	da->policy = RTE_DEV_ALLOWED;
	/* Note: if a devargs already exists for this device address, it just gets
	 * overridden.  So we do not need to check if the devargs already exists.
	 * DPDK will take care of memory management for the devargs structure after
	 * it has been inserted, so there's nothing SPDK needs to track.
	 */
	if (rte_devargs_insert(&da) != 0) {
		SPDK_ERRLOG("rte_devargs_insert() failed on '%s'\n", devargs_str);
		free(da);
		return -EINVAL;
	}

	return 0;
}
