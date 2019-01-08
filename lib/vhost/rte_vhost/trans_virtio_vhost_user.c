/* SPDX-License-Idenitifier: BSD-3-Clause
 * Copyright (C) 2018 Red Hat, Inc.
 */

/*
 * @file
 * virtio-vhost-user PCI transport driver
 *
 * This vhost-user transport communicates with the vhost-user master process
 * over the virtio-vhost-user PCI device.
 *
 * Interrupts are used since this is the control path, not the data path.  This
 * way the vhost-user command processing doesn't interfere with packet
 * processing.  This is similar to the AF_UNIX transport's fdman thread that
 * processes socket I/O separately.
 *
 * This transport replaces the usual vhost-user file descriptor passing with a
 * PCI BAR that contains doorbell registers for callfd and logfd, and shared
 * memory for the memory table regions.
 *
 * VIRTIO device specification:
 * https://stefanha.github.io/virtio/vhost-user-slave.html#x1-2830007
 */

#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_bus_pci.h>
#include <rte_io.h>

#include "vhost.h"
#include "virtio_pci.h"
#include "virtqueue.h"
#include "virtio_vhost_user.h"
#include "vhost_user.h"

/*
 * Data structures:
 *
 * Successfully probed virtio-vhost-user PCI adapters are added to
 * vvu_pci_device_list as struct vvu_pci_device elements.
 *
 * When rte_vhost_driver_register() is called, a struct vvu_socket is created
 * as the endpoint for future vhost-user connections.  The struct vvu_socket is
 * associated with the struct vvu_pci_device that will be used for
 * communication.
 *
 * When a vhost-user protocol connection is established, a struct
 * vvu_connection is created and the application's new_device(int vid) callback
 * is invoked.
 */

/** Probed PCI devices for lookup by rte_vhost_driver_register() */
TAILQ_HEAD(, vvu_pci_device) vvu_pci_device_list =
	TAILQ_HEAD_INITIALIZER(vvu_pci_device_list);

struct vvu_socket;
struct vvu_connection;

/** A virtio-vhost-vsock PCI adapter */
struct vvu_pci_device {
	struct virtio_hw hw;
	struct spdk_pci_device *pci_dev;
	struct vvu_socket *s;
	TAILQ_ENTRY(vvu_pci_device) next;
};

/** A vhost-user endpoint (aka per-path state) */
struct vvu_socket {
	struct vhost_user_socket socket; /* must be first field! */
	struct vvu_pci_device *pdev;
	struct vvu_connection *conn;

	/** Doorbell registers */
	uint16_t *doorbells;

	/** This struct virtio_vhost_user_config field determines the number of
	 * doorbells available so we keep it saved.
	 */
	uint32_t max_vhost_queues;

	/** Receive buffers */
	const struct rte_memzone *rxbuf_mz;

	/** Transmit buffers.  It is assumed that the device completes them
	 * in-order so a single wrapping index can be used to select the next
	 * free buffer.
	 */
	const struct rte_memzone *txbuf_mz;
	unsigned int txbuf_idx;
};

/** A vhost-user protocol session (aka per-vid state) */
struct vvu_connection {
	struct virtio_net device; /* must be first field! */
	struct vvu_socket *s;
};

/** Virtio feature bits that we support */
#define VVU_VIRTIO_FEATURES ((1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
			     (1ULL << VIRTIO_F_ANY_LAYOUT) | \
			     (1ULL << VIRTIO_F_VERSION_1) | \
			     (1ULL << VIRTIO_F_IOMMU_PLATFORM))

/** Virtqueue indices */
enum {
	VVU_VQ_RX,
	VVU_VQ_TX,
	VVU_VQ_MAX,
};

enum {
	/** Receive buffer size, in bytes */
	VVU_RXBUF_SIZE = 1024,

	/** Transmit buffer size, in bytes */
	VVU_TXBUF_SIZE = 1024,
};

/** Look up a struct vvu_pci_device from a DomBDF string */
static struct vvu_pci_device *
vvu_pci_by_name(const char *name)
{
	struct vvu_pci_device *pdev;

	TAILQ_FOREACH(pdev, &vvu_pci_device_list, next) {
		struct rte_pci_device *dev = pdev->pci_dev->dev_handle;
		if (!strcmp(dev->device.name, name))
			return pdev;
	}
	return NULL;
}

