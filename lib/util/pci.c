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

#include <pciaccess.h>

#ifdef __FreeBSD__
#include <sys/pciio.h>
#endif

#include "spdk/pci.h"

#define SYSFS_PCI_DEVICES	"/sys/bus/pci/devices"
#define SYSFS_PCI_DRIVERS	"/sys/bus/pci/drivers"
#define PCI_PRI_FMT		"%04x:%02x:%02x.%1u"
#define SPDK_PCI_PATH_MAX	256


/* var should be the pointer */
#define spdk_pcicfg_read32(handle, var, offset)  pci_device_cfg_read_u32(handle, var, offset)
#define spdk_pcicfg_write32(handle, var, offset) pci_device_cfg_write_u32(handle, *var, offset)

int
pci_device_get_serial_number(struct pci_device *dev, char *sn, int len)
{
	int err;
	uint32_t pos, header = 0;
	uint32_t i, buf[2];

	if (len < 17)
		return -1;

	err = spdk_pcicfg_read32(dev, &header, PCI_CFG_SIZE);
	if (err || !header)
		return -1;

	pos = PCI_CFG_SIZE;
	while (1) {
		if ((header & 0x0000ffff) == PCI_EXT_CAP_ID_SN) {
			if (pos) {
				/*skip the header*/
				pos += 4;
				for (i = 0; i < 2; i++) {
					err = spdk_pcicfg_read32(dev,
								 &buf[i], pos + 4 * i);
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
		err = spdk_pcicfg_read32(dev, &header, pos);
		if (err)
			return -1;
	}
	return -1;
}

#ifdef __linux__
int
pci_device_has_non_uio_driver(struct pci_device *dev)
{
	char linkname[SPDK_PCI_PATH_MAX];
	char driver[SPDK_PCI_PATH_MAX];
	ssize_t driver_len;
	char *driver_begin;

	snprintf(linkname, sizeof(linkname),
		 SYSFS_PCI_DEVICES "/" PCI_PRI_FMT "/driver",
		 spdk_pci_device_get_domain(dev), spdk_pci_device_get_bus(dev),
		 spdk_pci_device_get_dev(dev), spdk_pci_device_get_func(dev));

	driver_len = readlink(linkname, driver, sizeof(driver));

	if (driver_len < 0 || driver_len >= SPDK_PCI_PATH_MAX) {
		return 0;
	}

	driver[driver_len] = '\0'; /* readlink() doesn't null terminate, so we have to */

	driver_begin = strrchr(driver, '/');
	if (driver_begin) {
		/* Advance to the character after the slash */
		driver_begin++;
	} else {
		/* This shouldn't normally happen - driver should be a relative path with slashes */
		driver_begin = driver;
	}

	return strcmp(driver_begin, "uio_pci_generic") != 0;
}
#endif

#ifdef __FreeBSD__
int
pci_device_has_non_uio_driver(struct pci_device *dev)
{
	struct pci_conf_io	configsel;
	struct pci_match_conf	pattern;
	struct pci_conf		conf;
	int			fd;

	memset(&pattern, 0, sizeof(pattern));
	pattern.pc_sel.pc_domain = spdk_pci_device_get_domain(dev);
	pattern.pc_sel.pc_bus = spdk_pci_device_get_bus(dev);
	pattern.pc_sel.pc_dev = spdk_pci_device_get_dev(dev);
	pattern.pc_sel.pc_func = spdk_pci_device_get_func(dev);
	pattern.flags = PCI_GETCONF_MATCH_DOMAIN |
			PCI_GETCONF_MATCH_BUS |
			PCI_GETCONF_MATCH_DEV |
			PCI_GETCONF_MATCH_FUNC;

	memset(&configsel, 0, sizeof(configsel));
	configsel.match_buf_len = sizeof(conf);
	configsel.matches = &conf;
	configsel.num_patterns = 1;
	configsel.pat_buf_len = sizeof(pattern);
	configsel.patterns = &pattern;

	fd = open("/dev/pci", O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "could not open /dev/pci\n");
		return -1;
	}

	if (ioctl(fd, PCIOCGETCONF, &configsel) == -1) {
		fprintf(stderr, "ioctl(PCIOCGETCONF) failed\n");
		close(fd);
		return -1;
	}

	close(fd);

	if (configsel.num_matches != 1) {
		fprintf(stderr, "could not find specified device\n");
		return -1;
	}

	if (conf.pd_name[0] == '\0' || !strcmp(conf.pd_name, "nic_uio")) {
		return 0;
	} else {
		return 1;
	}
}
#endif

int
pci_device_unbind_kernel_driver(struct pci_device *dev)
{
	int n;
	FILE *fd;
	char filename[SPDK_PCI_PATH_MAX];
	char buf[256];

	snprintf(filename, sizeof(filename),
		 SYSFS_PCI_DEVICES "/" PCI_PRI_FMT "/driver/unbind",
		 spdk_pci_device_get_domain(dev), spdk_pci_device_get_bus(dev),
		 spdk_pci_device_get_dev(dev), spdk_pci_device_get_func(dev));

	fd = fopen(filename, "w");
	if (!fd)
		return 0;

	n = snprintf(buf, sizeof(buf), PCI_PRI_FMT,
		     spdk_pci_device_get_domain(dev), spdk_pci_device_get_bus(dev),
		     spdk_pci_device_get_dev(dev), spdk_pci_device_get_dev(dev));

	if (fwrite(buf, n, 1, fd) == 0)
		goto error;

	fclose(fd);
	return 0;

error:
	fclose(fd);
	return -1;
}

static int
check_modules(char *driver_name)
{
	FILE *fd;
	const char *proc_modules = "/proc/modules";
	char buffer[256];

	fd = fopen(proc_modules, "r");
	if (!fd)
		return -1;

	while (fgets(buffer, sizeof(buffer), fd)) {
		if (strstr(buffer, driver_name) == NULL)
			continue;
		else {
			fclose(fd);
			return 0;
		}
	}
	fclose(fd);

	return -1;
}

int
pci_device_bind_uio_driver(struct pci_device *dev, char *driver_name)
{
	int err, n;
	FILE *fd;
	char filename[SPDK_PCI_PATH_MAX];
	char buf[256];

	err = check_modules(driver_name);
	if (err < 0) {
		fprintf(stderr, "No %s module loaded\n", driver_name);
		return err;
	}

	snprintf(filename, sizeof(filename),
		 SYSFS_PCI_DRIVERS "/" "%s" "/new_id", driver_name);

	fd = fopen(filename, "w");
	if (!fd) {
		return -1;
	}

	n = snprintf(buf, sizeof(buf), "%04x %04x",
		     spdk_pci_device_get_vendor_id(dev),
		     spdk_pci_device_get_device_id(dev));

	if (fwrite(buf, n, 1, fd) == 0)
		goto error;

	fclose(fd);
	return 0;

error:
	fclose(fd);
	return -1;
}

int
pci_device_switch_to_uio_driver(struct pci_device *dev)
{
	if (pci_device_unbind_kernel_driver(dev)) {
		fprintf(stderr, "Device %d:%d:%d unbind from "
			"kernel driver failed\n",
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		return -1;
	}
	if (pci_device_bind_uio_driver(dev, PCI_UIO_DRIVER)) {
		fprintf(stderr, "Device %d:%d:%d bind to "
			"uio driver failed\n",
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		return -1;
	}
	printf("Device %d:%d:%d bind to uio driver success\n",
	       spdk_pci_device_get_bus(dev), spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));
	return 0;
}

int
pci_device_claim(struct pci_device *dev)
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
