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

/**
 * Callback for spdk_vfu_init().
 *
 * \param rc 0 on success, negative errno on failure.
 */
typedef void (*spdk_vfu_init_cb)(int rc);


/**
 * Callback for spdk_vfu_fini().
 */
typedef void (*spdk_vfu_fini_cb)(void);

/**
 * Initialize vfio-user target environment.
 */
void spdk_vfu_init(spdk_vfu_init_cb init_cb);

/**
 * Clean up vfio-user target environment.
 */
void spdk_vfu_fini(spdk_vfu_fini_cb fini_cb);

/**
 * Opaque handle to a PCI endpoint, it's representative of a Unix Domain socket file.
 */
struct spdk_vfu_endpoint;

#define SPDK_VFU_MAX_NAME_LEN (64)

/**
 * Vfio-user PCI device sparse MMAP region.
 *
 * The sparse mmap allows finer granularity of specifying areas
 * within a PCI region with mmap support.
 */
struct spdk_vfu_sparse_mmap {
	/**
	 * Sparse mmap region offset, starts from 0 of current PCI region.
	 */
	uint64_t offset;

	/**
	 * Sparse mmap region length.
	 */
	uint64_t len;
};

#define SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS	8

/**
 * Callback for vfio-user PCI region access.
 *
 * \param vfu_ctx Opaque value of the PCI endpoint, it's created by libvfio-user.
 * \param buf data buffer to R/W.
 * \param count R/W size, the value could be 1,2,4,8.
 * \param pos offset from PCI region.
 * \param is_write true if access is WRITE.
 *
 * \return count on success, negative errno on failure.
 */
typedef ssize_t (*spdk_vfu_access_cb)(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
				      bool is_write);

/**
 * Vfio-user device PCI region.
 *
 * PCI region is definition of PCI device BAR.
 */
struct spdk_vfu_pci_region {
	/**
	 * Offset of the PCI region.
	 */
	uint64_t offset;

	/**
	 * Length of the PCI region.
	 */
	uint64_t len;

	/**
	 * Capability flags.
	 */
	uint64_t flags;

	/**
	 * Number of sparse mmap region.
	 */
	uint32_t nr_sparse_mmaps;

	/**
	 * Representative of the PCI region access file descriptor.
	 */
	int fd;

	/**
	 * Sparse mmap regions.
	 */
	struct spdk_vfu_sparse_mmap mmaps[SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS];

	/**
	 * PCI region access callback.
	 */
	spdk_vfu_access_cb access_cb;
};

/**
 * Vfio-user PCI device information.
 *
 * vfio-user target uses this data structure to get all the informations
 * from backend emulated device module.
 */
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

	/* Standard Power Management capabilities */
	struct pmcap pmcap;
	/* Standard PCI Express Capability ID */
	struct pxcap pxcap;
	/* Standard MSI-X Capability */
	struct msixcap msixcap;
	/* Number of vendor specific capabilities */
	uint16_t nr_vendor_caps;

	/* Legacy interrupt pin number */
	uint16_t intr_ipin;
	/* Number of legacy interrupts */
	uint32_t nr_int_irqs;
	/* Number of MSIX interrupts */
	uint32_t nr_msix_irqs;

	/* PCI regions */
	struct spdk_vfu_pci_region regions[VFU_PCI_DEV_NUM_REGIONS];
};

struct spdk_vfu_endpoint_ops {
	/**
	 * Backend emulated PCI device type name.
	 */
	char name[SPDK_VFU_MAX_NAME_LEN];

	/**
	 * Initialize endpoint to PCI device with base path.
	 */
	void *(*init)(struct spdk_vfu_endpoint *endpoint,
		      char *basename, const char *endpoint_name);

	/**
	 * Get PCI device information from backend device module.
	 */
	int (*get_device_info)(struct spdk_vfu_endpoint *endpoint,
			       struct spdk_vfu_pci_device *device_info);

	/**
	 * Get vendor capabilitiy based on ID in PCI configuration space.
	 */
	uint16_t (*get_vendor_capability)(struct spdk_vfu_endpoint *endpoint, char *buf,
					  uint16_t buf_len, uint16_t idx);

	/**
	 * Attach active connection to the PCI endpoint.
	 */
	int (*attach_device)(struct spdk_vfu_endpoint *endpoint);

	/**
	 * Detach the active connection of the PCI endpoint.
	 */
	int (*detach_device)(struct spdk_vfu_endpoint *endpoint);

	/**
	 * Destruct the PCI endpoint.
	 */
	int (*destruct)(struct spdk_vfu_endpoint *endpoint);

