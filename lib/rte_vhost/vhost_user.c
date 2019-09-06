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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <asm/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#ifdef RTE_LIBRTE_VHOST_NUMA
#include <numaif.h>
#endif

#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_log.h>

#include "vhost.h"
#include "vhost_user.h"

#define VIRTIO_MIN_MTU 68
#define VIRTIO_MAX_MTU 65535

static const char *vhost_message_str[VHOST_USER_MAX] = {
	[VHOST_USER_NONE] = "VHOST_USER_NONE",
	[VHOST_USER_GET_FEATURES] = "VHOST_USER_GET_FEATURES",
	[VHOST_USER_SET_FEATURES] = "VHOST_USER_SET_FEATURES",
	[VHOST_USER_SET_OWNER] = "VHOST_USER_SET_OWNER",
	[VHOST_USER_RESET_OWNER] = "VHOST_USER_RESET_OWNER",
	[VHOST_USER_SET_MEM_TABLE] = "VHOST_USER_SET_MEM_TABLE",
	[VHOST_USER_SET_LOG_BASE] = "VHOST_USER_SET_LOG_BASE",
	[VHOST_USER_SET_LOG_FD] = "VHOST_USER_SET_LOG_FD",
	[VHOST_USER_SET_VRING_NUM] = "VHOST_USER_SET_VRING_NUM",
	[VHOST_USER_SET_VRING_ADDR] = "VHOST_USER_SET_VRING_ADDR",
	[VHOST_USER_SET_VRING_BASE] = "VHOST_USER_SET_VRING_BASE",
	[VHOST_USER_GET_VRING_BASE] = "VHOST_USER_GET_VRING_BASE",
	[VHOST_USER_SET_VRING_KICK] = "VHOST_USER_SET_VRING_KICK",
	[VHOST_USER_SET_VRING_CALL] = "VHOST_USER_SET_VRING_CALL",
	[VHOST_USER_SET_VRING_ERR]  = "VHOST_USER_SET_VRING_ERR",
	[VHOST_USER_GET_PROTOCOL_FEATURES]  = "VHOST_USER_GET_PROTOCOL_FEATURES",
	[VHOST_USER_SET_PROTOCOL_FEATURES]  = "VHOST_USER_SET_PROTOCOL_FEATURES",
	[VHOST_USER_GET_QUEUE_NUM]  = "VHOST_USER_GET_QUEUE_NUM",
	[VHOST_USER_SET_VRING_ENABLE]  = "VHOST_USER_SET_VRING_ENABLE",
	[VHOST_USER_SEND_RARP]  = "VHOST_USER_SEND_RARP",
	[VHOST_USER_NET_SET_MTU]  = "VHOST_USER_NET_SET_MTU",
	[VHOST_USER_GET_CONFIG] = "VHOST_USER_GET_CONFIG",
	[VHOST_USER_SET_CONFIG] = "VHOST_USER_SET_CONFIG",
	[VHOST_USER_NVME_ADMIN] = "VHOST_USER_NVME_ADMIN",
	[VHOST_USER_NVME_SET_CQ_CALL] = "VHOST_USER_NVME_SET_CQ_CALL",
	[VHOST_USER_NVME_GET_CAP] = "VHOST_USER_NVME_GET_CAP",
	[VHOST_USER_NVME_START_STOP] = "VHOST_USER_NVME_START_STOP",
	[VHOST_USER_NVME_SET_BAR_MR] = "VHOST_USER_NVME_SET_BAR_MR"
};

static uint64_t
get_blk_size(int fd)
{
	struct stat stat;
	int ret;

	ret = fstat(fd, &stat);
	return ret == -1 ? (uint64_t)-1 : (uint64_t)stat.st_blksize;
}

static void
free_mem_region(struct virtio_net *dev)
{
	uint32_t i;
	struct rte_vhost_mem_region *reg;

	if (!dev || !dev->mem)
		return;

	for (i = 0; i < dev->mem->nregions; i++) {
		reg = &dev->mem->regions[i];
		if (reg->host_user_addr) {
			munmap(reg->mmap_addr, reg->mmap_size);
			close(reg->fd);
		}
	}
}

void
vhost_backend_cleanup(struct virtio_net *dev)
{
	uint32_t i;

	if (dev->has_new_mem_table) {
		for (i = 0; i < dev->mem_table.nregions; i++) {
			close(dev->mem_table_fds[i]);
		}
		dev->has_new_mem_table = 0;
	}
	if (dev->mem) {
		free_mem_region(dev);
		rte_free(dev->mem);
		dev->mem = NULL;
	}

	free(dev->guest_pages);
	dev->guest_pages = NULL;

	if (dev->log_addr) {
		munmap((void *)(uintptr_t)dev->log_addr, dev->log_size);
		dev->log_addr = 0;
	}
	if (dev->bar_addr) {
		munmap((void *)(uintptr_t)dev->bar_addr, dev->bar_size);
		dev->bar_addr = NULL;
		dev->bar_size = 0;
	}
}

/*
 * This function just returns success at the moment unless
 * the device hasn't been initialised.
 */
static int
vhost_user_set_owner(void)
{
	return 0;
}

static int
vhost_user_reset_owner(struct virtio_net *dev)
{
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	cleanup_device(dev, 0);
	reset_device(dev);
	return 0;
}

/*
 * The features that we support are requested.
 */
static uint64_t
vhost_user_get_features(struct virtio_net *dev)
{
	return dev->features;
}

/*
 * We receive the negotiated features supported by us and the virtio device.
 */
