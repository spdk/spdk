/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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

#ifndef SPDK_VHOST_INTERNAL_H
#define SPDK_VHOST_INTERNAL_H
#include <linux/virtio_config.h>

#include "spdk/stdinc.h"

#include <rte_vhost.h>

#include "spdk_internal/vhost_user.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/config.h"

extern bool g_packed_ring_recovery;

/**
 * DPDK calls our callbacks synchronously but the work those callbacks
 * perform needs to be async. Luckily, all DPDK callbacks are called on
 * a DPDK-internal pthread, so we'll just wait on a semaphore in there.
 */
extern sem_t g_dpdk_sem;

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
	(1ULL << VIRTIO_F_RING_PACKED))

#define SPDK_VHOST_DISABLED_FEATURES ((1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY))

#define VRING_DESC_F_AVAIL	(1ULL << VRING_PACKED_DESC_F_AVAIL)
#define VRING_DESC_F_USED	(1ULL << VRING_PACKED_DESC_F_USED)
#define VRING_DESC_F_AVAIL_USED	(VRING_DESC_F_AVAIL | VRING_DESC_F_USED)

typedef struct rte_vhost_resubmit_desc spdk_vhost_resubmit_desc;
typedef struct rte_vhost_resubmit_info spdk_vhost_resubmit_info;
typedef struct rte_vhost_inflight_desc_packed	spdk_vhost_inflight_desc;

/* Path to folder where character device will be created. Can be set by user. */
extern char g_vhost_user_dev_dirname[PATH_MAX];

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

	bool initialized;
	bool started;
	bool needs_restart;
	bool forced_polling;
	bool interrupt_mode;
	bool skip_used_signal;

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

struct spdk_vhost_dev {
	char *name;
	char *path;

	struct spdk_thread *thread;
	bool registered;

	uint64_t virtio_features;
	uint64_t disabled_features;
	uint64_t protocol_features;

	const struct spdk_vhost_dev_backend *backend;

	/* Saved original values used to setup coalescing to avoid integer
	 * rounding issues during save/load config.
	 */
	uint32_t coalescing_delay_us;
	uint32_t coalescing_iops_threshold;

	/* Current connections to the device */
	TAILQ_HEAD(, spdk_vhost_session) vsessions;

	/* Increment-only session counter */
	uint64_t vsessions_num;

	/* Number of started and actively polled sessions */
	uint32_t active_session_num;

	/* Number of pending asynchronous operations */
	uint32_t pending_async_op_num;

	TAILQ_ENTRY(spdk_vhost_dev) tailq;
};

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

struct spdk_vhost_dev_backend {
	/**
	 * Size of additional per-session context data
	 * allocated whenever a new client connects.
	 */
	size_t session_ctx_size;

	spdk_vhost_session_fn start_session;
	int (*stop_session)(struct spdk_vhost_session *vsession);

	int (*vhost_get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);
	int (*vhost_set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
				uint32_t offset, uint32_t size, uint32_t flags);

	void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	int (*remove_device)(struct spdk_vhost_dev *vdev);
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
 * Send IRQs for all queues that need to be signaled.
 * \param vsession vhost session
 * \param vq virtqueue
 */
void vhost_session_used_signal(struct spdk_vhost_session *vsession);

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

static inline bool __attribute__((always_inline))
vhost_dev_has_feature(struct spdk_vhost_session *vsession, unsigned feature_id)
{
	return vsession->negotiated_features & (1ULL << feature_id);
}

int vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		       const struct spdk_vhost_dev_backend *backend);
int vhost_dev_unregister(struct spdk_vhost_dev *vdev);

void vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

/*
 * Vhost callbacks for vhost_device_ops interface
 */

int vhost_new_connection_cb(int vid, const char *ifname);
int vhost_start_device_cb(int vid);
int vhost_stop_device_cb(int vid);
int vhost_destroy_connection_cb(int vid);

/*
 * Set vhost session to run in interrupt or poll mode
 */
void vhost_session_set_interrupt_mode(struct spdk_vhost_session *vsession, bool interrupt_mode);

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
void vhost_dev_foreach_session(struct spdk_vhost_dev *dev,
			       spdk_vhost_session_fn fn,
			       spdk_vhost_dev_fn cpl_fn,
			       void *arg);

/**
 * Call a function on the provided lcore and block until either
 * spdk_vhost_session_start_done() or spdk_vhost_session_stop_done()
 * is called.
 *
 * This must be called under the global vhost mutex, which this function
 * will unlock for the time it's waiting. It's meant to be called only
 * from start/stop session callbacks.
 *
 * \param vsession vhost session
 * \param cb_fn the function to call. The void *arg parameter in cb_fn
 * is always NULL.
 * \param timeout_sec timeout in seconds. This function will still
 * block after the timeout expires, but will print the provided errmsg.
 * \param errmsg error message to print once the timeout expires
 * \return return the code passed to spdk_vhost_session_event_done().
 */
int vhost_session_send_event(struct spdk_vhost_session *vsession,
			     spdk_vhost_session_fn cb_fn, unsigned timeout_sec,
			     const char *errmsg);

/**
 * Finish a blocking spdk_vhost_session_send_event() call and finally
 * start the session. This must be called on the target lcore, which
 * will now receive all session-related messages (e.g. from
 * spdk_vhost_dev_foreach_session()).
 *
 * Must be called under the global vhost lock.
 *
 * \param vsession vhost session
 * \param response return code
 */
void vhost_session_start_done(struct spdk_vhost_session *vsession, int response);

/**
 * Finish a blocking spdk_vhost_session_send_event() call and finally
 * stop the session. This must be called on the session's lcore which
 * used to receive all session-related messages (e.g. from
 * spdk_vhost_dev_foreach_session()). After this call, the session-
 * related messages will be once again processed by any arbitrary thread.
 *
 * Must be called under the global vhost lock.
 *
 * Must be called under the global vhost mutex.
 *
 * \param vsession vhost session
 * \param response return code
 */
void vhost_session_stop_done(struct spdk_vhost_session *vsession, int response);

struct spdk_vhost_session *vhost_session_find_by_vid(int vid);
void vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession);
int vhost_register_unix_socket(const char *path, const char *ctrl_name,
			       uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features);
int vhost_driver_unregister(const char *path);
int vhost_get_mem_table(int vid, struct rte_vhost_memory **mem);
int vhost_get_negotiated_features(int vid, uint64_t *negotiated_features);

int remove_vhost_controller(struct spdk_vhost_dev *vdev);

/* Function calls from vhost.c to rte_vhost_user.c,
 * shall removed once virtio transport abstraction is complete. */
int vhost_user_session_set_coalescing(struct spdk_vhost_dev *vdev,
				      struct spdk_vhost_session *vsession, void *ctx);
int vhost_user_dev_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
				  uint32_t iops_threshold);

#endif /* SPDK_VHOST_INTERNAL_H */