/** Start connection establishment */
static void
vvu_connect(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	uint32_t status;

	virtio_pci_read_dev_config(hw,
			offsetof(struct virtio_vhost_user_config, status),
			&status, sizeof(status));
	status |= RTE_LE32(1u << VIRTIO_VHOST_USER_STATUS_SLAVE_UP);
	virtio_pci_write_dev_config(hw,
			offsetof(struct virtio_vhost_user_config, status),
			&status, sizeof(status));
}

static void
vvu_disconnect(struct vvu_socket *s)
{
	struct vhost_user_socket *vsocket = &s->socket;
	struct vvu_connection *conn = s->conn;
	struct virtio_hw *hw = &s->pdev->hw;
	uint32_t status;

	if (conn) {
		int vid = conn->device.vid;
		vhost_destroy_device(conn->device.vid);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(vid);
	}

	/* Make sure we're disconnected */
	virtio_pci_read_dev_config(hw,
			offsetof(struct virtio_vhost_user_config, status),
			&status, sizeof(status));
	status &= ~RTE_LE32(1u << VIRTIO_VHOST_USER_STATUS_SLAVE_UP);
	virtio_pci_write_dev_config(hw,
			offsetof(struct virtio_vhost_user_config, status),
			&status, sizeof(status));
}

static void
vvu_reconnect(struct vvu_socket *s)
{
	vvu_disconnect(s);
	vvu_connect(s);
}

static void vvu_process_rxq(struct vvu_socket *s);

static void
vvu_cleanup_device(struct virtio_net *dev, int destroy __rte_unused)
{
	struct vvu_connection *conn =
		container_of(dev, struct vvu_connection, device);
	struct vvu_socket *s = conn->s;

	s->conn = NULL;
	vvu_process_rxq(s); /* discard old replies from master */
	vvu_reconnect(s);
}

static int
vvu_vring_call(struct virtio_net *dev, struct vhost_virtqueue *vq)
{
	struct vvu_connection *conn =
		container_of(dev, struct vvu_connection, device);
	struct vvu_socket *s = conn->s;
	uint16_t vq_idx = vq->vring_idx;

	RTE_LOG(DEBUG, VHOST_CONFIG, "%s vq_idx %u\n", __func__, vq_idx);

	rte_write16(rte_cpu_to_le_16(vq_idx), &s->doorbells[vq_idx]);
	return 0;
}

static int
vvu_send_reply(struct virtio_net *dev, struct VhostUserMsg *reply)
{
	struct vvu_connection *conn =
		container_of(dev, struct vvu_connection, device);
	struct vvu_socket *s = conn->s;
	struct virtqueue *vq = s->pdev->hw.vqs[VVU_VQ_TX];
	struct vring_desc *desc;
	struct vq_desc_extra *descx;
	unsigned int i;
	void *buf;
	size_t len;

	RTE_LOG(DEBUG, VHOST_CONFIG,
		"%s request %u flags %#x size %u\n",
		__func__, reply->request,
		reply->flags, reply->size);

	/* TODO convert reply to little-endian */

	if (virtqueue_full(vq)) {
		RTE_LOG(ERR, VHOST_CONFIG, "Out of tx buffers\n");
		return -1;
	}

	i = s->txbuf_idx;
	len = VHOST_USER_HDR_SIZE + reply->size;
	buf = (uint8_t *)s->txbuf_mz->addr + i * VVU_TXBUF_SIZE;

	memcpy(buf, reply, len);

	desc = &vq->vq_ring.desc[i];
	descx = &vq->vq_descx[i];

	desc->addr = rte_cpu_to_le_64(s->txbuf_mz->iova + i * VVU_TXBUF_SIZE);
	desc->len = rte_cpu_to_le_32(len);
	desc->flags = 0;

	descx->cookie = buf;
	descx->ndescs = 1;

	vq->vq_free_cnt--;
	s->txbuf_idx = (s->txbuf_idx + 1) & (vq->vq_nentries - 1);

	vq_update_avail_ring(vq, i);
	vq_update_avail_idx(vq);

	if (virtqueue_kick_prepare(vq))
		virtqueue_notify(vq);

	return 0;
}