static int
vhost_user_set_features(struct virtio_net *dev, uint64_t features)
{
	uint64_t vhost_features = 0;

	vhost_features = vhost_user_get_features(dev);
	if (features & ~vhost_features) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) received invalid negotiated features.\n",
			dev->vid);
		return -1;
	}

	if ((dev->flags & VIRTIO_DEV_RUNNING) && dev->negotiated_features != features) {
		if (dev->notify_ops->features_changed) {
			dev->notify_ops->features_changed(dev->vid, features);
		} else {
			dev->flags &= ~VIRTIO_DEV_RUNNING;
			dev->notify_ops->destroy_device(dev->vid);
		}
	}

	dev->negotiated_features = features;
	if (dev->negotiated_features &
		((1 << VIRTIO_NET_F_MRG_RXBUF) | (1ULL << VIRTIO_F_VERSION_1))) {
		dev->vhost_hlen = sizeof(struct virtio_net_hdr_mrg_rxbuf);
	} else {
		dev->vhost_hlen = sizeof(struct virtio_net_hdr);
	}
	VHOST_LOG_DEBUG(VHOST_CONFIG,
		"(%d) mergeable RX buffers %s, virtio 1 %s\n",
		dev->vid,
		(dev->negotiated_features & (1 << VIRTIO_NET_F_MRG_RXBUF)) ? "on" : "off",
		(dev->negotiated_features & (1ULL << VIRTIO_F_VERSION_1)) ? "on" : "off");

	return 0;
}

/*
 * The virtio device sends us the size of the descriptor ring.
 */
static int
vhost_user_set_vring_num(struct virtio_net *dev,
			 VhostUserMsg *msg)
{
	struct vhost_virtqueue *vq = dev->virtqueue[msg->payload.state.index];

	vq->size = msg->payload.state.num;

	if (dev->dequeue_zero_copy) {
		vq->nr_zmbuf = 0;
		vq->last_zmbuf_idx = 0;
		vq->zmbuf_size = vq->size;
		vq->zmbufs = rte_zmalloc(NULL, vq->zmbuf_size *
					 sizeof(struct zcopy_mbuf), 0);
		if (vq->zmbufs == NULL) {
			RTE_LOG(WARNING, VHOST_CONFIG,
				"failed to allocate mem for zero copy; "
				"zero copy is force disabled\n");
			dev->dequeue_zero_copy = 0;
		}
	}

	vq->shadow_used_ring = rte_malloc(NULL,
				vq->size * sizeof(struct vring_used_elem),
				RTE_CACHE_LINE_SIZE);
	if (!vq->shadow_used_ring) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to allocate memory for shadow used ring.\n");
		return -1;
	}

	return 0;
}

/*
 * Reallocate virtio_dev and vhost_virtqueue data structure to make them on the
 * same numa node as the memory of vring descriptor.
 */
#ifdef RTE_LIBRTE_VHOST_NUMA
static struct virtio_net*
numa_realloc(struct virtio_net *dev, int index)
{
	int oldnode, newnode;
	struct virtio_net *old_dev;
	struct vhost_virtqueue *old_vq, *vq;
	int ret;

	old_dev = dev;
	vq = old_vq = dev->virtqueue[index];

	ret = get_mempolicy(&newnode, NULL, 0, old_vq->desc,
			    MPOL_F_NODE | MPOL_F_ADDR);

	/* check if we need to reallocate vq */
	ret |= get_mempolicy(&oldnode, NULL, 0, old_vq,
			     MPOL_F_NODE | MPOL_F_ADDR);
	if (ret) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Unable to get vq numa information.\n");
		return dev;
	}
	if (oldnode != newnode) {
		RTE_LOG(INFO, VHOST_CONFIG,
			"reallocate vq from %d to %d node\n", oldnode, newnode);
		vq = rte_malloc_socket(NULL, sizeof(*vq), 0, newnode);
		if (!vq)
			return dev;

		memcpy(vq, old_vq, sizeof(*vq));
		rte_free(old_vq);
	}

	/* check if we need to reallocate dev */
	ret = get_mempolicy(&oldnode, NULL, 0, old_dev,
			    MPOL_F_NODE | MPOL_F_ADDR);
	if (ret) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Unable to get dev numa information.\n");
		goto out;
	}
	if (oldnode != newnode) {
		RTE_LOG(INFO, VHOST_CONFIG,
			"reallocate dev from %d to %d node\n",
			oldnode, newnode);
		dev = rte_malloc_socket(NULL, sizeof(*dev), 0, newnode);
		if (!dev) {
			dev = old_dev;
			goto out;
		}

		memcpy(dev, old_dev, sizeof(*dev));
		rte_free(old_dev);
	}

out:
	dev->virtqueue[index] = vq;
	vhost_devices[dev->vid] = dev;

	return dev;
}
#else
static struct virtio_net*
numa_realloc(struct virtio_net *dev, int index __rte_unused)
{
	return dev;
}
#endif

/*
 * Converts QEMU virtual address to Vhost virtual address. This function is
 * used to convert the ring addresses to our address space.
 */
static uint64_t
qva_to_vva(struct virtio_net *dev, uint64_t qva, uint64_t *len)
{
	struct rte_vhost_mem_region *reg;
	uint32_t i;

	/* Find the region where the address lives. */
	for (i = 0; i < dev->mem->nregions; i++) {
		reg = &dev->mem->regions[i];

		if (qva >= reg->guest_user_addr &&
		    qva <  reg->guest_user_addr + reg->size) {

			if (unlikely(*len > reg->guest_user_addr + reg->size - qva))
				*len = reg->guest_user_addr + reg->size - qva;

			return qva - reg->guest_user_addr +
			       reg->host_user_addr;
		}
	}

	return 0;
}

