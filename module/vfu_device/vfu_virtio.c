/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * virtio over vfio-user common library
 */
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/barrier.h"
#include "spdk/thread.h"
#include "spdk/memory.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "vfu_virtio_internal.h"

static int vfu_virtio_dev_start(struct vfu_virtio_dev *dev);
static int vfu_virtio_dev_stop(struct vfu_virtio_dev *dev);

static inline void
vfu_virtio_unmap_q(struct vfu_virtio_dev *dev, struct q_mapping *mapping)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;

	if (mapping->addr != NULL) {
		spdk_vfu_unmap_sg(virtio_endpoint->endpoint, mapping->sg,
				  &mapping->iov, 1);
		mapping->addr = NULL;
		mapping->len = 0;
	}
}

static inline int
vfu_virtio_map_q(struct vfu_virtio_dev *dev, struct q_mapping *mapping, uint64_t phys_addr,
		 uint64_t len)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	void *addr;

	if (!mapping->addr && len && phys_addr) {
		addr = spdk_vfu_map_one(virtio_endpoint->endpoint, phys_addr, len,
					mapping->sg, &mapping->iov, PROT_READ | PROT_WRITE);
		if (addr == NULL) {
			return -EINVAL;
		}
		mapping->phys_addr = phys_addr;
		mapping->len = len;
		mapping->addr = addr;
	}

	return 0;
}

static int
virtio_dev_map_vq(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	int ret;
	uint64_t phys_addr, len;

	if (!vq->enabled || (vq->q_state == VFU_VQ_ACTIVE)) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio, "%s: try to map vq %u\n", dev->name, vq->id);

	len = virtio_queue_desc_size(dev, vq);
	phys_addr = ((((uint64_t)vq->desc_hi) << 32) | vq->desc_lo);
	ret = vfu_virtio_map_q(dev, &vq->desc, phys_addr, len);
	if (ret) {
		SPDK_DEBUGLOG(vfu_virtio, "Error to map descs\n");
		return ret;
	}

	len = virtio_queue_avail_size(dev, vq);
	phys_addr = ((((uint64_t)vq->avail_hi) << 32) | vq->avail_lo);
	ret = vfu_virtio_map_q(dev, &vq->avail, phys_addr, len);
	if (ret) {
		vfu_virtio_unmap_q(dev, &vq->desc);
		SPDK_DEBUGLOG(vfu_virtio, "Error to map available ring\n");
		return ret;
	}

	len = virtio_queue_used_size(dev, vq);
	phys_addr = ((((uint64_t)vq->used_hi) << 32) | vq->used_lo);
	ret = vfu_virtio_map_q(dev, &vq->used, phys_addr, len);
	if (ret) {
		vfu_virtio_unmap_q(dev, &vq->desc);
		vfu_virtio_unmap_q(dev, &vq->avail);
		SPDK_DEBUGLOG(vfu_virtio, "Error to map used ring\n");
		return ret;
	}

	/* We're running with polling mode */
	if (virtio_guest_has_feature(dev, VIRTIO_F_RING_PACKED)) {
		vq->used.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
	} else {
		vq->used.used->flags = VRING_USED_F_NO_NOTIFY;
	}

	SPDK_DEBUGLOG(vfu_virtio, "%s: map vq %u successfully\n", dev->name, vq->id);
	vq->q_state = VFU_VQ_ACTIVE;

	return 0;
}

static void
virtio_dev_unmap_vq(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	SPDK_DEBUGLOG(vfu_virtio, "%s: unmap vq %u\n", dev->name, vq->id);
	vq->q_state = VFU_VQ_INACTIVE;

	vfu_virtio_unmap_q(dev, &vq->desc);
	vfu_virtio_unmap_q(dev, &vq->avail);
	vfu_virtio_unmap_q(dev, &vq->used);
}

static bool
vfu_virtio_vq_should_unmap(struct vfu_virtio_vq *vq, void *map_start, void *map_end)
{
	/* always do unmap when stopping the device */
	if (!map_start || !map_end) {
		return true;
	}

	if (vq->desc.addr >= map_start && vq->desc.addr < map_end) {
		return true;
	}

	if (vq->avail.addr >= map_start && vq->avail.addr < map_end) {
		return true;
	}

	if (vq->used.addr >= map_start && vq->used.addr < map_end) {
		return true;
	}

	return false;
}

static void
vfu_virtio_dev_unmap_vqs(struct vfu_virtio_dev *dev, void *map_start, void *map_end)
{
	uint32_t i;
	struct vfu_virtio_vq *vq;

	for (i = 0; i < dev->num_queues; i++) {
		vq = &dev->vqs[i];
		if (!vq->enabled) {
			continue;
		}

		if (!vfu_virtio_vq_should_unmap(vq, map_start, map_end)) {
			continue;
		}
		virtio_dev_unmap_vq(dev, vq);
	}
}

/* This function is used to notify VM that the device
 * configuration space has been changed.
 */
void
vfu_virtio_notify_config(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct spdk_vfu_endpoint *endpoint = virtio_endpoint->endpoint;

	if (virtio_endpoint->dev == NULL) {
		return;
	}

	virtio_endpoint->dev->cfg.isr = 1;
	virtio_endpoint->dev->cfg.config_generation++;

	vfu_irq_trigger(spdk_vfu_get_vfu_ctx(endpoint), virtio_endpoint->dev->cfg.msix_config);
}

static void
vfu_virtio_dev_reset(struct vfu_virtio_dev *dev)
{
	uint32_t i;
	struct vfu_virtio_vq *vq;

	SPDK_DEBUGLOG(vfu_virtio, "device %s resetting\n", dev->name);

	for (i = 0; i < dev->num_queues; i++) {
		vq = &dev->vqs[i];

		vq->q_state = VFU_VQ_CREATED;
		vq->vector = 0;
		vq->enabled = false;
		vq->last_avail_idx = 0;
		vq->last_used_idx = 0;

		vq->packed.packed_ring = false;
		vq->packed.avail_phase = 0;
		vq->packed.used_phase = 0;
	}

	memset(&dev->cfg, 0, sizeof(struct virtio_pci_cfg));
}

static int
virtio_dev_set_status(struct vfu_virtio_dev *dev, uint8_t status)
{
	int ret = 0;

	SPDK_DEBUGLOG(vfu_virtio, "device current status %x, set status %x\n", dev->cfg.device_status,
		      status);

	if (!(virtio_dev_is_started(dev))) {
		if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
			ret = vfu_virtio_dev_start(dev);
		}
	} else {
		if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
			ret = vfu_virtio_dev_stop(dev);
		}
	}

	if (ret) {
		SPDK_ERRLOG("Failed to start/stop device\n");
		return ret;
	}

	dev->cfg.device_status = status;

	if (status == 0) {
		vfu_virtio_dev_reset(dev);
	}

	return 0;
}