static int
vvu_map_mem_regions(struct virtio_net *dev)
{
	struct vvu_connection *conn =
		container_of(dev, struct vvu_connection, device);
	struct vvu_socket *s = conn->s;
	struct rte_pci_device *pci_dev = s->pdev->pci_dev->dev_handle;
	uint8_t *mmap_addr;
	uint32_t i;

	/* Memory regions start after the doorbell registers */
	mmap_addr = (uint8_t *)pci_dev->mem_resource[2].addr +
		    RTE_ALIGN_CEIL((s->max_vhost_queues + 1 /* log fd */) *
				   sizeof(uint16_t), 4096);

	for (i = 0; i < dev->mem->nregions; i++) {
		struct rte_vhost_mem_region *reg = &dev->mem->regions[i];

		reg->mmap_addr = mmap_addr;
		reg->host_user_addr = (uint64_t)(uintptr_t)reg->mmap_addr +
				      reg->mmap_size - reg->size;

		mmap_addr += reg->mmap_size;
	}

	return 0;
}

static void
vvu_unmap_mem_regions(struct virtio_net *dev)
{
	uint32_t i;

	for (i = 0; i < dev->mem->nregions; i++) {
		struct rte_vhost_mem_region *reg = &dev->mem->regions[i];

		/* Just clear the pointers, the PCI BAR stays there */
		reg->mmap_addr = NULL;
		reg->host_user_addr = 0;
	}
}

static void vvu_process_new_connection(struct vvu_socket *s)
{
	struct vhost_user_socket *vsocket = &s->socket;
	struct vvu_connection *conn;
	struct virtio_net *dev;
	size_t size;

	dev = vhost_new_device(vsocket->trans_ops, vsocket->features);
	if (!dev) {
		vvu_reconnect(s);
		return;
	}

	conn = container_of(dev, struct vvu_connection, device);
	conn->s = s;

	size = strnlen(vsocket->path, PATH_MAX);
	vhost_set_ifname(dev->vid, vsocket->path, size);

	RTE_LOG(INFO, VHOST_CONFIG, "new device, handle is %d\n", dev->vid);

	if (vsocket->notify_ops->new_connection) {
		int ret = vsocket->notify_ops->new_connection(dev->vid);
		if (ret < 0) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"failed to add vhost user connection\n");
			vhost_destroy_device(dev->vid);
			vvu_reconnect(s);
			return;
		}
	}

	s->conn = conn;
	return;
}

static void vvu_process_status_change(struct vvu_socket *s, bool slave_up,
				      bool master_up)
{
	RTE_LOG(DEBUG, VHOST_CONFIG, "%s slave_up %d master_up %d\n",
		__func__, slave_up, master_up);

	/* Disconnected from the master, try reconnecting */
	if (!slave_up) {
		vvu_reconnect(s);
		return;
	}

	if (master_up && !s->conn) {
		vvu_process_new_connection(s);
		return;
	}
}

static void
vvu_process_txq(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	struct virtqueue *vq = hw->vqs[VVU_VQ_TX];
	uint16_t n = VIRTQUEUE_NUSED(vq);

	virtio_rmb();

	/* Just mark the buffers complete */
	vq->vq_used_cons_idx += n;
	vq->vq_free_cnt += n;
}

static void
vvu_process_rxq(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	struct virtqueue *vq = hw->vqs[VVU_VQ_RX];
	bool refilled = false;

	while (VIRTQUEUE_NUSED(vq)) {
		struct vring_used_elem *uep;
		VhostUserMsg *msg;
		uint32_t len;
		uint32_t desc_idx;
		uint16_t used_idx;
		size_t i;

		virtio_rmb();

		used_idx = (uint16_t)(vq->vq_used_cons_idx & (vq->vq_nentries - 1));
		uep = &vq->vq_ring.used->ring[used_idx];
		desc_idx = rte_le_to_cpu_32(uep->id);

		msg = vq->vq_descx[desc_idx].cookie;
		len = rte_le_to_cpu_32(uep->len);

		if (msg->size > sizeof(VhostUserMsg) ||
		    len != VHOST_USER_HDR_SIZE + msg->size) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"Invalid vhost-user message size %u, got %u bytes\n",
				msg->size, len);
			/* TODO reconnect */
			abort();
		}

		RTE_LOG(DEBUG, VHOST_CONFIG,
			"%s request %u flags %#x size %u\n",
			__func__, msg->request,
			msg->flags, msg->size);

		/* Mark file descriptors invalid */
		for (i = 0; i < RTE_DIM(msg->fds); i++)
			msg->fds[i] = VIRTIO_INVALID_EVENTFD;

		/* Only process messages while connected */
		if (s->conn) {
			if (vhost_user_msg_handler(s->conn->device.vid,
						   msg) < 0) {
				/* TODO reconnect */
				abort();
			}
		}

		vq->vq_used_cons_idx++;

		/* Refill rxq */
		vq_update_avail_ring(vq, desc_idx);
		vq_update_avail_idx(vq);
		refilled = true;
	}

	if (!refilled)
		return;
	if (virtqueue_kick_prepare(vq))
		virtqueue_notify(vq);
}

