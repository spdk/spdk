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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdbool.h>

#include <rte_config.h>
#include <rte_pci.h>
#include <rte_version.h>

#define spdk_pci_device rte_pci_device

#ifdef __FreeBSD__
#include <sys/pciio.h>
#endif

#include "spdk/env.h"
#include "spdk/pci_ids.h"

#define SYSFS_PCI_DEVICES	"/sys/bus/pci/devices"
#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"

#define SPDK_PCI_PATH_MAX	256
#define PCI_CFG_SIZE		256
#define PCI_EXT_CAP_ID_SN	0x03

struct spdk_pci_enum_ctx {
	struct rte_pci_driver	driver;
	spdk_pci_enum_cb	enum_cb;
	void 			*enum_ctx;
};

static struct rte_pci_id nvme_pci_driver_id[] = {
#if RTE_VERSION >= RTE_VERSION_NUM(16, 7, 0, 1)
	{
		.class_id = SPDK_PCI_CLASS_NVME,
		.vendor_id = PCI_ANY_ID,
		.device_id = PCI_ANY_ID,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID,
	},
#else
	{RTE_PCI_DEVICE(0x8086, 0x0953)},
#endif
	{ .vendor_id = 0, /* sentinel */ },
};

#define SPDK_IOAT_PCI_DEVICE(DEVICE_ID) RTE_PCI_DEVICE(SPDK_PCI_VID_INTEL, DEVICE_ID)
static struct rte_pci_id ioat_driver_id[] = {
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_SNB8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_IVB9)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_HSW9)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BWD3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDXDE3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX0)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX1)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX2)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX3)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX4)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX5)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX6)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX7)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX8)},
	{SPDK_IOAT_PCI_DEVICE(PCI_DEVICE_ID_INTEL_IOAT_BDX9)},
	{ .vendor_id = 0, /* sentinel */ },
};

static int
spdk_pci_device_init(struct rte_pci_driver *driver,
		     struct rte_pci_device *device)
{
	struct spdk_pci_enum_ctx *ctx = (struct spdk_pci_enum_ctx *)driver;

	if (device->kdrv == RTE_KDRV_VFIO) {
		/*
		 * TODO: This is a workaround for an issue where the device is not ready after VFIO reset.
		 * Figure out what is actually going wrong and remove this sleep.
		 */
		usleep(500 * 1000);
	}

	return ctx->enum_cb(ctx->enum_ctx, (struct spdk_pci_device *)device);
}

static int
spdk_pci_device_fini(struct rte_pci_device *device)
{
	return 0;
}

int
spdk_pci_enumerate(enum spdk_pci_device_type type,
		   spdk_pci_enum_cb enum_cb,
		   void *enum_ctx)
{
	struct spdk_pci_enum_ctx ctx = {};
	int rc;
	const char *name;

	if (type == SPDK_PCI_DEVICE_NVME) {
		name = "SPDK NVMe";
		ctx.driver.id_table = nvme_pci_driver_id;
	} else if (type == SPDK_PCI_DEVICE_IOAT) {
		name = "SPDK IOAT";
		ctx.driver.id_table = ioat_driver_id;
	} else {
		return -1;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.driver.drv_flags = RTE_PCI_DRV_NEED_MAPPING;

#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 0)
	ctx.driver.probe = spdk_pci_device_init;
	ctx.driver.remove = spdk_pci_device_fini;
	ctx.driver.driver.name = name;
#else
	ctx.driver.devinit = spdk_pci_device_init;
	ctx.driver.devuninit = spdk_pci_device_fini;
	ctx.driver.name = name;
#endif

	rte_eal_pci_register(&ctx.driver);
	rc = rte_eal_pci_probe();
	rte_eal_pci_unregister(&ctx.driver);

	return rc;
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

static int
pci_device_get_u32(struct spdk_pci_device *dev, const char *file, uint32_t *val)
{
	char filename[SPDK_PCI_PATH_MAX];
	FILE *fd;
	char buf[10];
	char *end;

	snprintf(filename, sizeof(filename),
		 SYSFS_PCI_DEVICES "/" PCI_PRI_FMT "/%s",
		 spdk_pci_device_get_domain(dev), spdk_pci_device_get_bus(dev),
		 spdk_pci_device_get_dev(dev), spdk_pci_device_get_func(dev), file);

	fd = fopen(filename, "r");
	if (!fd) {
		return -1;
	}

	if (fgets(buf, sizeof(buf), fd) == NULL) {
		fclose(fd);
		return -1;
	}

	*val = strtoul(buf, &end, 0);
	if ((buf[0] == '\0') || (end == NULL) || (*end != '\n')) {
		fclose(fd);
		return -1;
	}

	fclose(fd);
	return 0;

}

uint16_t
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

uint32_t
spdk_pci_device_get_class(struct spdk_pci_device *dev)
{
	uint32_t class_code;

	if (pci_device_get_u32(dev, "class", &class_code) < 0) {
		return 0xFFFFFFFFu;
	}

	return class_code;
}

const char *
spdk_pci_device_get_device_name(struct spdk_pci_device *dev)
{
	/* TODO */
	return NULL;
}

int
spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset)
{
	return rte_eal_pci_read_config(dev, value, 1, offset) == 1 ? 0 : -1;
}

int
spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset)
{
	return rte_eal_pci_write_config(dev, &value, 1, offset) == 1 ? 0 : -1;
}

