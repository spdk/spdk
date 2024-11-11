/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2022 Dell Inc, or its subsidiaries. All rights reserved.
 */

#ifndef SPDK_BDEV_NVME_H
#define SPDK_BDEV_NVME_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"
#include "spdk/module/bdev/nvme.h"
#include "spdk/jsonrpc.h"

TAILQ_HEAD(nvme_bdev_ctrlrs, nvme_bdev_ctrlr);
extern struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs;
extern pthread_mutex_t g_bdev_nvme_mutex;
extern bool g_bdev_nvme_module_finish;
extern struct spdk_thread *g_bdev_nvme_init_thread;

#define NVME_MAX_CONTROLLERS 1024

typedef void (*spdk_bdev_nvme_start_discovery_fn)(void *ctx, int status);
typedef void (*spdk_bdev_nvme_stop_discovery_fn)(void *ctx);

struct nvme_async_probe_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
	char *base_name;
	const char **names;
	uint32_t max_bdevs;
	uint32_t reported_bdevs;
	struct spdk_poller *poller;
	struct spdk_nvme_transport_id trid;
	struct spdk_bdev_nvme_ctrlr_opts bdev_opts;
	struct spdk_nvme_ctrlr_opts drv_opts;
	spdk_bdev_nvme_create_cb cb_fn;
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
	bool				ana_transition_timedout;
	bool				depopulating;
	struct spdk_poller		*anatt_timer;
	struct nvme_async_probe_ctx	*probe_ctx;
	TAILQ_ENTRY(nvme_ns)		tailq;
	RB_ENTRY(nvme_ns)		node;

	/**
	 * record io path stat before destroyed. Allocation of stat is
	 * decided by option io_path_stat of RPC
	 * bdev_nvme_set_options
	 */
	struct spdk_bdev_io_stat	*stat;
};

struct nvme_bdev_io;
struct nvme_bdev_ctrlr;
struct nvme_bdev;
struct nvme_io_path;
struct nvme_ctrlr_channel_iter;
struct nvme_bdev_channel_iter;

struct spdk_nvme_path_id {
	struct spdk_nvme_transport_id		trid;
	struct spdk_nvme_host_id		hostid;
	TAILQ_ENTRY(spdk_nvme_path_id)		link;
	uint64_t				last_failed_tsc;
};

typedef void (*bdev_nvme_ctrlr_op_cb)(void *cb_arg, int rc);
typedef void (*nvme_ctrlr_disconnected_cb)(struct nvme_ctrlr *nvme_ctrlr);

struct nvme_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_path_id		*active_path_id;
	int					ref;

	uint32_t				resetting : 1;
	uint32_t				reconnect_is_delayed : 1;
	uint32_t				in_failover : 1;
	uint32_t				pending_failover : 1;
	uint32_t				fast_io_fail_timedout : 1;
	uint32_t				destruct : 1;
	uint32_t				ana_log_page_updating : 1;
	uint32_t				io_path_cache_clearing : 1;
	uint32_t				dont_retry : 1;
	uint32_t				disabled : 1;

	struct spdk_bdev_nvme_ctrlr_opts	opts;

	RB_HEAD(nvme_ns_tree, nvme_ns)		namespaces;

	struct spdk_opal_dev			*opal_dev;

	struct spdk_poller			*adminq_timer_poller;
	struct spdk_thread			*thread;
	struct spdk_interrupt			*intr;

	bdev_nvme_ctrlr_op_cb			ctrlr_op_cb_fn;
	void					*ctrlr_op_cb_arg;
	/* Poller used to check for reset/detach completion */
	struct spdk_poller			*reset_detach_poller;
	struct spdk_nvme_detach_ctx		*detach_ctx;

	uint64_t				reset_start_tsc;
	struct spdk_poller			*reconnect_delay_timer;

	nvme_ctrlr_disconnected_cb		disconnected_cb;

	TAILQ_HEAD(, nvme_bdev_io)		pending_resets;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)			tailq;
	struct nvme_bdev_ctrlr			*nbdev_ctrlr;

	TAILQ_HEAD(nvme_paths, spdk_nvme_path_id)	trids;

	uint32_t				max_ana_log_page_size;
	struct spdk_nvme_ana_page		*ana_log_page;
	struct spdk_nvme_ana_group_descriptor	*copied_ana_desc;

	struct nvme_async_probe_ctx		*probe_ctx;
	struct spdk_key				*psk;
	struct spdk_key				*dhchap_key;
	struct spdk_key				*dhchap_ctrlr_key;

	pthread_mutex_t				mutex;
};

