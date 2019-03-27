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

#include "spdk/stdinc.h"

#include <rte_vhost.h>

#include "spdk_internal/log.h"
#include "spdk/event.h"
#include "spdk/rpc.h"
#include "spdk/config.h"

#define SPDK_CACHE_LINE_SIZE RTE_CACHE_LINE_SIZE

#ifndef VHOST_USER_F_PROTOCOL_FEATURES
#define VHOST_USER_F_PROTOCOL_FEATURES	30
#endif

#ifndef VIRTIO_F_VERSION_1
#define VIRTIO_F_VERSION_1 32
#endif

#ifndef VIRTIO_BLK_F_MQ
#define VIRTIO_BLK_F_MQ		12	/* support more than one vq */
#endif

#ifndef VIRTIO_BLK_F_CONFIG_WCE
#define VIRTIO_BLK_F_CONFIG_WCE	11
#endif

#define SPDK_VHOST_MAX_VQUEUES	256
#define SPDK_VHOST_MAX_VQ_SIZE	1024

#define SPDK_VHOST_SCSI_CTRLR_MAX_DEVS 8

#define SPDK_VHOST_IOVS_MAX 129

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
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC))

#define SPDK_VHOST_DISABLED_FEATURES ((1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY))

struct spdk_vhost_virtqueue {
	struct rte_vhost_vring vring;
	uint16_t last_avail_idx;
	uint16_t last_used_idx;

	void *tasks;

	/* Request count from last stats check */
	uint32_t req_cnt;

	/* Request count from last event */
	uint16_t used_req_cnt;

	/* How long interrupt is delayed */
	uint32_t irq_delay_time;

	/* Next time when we need to send event */
	uint64_t next_event_time;

} __attribute((aligned(SPDK_CACHE_LINE_SIZE)));

struct spdk_vhost_session {
	struct spdk_vhost_dev *vdev;

	/* rte_vhost connection ID. */
	int vid;

	/* Unique session ID. */
	unsigned id;

	int32_t lcore;

	bool initialized;
	bool needs_restart;
	bool forced_polling;

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

	struct spdk_vhost_virtqueue virtqueue[SPDK_VHOST_MAX_VQUEUES];

	TAILQ_ENTRY(spdk_vhost_session) tailq;

	struct spdk_vhost_session_fn_ctx *event_ctx;
};

struct spdk_vhost_dev {
	char *name;
	char *path;

	struct spdk_cpuset *cpumask;
	bool registered;

	const struct spdk_vhost_dev_backend *backend;

	/* Saved orginal values used to setup coalescing to avoid integer
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
 * Synchronized vhost session event used for backend callbacks.
 *
 * \param vdev vhost device. If the device has been deleted
 * in the meantime, this function will be called one last
 * time with vdev == NULL.
 * \param vsession vhost session. If all sessions have been
 * iterated through, this function will be called one last
 * time with vsession == NULL.
 * \param arg user-provided parameter.
 *
 * \return negative values will break the foreach call, meaning
 * the function won't be called again. Return codes zero and
 * positive don't have any effect.
 */
typedef int (*spdk_vhost_session_fn)(struct spdk_vhost_dev *vdev,
				     struct spdk_vhost_session *vsession,
				     void *arg);

struct spdk_vhost_dev_backend {
	uint64_t virtio_features;
	uint64_t disabled_features;

	/**
	 * Size of additional per-session context data
	 * allocated whenever a new client connects.
	 */
	size_t session_ctx_size;

	int (*start_session)(struct spdk_vhost_session *vsession);
	int (*stop_session)(struct spdk_vhost_session *vsession);

	int (*vhost_get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);
	int (*vhost_set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
				uint32_t offset, uint32_t size, uint32_t flags);

	void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	int (*remove_device)(struct spdk_vhost_dev *vdev);
};

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len);

uint16_t spdk_vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *vq, uint16_t *reqs,
				      uint16_t reqs_len);