static int vhost_setup_mem_table(struct virtio_net *dev);

/*
 * The virtio device sends us the desc, used and avail ring addresses.
 * This function then converts these to our address space.
 */
static int
vhost_user_set_vring_addr(struct virtio_net *dev, VhostUserMsg *msg)
{
	struct vhost_virtqueue *vq;
	uint64_t len;

	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	if (dev->has_new_mem_table) {
		vhost_setup_mem_table(dev);
		dev->has_new_mem_table = 0;
	}

	if (dev->mem == NULL)
		return -1;

	/* addr->index refers to the queue index. The txq 1, rxq is 0. */
	vq = dev->virtqueue[msg->payload.addr.index];

	/* The addresses are converted from QEMU virtual to Vhost virtual. */
	len = sizeof(struct vring_desc) * vq->size;
	vq->desc = (struct vring_desc *)(uintptr_t)qva_to_vva(dev,
			msg->payload.addr.desc_user_addr, &len);
	if (vq->desc == 0 || len != sizeof(struct vring_desc) * vq->size) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) failed to map desc ring.\n",
			dev->vid);
		return -1;
	}

	dev = numa_realloc(dev, msg->payload.addr.index);
	vq = dev->virtqueue[msg->payload.addr.index];

	len = sizeof(struct vring_avail) + sizeof(uint16_t) * vq->size;
	vq->avail = (struct vring_avail *)(uintptr_t)qva_to_vva(dev,
			msg->payload.addr.avail_user_addr, &len);
	if (vq->avail == 0 ||
			len != sizeof(struct vring_avail)
			+ sizeof(uint16_t) * vq->size) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) failed to find avail ring address.\n",
			dev->vid);
		return -1;
	}

	len = sizeof(struct vring_used) +
		sizeof(struct vring_used_elem) * vq->size;
	vq->used = (struct vring_used *)(uintptr_t)qva_to_vva(dev,
			msg->payload.addr.used_user_addr, &len);
	if (vq->used == 0 || len != sizeof(struct vring_used) +
			sizeof(struct vring_used_elem) * vq->size) {

		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) failed to find used ring address.\n",
			dev->vid);
		return -1;
	}

	if (vq->last_used_idx != vq->used->idx) {
		RTE_LOG(WARNING, VHOST_CONFIG,
			"last_used_idx (%u) and vq->used->idx (%u) mismatches; "
			"some packets maybe resent for Tx and dropped for Rx\n",
			vq->last_used_idx, vq->used->idx);
		vq->last_used_idx  = vq->used->idx;
		vq->last_avail_idx = vq->used->idx;
	}

	vq->log_guest_addr = msg->payload.addr.log_guest_addr;

	VHOST_LOG_DEBUG(VHOST_CONFIG, "(%d) mapped address desc: %p\n",
			dev->vid, vq->desc);
	VHOST_LOG_DEBUG(VHOST_CONFIG, "(%d) mapped address avail: %p\n",
			dev->vid, vq->avail);
	VHOST_LOG_DEBUG(VHOST_CONFIG, "(%d) mapped address used: %p\n",
			dev->vid, vq->used);
	VHOST_LOG_DEBUG(VHOST_CONFIG, "(%d) log_guest_addr: %" PRIx64 "\n",
			dev->vid, vq->log_guest_addr);

	return 0;
}

/*
 * The virtio device sends us the available ring last used index.
 */
static int
vhost_user_set_vring_base(struct virtio_net *dev,
			  VhostUserMsg *msg)
{
	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	dev->virtqueue[msg->payload.state.index]->last_used_idx  = msg->payload.state.num;
	dev->virtqueue[msg->payload.state.index]->last_avail_idx = msg->payload.state.num;

	return 0;
}

static void
add_one_guest_page(struct virtio_net *dev, uint64_t guest_phys_addr,
		   uint64_t host_phys_addr, uint64_t size)
{
	struct guest_page *page, *last_page;

	if (dev->nr_guest_pages == dev->max_guest_pages) {
		dev->max_guest_pages = RTE_MAX(8U, dev->max_guest_pages * 2);
		dev->guest_pages = realloc(dev->guest_pages,
					dev->max_guest_pages * sizeof(*page));
	}

	if (dev->nr_guest_pages > 0) {
		last_page = &dev->guest_pages[dev->nr_guest_pages - 1];
		/* merge if the two pages are continuous */
		if (host_phys_addr == last_page->host_phys_addr +
				      last_page->size) {
			last_page->size += size;
			return;
		}
	}

	page = &dev->guest_pages[dev->nr_guest_pages++];
	page->guest_phys_addr = guest_phys_addr;
	page->host_phys_addr  = host_phys_addr;
	page->size = size;
}

static void
add_guest_pages(struct virtio_net *dev, struct rte_vhost_mem_region *reg,
		uint64_t page_size)
{
	uint64_t reg_size = reg->size;
	uint64_t host_user_addr  = reg->host_user_addr;
	uint64_t guest_phys_addr = reg->guest_phys_addr;
	uint64_t host_phys_addr;
	uint64_t size;

	host_phys_addr = rte_mem_virt2phy((void *)(uintptr_t)host_user_addr);
	size = page_size - (guest_phys_addr & (page_size - 1));
	size = RTE_MIN(size, reg_size);

	add_one_guest_page(dev, guest_phys_addr, host_phys_addr, size);
	host_user_addr  += size;
	guest_phys_addr += size;
	reg_size -= size;

	while (reg_size > 0) {
		size = RTE_MIN(reg_size, page_size);
		host_phys_addr = rte_mem_virt2phy((void *)(uintptr_t)
						  host_user_addr);
		add_one_guest_page(dev, guest_phys_addr, host_phys_addr, size);

		host_user_addr  += size;
		guest_phys_addr += size;
		reg_size -= size;
	}
}