/* TODO Audit thread safety.  There are 3 threads involved:
 * 1. The main process thread that calls librte_vhost APIs during startup.
 * 2. The interrupt thread that calls vvu_interrupt_handler().
 * 3. Packet processing threads (lcores) calling librte_vhost APIs.
 *
 * It may be necessary to use locks if any of these code paths can race.  The
 * librte_vhost API entry points already do some locking but this needs to be
 * checked.
 */
static void
vvu_interrupt_handler(void *cb_arg)
{
	struct vvu_socket *s = cb_arg;
	struct virtio_hw *hw = &s->pdev->hw;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;
	struct rte_intr_handle *intr_handle = &dev->intr_handle;
	uint8_t isr;

	/* Read Interrupt Status Register (which also clears it) */
	isr = VTPCI_OPS(hw)->get_isr(hw);

	if (isr & VIRTIO_PCI_ISR_CONFIG) {
		uint32_t status;
		bool slave_up;
		bool master_up;

		virtio_pci_read_dev_config(hw,
				offsetof(struct virtio_vhost_user_config, status),
				&status, sizeof(status));
		status = rte_le_to_cpu_32(status);

		RTE_LOG(DEBUG, VHOST_CONFIG, "%s isr %#x status %#x\n", __func__, isr, status);

		slave_up = status & (1u << VIRTIO_VHOST_USER_STATUS_SLAVE_UP);
		master_up = status & (1u << VIRTIO_VHOST_USER_STATUS_MASTER_UP);
		vvu_process_status_change(s, slave_up, master_up);
	} else
		RTE_LOG(DEBUG, VHOST_CONFIG, "%s isr %#x\n", __func__, isr);

	/* Re-arm before processing virtqueues so no interrupts are lost */
	rte_intr_enable(intr_handle);

	vvu_process_txq(s);
	vvu_process_rxq(s);
}

static int
vvu_virtio_pci_init_rxq(struct vvu_socket *s)
{
	char name[sizeof("0000:00:00.00 vq 0 rxbufs")];
	struct virtqueue *vq;
	size_t size;
	size_t align;
	int i;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;

	vq = s->pdev->hw.vqs[VVU_VQ_RX];

	snprintf(name, sizeof(name), "%s vq %u rxbufs",
		 dev->device.name, VVU_VQ_RX);

	/* Allocate more than sizeof(VhostUserMsg) so there is room to grow */
	size = vq->vq_nentries * VVU_RXBUF_SIZE;
	align = 1024;
	s->rxbuf_mz = rte_memzone_reserve_aligned(name, size, SOCKET_ID_ANY,
						  0, align);
	if (!s->rxbuf_mz) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocate rxbuf memzone\n");
		return -1;
	}

	for (i = 0; i < vq->vq_nentries; i++) {
		struct vring_desc *desc = &vq->vq_ring.desc[i];
		struct vq_desc_extra *descx = &vq->vq_descx[i];

		desc->addr = rte_cpu_to_le_64(s->rxbuf_mz->iova +
				              i * VVU_RXBUF_SIZE);
		desc->len = RTE_LE32(VVU_RXBUF_SIZE);
		desc->flags = RTE_LE16(VRING_DESC_F_WRITE);

		descx->cookie = (uint8_t *)s->rxbuf_mz->addr + i * VVU_RXBUF_SIZE;
		descx->ndescs = 1;

		vq_update_avail_ring(vq, i);
		vq->vq_free_cnt--;
	}

	vq_update_avail_idx(vq);
	virtqueue_notify(vq);
	return 0;
}

