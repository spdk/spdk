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

#include "spdk/stdinc.h"

#include <sys/eventfd.h>

#include <linux/virtio_scsi.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_alarm.h>

#include "virtio_user/vhost.h"
#include "spdk/string.h"

#include "spdk_internal/virtio.h"

static int
virtio_user_create_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Of all per virtqueue MSGs, make sure VHOST_SET_VRING_CALL come
	 * firstly because vhost depends on this msg to allocate virtqueue
	 * pair.
	 */
	struct vhost_vring_file file;

	file.index = queue_sel;
	file.fd = dev->callfds[queue_sel];
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_CALL, &file);

	return 0;
}

static int
virtio_user_kick_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings[queue_sel];
	struct vhost_vring_addr addr = {
		.index = queue_sel,
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail,
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,
		.log_guest_addr = 0,
		.flags = 0, /* disable log */
	};

	state.index = queue_sel;
	state.num = vring->num;
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_NUM, &state);

	state.index = queue_sel;
	state.num = 0; /* no reservation */
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_BASE, &state);

	dev->ops->send_request(dev, VHOST_USER_SET_VRING_ADDR, &addr);

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel];
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_KICK, &file);

	return 0;
}

static int
virtio_user_stop_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;

	state.index = queue_sel;
	state.num = 0;
	dev->ops->send_request(dev, VHOST_USER_GET_VRING_BASE, &state);

	return 0;
}

static int
virtio_user_queue_setup(struct virtio_dev *vdev,
			int (*fn)(struct virtio_dev *, uint32_t))
{
	uint32_t i;

	for (i = 0; i < vdev->max_queues; ++i) {
		if (fn(vdev, i) < 0) {
			SPDK_ERRLOG("setup tx vq fails: %"PRIu32".\n", i);
			return -1;
		}
	}

	return 0;
}

static int
virtio_user_start_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t host_max_queues;
	int ret;

	/* negotiate the number of I/O queues. */
	ret = dev->ops->send_request(dev, VHOST_USER_GET_QUEUE_NUM, &host_max_queues);
	if (ret < 0) {
		return -1;
	}

	if (vdev->max_queues > host_max_queues + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues"
			     "but only %"PRIu64" available\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num,
			     host_max_queues);
		vdev->max_queues = host_max_queues;
	}

	/* tell vhost to create queues */
	if (virtio_user_queue_setup(vdev, virtio_user_create_queue) < 0) {
		return -1;
	}

	/* share memory regions */
	ret = dev->ops->send_request(dev, VHOST_USER_SET_MEM_TABLE, NULL);
	if (ret < 0) {
		return -1;
	}

	/* kick queues */
	if (virtio_user_queue_setup(vdev, virtio_user_kick_queue) < 0) {
		return -1;
	}

	return 0;
}

static int virtio_user_stop_device(struct virtio_dev *vdev)
{
	return virtio_user_queue_setup(vdev, virtio_user_stop_queue);
}

static int
virtio_user_dev_setup(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint16_t i;

	dev->vhostfd = -1;

	for (i = 0; i < SPDK_VIRTIO_MAX_VIRTQUEUES; ++i) {
		dev->callfds[i] = -1;
		dev->kickfds[i] = -1;
	}

	dev->ops = &ops_user;

	if (dev->ops->setup(dev) < 0) {
		return -1;
	}

	return 0;
}

static void
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
			    void *dst, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};

	cfg.offset = 0;
	cfg.size = VHOST_USER_MAX_CONFIG_SIZE;

	if (dev->ops->send_request(dev, VHOST_USER_GET_CONFIG, &cfg) < 0) {
		SPDK_ERRLOG("get_config failed: %s\n", spdk_strerror(errno));
		return;
	}

	memcpy(dst, cfg.region + offset, length);
}

static void
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
			     const void *src, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};

	cfg.offset = offset;
	cfg.size = length;
	memcpy(cfg.region, src, length);

	if (dev->ops->send_request(dev, VHOST_USER_SET_CONFIG, &cfg) < 0) {
		SPDK_ERRLOG("set_config failed: %s\n", spdk_strerror(errno));
		return;
	}
}

static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = vdev->ctx;

	if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		virtio_user_start_device(vdev);
	} else if (status == VIRTIO_CONFIG_S_RESET &&
		   (dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		virtio_user_stop_device(vdev);
	}
	dev->status = status;
}

static uint8_t
virtio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	return dev->status;
}

static uint64_t
virtio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;

	if (dev->ops->send_request(dev, VHOST_USER_GET_FEATURES, &features) < 0) {
		SPDK_ERRLOG("get_features failed: %s\n", spdk_strerror(errno));
		return 0;
	}

	return features;
}

static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int ret;

	ret = dev->ops->send_request(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0) {
		return -1;
	}

	vdev->negotiated_features = features;
	vdev->modern = virtio_dev_has_feature(vdev, VIRTIO_F_VERSION_1);

	return 0;
}

/* This function is to get the queue size, aka, number of descs, of a specified
 * queue. Different with the VHOST_USER_GET_QUEUE_NUM, which is used to get the
 * max supported queues.
 */
static uint16_t
virtio_user_get_queue_num(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Currently, each queue has same queue size */
	return dev->queue_size;
}

static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint16_t queue_idx = vq->vq_queue_index;
	uint64_t desc_addr, avail_addr, used_addr;
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
		SPDK_ERRLOG("callfd error, %s\n", spdk_strerror(errno));
		return -1;
	}

	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		SPDK_ERRLOG("kickfd error, %s\n", spdk_strerror(errno));
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
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;
}

static void
virtio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	uint64_t buf = 1;
	struct virtio_user_dev *dev = vdev->ctx;

	if (write(dev->kickfds[vq->vq_queue_index], &buf, sizeof(buf)) < 0) {
		SPDK_ERRLOG("failed to kick backend: %s.\n", spdk_strerror(errno));
	}
}

static void
virtio_user_destroy(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->vhostfd);
	free(dev);
}

static void
virtio_user_dump_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_name(w, "type");
	spdk_json_write_string(w, "user");

	spdk_json_write_name(w, "socket");
	spdk_json_write_string(w, dev->path);
}

static const struct virtio_dev_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,
	.write_dev_cfg	= virtio_user_write_dev_config,
	.get_status	= virtio_user_get_status,
	.set_status	= virtio_user_set_status,
	.get_features	= virtio_user_get_features,
	.set_features	= virtio_user_set_features,
	.destruct_dev	= virtio_user_destroy,
	.get_queue_num	= virtio_user_get_queue_num,
	.setup_queue	= virtio_user_setup_queue,
	.del_queue	= virtio_user_del_queue,
	.notify_queue	= virtio_user_notify_queue,
	.dump_json_config = virtio_user_dump_json_config,
};

int
virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
		     uint32_t queue_size)
{
	struct virtio_user_dev *dev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name gived for controller: %s\n", path);
		return -1;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -1;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return -1;
	}

	vdev->is_hw = 0;

	snprintf(dev->path, PATH_MAX, "%s", path);
	dev->queue_size = queue_size;

	if (virtio_user_dev_setup(vdev) < 0) {
		SPDK_ERRLOG("backend set up fails\n");
		goto err;
	}

	if (dev->ops->send_request(dev, VHOST_USER_SET_OWNER, NULL) < 0) {
		SPDK_ERRLOG("set_owner fails: %s\n", spdk_strerror(errno));
		goto err;
	}

	return 0;

err:
	virtio_dev_destruct(vdev);
	return -1;
}
