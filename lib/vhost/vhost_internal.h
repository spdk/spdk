/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VHOST_INTERNAL_H
#define SPDK_VHOST_INTERNAL_H
#include <linux/virtio_config.h>

#include "spdk/stdinc.h"

#include <rte_vhost.h>

#include "spdk_internal/vhost_user.h"
#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/config.h"

#define SPDK_VHOST_MAX_VQUEUES	256
#define SPDK_VHOST_MAX_VQ_SIZE	1024

#define SPDK_VHOST_SCSI_CTRLR_MAX_DEVS 8

#define SPDK_VHOST_IOVS_MAX 129

#define SPDK_VHOST_VQ_MAX_SUBMISSIONS	32

/*
 * Rate at which stats are checked for interrupt coalescing.
 */
#define SPDK_VHOST_STATS_CHECK_INTERVAL_MS 10
/*
 * Default threshold at which interrupts start to be coalesced.
 */
#define SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD 60000

/*
 * Currently coalescing is not used by default.
 * Setting this to value > 0 here or by RPC will enable coalescing.
 */
#define SPDK_VHOST_COALESCING_DELAY_BASE_US 0

#define SPDK_VHOST_FEATURES ((1ULL << VHOST_F_LOG_ALL) | \
	(1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
	(1ULL << VIRTIO_F_VERSION_1) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
	(1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC) | \
	(1ULL << VIRTIO_F_RING_PACKED) | \
	(1ULL << VIRTIO_F_ANY_LAYOUT))

#define SPDK_VHOST_DISABLED_FEATURES ((1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY))

#define VRING_DESC_F_AVAIL	(1ULL << VRING_PACKED_DESC_F_AVAIL)
#define VRING_DESC_F_USED	(1ULL << VRING_PACKED_DESC_F_USED)
#define VRING_DESC_F_AVAIL_USED	(VRING_DESC_F_AVAIL | VRING_DESC_F_USED)

typedef struct rte_vhost_resubmit_desc spdk_vhost_resubmit_desc;
typedef struct rte_vhost_resubmit_info spdk_vhost_resubmit_info;
typedef struct rte_vhost_inflight_desc_packed	spdk_vhost_inflight_desc;

struct spdk_vhost_virtqueue {
	struct rte_vhost_vring vring;
	struct rte_vhost_ring_inflight vring_inflight;
	uint16_t last_avail_idx;
	uint16_t last_used_idx;

	struct {
		/* To mark a descriptor as available in packed ring
		 * Equal to avail_wrap_counter in spec.
		 */
		uint8_t avail_phase	: 1;
		/* To mark a descriptor as used in packed ring
		 * Equal to used_wrap_counter in spec.
		 */
		uint8_t used_phase	: 1;
		uint8_t padding		: 5;
		bool packed_ring	: 1;
	} packed;

	void *tasks;

	/* Request count from last stats check */
	uint32_t req_cnt;

	/* Request count from last event */
	uint16_t used_req_cnt;

	/* How long interrupt is delayed */
	uint32_t irq_delay_time;

	/* Next time when we need to send event */
	uint64_t next_event_time;

	/* Associated vhost_virtqueue in the virtio device's virtqueue list */
	uint32_t vring_idx;

	struct spdk_vhost_session *vsession;

	struct spdk_interrupt *intr;
} __attribute((aligned(SPDK_CACHE_LINE_SIZE)));

struct spdk_vhost_session {
	struct spdk_vhost_dev *vdev;

	/* rte_vhost connection ID. */
	int vid;

	/* Unique session ID. */
	uint64_t id;
	/* Unique session name. */
	char *name;

	bool started;
	bool interrupt_mode;

	struct rte_vhost_memory *mem;

	int task_cnt;

	uint16_t max_queues;

	uint64_t negotiated_features;

	/* Local copy of device coalescing settings. */
	uint32_t coalescing_delay_time_base;
	uint32_t coalescing_io_rate_threshold;

	/* Next time when stats for event coalescing will be checked. */
	uint64_t next_stats_check_time;

	/* Interval used for event coalescing checking. */
	uint64_t stats_check_interval;

