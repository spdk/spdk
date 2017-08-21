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

#include "virtio_ethdev.h"
#include "virtio_pci.h"
#include "virtio_logs.h"
#include "virtqueue.h"

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_virtio_map[] = {
	{ RTE_PCI_DEVICE(VIRTIO_PCI_VENDORID, VIRTIO_PCI_DEVICEID_SCSI_MODERN) },
	{ .vendor_id = 0, /* sentinel */ },
};

static uint16_t
virtio_get_nr_vq(struct virtio_hw *hw)
{
	return hw->max_queues;
}

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
virtio_init_queue(struct virtio_hw *hw, uint16_t vtpci_queue_idx)
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
	vq_size = VTPCI_OPS(hw)->get_queue_num(hw, vtpci_queue_idx);
	PMD_INIT_LOG(DEBUG, "vq_size: %u", vq_size);
	if (vq_size == 0) {
		PMD_INIT_LOG(ERR, "virtqueue does not exist");
		return -EINVAL;
	}

	if (!rte_is_power_of_2(vq_size)) {
		PMD_INIT_LOG(ERR, "virtqueue size is not powerof 2");
		return -EINVAL;
	}

	snprintf(vq_name, sizeof(vq_name), "port%d_vq%d",
		 hw->port_id, vtpci_queue_idx);

	size = RTE_ALIGN_CEIL(sizeof(*vq) +
				vq_size * sizeof(struct vq_desc_extra),
				RTE_CACHE_LINE_SIZE);

	vq = rte_zmalloc_socket(vq_name, size, RTE_CACHE_LINE_SIZE,
				SOCKET_ID_ANY);
	if (vq == NULL) {
		PMD_INIT_LOG(ERR, "can not allocate vq");
		return -ENOMEM;
	}
	hw->vqs[vtpci_queue_idx] = vq;

	vq->hw = hw;
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

	/* For virtio_user case (that is when hw->dev is NULL), we use
	 * virtual address. And we need properly set _offset_, please see
	 * VIRTIO_MBUF_DATA_DMA_ADDR in virtqueue.h for more information.
	 */
	if (hw->virtio_user_dev) {
		vq->vq_ring_mem = (uintptr_t)mz->addr;
	}

	if (VTPCI_OPS(hw)->setup_queue(hw, vq) < 0) {
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
virtio_free_queues(struct virtio_hw *hw)
{
	uint16_t nr_vq = virtio_get_nr_vq(hw);
	struct virtqueue *vq;
	uint16_t i;

	if (hw->vqs == NULL)
		return;

	for (i = 0; i < nr_vq; i++) {
		vq = hw->vqs[i];
		if (!vq)
			continue;

		rte_memzone_free(vq->mz);

		rte_free(vq);
		hw->vqs[i] = NULL;
	}

	rte_free(hw->vqs);
	hw->vqs = NULL;
}

static int
virtio_alloc_queues(struct virtio_hw *hw)
{
	uint16_t nr_vq = virtio_get_nr_vq(hw);
	uint16_t i;
	int ret;

	hw->vqs = rte_zmalloc(NULL, sizeof(struct virtqueue *) * nr_vq, 0);
	if (!hw->vqs) {
		PMD_INIT_LOG(ERR, "failed to allocate vqs");
		return -ENOMEM;
	}

	for (i = 0; i < nr_vq; i++) {
		ret = virtio_init_queue(hw, i);
		if (ret < 0) {
			virtio_free_queues(hw);
			return ret;
		}
	}

	return 0;
}

static int
virtio_negotiate_features(struct virtio_hw *hw, uint64_t req_features)
{
	uint64_t host_features;

	/* Prepare guest_features: feature that driver wants to support */
	PMD_INIT_LOG(DEBUG, "guest_features before negotiate = %" PRIx64,
		req_features);

	/* Read device(host) feature bits */
	host_features = VTPCI_OPS(hw)->get_features(hw);
	PMD_INIT_LOG(DEBUG, "host_features before negotiate = %" PRIx64,
		host_features);

	/*
	 * Negotiate features: Subset of device feature bits are written back
	 * guest feature bits.
	 */
	hw->guest_features = req_features;
	hw->guest_features = vtpci_negotiate_features(hw, host_features);
	PMD_INIT_LOG(DEBUG, "features after negotiate = %" PRIx64,
		hw->guest_features);

	if (hw->modern) {
		if (!vtpci_with_feature(hw, VIRTIO_F_VERSION_1)) {
			PMD_INIT_LOG(ERR,
				"VIRTIO_F_VERSION_1 features is not enabled.");
			return -1;
		}
		vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_FEATURES_OK);
		if (!(vtpci_get_status(hw) & VIRTIO_CONFIG_STATUS_FEATURES_OK)) {
			PMD_INIT_LOG(ERR,
				"failed to set FEATURES_OK status!");
			return -1;
		}
	}

	hw->req_guest_features = req_features;

	return 0;
}