#ifdef RTE_LIBRTE_VHOST_DEBUG
/* TODO: enable it only in debug mode? */
static void
dump_guest_pages(struct virtio_net *dev)
{
	uint32_t i;
	struct guest_page *page;

	for (i = 0; i < dev->nr_guest_pages; i++) {
		page = &dev->guest_pages[i];

		RTE_LOG(INFO, VHOST_CONFIG,
			"guest physical page region %u\n"
			"\t guest_phys_addr: %" PRIx64 "\n"
			"\t host_phys_addr : %" PRIx64 "\n"
			"\t size           : %" PRIx64 "\n",
			i,
			page->guest_phys_addr,
			page->host_phys_addr,
			page->size);
	}
}
#else
#define dump_guest_pages(dev)
#endif

static int
vhost_user_set_mem_table(struct virtio_net *dev, struct VhostUserMsg *pmsg)
{
	uint32_t i;

	if (dev->has_new_mem_table) {
		/*
		 * The previous mem table was not consumed, so close the
		 *  file descriptors from that mem table before copying
		 *  the new one.
		 */
		for (i = 0; i < dev->mem_table.nregions; i++) {
			close(dev->mem_table_fds[i]);
		}
	}

	memcpy(&dev->mem_table, &pmsg->payload.memory, sizeof(dev->mem_table));
	memcpy(dev->mem_table_fds, pmsg->fds, sizeof(dev->mem_table_fds));
	dev->has_new_mem_table = 1;
	/* vhost-user-nvme will not send
	 * set vring addr message, enable
	 * memory address table now.
	 */
	if (dev->has_new_mem_table && dev->is_nvme) {
		vhost_setup_mem_table(dev);
		dev->has_new_mem_table = 0;
	}

	return 0;
}

 static int
vhost_setup_mem_table(struct virtio_net *dev)
{
	struct VhostUserMemory memory = dev->mem_table;
	struct rte_vhost_mem_region *reg;
	struct vhost_virtqueue *vq;
	void *mmap_addr;
	uint64_t mmap_size;
	uint64_t mmap_offset;
	uint64_t alignment;
	uint32_t i;
	int fd;

	if (dev->mem) {
		free_mem_region(dev);
		rte_free(dev->mem);
		dev->mem = NULL;
	}

	for (i = 0; i < dev->nr_vring; i++) {
		vq = dev->virtqueue[i];
		/* Those addresses won't be valid anymore in host address space
		 * after setting new mem table. Initiator need to resend these
		 * addresses.
		 */
		vq->desc = NULL;
		vq->avail = NULL;
		vq->used = NULL;
	}

	dev->nr_guest_pages = 0;
	if (!dev->guest_pages) {
		dev->max_guest_pages = 8;
		dev->guest_pages = malloc(dev->max_guest_pages *
						sizeof(struct guest_page));
	}

	dev->mem = rte_zmalloc("vhost-mem-table", sizeof(struct rte_vhost_memory) +
		sizeof(struct rte_vhost_mem_region) * memory.nregions, 0);
	if (dev->mem == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"(%d) failed to allocate memory for dev->mem\n",
			dev->vid);
		return -1;
	}
	dev->mem->nregions = memory.nregions;

	for (i = 0; i < memory.nregions; i++) {
		fd  = dev->mem_table_fds[i];
		reg = &dev->mem->regions[i];

		reg->guest_phys_addr = memory.regions[i].guest_phys_addr;
		reg->guest_user_addr = memory.regions[i].userspace_addr;
		reg->size            = memory.regions[i].memory_size;
		reg->fd              = fd;

		mmap_offset = memory.regions[i].mmap_offset;
		mmap_size   = reg->size + mmap_offset;

		/* mmap() without flag of MAP_ANONYMOUS, should be called
		 * with length argument aligned with hugepagesz at older
		 * longterm version Linux, like 2.6.32 and 3.2.72, or
		 * mmap() will fail with EINVAL.
		 *
		 * to avoid failure, make sure in caller to keep length
		 * aligned.
		 */
		alignment = get_blk_size(fd);
		if (alignment == (uint64_t)-1) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"couldn't get hugepage size through fstat\n");
			goto err_mmap;
		}
		mmap_size = RTE_ALIGN_CEIL(mmap_size, alignment);

		mmap_addr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_POPULATE, fd, 0);

		if (mmap_addr == MAP_FAILED) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"mmap region %u failed.\n", i);
			goto err_mmap;
		}

		if (madvise(mmap_addr, mmap_size, MADV_DONTDUMP) != 0) {
			RTE_LOG(INFO, VHOST_CONFIG,
				"MADV_DONTDUMP advice setting failed.\n");
		}

		reg->mmap_addr = mmap_addr;
		reg->mmap_size = mmap_size;
		reg->host_user_addr = (uint64_t)(uintptr_t)mmap_addr +
				      mmap_offset;

		if (dev->dequeue_zero_copy)
			add_guest_pages(dev, reg, alignment);

		RTE_LOG(INFO, VHOST_CONFIG,
			"guest memory region %u, size: 0x%" PRIx64 "\n"
			"\t guest physical addr: 0x%" PRIx64 "\n"
			"\t guest virtual  addr: 0x%" PRIx64 "\n"
			"\t host  virtual  addr: 0x%" PRIx64 "\n"
			"\t mmap addr : 0x%" PRIx64 "\n"
			"\t mmap size : 0x%" PRIx64 "\n"
			"\t mmap align: 0x%" PRIx64 "\n"
			"\t mmap off  : 0x%" PRIx64 "\n",
			i, reg->size,
			reg->guest_phys_addr,
			reg->guest_user_addr,
			reg->host_user_addr,
			(uint64_t)(uintptr_t)mmap_addr,
			mmap_size,
			alignment,
			mmap_offset);
	}

	dump_guest_pages(dev);

	return 0;