static int
virtio_dev_set_features(struct vfu_virtio_dev *dev, uint64_t features)
{
	if (dev->cfg.device_status & VIRTIO_CONFIG_S_FEATURES_OK) {
		SPDK_ERRLOG("Feature negotiation has finished\n");
		return -EINVAL;
	}

	if (features & ~dev->host_features) {
		SPDK_ERRLOG("Host features 0x%"PRIx64", guest features 0x%"PRIx64"\n",
			    dev->host_features, features);
		return -ENOTSUP;
	}

	SPDK_DEBUGLOG(vfu_virtio, "%s: negotiated features 0x%"PRIx64"\n", dev->name,
		      features);
	dev->cfg.guest_features = features;

	return 0;
}

static int
virtio_dev_enable_vq(struct vfu_virtio_dev *dev, uint16_t qid)
{
	struct vfu_virtio_vq *vq;

	SPDK_DEBUGLOG(vfu_virtio, "%s: enable vq %u\n", dev->name, qid);

	vq = &dev->vqs[qid];
	if (vq->enabled) {
		SPDK_ERRLOG("Queue %u is enabled\n", qid);
		return -EINVAL;
	}
	vq->enabled = true;

	if (virtio_dev_map_vq(dev, vq)) {
		SPDK_ERRLOG("Queue %u failed to map\n", qid);
		return 0;
	}

	vq->avail.avail->idx = 0;
	vq->last_avail_idx = 0;
	vq->used.used->idx = 0;
	vq->last_used_idx = 0;

	if (virtio_guest_has_feature(dev, VIRTIO_F_RING_PACKED)) {
		SPDK_DEBUGLOG(vfu_virtio, "%s: vq %u PACKED RING ENABLED\n", dev->name, qid);
		vq->packed.packed_ring = true;
		vq->packed.avail_phase = true;
		vq->packed.used_phase = true;
	}

	return 0;
}

static int
virtio_dev_disable_vq(struct vfu_virtio_dev *dev, uint16_t qid)
{
	struct vfu_virtio_vq *vq;

	SPDK_DEBUGLOG(vfu_virtio, "%s: disable vq %u\n", dev->name, qid);

	vq = &dev->vqs[qid];
	if (!vq->enabled) {
		SPDK_NOTICELOG("Queue %u isn't enabled\n", qid);
		return 0;
	}

	virtio_dev_unmap_vq(dev, vq);

	vq->q_state = VFU_VQ_CREATED;
	vq->vector = 0;
	vq->enabled = false;
	vq->last_avail_idx = 0;
	vq->last_used_idx = 0;
	vq->packed.packed_ring = false;
	vq->packed.avail_phase = 0;
	vq->packed.used_phase = 0;

	return 0;
}

static int
virtio_dev_split_get_avail_reqs(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq,
				uint16_t *reqs, uint16_t max_reqs)
{
	uint16_t count, i, avail_idx, last_idx;

	last_idx = vq->last_avail_idx;
	avail_idx = vq->avail.avail->idx;

	spdk_smp_rmb();

	count = avail_idx - last_idx;
	if (count == 0) {
		return 0;
	}

	count = spdk_min(count, max_reqs);
	vq->last_avail_idx += count;

	for (i = 0; i < count; i++) {
		reqs[i] = vq->avail.avail->ring[(last_idx + i) & (vq->qsize - 1)];
	}

	SPDK_DEBUGLOG(vfu_virtio_io,
		      "AVAIL: vq %u last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      vq->id, last_idx, avail_idx, count);

	return count;
}

static int
virtio_vring_split_desc_get_next(struct vring_desc **desc,
				 struct vring_desc *desc_table,
				 uint32_t desc_table_size)
{
	struct vring_desc *old_desc = *desc;
	uint16_t next_idx;

	if ((old_desc->flags & VRING_DESC_F_NEXT) == 0) {
		*desc = NULL;
		return 0;
	}

	next_idx = old_desc->next;
	if (spdk_unlikely(next_idx >= desc_table_size)) {
		*desc = NULL;
		return -1;
	}

	*desc = &desc_table[next_idx];
	return 0;
}

static inline void *
virtio_vring_desc_to_iov(struct vfu_virtio_dev *dev, struct vring_desc *desc,
			 dma_sg_t *sg, struct iovec *iov)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;

	return spdk_vfu_map_one(virtio_endpoint->endpoint, desc->addr, desc->len,
				sg, iov, PROT_READ | PROT_WRITE);
}

static int
virtio_split_vring_get_desc(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq, uint16_t desc_idx,
			    struct vring_desc **desc, struct vring_desc **desc_table,
			    uint32_t *desc_table_size,
			    dma_sg_t *sg, struct iovec *iov)
{
	*desc = &vq->desc.desc[desc_idx];

	if (virtio_vring_split_desc_is_indirect(*desc)) {
		*desc_table_size = (*desc)->len / sizeof(struct vring_desc);
		*desc_table = virtio_vring_desc_to_iov(dev, *desc, sg, iov);
		*desc = *desc_table;
		if (*desc == NULL) {
			return -EINVAL;
		}
		return 0;
	}

	*desc_table = vq->desc.desc;
	*desc_table_size = vq->qsize;

	return 0;
}

static inline dma_sg_t *
virtio_req_to_sg_t(struct vfu_virtio_req *req, uint32_t iovcnt)
{
	return (dma_sg_t *)(req->sg + iovcnt * dma_sg_size());
}

static inline struct vfu_virtio_req *
vfu_virtio_dev_get_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_req *req;

	req = STAILQ_FIRST(&vq->free_reqs);
	if (req == NULL) {
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&vq->free_reqs, link);

	req->iovcnt = 0;
	req->used_len = 0;
	req->payload_size = 0;
	req->req_idx = 0;
	req->buffer_id = 0;
	req->num_descs = 0;

	return req;
}

void
vfu_virtio_dev_put_req(struct vfu_virtio_req *req)
{
	struct vfu_virtio_dev *dev = req->dev;
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	vfu_ctx_t *vfu_ctx = spdk_vfu_get_vfu_ctx(virtio_endpoint->endpoint);

	if (req->indirect_iov->iov_base) {
		vfu_sgl_put(vfu_ctx, req->indirect_sg, req->indirect_iov, 1);
		req->indirect_iov->iov_base = NULL;
		req->indirect_iov->iov_len = 0;
	}

	if (req->iovcnt) {
		vfu_sgl_put(vfu_ctx, virtio_req_to_sg_t(req, 0), req->iovs, req->iovcnt);
		req->iovcnt = 0;
	}

	STAILQ_INSERT_HEAD(&req->vq->free_reqs, req, link);
}

