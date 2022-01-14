/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

TAILQ_HEAD(nvme_bdev_ctrlrs, nvme_bdev_ctrlr);
extern struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs;
extern pthread_mutex_t g_bdev_nvme_mutex;
extern bool g_bdev_nvme_module_finish;

#define NVME_MAX_CONTROLLERS 1024

typedef void (*spdk_bdev_create_nvme_fn)(void *ctx, size_t bdev_count, int rc);
typedef void (*spdk_bdev_nvme_start_discovery_fn)(void *ctx, int rc);
typedef void (*spdk_bdev_nvme_stop_discovery_fn)(void *ctx);

struct nvme_async_probe_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
	const char *base_name;
	const char **names;
	uint32_t count;
	uint32_t prchk_flags;
	int32_t ctrlr_loss_timeout_sec;
	uint32_t reconnect_delay_sec;
	uint32_t fast_io_fail_timeout_sec;
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

struct nvme_ns {
	uint32_t			id;
	struct spdk_nvme_ns		*ns;
	struct nvme_ctrlr		*ctrlr;
	struct nvme_bdev		*bdev;
	uint32_t			ana_group_id;
	enum spdk_nvme_ana_state	ana_state;
	bool				ana_state_updating;
	struct nvme_async_probe_ctx	*probe_ctx;
	TAILQ_ENTRY(nvme_ns)		tailq;
	RB_ENTRY(nvme_ns)		node;
};

struct nvme_bdev_io;
struct nvme_bdev_ctrlr;
struct nvme_bdev;
struct nvme_io_path;

struct nvme_path_id {
	struct spdk_nvme_transport_id		trid;
	struct spdk_nvme_host_id		hostid;
	TAILQ_ENTRY(nvme_path_id)		link;
	bool					is_failed;
};

typedef void (*bdev_nvme_reset_cb)(void *cb_arg, bool success);

struct nvme_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr			*ctrlr;
	struct nvme_path_id			*active_path_id;
	int					ref;

	uint32_t				resetting : 1;
	uint32_t				reconnect_is_delayed : 1;
	uint32_t				fast_io_fail_timedout : 1;
	uint32_t				destruct : 1;
	uint32_t				ana_log_page_updating : 1;
	/**
	 * PI check flags. This flags is set to NVMe controllers created only
	 * through bdev_nvme_attach_controller RPC or .INI config file. Hot added
	 * NVMe controllers are not included.
	 */
	uint32_t				prchk_flags;
	RB_HEAD(nvme_ns_tree, nvme_ns)		namespaces;

	struct spdk_opal_dev			*opal_dev;

	struct spdk_poller			*adminq_timer_poller;
	struct spdk_thread			*thread;

	bdev_nvme_reset_cb			reset_cb_fn;
	void					*reset_cb_arg;
	/* Poller used to check for reset/detach completion */
	struct spdk_poller			*reset_detach_poller;
	struct spdk_nvme_detach_ctx		*detach_ctx;

	uint64_t				reset_start_tsc;
	struct spdk_poller			*reconnect_delay_timer;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)			tailq;
	struct nvme_bdev_ctrlr			*nbdev_ctrlr;

	TAILQ_HEAD(nvme_paths, nvme_path_id)	trids;

	uint32_t				ana_log_page_size;
	struct spdk_nvme_ana_page		*ana_log_page;
	struct spdk_nvme_ana_group_descriptor	*copied_ana_desc;

	struct nvme_async_probe_ctx		*probe_ctx;

	uint32_t				reconnect_delay_sec;
	int32_t					ctrlr_loss_timeout_sec;
	uint32_t				fast_io_fail_timeout_sec;

	pthread_mutex_t				mutex;
};

struct nvme_bdev_ctrlr {
	char				*name;
	TAILQ_HEAD(, nvme_ctrlr)	ctrlrs;
	TAILQ_HEAD(, nvme_bdev)		bdevs;
	TAILQ_ENTRY(nvme_bdev_ctrlr)	tailq;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	uint32_t		nsid;
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	pthread_mutex_t		mutex;
	int			ref;
	TAILQ_HEAD(, nvme_ns)	nvme_ns_list;
	bool			opal;
	TAILQ_ENTRY(nvme_bdev)	tailq;
};

struct nvme_ctrlr_channel {
	struct spdk_nvme_qpair		*qpair;
	struct nvme_poll_group		*group;
	TAILQ_HEAD(, spdk_bdev_io)	pending_resets;
	TAILQ_ENTRY(nvme_ctrlr_channel)	tailq;

	/* The following is used to update io_path cache of nvme_bdev_channels. */
	TAILQ_HEAD(, nvme_io_path)	io_path_list;

};

#define nvme_ctrlr_channel_get_ctrlr(ctrlr_ch)	\
	(struct nvme_ctrlr *)spdk_io_channel_get_io_device(spdk_io_channel_from_ctx(ctrlr_ch))

