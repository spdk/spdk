/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_branch_prediction.h>
#include <rte_prefetch.h>

#include "virtio_logs.h"
#include "virtio_ethdev.h"
#include "virtio_pci.h"
#include "virtqueue.h"

#include "spdk/env.h"

static void
vq_ring_free_chain(struct virtqueue *vq, uint16_t desc_idx)
{
	struct vring_desc *dp, *dp_tail;
	struct vq_desc_extra *dxp;
	uint16_t desc_idx_last = desc_idx;

	dp  = &vq->vq_ring.desc[desc_idx];
	dxp = &vq->vq_descx[desc_idx];
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt + dxp->ndescs);
	if ((dp->flags & VRING_DESC_F_INDIRECT) == 0) {
		while (dp->flags & VRING_DESC_F_NEXT) {
			desc_idx_last = dp->next;
			dp = &vq->vq_ring.desc[dp->next];
		}
	}
	dxp->ndescs = 0;

	/*
	 * We must append the existing free chain, if any, to the end of
	 * newly freed chain. If the virtqueue was completely used, then
	 * head would be VQ_RING_DESC_CHAIN_END (ASSERTed above).
	 */
	if (vq->vq_desc_tail_idx == VQ_RING_DESC_CHAIN_END) {
		vq->vq_desc_head_idx = desc_idx;
	} else {
		dp_tail = &vq->vq_ring.desc[vq->vq_desc_tail_idx];
		dp_tail->next = desc_idx;
	}

	vq->vq_desc_tail_idx = desc_idx_last;
	dp->next = VQ_RING_DESC_CHAIN_END;
}

static uint16_t
virtqueue_dequeue_burst_rx(struct virtqueue *vq, struct virtio_req **rx_pkts,
			   uint32_t *len, uint16_t num)
{
	struct vring_used_elem *uep;
	struct virtio_req *cookie;
	uint16_t used_idx, desc_idx;
	uint16_t i;

	/*  Caller does the check */
	for (i = 0; i < num ; i++) {
		used_idx = (uint16_t)(vq->vq_used_cons_idx & (vq->vq_nentries - 1));
		uep = &vq->vq_ring.used->ring[used_idx];
		desc_idx = (uint16_t) uep->id;
		len[i] = uep->len;
		cookie = (struct virtio_req *)vq->vq_descx[desc_idx].cookie;

		if (unlikely(cookie == NULL)) {
			PMD_DRV_LOG(ERR, "vring descriptor with no mbuf cookie at %u",
				vq->vq_used_cons_idx);
			break;
		}

		rte_prefetch0(cookie);
		rx_pkts[i]  = cookie;
		vq->vq_used_cons_idx++;
		vq_ring_free_chain(vq, desc_idx);
		vq->vq_descx[desc_idx].cookie = NULL;
	}

	return i;
}

#ifndef DEFAULT_TX_FREE_THRESH
#define DEFAULT_TX_FREE_THRESH 32
#endif

/* avoid write operation when necessary, to lessen cache issues */
#define ASSIGN_UNLESS_EQUAL(var, val) do {	\
	if ((var) != (val))			\
		(var) = (val);			\
} while (0)

static inline void
virtqueue_iov_to_desc(struct virtqueue *vq, uint16_t desc_idx, struct iovec *iov)
{
	if (vq->hw->virtio_user_dev) {
		vq->vq_ring.desc[desc_idx].addr  = (uintptr_t)iov->iov_base;
	} else {
		vq->vq_ring.desc[desc_idx].addr = spdk_vtophys(iov->iov_base);
	}

	vq->vq_ring.desc[desc_idx].len = iov->iov_len;
}