void
vfu_virtio_finish_req(struct vfu_virtio_req *req)
{
	struct vfu_virtio_dev *dev = req->dev;
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;

	assert(virtio_endpoint->io_outstanding);
	virtio_endpoint->io_outstanding--;

	if (!virtio_guest_has_feature(req->dev, VIRTIO_F_RING_PACKED)) {
		virtio_vq_used_ring_split_enqueue(req->vq, req->req_idx, req->used_len);
	} else {
		virtio_vq_used_ring_packed_enqueue(req->vq, req->buffer_id, req->num_descs, req->used_len);
	}

	vfu_virtio_dev_put_req(req);
}

static inline void
vfu_virtio_dev_free_reqs(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_dev *dev)
{
	struct vfu_virtio_req *req;
	struct vfu_virtio_vq *vq;
	uint32_t i;

	for (i = 0; i < dev->num_queues; i++) {
		vq = &dev->vqs[i];
		while (!STAILQ_EMPTY(&vq->free_reqs)) {
			req = STAILQ_FIRST(&vq->free_reqs);
			STAILQ_REMOVE_HEAD(&vq->free_reqs, link);
			vfu_virtio_vq_free_req(virtio_endpoint, vq, req);
		}
	}
}

static int
virtio_dev_split_iovs_setup(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq,
			    uint16_t desc_idx, struct vfu_virtio_req *req)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size, len = 0;
	uint32_t desc_handled_cnt = 0;
	int rc;

	rc = virtio_split_vring_get_desc(dev, vq, desc_idx, &desc,
					 &desc_table, &desc_table_size,
					 req->indirect_sg, req->indirect_iov);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Invalid descriptor at index %"PRIu16".\n", desc_idx);
		return rc;
	}

	assert(req->iovcnt == 0);

	while (true) {
		if (spdk_unlikely(!virtio_vring_desc_to_iov(dev, desc, virtio_req_to_sg_t(req, req->iovcnt),
				  &req->iovs[req->iovcnt]))) {
			return -EINVAL;
		}
		req->desc_writeable[req->iovcnt] = false;
		if (virtio_vring_split_desc_is_wr(desc)) {
			req->desc_writeable[req->iovcnt] = true;
		}

		req->iovcnt++;
		len += desc->len;

		rc = virtio_vring_split_desc_get_next(&desc, desc_table, desc_table_size);
		if (spdk_unlikely(rc)) {
			return rc;
		} else if (desc == NULL) {
			break;
		}

		desc_handled_cnt++;
		if (spdk_unlikely(desc_handled_cnt > desc_table_size)) {
			return -EINVAL;
		}
	}

	req->payload_size = len;

	return 0;
}

void
virtio_vq_used_ring_split_enqueue(struct vfu_virtio_vq *vq, uint16_t req_idx, uint32_t used_len)
{
	uint16_t last_idx = vq->last_used_idx & (vq->qsize - 1);

	SPDK_DEBUGLOG(vfu_virtio_io,
		      "Queue %u - USED RING: last_idx=%"PRIu16" req_idx=%"PRIu16" used_len=%"PRIu32"\n",
		      vq->id, last_idx, req_idx, used_len);

	vq->used.used->ring[last_idx].id = req_idx;
	vq->used.used->ring[last_idx].len = used_len;
	vq->last_used_idx++;

	spdk_smp_wmb();

	*(volatile uint16_t *)&vq->used.used->idx = vq->last_used_idx;

	vq->used_req_cnt++;
}

void
virtio_vq_used_ring_packed_enqueue(struct vfu_virtio_vq *vq, uint16_t buffer_id, uint32_t num_descs,
				   uint32_t used_len)
{
	struct vring_packed_desc *desc = &vq->desc.desc_packed[vq->last_used_idx];

	SPDK_DEBUGLOG(vfu_virtio_io,
		      "Queue %u - USED RING: buffer_id=%"PRIu16" num_descs=%u used_len=%"PRIu32"\n",
		      vq->id, buffer_id, num_descs, used_len);

	if (spdk_unlikely(virtio_vring_packed_is_used(desc, vq->packed.used_phase))) {
		SPDK_ERRLOG("descriptor has been used before\n");
		return;
	}

	/* In used desc addr is unused and len specifies the buffer length
	 * that has been written to by the device.
	 */
	desc->addr = 0;
	desc->len = used_len;

	/* This bit specifies whether any data has been written by the device */
	if (used_len != 0) {
		desc->flags |= VRING_DESC_F_WRITE;
	}

	/* Buffer ID is included in the last descriptor in the list.
	 * The driver needs to keep track of the size of the list corresponding
	 * to each buffer ID.
	 */
	desc->id = buffer_id;

	/* A device MUST NOT make the descriptor used before buffer_id is
	 * written to the descriptor.
	 */
	spdk_smp_wmb();

	/* To mark a desc as used, the device sets the F_USED bit in flags to match
	 * the internal Device ring wrap counter. It also sets the F_AVAIL bit to
	 * match the same value.
	 */
	if (vq->packed.used_phase) {
		desc->flags |= (1 << VRING_PACKED_DESC_F_AVAIL);
		desc->flags |= (1 << VRING_PACKED_DESC_F_USED);
	} else {
		desc->flags &= ~(1 << VRING_PACKED_DESC_F_AVAIL);
		desc->flags &= ~(1 << VRING_PACKED_DESC_F_USED);
	}

	vq->last_used_idx += num_descs;
	if (vq->last_used_idx >= vq->qsize) {
		vq->last_used_idx -= vq->qsize;
		vq->packed.used_phase = !vq->packed.used_phase;
	}

	vq->used_req_cnt++;
}

static int
vfu_virtio_vq_post_irq(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	vfu_ctx_t *vfu_ctx = spdk_vfu_get_vfu_ctx(virtio_endpoint->endpoint);

	vq->used_req_cnt = 0;

	if (spdk_vfu_endpoint_msix_enabled(virtio_endpoint->endpoint)) {
		SPDK_DEBUGLOG(vfu_virtio_io, "%s: Queue %u post MSIX IV %u\n",
			      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
			      vq->id, vq->vector);
		return vfu_irq_trigger(vfu_ctx, vq->vector);
	} else {
		if (!spdk_vfu_endpoint_intx_enabled(virtio_endpoint->endpoint)) {
			SPDK_DEBUGLOG(vfu_virtio_io, "%s: IRQ disabled\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint));
			return 0;
		}

		SPDK_DEBUGLOG(vfu_virtio_io, "%s: Queue %u post ISR\n",
			      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), vq->id);
		dev->cfg.isr = 1;
		return vfu_irq_trigger(vfu_ctx, 0);
	}
}

void
vfu_virtio_vq_flush_irq(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	uint32_t delay_us;

	if (vq->used_req_cnt == 0) {
		return;
	}

	/* No need to notify client */
	if (virtio_queue_event_is_suppressed(dev, vq)) {
		return;
	}

	/* Interrupt coalescing disabled */
	if (!virtio_endpoint->coalescing_delay_us) {
		vfu_virtio_vq_post_irq(dev, vq);
		return;
	}

	/* No need for event right now */
	if (spdk_get_ticks() < vq->next_event_time) {
		return;
	}

	vfu_virtio_vq_post_irq(dev, vq);

	delay_us = virtio_endpoint->coalescing_delay_us;
	vq->next_event_time = spdk_get_ticks() + delay_us * spdk_get_ticks_hz() / (1000000ULL);
}