struct nvme_io_path {
	struct nvme_ns			*nvme_ns;
	struct nvme_ctrlr_channel	*ctrlr_ch;
	STAILQ_ENTRY(nvme_io_path)	stailq;

	/* The following are used to update io_path cache of the nvme_bdev_channel. */
	struct nvme_bdev_channel	*nbdev_ch;
	TAILQ_ENTRY(nvme_io_path)	tailq;
};

struct nvme_bdev_channel {
	struct nvme_io_path			*current_io_path;
	STAILQ_HEAD(, nvme_io_path)		io_path_list;
	TAILQ_HEAD(retry_io_head, spdk_bdev_io)	retry_io_list;
	struct spdk_poller			*retry_io_poller;
};

struct nvme_poll_group {
	struct spdk_nvme_poll_group		*group;
	struct spdk_io_channel			*accel_channel;
	struct spdk_poller			*poller;
	bool					collect_spin_stat;
	uint64_t				spin_ticks;
	uint64_t				start_ticks;
	uint64_t				end_ticks;
	TAILQ_HEAD(, nvme_ctrlr_channel)	ctrlr_ch_list;
};

struct nvme_ctrlr *nvme_ctrlr_get_by_name(const char *name);

struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get_by_name(const char *name);

typedef void (*nvme_bdev_ctrlr_for_each_fn)(struct nvme_bdev_ctrlr *nbdev_ctrlr, void *ctx);

void nvme_bdev_ctrlr_for_each(nvme_bdev_ctrlr_for_each_fn fn, void *ctx);

void nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

struct nvme_ns *nvme_ctrlr_get_ns(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid);
struct nvme_ns *nvme_ctrlr_get_first_active_ns(struct nvme_ctrlr *nvme_ctrlr);
struct nvme_ns *nvme_ctrlr_get_next_active_ns(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *ns);

enum spdk_bdev_timeout_action {
	SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE = 0,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT,
};

struct spdk_bdev_nvme_opts {
	enum spdk_bdev_timeout_action action_on_timeout;
	uint64_t timeout_us;
	uint64_t timeout_admin_us;
	uint32_t keep_alive_timeout_ms;
	/* The number of attempts per I/O in the transport layer before an I/O fails. */
	uint32_t transport_retry_count;
	uint32_t arbitration_burst;
	uint32_t low_priority_weight;
	uint32_t medium_priority_weight;
	uint32_t high_priority_weight;
	uint64_t nvme_adminq_poll_period_us;
	uint64_t nvme_ioq_poll_period_us;
	uint32_t io_queue_requests;
	bool delay_cmd_submit;
	/* The number of attempts per I/O in the bdev layer before an I/O fails. */
	int32_t bdev_retry_count;
};

struct spdk_nvme_qpair *bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch);
void bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts);
int bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts);
int bdev_nvme_set_hotplug(bool enabled, uint64_t period_us, spdk_msg_fn cb, void *cb_ctx);

int bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		     const char *base_name,
		     const char **names,
		     uint32_t count,
		     uint32_t prchk_flags,
		     spdk_bdev_create_nvme_fn cb_fn,
		     void *cb_ctx,
		     struct spdk_nvme_ctrlr_opts *opts,
		     bool multipath,
		     int32_t ctrlr_loss_timeout_sec,
		     uint32_t reconnect_delay_sec,
		     uint32_t fast_io_fail_timeout_sec);

int bdev_nvme_start_discovery(struct spdk_nvme_transport_id *trid, const char *base_name,
			      struct spdk_nvme_ctrlr_opts *opts,
			      spdk_bdev_nvme_start_discovery_fn cb_fn, void *cb_ctx);
int bdev_nvme_stop_discovery(const char *name, spdk_bdev_nvme_stop_discovery_fn cb_fn,
			     void *cb_ctx);

struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

/**
 * Delete NVMe controller with all bdevs on top of it, or delete the specified path
 * if there is any alternative path. Requires to pass name of NVMe controller.
 *
 * \param name NVMe controller name
 * \param path_id The specified path to remove (optional)
 * \return zero on success, -EINVAL on wrong parameters or -ENODEV if controller is not found
 */
int bdev_nvme_delete(const char *name, const struct nvme_path_id *path_id);

/**
 * Reset NVMe controller.
 *
 * \param nvme_ctrlr The specified NVMe controller to reset
 * \param cb_fn Function to be called back after reset completes
 * \param cb_arg Argument for callback function
 * \return zero on success. Negated errno on the following error conditions:
 * -ENXIO: controller is being destroyed.
 * -EBUSY: controller is already being reset.
 */
int bdev_nvme_reset_rpc(struct nvme_ctrlr *nvme_ctrlr, bdev_nvme_reset_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_NVME_H */