static int
vvu_virtio_pci_init_txq(struct vvu_socket *s)
{
	char name[sizeof("0000:00:00.00 vq 0 txbufs")];
	struct virtqueue *vq;
	size_t size;
	size_t align;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;

	vq = s->pdev->hw.vqs[VVU_VQ_TX];

	snprintf(name, sizeof(name), "%s vq %u txbufs",
		 dev->device.name, VVU_VQ_TX);

	/* Allocate more than sizeof(VhostUserMsg) so there is room to grow */
	size = vq->vq_nentries * VVU_TXBUF_SIZE;
	align = 1024;
	s->txbuf_mz = rte_memzone_reserve_aligned(name, size, SOCKET_ID_ANY,
						  0, align);
	if (!s->txbuf_mz) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocate txbuf memzone\n");
		return -1;
	}

	s->txbuf_idx = 0;
	return 0;
}

static void
virtio_init_vring(struct virtqueue *vq)
{
	int size = vq->vq_nentries;
	struct vring *vr = &vq->vq_ring;
	uint8_t *ring_mem = vq->vq_ring_virt_mem;

	memset(ring_mem, 0, vq->vq_ring_size);
	vring_init(vr, size, ring_mem, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_used_cons_idx = 0;
	vq->vq_desc_head_idx = 0;
	vq->vq_avail_idx = 0;
	vq->vq_desc_tail_idx = (uint16_t)(vq->vq_nentries - 1);
	vq->vq_free_cnt = vq->vq_nentries;
	memset(vq->vq_descx, 0, sizeof(struct vq_desc_extra) * vq->vq_nentries);

	vring_desc_init(vr->desc, size);
	virtqueue_enable_intr(vq);
}

static int
vvu_virtio_pci_init_vq(struct vvu_socket *s, int vq_idx)
{
	char vq_name[sizeof("0000:00:00.00 vq 0")];
	struct virtio_hw *hw = &s->pdev->hw;
	const struct rte_memzone *mz;
	struct virtqueue *vq;
	uint16_t q_num;
	size_t size;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;

	q_num = VTPCI_OPS(hw)->get_queue_num(hw, vq_idx);
	RTE_LOG(DEBUG, VHOST_CONFIG, "vq %d q_num: %u\n", vq_idx, q_num);
	if (q_num == 0) {
		RTE_LOG(ERR, VHOST_CONFIG, "virtqueue %d does not exist\n",
			vq_idx);
		return -1;
	}

	if (!rte_is_power_of_2(q_num)) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"virtqueue %d has non-power of 2 size (%u)\n",
			vq_idx, q_num);
		return -1;
	}

	snprintf(vq_name, sizeof(vq_name), "%s vq %u",
		 dev->device.name, vq_idx);

	size = RTE_ALIGN_CEIL(sizeof(*vq) +
			      q_num * sizeof(struct vq_desc_extra),
			      RTE_CACHE_LINE_SIZE);
	vq = rte_zmalloc(vq_name, size, RTE_CACHE_LINE_SIZE);
	if (!vq) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocated virtqueue %d\n", vq_idx);
		return -1;
	}
	hw->vqs[vq_idx] = vq;

	vq->hw = hw;
	vq->vq_queue_index = vq_idx;
	vq->vq_nentries = q_num;

	size = vring_size(q_num, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_ring_size = RTE_ALIGN_CEIL(size, VIRTIO_PCI_VRING_ALIGN);

	mz = rte_memzone_reserve_aligned(vq_name, vq->vq_ring_size,
					 SOCKET_ID_ANY, 0,
					 VIRTIO_PCI_VRING_ALIGN);
	if (mz == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to reserve memzone for virtqueue %d\n",
			vq_idx);
		goto err_vq;
	}

	memset(mz->addr, 0, mz->len);

	vq->mz = mz;
	vq->vq_ring_mem = mz->iova;
	vq->vq_ring_virt_mem = mz->addr;
	virtio_init_vring(vq);

	if (VTPCI_OPS(hw)->setup_queue(hw, vq) < 0)
		goto err_mz;

	return 0;

