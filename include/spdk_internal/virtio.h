/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#ifndef SPDK_VIRTIO_H
#define SPDK_VIRTIO_H

#include "spdk/stdinc.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_config.h>

#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/pci_ids.h"
#include "spdk/env.h"

/**
 * The maximum virtqueue size is 2^15. Use that value as the end of
 * descriptor chain terminator since it will never be a valid index
 * in the descriptor table. This is used to verify we are correctly
 * handling vq_free_cnt.
 */
#define VQ_RING_DESC_CHAIN_END 32768

#define SPDK_VIRTIO_MAX_VIRTQUEUES 0x100

/* Extra status define for readability */
#define VIRTIO_CONFIG_S_RESET 0

/* This flag means the descriptor was made available by the driver */
#define SPDK_VRING_PACKED_DESC_F_AVAIL       (1 << 7)
/* This flag means the descriptor was used by the device */
#define SPDK_VRING_PACKED_DESC_F_USED        (1 << 15)

#define VRING_PACKED_DESC_F_AVAIL_USED  (SPDK_VRING_PACKED_DESC_F_AVAIL | \
                                         SPDK_VRING_PACKED_DESC_F_USED)
struct virtio_dev_ops;


/* For support of packed virtqueues in Virtio 1.1 the format of descriptors
 * looks like this.
 */

#define RING_EVENT_FLAGS_ENABLE 0x0
#define RING_EVENT_FLAGS_DISABLE 0x1
#define RING_EVENT_FLAGS_DESC 0x2

struct vring_packed {
        unsigned int num;
        uint64_t desc_iova;
        struct vring_packed_desc *desc;
        struct vring_packed_desc_event *driver;
        struct vring_packed_desc_event *device;
};

struct virtio_dev {
	struct virtqueue **vqs;

	/** Name of this virtio dev set by backend */
	char		*name;

	/** Fixed number of backend-specific non-I/O virtqueues. */
	uint16_t	fixed_queues_num;

	/** Max number of virtqueues the host supports. */
	uint16_t	max_queues;

	/** Common device & guest features. */
	uint64_t	negotiated_features;

	int		is_hw;

	/** Modern/legacy virtio device flag. */
	uint8_t		modern;

	/** Mutex for asynchronous virtqueue-changing operations. */
	pthread_mutex_t	mutex;

	/** Backend-specific callbacks. */
	const struct virtio_dev_ops *backend_ops;

	/** Context for the backend ops */
	void		*ctx;
};

struct virtio_dev_ops {
	int (*read_dev_cfg)(struct virtio_dev *hw, size_t offset,
			    void *dst, int len);
	int (*write_dev_cfg)(struct virtio_dev *hw, size_t offset,
			     const void *src, int len);
	uint8_t (*get_status)(struct virtio_dev *hw);
	void (*set_status)(struct virtio_dev *hw, uint8_t status);

	/**
	 * Get device features. The features might be already
	 * negotiated with driver (guest) features.
	 */
	uint64_t (*get_features)(struct virtio_dev *vdev);

	/**
	 * Negotiate and set device features.
	 * The negotiation can fail with return code -1.
	 * This function should also set vdev->negotiated_features field.
	 */
	int (*set_features)(struct virtio_dev *vdev, uint64_t features);

	/** Destruct virtio device */
	void (*destruct_dev)(struct virtio_dev *vdev);

	uint16_t (*get_queue_size)(struct virtio_dev *vdev, uint16_t queue_id);
	int (*setup_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*del_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*notify_queue)(struct virtio_dev *hw, struct virtqueue *vq);

	void (*dump_json_info)(struct virtio_dev *hw, struct spdk_json_write_ctx *w);
	void (*write_json_config)(struct virtio_dev *hw, struct spdk_json_write_ctx *w);
};

struct vq_desc_extra {
	void *cookie;
	uint16_t ndescs;
	bool wrapped;
	uint16_t head_idx;
	uint16_t  last_idx;
	uint16_t next;
};