err_mmap:
	free_mem_region(dev);
	rte_free(dev->mem);
	dev->mem = NULL;
	return -1;
}

static int
vq_is_ready(struct vhost_virtqueue *vq)
{
	return vq && vq->desc   &&
	       vq->kickfd != VIRTIO_UNINITIALIZED_EVENTFD &&
	       vq->callfd != VIRTIO_UNINITIALIZED_EVENTFD &&
	       vq->kickfd != VIRTIO_INVALID_EVENTFD &&
	       vq->callfd != VIRTIO_INVALID_EVENTFD;
}

static int
virtio_is_ready(struct virtio_net *dev)
{
	struct vhost_virtqueue *vq;
	uint32_t i;

	if (dev->nr_vring == 0)
		return 0;

	for (i = 0; i < dev->nr_vring; i++) {
		vq = dev->virtqueue[i];

		if (vq_is_ready(vq)) {
			RTE_LOG(INFO, VHOST_CONFIG,
				"virtio is now ready for processing.\n");
			return 1;
		}
	}

	return 0;
}

static void
vhost_user_set_vring_call(struct virtio_net *dev, struct VhostUserMsg *pmsg)
{
	struct vhost_vring_file file;
	struct vhost_virtqueue *vq;

	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	file.index = pmsg->payload.u64 & VHOST_USER_VRING_IDX_MASK;
	if (pmsg->payload.u64 & VHOST_USER_VRING_NOFD_MASK)
		file.fd = VIRTIO_INVALID_EVENTFD;
	else
		file.fd = pmsg->fds[0];
	RTE_LOG(INFO, VHOST_CONFIG,
		"vring call idx:%d file:%d\n", file.index, file.fd);

	vq = dev->virtqueue[file.index];
	if (vq->callfd >= 0)
		close(vq->callfd);

	vq->callfd = file.fd;
}

static void
vhost_user_set_vring_kick(struct virtio_net *dev, struct VhostUserMsg *pmsg)
{
	struct vhost_vring_file file;
	struct vhost_virtqueue *vq;

	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	file.index = pmsg->payload.u64 & VHOST_USER_VRING_IDX_MASK;
	if (pmsg->payload.u64 & VHOST_USER_VRING_NOFD_MASK)
		file.fd = VIRTIO_INVALID_EVENTFD;
	else
		file.fd = pmsg->fds[0];
	RTE_LOG(INFO, VHOST_CONFIG,
		"vring kick idx:%d file:%d\n", file.index, file.fd);

	vq = dev->virtqueue[file.index];
	if (vq->kickfd >= 0)
		close(vq->kickfd);
	vq->kickfd = file.fd;
}

static void
free_zmbufs(struct vhost_virtqueue *vq)
{
	struct zcopy_mbuf *zmbuf, *next;

	for (zmbuf = TAILQ_FIRST(&vq->zmbuf_list);
	     zmbuf != NULL; zmbuf = next) {
		next = TAILQ_NEXT(zmbuf, next);

		rte_pktmbuf_free(zmbuf->mbuf);
		TAILQ_REMOVE(&vq->zmbuf_list, zmbuf, next);
	}

	rte_free(vq->zmbufs);
}

/*
 * when virtio is stopped, qemu will send us the GET_VRING_BASE message.
 */
static int
vhost_user_get_vring_base(struct virtio_net *dev,
			  VhostUserMsg *msg)
{
	struct vhost_virtqueue *vq = dev->virtqueue[msg->payload.state.index];

	/* We have to stop the queue (virtio) if it is running. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	dev->flags &= ~VIRTIO_DEV_READY;

	/* Here we are safe to get the last used index */
	msg->payload.state.num = vq->last_used_idx;

	RTE_LOG(INFO, VHOST_CONFIG,
		"vring base idx:%d file:%d\n", msg->payload.state.index, msg->payload.state.num);
	/*
	 * Based on current qemu vhost-user implementation, this message is
	 * sent and only sent in vhost_vring_stop.
	 * TODO: cleanup the vring, it isn't usable since here.
	 */
	if (vq->kickfd >= 0)
		close(vq->kickfd);

	vq->kickfd = VIRTIO_UNINITIALIZED_EVENTFD;

	if (vq->callfd >= 0)
		close(vq->callfd);

	vq->callfd = VIRTIO_UNINITIALIZED_EVENTFD;

	if (dev->dequeue_zero_copy)
		free_zmbufs(vq);
	rte_free(vq->shadow_used_ring);
	vq->shadow_used_ring = NULL;

	return 0;
}

/*
 * when virtio queues are ready to work, qemu will send us to
 * enable the virtio queue pair.
 */
static int
vhost_user_set_vring_enable(struct virtio_net *dev,
			    VhostUserMsg *msg)
{
	int enable = (int)msg->payload.state.num;

	RTE_LOG(INFO, VHOST_CONFIG,
		"set queue enable: %d to qp idx: %d\n",
		enable, msg->payload.state.index);

	if (dev->notify_ops->vring_state_changed)
		dev->notify_ops->vring_state_changed(dev->vid, msg->payload.state.index, enable);

	dev->virtqueue[msg->payload.state.index]->enabled = enable;

	return 0;
}

