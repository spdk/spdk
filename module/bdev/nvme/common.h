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

#ifndef SPDK_COMMON_BDEV_NVME_H
#define SPDK_COMMON_BDEV_NVME_H

#include "spdk/nvme.h"
#include "spdk/bdev_module.h"
#include "spdk/opal.h"

TAILQ_HEAD(nvme_bdev_ctrlrs, nvme_bdev_ctrlr);
extern struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs;
extern pthread_mutex_t g_bdev_nvme_mutex;
extern bool g_bdev_nvme_module_finish;

#define NVME_MAX_CONTROLLERS 1024

enum nvme_bdev_ns_type {
	NVME_BDEV_NS_UNKNOWN	= 0,
	NVME_BDEV_NS_STANDARD	= 1,
	NVME_BDEV_NS_OCSSD	= 2,
};

struct nvme_bdev_ns {
	uint32_t		id;
	enum nvme_bdev_ns_type	type;
	/** Marks whether this data structure has its bdevs
	 *  populated for the associated namespace.  It is used
	 *  to keep track if we need manage the populated
	 *  resources when a newly active namespace is found,
	 *  or when a namespace becomes inactive.
	 */
	bool			populated;
	struct spdk_nvme_ns	*ns;
	struct nvme_bdev_ctrlr	*ctrlr;
	TAILQ_HEAD(, nvme_bdev)	bdevs;
	void			*type_ctx;
};

struct ocssd_bdev_ctrlr;

struct nvme_bdev_ctrlr_trid {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(nvme_bdev_ctrlr_trid)	link;
};

struct nvme_bdev_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_transport_id		*connected_trid;
	char					*name;
	int					ref;
	bool					resetting;
	bool					failover_in_progress;
	bool					destruct;
	/**
	 * PI check flags. This flags is set to NVMe controllers created only
	 * through bdev_nvme_attach_controller RPC or .INI config file. Hot added
	 * NVMe controllers are not included.
	 */
	uint32_t				prchk_flags;
	uint32_t				num_ns;
	/** Array of pointers to namespaces indexed by nsid - 1 */
	struct nvme_bdev_ns			**namespaces;

	struct spdk_opal_dev			*opal_dev;

	struct spdk_poller			*adminq_timer_poller;
	struct spdk_poller			*destruct_poller;
	struct spdk_thread			*thread;

	struct ocssd_bdev_ctrlr			*ocssd_ctrlr;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_bdev_ctrlr)		tailq;

	TAILQ_HEAD(, nvme_bdev_ctrlr_trid)	trids;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	struct nvme_bdev_ns	*nvme_ns;
	TAILQ_ENTRY(nvme_bdev)	tailq;
};

struct nvme_bdev_poll_group {
	struct spdk_nvme_poll_group		*group;
	struct spdk_poller			*poller;
	bool					collect_spin_stat;
	uint64_t				spin_ticks;
	uint64_t				start_ticks;
	uint64_t				end_ticks;
};

typedef void (*spdk_bdev_create_nvme_fn)(void *ctx, size_t bdev_count, int rc);

struct nvme_async_probe_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
	const char *base_name;
	const char **names;
	uint32_t count;
	uint32_t prchk_flags;
	struct spdk_poller *poller;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr_opts opts;
	spdk_bdev_create_nvme_fn cb_fn;
	void *cb_ctx;
	uint32_t populates_in_progress;
};

struct ocssd_io_channel;

struct nvme_io_channel {
	struct nvme_bdev_ctrlr		*ctrlr;
	struct spdk_nvme_qpair		*qpair;
	struct nvme_bdev_poll_group	*group;
	TAILQ_HEAD(, spdk_bdev_io)	pending_resets;
	struct ocssd_io_channel		*ocssd_ch;
};

void nvme_ctrlr_populate_namespace_done(struct nvme_async_probe_ctx *ctx,
					struct nvme_bdev_ns *ns, int rc);
void nvme_ctrlr_depopulate_namespace_done(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);

struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get(const struct spdk_nvme_transport_id *trid);
struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get_by_name(const char *name);
struct nvme_bdev_ctrlr *nvme_bdev_first_ctrlr(void);
struct nvme_bdev_ctrlr *nvme_bdev_next_ctrlr(struct nvme_bdev_ctrlr *prev);

void nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

int nvme_bdev_ctrlr_destruct(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr);
void nvme_bdev_attach_bdev_to_ns(struct nvme_bdev_ns *nvme_ns, struct nvme_bdev *nvme_disk);
void nvme_bdev_detach_bdev_from_ns(struct nvme_bdev *nvme_disk);

#endif /* SPDK_COMMON_BDEV_NVME_H */