struct nvme_bdev_ctrlr {
	char				*name;
	TAILQ_HEAD(, nvme_ctrlr)	ctrlrs;
	TAILQ_HEAD(, nvme_bdev)		bdevs;
	TAILQ_ENTRY(nvme_bdev_ctrlr)	tailq;
};

struct nvme_error_stat {
	uint32_t status_type[8];
	uint32_t status[4][256];
};

struct nvme_bdev {
	struct spdk_bdev			disk;
	uint32_t				nsid;
	struct nvme_bdev_ctrlr			*nbdev_ctrlr;
	pthread_mutex_t				mutex;
	int					ref;
	enum spdk_bdev_nvme_multipath_policy	mp_policy;
	enum spdk_bdev_nvme_multipath_selector	mp_selector;
	uint32_t				rr_min_io;
	TAILQ_HEAD(, nvme_ns)			nvme_ns_list;
	bool					opal;
	TAILQ_ENTRY(nvme_bdev)			tailq;
	struct nvme_error_stat			*err_stat;
};

struct nvme_qpair {
	struct nvme_ctrlr		*ctrlr;
	struct spdk_nvme_qpair		*qpair;
	struct nvme_poll_group		*group;
	struct nvme_ctrlr_channel	*ctrlr_ch;

	/* The following is used to update io_path cache of nvme_bdev_channels. */
	TAILQ_HEAD(, nvme_io_path)	io_path_list;

	TAILQ_ENTRY(nvme_qpair)		tailq;
};

struct nvme_ctrlr_channel {
	struct nvme_qpair		*qpair;

	struct nvme_ctrlr_channel_iter	*reset_iter;
	struct spdk_poller		*connect_poller;
};

struct nvme_io_path {
	struct nvme_ns			*nvme_ns;
	struct nvme_qpair		*qpair;
	STAILQ_ENTRY(nvme_io_path)	stailq;

	/* The following are used to update io_path cache of the nvme_bdev_channel. */
	struct nvme_bdev_channel	*nbdev_ch;
	TAILQ_ENTRY(nvme_io_path)	tailq;

	/* allocation of stat is decided by option io_path_stat of RPC bdev_nvme_set_options */
	struct spdk_bdev_io_stat	*stat;
};

struct nvme_bdev_channel {
	struct nvme_io_path			*current_io_path;
	enum spdk_bdev_nvme_multipath_policy	mp_policy;
	enum spdk_bdev_nvme_multipath_selector	mp_selector;
	uint32_t				rr_min_io;
	uint32_t				rr_counter;
	STAILQ_HEAD(, nvme_io_path)		io_path_list;
	TAILQ_HEAD(retry_io_head, nvme_bdev_io)	retry_io_list;
	struct spdk_poller			*retry_io_poller;
	bool					resetting;
};

struct nvme_poll_group {
	struct spdk_nvme_poll_group		*group;
	struct spdk_io_channel			*accel_channel;
	struct spdk_poller			*poller;
	bool					collect_spin_stat;
	uint64_t				spin_ticks;
	uint64_t				start_ticks;
	uint64_t				end_ticks;
	TAILQ_HEAD(, nvme_qpair)		qpair_list;
	struct spdk_interrupt			*intr;
};

void nvme_io_path_info_json(struct spdk_json_write_ctx *w, struct nvme_io_path *io_path);

struct nvme_ctrlr *nvme_ctrlr_get_by_name(const char *name);

typedef void (*nvme_ctrlr_for_each_channel_msg)(struct nvme_ctrlr_channel_iter *iter,
		struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_ctrlr_channel *ctrlr_ch,
		void *ctx);

typedef void (*nvme_ctrlr_for_each_channel_done)(struct nvme_ctrlr *nvme_ctrlr,
		void *ctx, int status);

void nvme_ctrlr_for_each_channel(struct nvme_ctrlr *nvme_ctrlr,
				 nvme_ctrlr_for_each_channel_msg fn, void *ctx,
				 nvme_ctrlr_for_each_channel_done cpl);

void nvme_ctrlr_for_each_channel_continue(struct nvme_ctrlr_channel_iter *iter,
		int status);


typedef void (*nvme_bdev_for_each_channel_msg)(struct nvme_bdev_channel_iter *iter,
		struct nvme_bdev *nbdev,
		struct nvme_bdev_channel *nbdev_ch,
		void *ctx);

typedef void (*nvme_bdev_for_each_channel_done)(struct nvme_bdev *nbdev,
		void *ctx, int status);

void nvme_bdev_for_each_channel(struct nvme_bdev *nbdev,
				nvme_bdev_for_each_channel_msg fn, void *ctx,
				nvme_bdev_for_each_channel_done cpl);

