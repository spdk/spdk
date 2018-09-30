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
#define SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS 10
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

struct spdk_vhost_dev_backend {
	uint64_t virtio_features;
	uint64_t disabled_features;

	/**
	 * Callbacks for starting and pausing the device.
	 * The first param is struct spdk_vhost_dev *.
	 * The second one is event context that has to be
	 * passed to spdk_vhost_dev_backend_event_done().
	 */
	spdk_vhost_event_fn start_device;
	spdk_vhost_event_fn stop_device;

	int (*vhost_get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);
	int (*vhost_set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
				uint32_t offset, uint32_t size, uint32_t flags);

	void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
	int (*remove_device)(struct spdk_vhost_dev *vdev);
};

struct spdk_vhost_dev {
	struct rte_vhost_memory *mem;
	char *name;
	char *path;

	/* Unique device ID. */
	unsigned id;

	/* rte_vhost device ID. */
	int vid;
	int task_cnt;
	int32_t lcore;
	struct spdk_cpuset *cpumask;
	bool registered;

	const struct spdk_vhost_dev_backend *backend;

	/* Saved orginal values used to setup coalescing to avoid integer
	 * rounding issues during save/load config.
	 */
	uint32_t coalescing_delay_us;
	uint32_t coalescing_iops_threshold;

	uint32_t coalescing_delay_time_base;

	/* Threshold when event coalescing for virtqueue will be turned on. */
	uint32_t  coalescing_io_rate_threshold;

	/* Next time when stats for event coalescing will be checked. */
	uint64_t next_stats_check_time;

	/* Interval used for event coalescing checking. */
	uint64_t stats_check_interval;

	uint16_t max_queues;

	uint64_t negotiated_features;

	struct spdk_vhost_virtqueue virtqueue[SPDK_VHOST_MAX_VQUEUES];

	TAILQ_ENTRY(spdk_vhost_dev) tailq;
};

struct spdk_vhost_dev_destroy_ctx {
	struct spdk_poller *poller;
	void *event_ctx;
};

struct spdk_vhost_dev *spdk_vhost_dev_find(const char *ctrlr_name);

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr, uint64_t len);

uint16_t spdk_vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *vq, uint16_t *reqs,
				      uint16_t reqs_len);

/**
 * Get a virtio descriptor at given index in given virtqueue.
 * The descriptor will provide access to the entire descriptor
 * chain. The subsequent descriptors are accesible via
 * \c spdk_vhost_vring_desc_get_next.
 * \param vdev vhost device
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
int spdk_vhost_vq_get_desc(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *vq,
			   uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
			   uint32_t *desc_table_size);

/**
 * Send IRQ/call client (if pending) for \c vq.
 * \param vdev vhost device
 * \param vq virtqueue
 * \return
 *   0 - if no interrupt was signalled
 *   1 - if interrupt was signalled
 */
int spdk_vhost_vq_used_signal(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *vq);


/**
 * Send IRQs for all queues that need to be signaled.
 * \param vdev vhost device
 * \param vq virtqueue
 */
void spdk_vhost_dev_used_signal(struct spdk_vhost_dev *vdev);

void spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *vq,
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

int spdk_vhost_vring_desc_to_iov(struct spdk_vhost_dev *vdev, struct iovec *iov,
				 uint16_t *iov_index, const struct vring_desc *desc);

static inline bool __attribute__((always_inline))
spdk_vhost_dev_has_feature(struct spdk_vhost_dev *vdev, unsigned feature_id)
{
	return vdev->negotiated_features & (1ULL << feature_id);
}

int spdk_vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			    const struct spdk_vhost_dev_backend *backend);
int spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev);

int spdk_vhost_scsi_controller_construct(void);
int spdk_vhost_blk_controller_construct(void);
void spdk_vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
void spdk_vhost_dev_backend_event_done(void *event_ctx, int response);
void spdk_vhost_lock(void);
void spdk_vhost_unlock(void);
int spdk_remove_vhost_controller(struct spdk_vhost_dev *vdev);
int spdk_vhost_nvme_admin_passthrough(int vid, void *cmd, void *cqe, void *buf);
int spdk_vhost_nvme_set_cq_call(int vid, uint16_t qid, int fd);
int spdk_vhost_nvme_get_cap(int vid, uint64_t *cap);
int spdk_vhost_nvme_controller_construct(void);
int spdk_vhost_nvme_dev_construct(const char *name, const char *cpumask, uint32_t io_queues);
int spdk_vhost_nvme_dev_remove(struct spdk_vhost_dev *vdev);
int spdk_vhost_nvme_dev_add_ns(struct spdk_vhost_dev *vdev,
			       const char *bdev_name);

#endif /* SPDK_VHOST_INTERNAL_H */
