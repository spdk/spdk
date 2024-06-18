/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef _VFU_VIRTIO_INTERNAL_H
#define _VFU_VIRTIO_INTERNAL_H

#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>

#include "spdk/vfu_target.h"

#define VIRTIO_HOST_SUPPORTED_FEATURES ((1ULL << VIRTIO_F_VERSION_1) | \
					(1ULL << VIRTIO_RING_F_INDIRECT_DESC) | \
					(1ULL << VIRTIO_F_RING_PACKED))

/* virtio device layout:
 *
 * region 1: MSI-X Table
 * region 2: MSI-X PBA
 * region 4: virtio modern memory 64bits BAR
 *     Common configuration          0x0    - 0x1000
 *     ISR access                    0x1000 - 0x2000
 *     Device specific configuration 0x2000 - 0x3000
 *     Notifications                 0x3000 - 0x4000
 */
#define VIRTIO_PCI_COMMON_CFG_OFFSET	(0x0)
#define VIRTIO_PCI_COMMON_CFG_LENGTH	(0x1000)
#define VIRTIO_PCI_ISR_ACCESS_OFFSET	(VIRTIO_PCI_COMMON_CFG_OFFSET + VIRTIO_PCI_COMMON_CFG_LENGTH)
#define VIRTIO_PCI_ISR_ACCESS_LENGTH	(0x1000)
#define VIRTIO_PCI_SPECIFIC_CFG_OFFSET	(VIRTIO_PCI_ISR_ACCESS_OFFSET + VIRTIO_PCI_ISR_ACCESS_LENGTH)
#define VIRTIO_PCI_SPECIFIC_CFG_LENGTH	(0x1000)
#define VIRTIO_PCI_NOTIFICATIONS_OFFSET	(VIRTIO_PCI_SPECIFIC_CFG_OFFSET + VIRTIO_PCI_SPECIFIC_CFG_LENGTH)
#define VIRTIO_PCI_NOTIFICATIONS_LENGTH	(0x1000)

#define VIRTIO_PCI_BAR4_LENGTH		(VIRTIO_PCI_NOTIFICATIONS_OFFSET + VIRTIO_PCI_NOTIFICATIONS_LENGTH)

#define VIRTIO_DEV_MAX_IOVS		(129)
/* Maximum number of requests which can be processed one time */
#define VIRTIO_DEV_VRING_MAX_REQS	(32)
/* Maximum number of queues can be supported by virtio device */
#define VIRTIO_DEV_MAX_VQS		(64)
/* Default queue size */
#define VIRTIO_VQ_DEFAULT_SIZE		(128)
/* Maximum queue size */
#define VIRTIO_VQ_MAX_SIZE		(1024)

struct vfu_virtio_endpoint;
struct vfu_virtio_req;

struct virtio_pci_cfg {
	/* Common PCI configuration */
	uint32_t guest_feat_lo;
	uint32_t guest_feat_hi;

	/* Negotiated feature bits */
	uint64_t guest_features;

	uint32_t host_feature_select;
	uint32_t guest_feature_select;

	uint16_t msix_config;
	uint8_t device_status;
	uint8_t config_generation;
	uint16_t queue_select;

	/* ISR access */
	uint8_t isr;
};

enum vfu_vq_state {
	VFU_VQ_CREATED = 0,
	VFU_VQ_ACTIVE,
	VFU_VQ_INACTIVE,
};

struct q_mapping {
	/* iov of local process mapping. */
	struct iovec iov;
	/* Stored sg, needed for unmap. */
	dma_sg_t *sg;
	/* physical address */
	uint64_t phys_addr;
	/* virtual address */
	union {
		void *addr;

		struct vring_desc *desc;
		struct vring_packed_desc *desc_packed;

		struct vring_avail *avail;
		struct vring_packed_desc_event *driver_event;

		struct vring_used *used;
		struct vring_packed_desc_event *device_event;
	};
	/* size in bytes */
	uint64_t len;
};