err_mz:
	rte_memzone_free(mz);

err_vq:
	hw->vqs[vq_idx] = NULL;
	rte_free(vq);
	return -1;
}

static void
vvu_virtio_pci_free_virtqueues(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	int i;
	int ret;

	if (s->rxbuf_mz) {
		ret = rte_memzone_free(s->rxbuf_mz);
		if (ret < 0) {
			RTE_LOG(INFO, VHOST_CONFIG, "rte_memzone_free() for rxbuf failed\n");
		}
		s->rxbuf_mz = NULL;
	}
	if (s->txbuf_mz) {
		ret = rte_memzone_free(s->txbuf_mz);
		if (ret < 0) {
			RTE_LOG(INFO, VHOST_CONFIG, "rte_memzone_free() for txbuf failed\n");
		}
		s->txbuf_mz = NULL;
	}

	for (i = 0; i < VVU_VQ_MAX; i++) {
		struct virtqueue *vq = hw->vqs[i];

		if (!vq)
			continue;

		ret = rte_memzone_free(vq->mz);
		if (ret < 0) {
			RTE_LOG(INFO, VHOST_CONFIG, "rte_memzone_free() for vq%d failed\n", i);
		}
		rte_free(vq);
		hw->vqs[i] = NULL;
	}

	rte_free(hw->vqs);
	hw->vqs = NULL;
}

static void
vvu_virtio_pci_intr_cleanup(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;
	struct rte_intr_handle *intr_handle = &dev->intr_handle;
	int i;

	for (i = 0; i < VVU_VQ_MAX; i++)
		VTPCI_OPS(hw)->set_queue_irq(hw, hw->vqs[i],
					     VIRTIO_MSI_NO_VECTOR);
	VTPCI_OPS(hw)->set_config_irq(hw, VIRTIO_MSI_NO_VECTOR);
	rte_intr_disable(intr_handle);
	rte_intr_callback_unregister(intr_handle, vvu_interrupt_handler, s);
	rte_intr_efd_disable(intr_handle);
}

static int
vvu_virtio_pci_init_intr(struct vvu_socket *s)
{
	struct virtio_hw *hw = &s->pdev->hw;
	struct rte_pci_device *dev = s->pdev->pci_dev->dev_handle;
	struct rte_intr_handle *intr_handle = &dev->intr_handle;
	int i;

	if (!rte_intr_cap_multiple(intr_handle)) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Multiple intr vector not supported\n");
		return -1;
	}

	if (rte_intr_efd_enable(intr_handle, VVU_VQ_MAX) < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to create eventfds\n");
		return -1;
	}

	if (rte_intr_callback_register(intr_handle, vvu_interrupt_handler, s) < 0) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to register interrupt callback\n");
		goto err_efd;
	}

	if (rte_intr_enable(intr_handle) < 0)
		goto err_callback;

	if (VTPCI_OPS(hw)->set_config_irq(hw, 0) == VIRTIO_MSI_NO_VECTOR) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to set config MSI-X vector\n");
		goto err_enable;
	}

	/* TODO use separate vectors and interrupt handler functions.  It seems
	 * <rte_interrupts.h> doesn't allow efds to have interrupt_handler
	 * functions and it just clears efds when they are raised.  As a
	 * workaround we use the configuration change interrupt for virtqueue
	 * interrupts!
	 */
	for (i = 0; i < VVU_VQ_MAX; i++) {
		if (VTPCI_OPS(hw)->set_queue_irq(hw, hw->vqs[i], 0) ==
				VIRTIO_MSI_NO_VECTOR) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"Failed to set virtqueue MSI-X vector\n");
			goto err_vq;
		}
	}

	return 0;

err_vq:
	for (i = 0; i < VVU_VQ_MAX; i++)
		VTPCI_OPS(hw)->set_queue_irq(hw, hw->vqs[i],
					     VIRTIO_MSI_NO_VECTOR);
	VTPCI_OPS(hw)->set_config_irq(hw, VIRTIO_MSI_NO_VECTOR);
err_enable:
	rte_intr_disable(intr_handle);
err_callback:
	rte_intr_callback_unregister(intr_handle, vvu_interrupt_handler, s);