struct virtqueue {
	struct virtio_dev *vdev; /**< owner of this virtqueue */
	union {
		struct {
			/**< vring keeping desc, used and avail */
			struct vring ring;
		} vq_split;

		struct {
			/**< vring keeping descs and events */
			struct vring_packed ring;
			bool used_wrap_counter;
			uint16_t cached_flags; /**< cached flags for descs */
			uint16_t event_flags_shadow;
		} vq_packed;

	};
	/**
	 * Last consumed descriptor in the used table,
	 * trails vq_ring.used->idx.
	 */
	uint16_t vq_used_cons_idx;
	uint16_t vq_nentries;  /**< vring desc numbers */
	uint16_t vq_free_cnt;  /**< num of desc available */
	uint16_t vq_avail_idx; /**< sync until needed */

	void *vq_ring_virt_mem;  /**< virtual address of vring */
	unsigned int vq_ring_size;

	uint64_t vq_ring_mem; /**< physical address of vring */

	/**
	 * Head of the free chain in the descriptor table. If
	 * there are no free descriptors, this will be set to
	 * VQ_RING_DESC_CHAIN_END.
	 */
	uint16_t  vq_desc_head_idx;

	/**
	 * Tail of the free chain in desc table. If
	 * there are no free descriptors, this will be set to
	 * VQ_RING_DESC_CHAIN_END.
	 */
	uint16_t  vq_desc_tail_idx;
	uint16_t  vq_queue_index;   /**< PCI queue index */
	uint16_t  *notify_addr;

	/** Thread that's polling this queue. */
	struct spdk_thread *owner_thread;

	uint16_t req_start;
	uint16_t req_end;
	uint16_t reqs_finished;

	struct vq_desc_extra vq_descx[0];
};

enum spdk_virtio_desc_type {
	SPDK_VIRTIO_DESC_RO = 0, /**< Read only */
	SPDK_VIRTIO_DESC_WR = VRING_DESC_F_WRITE, /**< Write only */
	/* TODO VIRTIO_DESC_INDIRECT */
};

/** Context for creating PCI virtio_devs */
struct virtio_pci_ctx;

/**
 * Check if the device has negotiated given feature bit.
 *
 * \param vdev virtio device
 * \param bit feature bit
 */
static inline bool
virtio_dev_has_feature(struct virtio_dev *vdev, uint64_t bit)
{
        return !!(vdev->negotiated_features & (1ULL << bit));
}

static inline int
virtio_with_packed_queue(struct virtio_dev *vdev)
{
        return virtio_dev_has_feature(vdev, VIRTIO_F_RING_PACKED);
}

static inline size_t
vring_size_packed(unsigned int num, unsigned long align)
{
	size_t size;

	size = num * sizeof(struct vring_packed_desc);
	size += sizeof(struct vring_packed_desc_event);
	size = SPDK_ALIGN_CEIL(size, align);
	size += sizeof(struct vring_packed_desc_event);
	return size;
}

static inline void
vring_init_packed(struct vring_packed *vr, uint8_t *p, uint64_t iova,
                  unsigned long align, unsigned int num)
{
        vr->num = num;
        vr->desc = (struct vring_packed_desc *)p;
        vr->desc_iova = iova;
        vr->driver = (struct vring_packed_desc_event *)(p +
                        vr->num * sizeof(struct vring_packed_desc));
        vr->device = (struct vring_packed_desc_event *)
                SPDK_ALIGN_CEIL(((uintptr_t)vr->driver +
                                sizeof(struct vring_packed_desc_event)), align);
}

static inline void
vring_desc_init_packed(struct virtqueue *vq, int n)
{
        int i;
        for (i = 0; i < n - 1; i++) {
                vq->vq_packed.ring.desc[i].id = i;
                vq->vq_descx[i].next = i + 1;
        }
        vq->vq_packed.ring.desc[i].id = i;
        vq->vq_descx[i].next = VQ_RING_DESC_CHAIN_END;
}