int
vfu_virito_dev_process_split_ring(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	struct vfu_virtio_req *req;
	uint16_t reqs_idx[VIRTIO_DEV_VRING_MAX_REQS];
	uint16_t reqs_cnt, i;
	int ret;

	reqs_cnt = virtio_dev_split_get_avail_reqs(dev, vq, reqs_idx, VIRTIO_DEV_VRING_MAX_REQS);
	if (!reqs_cnt) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_io, "%s: get %u descriptors\n", dev->name, reqs_cnt);

	for (i = 0; i < reqs_cnt; i++) {
		req = vfu_virtio_dev_get_req(virtio_endpoint, vq);
		if (spdk_unlikely(!req)) {
			SPDK_ERRLOG("Error to get request\n");
			/* TODO: address the error case */
			return -EIO;
		}

		req->req_idx = reqs_idx[i];
		ret = virtio_dev_split_iovs_setup(dev, vq, req->req_idx, req);
		if (spdk_unlikely(ret)) {
			/* let the device to response this error */
			SPDK_ERRLOG("Split vring setup failed with index %u\n", i);
		}

		assert(virtio_endpoint->virtio_ops.exec_request);
		virtio_endpoint->io_outstanding++;
		virtio_endpoint->virtio_ops.exec_request(virtio_endpoint, vq, req);
	}

	return i;
}

struct vfu_virtio_req *
virito_dev_split_ring_get_next_avail_req(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	struct vfu_virtio_req *req;
	uint16_t reqs_idx[VIRTIO_DEV_VRING_MAX_REQS];
	uint16_t reqs_cnt;
	int ret;

	reqs_cnt = virtio_dev_split_get_avail_reqs(dev, vq, reqs_idx, 1);
	if (!reqs_cnt) {
		return NULL;
	}
	assert(reqs_cnt == 1);

	SPDK_DEBUGLOG(vfu_virtio_io, "%s: get 1 descriptors\n", dev->name);

	req = vfu_virtio_dev_get_req(virtio_endpoint, vq);
	if (!req) {
		SPDK_ERRLOG("Error to get request\n");
		return NULL;
	}

	req->req_idx = reqs_idx[0];
	ret = virtio_dev_split_iovs_setup(dev, vq, req->req_idx, req);
	if (ret) {
		SPDK_ERRLOG("Split vring setup failed\n");
		vfu_virtio_dev_put_req(req);
		return NULL;
	}

	return req;
}

static inline void *
virtio_vring_packed_desc_to_iov(struct vfu_virtio_dev *dev, struct vring_packed_desc *desc,
				dma_sg_t *sg, struct iovec *iov)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;

	return spdk_vfu_map_one(virtio_endpoint->endpoint, desc->addr, desc->len,
				sg, iov, PROT_READ | PROT_WRITE);
}

static int
virtio_dev_packed_iovs_setup(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq,
			     uint16_t last_avail_idx,
			     struct vring_packed_desc *current_desc, struct vfu_virtio_req *req)
{
	struct vring_packed_desc *desc, *desc_table = NULL;
	uint16_t new_idx, num_descs, desc_table_size = 0;
	uint32_t len = 0;

	SPDK_DEBUGLOG(vfu_virtio_io, "%s: last avail idx %u, req %p\n", dev->name, last_avail_idx, req);

	desc = NULL;
	num_descs = 1;
	if (virtio_vring_packed_desc_is_indirect(current_desc)) {
		req->buffer_id = current_desc->id;
		desc_table = virtio_vring_packed_desc_to_iov(dev, current_desc, req->indirect_sg,
				req->indirect_iov);
		if (spdk_unlikely(desc_table == NULL)) {
			SPDK_ERRLOG("Map Indirect Desc to IOV failed\n");
			return -EINVAL;
		}
		desc_table_size = current_desc->len / sizeof(struct vring_packed_desc);
		desc = desc_table;
		SPDK_DEBUGLOG(vfu_virtio_io, "%s: indirect desc %p, desc size %u, req %p\n",
			      dev->name, desc_table, desc_table_size, req);
	} else {
		desc = current_desc;
	}

	assert(req->iovcnt == 0);
	/* Map descs to IOVs */
	new_idx = last_avail_idx;
	while (1) {
		assert(desc != NULL);
		if (spdk_unlikely(req->iovcnt == VIRTIO_DEV_MAX_IOVS)) {
			SPDK_ERRLOG("Max IOVs in request reached (iovcnt = %d).\n", req->iovcnt);
			return -EINVAL;
		}

		if (spdk_unlikely(!virtio_vring_packed_desc_to_iov(dev, desc, virtio_req_to_sg_t(req, req->iovcnt),
				  &req->iovs[req->iovcnt]))) {
			SPDK_ERRLOG("Map Desc to IOV failed (iovcnt = %d).\n", req->iovcnt);
			return -EINVAL;
		}
		req->desc_writeable[req->iovcnt] = false;
		if (virtio_vring_packed_desc_is_wr(desc)) {
			req->desc_writeable[req->iovcnt] = true;
		}

		req->iovcnt++;
		len += desc->len;

		/* get next desc */
		if (desc_table) {
			if (req->iovcnt < desc_table_size) {
				desc = &desc_table[req->iovcnt];
			} else {
				desc = NULL;
			}
		} else {
			if ((desc->flags & VRING_DESC_F_NEXT) == 0) {
				req->buffer_id = desc->id;
				desc = NULL;
			} else {
				new_idx = (new_idx + 1) % vq->qsize;
				desc = &vq->desc.desc_packed[new_idx];
				num_descs++;
				req->buffer_id = desc->id;
			}
		}

		if (desc == NULL) {
			break;
		}
	}

	req->num_descs = num_descs;
	vq->last_avail_idx = (new_idx + 1) % vq->qsize;
	if (vq->last_avail_idx < last_avail_idx) {
		vq->packed.avail_phase = !vq->packed.avail_phase;
	}

	req->payload_size = len;

	SPDK_DEBUGLOG(vfu_virtio_io, "%s: req %p, iovcnt %u, num_descs %u\n",
		      dev->name, req, req->iovcnt, num_descs);
	return 0;
}