/**
 * Get a virtio descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accesible via
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
int spdk_vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq,
			   uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
			   uint32_t *desc_table_size);

/**
 * Send IRQ/call client (if pending) for \c vq.
 * \param vsession vhost session
 * \param vq virtqueue
 * \return
 *   0 - if no interrupt was signalled
 *   1 - if interrupt was signalled
 */
int spdk_vhost_vq_used_signal(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *vq);


/**
 * Send IRQs for all queues that need to be signaled.
 * \param vsession vhost session
 * \param vq virtqueue
 */
void spdk_vhost_session_used_signal(struct spdk_vhost_session *vsession);

void spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
				     struct spdk_vhost_virtqueue *vq,
				     uint16_t id, uint32_t len);

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
int spdk_vhost_vring_desc_get_next(struct vring_desc **desc,
				   struct vring_desc *desc_table, uint32_t desc_table_size);
bool spdk_vhost_vring_desc_is_wr(struct vring_desc *cur_desc);

int spdk_vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				 uint16_t *iov_index, const struct vring_desc *desc);

static inline bool __attribute__((always_inline))
spdk_vhost_dev_has_feature(struct spdk_vhost_session *vsession, unsigned feature_id)
{
	return vsession->negotiated_features & (1ULL << feature_id);
}

int spdk_vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			    const struct spdk_vhost_dev_backend *backend);
int spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev);

int spdk_vhost_scsi_controller_construct(void);
int spdk_vhost_blk_controller_construct(void);
void spdk_vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

/*
 * Call function for each active session on the provided
 * vhost device. The function will be called one-by-one
 * on each session's thread.
 *
 * \param vdev vhost device
 * \param fn function to call
 * \param arg additional argument to \c fn
 */
void spdk_vhost_dev_foreach_session(struct spdk_vhost_dev *dev,
				    spdk_vhost_session_fn fn, void *arg);

/**
 * Call a function on the provided lcore and block until either
 * spdk_vhost_session_start_done() or spdk_vhost_session_stop_done()
 * is called.
 *
 * This must be called under the global vhost mutex, which this function
 * will unlock for the time it's waiting. It's meant to be called only
 * from start/stop session callbacks.
 *
 * \param lcore target session's lcore
 * \param vsession vhost session
 * \param cb_fn the function to call. The void *arg parameter in cb_fn
 * is always NULL.
 * \param timeout_sec timeout in seconds. This function will still
 * block after the timeout expires, but will print the provided errmsg.
 * \param errmsg error message to print once the timeout expires
 * \return return the code passed to spdk_vhost_session_event_done().
 */
int spdk_vhost_session_send_event(int32_t lcore, struct spdk_vhost_session *vsession,
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
void spdk_vhost_session_start_done(struct spdk_vhost_session *vsession, int response);

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
void spdk_vhost_session_stop_done(struct spdk_vhost_session *vsession, int response);

struct spdk_vhost_session *spdk_vhost_session_find_by_vid(int vid);
void spdk_vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession);
void spdk_vhost_dev_install_rte_compat_hooks(struct spdk_vhost_dev *vdev);

void spdk_vhost_free_reactor(uint32_t lcore);
uint32_t spdk_vhost_allocate_reactor(struct spdk_cpuset *cpumask);

int spdk_remove_vhost_controller(struct spdk_vhost_dev *vdev);

#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
int spdk_vhost_nvme_admin_passthrough(int vid, void *cmd, void *cqe, void *buf);
int spdk_vhost_nvme_set_cq_call(int vid, uint16_t qid, int fd);
int spdk_vhost_nvme_set_bar_mr(int vid, void *bar_addr, uint64_t bar_size);
int spdk_vhost_nvme_get_cap(int vid, uint64_t *cap);
int spdk_vhost_nvme_controller_construct(void);
int spdk_vhost_nvme_dev_construct(const char *name, const char *cpumask, uint32_t io_queues);
int spdk_vhost_nvme_dev_remove(struct spdk_vhost_dev *vdev);
int spdk_vhost_nvme_dev_add_ns(struct spdk_vhost_dev *vdev,
			       const char *bdev_name);
#endif

#endif /* SPDK_VHOST_INTERNAL_H */
