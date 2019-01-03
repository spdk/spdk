/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

/* XXX This file is based on drivers/net/virtio/virtqueue.h.  It would be
 * better to create a shared rte_virtio library instead of duplicating this
 * code.
 */

#ifndef _VIRTQUEUE_H_
#define _VIRTQUEUE_H_

#include <stdint.h>
#include <linux/virtio_ring.h>

#include <rte_atomic.h>
#include <rte_memory.h>
#include <rte_mempool.h>

#include "virtio_pci.h"

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

struct vq_desc_extra {
	void *cookie;
	uint16_t ndescs;
};

struct virtqueue {
	struct virtio_hw  *hw; /**< virtio_hw structure pointer. */
	struct vring vq_ring;  /**< vring keeping desc, used and avail */
	/**
	 * Last consumed descriptor in the used table,
	 * trails vq_ring.used->idx.
	 */
	uint16_t vq_used_cons_idx;
	uint16_t vq_nentries;  /**< vring desc numbers */
	uint16_t vq_free_cnt;  /**< num of desc available */
	uint16_t vq_avail_idx; /**< sync until needed */
	uint16_t vq_free_thresh; /**< free threshold */

	void *vq_ring_virt_mem;  /**< linear address of vring*/
	unsigned int vq_ring_size;

	rte_iova_t vq_ring_mem; /**< physical address of vring */

	const struct rte_memzone *mz; /**< memzone backing vring */

	/**
	 * Head of the free chain in the descriptor table. If
	 * there are no free descriptors, this will be set to
	 * VQ_RING_DESC_CHAIN_END.
	 */
	uint16_t  vq_desc_head_idx;
	uint16_t  vq_desc_tail_idx;
	uint16_t  vq_queue_index;   /**< PCI queue index */
	uint16_t  *notify_addr;
	struct vq_desc_extra vq_descx[0];
};

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

/**
 * Tell the backend to interrupt us.
 */
static inline void
virtqueue_enable_intr(struct virtqueue *vq)
{
	vq->vq_ring.avail->flags &= (~VRING_AVAIL_F_NO_INTERRUPT);
}

/**
 *  Dump virtqueue internal structures, for debug purpose only.
 */
void virtqueue_dump(struct virtqueue *vq);

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
	if (unlikely(vq->vq_ring.avail->ring[avail_idx] != desc_idx))
		vq->vq_ring.avail->ring[avail_idx] = desc_idx;
	vq->vq_avail_idx++;
}

static inline int
virtqueue_kick_prepare(struct virtqueue *vq)
{
	return !(vq->vq_ring.used->flags & VRING_USED_F_NO_NOTIFY);
}

static inline void
virtqueue_notify(struct virtqueue *vq)
{
	/*
	 * Ensure updated avail->idx is visible to host.
	 * For virtio on IA, the notificaiton is through io port operation
	 * which is a serialization instruction itself.
	 */
	VTPCI_OPS(vq->hw)->notify_queue(vq->hw, vq);
}

#ifdef RTE_LIBRTE_VIRTIO_DEBUG_DUMP
#define VIRTQUEUE_DUMP(vq) do { \
	uint16_t used_idx, nused; \
	used_idx = (vq)->vq_ring.used->idx; \
	nused = (uint16_t)(used_idx - (vq)->vq_used_cons_idx); \
	RTE_LOG(DEBUG, VIRTIO_PCI_CONFIG, \
	  "VQ: - size=%d; free=%d; used=%d; desc_head_idx=%d;" \
	  " avail.idx=%d; used_cons_idx=%d; used.idx=%d;" \
	  " avail.flags=0x%x; used.flags=0x%x\n", \
	  (vq)->vq_nentries, (vq)->vq_free_cnt, nused, \
	  (vq)->vq_desc_head_idx, (vq)->vq_ring.avail->idx, \
	  (vq)->vq_used_cons_idx, (vq)->vq_ring.used->idx, \
	  (vq)->vq_ring.avail->flags, (vq)->vq_ring.used->flags); \
} while (0)
#else
#define VIRTQUEUE_DUMP(vq) do { } while (0)
#endif

#endif /* _VIRTQUEUE_H_ */