static inline void
virtqueue_disable_intr_packed(struct virtqueue *vq)
{
        /*
         * Set RING_EVENT_FLAGS_DISABLE to hint host
         * not to interrupt when it consumes packets
         * Note: this is only considered a hint to the host
         */
        if (vq->vq_packed.event_flags_shadow != RING_EVENT_FLAGS_DISABLE) {
                vq->vq_packed.event_flags_shadow = RING_EVENT_FLAGS_DISABLE;
                vq->vq_packed.ring.driver->flags =
                        vq->vq_packed.event_flags_shadow;
        }
}

static inline void
virtqueue_disable_intr_split(struct virtqueue *vq)
{
        /* Tell the backend not to interrupt us.
         * If F_EVENT_IDX is negotiated, we will always set incredibly high
         * used event idx, so that we will practically never receive an
         * interrupt. See virtqueue_req_flush()
         */
        if (vq->vdev->negotiated_features & (1ULL << VIRTIO_RING_F_EVENT_IDX)) {
                vring_used_event(&vq->vq_split.ring) = UINT16_MAX;
        } else {
                vq->vq_split.ring.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
        }
}

/*
 * *
 * Callback for creating virtio_dev from a PCI device.
 * \param pci_ctx PCI context to be associated with a virtio_dev
 * \param ctx context provided by the user
 * \return 0 on success, -1 on error.
 */
typedef int (*virtio_pci_create_cb)(struct virtio_pci_ctx *pci_ctx, void *ctx);

uint16_t virtio_recv_pkts(struct virtqueue *vq, void **io, uint32_t *len, uint16_t io_cnt);

/**
 * Start a new request on the current vring head position and associate it
 * with an opaque cookie object. The previous request in given vq will be
 * made visible to the device in hopes it can be processed early, but there's
 * no guarantee it will be until the device is notified with \c
 * virtqueue_req_flush. This behavior is simply an optimization and virtqueues
 * must always be flushed. Empty requests (with no descriptors added) will be
 * ignored. The device owning given virtqueue must be started.
 *
 * \param vq virtio queue
 * \param cookie opaque object to associate with this request. Once the request
 * is sent, processed and a response is received, the same object will be
 * returned to the user after calling the virtio poll API.
 * \param iovcnt number of required iovectors for the request. This can be
 * higher than than the actual number of iovectors to be added.
 * \return 0 on success or negative errno otherwise. If the `iovcnt` is
 * greater than virtqueue depth, -EINVAL is returned. If simply not enough
 * iovectors are available, -ENOMEM is returned.
 */
int virtqueue_req_start(struct virtqueue *vq, void *cookie, int iovcnt);

/**
 * Flush a virtqueue. This will notify the device if it's required.
 * The device owning given virtqueue must be started.
 *
 * \param vq virtio queue
 */
void virtqueue_req_flush(struct virtqueue *vq);

/**
 * Abort the very last request in a virtqueue. This will restore virtqueue
 * state to the point before the last request was created. Note that this
 * is only effective if a queue hasn't been flushed yet.  The device owning
 * given virtqueue must be started.
 *
 * \param vq virtio queue
 */
void virtqueue_req_abort(struct virtqueue *vq);

/**
 * Add iovec chain to the last created request. This call does not provide any
 * error-checking. The caller has to ensure that he doesn't add more iovs than
 * what was specified during request creation. The device owning given virtqueue
 * must be started.
 *
 * \param vq virtio queue
 * \param iovs iovec array
 * \param iovcnt number of iovs in iovec array
 * \param desc_type type of all given iovectors
 */
void virtqueue_req_add_iovs(struct virtqueue *vq, struct iovec *iovs, uint16_t iovcnt,
			    enum spdk_virtio_desc_type desc_type);

/**
 * Construct a virtio device.  The device will be in stopped state by default.
 * Before doing any I/O, it has to be manually started via \c virtio_dev_restart.
 *
 * \param vdev memory for virtio device, must be zeroed
 * \param name name for the virtio device
 * \param ops backend callbacks
 * \param ops_ctx argument for the backend callbacks
 * \return zero on success, or negative error code otherwise
 */
