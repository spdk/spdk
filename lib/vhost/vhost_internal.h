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

#define SPDK_VHOST_SCSI_CTRLR_MAX_DEVS 8

#define SPDK_VHOST_IOVS_MAX 128

#define SPDK_VHOST_FEATURES ((1ULL << VHOST_F_LOG_ALL) | \
	(1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
	(1ULL << VIRTIO_F_VERSION_1) | \
	(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
	(1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC))

#define SPDK_VHOST_DISABLED_FEATURES ((1ULL << VHOST_F_LOG_ALL) | \
	(1ULL << VIRTIO_RING_F_EVENT_IDX) | \
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC))

enum spdk_vhost_dev_type {
	SPDK_VHOST_DEV_T_SCSI,
	SPDK_VHOST_DEV_T_BLK,
};

struct spdk_vhost_dev_backend {
	uint64_t virtio_features;
	uint64_t disabled_features;
	int (*new_device)(struct spdk_vhost_dev *);
	int (*destroy_device)(struct spdk_vhost_dev *);
	void (*dump_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);
};

struct spdk_vhost_dev {
	struct rte_vhost_memory *mem;
	char *name;
	char *path;

	int vid;
	int task_cnt;
	int32_t lcore;
	uint64_t cpumask;

	enum spdk_vhost_dev_type type;
	const struct spdk_vhost_dev_backend *backend;

	uint16_t num_queues;
	uint64_t negotiated_features;
	struct rte_vhost_vring virtqueue[SPDK_VHOST_MAX_VQUEUES] __attribute((aligned(
				SPDK_CACHE_LINE_SIZE)));
};

void spdk_vhost_dev_mem_register(struct spdk_vhost_dev *vdev);
void spdk_vhost_dev_mem_unregister(struct spdk_vhost_dev *vdev);

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr);

uint16_t spdk_vhost_vq_avail_ring_get(struct rte_vhost_vring *vq, uint16_t *reqs,
				      uint16_t reqs_len);
bool spdk_vhost_vq_should_notify(struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq);

struct vring_desc *spdk_vhost_vq_get_desc(struct rte_vhost_vring *vq, uint16_t req_idx);

void spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq,
				     uint16_t id, uint32_t len);
bool spdk_vhost_vring_desc_has_next(struct vring_desc *cur_desc);
struct vring_desc *spdk_vhost_vring_desc_get_next(struct vring_desc *vq_desc,
		struct vring_desc *cur_desc);
bool spdk_vhost_vring_desc_is_wr(struct vring_desc *cur_desc);

int spdk_vhost_vring_desc_to_iov(struct spdk_vhost_dev *vdev, struct iovec *iov,
				 uint16_t *iov_index, const struct vring_desc *desc);
bool spdk_vhost_dev_has_feature(struct spdk_vhost_dev *vdev, unsigned feature_id);

int spdk_vhost_dev_construct(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			     enum spdk_vhost_dev_type type, const struct spdk_vhost_dev_backend *backend);
int spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev);

typedef void (*spdk_vhost_timed_event_fn)(void *);

struct spdk_vhost_timed_event {
	/** User callback function to be executed on given lcore. */
	spdk_vhost_timed_event_fn cb_fn;

	/** Semaphore used to signal that event is done. */
	sem_t sem;

	/** Timout specified during initialization. */
	struct timespec timeout;

	/** Event object that can be passed to *spdk_event_call()*. */
	struct spdk_event *spdk_event;
};

void spdk_vhost_timed_event_init(struct spdk_vhost_timed_event *ev, int32_t lcore,
				 spdk_vhost_timed_event_fn cb_fn, void *arg, unsigned timeout_sec);

void spdk_vhost_timed_event_send(int32_t lcore, spdk_vhost_timed_event_fn cn_fn, void *arg,
				 unsigned timeout_sec, const char *errmsg);
void spdk_vhost_timed_event_wait(struct spdk_vhost_timed_event *event, const char *errmsg);

int spdk_vhost_blk_controller_construct(void);
void spdk_vhost_dump_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

#endif /* SPDK_VHOST_INTERNAL_H */