int
vfu_virito_dev_process_packed_ring(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	struct vring_packed_desc *desc;
	int ret;
	struct vfu_virtio_req *req;
	uint16_t i, max_reqs;

	max_reqs = VIRTIO_DEV_VRING_MAX_REQS;
	for (i = 0; i < max_reqs; i++) {
		desc = &vq->desc.desc_packed[vq->last_avail_idx];
		if (!virtio_vring_packed_is_avail(desc, vq->packed.avail_phase)) {
			return i;
		}

		req = vfu_virtio_dev_get_req(virtio_endpoint, vq);
		if (spdk_unlikely(!req)) {
			SPDK_ERRLOG("Error to get request\n");
			/* TODO: address the error case */
			assert(false);
			return -EIO;
		}

		ret = virtio_dev_packed_iovs_setup(dev, vq, vq->last_avail_idx, desc, req);
		if (spdk_unlikely(ret)) {
			/* let the device to response the error */
			SPDK_ERRLOG("virtio_dev_packed_iovs_setup failed\n");
		}

		assert(virtio_endpoint->virtio_ops.exec_request);
		virtio_endpoint->io_outstanding++;
		virtio_endpoint->virtio_ops.exec_request(virtio_endpoint, vq, req);
	}

	return i;
}

struct vfu_virtio_req *
virito_dev_packed_ring_get_next_avail_req(struct vfu_virtio_dev *dev, struct vfu_virtio_vq *vq)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	struct vring_packed_desc *desc;
	int ret;
	struct vfu_virtio_req *req;

	desc = &vq->desc.desc_packed[vq->last_avail_idx];
	if (!virtio_vring_packed_is_avail(desc, vq->packed.avail_phase)) {
		return NULL;
	}

	SPDK_DEBUGLOG(vfu_virtio_io, "%s: get 1 descriptors\n", dev->name);

	req = vfu_virtio_dev_get_req(virtio_endpoint, vq);
	if (!req) {
		SPDK_ERRLOG("Error to get request\n");
		return NULL;
	}

	ret = virtio_dev_packed_iovs_setup(dev, vq, vq->last_avail_idx, desc, req);
	if (ret) {
		SPDK_ERRLOG("virtio_dev_packed_iovs_setup failed\n");
		vfu_virtio_dev_put_req(req);
		return NULL;
	}

	return req;
}

static int
virtio_vfu_pci_common_cfg(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
			  size_t count, loff_t pos, bool is_write)
{
	struct vfu_virtio_dev *dev = virtio_endpoint->dev;
	uint32_t offset, value = 0;
	int ret;

	assert(count <= 4);
	offset = pos - VIRTIO_PCI_COMMON_CFG_OFFSET;

	if (is_write) {
		memcpy(&value, buf, count);
		switch (offset) {
		case VIRTIO_PCI_COMMON_DFSELECT:
			dev->cfg.host_feature_select = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_DFSELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_GFSELECT:
			dev->cfg.guest_feature_select = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_GFSELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_GF:
			assert(dev->cfg.guest_feature_select <= 1);
			if (dev->cfg.guest_feature_select) {
				dev->cfg.guest_feat_hi = value;
				SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_GF_HI with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			} else {
				dev->cfg.guest_feat_lo = value;
				SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_GF_LO with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			}

			ret = virtio_dev_set_features(dev,
						      (((uint64_t)dev->cfg.guest_feat_hi << 32) | dev->cfg.guest_feat_lo));
			if (ret) {
				return ret;
			}
			break;
		case VIRTIO_PCI_COMMON_MSIX:
			dev->cfg.msix_config = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_MSIX with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_STATUS:
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_STATUS with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			ret = virtio_dev_set_status(dev, value);
			if (ret) {
				return ret;
			}
			break;
		case VIRTIO_PCI_COMMON_Q_SELECT:
			if (value < VIRTIO_DEV_MAX_VQS) {
				dev->cfg.queue_select = value;
			}
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_Q_SELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_SIZE:
			dev->vqs[dev->cfg.queue_select].qsize = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_Q_SIZE with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_MSIX:
			dev->vqs[dev->cfg.queue_select].vector = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_Q_MSIX with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_ENABLE:
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE PCI_COMMON_Q_ENABLE with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			if (value == 1) {
				ret = virtio_dev_enable_vq(dev, dev->cfg.queue_select);
				if (ret) {
					return ret;
				}
			} else {
				ret = virtio_dev_disable_vq(dev, dev->cfg.queue_select);
				if (ret) {
					return ret;
				}
			}
			break;
		case VIRTIO_PCI_COMMON_Q_DESCLO:
			dev->vqs[dev->cfg.queue_select].desc_lo = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_DESCLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_DESCHI:
			dev->vqs[dev->cfg.queue_select].desc_hi = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_DESCHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_AVAILLO:
			dev->vqs[dev->cfg.queue_select].avail_lo = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_AVAILLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_AVAILHI:
			dev->vqs[dev->cfg.queue_select].avail_hi = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_AVAILHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_USEDLO:
			dev->vqs[dev->cfg.queue_select].used_lo = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_USEDLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_USEDHI:
			dev->vqs[dev->cfg.queue_select].used_hi = value;
			SPDK_DEBUGLOG(vfu_virtio, "%s: WRITE queue %u PCI_COMMON_Q_USEDHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;

		default:
			SPDK_ERRLOG("%s: WRITE UNSUPPORTED offset 0x%x\n",
				    spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), offset);
			errno = EIO;
			return -1;
		}
	} else {
		switch (offset) {
		case VIRTIO_PCI_COMMON_DFSELECT:
			value = dev->cfg.host_feature_select;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_DFSELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_DF:
			assert(dev->cfg.host_feature_select <= 1);
			if (dev->cfg.host_feature_select) {
				value = dev->host_features >> 32;
				SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_DF_HI with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			} else {
				value = dev->host_features;
				SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_DF_LO with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			}
			break;
		case VIRTIO_PCI_COMMON_GFSELECT:
			value = dev->cfg.guest_feature_select;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_GFSELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_GF:
			assert(dev->cfg.guest_feature_select <= 1);
			if (dev->cfg.guest_feature_select) {
				value = dev->cfg.guest_feat_hi;
				SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_GF_HI with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			} else {
				value = dev->cfg.guest_feat_lo;
				SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_GF_LO with 0x%x\n",
					      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
					      value);
			}
			break;
		case VIRTIO_PCI_COMMON_MSIX:
			value = dev->cfg.msix_config;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_MSIX with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_NUMQ:
			value = dev->num_queues;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_NUMQ with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_STATUS:
			value = dev->cfg.device_status;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_STATUS with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_CFGGENERATION:
			value = dev->cfg.config_generation;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_CFGGENERATION with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_NOFF:
			value = dev->cfg.queue_select;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_Q_NOFF with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_SELECT:
			value = dev->cfg.queue_select;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ PCI_COMMON_Q_SELECT with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      value);
			break;
		case VIRTIO_PCI_COMMON_Q_SIZE:
			value = dev->vqs[dev->cfg.queue_select].qsize;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_SIZE with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_MSIX:
			value = dev->vqs[dev->cfg.queue_select].vector;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_MSIX with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
				      dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_ENABLE:
			value = dev->vqs[dev->cfg.queue_select].enabled;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_ENABLE with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_DESCLO:
			value = dev->vqs[dev->cfg.queue_select].desc_lo;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_DESCLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_DESCHI:
			value = dev->vqs[dev->cfg.queue_select].desc_hi;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_DESCHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_AVAILLO:
			value = dev->vqs[dev->cfg.queue_select].avail_lo;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_AVAILLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_AVAILHI:
			value = dev->vqs[dev->cfg.queue_select].avail_hi;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_AVAILHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_USEDLO:
			value = dev->vqs[dev->cfg.queue_select].used_lo;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_USEDLO with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		case VIRTIO_PCI_COMMON_Q_USEDHI:
			value = dev->vqs[dev->cfg.queue_select].used_hi;
			SPDK_DEBUGLOG(vfu_virtio, "%s: READ queue %u PCI_COMMON_Q_USEDHI with 0x%x\n",
				      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), dev->cfg.queue_select, value);
			break;
		default:
			SPDK_ERRLOG("%s: READ UNSUPPORTED offset 0x%x\n",
				    spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint), offset);
			errno = EIO;
			return -1;
		}
		memcpy(buf, &value, count);
	}

	return count;
}