	/**
	 * Post-notification to backend module after a new memory region is added.
	 */
	int (*post_memory_add)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);

	/**
	 * Pre-notification to backend module before removing the memory region.
	 */
	int (*pre_memory_remove)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);

	/**
	 * PCI device reset callback.
	 */
	int (*reset_device)(struct spdk_vfu_endpoint *endpoint);

	/**
	 * PCI device quiesce callback, after this callback, the backend device module
	 * should stopping processing any IOs.
	 */
	int (*quiesce_device)(struct spdk_vfu_endpoint *endpoint);
};

/**
 * Register the operations of emulated backend PCI device.
 *
 * \param ops The operations of emulated backend PCI device.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_vfu_register_endpoint_ops(struct spdk_vfu_endpoint_ops *ops);

/**
 * Create a PCI endpoint.
 *
 * \param endpoint_name Name of the PCI endpoint.
 * \param cpumask_str CPU masks that the endpoint is running on.
 * \param dev_type_name Name of the registered operation.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_vfu_create_endpoint(const char *endpoint_name, const char *cpumask_str,
			     const char *dev_type_name);

/**
 * Delete a PCI endpoint.
 *
 * \param endpoint_name Name of the PCI endpoint.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_vfu_delete_endpoint(const char *endpoint_name);

/**
 * Set the base path to create socket files.
 *
 * \param basename Path to create socket files.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_vfu_set_socket_path(const char *basename);

/**
 * Get UUID of the PCI endpoint.
 *
 * This function will return the absolute path of the PCI endpoint which
 * represented by a socket file.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return absolute path of the PCI endpoint.
 */
const char *spdk_vfu_get_endpoint_id(struct spdk_vfu_endpoint *endpoint);

/**
 * Get name of the PCI endpoint.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return name of the PCI endpoint.
 */
const char *spdk_vfu_get_endpoint_name(struct spdk_vfu_endpoint *endpoint);

/**
 * Get opaque handle of the PCI endpoint that created by libvfio-user library.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return opaque handle on success, NULL on failure.
 */
vfu_ctx_t *spdk_vfu_get_vfu_ctx(struct spdk_vfu_endpoint *endpoint);

/**
 * Get private opaque handle of backend PCI device moduel.
 *
 * This function is used in backend PCI device module to get the internal
 * private data structure saved in vfu_target library.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return opaque handle of backend device on success, NULL on failure.
 */
void *spdk_vfu_get_endpoint_private(struct spdk_vfu_endpoint *endpoint);

/**
 * MSI-X is enabled or not.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return ture if MSI-X is enabled, false otherwise.
 */
bool spdk_vfu_endpoint_msix_enabled(struct spdk_vfu_endpoint *endpoint);

/**
 * INT-X is enabled or not.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return true if INT-X is enabled, false otherwise.
 */
bool spdk_vfu_endpoint_intx_enabled(struct spdk_vfu_endpoint *endpoint);

/**
 * Get PCI configuration space.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return pointer to PCI configuration space on success, NULL on failure.
 */
void *spdk_vfu_endpoint_get_pci_config(struct spdk_vfu_endpoint *endpoint);

/**
 * Get PCI endpoint via name.
 *
 * \param name The PCI endpoint name.
 *
 * \return PCI endpoint pointer on success, NULL on failure.
 */
struct spdk_vfu_endpoint *spdk_vfu_get_endpoint_by_name(const char *name);

/**
 * Map Guest Physical Address to Host Virtual Address.
 *
 * \param endpoint The PCI endpoint.
 * \param addr Physical address that to be mapped.
 * \param len Length of mapped address.
 * \param sg Scatter/gather entry to be mapped.
 * \param iov IOV to save mapped virtual address and length.
 * \param prot Protection flags.
 *
 * \return mapped virtual address on success, NULL on failure.
 */
void *spdk_vfu_map_one(struct spdk_vfu_endpoint *endpoint, uint64_t addr, uint64_t len,
		       dma_sg_t *sg, struct iovec *iov, int prot);

/**
 * Unmap array of scatter/gather entries.
 *
 * \param endpoint The PCI endpoint.
 * \param array of scatter/gather entries to be unmapped.
 * \param iov array of IOVs contain virtual addresses and length.
 * \param iovcnt Number of IOVs.
 *
 */
void spdk_vfu_unmap_sg(struct spdk_vfu_endpoint *endpoint, dma_sg_t *sg, struct iovec *iov,
		       int iovcnt);
#ifdef __cplusplus
}
#endif

#endif