struct vfu_virtio_vq {
	/* Read Only */
	uint16_t id;
	uint16_t qsize;

	bool enabled;
	uint16_t vector;

	enum vfu_vq_state q_state;
	STAILQ_HEAD(, vfu_virtio_req) free_reqs;

	uint32_t desc_lo;
	uint32_t desc_hi;
	uint32_t avail_lo;
	uint32_t avail_hi;
	uint32_t used_lo;
	uint32_t used_hi;

	struct q_mapping avail;
	struct q_mapping used;
	struct q_mapping desc;

	uint16_t last_avail_idx;
	uint16_t last_used_idx;

	struct {
		/* To mark a descriptor as available in packed ring
		 * Equal to avail_wrap_counter in spec.
		 */
		uint8_t avail_phase	: 1;
		/* To mark a descriptor as used in packed ring
		 * Equal to used_wrap_counter in spec.
		 */
		uint8_t used_phase	: 1;
		uint8_t padding		: 5;
		bool packed_ring	: 1;
	} packed;

	/* Request count from last event */
	uint16_t used_req_cnt;
	/* Next time when we need to send event */
	uint64_t next_event_time;
};

struct vfu_virtio_dev {
	char name[SPDK_VFU_MAX_NAME_LEN];
	/* RO for Guest Driver */
	uint16_t num_queues;
	/* Supported feature bits by host driver, RO for Guest Driver */
	uint64_t host_features;

	struct virtio_pci_cfg cfg;
	struct vfu_virtio_vq vqs[VIRTIO_DEV_MAX_VQS];

	struct vfu_virtio_endpoint *virtio_endpoint;

	/* VIRTIO_DEV_MAX_VQS * 3 worth of dma_sg_size() */
	uint8_t sg[];
};

struct vfu_virtio_ops {
	uint64_t (*get_device_features)(struct vfu_virtio_endpoint *virtio_endpoint);
	struct vfu_virtio_req *(*alloc_req)(struct vfu_virtio_endpoint *virtio_endpoint,
					    struct vfu_virtio_vq *vq);
	void (*free_req)(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
			 struct vfu_virtio_req *req);
	int (*exec_request)(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
			    struct vfu_virtio_req *req);
	int (*get_config)(struct vfu_virtio_endpoint *virtio_endpoint, char *buf, uint64_t offset,
			  uint64_t count);
	int (*set_config)(struct vfu_virtio_endpoint *virtio_endpoint, char *buf, uint64_t offset,
			  uint64_t count);
	int (*start_device)(struct vfu_virtio_endpoint *virtio_endpoint);
	int (*stop_device)(struct vfu_virtio_endpoint *virtio_endpoint);
};

struct vfu_virtio_endpoint {
	struct vfu_virtio_dev		*dev;
	int				devmem_fd;
	volatile uint32_t		*doorbells;

	uint16_t			num_queues;
	uint16_t			qsize;
	bool				packed_ring;

	uint32_t			coalescing_delay_us;

	struct spdk_vfu_endpoint	*endpoint;
	struct spdk_thread		*thread;

	struct vfu_virtio_ops		virtio_ops;

	/* quiesce poller */
	uint32_t			io_outstanding;
	bool				quiesce_in_progress;
	struct spdk_poller		*quiesce_poller;
};

struct vfu_virtio_req {
	struct vfu_virtio_dev *dev;
	struct vfu_virtio_vq *vq;

	STAILQ_ENTRY(vfu_virtio_req) link;

	uint32_t payload_size;
	uint32_t used_len;

	/* split vring */
	uint16_t req_idx;
	/* packed vring */
	uint16_t buffer_id;
	uint16_t num_descs;

	uint16_t iovcnt;
	struct iovec iovs[VIRTIO_DEV_MAX_IOVS + 1];
	uint8_t desc_writeable[VIRTIO_DEV_MAX_IOVS + 1];

	struct iovec *indirect_iov;
	dma_sg_t *indirect_sg;

