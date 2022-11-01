/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef _VFU_TARGET_H
#define _VFU_TARGET_H

#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_vfu_init_cb)(int rc);
typedef void (*spdk_vfu_fini_cb)(void);

void spdk_vfu_init(spdk_vfu_init_cb init_cb);
void spdk_vfu_fini(spdk_vfu_fini_cb fini_cb);

struct spdk_vfu_endpoint;

#define SPDK_VFU_MAX_NAME_LEN (64)

struct spdk_vfu_sparse_mmap {
	uint64_t offset;
	uint64_t len;
};

#define SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS	8

typedef ssize_t (*spdk_vfu_access_cb)(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
				      bool is_write);

struct spdk_vfu_pci_region {
	uint64_t offset;
	uint64_t len;
	uint64_t flags;
	uint32_t nr_sparse_mmaps;
	int fd;
	struct spdk_vfu_sparse_mmap mmaps[SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS];
	spdk_vfu_access_cb access_cb;
};

struct spdk_vfu_pci_device {
	struct {
		/* Vendor ID */
		uint16_t vid;
		/* Device ID */
		uint16_t did;
		/* Subsystem Vendor ID */
		uint16_t ssvid;
		/* Subsystem ID */
		uint16_t ssid;
	} id;

	struct {
		/* Base Class Code */
		uint8_t bcc;
		/* Sub Class code */
		uint8_t scc;
		/* Programming Interface */
		uint8_t pi;
	} class;

	/* Standard PCI Capabilities */
	struct pmcap pmcap;
	struct pxcap pxcap;
	struct msixcap msixcap;
	uint16_t nr_vendor_caps;

	uint16_t intr_ipin;
	uint32_t nr_int_irqs;
	uint32_t nr_msix_irqs;

	struct spdk_vfu_pci_region regions[VFU_PCI_DEV_NUM_REGIONS];
};

struct spdk_vfu_endpoint_ops {
	/* PCI device type name */
	char name[SPDK_VFU_MAX_NAME_LEN];

	void *(*init)(struct spdk_vfu_endpoint *endpoint,
		      char *basename, const char *endpoint_name);
	int (*get_device_info)(struct spdk_vfu_endpoint *endpoint,
			       struct spdk_vfu_pci_device *device_info);
	uint16_t (*get_vendor_capability)(struct spdk_vfu_endpoint *endpoint, char *buf,
					  uint16_t buf_len, uint16_t idx);
	int (*attach_device)(struct spdk_vfu_endpoint *endpoint);
	int (*detach_device)(struct spdk_vfu_endpoint *endpoint);
	int (*destruct)(struct spdk_vfu_endpoint *endpoint);

	int (*post_memory_add)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);
	int (*pre_memory_remove)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);
	int (*reset_device)(struct spdk_vfu_endpoint *endpoint);
	int (*quiesce_device)(struct spdk_vfu_endpoint *endpoint);
};

int spdk_vfu_register_endpoint_ops(struct spdk_vfu_endpoint_ops *ops);
int spdk_vfu_create_endpoint(const char *endpoint_name, const char *cpumask_str,
			     const char *dev_type_name);
int spdk_vfu_delete_endpoint(const char *endpoint_name);
int spdk_vfu_set_socket_path(const char *basename);
const char *spdk_vfu_get_endpoint_id(struct spdk_vfu_endpoint *endpoint);
const char *spdk_vfu_get_endpoint_name(struct spdk_vfu_endpoint *endpoint);
vfu_ctx_t *spdk_vfu_get_vfu_ctx(struct spdk_vfu_endpoint *endpoint);
void *spdk_vfu_get_endpoint_private(struct spdk_vfu_endpoint *endpoint);
bool spdk_vfu_endpoint_msix_enabled(struct spdk_vfu_endpoint *endpoint);
bool spdk_vfu_endpoint_intx_enabled(struct spdk_vfu_endpoint *endpoint);
void *spdk_vfu_endpoint_get_pci_config(struct spdk_vfu_endpoint *endpoint);
struct spdk_vfu_endpoint *spdk_vfu_get_endpoint_by_name(const char *name);
void *spdk_vfu_map_one(struct spdk_vfu_endpoint *endpoint, uint64_t addr, uint64_t len,
		       dma_sg_t *sg, struct iovec *iov, int prot);
void spdk_vfu_unmap_sg(struct spdk_vfu_endpoint *endpoint, dma_sg_t *sg, struct iovec *iov,
		       int iovcnt);
#ifdef __cplusplus
}
#endif

#endif