int virtio_dev_construct(struct virtio_dev *vdev, const char *name,
			 const struct virtio_dev_ops *ops, void *ops_ctx);

/**
 * Reset the device and prepare it to be `virtio_dev_start`ed.  This call
 * will also renegotiate feature flags.
 *
 * \param vdev virtio device
 * \param req_features features this driver supports. A VIRTIO_F_VERSION_1
 * flag will be automatically appended, as legacy devices are not supported.
 */
int virtio_dev_reset(struct virtio_dev *vdev, uint64_t req_features);

/**
 * Notify the host to start processing this virtio device.  This is
 * a blocking call that won't return until the host has started.
 * This will also allocate virtqueues.
 *
 * \param vdev virtio device
 * \param max_queues number of queues to allocate. The max number of
 * usable I/O queues is also limited by the host device. `vdev` will be
 * started successfully even if the host supports less queues than requested.
 * \param fixed_queue_num number of queues preceding the first
 * request queue. For Virtio-SCSI this is equal to 2, as there are
 * additional event and control queues.
 */
int virtio_dev_start(struct virtio_dev *vdev, uint16_t max_queues,
		     uint16_t fixed_queues_num);

/**
 * Stop the host from processing the device.  This is a blocking call
 * that won't return until all outstanding I/O has been processed on
 * the host (virtio device) side. In order to re-start the device, it
 * has to be `virtio_dev_reset` first.
 *
 * \param vdev virtio device
 */
void virtio_dev_stop(struct virtio_dev *vdev);

/**
 * Destruct a virtio device.  Note that it must be in the stopped state.
 * The virtio_dev should be manually freed afterwards.
 *
 * \param vdev virtio device
 */
void virtio_dev_destruct(struct virtio_dev *vdev);

/**
 * Bind a virtqueue with given index to the current thread;
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index virtqueue index
 * \return 0 on success, -1 in case a virtqueue with given index either
 * does not exists or is already acquired.
 */
int virtio_dev_acquire_queue(struct virtio_dev *vdev, uint16_t index);

/**
 * Look for unused queue and bind it to the current thread.  This will
 * scan the queues in range from *start_index* (inclusive) up to
 * vdev->max_queues (exclusive).
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param start_index virtqueue index to start looking from
 * \return index of acquired queue or -1 in case no unused queue in given range
 * has been found
 */
int32_t virtio_dev_find_and_acquire_queue(struct virtio_dev *vdev, uint16_t start_index);

/**
 * Get thread that acquired given virtqueue.
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index index of virtqueue
 * \return thread that acquired given virtqueue. If the queue is unused
 * or doesn't exist a NULL is returned.
 */
struct spdk_thread *virtio_dev_queue_get_thread(struct virtio_dev *vdev, uint16_t index);

/**
 * Check if virtqueue with given index is acquired.
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param index index of virtqueue
 * \return virtqueue acquire status. in case of invalid index *false* is returned.
 */
bool virtio_dev_queue_is_acquired(struct virtio_dev *vdev, uint16_t index);

/**
 * Release previously acquired queue.
 *
 * This function must be called from the thread that acquired the queue.
 *
 * \param vdev vhost device
 * \param index index of virtqueue to release
 */
void virtio_dev_release_queue(struct virtio_dev *vdev, uint16_t index);

/**
 * Get Virtio status flags.
 *
 * \param vdev virtio device
 */
uint8_t virtio_dev_get_status(struct virtio_dev *vdev);

/**
 * Set Virtio status flag.  The flags have to be set in very specific order
 * defined the VIRTIO 1.0 spec section 3.1.1. To unset the flags, stop the
 * device or set \c VIRTIO_CONFIG_S_RESET status flag. There is no way to
 * unset only particular flags.
 *
 * \param vdev virtio device
 * \param flag flag to set
 */
void virtio_dev_set_status(struct virtio_dev *vdev, uint8_t flag);