err_efd:
	rte_intr_efd_disable(intr_handle);
	return -1;
}

static int
vvu_virtio_pci_init_bar(struct vvu_socket *s)
{
	struct rte_pci_device *pci_dev = s->pdev->pci_dev->dev_handle;
	struct virtio_net *dev = NULL; /* just for sizeof() */

	s->doorbells = pci_dev->mem_resource[2].addr;
	if (!s->doorbells) {
		RTE_LOG(ERR, VHOST_CONFIG, "BAR 2 not availabled\n");
		return -1;
	}

	/* The number of doorbells is max_vhost_queues + 1 */
	virtio_pci_read_dev_config(&s->pdev->hw,
			offsetof(struct virtio_vhost_user_config,
				 max_vhost_queues),
			&s->max_vhost_queues,
			sizeof(s->max_vhost_queues));
	s->max_vhost_queues = rte_le_to_cpu_32(s->max_vhost_queues);
	if (s->max_vhost_queues < RTE_DIM(dev->virtqueue)) {
		/* We could support devices with a smaller max number of
		 * virtqueues than dev->virtqueue[] in the future.  Fail early
		 * for now since the current assumption is that all of
		 * dev->virtqueue[] can be used.
		 */
		RTE_LOG(ERR, VHOST_CONFIG,
			"Device supports fewer virtqueues than driver!\n");
		return -1;
	}

	return 0;
}

static int
vvu_virtio_pci_init(struct vvu_socket *s)
{
	uint64_t host_features;
	struct virtio_hw *hw = &s->pdev->hw;
	int i;

	virtio_pci_set_status(hw, VIRTIO_CONFIG_STATUS_ACK);
	virtio_pci_set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER);

	hw->guest_features = VVU_VIRTIO_FEATURES;
	host_features = VTPCI_OPS(hw)->get_features(hw);
	hw->guest_features = virtio_pci_negotiate_features(hw, host_features);

	if (!virtio_pci_with_feature(hw, VIRTIO_F_VERSION_1)) {
		RTE_LOG(ERR, VHOST_CONFIG, "Missing VIRTIO 1 feature bit\n");
		goto err;
	}

	virtio_pci_set_status(hw, VIRTIO_CONFIG_STATUS_FEATURES_OK);
	if (!(virtio_pci_get_status(hw) & VIRTIO_CONFIG_STATUS_FEATURES_OK)) {
		RTE_LOG(ERR, VHOST_CONFIG, "Failed to set FEATURES_OK\n");
		goto err;
	}

	if (vvu_virtio_pci_init_bar(s) < 0)
		goto err;

	hw->vqs = rte_zmalloc(NULL, sizeof(struct virtqueue *) * VVU_VQ_MAX, 0);
	if (!hw->vqs)
		goto err;

	for (i = 0; i < VVU_VQ_MAX; i++) {
		if (vvu_virtio_pci_init_vq(s, i) < 0) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"virtqueue %u init failed\n", i);
			goto err_init_vq;
		}
	}

	if (vvu_virtio_pci_init_rxq(s) < 0)
		goto err_init_vq;

	if (vvu_virtio_pci_init_txq(s) < 0)
		goto err_init_vq;

	if (vvu_virtio_pci_init_intr(s) < 0)
		goto err_init_vq;

	virtio_pci_set_status(hw, VIRTIO_CONFIG_STATUS_DRIVER_OK);

	return 0;

err_init_vq:
	vvu_virtio_pci_free_virtqueues(s);

err:
	virtio_pci_reset(hw);
	RTE_LOG(DEBUG, VHOST_CONFIG, "%s failed\n", __func__);
	return -1;
}

int
rte_vhost_vvu_pci_probe(void *probe_ctx __rte_unused,
	      struct spdk_pci_device *pci_dev)
{
	struct vvu_pci_device *pdev;
	struct rte_pci_device *dev = pci_dev->dev_handle;