	/* VIRIO_DEV_MAX_IOVS + 1 worth of dma_sg_size() */
	uint8_t sg[];
};

static inline bool
virtio_guest_has_feature(struct vfu_virtio_dev *dev, uint32_t feature_bit)
{
	assert(feature_bit <= 64);

	return !!(dev->cfg.guest_features & (1ULL << feature_bit));
}

static inline uint64_t
virtio_queue_desc_size(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	return sizeof(struct vring_desc) * vq->qsize;
}

static inline uint64_t
virtio_queue_avail_size(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	uint16_t event_size;

	if (virtio_guest_has_feature(dev, VIRTIO_F_RING_PACKED)) {
		return sizeof(struct vring_packed_desc_event);
	}

	event_size = virtio_guest_has_feature(dev, VIRTIO_RING_F_EVENT_IDX) ? 2 : 0;
	return (sizeof(struct vring_avail) + sizeof(uint16_t) * vq->qsize
		+ event_size);
}

static inline uint64_t
virtio_queue_used_size(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	uint16_t event_size;

	if (virtio_guest_has_feature(dev, VIRTIO_F_RING_PACKED)) {
		return sizeof(struct vring_packed_desc_event);
	}

	event_size = virtio_guest_has_feature(dev, VIRTIO_RING_F_EVENT_IDX) ? 2 : 0;
	return (sizeof(struct vring_used) + sizeof(struct vring_used_elem) * vq->qsize
		+ event_size);
}

static inline bool
virtio_queue_event_is_suppressed(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	bool is_suppressed = false;

	if (virtio_guest_has_feature(dev, VIRTIO_F_RING_PACKED)) {
		is_suppressed = vq->avail.driver_event->flags & VRING_PACKED_EVENT_FLAG_DISABLE;
	} else {
		is_suppressed = vq->avail.avail->flags & VRING_AVAIL_F_NO_INTERRUPT;

	}

	return is_suppressed;
}

static inline bool
virtio_dev_is_started(struct vfu_virtio_dev *dev)
{
	return !!(dev->cfg.device_status & VIRTIO_CONFIG_S_DRIVER_OK);
}

static inline bool
virtio_vring_split_desc_is_indirect(struct vring_desc *desc)
{
	return !!(desc->flags & VRING_DESC_F_INDIRECT);
}

static inline bool
virtio_vring_packed_desc_is_indirect(struct vring_packed_desc *desc)
{
	return !!(desc->flags & VRING_DESC_F_INDIRECT);
}

static inline bool
virtio_vring_split_desc_is_wr(struct vring_desc *desc)
{
	return !!(desc->flags & VRING_DESC_F_WRITE);
}

static inline bool
virtio_vring_packed_desc_is_wr(struct vring_packed_desc *desc)
{
	return !!(desc->flags & VRING_DESC_F_WRITE);
}

static inline bool
virtio_vring_packed_is_avail(struct vring_packed_desc *desc, bool avail_phase)
{
	bool avail_flag, used_flag;
	uint16_t flags = desc->flags;

	avail_flag = !!(flags & (1 << VRING_PACKED_DESC_F_AVAIL));
	used_flag = !!(flags & (1 << VRING_PACKED_DESC_F_USED));

	/* To mark a desc as available, the driver sets the F_AVAIL bit in flags
	 * to match the internal avail wrap counter. It also sets the F_USED bit to
	 * match the inverse value but it's not mandatory.
	 */
	return (avail_flag != used_flag) && (avail_flag == avail_phase);
}

static inline bool
virtio_vring_packed_is_used(struct vring_packed_desc *desc, bool used_phase)
{
	bool avail_flag, used_flag;
	uint16_t flags = desc->flags;

	avail_flag = !!(flags & (1 << VRING_PACKED_DESC_F_AVAIL));
	used_flag = !!(flags & (1 << VRING_PACKED_DESC_F_USED));

	/* When the descriptor is used, two flags in descriptor
	 * avail flag and used flag are set to equal
	 * and used flag value == used_wrap_counter.
	 */
	return (used_flag == avail_flag) && (used_flag == used_phase);
}

