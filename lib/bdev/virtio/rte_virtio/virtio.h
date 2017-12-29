/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#ifndef SPDK_VIRTIO_H
#define SPDK_VIRTIO_H

#include "spdk/stdinc.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_config.h>

#include <rte_config.h>
#include <rte_mempool.h>

#include "spdk_internal/log.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/json.h"
#include "spdk/io_channel.h"

#define VIRTQUEUE_MAX_NAME_SZ 32

/**
 * The maximum virtqueue size is 2^15. Use that value as the end of
 * descriptor chain terminator since it will never be a valid index
 * in the descriptor table. This is used to verify we are correctly
 * handling vq_free_cnt.
 */
#define VQ_RING_DESC_CHAIN_END 32768

/* Number of non-request queues - eventq and controlq */
#define SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED 2

/* Extra status define for readability */
#define VIRTIO_CONFIG_S_RESET 0

struct virtio_dev_ops;

struct virtio_dev {
	struct virtqueue **vqs;

	/** Name of this virtio dev set by backend */
	char		*name;
	uint16_t	started;

	/** Max number of queues the host supports. */
	uint16_t	max_queues;

	/** Device index. */
	uint32_t	id;

	/** Common device & guest features. */
	uint64_t	negotiated_features;

	int		is_hw;

	/** Modern/legacy virtio device flag. */
	uint8_t		modern;

	/** Mutex for asynchronous virtqueue-changing operations. */
	pthread_mutex_t	mutex;

	/** Backend-specific callbacks. */
	const struct virtio_dev_ops *backend_ops;

	/** Context for the backend ops */
	void		*ctx;

	TAILQ_ENTRY(virtio_dev) tailq;
};

struct virtio_dev_ops {
	void (*read_dev_cfg)(struct virtio_dev *hw, size_t offset,
			     void *dst, int len);
	void (*write_dev_cfg)(struct virtio_dev *hw, size_t offset,
			      const void *src, int len);
	uint8_t (*get_status)(struct virtio_dev *hw);
	void (*set_status)(struct virtio_dev *hw, uint8_t status);

	/**
	 * Get device features. The features might be already
	 * negotiated with driver (guest) features.
	 */
	uint64_t (*get_features)(struct virtio_dev *vdev);

	/**
	 * Negotiate and set device features.
	 * The negotiation can fail with return code -1.
	 * This function should also set vdev->negotiated_features field.
	 */
	int (*set_features)(struct virtio_dev *vdev, uint64_t features);

	/** Deinit and free virtio device */
	void (*free_vdev)(struct virtio_dev *vdev);

	uint16_t (*get_queue_num)(struct virtio_dev *hw, uint16_t queue_id);
	int (*setup_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*del_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*notify_queue)(struct virtio_dev *hw, struct virtqueue *vq);

	void (*dump_json_config)(struct virtio_dev *hw, struct spdk_json_write_ctx *w);
};

struct vq_desc_extra {
	void *cookie;
	uint16_t ndescs;
};

struct virtqueue {
	struct virtio_dev *vdev; /**< owner of this virtqueue */
	struct vring vq_ring;  /**< vring keeping desc, used and avail */
	/**
	 * Last consumed descriptor in the used table,
	 * trails vq_ring.used->idx.
	 */
	uint16_t vq_used_cons_idx;
	uint16_t vq_nentries;  /**< vring desc numbers */
	uint16_t vq_free_cnt;  /**< num of desc available */
	uint16_t vq_avail_idx; /**< sync until needed */

	void *vq_ring_virt_mem;  /**< virtual address of vring */
	unsigned int vq_ring_size;

	const struct rte_memzone *mz;    /**< mem zone to populate TX ring. */

	phys_addr_t vq_ring_mem; /**< physical address of vring */

	/**
	 * Head of the free chain in the descriptor table. If
	 * there are no free descriptors, this will be set to
	 * VQ_RING_DESC_CHAIN_END.
	 */
	uint16_t  vq_desc_head_idx;

	/**
	 * Tail of the free chain in desc table. If
	 * there are no free descriptors, this will be set to
	 * VQ_RING_DESC_CHAIN_END.
	 */
	uint16_t  vq_desc_tail_idx;
	uint16_t  vq_queue_index;   /**< PCI queue index */
	uint16_t  *notify_addr;

	/** Thread that's polling this queue. */
	struct spdk_thread *owner_thread;

	/** Response poller. */
	struct spdk_poller	*poller;

	/** Context for response poller. */
	void *poller_ctx;

	struct vq_desc_extra vq_descx[0];
};

struct virtio_req {
	struct iovec	*iov;
	struct iovec	iov_req;
	struct iovec	iov_resp;
	uint32_t	iovcnt;
	int		is_write;
	uint32_t	data_transferred;
};

struct virtio_driver {
	TAILQ_HEAD(, virtio_dev) init_ctrlrs;
	TAILQ_HEAD(, virtio_dev) attached_ctrlrs;

	/* Increment-only virtio_dev counter */
	unsigned ctrlr_counter;
};

extern struct virtio_driver g_virtio_driver;

/* Features desired/implemented by this driver. */
#define VIRTIO_SCSI_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_SCSI_F_INOUT		|	\
	 1ULL << VIRTIO_F_VERSION_1)

uint16_t virtio_recv_pkts(struct virtqueue *vq, struct virtio_req **reqs,
			  uint16_t nb_pkts);