static void
vhost_user_set_protocol_features(struct virtio_net *dev,
				 uint64_t protocol_features)
{
	if (protocol_features & ~VHOST_USER_PROTOCOL_FEATURES)
		return;

	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	dev->protocol_features = protocol_features;
}

static int
vhost_user_set_log_base(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	int fd = msg->fds[0];
	uint64_t size, off;
	void *addr;

	if (fd < 0) {
		RTE_LOG(ERR, VHOST_CONFIG, "invalid log fd: %d\n", fd);
		return -1;
	}

	if (msg->size != sizeof(VhostUserLog)) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"invalid log base msg size: %"PRId32" != %d\n",
			msg->size, (int)sizeof(VhostUserLog));
		return -1;
	}

	/* Remove from the data plane. */
	if (dev->flags & VIRTIO_DEV_RUNNING) {
		dev->flags &= ~VIRTIO_DEV_RUNNING;
		dev->notify_ops->destroy_device(dev->vid);
	}

	size = msg->payload.log.mmap_size;
	off  = msg->payload.log.mmap_offset;
	RTE_LOG(INFO, VHOST_CONFIG,
		"log mmap size: %"PRId64", offset: %"PRId64"\n",
		size, off);

	/*
	 * mmap from 0 to workaround a hugepage mmap bug: mmap will
	 * fail when offset is not page size aligned.
	 */
	addr = mmap(0, size + off, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED) {
		RTE_LOG(ERR, VHOST_CONFIG, "mmap log base failed!\n");
		return -1;
	}

	/*
	 * Free previously mapped log memory on occasionally
	 * multiple VHOST_USER_SET_LOG_BASE.
	 */
	if (dev->log_addr) {
		munmap((void *)(uintptr_t)dev->log_addr, dev->log_size);
	}
	dev->log_addr = (uint64_t)(uintptr_t)addr;
	dev->log_base = dev->log_addr + off;
	dev->log_size = size;

	return 0;
}

/*
 * An rarp packet is constructed and broadcasted to notify switches about
 * the new location of the migrated VM, so that packets from outside will
 * not be lost after migration.
 *
 * However, we don't actually "send" a rarp packet here, instead, we set
 * a flag 'broadcast_rarp' to let rte_vhost_dequeue_burst() inject it.
 */
static int
vhost_user_send_rarp(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	uint8_t *mac = (uint8_t *)&msg->payload.u64;

	RTE_LOG(DEBUG, VHOST_CONFIG,
		":: mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	memcpy(dev->mac.addr_bytes, mac, 6);

	/*
	 * Set the flag to inject a RARP broadcast packet at
	 * rte_vhost_dequeue_burst().
	 *
	 * rte_smp_wmb() is for making sure the mac is copied
	 * before the flag is set.
	 */
	rte_smp_wmb();
	rte_atomic16_set(&dev->broadcast_rarp, 1);

	return 0;
}

static int
vhost_user_net_set_mtu(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	if (msg->payload.u64 < VIRTIO_MIN_MTU ||
			msg->payload.u64 > VIRTIO_MAX_MTU) {
		RTE_LOG(ERR, VHOST_CONFIG, "Invalid MTU size (%"PRIu64")\n",
				msg->payload.u64);

		return -1;
	}

	dev->mtu = msg->payload.u64;

	return 0;
}

/* return bytes# of read on success or negative val on failure. */
static int
read_vhost_message(int sockfd, struct VhostUserMsg *msg)
{
	int ret;

	ret = read_fd_message(sockfd, (char *)msg, VHOST_USER_HDR_SIZE,
		msg->fds, VHOST_MEMORY_MAX_NREGIONS);
	if (ret <= 0)
		return ret;

	if (msg && msg->size) {
		if (msg->size > sizeof(msg->payload)) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"invalid msg size: %d\n", msg->size);
			return -1;
		}
		ret = read(sockfd, &msg->payload, msg->size);
		if (ret <= 0)
			return ret;
		if (ret != (int)msg->size) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"read control message failed\n");
			return -1;
		}
	}

	return ret;
}

static int
send_vhost_message(int sockfd, struct VhostUserMsg *msg)
{
	int ret;

	if (!msg)
		return 0;

	msg->flags &= ~VHOST_USER_VERSION_MASK;
	msg->flags &= ~VHOST_USER_NEED_REPLY;
	msg->flags |= VHOST_USER_VERSION;
	msg->flags |= VHOST_USER_REPLY_MASK;

	ret = send_fd_message(sockfd, (char *)msg,
		VHOST_USER_HDR_SIZE + msg->size, NULL, 0);

	return ret;
}

/*
 * Allocate a queue pair if it hasn't been allocated yet
 */
static int
vhost_user_check_and_alloc_queue_pair(struct virtio_net *dev, VhostUserMsg *msg)
{
	uint16_t vring_idx;

	switch (msg->request) {
	case VHOST_USER_SET_VRING_KICK:
	case VHOST_USER_SET_VRING_CALL:
	case VHOST_USER_SET_VRING_ERR:
		vring_idx = msg->payload.u64 & VHOST_USER_VRING_IDX_MASK;
		break;
	case VHOST_USER_SET_VRING_NUM:
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ENABLE:
		vring_idx = msg->payload.state.index;
		break;
	case VHOST_USER_SET_VRING_ADDR:
		vring_idx = msg->payload.addr.index;
		break;
	default:
		return 0;
	}

	if (vring_idx >= VHOST_MAX_VRING) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"invalid vring index: %u\n", vring_idx);
		return -1;
	}

	if (dev->virtqueue[vring_idx])
		return 0;

	return alloc_vring_queue(dev, vring_idx);
}