static inline bool
virtio_req_iov_is_wr(struct vfu_virtio_req *req, uint32_t iov_num)
{
	assert(iov_num <= VIRTIO_DEV_MAX_IOVS);
	return req->desc_writeable[iov_num];
}

static inline struct vfu_virtio_req *
vfu_virtio_vq_alloc_req(struct vfu_virtio_endpoint *endpoint, struct vfu_virtio_vq *vq)
{
	assert(endpoint->virtio_ops.alloc_req != NULL);
	return endpoint->virtio_ops.alloc_req(endpoint, vq);
}

static inline void
vfu_virtio_vq_free_req(struct vfu_virtio_endpoint *endpoint, struct vfu_virtio_vq *vq,
		       struct vfu_virtio_req *req)
{
	assert(endpoint->virtio_ops.free_req);
	endpoint->virtio_ops.free_req(endpoint, vq, req);
}

void virtio_vq_used_ring_split_enqueue(struct vfu_virtio_vq *vq, uint16_t req_idx,
				       uint32_t used_len);
void virtio_vq_used_ring_packed_enqueue(struct vfu_virtio_vq *vq, uint16_t buffer_id,
					uint32_t num_descs, uint32_t used_len);
struct vfu_virtio_req *virito_dev_packed_ring_get_next_avail_req(struct vfu_virtio_dev *dev,
		struct vfu_virtio_vq *vq);
struct vfu_virtio_req *virito_dev_split_ring_get_next_avail_req(struct vfu_virtio_dev *dev,
		struct vfu_virtio_vq *vq);

int vfu_virtio_quiesce_cb(struct spdk_vfu_endpoint *endpoint);

void vfu_virtio_dev_put_req(struct vfu_virtio_req *req);
void vfu_virtio_finish_req(struct vfu_virtio_req *req);
void vfu_virtio_vq_flush_irq(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq);
int vfu_virito_dev_process_packed_ring(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq);
int vfu_virito_dev_process_split_ring(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq);
void vfu_virtio_notify_config(struct vfu_virtio_endpoint *virtio_endpoint);
int vfu_virtio_endpoint_setup(struct vfu_virtio_endpoint *virtio_endpoint,
			      struct spdk_vfu_endpoint *endpoint,
			      char *basename, const char *endpoint_name,
			      struct vfu_virtio_ops *ops);
int vfu_virtio_endpoint_destruct(struct vfu_virtio_endpoint *virtio_endpoint);
void vfu_virtio_get_device_info(struct vfu_virtio_endpoint *virtio_endpoint,
				struct spdk_vfu_pci_device *device_info);
int vfu_virtio_attach_device(struct spdk_vfu_endpoint *endpoint);
int vfu_virtio_detach_device(struct spdk_vfu_endpoint *endpoint);
uint16_t vfu_virtio_get_vendor_capability(struct spdk_vfu_endpoint *endpoint, char *buf,
		uint16_t buf_len, uint16_t idx);
int vfu_virtio_post_memory_add(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);
int vfu_virtio_pre_memory_remove(struct spdk_vfu_endpoint *endpoint, void *map_start,
				 void *map_end);
int vfu_virtio_pci_reset_cb(struct spdk_vfu_endpoint *endpoint);
int vfu_virtio_blk_add_bdev(const char *name, const char *bdev_name,
			    uint16_t num_queues, uint16_t qsize, bool packed_ring);
/* virtio_scsi */
int vfu_virtio_scsi_add_target(const char *name, uint8_t scsi_target_num,
			       const char *bdev_name);
int vfu_virtio_scsi_remove_target(const char *name, uint8_t scsi_target_num);
int vfu_virtio_scsi_set_options(const char *name, uint16_t num_io_queues, uint16_t qsize,
				bool packed_ring);
#endif
