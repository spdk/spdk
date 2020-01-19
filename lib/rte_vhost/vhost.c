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

#include <linux/vhost.h>
#include <linux/virtio_net.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef RTE_LIBRTE_VHOST_NUMA
#include <numaif.h>
#endif

#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_string_fns.h>
#include <rte_memory.h>
#include <rte_malloc.h>
#include <rte_vhost.h>

#include "vhost.h"

struct virtio_net *vhost_devices[MAX_VHOST_DEVICE];

struct virtio_net *
get_device(int vid)
{
	struct virtio_net *dev = vhost_devices[vid];

	if (unlikely(!dev)) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) device not found.\n", vid);
	}

	return dev;
}

static void
cleanup_vq(struct vhost_virtqueue *vq, int destroy)
{
	if ((vq->callfd >= 0) && (destroy != 0))
		close(vq->callfd);
	if (vq->kickfd >= 0)
		close(vq->kickfd);
}

/*
 * Unmap any memory, close any file descriptors and
 * free any memory owned by a device.
 */
void
cleanup_device(struct virtio_net *dev, int destroy)
{
	uint32_t i;

	vhost_backend_cleanup(dev);

	for (i = 0; i < dev->nr_vring; i++)
		cleanup_vq(dev->virtqueue[i], destroy);
}

/*
 * Release virtqueues and device memory.
 */
static void
free_device(struct virtio_net *dev)
{
	uint32_t i;
	struct vhost_virtqueue *vq;

	for (i = 0; i < dev->nr_vring; i++) {
		vq = dev->virtqueue[i];

		rte_free(vq->shadow_used_ring);

		rte_free(vq);
	}

	rte_free(dev);
}

static void
init_vring_queue(struct vhost_virtqueue *vq)
{
	memset(vq, 0, sizeof(struct vhost_virtqueue));

	vq->kickfd = VIRTIO_UNINITIALIZED_EVENTFD;
	vq->callfd = VIRTIO_UNINITIALIZED_EVENTFD;

	/* Backends are set to -1 indicating an inactive device. */
	vq->backend = -1;

	/*
	 * always set the vq to enabled; this is to keep compatibility
	 * with the old QEMU, whereas there is no SET_VRING_ENABLE message.
	 */
	vq->enabled = 1;

	TAILQ_INIT(&vq->zmbuf_list);
}

static void
reset_vring_queue(struct vhost_virtqueue *vq)
{
	int callfd;

	callfd = vq->callfd;
	init_vring_queue(vq);
	vq->callfd = callfd;
}

int
alloc_vring_queue(struct virtio_net *dev, uint32_t vring_idx)
{
	struct vhost_virtqueue *vq;

	vq = rte_malloc(NULL, sizeof(struct vhost_virtqueue), 0);
	if (vq == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocate memory for vring:%u.\n", vring_idx);
		return -1;
	}

	dev->virtqueue[vring_idx] = vq;
	init_vring_queue(vq);

	dev->nr_vring += 1;

	return 0;
}

/*
 * Reset some variables in device structure, while keeping few
 * others untouched, such as vid, ifname, nr_vring: they
 * should be same unless the device is removed.
 */
void
reset_device(struct virtio_net *dev)
{
	uint32_t i;

	dev->negotiated_features = 0;
	dev->protocol_features = 0;
	dev->flags = 0;

	for (i = 0; i < dev->nr_vring; i++)
		reset_vring_queue(dev->virtqueue[i]);
}

/*
 * Invoked when there is a new vhost-user connection established (when
 * there is a new virtio device being attached).
 */
