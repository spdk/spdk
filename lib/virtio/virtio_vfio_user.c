/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/memory.h"
#include "spdk/vfio_user_pci.h"

#include "spdk_internal/virtio.h"

#include <linux/vfio.h>

struct virtio_vfio_user_dev {
	struct vfio_device	*ctx;
	char			path[PATH_MAX];

	uint32_t		pci_cap_region;
	uint32_t		pci_cap_common_cfg_offset;
	uint32_t		pci_cap_common_cfg_length;
	uint32_t		pci_cap_device_specific_offset;
	uint32_t		pci_cap_device_specific_length;
	uint32_t		pci_cap_notifications_offset;
	uint32_t		pci_cap_notifications_length;
};

static int
virtio_vfio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
				 void *dst, int length)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;

	SPDK_DEBUGLOG(virtio_vfio_user, "offset 0x%lx, length 0x%x\n", offset, length);
	return spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					     dev->pci_cap_device_specific_offset + offset,
					     length, dst, false);
}

static int
virtio_vfio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
				  const void *src, int length)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;

	SPDK_DEBUGLOG(virtio_vfio_user, "offset 0x%lx, length 0x%x\n", offset, length);
	return spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					     dev->pci_cap_device_specific_offset + offset,
					     length, (void *)src, true);
}

static uint8_t
virtio_vfio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint8_t status = 0;
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_STATUS;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 1, &status, false);
	if (rc) {
		SPDK_ERRLOG("Failed to get device status\n");
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "device status %x\n", status);

	return status;
}

static void
virtio_vfio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	int rc;

	SPDK_DEBUGLOG(virtio_vfio_user, "device status %x\n", status);

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_STATUS;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 1, &status, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set device status\n");
	}
}

static uint64_t
virtio_vfio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint32_t features_lo, features_hi, feature_select;
	int rc;

	feature_select = 0;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set device feature select\n");
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DF;
	features_lo = 0;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_lo, false);
	if (rc) {
		SPDK_ERRLOG("Failed to get device feature low\n");
	}

	feature_select = 1;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set device feature select\n");
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_DF;
	features_hi = 0;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_hi, false);
	if (rc) {
		SPDK_ERRLOG("Failed to get device feature high\n");
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "feature_hi 0x%x, feature_low 0x%x\n", features_hi, features_lo);

	return (((uint64_t)features_hi << 32) | ((uint64_t)features_lo));
}

static int
virtio_vfio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint32_t features_lo, features_hi, feature_select;
	int rc;

	feature_select = 0;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature select\n");
		return rc;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GF;
	features_lo = (uint32_t)features;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_lo, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature low\n");
		return rc;
	}

	feature_select = 1;
	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GFSELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &feature_select, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature select\n");
		return rc;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_GF;
	features_hi = (uint32_t)(features >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &features_hi, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set Guest feature high\n");
	}

	vdev->negotiated_features = features;
	SPDK_DEBUGLOG(virtio_vfio_user, "features 0x%"PRIx64"\n", features);

	return rc;
}

static void
virtio_vfio_user_destruct_dev(struct virtio_dev *vdev)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;

	if (dev) {
		spdk_vfio_user_release(dev->ctx);
		free(dev);
	}
}

static uint16_t
virtio_vfio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint16_t qsize = 0;
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_id, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set queue select\n");
		return 0;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SIZE;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &qsize, false);
	if (rc) {
		SPDK_ERRLOG("Failed to get queue size\n");
		return 0;
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "queue %u, size %u\n", queue_id, qsize);

	return qsize;
}