void nvme_bdev_for_each_channel_continue(struct nvme_bdev_channel_iter *iter,
		int status);

struct nvme_ctrlr *nvme_bdev_ctrlr_get_ctrlr_by_id(struct nvme_bdev_ctrlr *nbdev_ctrlr,
		uint16_t cntlid);

struct nvme_bdev_ctrlr *nvme_bdev_ctrlr_get_by_name(const char *name);

typedef void (*nvme_bdev_ctrlr_for_each_fn)(struct nvme_bdev_ctrlr *nbdev_ctrlr, void *ctx);

void nvme_bdev_ctrlr_for_each(nvme_bdev_ctrlr_for_each_fn fn, void *ctx);

void nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid,
			      struct spdk_json_write_ctx *w);

void nvme_ctrlr_info_json(struct spdk_json_write_ctx *w, struct nvme_ctrlr *nvme_ctrlr);

struct nvme_ns *nvme_ctrlr_get_ns(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid);
struct nvme_ns *nvme_ctrlr_get_first_active_ns(struct nvme_ctrlr *nvme_ctrlr);
struct nvme_ns *nvme_ctrlr_get_next_active_ns(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *ns);

struct spdk_nvme_qpair *bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch);
int bdev_nvme_set_hotplug(bool enabled, uint64_t period_us, spdk_msg_fn cb, void *cb_ctx);

int bdev_nvme_start_discovery(struct spdk_nvme_transport_id *trid, const char *base_name,
			      struct spdk_nvme_ctrlr_opts *drv_opts, struct spdk_bdev_nvme_ctrlr_opts *bdev_opts,
			      uint64_t timeout, bool from_mdns,
			      spdk_bdev_nvme_start_discovery_fn cb_fn, void *cb_ctx);
int bdev_nvme_stop_discovery(const char *name, spdk_bdev_nvme_stop_discovery_fn cb_fn,
			     void *cb_ctx);
void bdev_nvme_get_discovery_info(struct spdk_json_write_ctx *w);

int bdev_nvme_start_mdns_discovery(const char *base_name,
				   const char *svcname,
				   struct spdk_nvme_ctrlr_opts *drv_opts,
				   struct spdk_bdev_nvme_ctrlr_opts *bdev_opts);
int bdev_nvme_stop_mdns_discovery(const char *name);
void bdev_nvme_get_mdns_discovery_info(struct spdk_jsonrpc_request *request);
void bdev_nvme_mdns_discovery_config_json(struct spdk_json_write_ctx *w);

typedef void (*bdev_nvme_set_keys_cb)(void *ctx, int status);

int bdev_nvme_set_keys(const char *name, const char *dhchap_key, const char *dhchap_ctrlr_key,
		       bdev_nvme_set_keys_cb cb_fn, void *cb_ctx);

struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

enum nvme_ctrlr_op {
	NVME_CTRLR_OP_RESET = 1,
	NVME_CTRLR_OP_ENABLE,
	NVME_CTRLR_OP_DISABLE,
};

/**
 * Perform specified operation on an NVMe controller.
 *
 * NOTE: The callback function is always called after this function returns except for
 * out of memory cases.
 *
 * \param nvme_ctrlr The specified NVMe controller to operate
 * \param op Operation code
 * \param cb_fn Function to be called back after operation completes
 * \param cb_arg Argument for callback function
 */
void nvme_ctrlr_op_rpc(struct nvme_ctrlr *nvme_ctrlr, enum nvme_ctrlr_op op,
		       bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg);

/**
 * Perform specified operation on all NVMe controllers in an NVMe bdev controller.
 *
 * NOTE: The callback function is always called after this function returns except for
 * out of memory cases.
 *
 * \param nbdev_ctrlr The specified NVMe bdev controller to operate
 * \param op Operation code
 * \param cb_fn Function to be called back after operation completes
 * \param cb_arg Argument for callback function
 */
void nvme_bdev_ctrlr_op_rpc(struct nvme_bdev_ctrlr *nbdev_ctrlr, enum nvme_ctrlr_op op,
			    bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg);

typedef void (*bdev_nvme_set_preferred_path_cb)(void *cb_arg, int rc);

/**
 * Set the preferred I/O path for an NVMe bdev in multipath mode.
 *
 * NOTE: This function does not support NVMe bdevs in failover mode.
 *
 * \param name NVMe bdev name
 * \param cntlid NVMe-oF controller ID
 * \param cb_fn Function to be called back after completion.
 * \param cb_arg Argument for callback function.
 */
void bdev_nvme_set_preferred_path(const char *name, uint16_t cntlid,
				  bdev_nvme_set_preferred_path_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_NVME_H */