int
vhost_new_device(uint64_t features, struct vhost_device_ops const *ops)
{
	struct virtio_net *dev;
	int i;

	dev = rte_zmalloc(NULL, sizeof(struct virtio_net), 0);
	if (dev == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocate memory for new dev.\n");
		return -1;
	}

	for (i = 0; i < MAX_VHOST_DEVICE; i++) {
		if (vhost_devices[i] == NULL)
			break;
	}
	if (i == MAX_VHOST_DEVICE) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to find a free slot for new device.\n");
		rte_free(dev);
		return -1;
	}

	vhost_devices[i] = dev;
	dev->vid = i;
	dev->features = features;
	dev->notify_ops = ops;

	return i;
}

/*
 * Invoked when there is the vhost-user connection is broken (when
 * the virtio device is being detached).
 */
void
vhost_destroy_device(int vid)
{
	struct virtio_net *dev = get_device(vid);

	if (dev == NULL)
		return;

	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(vid);
	}

	cleanup_device(dev, 1);
	free_device(dev);

	vhost_devices[vid] = NULL;
}

void
vhost_set_ifname(int vid, const char *if_name, unsigned int if_len)
{
	struct virtio_net *dev;
	unsigned int len;

	dev = get_device(vid);
	if (dev == NULL)
		return;

	len = if_len > sizeof(dev->ifname) ?
		sizeof(dev->ifname) : if_len;

	strncpy(dev->ifname, if_name, len);
	dev->ifname[sizeof(dev->ifname) - 1] = '\0';
}

void
vhost_enable_dequeue_zero_copy(int vid)
{
	struct virtio_net *dev = get_device(vid);

	if (dev == NULL)
		return;

	dev->dequeue_zero_copy = 1;
}

int
rte_vhost_get_mtu(int vid, uint16_t *mtu)
{
	struct virtio_net *dev = get_device(vid);

	if (!dev)
		return -ENODEV;

	if (!(dev->flags & VIRTIO_DEV_READY))
		return -EAGAIN;

	if (!(dev->negotiated_features & VIRTIO_NET_F_MTU))
		return -ENOTSUP;

	*mtu = dev->mtu;

	return 0;
}

int
rte_vhost_get_numa_node(int vid)
{
#ifdef RTE_LIBRTE_VHOST_NUMA
	struct virtio_net *dev = get_device(vid);
	int numa_node;
	int ret;

	if (dev == NULL)
		return -1;

	ret = get_mempolicy(&numa_node, NULL, 0, dev,
			    MPOL_F_NODE | MPOL_F_ADDR);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) failed to query numa node: %d\n", vid, ret);
		return -1;
	}

	return numa_node;
#else
	RTE_SET_USED(vid);
	return -1;
#endif
}

int
rte_vhost_get_ifname(int vid, char *buf, size_t len)
{
	struct virtio_net *dev = get_device(vid);

	if (dev == NULL)
		return -1;

	len = RTE_MIN(len, sizeof(dev->ifname));

	strncpy(buf, dev->ifname, len);
	buf[len - 1] = '\0';

	return 0;
}

int
rte_vhost_get_negotiated_features(int vid, uint64_t *features)
{
	struct virtio_net *dev;

	dev = get_device(vid);
	if (!dev)
		return -1;

	*features = dev->negotiated_features;
	return 0;
}

int
rte_vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	struct virtio_net *dev;
	struct rte_vhost_memory *m;
	size_t size;

	dev = get_device(vid);
	if (!dev)
		return -1;

	size = dev->mem->nregions * sizeof(struct rte_vhost_mem_region);
	m = malloc(sizeof(struct rte_vhost_memory) + size);
	if (!m)
		return -1;

	m->nregions = dev->mem->nregions;
	memcpy(m->regions, dev->mem->regions, size);
	*mem = m;

	return 0;
}

int
rte_vhost_get_vhost_vring(int vid, uint16_t vring_idx,
			  struct rte_vhost_vring *vring)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if (!dev)
		return -1;

	if (vring_idx >= VHOST_MAX_VRING)
		return -1;

	vq = dev->virtqueue[vring_idx];
	if (!vq)
		return -1;

	vring->desc  = vq->desc;
	vring->avail = vq->avail;
	vring->used  = vq->used;
	vring->log_guest_addr  = vq->log_guest_addr;

	vring->callfd  = vq->callfd;
	vring->kickfd  = vq->kickfd;
	vring->size    = vq->size;

	return 0;
}