static int
virtio_vfu_device_specific_cfg(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
			       size_t count, loff_t pos, bool is_write)
{
	loff_t offset;
	int ret = -1;

	assert(count <= 8);
	offset = pos - VIRTIO_PCI_SPECIFIC_CFG_OFFSET;
	if (!is_write) {
		if (virtio_endpoint->virtio_ops.get_config) {
			ret = virtio_endpoint->virtio_ops.get_config(virtio_endpoint, buf, offset, count);
		}
	} else {
		if (virtio_endpoint->virtio_ops.set_config) {
			ret = virtio_endpoint->virtio_ops.set_config(virtio_endpoint, buf, offset, count);
		}
	}

	if (ret < 0) {
		return ret;
	}

	return count;
}

static int
virtio_vfu_pci_isr(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
		   size_t count, bool is_write)
{
	uint8_t *isr;

	if (count != 1) {
		SPDK_ERRLOG("ISR register is 1 byte\n");
		errno = EIO;
		return -1;
	}

	isr = buf;

	if (!is_write) {
		SPDK_DEBUGLOG(vfu_virtio, "READ PCI ISR\n");
		/* Read-Acknowledge Clear */
		*isr = virtio_endpoint->dev->cfg.isr;
		virtio_endpoint->dev->cfg.isr = 0;
	} else {
		SPDK_ERRLOG("ISR register is RO\n");
		errno = EIO;
		return -1;
	}

	return count;
}

static ssize_t
virtio_vfu_access_bar4(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
		       loff_t pos,
		       bool is_write)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	uint64_t start, end;

	start = pos;
	end = start + count;
	SPDK_DEBUGLOG(vfu_virtio, "%s: %s bar4 0x%"PRIX64"-0x%"PRIX64", len = %lu\n",
		      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
		      is_write ? "write" : "read", start, end - 1, count);

	if (end < VIRTIO_PCI_COMMON_CFG_OFFSET + VIRTIO_PCI_COMMON_CFG_LENGTH) {
		/* virtio PCI common configuration */
		return virtio_vfu_pci_common_cfg(virtio_endpoint, buf, count, pos, is_write);
	} else if (start >= VIRTIO_PCI_ISR_ACCESS_OFFSET &&
		   end < VIRTIO_PCI_ISR_ACCESS_OFFSET + VIRTIO_PCI_ISR_ACCESS_LENGTH) {
		/* ISR access */
		return virtio_vfu_pci_isr(virtio_endpoint, buf, count, is_write);
	} else if (start >= VIRTIO_PCI_SPECIFIC_CFG_OFFSET &&
		   end < VIRTIO_PCI_SPECIFIC_CFG_OFFSET + VIRTIO_PCI_SPECIFIC_CFG_LENGTH) {
		/* Device specific configuration */
		return virtio_vfu_device_specific_cfg(virtio_endpoint, buf, count, pos, is_write);
	} else if (start >= VIRTIO_PCI_NOTIFICATIONS_OFFSET &&
		   end < VIRTIO_PCI_NOTIFICATIONS_OFFSET + VIRTIO_PCI_NOTIFICATIONS_LENGTH) {
		/* Notifications */
		/* Sparse mmap region by default, there are no MMIO R/W messages */
		assert(false);
		return count;
	} else {
		assert(false);
	}

	return 0;
}

int
vfu_virtio_post_memory_add(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct vfu_virtio_dev *dev = virtio_endpoint->dev;
	uint32_t i;

	if (!dev) {
		return 0;
	}

	for (i = 0; i < dev->num_queues; i++) {
		/* Try to remap VQs if necessary */
		virtio_dev_map_vq(dev, &dev->vqs[i]);
	}

	return 0;
}

int
vfu_virtio_pre_memory_remove(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);

	if (virtio_endpoint->dev != NULL) {
		vfu_virtio_dev_unmap_vqs(virtio_endpoint->dev, map_start, map_end);
	}

	return 0;
}

int
vfu_virtio_pci_reset_cb(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);

	if (virtio_endpoint->dev) {
		vfu_virtio_dev_stop(virtio_endpoint->dev);
		vfu_virtio_dev_reset(virtio_endpoint->dev);
	}

	return 0;
}

static ssize_t
access_pci_config(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset,
		  bool is_write)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	void *pci_config = spdk_vfu_endpoint_get_pci_config(endpoint);

	SPDK_DEBUGLOG(vfu_virtio,
		      "%s: PCI_CFG %s %#lx-%#lx\n",
		      spdk_vfu_get_endpoint_id(endpoint), is_write ? "write" : "read",
		      offset, offset + count);

	if (is_write) {
		SPDK_ERRLOG("write %#lx-%#lx not supported\n",
			    offset, offset + count);
		errno = EINVAL;
		return -1;
	}

	if (offset + count > 0x1000) {
		SPDK_ERRLOG("access past end of extended PCI configuration space, want=%ld+%ld, max=%d\n",
			    offset, count, 0x1000);
		errno = ERANGE;
		return -1;
	}

	memcpy(buf, ((unsigned char *)pci_config) + offset, count);
	return count;
}

static int
vfu_virtio_dev_start(struct vfu_virtio_dev *dev)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	int ret = 0;

	SPDK_DEBUGLOG(vfu_virtio, "start %s\n", dev->name);

	if (virtio_dev_is_started(dev)) {
		SPDK_ERRLOG("Device %s is already started\n", dev->name);
		return -EFAULT;
	}

	if (virtio_endpoint->virtio_ops.start_device) {
		virtio_endpoint->io_outstanding = 0;
		ret = virtio_endpoint->virtio_ops.start_device(virtio_endpoint);
	}

	SPDK_DEBUGLOG(vfu_virtio, "%s is started with ret %d\n", dev->name, ret);

	return ret;
}