static inline void
virtqueue_enqueue_xmit(struct virtqueue *vq, struct virtio_req *req)
{
	struct vq_desc_extra *dxp;
	struct vring_desc *descs;
	uint32_t i;
	uint16_t head_idx, idx;
	struct iovec *iov = req->iov;

	head_idx = vq->vq_desc_head_idx;
	idx = head_idx;
	dxp = &vq->vq_descx[idx];
	dxp->cookie = (void *)req;
	dxp->ndescs = req->iovcnt;

	descs = vq->vq_ring.desc;

	virtqueue_iov_to_desc(vq, idx, &req->iov_req);
	descs[idx].flags = VRING_DESC_F_NEXT;
	idx = descs[idx].next;

	if (req->is_write) {
		for (i = 0; i < req->iovcnt; i++) {
			virtqueue_iov_to_desc(vq, idx, &iov[i]);
			descs[idx].flags = VRING_DESC_F_NEXT;
			idx = descs[idx].next;
		}

		virtqueue_iov_to_desc(vq, idx, &req->iov_resp);
		descs[idx].flags = VRING_DESC_F_WRITE;
		idx = descs[idx].next;
	} else {
		virtqueue_iov_to_desc(vq, idx, &req->iov_resp);
		descs[idx].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
		idx = descs[idx].next;

		for (i = 0; i < req->iovcnt; i++) {
			virtqueue_iov_to_desc(vq, idx, &iov[i]);
			descs[idx].flags = VRING_DESC_F_WRITE;
			descs[idx].flags |= (i + 1) != req->iovcnt ? VRING_DESC_F_NEXT : 0;
			idx = descs[idx].next;
		}
	}

	vq->vq_desc_head_idx = idx;
	if (vq->vq_desc_head_idx == VQ_RING_DESC_CHAIN_END)
		vq->vq_desc_tail_idx = idx;
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt - req->iovcnt);
	vq_update_avail_ring(vq, head_idx);
}

/*
 * struct rte_eth_dev *dev: Used to update dev
 * uint16_t nb_desc: Defaults to values read from config space
 * unsigned int socket_id: Used to allocate memzone
 * const struct rte_eth_txconf *tx_conf: Used to setup tx engine
 * uint16_t queue_idx: Just used as an index in dev txq list
 */
int
virtio_dev_tx_queue_setup(struct virtio_hw *hw,
			uint16_t queue_idx,
			uint16_t nb_desc,
			unsigned int socket_id __rte_unused)
{
	struct virtqueue *vq = hw->vqs[queue_idx];

	PMD_INIT_FUNC_TRACE();

	if (nb_desc == 0 || nb_desc > vq->vq_nentries)
		nb_desc = vq->vq_nentries;
	vq->vq_free_cnt = RTE_MIN(vq->vq_free_cnt, nb_desc);
	return 0;
}

#define VIRTIO_MBUF_BURST_SZ 64
#define DESC_PER_CACHELINE (RTE_CACHE_LINE_SIZE / sizeof(struct vring_desc))
uint16_t
virtio_recv_pkts(struct virtqueue *vq, struct virtio_req **reqs, uint16_t nb_pkts)
{
	struct virtio_hw *hw = vq->hw;
	struct virtio_req *rxm;
	uint16_t nb_used, num, nb_rx;
	uint32_t len[VIRTIO_MBUF_BURST_SZ];
	struct virtio_req *rcv_pkts[VIRTIO_MBUF_BURST_SZ];
	uint32_t i;

	nb_rx = 0;
	if (unlikely(hw->started == 0))
		return nb_rx;

	nb_used = VIRTQUEUE_NUSED(vq);

	virtio_rmb();

	num = (uint16_t)(likely(nb_used <= nb_pkts) ? nb_used : nb_pkts);
	num = (uint16_t)(likely(num <= VIRTIO_MBUF_BURST_SZ) ? num : VIRTIO_MBUF_BURST_SZ);
	if (likely(num > DESC_PER_CACHELINE))
		num = num - ((vq->vq_used_cons_idx + num) % DESC_PER_CACHELINE);

	num = virtqueue_dequeue_burst_rx(vq, rcv_pkts, len, num);
	PMD_RX_LOG(DEBUG, "used:%d dequeue:%d", nb_used, num);

	for (i = 0; i < num ; i++) {
		rxm = rcv_pkts[i];

		PMD_RX_LOG(DEBUG, "packet len:%d", len[i]);

		rxm->data_transferred = (uint16_t)(len[i]);

		reqs[nb_rx++] = rxm;
	}

	return nb_rx;
}

uint16_t
virtio_xmit_pkts(struct virtqueue *vq, struct virtio_req *req)
{
	struct virtio_hw *hw = vq->hw;

	if (unlikely(hw->started == 0))
		return 0;

	virtio_rmb();

	virtqueue_enqueue_xmit(vq, req);

	vq_update_avail_idx(vq);

	if (unlikely(virtqueue_kick_prepare(vq))) {
		virtqueue_notify(vq);
		PMD_TX_LOG(DEBUG, "Notified backend after xmit");
	}

	return 1;
}
