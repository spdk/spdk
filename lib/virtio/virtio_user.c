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

#include "vhost_user.h"
#include "spdk/string.h"
#include "spdk/config.h"

#include "spdk_internal/virtio.h"

#define VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES \
	((1ULL << VHOST_USER_PROTOCOL_F_MQ) | \
	(1ULL << VHOST_USER_PROTOCOL_F_CONFIG))

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
	return dev->ops->send_request(dev, VHOST_USER_SET_VRING_CALL, &file);
}

static int
virtio_user_set_vring_addr(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vring *vring = &dev->vrings[queue_sel];
	struct vhost_vring_addr addr = {
		.index = queue_sel,
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail,
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,
		.log_guest_addr = 0,
		.flags = 0, /* disable log */
	};

	return dev->ops->send_request(dev, VHOST_USER_SET_VRING_ADDR, &addr);
}

static int
virtio_user_kick_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings[queue_sel];
	int rc;

	state.index = queue_sel;
	state.num = vring->num;
	rc = dev->ops->send_request(dev, VHOST_USER_SET_VRING_NUM, &state);
	if (rc < 0) {
		return rc;
	}

	state.index = queue_sel;
	state.num = 0; /* no reservation */
	rc = dev->ops->send_request(dev, VHOST_USER_SET_VRING_BASE, &state);
	if (rc < 0) {
		return rc;
	}

	virtio_user_set_vring_addr(vdev, queue_sel);

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel];
	return dev->ops->send_request(dev, VHOST_USER_SET_VRING_KICK, &file);
}

static int
virtio_user_stop_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;

	state.index = queue_sel;
	state.num = 0;

	return dev->ops->send_request(dev, VHOST_USER_GET_VRING_BASE, &state);
}

static int
virtio_user_queue_setup(struct virtio_dev *vdev,
			int (*fn)(struct virtio_dev *, uint32_t))
{
	uint32_t i;
	int rc;

	for (i = 0; i < vdev->max_queues; ++i) {
		rc = fn(vdev, i);
		if (rc < 0) {
			SPDK_ERRLOG("setup tx vq fails: %"PRIu32".\n", i);
			return rc;
		}
	}

	return 0;
}

static int
virtio_user_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action,
		       void *vaddr, size_t size)
{
	struct virtio_dev *vdev = cb_ctx;
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int ret;

	/* We have to resend all mappings anyway, so don't bother with any
	 * page tracking.
	 */
	ret = dev->ops->send_request(dev, VHOST_USER_SET_MEM_TABLE, NULL);
	if (ret < 0) {
		return ret;
	}

	/* Since we might want to use that mapping straight away, we have to
	 * make sure the guest has already processed our SET_MEM_TABLE message.
	 * F_REPLY_ACK is just a feature and the host is not obliged to
	 * support it, so we send a simple message that always has a response
	 * and we wait for that response. Messages are always processed in order.
	 */
	return dev->ops->send_request(dev, VHOST_USER_GET_FEATURES, &features);
}

static int
virtio_user_register_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	const struct spdk_mem_map_ops virtio_user_map_ops = {
		.notify_cb = virtio_user_map_notify,
		.are_contiguous = NULL
	};

	dev->mem_map = spdk_mem_map_alloc(0, &virtio_user_map_ops, vdev);
	if (dev->mem_map == NULL) {
		SPDK_ERRLOG("spdk_mem_map_alloc() failed\n");
		return -1;
	}

	return 0;
}

static void
virtio_user_unregister_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_mem_map_free(&dev->mem_map);
}

static int
virtio_user_start_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t host_max_queues;
	int ret;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ)) == 0 &&
	    vdev->max_queues > 1 + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues, but the "
			     "host doesn't support VHOST_USER_PROTOCOL_F_MQ. "
			     "Only one request queue will be used.\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num);
		vdev->max_queues = 1 + vdev->fixed_queues_num;
	}

	/* negotiate the number of I/O queues. */
	ret = dev->ops->send_request(dev, VHOST_USER_GET_QUEUE_NUM, &host_max_queues);
	if (ret < 0) {
		return ret;
	}

	if (vdev->max_queues > host_max_queues + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues"
			     "but only %"PRIu64" available\n",
			     vdev->name, vdev->max_queues - vdev->fixed_queues_num,
			     host_max_queues);
		vdev->max_queues = host_max_queues;
	}

	/* tell vhost to create queues */
	ret = virtio_user_queue_setup(vdev, virtio_user_create_queue);
	if (ret < 0) {
		return ret;
	}

	ret = virtio_user_register_mem(vdev);
	if (ret < 0) {
		return ret;
	}

	return virtio_user_queue_setup(vdev, virtio_user_kick_queue);
}

static int
virtio_user_stop_device(struct virtio_dev *vdev)
{
	int ret;

	ret = virtio_user_queue_setup(vdev, virtio_user_stop_queue);
	/* a queue might fail to stop for various reasons, e.g. socket
	 * connection going down, but this mustn't prevent us from freeing
	 * the mem map.
	 */
	virtio_user_unregister_mem(vdev);
	return ret;
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

	return dev->ops->setup(dev);
}