	/* Session's stop poller will only try limited times to destroy the session. */
	uint32_t stop_retry_count;

	struct spdk_vhost_virtqueue virtqueue[SPDK_VHOST_MAX_VQUEUES];

	TAILQ_ENTRY(spdk_vhost_session) tailq;
};

struct spdk_vhost_user_dev {
	struct spdk_vhost_dev *vdev;

	const struct spdk_vhost_user_dev_backend *user_backend;

	/* Saved original values used to setup coalescing to avoid integer
	 * rounding issues during save/load config.
	 */
	uint32_t coalescing_delay_us;
	uint32_t coalescing_iops_threshold;

	bool registered;

	/* Use this lock to protect multiple sessions. */
	pthread_mutex_t lock;

	/* Current connections to the device */
	TAILQ_HEAD(, spdk_vhost_session) vsessions;

	/* Increment-only session counter */
	uint64_t vsessions_num;

	/* Number of pending asynchronous operations */
	uint32_t pending_async_op_num;
};

struct spdk_vhost_dev {
	char *name;
	char *path;

	struct spdk_thread *thread;

	uint64_t virtio_features;
	uint64_t disabled_features;
	uint64_t protocol_features;
	bool packed_ring_recovery;

	const struct spdk_vhost_dev_backend *backend;

	/* Context passed from transport */
	void *ctxt;

	TAILQ_ENTRY(spdk_vhost_dev) tailq;
};

static inline struct spdk_vhost_user_dev *
to_user_dev(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return vdev->ctxt;
}

/**
 * \param vdev vhost device.
 * \param vsession vhost session.
 * \param arg user-provided parameter.
 *
 * \return negative values will break the foreach call, meaning
 * the function won't be called again. Return codes zero and
 * positive don't have any effect.
 */
typedef int (*spdk_vhost_session_fn)(struct spdk_vhost_dev *vdev,
				     struct spdk_vhost_session *vsession,
				     void *arg);

/**
 * \param vdev vhost device.
 * \param arg user-provided parameter.
 */
typedef void (*spdk_vhost_dev_fn)(struct spdk_vhost_dev *vdev, void *arg);

struct spdk_vhost_user_dev_backend {
	/**
	 * Size of additional per-session context data
	 * allocated whenever a new client connects.
	 */
	size_t session_ctx_size;

	spdk_vhost_session_fn start_session;
	spdk_vhost_session_fn stop_session;
	int (*alloc_vq_tasks)(struct spdk_vhost_session *vsession, uint16_t qid);
};

enum vhost_backend_type {
	VHOST_BACKEND_BLK = 0,
	VHOST_BACKEND_SCSI,
};

struct spdk_vhost_dev_backend {
	enum vhost_backend_type type;

	int (*vhost_get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);
	int (*vhost_set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
				uint32_t offset, uint32_t size, uint32_t flags);

	void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	int (*remove_device)(struct spdk_vhost_dev *vdev);
	int (*set_coalescing)(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);
	void (*get_coalescing)(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);
};

void *vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len);

uint16_t vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *vq, uint16_t *reqs,
				 uint16_t reqs_len);

/**
 * Get a virtio split descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accessible via
 * \c spdk_vhost_vring_desc_get_next.
 * \param vsession vhost session
 * \param vq virtqueue
 * \param req_idx descriptor index
 * \param desc pointer to be set to the descriptor
 * \param desc_table descriptor table to be used with
 * \c spdk_vhost_vring_desc_get_next. This might be either
 * default virtqueue descriptor table or per-chain indirect
 * table.
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid.
 * If -1 is returned, the content of params is undefined.
 */
int vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq,
		      uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		      uint32_t *desc_table_size);

/**
 * Get a virtio packed descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accessible via
 * \c vhost_vring_packed_desc_get_next.
 * \param vsession vhost session
 * \param vq virtqueue
 * \param req_idx descriptor index
 * \param desc pointer to be set to the descriptor
 * \param desc_table descriptor table to be used with
 * \c spdk_vhost_vring_desc_get_next. This might be either
 * \c NULL or per-chain indirect table.
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid.
 * If -1 is returned, the content of params is undefined.
 */