	/* TODO support multi-process applications */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"virtio-vhost-pci does not support multi-process "
			"applications\n");
		return -1;
	}

	pdev = rte_zmalloc_socket(dev->device.name, sizeof(*pdev),
				  RTE_CACHE_LINE_SIZE,
				  dev->device.numa_node);
	if (!pdev)
		return -1;

	pdev->pci_dev = pci_dev;

	if (virtio_pci_init(dev, &pdev->hw) != 0) {
		rte_free(pdev);
		return -1;
	}

	/* Reset the device now, the rest is done in vvu_socket_init() */
	virtio_pci_reset(&pdev->hw);

	if (pdev->hw.use_msix == VIRTIO_MSIX_NONE) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"MSI-X is required for PCI device at %s\n",
			dev->device.name);
		rte_free(pdev);
		rte_pci_unmap_device(dev);
		return -1;
	}

	TAILQ_INSERT_TAIL(&vvu_pci_device_list, pdev, next);

	RTE_LOG(INFO, VHOST_CONFIG,
		"Added virtio-vhost-user device at %s\n",
		dev->device.name);

	return 0;
}

static int
vvu_pci_remove(struct spdk_pci_device *pci_dev)
{
	struct vvu_pci_device *pdev;

	TAILQ_FOREACH(pdev, &vvu_pci_device_list, next)
		if (pdev->pci_dev == pci_dev)
			break;
	if (!pdev)
		return -1;

	if (pdev->s) {
		struct rte_pci_device *dev = pci_dev->dev_handle;
		RTE_LOG(ERR, VHOST_CONFIG,
			"Cannot remove PCI device at %s with vhost still attached\n",
			dev->device.name);
		return -1;
	}

	TAILQ_REMOVE(&vvu_pci_device_list, pdev, next);
	rte_free(pdev);

	spdk_pci_device_detach(pci_dev);

	return 0;
}

static int
vvu_socket_init(struct vhost_user_socket *vsocket, uint64_t flags)
{
	struct vvu_socket *s =
		container_of(vsocket, struct vvu_socket, socket);
	struct vvu_pci_device *pdev;

	if (flags & RTE_VHOST_USER_NO_RECONNECT) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"error: reconnect cannot be disabled for virtio-vhost-user\n");
		return -1;
	}
	if (flags & RTE_VHOST_USER_CLIENT) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"error: virtio-vhost-user does not support client mode\n");
		return -1;
	}
	if (flags & RTE_VHOST_USER_DEQUEUE_ZERO_COPY) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"error: virtio-vhost-user does not support dequeue-zero-copy\n");
		return -1;
	}

	pdev = vvu_pci_by_name(vsocket->path);
	if (!pdev) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Cannot find virtio-vhost-user PCI device at %s\n",
			vsocket->path);
		return -1;
	}

	if (pdev->s) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Device at %s is already in use\n",
			vsocket->path);
		return -1;
	}

	s->pdev = pdev;
	pdev->s = s;

	if (vvu_virtio_pci_init(s) < 0) {
		s->pdev = NULL;
		pdev->s = NULL;
		return -1;
	}

	RTE_LOG(INFO, VHOST_CONFIG, "%s at %s\n", __func__, vsocket->path);
	return 0;
}

static void
vvu_socket_cleanup(struct vhost_user_socket *vsocket)
{
	struct vvu_socket *s =
		container_of(vsocket, struct vvu_socket, socket);

	if (s->conn)
		vhost_destroy_device(s->conn->device.vid);

	vvu_virtio_pci_intr_cleanup(s);
	virtio_pci_reset(&s->pdev->hw);
	vvu_virtio_pci_free_virtqueues(s);

	s->pdev->s = NULL;

	vvu_pci_remove(s->pdev->pci_dev);

	s->pdev = NULL;
}

static int
vvu_socket_start(struct vhost_user_socket *vsocket)
{
	struct vvu_socket *s =
		container_of(vsocket, struct vvu_socket, socket);

	vvu_connect(s);
	return 0;
}

const struct vhost_transport_ops virtio_vhost_user_trans_ops = {
	.socket_size = sizeof(struct vvu_socket),
	.device_size = sizeof(struct vvu_connection),
	.socket_init = vvu_socket_init,
	.socket_cleanup = vvu_socket_cleanup,
	.socket_start = vvu_socket_start,
	.cleanup_device = vvu_cleanup_device,
	.vring_call = vvu_vring_call,
	.send_reply = vvu_send_reply,
	.map_mem_regions = vvu_map_mem_regions,
	.unmap_mem_regions = vvu_unmap_mem_regions,
};