/* reset device and renegotiate features if needed */
static int
virtio_init_device(struct virtio_hw *hw, uint64_t req_features)
{
	struct rte_pci_device *pci_dev = NULL;
	int ret;

	/* Reset the device although not necessary at startup */
	vtpci_reset(hw);

	/* Tell the host we've noticed this device. */
	vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_ACK);

	/* Tell the host we've known how to drive the device. */
	vtpci_set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER);
	if (virtio_negotiate_features(hw, req_features) < 0)
		return -1;

	vtpci_read_dev_config(hw, offsetof(struct virtio_scsi_config, num_queues),
			      &hw->max_queues, sizeof(hw->max_queues));
	if (!hw->virtio_user_dev) {
		hw->max_queues = 3;
	}

	ret = virtio_alloc_queues(hw);
	if (ret < 0)
		return ret;

	vtpci_reinit_complete(hw);

	if (pci_dev)
		PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x",
			hw->port_id, pci_dev->id.vendor_id,
			pci_dev->id.device_id);

	return 0;
}

static void
virtio_set_vtpci_ops(struct virtio_hw *hw)
{
	VTPCI_OPS(hw) = &virtio_user_ops;
}

/*
 * This function is based on probe() function in virtio_pci.c
 * It returns 0 on success.
 */
int
eth_virtio_dev_init(struct virtio_hw *hw, int num_queues)
{
	int ret, i;

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		virtio_set_vtpci_ops(hw);
		return 0;
	}

	if (!hw->virtio_user_dev) {
		ret = vtpci_init(hw->pci_dev, hw);
		if (ret)
			return ret;
	}

	/* reset device and negotiate default features */
	ret = virtio_init_device(hw, VIRTIO_PMD_DEFAULT_GUEST_FEATURES);
	if (ret < 0)
		return ret;

	hw->nb_tx_queues = num_queues;

	for (i = 0; i < num_queues; i++) {
		virtio_dev_tx_queue_setup(hw, i, 512, -1);
	}

	return 0;
}

int
virtio_dev_start(struct virtio_hw *hw)
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

	hw->started = 1;

	return 0;
}

static struct virtio_hw *g_pci_hw = NULL;

struct virtio_hw *
get_pci_virtio_hw(void)
{
	printf("%s[%d] %p\n", __func__, __LINE__, g_pci_hw);
	return g_pci_hw;
}

static int virtio_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	struct rte_pci_device *pci_dev)
{
	struct virtio_hw *hw;

	hw = calloc(1, sizeof(*hw));
	hw->pci_dev = pci_dev;

	g_pci_hw = hw;

	printf("%s[%d]\n", __func__, __LINE__);
	return 0;
}

static int virtio_pci_remove(struct rte_pci_device *pci_dev)
{
	printf("%s[%d]\n", __func__, __LINE__);
	return 0;
}

static struct rte_pci_driver rte_virtio_pmd = {
	.driver = {
		.name = "net_virtio",
	},
	.id_table = pci_id_virtio_map,
	.drv_flags = 0,
	.probe = virtio_pci_probe,
	.remove = virtio_pci_remove,
};

RTE_INIT(rte_virtio_pmd_init);
static void
rte_virtio_pmd_init(void)
{
	if (rte_eal_iopl_init() != 0) {
		PMD_INIT_LOG(ERR, "IOPL call failed - cannot use virtio PMD");
		return;
	}

	rte_pci_register(&rte_virtio_pmd);
}

RTE_PMD_EXPORT_NAME(net_virtio, __COUNTER__);
RTE_PMD_REGISTER_PCI_TABLE(net_virtio, pci_id_virtio_map);
RTE_PMD_REGISTER_KMOD_DEP(net_virtio, "* igb_uio | uio_pci_generic | vfio");