uint16_t
rte_vhost_avail_entries(int vid, uint16_t queue_id)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if (!dev)
		return 0;

	vq = dev->virtqueue[queue_id];
	if (!vq->enabled)
		return 0;

	return *(volatile uint16_t *)&vq->avail->idx - vq->last_used_idx;
}

int
rte_vhost_enable_guest_notification(int vid, uint16_t queue_id, int enable)
{
	struct virtio_net *dev = get_device(vid);

	if (dev == NULL)
		return -1;

	if (enable) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"guest notification isn't supported.\n");
		return -1;
	}

	dev->virtqueue[queue_id]->used->flags = VRING_USED_F_NO_NOTIFY;
	return 0;
}

void
rte_vhost_log_write(int vid, uint64_t addr, uint64_t len)
{
	struct virtio_net *dev = get_device(vid);

	if (dev == NULL)
		return;

	vhost_log_write(dev, addr, len);
}

void
rte_vhost_log_used_vring(int vid, uint16_t vring_idx,
			 uint64_t offset, uint64_t len)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if (dev == NULL)
		return;

	if (vring_idx >= VHOST_MAX_VRING)
		return;
	vq = dev->virtqueue[vring_idx];
	if (!vq)
		return;

	vhost_log_used_vring(dev, vq, offset, len);
}

int
rte_vhost_set_vring_base(int vid, uint16_t vring_idx,
		uint16_t last_avail_idx, uint16_t last_used_idx)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if (!dev)
		return -1;

	if (vring_idx >= VHOST_MAX_VRING)
		return -1;

	vq = dev->virtqueue[vring_idx];
	if (!vq)
		return -1;

	vq->last_avail_idx = last_avail_idx;
	vq->last_used_idx = last_used_idx;

	return 0;
}

int
rte_vhost_get_vring_base(int vid, uint16_t vring_idx,
		uint16_t *last_avail_idx, uint16_t *last_used_idx)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if (!dev)
		return -1;

	if (vring_idx >= VHOST_MAX_VRING)
		return -1;

	vq = dev->virtqueue[vring_idx];
	if (!vq)
		return -1;

	*last_avail_idx = vq->last_avail_idx;
	*last_used_idx = vq->last_used_idx;

	return 0;
}

int
rte_vhost_vring_call(int vid, uint16_t vring_idx)
{
	struct virtio_net *dev;
	struct vhost_virtqueue *vq;

	dev = get_device(vid);
	if(!dev)
		return -1;

	if (vring_idx >= VHOST_MAX_VRING)
		return -1;

	vq = dev->virtqueue[vring_idx];
	if (!vq)
		return -1;

	/* Ensure all our used ring changes are visible to the guest at the time
	 * of interrupt.
	 * TODO: this is currently an sfence on x86. For other architectures we
	 * will most likely need an smp_mb(), but smp_mb() is an overkill for x86.
	 */
	rte_wmb();

	if (vq->callfd != -1) {
		eventfd_write(vq->callfd, (eventfd_t)1);
		return 0;
	}

	return -1;
}

int
rte_vhost_set_last_inflight_io_split(int vid, uint16_t vring_idx,
				     uint16_t idx)
{
	return 0;
}

int
rte_vhost_clr_inflight_desc_split(int vid, uint16_t vring_idx,
				  uint16_t last_used_idx, uint16_t idx)
{
	return 0;
}

int
rte_vhost_set_inflight_desc_split(int vid, uint16_t vring_idx,
				  uint16_t idx)
{
	return 0;
}

int
rte_vhost_get_vhost_ring_inflight(int vid, uint16_t vring_idx,
				  struct rte_vhost_ring_inflight *vring)
{
	return 0;
}