static int
vfu_virtio_dev_stop(struct vfu_virtio_dev *dev)
{
	struct vfu_virtio_endpoint *virtio_endpoint = dev->virtio_endpoint;
	int ret = 0;

	SPDK_DEBUGLOG(vfu_virtio, "stop %s\n", dev->name);

	if (!virtio_dev_is_started(dev)) {
		SPDK_DEBUGLOG(vfu_virtio, "%s isn't started\n", dev->name);
		return 0;
	}

	if (virtio_endpoint->virtio_ops.stop_device) {
		ret = virtio_endpoint->virtio_ops.stop_device(virtio_endpoint);
		assert(ret == 0);
	}

	/* Unmap all VQs */
	vfu_virtio_dev_unmap_vqs(dev, NULL, NULL);

	return ret;
}

int
vfu_virtio_detach_device(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct vfu_virtio_dev *dev = virtio_endpoint->dev;

	if (virtio_endpoint->dev == NULL) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio, "detach device %s\n", dev->name);

	vfu_virtio_dev_stop(dev);
	vfu_virtio_dev_free_reqs(virtio_endpoint, dev);
	virtio_endpoint->dev = NULL;
	free(dev);

	return 0;
}

int
vfu_virtio_attach_device(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	uint64_t supported_features = 0;
	struct vfu_virtio_dev *dev;
	struct vfu_virtio_vq *vq;
	struct vfu_virtio_req *req;
	uint32_t i, j;
	int ret = 0;

	dev = calloc(1, sizeof(*dev) + virtio_endpoint->num_queues * 3 * dma_sg_size());
	if (dev == NULL) {
		return -ENOMEM;
	}

	dev->num_queues = virtio_endpoint->num_queues;
	for (i = 0; i < dev->num_queues; i++) {
		vq = &dev->vqs[i];
		vq->id = i;
		vq->qsize = virtio_endpoint->qsize;
		vq->avail.sg = (dma_sg_t *)(dev->sg + i * dma_sg_size() * 3);
		vq->used.sg = (dma_sg_t *)((uint8_t *)vq->avail.sg + dma_sg_size());
		vq->desc.sg = (dma_sg_t *)((uint8_t *)vq->used.sg + dma_sg_size());

		STAILQ_INIT(&vq->free_reqs);
		for (j = 0; j <= vq->qsize; j++) {
			req = vfu_virtio_vq_alloc_req(virtio_endpoint, vq);
			if (!req) {
				SPDK_ERRLOG("Error to allocate req\n");
				ret = -ENOMEM;
				goto out;
			}
			req->indirect_iov = &req->iovs[VIRTIO_DEV_MAX_IOVS];
			req->indirect_sg = virtio_req_to_sg_t(req, VIRTIO_DEV_MAX_IOVS);
			req->dev = dev;
			req->vq = vq;
			STAILQ_INSERT_TAIL(&vq->free_reqs, req, link);
		}
	}

	if (virtio_endpoint->virtio_ops.get_device_features) {
		supported_features = virtio_endpoint->virtio_ops.get_device_features(virtio_endpoint);
	}
	dev->host_features = supported_features;

	snprintf(dev->name, SPDK_VFU_MAX_NAME_LEN, "%s",
		 spdk_vfu_get_endpoint_name(virtio_endpoint->endpoint));
	virtio_endpoint->dev = dev;
	dev->virtio_endpoint = virtio_endpoint;
	virtio_endpoint->thread = spdk_get_thread();
	return 0;

out:
	vfu_virtio_dev_free_reqs(virtio_endpoint, dev);
	return ret;
}

int
vfu_virtio_endpoint_setup(struct vfu_virtio_endpoint *virtio_endpoint,
			  struct spdk_vfu_endpoint *endpoint,
			  char *basename, const char *endpoint_name,
			  struct vfu_virtio_ops *ops)
{
	char path[PATH_MAX] = "";
	int ret;

	if (!ops) {
		return -EINVAL;
	}

	ret = snprintf(path, PATH_MAX, "%s%s_bar4", basename, endpoint_name);
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get socket path: %s.\n", basename, spdk_strerror(errno));
		return -EINVAL;
	}

	ret = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ret == -1) {
		SPDK_ERRLOG("%s: failed to open device memory at %s.\n",
			    path, spdk_strerror(errno));
		return ret;
	}
	unlink(path);

	virtio_endpoint->devmem_fd = ret;
	ret = ftruncate(virtio_endpoint->devmem_fd, VIRTIO_PCI_BAR4_LENGTH);
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate file %s.\n", path,
			    spdk_strerror(errno));
		close(virtio_endpoint->devmem_fd);
		return ret;
	}

	virtio_endpoint->doorbells = mmap(NULL, VIRTIO_PCI_NOTIFICATIONS_LENGTH, PROT_READ | PROT_WRITE,
					  MAP_SHARED,
					  virtio_endpoint->devmem_fd, VIRTIO_PCI_NOTIFICATIONS_OFFSET);
	if (virtio_endpoint->doorbells == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s.\n", path, spdk_strerror(errno));
		close(virtio_endpoint->devmem_fd);
		return -EFAULT;
	}
	virtio_endpoint->endpoint = endpoint;
	virtio_endpoint->virtio_ops = *ops;
	virtio_endpoint->num_queues = VIRTIO_DEV_MAX_VQS;
	virtio_endpoint->qsize = VIRTIO_VQ_DEFAULT_SIZE;

	SPDK_DEBUGLOG(vfu_virtio, "mmap file %s, devmem_fd %d\n", path, virtio_endpoint->devmem_fd);
	return 0;
}

int
vfu_virtio_endpoint_destruct(struct vfu_virtio_endpoint *virtio_endpoint)
{
	if (virtio_endpoint->doorbells) {
		munmap((void *)virtio_endpoint->doorbells, VIRTIO_PCI_NOTIFICATIONS_LENGTH);
	}

	if (virtio_endpoint->devmem_fd) {
		close(virtio_endpoint->devmem_fd);
	}

	return 0;
}

static int
vfu_virtio_quiesce_poll(void *ctx)
{
	struct vfu_virtio_endpoint *virtio_endpoint = ctx;
	vfu_ctx_t *vfu_ctx = spdk_vfu_get_vfu_ctx(virtio_endpoint->endpoint);

	if (virtio_endpoint->io_outstanding) {
		return SPDK_POLLER_IDLE;
	}

	spdk_poller_unregister(&virtio_endpoint->quiesce_poller);
	virtio_endpoint->quiesce_in_progress = false;
	vfu_device_quiesced(vfu_ctx, 0);

	return SPDK_POLLER_BUSY;
}