int vhost_vq_get_desc_packed(struct spdk_vhost_session *vsession,
			     struct spdk_vhost_virtqueue *virtqueue,
			     uint16_t req_idx, struct vring_packed_desc **desc,
			     struct vring_packed_desc **desc_table, uint32_t *desc_table_size);

int vhost_inflight_queue_get_desc(struct spdk_vhost_session *vsession,
				  spdk_vhost_inflight_desc *desc_array,
				  uint16_t req_idx, spdk_vhost_inflight_desc **desc,
				  struct vring_packed_desc  **desc_table, uint32_t *desc_table_size);

/**
 * Send IRQ/call client (if pending) for \c vq.
 * \param vsession vhost session
 * \param vq virtqueue
 * \return
 *   0 - if no interrupt was signalled
 *   1 - if interrupt was signalled
 */
int vhost_vq_used_signal(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq);

/**
 * Send IRQs for the queue that need to be signaled.
 * \param vq virtqueue
 */
void vhost_session_vq_used_signal(struct spdk_vhost_virtqueue *virtqueue);

void vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
				struct spdk_vhost_virtqueue *vq,
				uint16_t id, uint32_t len);

/**
 * Enqueue the entry to the used ring when device complete the request.
 * \param vsession vhost session
 * \param vq virtqueue
 * \req_idx descriptor index. It's the first index of this descriptor chain.
 * \num_descs descriptor count. It's the count of the number of buffers in the chain.
 * \buffer_id descriptor buffer ID.
 * \length device write length. Specify the length of the buffer that has been initialized
 * (written to) by the device
 * \inflight_head the head idx of this IO inflight desc chain.
 */
void vhost_vq_packed_ring_enqueue(struct spdk_vhost_session *vsession,
				  struct spdk_vhost_virtqueue *virtqueue,
				  uint16_t num_descs, uint16_t buffer_id,
				  uint32_t length, uint16_t inflight_head);

/**
 * Get subsequent descriptor from given table.
 * \param desc current descriptor, will be set to the
 * next descriptor (NULL in case this is the last
 * descriptor in the chain or the next desc is invalid)
 * \param desc_table descriptor table
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid
 * The *desc* param will be set regardless of the
 * return value.
 */
int vhost_vring_desc_get_next(struct vring_desc **desc,
			      struct vring_desc *desc_table, uint32_t desc_table_size);
static inline bool
vhost_vring_desc_is_wr(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_WRITE);
}

int vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			    uint16_t *iov_index, const struct vring_desc *desc);

bool vhost_vq_packed_ring_is_avail(struct spdk_vhost_virtqueue *virtqueue);

/**
 * Get subsequent descriptor from vq or desc table.
 * \param desc current descriptor, will be set to the
 * next descriptor (NULL in case this is the last
 * descriptor in the chain or the next desc is invalid)
 * \req_idx index of current desc, will be set to the next
 * index. If desc_table != NULL the req_idx is the the vring index
 * or the req_idx is the desc_table index.
 * \param desc_table descriptor table
 * \param desc_table_size size of the *desc_table*
 * \return 0 on success, -1 if given index is invalid
 * The *desc* param will be set regardless of the
 * return value.
 */
int vhost_vring_packed_desc_get_next(struct vring_packed_desc **desc, uint16_t *req_idx,
				     struct spdk_vhost_virtqueue *vq,
				     struct vring_packed_desc *desc_table,
				     uint32_t desc_table_size);

bool vhost_vring_packed_desc_is_wr(struct vring_packed_desc *cur_desc);

int vhost_vring_packed_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				   uint16_t *iov_index, const struct vring_packed_desc *desc);

bool vhost_vring_inflight_desc_is_wr(spdk_vhost_inflight_desc *cur_desc);

int vhost_vring_inflight_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				     uint16_t *iov_index, const spdk_vhost_inflight_desc *desc);

uint16_t vhost_vring_packed_desc_get_buffer_id(struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
		uint16_t *num_descs);