int
spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset)
{
	return rte_eal_pci_read_config(dev, value, 2, offset) == 2 ? 0 : -1;
}

int
spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset)
{
	return rte_eal_pci_write_config(dev, &value, 2, offset) == 2 ? 0 : -1;
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset)
{
	return rte_eal_pci_read_config(dev, value, 4, offset) == 4 ? 0 : -1;
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset)
{
	return rte_eal_pci_write_config(dev, &value, 4, offset) == 4 ? 0 : -1;
}

int
spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len)
{
	int err;
	uint32_t pos, header = 0;
	uint32_t i, buf[2];

	if (len < 17)
		return -1;

	err = spdk_pci_device_cfg_read32(dev, &header, PCI_CFG_SIZE);
	if (err || !header)
		return -1;

	pos = PCI_CFG_SIZE;
	while (1) {
		if ((header & 0x0000ffff) == PCI_EXT_CAP_ID_SN) {
			if (pos) {
				/*skip the header*/
				pos += 4;
				for (i = 0; i < 2; i++) {
					err = spdk_pci_device_cfg_read32(dev, &buf[i], pos + 4 * i);
					if (err)
						return -1;
				}
				sprintf(sn, "%08x%08x", buf[1], buf[0]);
				return 0;
			}
		}
		pos = (header >> 20) & 0xffc;
		/*0 if no other items exist*/
		if (pos < PCI_CFG_SIZE)
			return -1;
		err = spdk_pci_device_cfg_read32(dev, &header, pos);
		if (err)
			return -1;
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
spdk_pci_device_claim(struct spdk_pci_device *dev)
{
	int dev_fd;
	char shm_name[64];
	int pid;
	void *dev_map;
	struct flock pcidev_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	sprintf(shm_name, PCI_PRI_FMT, spdk_pci_device_get_domain(dev),
		spdk_pci_device_get_bus(dev), spdk_pci_device_get_dev(dev),
		spdk_pci_device_get_func(dev));

	dev_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (dev_fd == -1) {
		fprintf(stderr, "could not shm_open %s\n", shm_name);
		return -1;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		fprintf(stderr, "could not truncate shm %s\n", shm_name);
		close(dev_fd);
		return -1;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	if (dev_map == NULL) {
		fprintf(stderr, "could not mmap shm %s\n", shm_name);
		close(dev_fd);
		return -1;
	}

	if (fcntl(dev_fd, F_SETLK, &pcidev_lock) != 0) {
		pid = *(int *)dev_map;
		fprintf(stderr, "Cannot create lock on device %s, probably"
			" process %d has claimed it\n", shm_name, pid);
		munmap(dev_map, sizeof(int));
		close(dev_fd);
		return -1;
	}

	*(int *)dev_map = (int)getpid();
	munmap(dev_map, sizeof(int));
	/* Keep dev_fd open to maintain the lock. */
	return 0;
}
#endif /* __linux__ */

#ifdef __FreeBSD__
int
spdk_pci_device_claim(struct spdk_pci_device *dev)
{
	/* TODO */
	return 0;
}
#endif /* __FreeBSD__ */