static int
virtio_vfio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t desc_addr, avail_addr, used_addr, offset;
	uint32_t addr_lo, addr_hi;
	uint16_t notify_off, queue_enable;
	void *queue_mem;
	uint64_t queue_mem_phys_addr;
	int rc;

	/* To ensure physical address contiguity we make the queue occupy
	 * only a single hugepage (2MB). As of Virtio 1.0, the queue size
	 * always falls within this limit.
	 */
	if (vq->vq_ring_size > VALUE_2MB) {
		return -ENOMEM;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VALUE_2MB, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (queue_mem == NULL) {
		return -ENOMEM;
	}

	queue_mem_phys_addr = spdk_vtophys(queue_mem, NULL);
	if (queue_mem_phys_addr == SPDK_VTOPHYS_ERROR) {
		spdk_free(queue_mem);
		return -EFAULT;
	}

	vq->vq_ring_mem = queue_mem_phys_addr;
	vq->vq_ring_virt_mem = queue_mem;

	desc_addr = vq->vq_ring_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = (avail_addr + offsetof(struct vring_avail, ring[vq->vq_nentries])
		     + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &vq->vq_queue_index, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set queue select\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_DESCLO;
	addr_lo = (uint32_t)desc_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set desc addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_DESCHI;
	addr_hi = (uint32_t)(desc_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set desc addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_AVAILLO;
	addr_lo = (uint32_t)avail_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set avail addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_AVAILHI;
	addr_hi = (uint32_t)(avail_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set avail addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_USEDLO;
	addr_lo = (uint32_t)used_addr;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_lo, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set used addr low\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_USEDHI;
	addr_hi = (uint32_t)(used_addr >> 32);
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 4, &addr_hi, true);
	if (rc) {
		SPDK_ERRLOG("Failed to set used addr high\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_NOFF;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &notify_off, false);
	if (rc) {
		SPDK_ERRLOG("Failed to get queue notify off\n");
		goto err;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_ENABLE;
	queue_enable = 1;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_enable, true);
	if (rc) {
		SPDK_ERRLOG("Failed to enable queue %u\n", vq->vq_queue_index);
		goto err;
	}

	SPDK_DEBUGLOG(virtio_vfio_user, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(virtio_vfio_user, "\t used_addr: %" PRIx64 "\n", used_addr);

	return 0;
err:
	spdk_free(queue_mem);
	return rc;
}

static void
virtio_vfio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_vfio_user_dev *dev = vdev->ctx;
	uint64_t offset;
	uint16_t queue_enable = 0;
	int rc;

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_SELECT;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &vq->vq_queue_index, true);
	if (rc) {
		SPDK_ERRLOG("Failed to select queue %u\n", vq->vq_queue_index);
		spdk_free(vq->vq_ring_virt_mem);
		return;
	}

	offset = dev->pci_cap_common_cfg_offset + VIRTIO_PCI_COMMON_Q_ENABLE;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, dev->pci_cap_region,
					   offset, 2, &queue_enable, true);
	if (rc) {
		SPDK_ERRLOG("Failed to enable queue %u\n", vq->vq_queue_index);
	}

	spdk_free(vq->vq_ring_virt_mem);
	/* TODO: clear desc/avail/used address */
}

static void
virtio_vfio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* we're running in polling mode, no need to write doorbells */
}

static const struct virtio_dev_ops virtio_vfio_user_ops = {
	.read_dev_cfg	= virtio_vfio_user_read_dev_config,
	.write_dev_cfg	= virtio_vfio_user_write_dev_config,
	.get_status	= virtio_vfio_user_get_status,
	.set_status	= virtio_vfio_user_set_status,
	.get_features	= virtio_vfio_user_get_features,
	.set_features	= virtio_vfio_user_set_features,
	.destruct_dev	= virtio_vfio_user_destruct_dev,
	.get_queue_size	= virtio_vfio_user_get_queue_size,
	.setup_queue	= virtio_vfio_user_setup_queue,
	.del_queue	= virtio_vfio_user_del_queue,
	.notify_queue	= virtio_vfio_user_notify_queue
};

int
virtio_vfio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path)
{
	struct virtio_vfio_user_dev *dev;
	uint16_t cmd_reg;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name gived for controller: %s\n", path);
		return -EINVAL;
	}

	rc = access(path, F_OK);
	if (rc != 0) {
		SPDK_ERRLOG("Access path %s failed\n", path);
		return -EACCES;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_vfio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	snprintf(dev->path, PATH_MAX, "%s", path);
	dev->ctx = spdk_vfio_user_setup(path);
	if (!dev->ctx) {
		SPDK_ERRLOG("Error to setup %s as vfio device\n", path);
		virtio_dev_destruct(vdev);
		return -EINVAL;
	}

	/* Enable PCI busmaster and disable INTx */
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					   &cmd_reg, false);
	if (rc != 0) {
		SPDK_ERRLOG("Read PCI CMD REG failed\n");
		virtio_dev_destruct(vdev);
		return rc;
	}
	cmd_reg |= 0x404;
	rc = spdk_vfio_user_pci_bar_access(dev->ctx, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					   &cmd_reg, true);
	if (rc != 0) {
		SPDK_ERRLOG("Write PCI CMD REG failed\n");
		virtio_dev_destruct(vdev);
		return rc;
	}

	/* TODO: we cat get virtio device PCI common space layout via
	 * iterating vendor capabilities in PCI Configuration space,
	 * while here we use hardcoded layout first, this feature can
	 * be added in future.
	 *
	 * vfio-user emulated virtio device layout in Target:
	 *
	 * region 1: MSI-X Table
	 * region 2: MSI-X PBA
	 * region 4: virtio modern memory 64bits BAR
	 *     Common configuration          0x0    - 0x1000
	 *     ISR access                    0x1000 - 0x2000
	 *     Device specific configuration 0x2000 - 0x3000
	 *     Notifications                 0x3000 - 0x4000
	 */
	dev->pci_cap_region = VFIO_PCI_BAR4_REGION_INDEX;
	dev->pci_cap_common_cfg_offset = 0x0;
	dev->pci_cap_common_cfg_length = 0x1000;
	dev->pci_cap_device_specific_offset = 0x2000;
	dev->pci_cap_device_specific_length = 0x1000;
	dev->pci_cap_notifications_offset = 0x3000;
	dev->pci_cap_notifications_length = 0x1000;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(virtio_vfio_user)
