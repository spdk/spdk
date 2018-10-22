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

#include "spdk/env.h"

#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"

#define PCI_CFG_SIZE		256
#define PCI_EXT_CAP_ID_SN	0x03

int
spdk_pci_device_init(struct rte_pci_driver *driver,
		     struct rte_pci_device *device)
{
	struct spdk_pci_enum_ctx *ctx = (struct spdk_pci_enum_ctx *)driver;
	int rc;

	if (!ctx->cb_fn) {
#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
		rte_pci_unmap_device(device);
#elif RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
		rte_eal_pci_unmap_device(device);
#endif

		/* Return a positive value to indicate that this device does not belong to this driver, but
		 * this isn't an error. */
		return 1;
	}

	rc = ctx->cb_fn(ctx->cb_arg, (struct spdk_pci_device *)device);
	if (rc != 0) {
		return rc;
	}

	spdk_vtophys_pci_device_added(device);
	return 0;
}

int
spdk_pci_device_fini(struct rte_pci_device *device)
{
	spdk_vtophys_pci_device_removed(device);
	return 0;
}

void
spdk_pci_device_detach(struct spdk_pci_device *device)
{
#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
#if RTE_VERSION < RTE_VERSION_NUM(17, 05, 0, 0)
	rte_eal_device_remove(&device->device);
#endif
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	struct spdk_pci_addr	addr;
	char			bdf[32];

	addr.domain = device->addr.domain;
	addr.bus = device->addr.bus;
	addr.dev = device->addr.devid;
	addr.func = device->addr.function;

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &addr);
	if (rte_eal_dev_detach(&device->device) < 0) {
		fprintf(stderr, "Failed to detach PCI device %s (device already removed?).\n", bdf);
	}
#elif RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
	rte_pci_detach(&device->addr);
#else
	rte_eal_pci_detach(&device->addr);
#endif
}

int
spdk_pci_device_attach(struct spdk_pci_enum_ctx *ctx,
		       spdk_pci_enum_cb enum_cb,
		       void *enum_ctx, struct spdk_pci_addr *pci_address)
{
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	char				bdf[32];

	spdk_pci_addr_fmt(bdf, sizeof(bdf), pci_address);
#else
	struct rte_pci_addr		addr;

	addr.domain = pci_address->domain;
	addr.bus = pci_address->bus;
	addr.devid = pci_address->dev;
	addr.function = pci_address->func;
#endif

	pthread_mutex_lock(&ctx->mtx);

	if (!ctx->is_registered) {
		ctx->is_registered = true;
#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
		rte_pci_register(&ctx->driver);
#else
		rte_eal_pci_register(&ctx->driver);
#endif
	}

	ctx->cb_fn = enum_cb;
	ctx->cb_arg = enum_ctx;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	if (rte_eal_dev_attach(bdf, "") != 0) {
#elif RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
	if (rte_pci_probe_one(&addr) != 0) {
#else
	if (rte_eal_pci_probe_one(&addr) != 0) {
#endif
		ctx->cb_arg = NULL;
		ctx->cb_fn = NULL;
		pthread_mutex_unlock(&ctx->mtx);
		return -1;
	}

	ctx->cb_arg = NULL;
	ctx->cb_fn = NULL;
	pthread_mutex_unlock(&ctx->mtx);

	return 0;
}

/* Note: You can call spdk_pci_enumerate from more than one thread
 *       simultaneously safely, but you cannot call spdk_pci_enumerate
 *       and rte_eal_pci_probe simultaneously.
 */
int
spdk_pci_enumerate(struct spdk_pci_enum_ctx *ctx,
		   spdk_pci_enum_cb enum_cb,
		   void *enum_ctx)
{
	pthread_mutex_lock(&ctx->mtx);

	if (!ctx->is_registered) {
		ctx->is_registered = true;
#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
		rte_pci_register(&ctx->driver);
#else
		rte_eal_pci_register(&ctx->driver);
#endif
	}

	ctx->cb_fn = enum_cb;
	ctx->cb_arg = enum_ctx;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	if (rte_bus_probe() != 0) {
#elif RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
	if (rte_pci_probe() != 0) {
#else
	if (rte_eal_pci_probe() != 0) {
#endif
		ctx->cb_arg = NULL;
		ctx->cb_fn = NULL;
		pthread_mutex_unlock(&ctx->mtx);
		return -1;
	}

	ctx->cb_arg = NULL;
	ctx->cb_fn = NULL;
	pthread_mutex_unlock(&ctx->mtx);

	return 0;
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *device, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct rte_pci_device *dev = device;

	*mapped_addr = dev->mem_resource[bar].addr;
	*phys_addr = (uint64_t)dev->mem_resource[bar].phys_addr;
	*size = (uint64_t)dev->mem_resource[bar].len;

	return 0;
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *device, uint32_t bar, void *addr)
{
	return 0;
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
	return dev->addr.devid;
}

uint8_t
spdk_pci_device_get_func(struct spdk_pci_device *dev)
{
	return dev->addr.function;
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
	return dev->id.subsystem_vendor_id;
}

uint16_t
spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev)
{
	return dev->id.subsystem_device_id;
}

struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_id pci_id;

	pci_id.vendor_id = spdk_pci_device_get_vendor_id(pci_dev);
	pci_id.device_id = spdk_pci_device_get_device_id(pci_dev);
	pci_id.subvendor_id = spdk_pci_device_get_subvendor_id(pci_dev);
	pci_id.subdevice_id = spdk_pci_device_get_subdevice_id(pci_dev);

	return pci_id;
}

int
spdk_pci_device_get_socket_id(struct spdk_pci_device *pci_dev)
{
#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
	return pci_dev->device.numa_node;
#else
	return pci_dev->numa_node;
#endif
}

int
spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
	rc = rte_pci_read_config(dev, value, len, offset);
#else
	rc = rte_eal_pci_read_config(dev, value, len, offset);
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
}

int
spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len, uint32_t offset)
{
	int rc;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 4)
	rc = rte_pci_write_config(dev, value, len, offset);
#else
	rc = rte_eal_pci_write_config(dev, value, len, offset);
#endif
	return (rc > 0 && (uint32_t) rc == len) ? 0 : -1;
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
spdk_pci_device_get_addr(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr;

	pci_addr.domain = spdk_pci_device_get_domain(pci_dev);
	pci_addr.bus = spdk_pci_device_get_bus(pci_dev);
	pci_addr.dev = spdk_pci_device_get_dev(pci_dev);
	pci_addr.func = spdk_pci_device_get_func(pci_dev);

	return pci_addr;
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
