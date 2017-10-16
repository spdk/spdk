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
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#include <linux/virtio_scsi.h>

#include <rte_malloc.h>
#include <rte_vdev.h>
#include <rte_alarm.h>

#include "virtio_dev.h"
#include "virtio_pci.h"
#include "virtio_user/virtio_user_dev.h"

#include "spdk/string.h"

#define virtio_dev_get_user_dev(dev) \
	((struct virtio_user_dev *)((uintptr_t)(dev) - offsetof(struct virtio_user_dev, vdev)))

static void
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
		     void *dst, int length)
{
	SPDK_ERRLOG("not supported offset=%zu, len=%d\n", offset, length);
}

static void
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
		      const void *src, int length)
{
	SPDK_ERRLOG("not supported offset=%zu, len=%d\n", offset, length);
}

static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);

	if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		virtio_user_start_device(dev);
	} else if (status == VIRTIO_CONFIG_S_RESET &&
			(dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		virtio_user_stop_device(dev);
	}
	dev->status = status;
}

static uint8_t
virtio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);

	return dev->status;
}

static uint64_t
virtio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);
	char err_str[64];
	uint64_t features;

	if (dev->ops->send_request(dev, VHOST_USER_GET_FEATURES, &features) < 0) {
		spdk_strerror_r(errno, err_str, sizeof(err_str));
		SPDK_ERRLOG("get_features failed: %s\n", err_str);
		return 0;
	}

	return features;
}

static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);
	int ret;

	ret = dev->ops->send_request(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0) {
		return -1;
	}

	vdev->negotiated_features = features;
	vdev->modern = vtpci_with_feature(vdev, VIRTIO_F_VERSION_1);

	return 0;
}

static uint8_t
virtio_user_get_isr(struct virtio_dev *vdev)
{
	/* rxq interrupts and config interrupt are separated in virtio-user,
	 * here we only report config change.
	 */
	return VIRTIO_PCI_ISR_CONFIG;
}

static uint16_t
virtio_user_set_config_irq(struct virtio_dev *vdev,
			   uint16_t vec)
{
	return 0;
}

static uint16_t
virtio_user_set_queue_irq(struct virtio_dev *vdev,
			  struct virtqueue *vq,
			  uint16_t vec)
{
	/* pretend we have done that */
	return vec;
}

/* This function is to get the queue size, aka, number of descs, of a specified
 * queue. Different with the VHOST_USER_GET_QUEUE_NUM, which is used to get the
 * max supported queues.
 */
static uint16_t
virtio_user_get_queue_num(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);

	/* Currently, each queue has same queue size */
	return dev->queue_size;
}

static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);
	uint16_t queue_idx = vq->vq_queue_index;
	uint64_t desc_addr, avail_addr, used_addr;
	char err_str[64];
	int callfd;
	int kickfd;

	if (dev->callfds[queue_idx] != -1 || dev->kickfds[queue_idx] != -1) {
		SPDK_ERRLOG("queue %"PRIu16" already exists\n", queue_idx);
		return -1;
	}

	/* May use invalid flag, but some backend uses kickfd and
	 * callfd as criteria to judge if dev is alive. so finally we
	 * use real event_fd.
	 */
	callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (callfd < 0) {
		spdk_strerror_r(errno, err_str, sizeof(err_str));
		SPDK_ERRLOG("callfd error, %s\n", err_str);
		return -1;
	}

	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		spdk_strerror_r(errno, err_str, sizeof(err_str));
		SPDK_ERRLOG("kickfd error, %s\n", err_str);
		close(callfd);
		return -1;
	}

	dev->callfds[queue_idx] = callfd;
	dev->kickfds[queue_idx] = kickfd;

	desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = RTE_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
							 ring[vq->vq_nentries]),
				   VIRTIO_PCI_VRING_ALIGN);

	dev->vrings[queue_idx].num = vq->vq_nentries;
	dev->vrings[queue_idx].desc = (void *)(uintptr_t)desc_addr;
	dev->vrings[queue_idx].avail = (void *)(uintptr_t)avail_addr;
	dev->vrings[queue_idx].used = (void *)(uintptr_t)used_addr;

	return 0;
}

static void
virtio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* For legacy devices, write 0 to VIRTIO_PCI_QUEUE_PFN port, QEMU
	 * correspondingly stops the ioeventfds, and reset the status of
	 * the device.
	 * For modern devices, set queue desc, avail, used in PCI bar to 0,
	 * not see any more behavior in QEMU.
	 *
	 * Here we just care about what information to deliver to vhost-user
	 * or vhost-kernel. So we just close ioeventfd for now.
	 */
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;
}

static void
virtio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	uint64_t buf = 1;
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);
	char err_str[64];

	if (write(dev->kickfds[vq->vq_queue_index], &buf, sizeof(buf)) < 0)
		spdk_strerror_r(errno, err_str, sizeof(err_str));
		SPDK_ERRLOG("failed to kick backend: %s.\n", err_str);
}

static void
virtio_user_free(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = virtio_dev_get_user_dev(vdev);

	virtio_user_dev_uninit(dev);
}

const struct virtio_pci_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,
	.write_dev_cfg	= virtio_user_write_dev_config,
	.get_status	= virtio_user_get_status,
	.set_status	= virtio_user_set_status,
	.get_features	= virtio_user_get_features,
	.set_features	= virtio_user_set_features,
	.get_isr	= virtio_user_get_isr,
	.set_config_irq	= virtio_user_set_config_irq,
	.free_vdev	= virtio_user_free,
	.set_queue_irq	= virtio_user_set_queue_irq,
	.get_queue_num	= virtio_user_get_queue_num,
	.setup_queue	= virtio_user_setup_queue,
	.del_queue	= virtio_user_del_queue,
	.notify_queue	= virtio_user_notify_queue,
};