static int
vhost_user_nvme_admin_passthrough(struct virtio_net *dev,
				  void *cmd, void *cqe, void *buf)
{
	if (dev->notify_ops->vhost_nvme_admin_passthrough) {
		return dev->notify_ops->vhost_nvme_admin_passthrough(dev->vid, cmd, cqe, buf);
	}

	return -1;
}

static int
vhost_user_nvme_set_cq_call(struct virtio_net *dev, uint16_t qid, int fd)
{
	if (dev->notify_ops->vhost_nvme_set_cq_call) {
		return dev->notify_ops->vhost_nvme_set_cq_call(dev->vid, qid, fd);
	}

	return -1;
}

static int
vhost_user_nvme_get_cap(struct virtio_net *dev, uint64_t *cap)
{
	if (dev->notify_ops->vhost_nvme_get_cap) {
		return dev->notify_ops->vhost_nvme_get_cap(dev->vid, cap);
	}

	return -1;
}

static int
vhost_user_nvme_set_bar_mr(struct virtio_net *dev, struct VhostUserMsg *pmsg)
{
	struct VhostUserMemory mem_table;
	int fd = pmsg->fds[0];
	void *mmap_addr;
	uint64_t mmap_size;
	uint64_t mmap_offset;
	uint64_t alignment;
	struct rte_vhost_mem_region reg;
	int ret = 0;

	memcpy(&mem_table, &pmsg->payload.memory, sizeof(mem_table));

	reg.guest_phys_addr = mem_table.regions[0].guest_phys_addr;
	reg.guest_user_addr = mem_table.regions[0].userspace_addr;
	reg.size            = mem_table.regions[0].memory_size;
	reg.fd              = fd;
	mmap_offset = mem_table.regions[0].mmap_offset;
	mmap_size   = reg.size + mmap_offset;

	alignment = get_blk_size(fd);
	if (alignment == (uint64_t)-1) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"couldn't get hugepage size through fstat\n");
			return -1;
	}
	mmap_size = RTE_ALIGN_CEIL(mmap_size, alignment);

	mmap_addr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, fd, 0);

	if (mmap_addr == MAP_FAILED) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"mmap region failed.\n");
		return -1;
	}

	if (madvise(mmap_addr, mmap_size, MADV_DONTDUMP) != 0) {
		RTE_LOG(INFO, VHOST_CONFIG,
			"MADV_DONTDUMP advice setting failed.\n");
	}

	reg.mmap_addr = mmap_addr;
	reg.mmap_size = mmap_size;
	reg.host_user_addr = (uint64_t)(uintptr_t)mmap_addr +
				      mmap_offset;

	RTE_LOG(INFO, VHOST_CONFIG,
			"BAR memory region %u, size: 0x%" PRIx64 "\n"
			"\t guest physical addr: 0x%" PRIx64 "\n"
			"\t guest virtual  addr: 0x%" PRIx64 "\n"
			"\t host  virtual  addr: 0x%" PRIx64 "\n"
			"\t mmap addr : 0x%" PRIx64 "\n"
			"\t mmap size : 0x%" PRIx64 "\n"
			"\t mmap align: 0x%" PRIx64 "\n"
			"\t mmap off  : 0x%" PRIx64 "\n",
			0, reg.size,
			reg.guest_phys_addr,
			reg.guest_user_addr,
			reg.host_user_addr,
			(uint64_t)(uintptr_t)mmap_addr,
			mmap_size,
			alignment,
			mmap_offset);

	if (dev->bar_addr) {
		munmap((void *)(uintptr_t)dev->bar_addr, dev->bar_size);
	}
	dev->bar_addr = (void *)(uintptr_t)reg.host_user_addr;
	dev->bar_size = reg.mmap_size;

	if (dev->notify_ops->vhost_nvme_set_bar_mr) {
		ret = dev->notify_ops->vhost_nvme_set_bar_mr(dev->vid, dev->bar_addr, dev->bar_size);
		if (ret) {
			munmap((void *)(uintptr_t)dev->bar_addr, dev->bar_size);
			dev->bar_addr = NULL;
			dev->bar_size = 0;
		}
	}

	return ret;
}

