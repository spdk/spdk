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

#ifndef _VIRTIO_DEV_H_
#define _VIRTIO_DEV_H_

#include "spdk/stdinc.h"

#include <linux/virtio_ring.h>

#include <rte_config.h>
#include <rte_memory.h>
#include <rte_mempool.h>

#include "spdk_internal/log.h"
#include "spdk/likely.h"
#include "spdk/queue.h"

/*
 * Per virtio_config.h in Linux.
 *     For virtio_pci on SMP, we don't need to order with respect to MMIO
 *     accesses through relaxed memory I/O windows, so smp_mb() et al are
 *     sufficient.
 *
 */
#define virtio_mb()	rte_smp_mb()
#define virtio_rmb()	rte_smp_rmb()
#define virtio_wmb()	rte_smp_wmb()

#define VIRTQUEUE_MAX_NAME_SZ 32

/**
 * The maximum virtqueue size is 2^15. Use that value as the end of
 * descriptor chain terminator since it will never be a valid index
 * in the descriptor table. This is used to verify we are correctly
 * handling vq_free_cnt.
 */
#define VQ_RING_DESC_CHAIN_END 32768

/* This is a work-around for fio-plugin bug, where each
 * fio job thread returns local lcore id = -1
 */
#define SPDK_VIRTIO_QUEUE_LCORE_ID_UNUSED (UINT32_MAX - 1)

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

	TAILQ_ENTRY(virtio_dev) tailq;
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

	/** Logical CPU ID that's polling this queue. */
	uint32_t owner_lcore;

	/** Response poller. */
	struct spdk_bdev_poller	*poller;

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

int virtio_dev_init(struct virtio_dev *hw, uint64_t req_features);
void virtio_dev_free(struct virtio_dev *dev);
int virtio_dev_start(struct virtio_dev *hw);

/* Chain all the descriptors in the ring with an END */
static inline void
vring_desc_init(struct vring_desc *dp, uint16_t n)
{
	uint16_t i;

	for (i = 0; i < n - 1; i++)
		dp[i].next = (uint16_t)(i + 1);
	dp[i].next = VQ_RING_DESC_CHAIN_END;
}

/**
 * Tell the backend not to interrupt us.
 */
static inline void
virtqueue_disable_intr(struct virtqueue *vq)
{
	vq->vq_ring.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
}

static inline int
virtqueue_full(const struct virtqueue *vq)
{
	return vq->vq_free_cnt == 0;
}

#define VIRTQUEUE_NUSED(vq) ((uint16_t)((vq)->vq_ring.used->idx - (vq)->vq_used_cons_idx))

static inline void
vq_update_avail_idx(struct virtqueue *vq)
{
	virtio_wmb();
	vq->vq_ring.avail->idx = vq->vq_avail_idx;
}

static inline void
vq_update_avail_ring(struct virtqueue *vq, uint16_t desc_idx)
{
	uint16_t avail_idx;
	/*
	 * Place the head of the descriptor chain into the next slot and make
	 * it usable to the host. The chain is made available now rather than
	 * deferring to virtqueue_notify() in the hopes that if the host is
	 * currently running on another CPU, we can keep it processing the new
	 * descriptor.
	 */
	avail_idx = (uint16_t)(vq->vq_avail_idx & (vq->vq_nentries - 1));
	if (spdk_unlikely(vq->vq_ring.avail->ring[avail_idx] != desc_idx))
		vq->vq_ring.avail->ring[avail_idx] = desc_idx;
	vq->vq_avail_idx++;
}

static inline int
virtqueue_kick_prepare(struct virtqueue *vq)
{
	return !(vq->vq_ring.used->flags & VRING_USED_F_NO_NOTIFY);
}

/**
 * Bind a virtqueue with given index to the current CPU core.
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
 * Look for unused queue and bind it to the current CPU core.  This will
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

#endif /* _VIRTIO_DEV_H_ */