int
vfu_virtio_quiesce_cb(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);

	if (virtio_endpoint->quiesce_in_progress) {
		return -EBUSY;
	}

	if (!virtio_endpoint->io_outstanding) {
		return 0;
	}

	virtio_endpoint->quiesce_in_progress = true;
	virtio_endpoint->quiesce_poller = SPDK_POLLER_REGISTER(vfu_virtio_quiesce_poll, virtio_endpoint,
					  10);

	return -EBUSY;
}

static struct spdk_vfu_pci_device vfu_virtio_device_info = {
	.id = {
		.vid = SPDK_PCI_VID_VIRTIO,
		/* Realize when calling get device information */
		.did = 0x0,
		.ssvid = SPDK_PCI_VID_VIRTIO,
		.ssid = 0x0,
	},

	.class = {
		/* 0x01, mass storage controller */
		.bcc = 0x01,
		/* 0x00, SCSI controller */
		.scc = 0x00,
		/* 0x00, SCSI controller - vendor specific interface */
		.pi = 0x00,
	},

	.pmcap = {
		.hdr.id = PCI_CAP_ID_PM,
		.pmcs.nsfrst = 0x1,
	},

	.pxcap = {
		.hdr.id = PCI_CAP_ID_EXP,
		.pxcaps.ver = 0x2,
		.pxdcap = {.rer = 0x1, .flrc = 0x1},
		.pxdcap2.ctds = 0x1,
	},

	.msixcap = {
		.hdr.id = PCI_CAP_ID_MSIX,
		.mxc.ts = VIRTIO_DEV_MAX_VQS - 1,
		.mtab = {.tbir = 0x1, .to = 0x0},
		.mpba = {.pbir = 0x2, .pbao = 0x0},
	},

	.nr_vendor_caps = 4,

	.intr_ipin = 0x1,
	.nr_int_irqs = 0x1,
	.nr_msix_irqs = VIRTIO_DEV_MAX_VQS,

	.regions = {
		/* BAR0 */
		{0},
		/* BAR1 */
		{
			.access_cb = NULL,
			.offset = 0,
			.fd = -1,
			.len = 0x1000,
			.flags = VFU_REGION_FLAG_RW,
			.nr_sparse_mmaps = 0,
		},
		/* BAR2 */
		{
			.access_cb = NULL,
			.offset = 0,
			.fd = -1,
			.len = 0x1000,
			.flags = VFU_REGION_FLAG_RW,
			.nr_sparse_mmaps = 0,
		},
		/* BAR3 */
		{0},
		/* BAR4 */
		{
			.access_cb = virtio_vfu_access_bar4,
			.offset = 0,
			.fd = -1,
			.len = VIRTIO_PCI_BAR4_LENGTH,
			.flags = VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
			.nr_sparse_mmaps = 1,
			.mmaps = {
				{
					.offset = VIRTIO_PCI_NOTIFICATIONS_OFFSET,
					.len = VIRTIO_PCI_NOTIFICATIONS_LENGTH,
				},
			},
		},
		/* BAR5 */
		{0},
		/* BAR6 */
		{0},
		/* ROM */
		{0},
		/* PCI Config */
		{
			.access_cb = access_pci_config,
			.offset = 0,
			.fd = -1,
			.len = 0x1000,
			.flags = VFU_REGION_FLAG_RW,
			.nr_sparse_mmaps = 0,
		},
	},
};

void
vfu_virtio_get_device_info(struct vfu_virtio_endpoint *virtio_endpoint,
			   struct spdk_vfu_pci_device *device_info)
{
	memcpy(device_info, &vfu_virtio_device_info, sizeof(*device_info));

	/* BAR4 Region FD */
	device_info->regions[VFU_PCI_DEV_BAR4_REGION_IDX].fd = virtio_endpoint->devmem_fd;
	SPDK_DEBUGLOG(vfu_virtio, "%s: get device information, fd %d\n",
		      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
		      virtio_endpoint->devmem_fd);
}

static struct virtio_pci_cap common_cap = {
	.cap_vndr = PCI_CAP_ID_VNDR,
	.cap_len = sizeof(common_cap),
	.cfg_type = VIRTIO_PCI_CAP_COMMON_CFG,
	.bar = 4,
	.offset = VIRTIO_PCI_COMMON_CFG_OFFSET,
	.length = VIRTIO_PCI_COMMON_CFG_LENGTH,
};

static struct virtio_pci_cap isr_cap = {
	.cap_vndr = PCI_CAP_ID_VNDR,
	.cap_len = sizeof(isr_cap),
	.cfg_type = VIRTIO_PCI_CAP_ISR_CFG,
	.bar = 4,
	.offset = VIRTIO_PCI_ISR_ACCESS_OFFSET,
	.length = VIRTIO_PCI_ISR_ACCESS_LENGTH,
};

static struct virtio_pci_cap dev_cap = {
	.cap_vndr = PCI_CAP_ID_VNDR,
	.cap_len = sizeof(dev_cap),
	.cfg_type = VIRTIO_PCI_CAP_DEVICE_CFG,
	.bar = 4,
	.offset = VIRTIO_PCI_SPECIFIC_CFG_OFFSET,
	.length = VIRTIO_PCI_SPECIFIC_CFG_LENGTH,
};

static struct virtio_pci_notify_cap notify_cap = {
	.cap = {
		.cap_vndr = PCI_CAP_ID_VNDR,
		.cap_len = sizeof(notify_cap),
		.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG,
		.bar = 4,
		.offset = VIRTIO_PCI_NOTIFICATIONS_OFFSET,
		.length = VIRTIO_PCI_NOTIFICATIONS_LENGTH,
	},
	.notify_off_multiplier = 4,
};

uint16_t
vfu_virtio_get_vendor_capability(struct spdk_vfu_endpoint *endpoint, char *buf,
				 uint16_t buf_len,
				 uint16_t idx)
{
	uint16_t len;

	SPDK_DEBUGLOG(vfu_virtio, "%s: get vendor capability, idx %u\n",
		      spdk_vfu_get_endpoint_id(endpoint), idx);

	switch (idx) {
	case 0:
		assert(buf_len > sizeof(common_cap));
		memcpy(buf, &common_cap, sizeof(common_cap));
		len = sizeof(common_cap);
		break;
	case 1:
		assert(buf_len > sizeof(isr_cap));
		memcpy(buf, &isr_cap, sizeof(isr_cap));
		len = sizeof(isr_cap);
		break;
	case 2:
		assert(buf_len > sizeof(dev_cap));
		memcpy(buf, &dev_cap, sizeof(dev_cap));
		len = sizeof(dev_cap);
		break;
	case 3:
		assert(buf_len > sizeof(notify_cap));
		memcpy(buf, &notify_cap, sizeof(notify_cap));
		len = sizeof(notify_cap);
		break;
	default:
		return 0;
	}

	return len;
}

SPDK_LOG_REGISTER_COMPONENT(vfu_virtio)
SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_io)