/**
 * Put given request into the virtqueue.  The virtio device owning
 * the virtqueue must be started. This will also send an interrupt unless
 * the host explicitly set VRING_USED_F_NO_NOTIFY in virtqueue flags.
 *
 * \param vq virtio queue
 * \param req virtio request
 * \return 0 on success, negative errno on error. In case the ring is full
 * or no free descriptors are available -ENOMEM is returned. If virtio
 * device owning the virtqueue is not started -EIO is returned.
 */
int virtio_xmit_pkt(struct virtqueue *vq, struct virtio_req *req);

/**
 * Construct virtio device.  This will set vdev->id field.
 * The device has to be freed with \c virtio_dev_free.
 *
 * \param ops backend callbacks
 * \param ctx argument for the backend callbacks
 */
struct virtio_dev *virtio_dev_construct(const struct virtio_dev_ops *ops, void *ctx);

int virtio_dev_init(struct virtio_dev *hw, uint64_t req_features);
void virtio_dev_free(struct virtio_dev *dev);
int virtio_dev_start(struct virtio_dev *hw);

/**
 * Bind a virtqueue with given index to the current thread;
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index virtqueue index
 * \return 0 on success, -1 in case a virtqueue with given index either
 * does not exists or is already acquired.
 */
int virtio_dev_acquire_queue(struct virtio_dev *vdev, uint16_t index);

/**
 * Look for unused queue and bind it to the current thread.  This will
 * scan the queues in range from *start_index* (inclusive) up to
 * vdev->max_queues (exclusive).
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param start_index virtqueue index to start looking from
 * \return index of acquired queue or -1 in case no unused queue in given range
 * has been found
 */
int32_t virtio_dev_find_and_acquire_queue(struct virtio_dev *vdev, uint16_t start_index);

/**
 * Get thread that acquired given virtqueue.
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index index of virtqueue
 * \return thread that acquired given virtqueue. If the queue is unused
 * or doesn't exist a NULL is returned.
 */
struct spdk_thread *virtio_dev_queue_get_thread(struct virtio_dev *vdev, uint16_t index);

/**
 * Check if virtqueue with given index is acquired.
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index index of virtqueue
 * \return virtqueue acquire status. in case of invalid index *false* is returned.
 */
bool virtio_dev_queue_is_acquired(struct virtio_dev *vdev, uint16_t index);

/**
 * Release previously acquired queue.
 *
 * This function must be called from the thread that acquired the queue.
 *
 * \param vdev vhost device
 * \param index index of virtqueue to release
 */
void virtio_dev_release_queue(struct virtio_dev *vdev, uint16_t index);

/**
 * Reset given virtio device.  This will leave the device in unusable state.
 * To reuse the device, call \c virtio_dev_init.
 *
 * \param vdev virtio device
 */
void virtio_dev_reset(struct virtio_dev *vdev);

/**
 * Get Virtio status flags.
 *
 * \param vdev virtio device
 */
uint8_t virtio_dev_get_status(struct virtio_dev *vdev);

/**
 * Set Virtio status flag.  The flags have to be set in very specific order
 * defined the VirtIO 1.0 spec section 3.1.1. To unset the flags, call
 * \c virtio_dev_reset or set \c VIRTIO_CONFIG_S_RESET flag. There is
 * no way to unset particular flags.
 *
 * \param vdev virtio device
 * \param flag flag to set
 */
void virtio_dev_set_status(struct virtio_dev *vdev, uint8_t flag);

/**
 * Write raw data into the device config at given offset.  This call does not
 * provide any error checking.
 *
 * \param vdev virtio device
 * \param offset offset in bytes
 * \param src pointer to data to copy from
 * \param len length of data to copy in bytes
 */
void virtio_dev_write_dev_config(struct virtio_dev *vdev, size_t offset, const void *src, int len);

/**
 * Read raw data from the device config at given offset.  This call does not
 * provide any error checking.
 *
 * \param vdev virtio device
 * \param offset offset in bytes
 * \param dst pointer to buffer to copy data into
 * \param len length of data to copy in bytes
 */
void virtio_dev_read_dev_config(struct virtio_dev *vdev, size_t offset, void *dst, int len);

/**
 * Get backend-specific ops for given device.
 *
 * \param vdev virtio device
 */
const struct virtio_dev_ops *virtio_dev_backend_ops(struct virtio_dev *vdev);

/**
 * Check if the device has negotiated given feature bit.
 *
 * \param vdev virtio device
 * \param bit feature bit
 */
static inline bool
virtio_dev_has_feature(struct virtio_dev *vdev, uint64_t bit)
{
	return !!(vdev->negotiated_features & (1ULL << bit));
}

/**
 * Dump all device specific information into given json stream.
 *
 * \param vdev virtio device
 * \param w json stream
 */
void virtio_dev_dump_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w);

/**
 * Init all compatible Virtio PCI devices.
 */
int virtio_enumerate_pci(void);

/**
 * Connect to a vhost-user device and create corresponding virtio_dev.
 *
 * \param name name of this virtio device
 * \param path path to the Unix domain socket of the vhost-user device
 * \param requested_queues maximum number of request queues that this
 * device will support
 * \param queue_size size of each of the queues
 * \param fixed_queue_num number of queues preceeding the first
 * request queue. For Virtio-SCSI this is equal to 2, as there are
 * additional event and control queues.
 * \return virtio device
 */
struct virtio_dev *virtio_user_dev_init(const char *name, const char *path,
					uint16_t requested_queues,
					uint32_t queue_size, uint16_t fixed_queue_num);

#endif /* SPDK_VIRTIO_H */