static inline bool
__attribute__((always_inline))
vhost_dev_has_feature(struct spdk_vhost_session *vsession, unsigned feature_id)
{
	return vsession->negotiated_features & (1ULL << feature_id);
}

int vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		       const struct spdk_json_val *params,
		       const struct spdk_vhost_dev_backend *backend,
		       const struct spdk_vhost_user_dev_backend *user_backend);
int vhost_dev_unregister(struct spdk_vhost_dev *vdev);

void vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

/*
 * Set vhost session to run in interrupt or poll mode
 */
void vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession,
		bool interrupt_mode);

/*
 * Memory registration functions used in start/stop device callbacks
 */
void vhost_session_mem_register(struct rte_vhost_memory *mem);
void vhost_session_mem_unregister(struct rte_vhost_memory *mem);

/*
 * Call a function for each session of the provided vhost device.
 * The function will be called one-by-one on each session's thread.
 *
 * \param vdev vhost device
 * \param fn function to call on each session's thread
 * \param cpl_fn function to be called at the end of the iteration on
 * the vhost management thread.
 * Optional, can be NULL.
 * \param arg additional argument to the both callbacks
 */
void vhost_user_dev_foreach_session(struct spdk_vhost_dev *dev,
				    spdk_vhost_session_fn fn,
				    spdk_vhost_dev_fn cpl_fn,
				    void *arg);

/**
 * Finish a blocking vhost_user_wait_for_session_stop() call and finally
 * stop the session. This must be called on the session's lcore which
 * used to receive all session-related messages (e.g. from
 * vhost_user_dev_foreach_session()). After this call, the session-
 * related messages will be once again processed by any arbitrary thread.
 *
 * Must be called under the vhost user device's session access lock.
 *
 * \param vsession vhost session
 * \param response return code
 */
void vhost_user_session_stop_done(struct spdk_vhost_session *vsession, int response);

struct spdk_vhost_session *vhost_session_find_by_vid(int vid);
void vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession);
int vhost_register_unix_socket(const char *path, const char *ctrl_name,
			       uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features);
int vhost_driver_unregister(const char *path);
int vhost_get_mem_table(int vid, struct rte_vhost_memory **mem);
int vhost_get_negotiated_features(int vid, uint64_t *negotiated_features);

int remove_vhost_controller(struct spdk_vhost_dev *vdev);

struct spdk_io_channel *vhost_blk_get_io_channel(struct spdk_vhost_dev *vdev);
void vhost_blk_put_io_channel(struct spdk_io_channel *ch);

/* The spdk_bdev pointer should only be used to retrieve
 * the device properties, ex. number of blocks or I/O type supported. */
struct spdk_bdev *vhost_blk_get_bdev(struct spdk_vhost_dev *vdev);

/* Function calls from vhost.c to rte_vhost_user.c,
 * shall removed once virtio transport abstraction is complete. */
int vhost_user_session_set_coalescing(struct spdk_vhost_dev *dev,
				      struct spdk_vhost_session *vsession, void *ctx);
int vhost_user_dev_set_coalescing(struct spdk_vhost_user_dev *user_dev, uint32_t delay_base_us,
				  uint32_t iops_threshold);
int vhost_user_dev_register(struct spdk_vhost_dev *vdev, const char *name,
			    struct spdk_cpuset *cpumask, const struct spdk_vhost_user_dev_backend *user_backend);
int vhost_user_dev_unregister(struct spdk_vhost_dev *vdev);
int vhost_user_init(void);
void vhost_user_fini(spdk_vhost_fini_cb vhost_cb);
int vhost_user_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);
void vhost_user_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);

int virtio_blk_construct_ctrlr(struct spdk_vhost_dev *vdev, const char *address,
			       struct spdk_cpuset *cpumask, const struct spdk_json_val *params,
			       const struct spdk_vhost_user_dev_backend *user_backend);
int virtio_blk_destroy_ctrlr(struct spdk_vhost_dev *vdev);

struct spdk_vhost_blk_task;

