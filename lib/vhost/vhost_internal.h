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

#define SPDK_CACHE_LINE_SIZE RTE_CACHE_LINE_SIZE

#define MAX_VHOST_VRINGS	256

struct spdk_vhost_dev {
	struct rte_vhost_memory *mem;
	char *name;

	int vid;
	int task_cnt;
	int32_t lcore;
	uint64_t cpumask;

	uint16_t num_queues;
	uint64_t negotiated_features;
	struct rte_vhost_vring virtqueue[MAX_VHOST_VRINGS] __attribute((aligned(SPDK_CACHE_LINE_SIZE)));
};


struct spdk_vhost_dev_backend {
	uint64_t virtio_features;
	uint64_t disabled_features;
	const struct vhost_device_ops ops;
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

struct spdk_vhost_dev *spdk_vhost_dev_find_by_vid(int vid);
struct spdk_vhost_dev *spdk_vhost_dev_load(int vid);
int spdk_vhost_dev_register(struct spdk_vhost_dev *dev,
			    const struct spdk_vhost_dev_backend *backend);
int spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev);
void spdk_vhost_dev_unload(struct spdk_vhost_dev *dev);

#endif /* SPDK_VHOST_INTERNAL_H */