/**
 * Write raw data into the device config at given offset.  This call does not
 * provide any error checking.
 *
 * \param vdev virtio device
 * \param offset offset in bytes
 * \param src pointer to data to copy from
 * \param len length of data to copy in bytes
 * \return 0 on success, negative errno otherwise
 */
int virtio_dev_write_dev_config(struct virtio_dev *vdev, size_t offset, const void *src, int len);

/**
 * Read raw data from the device config at given offset.  This call does not
 * provide any error checking.
 *
 * \param vdev virtio device
 * \param offset offset in bytes
 * \param dst pointer to buffer to copy data into
 * \param len length of data to copy in bytes
 * \return 0 on success, negative errno otherwise
 */
int virtio_dev_read_dev_config(struct virtio_dev *vdev, size_t offset, void *dst, int len);

/**
 * Get backend-specific ops for given device.
 *
 * \param vdev virtio device
 */
const struct virtio_dev_ops *virtio_dev_backend_ops(struct virtio_dev *vdev);

/**
 * Dump all device specific information into given json stream.
 *
 * \param vdev virtio device
 * \param w json stream
 */
void virtio_dev_dump_json_info(struct virtio_dev *vdev, struct spdk_json_write_ctx *w);

/**
 * Enumerate all PCI Virtio devices of given type on the system.
 *
 * \param enum_cb a function to be called for each valid PCI device.
 * If a virtio_dev is has been created, the callback should return 0.
 * Returning any other value will cause the PCI context to be freed,
 * making it unusable.
 * \param enum_ctx additional opaque context to be passed into `enum_cb`
 * \param pci_device_id PCI Device ID of devices to iterate through
 */
int virtio_pci_dev_enumerate(virtio_pci_create_cb enum_cb, void *enum_ctx,
			     uint16_t pci_device_id);

/**
 * Attach a PCI Virtio device of given type.
 *
 * \param create_cb callback to create a virtio_dev.
 * If virtio_dev is has been created, the callback should return 0.
 * Returning any other value will cause the PCI context to be freed,
 * making it unusable.
 * \param enum_ctx additional opaque context to be passed into `enum_cb`
 * \param device_id Device ID of devices to iterate through
 * \param pci_addr PCI address of the device to attach
 */
int virtio_pci_dev_attach(virtio_pci_create_cb create_cb, void *enum_ctx,
			  uint16_t device_id, struct spdk_pci_addr *pci_addr);

/**
 * Connect to a vhost-user device and init corresponding virtio_dev struct.
 * The virtio_dev will have to be freed with \c virtio_dev_free.
 *
 * \param vdev preallocated vhost device struct to operate on
 * \param name name of this virtio device
 * \param path path to the Unix domain socket of the vhost-user device
 * \param queue_size size of each of the queues
 * \return virtio device
 */
int virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
			 uint32_t queue_size);

/**
 * Connect to a vfio-user device and init corresponding virtio_dev struct.
 * The virtio_dev will have to be freed with \c virtio_dev_free.
 *
 * \param vdev preallocated vhost device struct to operate on
 * \param name name of this virtio device
 * \param path path to the Unix domain socket of the vhost-user device
 * \return virtio device
 */
int virtio_vfio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path);

/**
 * Initialize virtio_dev for a given PCI device.
 * The virtio_dev has to be freed with \c virtio_dev_destruct.
 *
 * \param vdev preallocated vhost device struct to operate on
 * \param name name of this virtio device
 * \param pci_ctx context of the PCI device
 * \return 0 on success, -1 on error.
 */
int virtio_pci_dev_init(struct virtio_dev *vdev, const char *name,
			struct virtio_pci_ctx *pci_ctx);

/**
 * Process the uevent which is accepted from the kernel and the
 * uevent descript the physical device hot add or remove action.
 *
 * \param fd the file descriptor of the kobject netlink socket
 * \param device_id virtio device ID used to represent virtio-blk or other device.
 * \return the name of the virtio device on success, NULL means it
 * is not a suitable uevent.
 */
const char *virtio_pci_dev_event_process(int fd, uint16_t device_id);

#endif /* SPDK_VIRTIO_H */