static int
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
			    void *dst, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;
	}

	cfg.offset = 0;
	cfg.size = VHOST_USER_MAX_CONFIG_SIZE;

	rc = dev->ops->send_request(dev, VHOST_USER_GET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("get_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	memcpy(dst, cfg.region + offset, length);
	return 0;
}

static int
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
			     const void *src, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_user_config cfg = {0};
	int rc;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
		return -ENOTSUP;
	}

	cfg.offset = offset;
	cfg.size = length;
	memcpy(cfg.region, src, length);

	rc = dev->ops->send_request(dev, VHOST_USER_SET_CONFIG, &cfg);
	if (rc < 0) {
		SPDK_ERRLOG("set_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc = 0;

	if ((dev->status & VIRTIO_CONFIG_S_NEEDS_RESET) &&
	    status != VIRTIO_CONFIG_S_RESET) {
		rc = -1;
	} else if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		rc = virtio_user_start_device(vdev);
	} else if (status == VIRTIO_CONFIG_S_RESET &&
		   (dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		rc = virtio_user_stop_device(vdev);
	}

	if (rc != 0) {
		dev->status |= VIRTIO_CONFIG_S_NEEDS_RESET;
	} else {
		dev->status = status;
	}
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
	int rc;

	rc = dev->ops->send_request(dev, VHOST_USER_GET_FEATURES, &features);
	if (rc < 0) {
		SPDK_ERRLOG("get_features failed: %s\n", spdk_strerror(-rc));
		return 0;
	}

	return features;
}

static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t protocol_features;
	int ret;

	ret = dev->ops->send_request(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0) {
		return ret;
	}

	vdev->negotiated_features = features;
	vdev->modern = virtio_dev_has_feature(vdev, VIRTIO_F_VERSION_1);

	if (!virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		/* nothing else to do */
		return 0;
	}

	ret = dev->ops->send_request(dev, VHOST_USER_GET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	protocol_features &= VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES;
	ret = dev->ops->send_request(dev, VHOST_USER_SET_PROTOCOL_FEATURES, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	dev->protocol_features = protocol_features;
	return 0;
}

static uint16_t
virtio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Currently each queue has same queue size */
	return dev->queue_size;
}

static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;
	uint16_t queue_idx = vq->vq_queue_index;
	void *queue_mem;
	uint64_t desc_addr, avail_addr, used_addr;
	int callfd, kickfd, rc;

	if (dev->callfds[queue_idx] != -1 || dev->kickfds[queue_idx] != -1) {
		SPDK_ERRLOG("queue %"PRIu16" already exists\n", queue_idx);
		return -EEXIST;
	}

	/* May use invalid flag, but some backend uses kickfd and
	 * callfd as criteria to judge if dev is alive. so finally we
	 * use real event_fd.
	 */
	callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (callfd < 0) {
		SPDK_ERRLOG("callfd error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		SPDK_ERRLOG("kickfd error, %s\n", spdk_strerror(errno));
		close(callfd);
		return -errno;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VIRTIO_PCI_VRING_ALIGN, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (queue_mem == NULL) {
		close(kickfd);
		close(callfd);
		return -ENOMEM;
	}

	vq->vq_ring_mem = SPDK_VTOPHYS_ERROR;
	vq->vq_ring_virt_mem = queue_mem;

	state.index = vq->vq_queue_index;
	state.num = vq->vq_nentries;

	if (virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
		rc = dev->ops->send_request(dev, VHOST_USER_SET_VRING_ENABLE, &state);
		if (rc < 0) {
			SPDK_ERRLOG("failed to send VHOST_USER_SET_VRING_ENABLE: %s\n",
				    spdk_strerror(-rc));
			close(kickfd);
			close(callfd);
			spdk_free(queue_mem);
			return -rc;
		}
	}

	dev->callfds[queue_idx] = callfd;
	dev->kickfds[queue_idx] = kickfd;

	desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = SPDK_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
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
	 * Here we just care about what information to deliver to vhost-user.
	 * So we just close ioeventfd for now.
	 */
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;

	spdk_free(vq->vq_ring_virt_mem);
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
virtio_user_dump_json_info(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "type", "user");
	spdk_json_write_named_string(w, "socket", dev->path);
}

static void
virtio_user_write_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "trtype", "user");
	spdk_json_write_named_string(w, "traddr", dev->path);
	spdk_json_write_named_uint32(w, "vq_count", vdev->max_queues - vdev->fixed_queues_num);
	spdk_json_write_named_uint32(w, "vq_size", virtio_dev_backend_ops(vdev)->get_queue_size(vdev, 0));
}

static const struct virtio_dev_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,
	.write_dev_cfg	= virtio_user_write_dev_config,
	.get_status	= virtio_user_get_status,
	.set_status	= virtio_user_set_status,
	.get_features	= virtio_user_get_features,
	.set_features	= virtio_user_set_features,
	.destruct_dev	= virtio_user_destroy,
	.get_queue_size	= virtio_user_get_queue_size,
	.setup_queue	= virtio_user_setup_queue,
	.del_queue	= virtio_user_del_queue,
	.notify_queue	= virtio_user_notify_queue,
	.dump_json_info = virtio_user_dump_json_info,
	.write_json_config = virtio_user_write_json_config,
};

int
virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
		     uint32_t queue_size)
{
	struct virtio_user_dev *dev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name gived for controller: %s\n", path);
		return -EINVAL;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	vdev->is_hw = 0;

	snprintf(dev->path, PATH_MAX, "%s", path);
	dev->queue_size = queue_size;

	rc = virtio_user_dev_setup(vdev);
	if (rc < 0) {
		SPDK_ERRLOG("backend set up fails\n");
		goto err;
	}

	rc = dev->ops->send_request(dev, VHOST_USER_SET_OWNER, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("set_owner fails: %s\n", spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	virtio_dev_destruct(vdev);
	return rc;
}
