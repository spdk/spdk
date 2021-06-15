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

#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"
#include "spdk/opal.h"

TAILQ_HEAD(nvme_ctrlrs, nvme_ctrlr);
extern struct nvme_ctrlrs g_nvme_ctrlrs;
extern pthread_mutex_t g_bdev_nvme_mutex;
extern bool g_bdev_nvme_module_finish;

#define NVME_MAX_CONTROLLERS 1024

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
	bool ctrlr_attached;
	bool probe_done;
	bool namespaces_populated;
};

enum nvme_ns_type {
	NVME_NS_UNKNOWN		= 0,
	NVME_NS_STANDARD	= 1,
	NVME_NS_OCSSD		= 2,
};

struct nvme_ns {
	uint32_t		id;
	enum nvme_ns_type	type;
	/** Marks whether this data structure has its bdevs
	 *  populated for the associated namespace.  It is used
	 *  to keep track if we need manage the populated
	 *  resources when a newly active namespace is found,
	 *  or when a namespace becomes inactive.
	 */
	bool			populated;
	struct spdk_nvme_ns	*ns;
	struct nvme_ctrlr	*ctrlr;
	struct nvme_bdev	*bdev;
	void			*type_ctx;
	uint32_t		ana_group_id;
	enum spdk_nvme_ana_state ana_state;
};

struct nvme_bdev_io;
struct ocssd_bdev_ctrlr;

struct nvme_ctrlr_trid {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(nvme_ctrlr_trid)		link;
	bool					is_failed;
};

typedef void (*bdev_nvme_reset_cb)(void *cb_arg, int rc);

struct nvme_ctrlr {
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
	bool					destruct_after_reset;
	/**
	 * PI check flags. This flags is set to NVMe controllers created only
	 * through bdev_nvme_attach_controller RPC or .INI config file. Hot added
	 * NVMe controllers are not included.
	 */
	uint32_t				prchk_flags;
	uint32_t				num_ns;
	/** Array of pointers to namespaces indexed by nsid - 1 */
	struct nvme_ns				**namespaces;

	struct spdk_opal_dev			*opal_dev;

	struct spdk_poller			*adminq_timer_poller;
	struct spdk_thread			*thread;

	struct ocssd_bdev_ctrlr			*ocssd_ctrlr;

	bdev_nvme_reset_cb			reset_cb_fn;
	void					*reset_cb_arg;
	struct spdk_nvme_ctrlr_reset_ctx	*reset_ctx;
	struct spdk_poller			*reset_poller;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)			tailq;

	TAILQ_HEAD(, nvme_ctrlr_trid)		trids;

	uint32_t				ana_log_page_size;
	struct spdk_nvme_ana_page		*ana_log_page;
	struct spdk_nvme_ana_group_descriptor	*copied_ana_desc;

	struct nvme_async_probe_ctx		*probe_ctx;

	pthread_mutex_t				mutex;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	struct nvme_ns		*nvme_ns;
	bool			opal;
};

struct nvme_poll_group {
	struct spdk_nvme_poll_group		*group;
	struct spdk_io_channel			*accel_channel;
	struct spdk_poller			*poller;
	bool					collect_spin_stat;
	uint64_t				spin_ticks;
	uint64_t				start_ticks;
	uint64_t				end_ticks;
};

struct ocssd_io_channel;

struct nvme_ctrlr_channel {
	struct nvme_ctrlr		*ctrlr;
	struct spdk_nvme_qpair		*qpair;
	struct nvme_poll_group		*group;
	TAILQ_HEAD(, spdk_bdev_io)	pending_resets;
	struct ocssd_io_channel		*ocssd_ch;
};

struct nvme_bdev_channel {
	struct nvme_ns			*nvme_ns;
	struct nvme_ctrlr_channel	*ctrlr_ch;
};

void nvme_ctrlr_populate_namespace_done(struct nvme_async_probe_ctx *ctx,
					struct nvme_ns *nvme_ns, int rc);
void nvme_ctrlr_depopulate_namespace_done(struct nvme_ns *nvme_ns);

struct nvme_ctrlr *nvme_ctrlr_get(const struct spdk_nvme_transport_id *trid);
struct nvme_ctrlr *nvme_ctrlr_get_by_name(const char *name);

typedef void (*nvme_ctrlr_for_each_fn)(struct nvme_ctrlr *nvme_ctrlr, void *ctx);

void nvme_ctrlr_for_each(nvme_ctrlr_for_each_fn fn, void *ctx);

void nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

void nvme_ctrlr_release(struct nvme_ctrlr *nvme_ctrlr);
void nvme_ctrlr_unregister(void *ctx);
void nvme_ctrlr_delete(struct nvme_ctrlr *nvme_ctrlr);

int bdev_nvme_create_bdev_channel_cb(void *io_device, void *ctx_buf);
void bdev_nvme_destroy_bdev_channel_cb(void *io_device, void *ctx_buf);

#endif /* SPDK_COMMON_BDEV_NVME_H */