int
vhost_user_msg_handler(int vid, int fd)
{
	struct virtio_net *dev;
	struct VhostUserMsg msg;
	struct vhost_vring_file file;
	int ret;
	uint64_t cap;
	uint64_t enable;
	uint8_t cqe[16];
	uint8_t cmd[64];
	uint8_t buf[4096];

	dev = get_device(vid);
	if (dev == NULL)
		return -1;

	ret = read_vhost_message(fd, &msg);
	if (ret <= 0 || msg.request >= VHOST_USER_MAX) {
		if (ret < 0)
			RTE_LOG(ERR, VHOST_CONFIG,
				"vhost read message failed\n");
		else if (ret == 0)
			RTE_LOG(INFO, VHOST_CONFIG,
				"vhost peer closed\n");
		else
			RTE_LOG(ERR, VHOST_CONFIG,
				"vhost read incorrect message\n");

		return -1;
	}

	RTE_LOG(INFO, VHOST_CONFIG, "%s: read message %s\n",
		dev->ifname, vhost_message_str[msg.request]);

	ret = vhost_user_check_and_alloc_queue_pair(dev, &msg);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"failed to alloc queue\n");
		return -1;
	}

	switch (msg.request) {
	case VHOST_USER_GET_CONFIG:
		if (dev->notify_ops->get_config(dev->vid,
						msg.payload.config.region,
						msg.payload.config.size) != 0) {
			msg.size = sizeof(uint64_t);
		}
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_SET_CONFIG:
		if ((dev->notify_ops->set_config(dev->vid,
						msg.payload.config.region,
						msg.payload.config.offset,
						msg.payload.config.size,
						msg.payload.config.flags)) != 0) {
			ret = 1;
		} else {
			ret = 0;
		}
		break;
	case VHOST_USER_NVME_ADMIN:
		if (!dev->is_nvme) {
			dev->is_nvme = 1;
		}
		memcpy(cmd, msg.payload.nvme.cmd.req, sizeof(cmd));
		ret = vhost_user_nvme_admin_passthrough(dev, cmd, cqe, buf);
		memcpy(msg.payload.nvme.cmd.cqe, cqe, sizeof(cqe));
		msg.size = sizeof(cqe);
		/* NVMe Identify Command */
		if (cmd[0] == 0x06) {
			memcpy(msg.payload.nvme.buf, &buf, 4096);
			msg.size += 4096;
		}
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_NVME_SET_CQ_CALL:
		file.index = msg.payload.u64 & VHOST_USER_VRING_IDX_MASK;
		file.fd = msg.fds[0];
		ret = vhost_user_nvme_set_cq_call(dev, file.index, file.fd);
		break;
	case VHOST_USER_NVME_GET_CAP:
		ret = vhost_user_nvme_get_cap(dev, &cap);
		if (!ret)
			msg.payload.u64 = cap;
		else
			msg.payload.u64 = 0;
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_NVME_START_STOP:
		enable = msg.payload.u64;
		/* device must be started before set cq call */
		if (enable) {
			if (!(dev->flags & VIRTIO_DEV_RUNNING)) {
				if (dev->notify_ops->new_device(dev->vid) == 0)
					dev->flags |= VIRTIO_DEV_RUNNING;
			}
		} else {
			if (dev->flags & VIRTIO_DEV_RUNNING) {
				dev->flags &= ~VIRTIO_DEV_RUNNING;
				dev->notify_ops->destroy_device(dev->vid);
			}
		}
		break;
	case VHOST_USER_NVME_SET_BAR_MR:
		ret = vhost_user_nvme_set_bar_mr(dev, &msg);
		break;
	case VHOST_USER_GET_FEATURES:
		msg.payload.u64 = vhost_user_get_features(dev);
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_SET_FEATURES:
		vhost_user_set_features(dev, msg.payload.u64);
		break;

	case VHOST_USER_GET_PROTOCOL_FEATURES:
		msg.payload.u64 = VHOST_USER_PROTOCOL_FEATURES;
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_SET_PROTOCOL_FEATURES:
		vhost_user_set_protocol_features(dev, msg.payload.u64);
		break;

	case VHOST_USER_SET_OWNER:
		vhost_user_set_owner();
		break;
	case VHOST_USER_RESET_OWNER:
		vhost_user_reset_owner(dev);
		break;

	case VHOST_USER_SET_MEM_TABLE:
		ret = vhost_user_set_mem_table(dev, &msg);
		break;

	case VHOST_USER_SET_LOG_BASE:
		vhost_user_set_log_base(dev, &msg);

		/* it needs a reply */
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
		break;
	case VHOST_USER_SET_LOG_FD:
		close(msg.fds[0]);
		RTE_LOG(INFO, VHOST_CONFIG, "not implemented.\n");
		break;

	case VHOST_USER_SET_VRING_NUM:
		vhost_user_set_vring_num(dev, &msg);
		break;
	case VHOST_USER_SET_VRING_ADDR:
		vhost_user_set_vring_addr(dev, &msg);
		break;
	case VHOST_USER_SET_VRING_BASE:
		vhost_user_set_vring_base(dev, &msg);
		break;

	case VHOST_USER_GET_VRING_BASE:
		vhost_user_get_vring_base(dev, &msg);
		msg.size = sizeof(msg.payload.state);
		send_vhost_message(fd, &msg);
		break;

	case VHOST_USER_SET_VRING_KICK:
		vhost_user_set_vring_kick(dev, &msg);
		break;
	case VHOST_USER_SET_VRING_CALL:
		vhost_user_set_vring_call(dev, &msg);
		break;

	case VHOST_USER_SET_VRING_ERR:
		if (!(msg.payload.u64 & VHOST_USER_VRING_NOFD_MASK))
			close(msg.fds[0]);
		RTE_LOG(INFO, VHOST_CONFIG, "not implemented\n");
		break;

	case VHOST_USER_GET_QUEUE_NUM:
		msg.payload.u64 = VHOST_MAX_QUEUE_PAIRS;
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
		break;

	case VHOST_USER_SET_VRING_ENABLE:
		vhost_user_set_vring_enable(dev, &msg);
		break;
	case VHOST_USER_SEND_RARP:
		vhost_user_send_rarp(dev, &msg);
		break;

	case VHOST_USER_NET_SET_MTU:
		ret = vhost_user_net_set_mtu(dev, &msg);
		break;

	default:
		ret = -1;
		break;

	}

	if (msg.flags & VHOST_USER_NEED_REPLY) {
		msg.payload.u64 = !!ret;
		msg.size = sizeof(msg.payload.u64);
		send_vhost_message(fd, &msg);
	}

	if (!(dev->flags & VIRTIO_DEV_RUNNING) && virtio_is_ready(dev)) {
		dev->flags |= VIRTIO_DEV_READY;

		if (!(dev->flags & VIRTIO_DEV_RUNNING)) {
			if (dev->dequeue_zero_copy) {
				RTE_LOG(INFO, VHOST_CONFIG,
						"dequeue zero copy is enabled\n");
			}

			if (dev->notify_ops->new_device(dev->vid) == 0)
				dev->flags |= VIRTIO_DEV_RUNNING;
		}
	}

	return 0;
}
