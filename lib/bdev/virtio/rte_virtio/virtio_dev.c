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

#include "virtio_dev.h"
#include "virtio_pci.h"
#include "virtio_logs.h"
#include "virtio_queue.h"
#include "virtio_user/virtio_user_dev.h"

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_virtio_map[] = {
	{ RTE_PCI_DEVICE(VIRTIO_PCI_VENDORID, VIRTIO_PCI_DEVICEID_SCSI_MODERN) },
	{ .vendor_id = 0, /* sentinel */ },
};

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

int
virtio_dev_init_queue(struct virtio_dev *vdev, uint16_t vtpci_queue_idx)
{
	char vq_name[VIRTQUEUE_MAX_NAME_SZ];
	const struct rte_memzone *mz = NULL;
	unsigned int vq_size, size;
	struct virtqueue *vq;
	int ret;

	PMD_INIT_LOG(DEBUG, "setting up queue: %u", vtpci_queue_idx);

	if (vtpci_queue_idx >= vdev->max_queues) {
		PMD_INIT_LOG(ERR, "virtuqueue id %u exceeds host virtqueue limit %u",
				vtpci_queue_idx, dev->max_queues);
		return -1;
	}

	if (vdev->vqs[vtpci_queue_idx]) {
		return 0;
	}

	/*
	 * Read the virtqueue size from the Queue Size field
	 * Always power of 2 and if 0 virtqueue does not exist
	 */
	vq_size = VTPCI_OPS(vdev)->get_queue_num(vdev, vtpci_queue_idx);
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
		 vdev->port_id, vtpci_queue_idx);

	size = RTE_ALIGN_CEIL(sizeof(*vq) +
				vq_size * sizeof(struct vq_desc_extra),
				RTE_CACHE_LINE_SIZE);

	vq = rte_zmalloc_socket(vq_name, size, RTE_CACHE_LINE_SIZE,
				SOCKET_ID_ANY);
	if (vq == NULL) {
		PMD_INIT_LOG(ERR, "can not allocate vq");
		return -ENOMEM;
	}
	vdev->vqs[vtpci_queue_idx] = vq;

	vq->vdev = vdev;
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

	if (VTPCI_OPS(vdev)->setup_queue(vdev, vq) < 0) {
		PMD_INIT_LOG(ERR, "setup_queue failed");
		return -EINVAL;
	}

	return 0;

fail_q_alloc:
	rte_memzone_free(mz);
	rte_free(vq);

	return ret;
}

void
virtio_dev_free_queue(struct virtio_dev *vdev, uint16_t queue_idx)
{
	struct virtqueue *vq = vdev->vqs[queue_idx];

	if (!vq)
		return;

	VTPCI_OPS(vdev)->del_queue(vdev, vq);

	rte_memzone_free(vq->mz);
	rte_free(vq);
	vdev->vqs[queue_idx] = NULL;
}

static int
virtio_alloc_queues(struct virtio_dev *vdev)
{
	vdev->vqs = rte_zmalloc(NULL, sizeof(struct virtqueue *) * vdev->max_queues, 0);
	if (!vdev->vqs) {
		PMD_INIT_LOG(ERR, "failed to allocate vqs");
		return -ENOMEM;
	}

	return 0;
}

/* Negotiate virtio features. This will also set dev->modern if virtio
 * device offers VIRTIO_F_VERSION_1 flag. If dev->modern has been set before,
 * the mentioned flag must be offered. Otherwise an error is returned.
 */
static int
virtio_negotiate_features(struct virtio_dev *vdev, uint64_t req_features)
{
	uint64_t host_features;

	/* Prepare guest_features: feature that driver wants to support */
	PMD_INIT_LOG(DEBUG, "guest_features before negotiate = %" PRIx64,
		req_features);

	/* Read device(host) feature bits */
	host_features = VTPCI_OPS(vdev)->get_features(vdev);
	PMD_INIT_LOG(DEBUG, "host_features before negotiate = %" PRIx64,
		host_features);

	/*
	 * Negotiate features: Subset of device feature bits are written back
	 * guest feature bits.
	 */
	vdev->req_guest_features = req_features;
	vdev->guest_features = vtpci_negotiate_features(vdev, host_features);
	PMD_INIT_LOG(DEBUG, "features after negotiate = %" PRIx64,
		dev->guest_features);

	if (!vtpci_with_feature(vdev, VIRTIO_F_VERSION_1)) {
		if (vdev->modern) {
			PMD_INIT_LOG(ERR,
				     "VIRTIO_F_VERSION_1 features is not enabled.");
			return -1;
		}

		return 0;
	}

	vdev->modern = 1;
	vtpci_set_status(vdev, VIRTIO_CONFIG_STATUS_FEATURES_OK);
	if (!(vtpci_get_status(vdev) & VIRTIO_CONFIG_STATUS_FEATURES_OK)) {
		PMD_INIT_LOG(ERR,
			     "failed to set FEATURES_OK status!");
		return -1;
	}

	return 0;
}

/* reset device and renegotiate features if needed */
int
virtio_dev_init(struct virtio_dev *vdev, uint64_t req_features)
{
	int ret;

	/* Reset the device although not necessary at startup */
	vtpci_reset(vdev);

	/* Tell the host we've noticed this device. */
	vtpci_set_status(vdev, VIRTIO_CONFIG_STATUS_ACK);

	/* Tell the host we've known how to drive the device. */
	vtpci_set_status(vdev, VIRTIO_CONFIG_STATUS_DRIVER);
	if (virtio_negotiate_features(vdev, req_features) < 0)
		return -1;

	ret = virtio_alloc_queues(vdev);
	if (ret < 0)
		return ret;

	vtpci_reinit_complete(vdev);
	return 0;
}

void
virtio_dev_deinit(struct virtio_dev *vdev)
{
	uint16_t i;

	for (i = 0; i < vdev->max_queues; ++i) {
		virtio_dev_free_queue(vdev, i);
	}

	virtio_hw_internal[vdev->port_id].vtpci_ops = NULL;
	free(vdev);
}

int
virtio_dev_start(struct virtio_dev *vdev)
{
	return VTPCI_OPS(vdev)->set_enabled(vdev, 1);
}

static struct virtio_hw *g_pci_hw = NULL;

struct virtio_dev *
get_pci_virtio_hw(void)
{
	int ret;
	struct virtio_dev *vdev = &g_pci_hw->vdev;

	printf("%s[%d] %p\n", __func__, __LINE__, g_pci_hw);
	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		PMD_DRV_LOG(ERR, "rte secondary process support is not implemented yet");
		return NULL;
	}

	ret = vtpci_init(g_pci_hw->pci_dev, vdev);
	if (ret)
		return NULL;

	vtpci_read_dev_config(vdev, offsetof(struct virtio_scsi_config, num_queues),
			      &vdev->max_queues, sizeof(vdev->max_queues));

	return vdev;
}

static int virtio_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	struct rte_pci_device *pci_dev)
{
	struct virtio_hw *hw;

	hw = calloc(1, sizeof(*hw));
	hw->vdev.is_hw = 1;
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
