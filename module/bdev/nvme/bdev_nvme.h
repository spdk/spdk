/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#ifndef SPDK_BDEV_NVME_H
#define SPDK_BDEV_NVME_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"

#include "common.h"

enum spdk_bdev_timeout_action {
	SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE = 0,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT,
};

struct spdk_bdev_nvme_opts {
	enum spdk_bdev_timeout_action action_on_timeout;
	uint64_t timeout_us;
	uint32_t retry_count;
	uint32_t arbitration_burst;
	uint32_t low_priority_weight;
	uint32_t medium_priority_weight;
	uint32_t high_priority_weight;
	uint64_t nvme_adminq_poll_period_us;
	uint64_t nvme_ioq_poll_period_us;
	uint32_t io_queue_requests;
	bool delay_cmd_submit;
};

struct spdk_nvme_qpair *bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch);
void bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts);
int bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts);
int bdev_nvme_set_hotplug(bool enabled, uint64_t period_us, spdk_msg_fn cb, void *cb_ctx);
int bdev_nvme_remove_trid(const char *name, struct spdk_nvme_transport_id *trid);

int bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_host_id *hostid,
		     const char *base_name,
		     const char **names,
		     uint32_t count,
		     const char *hostnqn,
		     uint32_t prchk_flags,
		     spdk_bdev_create_nvme_fn cb_fn,
		     void *cb_ctx);
struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

/**
 * Delete NVMe controller with all bdevs on top of it.
 * Requires to pass name of NVMe controller.
 *
 * \param name NVMe controller name
 * \return zero on success, -EINVAL on wrong parameters or -ENODEV if controller is not found
 */
int bdev_nvme_delete(const char *name);

#endif /* SPDK_BDEV_NVME_H */
