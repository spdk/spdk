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

#ifndef SPDK_VHOST_TASK_H
#define SPDK_VHOST_TASK_H

#include "spdk/scsi.h"

#undef container_of
#define container_of(ptr, type, member) ({ \
		typeof(((type *)0)->member) *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })

/* Allocated iovec buffer len */
#define VHOST_SCSI_IOVS_LEN		128

struct spdk_vhost_dev;

struct spdk_vhost_task {
	struct spdk_scsi_task	scsi;
	struct iovec iovs[VHOST_SCSI_IOVS_LEN];

	union {
		struct virtio_scsi_cmd_resp *resp;
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	};

	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev *scsi_dev;

	int req_idx;

	struct rte_vhost_vring *vq;
};

void spdk_vhost_task_put(struct spdk_vhost_task *task);
struct spdk_vhost_task *spdk_vhost_task_get(struct spdk_vhost_scsi_dev *vdev,
		spdk_scsi_task_cpl cpl_fn);

void spdk_vhost_task_cpl(struct spdk_scsi_task *scsi_task);
void spdk_vhost_task_mgmt_cpl(struct spdk_scsi_task *scsi_task);

#endif /* SPDK_VHOST_TASK_H */
