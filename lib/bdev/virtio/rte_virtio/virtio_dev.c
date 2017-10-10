/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <linux/virtio_scsi.h>

#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_errno.h>

#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_dev.h>

#include "virtio_user/vhost.h"
#include "virtio_dev.h"
#include "virtio_pci.h"
#include "virtio_logs.h"
#include "virtio_queue.h"

static void
virtio_init_vring(struct virtqueue *vq)
{
	int size = vq->vq_nentries;
	struct vring *vr = &vq->vq_ring;
	uint8_t *ring_mem = vq->vq_ring_virt_mem;

	PMD_INIT_FUNC_TRACE();

	/*
	 * Reinitialise since virtio port might have been stopped and restarted
	 */
	memset(ring_mem, 0, vq->vq_ring_size);
	vring_init(vr, size, ring_mem, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_used_cons_idx = 0;
	vq->vq_desc_head_idx = 0;
	vq->vq_avail_idx = 0;
	vq->vq_desc_tail_idx = (uint16_t)(vq->vq_nentries - 1);
	vq->vq_free_cnt = vq->vq_nentries;
	memset(vq->vq_descx, 0, sizeof(struct vq_desc_extra) * vq->vq_nentries);

	vring_desc_init(vr->desc, size);

	/*
	 * Disable device(host) interrupting guest
	 */
	virtqueue_disable_intr(vq);
}

static int
virtio_init_queue(struct virtio_dev *dev, uint16_t vtpci_queue_idx)
{
	char vq_name[VIRTQUEUE_MAX_NAME_SZ];
	const struct rte_memzone *mz = NULL;
	unsigned int vq_size, size;
	struct virtqueue *vq;
	int ret;

	PMD_INIT_LOG(DEBUG, "setting up queue: %u", vtpci_queue_idx);

	/*
	 * Read the virtqueue size from the Queue Size field
	 * Always power of 2 and if 0 virtqueue does not exist
	 */
	vq_size = vtpci_ops(dev)->get_queue_num(dev, vtpci_queue_idx);
	PMD_INIT_LOG(DEBUG, "vq_size: %u", vq_size);
	if (vq_size == 0) {
		PMD_INIT_LOG(ERR, "virtqueue does not exist");
		return -EINVAL;
	}

	if (!rte_is_power_of_2(vq_size)) {
		PMD_INIT_LOG(ERR, "virtqueue size is not powerof 2");
		return -EINVAL;
	}

	snprintf(vq_name, sizeof(vq_name), "dev%d_vq%d",
		 dev->id, vtpci_queue_idx);

	size = RTE_ALIGN_CEIL(sizeof(*vq) +
				vq_size * sizeof(struct vq_desc_extra),
				RTE_CACHE_LINE_SIZE);

	vq = rte_zmalloc_socket(vq_name, size, RTE_CACHE_LINE_SIZE,
				SOCKET_ID_ANY);
	if (vq == NULL) {
		PMD_INIT_LOG(ERR, "can not allocate vq");
		return -ENOMEM;
	}
	dev->vqs[vtpci_queue_idx] = vq;

	vq->vdev = dev;
	vq->vq_queue_index = vtpci_queue_idx;
	vq->vq_nentries = vq_size;

	/*
	 * Reserve a memzone for vring elements
	 */
	size = vring_size(vq_size, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_ring_size = RTE_ALIGN_CEIL(size, VIRTIO_PCI_VRING_ALIGN);
	PMD_INIT_LOG(DEBUG, "vring_size: %d, rounded_vring_size: %d",
		     size, vq->vq_ring_size);

	mz = rte_memzone_reserve_aligned(vq_name, vq->vq_ring_size,
					 SOCKET_ID_ANY,
					 0, VIRTIO_PCI_VRING_ALIGN);
	if (mz == NULL) {
		if (rte_errno == EEXIST)
			mz = rte_memzone_lookup(vq_name);
		if (mz == NULL) {
			ret = -ENOMEM;
			goto fail_q_alloc;
		}
	}

	memset(mz->addr, 0, sizeof(mz->len));

	vq->vq_ring_mem = mz->phys_addr;
	vq->vq_ring_virt_mem = mz->addr;
	PMD_INIT_LOG(DEBUG, "vq->vq_ring_mem:      0x%" PRIx64,
		     (uint64_t)mz->phys_addr);
	PMD_INIT_LOG(DEBUG, "vq->vq_ring_virt_mem: 0x%" PRIx64,
		     (uint64_t)(uintptr_t)mz->addr);

	virtio_init_vring(vq);

	vq->mz = mz;

	if (vtpci_ops(dev)->setup_queue(dev, vq) < 0) {
		PMD_INIT_LOG(ERR, "setup_queue failed");
		return -EINVAL;
	}

	return 0;

fail_q_alloc:
	rte_memzone_free(mz);
	rte_free(vq);

	return ret;
}

static void
virtio_free_queues(struct virtio_dev *dev)
{
	uint16_t nr_vq = dev->max_queues;
	struct virtqueue *vq;
	uint16_t i;

	if (dev->vqs == NULL)
		return;

	for (i = 0; i < nr_vq; i++) {
		vq = dev->vqs[i];
		if (!vq)
			continue;

		rte_memzone_free(vq->mz);

		rte_free(vq);
		dev->vqs[i] = NULL;
	}

	rte_free(dev->vqs);
	dev->vqs = NULL;
}

static int
virtio_alloc_queues(struct virtio_dev *dev)
{
	uint16_t nr_vq = dev->max_queues;
	uint16_t i;
	int ret;

	dev->vqs = rte_zmalloc(NULL, sizeof(struct virtqueue *) * nr_vq, 0);
	if (!dev->vqs) {
		PMD_INIT_LOG(ERR, "failed to allocate vqs");
		return -ENOMEM;
	}

	for (i = 0; i < nr_vq; i++) {
		ret = virtio_init_queue(dev, i);
		if (ret < 0) {
			virtio_free_queues(dev);
			return ret;
		}
	}

	return 0;
}

/**
 * Negotiate virtio features. For virtio_user this will also set
 * dev->modern flag if VIRTIO_F_VERSION_1 flag is negotiated.
 */
static int
virtio_negotiate_features(struct virtio_dev *dev, uint64_t req_features)
{
	uint64_t host_features = vtpci_ops(dev)->get_features(dev);
	int rc;

	PMD_INIT_LOG(DEBUG, "guest features = %" PRIx64, req_features);
	PMD_INIT_LOG(DEBUG, "device features = %" PRIx64, host_features);

	rc = vtpci_ops(dev)->set_features(dev, req_features & host_features);
	if (rc != 0) {
		PMD_INIT_LOG(ERR, "failed to negotiate device features.");
		return -1;
	}

	PMD_INIT_LOG(DEBUG, "negotiated features = %" PRIx64, dev->negotiated_features);

	vtpci_set_status(dev, VIRTIO_CONFIG_S_FEATURES_OK);
	if (!(vtpci_get_status(dev) & VIRTIO_CONFIG_S_FEATURES_OK)) {
		PMD_INIT_LOG(ERR,
			     "failed to set FEATURES_OK status!");
		return -1;
	}

	return 0;
}

/* reset device and renegotiate features if needed */
int
virtio_dev_init(struct virtio_dev *dev, uint64_t req_features)
{
	int ret;

	/* Reset the device although not necessary at startup */
	vtpci_reset(dev);

	/* Tell the host we've noticed this device. */
	vtpci_set_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);

	/* Tell the host we've known how to drive the device. */
	vtpci_set_status(dev, VIRTIO_CONFIG_S_DRIVER);
	if (virtio_negotiate_features(dev, req_features) < 0)
		return -1;

	/* FIXME
	 * Hardcode num_queues to 3 until we add proper
	 * mutli-queue support. This value should be limited
	 * by number of cores assigned to SPDK
	 */
	dev->max_queues = 3;

	ret = virtio_alloc_queues(dev);
	if (ret < 0)
		return ret;

	vtpci_reinit_complete(dev);
	return 0;
}

void
virtio_dev_free(struct virtio_dev *dev)
{
	uint32_t vdev_id = dev->id;

	vtpci_reset(dev);
	virtio_free_queues(dev);
	vtpci_ops(dev)->free_vdev(dev);
	vtpci_deinit(vdev_id);
}

int
virtio_dev_start(struct virtio_dev *vdev)
{
	struct virtnet_tx *txvq __rte_unused;

	/* Enable uio/vfio intr/eventfd mapping: althrough we already did that
	 * in device configure, but it could be unmapped  when device is
	 * stopped.
	 */
	/** TODO: interrupt handling for virtio_scsi */
#if 0
	if (dev->data->dev_conf.intr_conf.lsc ||
	    dev->data->dev_conf.intr_conf.rxq) {
		rte_intr_disable(dev->intr_handle);

		if (rte_intr_enable(dev->intr_handle) < 0) {
			PMD_DRV_LOG(ERR, "interrupt enable failed");
			return -EIO;
		}
	}
#endif

	PMD_INIT_LOG(DEBUG, "Notified backend at initialization");

	vdev->started = 1;

	return 0;
}