typedef void (*virtio_blk_request_cb)(uint8_t status, struct spdk_vhost_blk_task *task,
				      void *cb_arg);

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	virtio_blk_request_cb cb;
	void *cb_arg;

	volatile uint8_t *status;

	/* for io wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	struct spdk_io_channel *bdev_io_wait_ch;
	struct spdk_vhost_dev *bdev_io_wait_vdev;

	/** Number of bytes that were written. */
	uint32_t used_len;
	uint16_t iovcnt;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];

	/** Size of whole payload in bytes */
	uint32_t payload_size;
};

int virtio_blk_process_request(struct spdk_vhost_dev *vdev, struct spdk_io_channel *ch,
			       struct spdk_vhost_blk_task *task, virtio_blk_request_cb cb, void *cb_arg);

typedef void (*bdev_event_cb_complete)(struct spdk_vhost_dev *vdev, void *ctx);

#define SPDK_VIRTIO_BLK_TRSTRING_MAX_LEN 32

struct spdk_virtio_blk_transport_ops {
	/**
	 * Transport name
	 */
	char name[SPDK_VIRTIO_BLK_TRSTRING_MAX_LEN];

	/**
	 * Create a transport for the given transport opts
	 */
	struct spdk_virtio_blk_transport *(*create)(const struct spdk_json_val *params);

	/**
	 * Dump transport-specific opts into JSON
	 */
	void (*dump_opts)(struct spdk_virtio_blk_transport *transport, struct spdk_json_write_ctx *w);

	/**
	 * Destroy the transport
	 */
	int (*destroy)(struct spdk_virtio_blk_transport *transport,
		       spdk_vhost_fini_cb cb_fn);

	/**
	 * Create vhost block controller
	 */
	int (*create_ctrlr)(struct spdk_vhost_dev *vdev, struct spdk_cpuset *cpumask,
			    const char *address, const struct spdk_json_val *params,
			    void *custom_opts);

	/**
	 * Destroy vhost block controller
	 */
	int (*destroy_ctrlr)(struct spdk_vhost_dev *vdev);

	/*
	 * Signal removal of the bdev.
	 */
	void (*bdev_event)(enum spdk_bdev_event_type type, struct spdk_vhost_dev *vdev,
			   bdev_event_cb_complete cb, void *cb_arg);

	/**
	 * Set coalescing parameters.
	 */
	int (*set_coalescing)(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);

	/**
	 * Get coalescing parameters.
	 */
	void (*get_coalescing)(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);
};

struct spdk_virtio_blk_transport {
	const struct spdk_virtio_blk_transport_ops	*ops;
	TAILQ_ENTRY(spdk_virtio_blk_transport)		tailq;
};

struct virtio_blk_transport_ops_list_element {
	struct spdk_virtio_blk_transport_ops			ops;
	TAILQ_ENTRY(virtio_blk_transport_ops_list_element)	link;
};

void virtio_blk_transport_register(const struct spdk_virtio_blk_transport_ops *ops);
int virtio_blk_transport_create(const char *transport_name, const struct spdk_json_val *params);
int virtio_blk_transport_destroy(struct spdk_virtio_blk_transport *transport,
				 spdk_vhost_fini_cb cb_fn);
struct spdk_virtio_blk_transport *virtio_blk_transport_get_first(void);
struct spdk_virtio_blk_transport *virtio_blk_transport_get_next(
	struct spdk_virtio_blk_transport *transport);
void virtio_blk_transport_dump_opts(struct spdk_virtio_blk_transport *transport,
				    struct spdk_json_write_ctx *w);
struct spdk_virtio_blk_transport *virtio_blk_tgt_get_transport(const char *transport_name);
const struct spdk_virtio_blk_transport_ops *virtio_blk_get_transport_ops(
	const char *transport_name);


/*
 * Macro used to register new transports.
 */
#define SPDK_VIRTIO_BLK_TRANSPORT_REGISTER(name, transport_ops) \
static void __attribute__((constructor)) _virtio_blk_transport_register_##name(void) \
{ \
	virtio_blk_transport_register(transport_ops); \
}

#endif /* SPDK_VHOST_INTERNAL_H */
