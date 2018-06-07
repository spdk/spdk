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

#ifndef SPDK_BDEV_NVME_H
#define SPDK_BDEV_NVME_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"

#define NVME_MAX_CONTROLLERS 1024

struct nvme_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_transport_id	trid;
	char				*name;
	int				ref;

	struct spdk_poller		*adminq_timer_poller;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)	tailq;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	struct nvme_ctrlr	*nvme_ctrlr;
	struct spdk_nvme_ns	*ns;

	TAILQ_ENTRY(nvme_bdev)	link;
};

int spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  const char **names, size_t *count);
struct spdk_nvme_ctrlr *spdk_bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

#endif // SPDK_BDEV_NVME_H
