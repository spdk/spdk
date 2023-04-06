/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/scsi_spec.h"
#include "spdk/notify.h"
#include "spdk/util.h"
#include "spdk/trace.h"
#include "spdk/dma.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "bdev_internal.h"
#include "spdk_internal/trace_defs.h"

#ifdef SPDK_CONFIG_VTUNE
#include "ittnotify.h"
#include "ittnotify_types.h"
int __itt_init_ittlib(const char *, __itt_group_id);
#endif

#define SPDK_BDEV_IO_POOL_SIZE			(64 * 1024 - 1)
#define SPDK_BDEV_IO_CACHE_SIZE			256
#define SPDK_BDEV_AUTO_EXAMINE			true
#define BUF_SMALL_POOL_SIZE			8191
#define BUF_LARGE_POOL_SIZE			1023
#define BUF_SMALL_CACHE_SIZE			128
#define BUF_LARGE_CACHE_SIZE			16
#define NOMEM_THRESHOLD_COUNT			8

#define SPDK_BDEV_QOS_TIMESLICE_IN_USEC		1000
#define SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE	1
#define SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE	512
#define SPDK_BDEV_QOS_MIN_IOS_PER_SEC		1000
#define SPDK_BDEV_QOS_MIN_BYTES_PER_SEC		(1024 * 1024)
#define SPDK_BDEV_QOS_LIMIT_NOT_DEFINED		UINT64_MAX
#define SPDK_BDEV_IO_POLL_INTERVAL_IN_MSEC	1000

/* The maximum number of children requests for a UNMAP or WRITE ZEROES command
 * when splitting into children requests at a time.
 */
#define SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS (8)
#define BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD 1000000

/* The maximum number of children requests for a COPY command
 * when splitting into children requests at a time.
 */
#define SPDK_BDEV_MAX_CHILDREN_COPY_REQS (8)

SPDK_LOG_DEPRECATION_REGISTER(bdev_register_examine_thread,
			      "bdev register and examine on non-app thread", "SPDK 23.05", 0);

SPDK_LOG_DEPRECATION_REGISTER(vtune_support, "Intel(R) VTune integration", "SPDK 23.05", 0);

static const char *qos_rpc_type[] = {"rw_ios_per_sec",
				     "rw_mbytes_per_sec", "r_mbytes_per_sec", "w_mbytes_per_sec"
				    };

TAILQ_HEAD(spdk_bdev_list, spdk_bdev);

RB_HEAD(bdev_name_tree, spdk_bdev_name);

static int
bdev_name_cmp(struct spdk_bdev_name *name1, struct spdk_bdev_name *name2)
{
	return strcmp(name1->name, name2->name);
}

RB_GENERATE_STATIC(bdev_name_tree, spdk_bdev_name, node, bdev_name_cmp);

struct spdk_bdev_mgr {
	struct spdk_mempool *bdev_io_pool;

	void *zero_buffer;

	TAILQ_HEAD(bdev_module_list, spdk_bdev_module) bdev_modules;

	struct spdk_bdev_list bdevs;
	struct bdev_name_tree bdev_names;

	bool init_complete;
	bool module_init_complete;

	struct spdk_spinlock spinlock;

#ifdef SPDK_CONFIG_VTUNE
	__itt_domain	*domain;
#endif
};

static struct spdk_bdev_mgr g_bdev_mgr = {
	.bdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdev_modules),
	.bdevs = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdevs),
	.bdev_names = RB_INITIALIZER(g_bdev_mgr.bdev_names),
	.init_complete = false,
	.module_init_complete = false,
};

static void
__attribute__((constructor))
_bdev_init(void)
{
	spdk_spin_init(&g_bdev_mgr.spinlock);
}

typedef void (*lock_range_cb)(void *ctx, int status);

typedef void (*bdev_copy_bounce_buffer_cpl)(void *ctx, int rc);

struct lba_range {
	uint64_t			offset;
	uint64_t			length;
	void				*locked_ctx;
	struct spdk_bdev_channel	*owner_ch;
	TAILQ_ENTRY(lba_range)		tailq;
};

static struct spdk_bdev_opts	g_bdev_opts = {
	.bdev_io_pool_size = SPDK_BDEV_IO_POOL_SIZE,
	.bdev_io_cache_size = SPDK_BDEV_IO_CACHE_SIZE,
	.bdev_auto_examine = SPDK_BDEV_AUTO_EXAMINE,
	.small_buf_pool_size = BUF_SMALL_POOL_SIZE,
	.large_buf_pool_size = BUF_LARGE_POOL_SIZE,
};

static spdk_bdev_init_cb	g_init_cb_fn = NULL;
static void			*g_init_cb_arg = NULL;

static spdk_bdev_fini_cb	g_fini_cb_fn = NULL;
static void			*g_fini_cb_arg = NULL;
static struct spdk_thread	*g_fini_thread = NULL;

struct spdk_bdev_qos_limit {
	/** IOs or bytes allowed per second (i.e., 1s). */
	uint64_t limit;

	/** Remaining IOs or bytes allowed in current timeslice (e.g., 1ms).
	 *  For remaining bytes, allowed to run negative if an I/O is submitted when
	 *  some bytes are remaining, but the I/O is bigger than that amount. The
	 *  excess will be deducted from the next timeslice.
	 */
	int64_t remaining_this_timeslice;

	/** Minimum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t min_per_timeslice;

	/** Maximum allowed IOs or bytes to be issued in one timeslice (e.g., 1ms). */
	uint32_t max_per_timeslice;

	/** Function to check whether to queue the IO. */
	bool (*queue_io)(const struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);

	/** Function to update for the submitted IO. */
	void (*update_quota)(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io);
};

struct spdk_bdev_qos {
	/** Types of structure of rate limits. */
	struct spdk_bdev_qos_limit rate_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];

	/** The channel that all I/O are funneled through. */
	struct spdk_bdev_channel *ch;

	/** The thread on which the poller is running. */
	struct spdk_thread *thread;

	/** Queue of I/O waiting to be issued. */
	bdev_io_tailq_t queued;

	/** Size of a timeslice in tsc ticks. */
	uint64_t timeslice_size;

	/** Timestamp of start of last timeslice. */
	uint64_t last_timeslice;

	/** Poller that processes queued I/O commands each time slice. */
	struct spdk_poller *poller;
};

struct spdk_bdev_mgmt_channel {
	/*
	 * Each thread keeps a cache of bdev_io - this allows
	 *  bdev threads which are *not* DPDK threads to still
	 *  benefit from a per-thread bdev_io cache.  Without
	 *  this, non-DPDK threads fetching from the mempool
	 *  incur a cmpxchg on get and put.
	 */
	bdev_io_stailq_t per_thread_cache;
	uint32_t	per_thread_cache_count;
	uint32_t	bdev_io_cache_size;

	struct spdk_iobuf_channel iobuf;

	TAILQ_HEAD(, spdk_bdev_shared_resource)	shared_resources;
	TAILQ_HEAD(, spdk_bdev_io_wait_entry)	io_wait_queue;
};

/*
 * Per-module (or per-io_device) data. Multiple bdevs built on the same io_device
 * will queue here their IO that awaits retry. It makes it possible to retry sending
 * IO to one bdev after IO from other bdev completes.
 */
struct spdk_bdev_shared_resource {
	/* The bdev management channel */
	struct spdk_bdev_mgmt_channel *mgmt_ch;

	/*
	 * Count of I/O submitted to bdev module and waiting for completion.
	 * Incremented before submit_request() is called on an spdk_bdev_io.
	 */
	uint64_t		io_outstanding;

	/*
	 * Queue of IO awaiting retry because of a previous NOMEM status returned
	 *  on this channel.
	 */
	bdev_io_tailq_t		nomem_io;

	/*
	 * Threshold which io_outstanding must drop to before retrying nomem_io.
	 */
	uint64_t		nomem_threshold;

	/* I/O channel allocated by a bdev module */
	struct spdk_io_channel	*shared_ch;

	/* Refcount of bdev channels using this resource */
	uint32_t		ref;

	TAILQ_ENTRY(spdk_bdev_shared_resource) link;
};

#define BDEV_CH_RESET_IN_PROGRESS	(1 << 0)
#define BDEV_CH_QOS_ENABLED		(1 << 1)

struct spdk_bdev_channel {
	struct spdk_bdev	*bdev;

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;

	/* Per io_device per thread data */
	struct spdk_bdev_shared_resource *shared_resource;

	struct spdk_bdev_io_stat *stat;

	/*
	 * Count of I/O submitted to the underlying dev module through this channel
	 * and waiting for completion.
	 */
	uint64_t		io_outstanding;

	/*
	 * List of all submitted I/Os including I/O that are generated via splitting.
	 */
	bdev_io_tailq_t		io_submitted;

	/*
	 * List of spdk_bdev_io that are currently queued because they write to a locked
	 * LBA range.
	 */
	bdev_io_tailq_t		io_locked;

	uint32_t		flags;

	struct spdk_histogram_data *histogram;

#ifdef SPDK_CONFIG_VTUNE
	uint64_t		start_tsc;
	uint64_t		interval_tsc;
	__itt_string_handle	*handle;
	struct spdk_bdev_io_stat *prev_stat;
#endif

	bdev_io_tailq_t		queued_resets;

	lba_range_tailq_t	locked_ranges;
};

struct media_event_entry {
	struct spdk_bdev_media_event	event;
	TAILQ_ENTRY(media_event_entry)	tailq;
};

#define MEDIA_EVENT_POOL_SIZE 64

struct spdk_bdev_desc {
	struct spdk_bdev		*bdev;
	struct spdk_thread		*thread;
	struct {
		spdk_bdev_event_cb_t event_fn;
		void *ctx;
	}				callback;
	bool				closed;
	bool				write;
	bool				memory_domains_supported;
	struct spdk_spinlock		spinlock;
	uint32_t			refs;
	TAILQ_HEAD(, media_event_entry)	pending_media_events;
	TAILQ_HEAD(, media_event_entry)	free_media_events;
	struct media_event_entry	*media_events_buffer;
	TAILQ_ENTRY(spdk_bdev_desc)	link;

	uint64_t		timeout_in_sec;
	spdk_bdev_io_timeout_cb	cb_fn;
	void			*cb_arg;
	struct spdk_poller	*io_timeout_poller;
};

struct spdk_bdev_iostat_ctx {
	struct spdk_bdev_io_stat *stat;
	spdk_bdev_get_device_stat_cb cb;
	void *cb_arg;
};

struct set_qos_limit_ctx {
	void (*cb_fn)(void *cb_arg, int status);
	void *cb_arg;
	struct spdk_bdev *bdev;
};

struct spdk_bdev_channel_iter {
	spdk_bdev_for_each_channel_msg fn;
	spdk_bdev_for_each_channel_done cpl;
	struct spdk_io_channel_iter *i;
	void *ctx;
};

struct spdk_bdev_io_error_stat {
	uint32_t error_status[-SPDK_MIN_BDEV_IO_STATUS];
};

#define __bdev_to_io_dev(bdev)		(((char *)bdev) + 1)
#define __bdev_from_io_dev(io_dev)	((struct spdk_bdev *)(((char *)io_dev) - 1))
#define __io_ch_to_bdev_ch(io_ch)	((struct spdk_bdev_channel *)spdk_io_channel_get_ctx(io_ch))
#define __io_ch_to_bdev_mgmt_ch(io_ch)	((struct spdk_bdev_mgmt_channel *)spdk_io_channel_get_ctx(io_ch))

static inline void bdev_io_complete(void *ctx);

static void bdev_write_zero_buffer_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void bdev_write_zero_buffer_next(void *_bdev_io);

static void bdev_enable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *ch, void *_ctx);
static void bdev_enable_qos_done(struct spdk_bdev *bdev, void *_ctx, int status);

static int bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     struct iovec *iov, int iovcnt, void *md_buf, uint64_t offset_blocks,
				     uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
				     struct spdk_bdev_ext_io_opts *opts, bool copy_opts);
static int bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				      struct iovec *iov, int iovcnt, void *md_buf,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg,
				      struct spdk_bdev_ext_io_opts *opts, bool copy_opts);

static int bdev_lock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
			       uint64_t offset, uint64_t length,
			       lock_range_cb cb_fn, void *cb_arg);

static int bdev_unlock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
				 uint64_t offset, uint64_t length,
				 lock_range_cb cb_fn, void *cb_arg);

static inline void bdev_io_complete(void *ctx);

static bool bdev_abort_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_io *bio_to_abort);
static bool bdev_abort_buf_io(struct spdk_bdev_mgmt_channel *ch, struct spdk_bdev_io *bio_to_abort);

void
spdk_bdev_get_opts(struct spdk_bdev_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_bdev_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_bdev_opts.field; \
	} \

	SET_FIELD(bdev_io_pool_size);
	SET_FIELD(bdev_io_cache_size);
	SET_FIELD(bdev_auto_examine);
	SET_FIELD(small_buf_pool_size);
	SET_FIELD(large_buf_pool_size);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_opts) == 32, "Incorrect size");

#undef SET_FIELD
}

SPDK_LOG_DEPRECATION_REGISTER(bdev_opts_small_buf_pool_size, "spdk_bdev_opts.small_buf_pool_size",
			      "v23.05", 0);
SPDK_LOG_DEPRECATION_REGISTER(bdev_opts_large_buf_pool_size, "spdk_bdev_opts.large_buf_pool_size",
			      "v23.05", 0);
int
spdk_bdev_set_opts(struct spdk_bdev_opts *opts)
{
	struct spdk_iobuf_opts iobuf_opts;
	uint32_t min_pool_size;
	int rc;

	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
	}

	/*
	 * Add 1 to the thread count to account for the extra mgmt_ch that gets created during subsystem
	 *  initialization.  A second mgmt_ch will be created on the same thread when the application starts
	 *  but before the deferred put_io_channel event is executed for the first mgmt_ch.
	 */
	min_pool_size = opts->bdev_io_cache_size * (spdk_thread_get_count() + 1);
	if (opts->bdev_io_pool_size < min_pool_size) {
		SPDK_ERRLOG("bdev_io_pool_size %" PRIu32 " is not compatible with bdev_io_cache_size %" PRIu32
			    " and %" PRIu32 " threads\n", opts->bdev_io_pool_size, opts->bdev_io_cache_size,
			    spdk_thread_get_count());
		SPDK_ERRLOG("bdev_io_pool_size must be at least %" PRIu32 "\n", min_pool_size);
		return -1;
	}

	if (opts->small_buf_pool_size != BUF_SMALL_POOL_SIZE) {
		SPDK_LOG_DEPRECATED(bdev_opts_small_buf_pool_size);
	}
	if (opts->large_buf_pool_size != BUF_LARGE_POOL_SIZE) {
		SPDK_LOG_DEPRECATED(bdev_opts_large_buf_pool_size);
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_bdev_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_bdev_opts.field = opts->field; \
        } \

	SET_FIELD(bdev_io_pool_size);
	SET_FIELD(bdev_io_cache_size);
	SET_FIELD(bdev_auto_examine);
	SET_FIELD(small_buf_pool_size);
	SET_FIELD(large_buf_pool_size);

	spdk_iobuf_get_opts(&iobuf_opts);
	iobuf_opts.small_pool_count = opts->small_buf_pool_size;
	iobuf_opts.large_pool_count = opts->large_buf_pool_size;

	rc = spdk_iobuf_set_opts(&iobuf_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to set iobuf opts\n");
		return -1;
	}

	g_bdev_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

static struct spdk_bdev *
bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev_name find;
	struct spdk_bdev_name *res;

	find.name = (char *)bdev_name;
	res = RB_FIND(bdev_name_tree, &g_bdev_mgr.bdev_names, &find);
	if (res != NULL) {
		return res->bdev;
	}

	return NULL;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev;

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev = bdev_get_by_name(bdev_name);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return bdev;
}

struct bdev_io_status_string {
	enum spdk_bdev_io_status status;
	const char *str;
};

static const struct bdev_io_status_string bdev_io_status_strings[] = {
	{ SPDK_BDEV_IO_STATUS_AIO_ERROR, "aio_error" },
	{ SPDK_BDEV_IO_STATUS_ABORTED, "aborted" },
	{ SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED, "first_fused_failed" },
	{ SPDK_BDEV_IO_STATUS_MISCOMPARE, "miscompare" },
	{ SPDK_BDEV_IO_STATUS_NOMEM, "nomem" },
	{ SPDK_BDEV_IO_STATUS_SCSI_ERROR, "scsi_error" },
	{ SPDK_BDEV_IO_STATUS_NVME_ERROR, "nvme_error" },
	{ SPDK_BDEV_IO_STATUS_FAILED, "failed" },
	{ SPDK_BDEV_IO_STATUS_PENDING, "pending" },
	{ SPDK_BDEV_IO_STATUS_SUCCESS, "success" },
};

static const char *
bdev_io_status_get_string(enum spdk_bdev_io_status status)
{
	uint32_t i;

	for (i = 0; i < SPDK_COUNTOF(bdev_io_status_strings); i++) {
		if (bdev_io_status_strings[i].status == status) {
			return bdev_io_status_strings[i].str;
		}
	}

	return "reserved";
}

struct spdk_bdev_wait_for_examine_ctx {
	struct spdk_poller              *poller;
	spdk_bdev_wait_for_examine_cb	cb_fn;
	void				*cb_arg;
};

static bool bdev_module_all_actions_completed(void);

static int
bdev_wait_for_examine_cb(void *arg)
{
	struct spdk_bdev_wait_for_examine_ctx *ctx = arg;

	if (!bdev_module_all_actions_completed()) {
		return SPDK_POLLER_IDLE;
	}

	spdk_poller_unregister(&ctx->poller);
	ctx->cb_fn(ctx->cb_arg);
	free(ctx);

	return SPDK_POLLER_BUSY;
}

int
spdk_bdev_wait_for_examine(spdk_bdev_wait_for_examine_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_wait_for_examine_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->poller = SPDK_POLLER_REGISTER(bdev_wait_for_examine_cb, ctx, 0);

	return 0;
}

struct spdk_bdev_examine_item {
	char *name;
	TAILQ_ENTRY(spdk_bdev_examine_item) link;
};

TAILQ_HEAD(spdk_bdev_examine_allowlist, spdk_bdev_examine_item);

struct spdk_bdev_examine_allowlist g_bdev_examine_allowlist = TAILQ_HEAD_INITIALIZER(
			g_bdev_examine_allowlist);

static inline bool
bdev_examine_allowlist_check(const char *name)
{
	struct spdk_bdev_examine_item *item;
	TAILQ_FOREACH(item, &g_bdev_examine_allowlist, link) {
		if (strcmp(name, item->name) == 0) {
			return true;
		}
	}
	return false;
}

static inline void
bdev_examine_allowlist_free(void)
{
	struct spdk_bdev_examine_item *item;
	while (!TAILQ_EMPTY(&g_bdev_examine_allowlist)) {
		item = TAILQ_FIRST(&g_bdev_examine_allowlist);
		TAILQ_REMOVE(&g_bdev_examine_allowlist, item, link);
		free(item->name);
		free(item);
	}
}

static inline bool
bdev_in_examine_allowlist(struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *tmp;
	if (bdev_examine_allowlist_check(bdev->name)) {
		return true;
	}
	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		if (bdev_examine_allowlist_check(tmp->alias.name)) {
			return true;
		}
	}
	return false;
}

static inline bool
bdev_ok_to_examine(struct spdk_bdev *bdev)
{
	if (g_bdev_opts.bdev_auto_examine) {
		return true;
	} else {
		return bdev_in_examine_allowlist(bdev);
	}
}

static void
bdev_examine(struct spdk_bdev *bdev)
{
	struct spdk_bdev_module *module;
	uint32_t action;

	if (!bdev_ok_to_examine(bdev)) {
		return;
	}

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (module->examine_config) {
			spdk_spin_lock(&module->internal.spinlock);
			action = module->internal.action_in_progress;
			module->internal.action_in_progress++;
			spdk_spin_unlock(&module->internal.spinlock);
			module->examine_config(bdev);
			if (action != module->internal.action_in_progress) {
				SPDK_ERRLOG("examine_config for module %s did not call "
					    "spdk_bdev_module_examine_done()\n", module->name);
			}
		}
	}

	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		module = bdev->internal.claim.v1.module;
		if (module->examine_disk) {
			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress++;
			spdk_spin_unlock(&module->internal.spinlock);
			module->examine_disk(bdev);
		}
		return;
	}

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (module->examine_disk) {
			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress++;
			spdk_spin_unlock(&module->internal.spinlock);
			module->examine_disk(bdev);
		}
	}
}

int
spdk_bdev_examine(const char *name)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_examine_item *item;

	if (spdk_unlikely(spdk_thread_get_app_thread() != spdk_get_thread())) {
		SPDK_LOG_DEPRECATED(bdev_register_examine_thread);
	}

	if (g_bdev_opts.bdev_auto_examine) {
		SPDK_ERRLOG("Manual examine is not allowed if auto examine is enabled");
		return -EINVAL;
	}

	if (bdev_examine_allowlist_check(name)) {
		SPDK_ERRLOG("Duplicate bdev name for manual examine: %s\n", name);
		return -EEXIST;
	}

	item = calloc(1, sizeof(*item));
	if (!item) {
		return -ENOMEM;
	}
	item->name = strdup(name);
	if (!item->name) {
		free(item);
		return -ENOMEM;
	}
	TAILQ_INSERT_TAIL(&g_bdev_examine_allowlist, item, link);

	bdev = spdk_bdev_get_by_name(name);
	if (bdev) {
		bdev_examine(bdev);
	}
	return 0;
}

static inline void
bdev_examine_allowlist_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_examine_item *item;
	TAILQ_FOREACH(item, &g_bdev_examine_allowlist, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_examine");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", item->name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

struct spdk_bdev *
spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	if (bdev) {
		SPDK_DEBUGLOG(bdev, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_NEXT(prev, internal.link);
	if (bdev) {
		SPDK_DEBUGLOG(bdev, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

static struct spdk_bdev *
_bdev_next_leaf(struct spdk_bdev *bdev)
{
	while (bdev != NULL) {
		if (bdev->internal.claim_type == SPDK_BDEV_CLAIM_NONE) {
			return bdev;
		} else {
			bdev = TAILQ_NEXT(bdev, internal.link);
		}
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_first_leaf(void)
{
	struct spdk_bdev *bdev;

	bdev = _bdev_next_leaf(TAILQ_FIRST(&g_bdev_mgr.bdevs));

	if (bdev) {
		SPDK_DEBUGLOG(bdev, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next_leaf(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = _bdev_next_leaf(TAILQ_NEXT(prev, internal.link));

	if (bdev) {
		SPDK_DEBUGLOG(bdev, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

static inline bool
bdev_io_use_memory_domain(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.ext_opts && bdev_io->internal.ext_opts->memory_domain;
}

void
spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len)
{
	struct iovec *iovs;

	if (bdev_io->u.bdev.iovs == NULL) {
		bdev_io->u.bdev.iovs = &bdev_io->iov;
		bdev_io->u.bdev.iovcnt = 1;
	}

	iovs = bdev_io->u.bdev.iovs;

	assert(iovs != NULL);
	assert(bdev_io->u.bdev.iovcnt >= 1);

	iovs[0].iov_base = buf;
	iovs[0].iov_len = len;
}

void
spdk_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len)
{
	assert((len / spdk_bdev_get_md_size(bdev_io->bdev)) >= bdev_io->u.bdev.num_blocks);
	bdev_io->u.bdev.md_buf = md_buf;
}

static bool
_is_buf_allocated(const struct iovec *iovs)
{
	if (iovs == NULL) {
		return false;
	}

	return iovs[0].iov_base != NULL;
}

static bool
_are_iovs_aligned(struct iovec *iovs, int iovcnt, uint32_t alignment)
{
	int i;
	uintptr_t iov_base;

	if (spdk_likely(alignment == 1)) {
		return true;
	}

	for (i = 0; i < iovcnt; i++) {
		iov_base = (uintptr_t)iovs[i].iov_base;
		if ((iov_base & (alignment - 1)) != 0) {
			return false;
		}
	}

	return true;
}

static void
bdev_io_get_buf_complete(struct spdk_bdev_io *bdev_io, bool status)
{
	struct spdk_io_channel *ch = spdk_bdev_io_get_io_channel(bdev_io);
	void *buf;

	if (spdk_unlikely(bdev_io->internal.get_aux_buf_cb != NULL)) {
		buf = bdev_io->internal.buf;
		bdev_io->internal.buf = NULL;
		bdev_io->internal.get_aux_buf_cb(ch, bdev_io, buf);
		bdev_io->internal.get_aux_buf_cb = NULL;
	} else {
		assert(bdev_io->internal.get_buf_cb != NULL);
		bdev_io->internal.get_buf_cb(ch, bdev_io, status);
		bdev_io->internal.get_buf_cb = NULL;
	}
}

static void
_bdev_io_pull_buffer_cpl(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (rc) {
		SPDK_ERRLOG("Set bounce buffer failed with rc %d\n", rc);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	bdev_io_get_buf_complete(bdev_io, !rc);
}

static void
_bdev_io_pull_bounce_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len)
{
	int rc = 0;

	/* save original md_buf */
	bdev_io->internal.orig_md_iov.iov_base = bdev_io->u.bdev.md_buf;
	bdev_io->internal.orig_md_iov.iov_len = len;
	bdev_io->internal.bounce_md_iov.iov_base = md_buf;
	bdev_io->internal.bounce_md_iov.iov_len = len;
	/* set bounce md_buf */
	bdev_io->u.bdev.md_buf = md_buf;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (bdev_io_use_memory_domain(bdev_io)) {
			rc = spdk_memory_domain_pull_data(bdev_io->internal.ext_opts->memory_domain,
							  bdev_io->internal.ext_opts->memory_domain_ctx,
							  &bdev_io->internal.orig_md_iov, 1,
							  &bdev_io->internal.bounce_md_iov, 1,
							  bdev_io->internal.data_transfer_cpl,
							  bdev_io);
			if (rc == 0) {
				/* Continue to submit IO in completion callback */
				return;
			}
			SPDK_ERRLOG("Failed to pull data from memory domain %s, rc %d\n",
				    spdk_memory_domain_get_dma_device_id(bdev_io->internal.ext_opts->memory_domain), rc);
		} else {
			memcpy(md_buf, bdev_io->internal.orig_md_iov.iov_base, bdev_io->internal.orig_md_iov.iov_len);
		}
	}

	assert(bdev_io->internal.data_transfer_cpl);
	bdev_io->internal.data_transfer_cpl(bdev_io, rc);
}

static void
_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint64_t md_len;
	void *buf;

	if (spdk_bdev_is_md_separate(bdev)) {
		buf = (char *)bdev_io->u.bdev.iovs[0].iov_base + bdev_io->u.bdev.iovs[0].iov_len;
		md_len = bdev_io->u.bdev.num_blocks * bdev->md_len;

		assert(((uintptr_t)buf & (spdk_bdev_get_buf_align(bdev) - 1)) == 0);

		if (bdev_io->u.bdev.md_buf != NULL) {
			_bdev_io_pull_bounce_md_buf(bdev_io, buf, md_len);
			return;
		} else {
			spdk_bdev_io_set_md_buf(bdev_io, buf, md_len);
		}
	}

	bdev_io_get_buf_complete(bdev_io, true);
}

static void
_bdev_io_pull_bounce_data_buf_done(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (rc) {
		SPDK_ERRLOG("Failed to get data buffer\n");
		assert(bdev_io->internal.data_transfer_cpl);
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
		return;
	}

	_bdev_io_set_md_buf(bdev_io);
}

static void
_bdev_io_pull_bounce_data_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len,
			      bdev_copy_bounce_buffer_cpl cpl_cb)
{
	int rc = 0;

	bdev_io->internal.data_transfer_cpl = cpl_cb;
	/* save original iovec */
	bdev_io->internal.orig_iovs = bdev_io->u.bdev.iovs;
	bdev_io->internal.orig_iovcnt = bdev_io->u.bdev.iovcnt;
	/* set bounce iov */
	bdev_io->u.bdev.iovs = &bdev_io->internal.bounce_iov;
	bdev_io->u.bdev.iovcnt = 1;
	/* set bounce buffer for this operation */
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = len;
	/* if this is write path, copy data from original buffer to bounce buffer */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (bdev_io_use_memory_domain(bdev_io)) {
			rc = spdk_memory_domain_pull_data(bdev_io->internal.ext_opts->memory_domain,
							  bdev_io->internal.ext_opts->memory_domain_ctx,
							  bdev_io->internal.orig_iovs,
							  (uint32_t) bdev_io->internal.orig_iovcnt,
							  bdev_io->u.bdev.iovs, 1,
							  _bdev_io_pull_bounce_data_buf_done,
							  bdev_io);
			if (rc == 0) {
				/* Continue to submit IO in completion callback */
				return;
			}
			SPDK_ERRLOG("Failed to pull data from memory domain %s\n",
				    spdk_memory_domain_get_dma_device_id(bdev_io->internal.ext_opts->memory_domain));
		} else {
			spdk_copy_iovs_to_buf(buf, len, bdev_io->internal.orig_iovs, bdev_io->internal.orig_iovcnt);
		}
	}

	_bdev_io_pull_bounce_data_buf_done(bdev_io, rc);
}

static void
_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	bool buf_allocated;
	uint64_t alignment;
	void *aligned_buf;

	bdev_io->internal.buf = buf;

	if (spdk_unlikely(bdev_io->internal.get_aux_buf_cb != NULL)) {
		bdev_io_get_buf_complete(bdev_io, true);
		return;
	}

	alignment = spdk_bdev_get_buf_align(bdev);
	buf_allocated = _is_buf_allocated(bdev_io->u.bdev.iovs);
	aligned_buf = (void *)(((uintptr_t)buf + (alignment - 1)) & ~(alignment - 1));

	if (buf_allocated) {
		_bdev_io_pull_bounce_data_buf(bdev_io, aligned_buf, len, _bdev_io_pull_buffer_cpl);
		/* Continue in completion callback */
		return;
	} else {
		spdk_bdev_io_set_buf(bdev_io, aligned_buf, len);
	}

	_bdev_io_set_md_buf(bdev_io);
}

static inline uint64_t
bdev_io_get_max_buf_len(struct spdk_bdev_io *bdev_io, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint64_t md_len, alignment;

	md_len = spdk_bdev_is_md_separate(bdev) ? bdev_io->u.bdev.num_blocks * bdev->md_len : 0;

	/* 1 byte alignment needs 0 byte of extra space, 64 bytes alignment needs 63 bytes of extra space, etc. */
	alignment = spdk_bdev_get_buf_align(bdev) - 1;

	return len + alignment + md_len;
}

static void
_bdev_io_put_buf(struct spdk_bdev_io *bdev_io, void *buf, uint64_t buf_len)
{
	struct spdk_bdev_mgmt_channel *ch;

	ch = bdev_io->internal.ch->shared_resource->mgmt_ch;
	spdk_iobuf_put(&ch->iobuf, buf, bdev_io_get_max_buf_len(bdev_io, buf_len));
}

static void
bdev_io_put_buf(struct spdk_bdev_io *bdev_io)
{
	assert(bdev_io->internal.buf != NULL);
	_bdev_io_put_buf(bdev_io, bdev_io->internal.buf, bdev_io->internal.buf_len);
	bdev_io->internal.buf = NULL;
}

void
spdk_bdev_io_put_aux_buf(struct spdk_bdev_io *bdev_io, void *buf)
{
	uint64_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	assert(buf != NULL);
	_bdev_io_put_buf(bdev_io, buf, len);
}

static void
bdev_ch_retry_io(struct spdk_bdev_channel *bdev_ch)
{
	struct spdk_bdev *bdev = bdev_ch->bdev;
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;
	struct spdk_bdev_io *bdev_io;

	if (shared_resource->io_outstanding > shared_resource->nomem_threshold) {
		/*
		 * Allow some more I/O to complete before retrying the nomem_io queue.
		 *  Some drivers (such as nvme) cannot immediately take a new I/O in
		 *  the context of a completion, because the resources for the I/O are
		 *  not released until control returns to the bdev poller.  Also, we
		 *  may require several small I/O to complete before a larger I/O
		 *  (that requires splitting) can be submitted.
		 */
		return;
	}

	while (!TAILQ_EMPTY(&shared_resource->nomem_io)) {
		bdev_io = TAILQ_FIRST(&shared_resource->nomem_io);
		TAILQ_REMOVE(&shared_resource->nomem_io, bdev_io, internal.link);
		bdev_io->internal.ch->io_outstanding++;
		shared_resource->io_outstanding++;
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING;
		bdev_io->internal.error.nvme.cdw0 = 0;
		bdev_io->num_retries++;
		bdev->fn_table->submit_request(spdk_bdev_io_get_io_channel(bdev_io), bdev_io);
		if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			break;
		}
	}
}

static inline void
_bdev_io_decrement_outstanding(struct spdk_bdev_channel *bdev_ch,
			       struct spdk_bdev_shared_resource *shared_resource)
{
	assert(bdev_ch->io_outstanding > 0);
	assert(shared_resource->io_outstanding > 0);
	bdev_ch->io_outstanding--;
	shared_resource->io_outstanding--;
}

static inline bool
_bdev_io_handle_no_mem(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;

	if (spdk_unlikely(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM)) {
		TAILQ_INSERT_HEAD(&shared_resource->nomem_io, bdev_io, internal.link);
		/*
		 * Wait for some of the outstanding I/O to complete before we
		 *  retry any of the nomem_io.  Normally we will wait for
		 *  NOMEM_THRESHOLD_COUNT I/O to complete but for low queue
		 *  depth channels we will instead wait for half to complete.
		 */
		shared_resource->nomem_threshold = spdk_max((int64_t)shared_resource->io_outstanding / 2,
						   (int64_t)shared_resource->io_outstanding - NOMEM_THRESHOLD_COUNT);
		return true;
	}

	if (spdk_unlikely(!TAILQ_EMPTY(&shared_resource->nomem_io))) {
		bdev_ch_retry_io(bdev_ch);
	}

	return false;
}

static void
_bdev_io_complete_push_bounce_done(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;

	if (rc) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	/* We want to free the bounce buffer here since we know we're done with it (as opposed
	 * to waiting for the conditional free of internal.buf in spdk_bdev_free_io()).
	 */
	bdev_io_put_buf(bdev_io);

	/* Continue with IO completion flow */
	_bdev_io_decrement_outstanding(bdev_ch, shared_resource);
	if (spdk_unlikely(_bdev_io_handle_no_mem(bdev_io))) {
		return;
	}

	bdev_io_complete(bdev_io);
}

static inline void
_bdev_io_push_bounce_md_buffer(struct spdk_bdev_io *bdev_io)
{
	int rc = 0;

	/* do the same for metadata buffer */
	if (spdk_unlikely(bdev_io->internal.orig_md_iov.iov_base != NULL)) {
		assert(spdk_bdev_is_md_separate(bdev_io->bdev));

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
		    bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			if (bdev_io_use_memory_domain(bdev_io)) {
				/* If memory domain is used then we need to call async push function */
				rc = spdk_memory_domain_push_data(bdev_io->internal.ext_opts->memory_domain,
								  bdev_io->internal.ext_opts->memory_domain_ctx,
								  &bdev_io->internal.orig_md_iov,
								  (uint32_t)bdev_io->internal.orig_iovcnt,
								  &bdev_io->internal.bounce_md_iov, 1,
								  bdev_io->internal.data_transfer_cpl,
								  bdev_io);
				if (rc == 0) {
					/* Continue IO completion in async callback */
					return;
				}
				SPDK_ERRLOG("Failed to push md to memory domain %s\n",
					    spdk_memory_domain_get_dma_device_id(bdev_io->internal.ext_opts->memory_domain));
			} else {
				memcpy(bdev_io->internal.orig_md_iov.iov_base, bdev_io->u.bdev.md_buf,
				       bdev_io->internal.orig_md_iov.iov_len);
			}
		}
	}

	assert(bdev_io->internal.data_transfer_cpl);
	bdev_io->internal.data_transfer_cpl(bdev_io, rc);
}

static void
_bdev_io_push_bounce_data_buffer_done(void *ctx, int rc)
{
	struct spdk_bdev_io *bdev_io = ctx;

	assert(bdev_io->internal.data_transfer_cpl);

	if (rc) {
		bdev_io->internal.data_transfer_cpl(bdev_io, rc);
		return;
	}

	/* set original buffer for this io */
	bdev_io->u.bdev.iovcnt = bdev_io->internal.orig_iovcnt;
	bdev_io->u.bdev.iovs = bdev_io->internal.orig_iovs;
	/* disable bouncing buffer for this io */
	bdev_io->internal.orig_iovcnt = 0;
	bdev_io->internal.orig_iovs = NULL;

	_bdev_io_push_bounce_md_buffer(bdev_io);
}

static inline void
_bdev_io_push_bounce_data_buffer(struct spdk_bdev_io *bdev_io, bdev_copy_bounce_buffer_cpl cpl_cb)
{
	int rc = 0;

	bdev_io->internal.data_transfer_cpl = cpl_cb;

	/* if this is read path, copy data from bounce buffer to original buffer */
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
	    bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		if (bdev_io_use_memory_domain(bdev_io)) {
			/* If memory domain is used then we need to call async push function */
			rc = spdk_memory_domain_push_data(bdev_io->internal.ext_opts->memory_domain,
							  bdev_io->internal.ext_opts->memory_domain_ctx,
							  bdev_io->internal.orig_iovs,
							  (uint32_t)bdev_io->internal.orig_iovcnt,
							  &bdev_io->internal.bounce_iov, 1,
							  _bdev_io_push_bounce_data_buffer_done,
							  bdev_io);
			if (rc == 0) {
				/* Continue IO completion in async callback */
				return;
			}
			SPDK_ERRLOG("Failed to push data to memory domain %s\n",
				    spdk_memory_domain_get_dma_device_id(bdev_io->internal.ext_opts->memory_domain));
		} else {
			spdk_copy_buf_to_iovs(bdev_io->internal.orig_iovs,
					      bdev_io->internal.orig_iovcnt,
					      bdev_io->internal.bounce_iov.iov_base,
					      bdev_io->internal.bounce_iov.iov_len);
		}
	}

	_bdev_io_push_bounce_data_buffer_done(bdev_io, rc);
}

static void
bdev_io_get_iobuf_cb(struct spdk_iobuf_entry *iobuf, void *buf)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = SPDK_CONTAINEROF(iobuf, struct spdk_bdev_io, internal.iobuf);
	_bdev_io_set_buf(bdev_io, buf, bdev_io->internal.buf_len);
}

static void
bdev_io_get_buf(struct spdk_bdev_io *bdev_io, uint64_t len)
{
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	uint64_t max_len;
	void *buf;

	assert(spdk_bdev_io_get_thread(bdev_io) == spdk_get_thread());
	mgmt_ch = bdev_io->internal.ch->shared_resource->mgmt_ch;
	max_len = bdev_io_get_max_buf_len(bdev_io, len);

	if (spdk_unlikely(max_len > mgmt_ch->iobuf.large.bufsize)) {
		SPDK_ERRLOG("Length %" PRIu64 " is larger than allowed\n", max_len);
		bdev_io_get_buf_complete(bdev_io, false);
		return;
	}

	bdev_io->internal.buf_len = len;
	buf = spdk_iobuf_get(&mgmt_ch->iobuf, max_len, &bdev_io->internal.iobuf,
			     bdev_io_get_iobuf_cb);
	if (buf != NULL) {
		_bdev_io_set_buf(bdev_io, buf, len);
	}
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint64_t alignment;

	assert(cb != NULL);
	bdev_io->internal.get_buf_cb = cb;

	alignment = spdk_bdev_get_buf_align(bdev);

	if (_is_buf_allocated(bdev_io->u.bdev.iovs) &&
	    _are_iovs_aligned(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, alignment)) {
		/* Buffer already present and aligned */
		cb(spdk_bdev_io_get_io_channel(bdev_io), bdev_io, true);
		return;
	}

	bdev_io_get_buf(bdev_io, len);
}

static void
_bdev_memory_domain_get_io_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			      bool success)
{
	if (!success) {
		SPDK_ERRLOG("Failed to get data buffer, completing IO\n");
		bdev_io_complete(bdev_io);
	} else {
		bdev_io_submit(bdev_io);
	}
}

static void
_bdev_memory_domain_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
			       uint64_t len)
{
	assert(cb != NULL);
	bdev_io->internal.get_buf_cb = cb;

	bdev_io_get_buf(bdev_io, len);
}

void
spdk_bdev_io_get_aux_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_aux_buf_cb cb)
{
	uint64_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	assert(cb != NULL);
	assert(bdev_io->internal.get_aux_buf_cb == NULL);
	bdev_io->internal.get_aux_buf_cb = cb;
	bdev_io_get_buf(bdev_io, len);
}

static int
bdev_module_get_max_ctx_size(void)
{
	struct spdk_bdev_module *bdev_module;
	int max_bdev_module_size = 0;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
}

static void
bdev_qos_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	int i;
	struct spdk_bdev_qos *qos = bdev->internal.qos;
	uint64_t limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];

	if (!qos) {
		return;
	}

	spdk_bdev_get_qos_rate_limits(bdev, limits);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_set_qos_limit");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] > 0) {
			spdk_json_write_named_uint64(w, qos_rpc_type[i], limits[i]);
		}
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

void
spdk_bdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_module *bdev_module;
	struct spdk_bdev *bdev;

	assert(w != NULL);

	spdk_json_write_array_begin(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_set_options");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "bdev_io_pool_size", g_bdev_opts.bdev_io_pool_size);
	spdk_json_write_named_uint32(w, "bdev_io_cache_size", g_bdev_opts.bdev_io_cache_size);
	spdk_json_write_named_bool(w, "bdev_auto_examine", g_bdev_opts.bdev_auto_examine);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);

	bdev_examine_allowlist_config_json(w);

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (bdev_module->config_json) {
			bdev_module->config_json(w);
		}
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock);

	TAILQ_FOREACH(bdev, &g_bdev_mgr.bdevs, internal.link) {
		if (bdev->fn_table->write_config_json) {
			bdev->fn_table->write_config_json(bdev, w);
		}

		bdev_qos_config_json(bdev, w);
	}

	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	/* This has to be last RPC in array to make sure all bdevs finished examine */
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_wait_for_examine");
	spdk_json_write_object_end(w);

	spdk_json_write_array_end(w);
}

static void
bdev_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;
	struct spdk_bdev_io *bdev_io;

	spdk_iobuf_channel_fini(&ch->iobuf);

	while (!STAILQ_EMPTY(&ch->per_thread_cache)) {
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		ch->per_thread_cache_count--;
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
	}

	assert(ch->per_thread_cache_count == 0);
}

static int
bdev_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;
	struct spdk_bdev_io *bdev_io;
	uint32_t i;
	int rc;

	rc = spdk_iobuf_channel_init(&ch->iobuf, "bdev", BUF_SMALL_CACHE_SIZE, BUF_LARGE_CACHE_SIZE);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create iobuf channel: %s\n", spdk_strerror(-rc));
		return -1;
	}

	STAILQ_INIT(&ch->per_thread_cache);
	ch->bdev_io_cache_size = g_bdev_opts.bdev_io_cache_size;

	/* Pre-populate bdev_io cache to ensure this thread cannot be starved. */
	ch->per_thread_cache_count = 0;
	for (i = 0; i < ch->bdev_io_cache_size; i++) {
		bdev_io = spdk_mempool_get(g_bdev_mgr.bdev_io_pool);
		if (bdev_io == NULL) {
			SPDK_ERRLOG("You need to increase bdev_io_pool_size using bdev_set_options RPC.\n");
			assert(false);
			bdev_mgmt_channel_destroy(io_device, ctx_buf);
			return -1;
		}
		ch->per_thread_cache_count++;
		STAILQ_INSERT_HEAD(&ch->per_thread_cache, bdev_io, internal.buf_link);
	}

	TAILQ_INIT(&ch->shared_resources);
	TAILQ_INIT(&ch->io_wait_queue);

	return 0;
}

static void
bdev_init_complete(int rc)
{
	spdk_bdev_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;
	struct spdk_bdev_module *m;

	g_bdev_mgr.init_complete = true;
	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	/*
	 * For modules that need to know when subsystem init is complete,
	 * inform them now.
	 */
	if (rc == 0) {
		TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, internal.tailq) {
			if (m->init_complete) {
				m->init_complete();
			}
		}
	}

	cb_fn(cb_arg, rc);
}

static bool
bdev_module_all_actions_completed(void)
{
	struct spdk_bdev_module *m;

	TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (m->internal.action_in_progress > 0) {
			return false;
		}
	}
	return true;
}

static void
bdev_module_action_complete(void)
{
	/*
	 * Don't finish bdev subsystem initialization if
	 * module pre-initialization is still in progress, or
	 * the subsystem been already initialized.
	 */
	if (!g_bdev_mgr.module_init_complete || g_bdev_mgr.init_complete) {
		return;
	}

	/*
	 * Check all bdev modules for inits/examinations in progress. If any
	 * exist, return immediately since we cannot finish bdev subsystem
	 * initialization until all are completed.
	 */
	if (!bdev_module_all_actions_completed()) {
		return;
	}

	/*
	 * Modules already finished initialization - now that all
	 * the bdev modules have finished their asynchronous I/O
	 * processing, the entire bdev layer can be marked as complete.
	 */
	bdev_init_complete(0);
}

static void
bdev_module_action_done(struct spdk_bdev_module *module)
{
	spdk_spin_lock(&module->internal.spinlock);
	assert(module->internal.action_in_progress > 0);
	module->internal.action_in_progress--;
	spdk_spin_unlock(&module->internal.spinlock);
	bdev_module_action_complete();
}

void
spdk_bdev_module_init_done(struct spdk_bdev_module *module)
{
	assert(module->async_init);
	bdev_module_action_done(module);
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
	bdev_module_action_done(module);
}

/** The last initialized bdev module */
static struct spdk_bdev_module *g_resume_bdev_module = NULL;

static void
bdev_init_failed(void *cb_arg)
{
	struct spdk_bdev_module *module = cb_arg;

	spdk_spin_lock(&module->internal.spinlock);
	assert(module->internal.action_in_progress > 0);
	module->internal.action_in_progress--;
	spdk_spin_unlock(&module->internal.spinlock);
	bdev_init_complete(-1);
}

static int
bdev_modules_init(void)
{
	struct spdk_bdev_module *module;
	int rc = 0;

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		g_resume_bdev_module = module;
		if (module->async_init) {
			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress = 1;
			spdk_spin_unlock(&module->internal.spinlock);
		}
		rc = module->module_init();
		if (rc != 0) {
			/* Bump action_in_progress to prevent other modules from completion of modules_init
			 * Send message to defer application shutdown until resources are cleaned up */
			spdk_spin_lock(&module->internal.spinlock);
			module->internal.action_in_progress = 1;
			spdk_spin_unlock(&module->internal.spinlock);
			spdk_thread_send_msg(spdk_get_thread(), bdev_init_failed, module);
			return rc;
		}
	}

	g_resume_bdev_module = NULL;
	return 0;
}

void
spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg)
{
	int rc = 0;
	char mempool_name[32];

	assert(cb_fn != NULL);

	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;

	spdk_notify_type_register("bdev_register");
	spdk_notify_type_register("bdev_unregister");

	snprintf(mempool_name, sizeof(mempool_name), "bdev_io_%d", getpid());

	rc = spdk_iobuf_register_module("bdev");
	if (rc != 0) {
		SPDK_ERRLOG("could not register bdev iobuf module: %s\n", spdk_strerror(-rc));
		bdev_init_complete(-1);
		return;
	}

	g_bdev_mgr.bdev_io_pool = spdk_mempool_create(mempool_name,
				  g_bdev_opts.bdev_io_pool_size,
				  sizeof(struct spdk_bdev_io) +
				  bdev_module_get_max_ctx_size(),
				  0,
				  SPDK_ENV_SOCKET_ID_ANY);

	if (g_bdev_mgr.bdev_io_pool == NULL) {
		SPDK_ERRLOG("could not allocate spdk_bdev_io pool\n");
		bdev_init_complete(-1);
		return;
	}

	g_bdev_mgr.zero_buffer = spdk_zmalloc(ZERO_BUFFER_SIZE, ZERO_BUFFER_SIZE,
					      NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_bdev_mgr.zero_buffer) {
		SPDK_ERRLOG("create bdev zero buffer failed\n");
		bdev_init_complete(-1);
		return;
	}

#ifdef SPDK_CONFIG_VTUNE
	SPDK_LOG_DEPRECATED(vtune_support);
	g_bdev_mgr.domain = __itt_domain_create("spdk_bdev");
#endif

	spdk_io_device_register(&g_bdev_mgr, bdev_mgmt_channel_create,
				bdev_mgmt_channel_destroy,
				sizeof(struct spdk_bdev_mgmt_channel),
				"bdev_mgr");

	rc = bdev_modules_init();
	g_bdev_mgr.module_init_complete = true;
	if (rc != 0) {
		SPDK_ERRLOG("bdev modules init failed\n");
		return;
	}

	bdev_module_action_complete();
}

static void
bdev_mgr_unregister_cb(void *io_device)
{
	spdk_bdev_fini_cb cb_fn = g_fini_cb_fn;

	if (g_bdev_mgr.bdev_io_pool) {
		if (spdk_mempool_count(g_bdev_mgr.bdev_io_pool) != g_bdev_opts.bdev_io_pool_size) {
			SPDK_ERRLOG("bdev IO pool count is %zu but should be %u\n",
				    spdk_mempool_count(g_bdev_mgr.bdev_io_pool),
				    g_bdev_opts.bdev_io_pool_size);
		}

		spdk_mempool_free(g_bdev_mgr.bdev_io_pool);
	}

	spdk_free(g_bdev_mgr.zero_buffer);

	bdev_examine_allowlist_free();

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
	g_bdev_mgr.init_complete = false;
	g_bdev_mgr.module_init_complete = false;
}

static void
bdev_module_fini_iter(void *arg)
{
	struct spdk_bdev_module *bdev_module;

	/* FIXME: Handling initialization failures is broken now,
	 * so we won't even try cleaning up after successfully
	 * initialized modules. if module_init_complete is false,
	 * just call spdk_bdev_mgr_unregister_cb
	 */
	if (!g_bdev_mgr.module_init_complete) {
		bdev_mgr_unregister_cb(NULL);
		return;
	}

	/* Start iterating from the last touched module */
	if (!g_resume_bdev_module) {
		bdev_module = TAILQ_LAST(&g_bdev_mgr.bdev_modules, bdev_module_list);
	} else {
		bdev_module = TAILQ_PREV(g_resume_bdev_module, bdev_module_list,
					 internal.tailq);
	}

	while (bdev_module) {
		if (bdev_module->async_fini) {
			/* Save our place so we can resume later. We must
			 * save the variable here, before calling module_fini()
			 * below, because in some cases the module may immediately
			 * call spdk_bdev_module_fini_done() and re-enter
			 * this function to continue iterating. */
			g_resume_bdev_module = bdev_module;
		}

		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}

		if (bdev_module->async_fini) {
			return;
		}

		bdev_module = TAILQ_PREV(bdev_module, bdev_module_list,
					 internal.tailq);
	}

	g_resume_bdev_module = NULL;
	spdk_io_device_unregister(&g_bdev_mgr, bdev_mgr_unregister_cb);
}

void
spdk_bdev_module_fini_done(void)
{
	if (spdk_get_thread() != g_fini_thread) {
		spdk_thread_send_msg(g_fini_thread, bdev_module_fini_iter, NULL);
	} else {
		bdev_module_fini_iter(NULL);
	}
}

static void
bdev_finish_unregister_bdevs_iter(void *cb_arg, int bdeverrno)
{
	struct spdk_bdev *bdev = cb_arg;

	if (bdeverrno && bdev) {
		SPDK_WARNLOG("Unable to unregister bdev '%s' during spdk_bdev_finish()\n",
			     bdev->name);

		/*
		 * Since the call to spdk_bdev_unregister() failed, we have no way to free this
		 *  bdev; try to continue by manually removing this bdev from the list and continue
		 *  with the next bdev in the list.
		 */
		TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, internal.link);
	}

	if (TAILQ_EMPTY(&g_bdev_mgr.bdevs)) {
		SPDK_DEBUGLOG(bdev, "Done unregistering bdevs\n");
		/*
		 * Bdev module finish need to be deferred as we might be in the middle of some context
		 * (like bdev part free) that will use this bdev (or private bdev driver ctx data)
		 * after returning.
		 */
		spdk_thread_send_msg(spdk_get_thread(), bdev_module_fini_iter, NULL);
		return;
	}

	/*
	 * Unregister last unclaimed bdev in the list, to ensure that bdev subsystem
	 * shutdown proceeds top-down. The goal is to give virtual bdevs an opportunity
	 * to detect clean shutdown as opposed to run-time hot removal of the underlying
	 * base bdevs.
	 *
	 * Also, walk the list in the reverse order.
	 */
	for (bdev = TAILQ_LAST(&g_bdev_mgr.bdevs, spdk_bdev_list);
	     bdev; bdev = TAILQ_PREV(bdev, spdk_bdev_list, internal.link)) {
		spdk_spin_lock(&bdev->internal.spinlock);
		if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
			SPDK_DEBUGLOG(bdev, "Skipping claimed bdev '%s'(<-'%s').\n",
				      bdev->name, bdev->internal.claim.v1.module->name);
			spdk_spin_unlock(&bdev->internal.spinlock);
			continue;
		}
		spdk_spin_unlock(&bdev->internal.spinlock);

		SPDK_DEBUGLOG(bdev, "Unregistering bdev '%s'\n", bdev->name);
		spdk_bdev_unregister(bdev, bdev_finish_unregister_bdevs_iter, bdev);
		return;
	}

	/*
	 * If any bdev fails to unclaim underlying bdev properly, we may face the
	 * case of bdev list consisting of claimed bdevs only (if claims are managed
	 * correctly, this would mean there's a loop in the claims graph which is
	 * clearly impossible). Warn and unregister last bdev on the list then.
	 */
	for (bdev = TAILQ_LAST(&g_bdev_mgr.bdevs, spdk_bdev_list);
	     bdev; bdev = TAILQ_PREV(bdev, spdk_bdev_list, internal.link)) {
		SPDK_WARNLOG("Unregistering claimed bdev '%s'!\n", bdev->name);
		spdk_bdev_unregister(bdev, bdev_finish_unregister_bdevs_iter, bdev);
		return;
	}
}

static void
bdev_module_fini_start_iter(void *arg)
{
	struct spdk_bdev_module *bdev_module;

	if (!g_resume_bdev_module) {
		bdev_module = TAILQ_LAST(&g_bdev_mgr.bdev_modules, bdev_module_list);
	} else {
		bdev_module = TAILQ_PREV(g_resume_bdev_module, bdev_module_list, internal.tailq);
	}

	while (bdev_module) {
		if (bdev_module->async_fini_start) {
			/* Save our place so we can resume later. We must
			 * save the variable here, before calling fini_start()
			 * below, because in some cases the module may immediately
			 * call spdk_bdev_module_fini_start_done() and re-enter
			 * this function to continue iterating. */
			g_resume_bdev_module = bdev_module;
		}

		if (bdev_module->fini_start) {
			bdev_module->fini_start();
		}

		if (bdev_module->async_fini_start) {
			return;
		}

		bdev_module = TAILQ_PREV(bdev_module, bdev_module_list, internal.tailq);
	}

	g_resume_bdev_module = NULL;

	bdev_finish_unregister_bdevs_iter(NULL, 0);
}

void
spdk_bdev_module_fini_start_done(void)
{
	if (spdk_get_thread() != g_fini_thread) {
		spdk_thread_send_msg(g_fini_thread, bdev_module_fini_start_iter, NULL);
	} else {
		bdev_module_fini_start_iter(NULL);
	}
}

static void
bdev_finish_wait_for_examine_done(void *cb_arg)
{
	bdev_module_fini_start_iter(NULL);
}

void
spdk_bdev_finish(spdk_bdev_fini_cb cb_fn, void *cb_arg)
{
	int rc;

	assert(cb_fn != NULL);

	g_fini_thread = spdk_get_thread();

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	rc = spdk_bdev_wait_for_examine(bdev_finish_wait_for_examine_done, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("wait_for_examine failed: %s\n", spdk_strerror(-rc));
		bdev_finish_wait_for_examine_done(NULL);
	}
}

struct spdk_bdev_io *
bdev_channel_get_io(struct spdk_bdev_channel *channel)
{
	struct spdk_bdev_mgmt_channel *ch = channel->shared_resource->mgmt_ch;
	struct spdk_bdev_io *bdev_io;

	if (ch->per_thread_cache_count > 0) {
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, internal.buf_link);
		ch->per_thread_cache_count--;
	} else if (spdk_unlikely(!TAILQ_EMPTY(&ch->io_wait_queue))) {
		/*
		 * Don't try to look for bdev_ios in the global pool if there are
		 * waiters on bdev_ios - we don't want this caller to jump the line.
		 */
		bdev_io = NULL;
	} else {
		bdev_io = spdk_mempool_get(g_bdev_mgr.bdev_io_pool);
	}

	return bdev_io;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_mgmt_channel *ch;

	assert(bdev_io != NULL);
	assert(bdev_io->internal.status != SPDK_BDEV_IO_STATUS_PENDING);

	ch = bdev_io->internal.ch->shared_resource->mgmt_ch;

	if (bdev_io->internal.buf != NULL) {
		bdev_io_put_buf(bdev_io);
	}

	if (ch->per_thread_cache_count < ch->bdev_io_cache_size) {
		ch->per_thread_cache_count++;
		STAILQ_INSERT_HEAD(&ch->per_thread_cache, bdev_io, internal.buf_link);
		while (ch->per_thread_cache_count > 0 && !TAILQ_EMPTY(&ch->io_wait_queue)) {
			struct spdk_bdev_io_wait_entry *entry;

			entry = TAILQ_FIRST(&ch->io_wait_queue);
			TAILQ_REMOVE(&ch->io_wait_queue, entry, link);
			entry->cb_fn(entry->cb_arg);
		}
	} else {
		/* We should never have a full cache with entries on the io wait queue. */
		assert(TAILQ_EMPTY(&ch->io_wait_queue));
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
	}
}

static bool
bdev_qos_is_iops_rate_limit(enum spdk_bdev_qos_rate_limit_type limit)
{
	assert(limit != SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);

	switch (limit) {
	case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
		return true;
	case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
	case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
		return false;
	case SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES:
	default:
		return false;
	}
}

static bool
bdev_qos_io_to_limit(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			return true;
		} else {
			return false;
		}
	default:
		return false;
	}
}

static bool
bdev_is_read_io(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* Bit 1 (0x2) set for read operation */
		if (bdev_io->u.nvme_passthru.cmd.opc & SPDK_NVME_OPC_READ) {
			return true;
		} else {
			return false;
		}
	case SPDK_BDEV_IO_TYPE_READ:
		return true;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Populate to read from disk */
		if (bdev_io->u.bdev.zcopy.populate) {
			return true;
		} else {
			return false;
		}
	default:
		return false;
	}
}

static uint64_t
bdev_get_io_size_in_byte(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev	*bdev = bdev_io->bdev;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return bdev_io->u.nvme_passthru.nbytes;
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_io->u.bdev.num_blocks * bdev->blocklen;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Track the data in the start phase only */
		if (bdev_io->u.bdev.zcopy.start) {
			return bdev_io->u.bdev.num_blocks * bdev->blocklen;
		} else {
			return 0;
		}
	default:
		return 0;
	}
}

static bool
bdev_qos_rw_queue_io(const struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (limit->max_per_timeslice > 0 && limit->remaining_this_timeslice <= 0) {
		return true;
	} else {
		return false;
	}
}

static bool
bdev_qos_r_queue_io(const struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == false) {
		return false;
	}

	return bdev_qos_rw_queue_io(limit, io);
}

static bool
bdev_qos_w_queue_io(const struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == true) {
		return false;
	}

	return bdev_qos_rw_queue_io(limit, io);
}

static void
bdev_qos_rw_iops_update_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	limit->remaining_this_timeslice--;
}

static void
bdev_qos_rw_bps_update_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	limit->remaining_this_timeslice -= bdev_get_io_size_in_byte(io);
}

static void
bdev_qos_r_bps_update_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == false) {
		return;
	}

	return bdev_qos_rw_bps_update_quota(limit, io);
}

static void
bdev_qos_w_bps_update_quota(struct spdk_bdev_qos_limit *limit, struct spdk_bdev_io *io)
{
	if (bdev_is_read_io(io) == true) {
		return;
	}

	return bdev_qos_rw_bps_update_quota(limit, io);
}

static void
bdev_qos_set_ops(struct spdk_bdev_qos *qos)
{
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			qos->rate_limits[i].queue_io = NULL;
			qos->rate_limits[i].update_quota = NULL;
			continue;
		}

		switch (i) {
		case SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_rw_queue_io;
			qos->rate_limits[i].update_quota = bdev_qos_rw_iops_update_quota;
			break;
		case SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_rw_queue_io;
			qos->rate_limits[i].update_quota = bdev_qos_rw_bps_update_quota;
			break;
		case SPDK_BDEV_QOS_R_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_r_queue_io;
			qos->rate_limits[i].update_quota = bdev_qos_r_bps_update_quota;
			break;
		case SPDK_BDEV_QOS_W_BPS_RATE_LIMIT:
			qos->rate_limits[i].queue_io = bdev_qos_w_queue_io;
			qos->rate_limits[i].update_quota = bdev_qos_w_bps_update_quota;
			break;
		default:
			break;
		}
	}
}

static void
_bdev_io_complete_in_submit(struct spdk_bdev_channel *bdev_ch,
			    struct spdk_bdev_io *bdev_io,
			    enum spdk_bdev_io_status status)
{
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;

	bdev_io->internal.in_submit_request = true;
	bdev_ch->io_outstanding++;
	shared_resource->io_outstanding++;
	spdk_bdev_io_complete(bdev_io, status);
	bdev_io->internal.in_submit_request = false;
}

static inline void
bdev_io_do_submit(struct spdk_bdev_channel *bdev_ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_io_channel *ch = bdev_ch->channel;
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT)) {
		struct spdk_bdev_mgmt_channel *mgmt_channel = shared_resource->mgmt_ch;
		struct spdk_bdev_io *bio_to_abort = bdev_io->u.abort.bio_to_abort;

		if (bdev_abort_queued_io(&shared_resource->nomem_io, bio_to_abort) ||
		    bdev_abort_buf_io(mgmt_channel, bio_to_abort)) {
			_bdev_io_complete_in_submit(bdev_ch, bdev_io,
						    SPDK_BDEV_IO_STATUS_SUCCESS);
			return;
		}
	}

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE &&
			  bdev_io->bdev->split_on_write_unit &&
			  bdev_io->u.bdev.num_blocks < bdev_io->bdev->write_unit_size)) {
		SPDK_ERRLOG("IO num_blocks %lu does not match the write_unit_size %u\n",
			    bdev_io->u.bdev.num_blocks, bdev_io->bdev->write_unit_size);
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (spdk_likely(TAILQ_EMPTY(&shared_resource->nomem_io))) {
		bdev_ch->io_outstanding++;
		shared_resource->io_outstanding++;
		bdev_io->internal.in_submit_request = true;
		bdev->fn_table->submit_request(ch, bdev_io);
		bdev_io->internal.in_submit_request = false;
	} else {
		TAILQ_INSERT_TAIL(&shared_resource->nomem_io, bdev_io, internal.link);
	}
}

static bool
bdev_qos_queue_io(struct spdk_bdev_qos *qos, struct spdk_bdev_io *bdev_io)
{
	int i;

	if (bdev_qos_io_to_limit(bdev_io) == true) {
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (!qos->rate_limits[i].queue_io) {
				continue;
			}

			if (qos->rate_limits[i].queue_io(&qos->rate_limits[i],
							 bdev_io) == true) {
				return true;
			}
		}
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (!qos->rate_limits[i].update_quota) {
				continue;
			}

			qos->rate_limits[i].update_quota(&qos->rate_limits[i], bdev_io);
		}
	}

	return false;
}

static inline void
_bdev_io_do_submit(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;

	bdev_io_do_submit(ch, bdev_io);
}

static int
bdev_qos_io_submit(struct spdk_bdev_channel *ch, struct spdk_bdev_qos *qos)
{
	struct spdk_bdev_io		*bdev_io = NULL, *tmp = NULL;
	int				submitted_ios = 0;

	TAILQ_FOREACH_SAFE(bdev_io, &qos->queued, internal.link, tmp) {
		if (!bdev_qos_queue_io(qos, bdev_io)) {
			TAILQ_REMOVE(&qos->queued, bdev_io, internal.link);

			if (bdev_io->internal.io_submit_ch) {
				/* Send back the IO to the original thread for the actual processing. */
				bdev_io->internal.ch = bdev_io->internal.io_submit_ch;
				bdev_io->internal.io_submit_ch = NULL;
				spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
						     _bdev_io_do_submit, bdev_io);
			} else {
				bdev_io_do_submit(ch, bdev_io);
			}

			submitted_ios++;
		}
	}

	return submitted_ios;
}

static void
bdev_queue_io_wait_with_cb(struct spdk_bdev_io *bdev_io, spdk_bdev_io_wait_cb cb_fn)
{
	int rc;

	bdev_io->internal.waitq_entry.bdev = bdev_io->bdev;
	bdev_io->internal.waitq_entry.cb_fn = cb_fn;
	bdev_io->internal.waitq_entry.cb_arg = bdev_io;
	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, spdk_io_channel_from_ctx(bdev_io->internal.ch),
				     &bdev_io->internal.waitq_entry);
	if (rc != 0) {
		SPDK_ERRLOG("Queue IO failed, rc=%d\n", rc);
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

static bool
bdev_rw_should_split(struct spdk_bdev_io *bdev_io)
{
	uint32_t io_boundary;
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint32_t max_size = bdev->max_segment_size;
	int max_segs = bdev->max_num_segments;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE && bdev->split_on_write_unit) {
		io_boundary = bdev->write_unit_size;
	} else if (bdev->split_on_optimal_io_boundary) {
		io_boundary = bdev->optimal_io_boundary;
	} else {
		io_boundary = 0;
	}

	if (spdk_likely(!io_boundary && !max_segs && !max_size)) {
		return false;
	}

	if (io_boundary) {
		uint64_t start_stripe, end_stripe;

		start_stripe = bdev_io->u.bdev.offset_blocks;
		end_stripe = start_stripe + bdev_io->u.bdev.num_blocks - 1;
		/* Avoid expensive div operations if possible.  These spdk_u32 functions are very cheap. */
		if (spdk_likely(spdk_u32_is_pow2(io_boundary))) {
			start_stripe >>= spdk_u32log2(io_boundary);
			end_stripe >>= spdk_u32log2(io_boundary);
		} else {
			start_stripe /= io_boundary;
			end_stripe /= io_boundary;
		}

		if (start_stripe != end_stripe) {
			return true;
		}
	}

	if (max_segs) {
		if (bdev_io->u.bdev.iovcnt > max_segs) {
			return true;
		}
	}

	if (max_size) {
		for (int i = 0; i < bdev_io->u.bdev.iovcnt; i++) {
			if (bdev_io->u.bdev.iovs[i].iov_len > max_size) {
				return true;
			}
		}
	}

	return false;
}

static bool
bdev_unmap_should_split(struct spdk_bdev_io *bdev_io)
{
	uint32_t num_unmap_segments;

	if (!bdev_io->bdev->max_unmap || !bdev_io->bdev->max_unmap_segments) {
		return false;
	}
	num_unmap_segments = spdk_divide_round_up(bdev_io->u.bdev.num_blocks, bdev_io->bdev->max_unmap);
	if (num_unmap_segments > bdev_io->bdev->max_unmap_segments) {
		return true;
	}

	return false;
}

static bool
bdev_write_zeroes_should_split(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io->bdev->max_write_zeroes) {
		return false;
	}

	if (bdev_io->u.bdev.num_blocks > bdev_io->bdev->max_write_zeroes) {
		return true;
	}

	return false;
}

static bool
bdev_copy_should_split(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io->bdev->max_copy != 0 &&
	    bdev_io->u.bdev.num_blocks > bdev_io->bdev->max_copy) {
		return true;
	}

	return false;
}

static bool
bdev_io_should_split(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_rw_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_unmap_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return bdev_write_zeroes_should_split(bdev_io);
	case SPDK_BDEV_IO_TYPE_COPY:
		return bdev_copy_should_split(bdev_io);
	default:
		return false;
	}
}

static uint32_t
_to_next_boundary(uint64_t offset, uint32_t boundary)
{
	return (boundary - (offset % boundary));
}

static void bdev_io_split_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void _bdev_rw_split(void *_bdev_io);

static void bdev_unmap_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_unmap_split(void *_bdev_io)
{
	return bdev_unmap_split((struct spdk_bdev_io *)_bdev_io);
}

static void bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_write_zeroes_split(void *_bdev_io)
{
	return bdev_write_zeroes_split((struct spdk_bdev_io *)_bdev_io);
}

static void bdev_copy_split(struct spdk_bdev_io *bdev_io);

static void
_bdev_copy_split(void *_bdev_io)
{
	return bdev_copy_split((struct spdk_bdev_io *)_bdev_io);
}

static int
bdev_io_split_submit(struct spdk_bdev_io *bdev_io, struct iovec *iov, int iovcnt, void *md_buf,
		     uint64_t num_blocks, uint64_t *offset, uint64_t *remaining)
{
	int rc;
	uint64_t current_offset, current_remaining, current_src_offset;
	spdk_bdev_io_wait_cb io_wait_fn;

	current_offset = *offset;
	current_remaining = *remaining;

	bdev_io->u.bdev.split_outstanding++;

	io_wait_fn = _bdev_rw_split;
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rc = bdev_readv_blocks_with_md(bdev_io->internal.desc,
					       spdk_io_channel_from_ctx(bdev_io->internal.ch),
					       iov, iovcnt, md_buf, current_offset,
					       num_blocks,
					       bdev_io_split_done, bdev_io,
					       bdev_io->internal.ext_opts, true);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_writev_blocks_with_md(bdev_io->internal.desc,
						spdk_io_channel_from_ctx(bdev_io->internal.ch),
						iov, iovcnt, md_buf, current_offset,
						num_blocks,
						bdev_io_split_done, bdev_io,
						bdev_io->internal.ext_opts, true);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		io_wait_fn = _bdev_unmap_split;
		rc = spdk_bdev_unmap_blocks(bdev_io->internal.desc,
					    spdk_io_channel_from_ctx(bdev_io->internal.ch),
					    current_offset, num_blocks,
					    bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		io_wait_fn = _bdev_write_zeroes_split;
		rc = spdk_bdev_write_zeroes_blocks(bdev_io->internal.desc,
						   spdk_io_channel_from_ctx(bdev_io->internal.ch),
						   current_offset, num_blocks,
						   bdev_io_split_done, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		io_wait_fn = _bdev_copy_split;
		current_src_offset = bdev_io->u.bdev.copy.src_offset_blocks +
				     (current_offset - bdev_io->u.bdev.offset_blocks);
		rc = spdk_bdev_copy_blocks(bdev_io->internal.desc,
					   spdk_io_channel_from_ctx(bdev_io->internal.ch),
					   current_offset, current_src_offset, num_blocks,
					   bdev_io_split_done, bdev_io);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		current_offset += num_blocks;
		current_remaining -= num_blocks;
		bdev_io->u.bdev.split_current_offset_blocks = current_offset;
		bdev_io->u.bdev.split_remaining_num_blocks = current_remaining;
		*offset = current_offset;
		*remaining = current_remaining;
	} else {
		bdev_io->u.bdev.split_outstanding--;
		if (rc == -ENOMEM) {
			if (bdev_io->u.bdev.split_outstanding == 0) {
				/* No I/O is outstanding. Hence we should wait here. */
				bdev_queue_io_wait_with_cb(bdev_io, io_wait_fn);
			}
		} else {
			bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			if (bdev_io->u.bdev.split_outstanding == 0) {
				spdk_trace_record(TRACE_BDEV_IO_DONE, 0, 0, (uintptr_t)bdev_io, bdev_io->internal.caller_ctx);
				TAILQ_REMOVE(&bdev_io->internal.ch->io_submitted, bdev_io, internal.ch_link);
				bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
			}
		}
	}

	return rc;
}

static void
_bdev_rw_split(void *_bdev_io)
{
	struct iovec *parent_iov, *iov;
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct spdk_bdev *bdev = bdev_io->bdev;
	uint64_t parent_offset, current_offset, remaining;
	uint32_t parent_iov_offset, parent_iovcnt, parent_iovpos, child_iovcnt;
	uint32_t to_next_boundary, to_next_boundary_bytes, to_last_block_bytes;
	uint32_t iovcnt, iov_len, child_iovsize;
	uint32_t blocklen = bdev->blocklen;
	uint32_t io_boundary;
	uint32_t max_segment_size = bdev->max_segment_size;
	uint32_t max_child_iovcnt = bdev->max_num_segments;
	void *md_buf = NULL;
	int rc;

	max_segment_size = max_segment_size ? max_segment_size : UINT32_MAX;
	max_child_iovcnt = max_child_iovcnt ? spdk_min(max_child_iovcnt, SPDK_BDEV_IO_NUM_CHILD_IOV) :
			   SPDK_BDEV_IO_NUM_CHILD_IOV;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE && bdev->split_on_write_unit) {
		io_boundary = bdev->write_unit_size;
	} else if (bdev->split_on_optimal_io_boundary) {
		io_boundary = bdev->optimal_io_boundary;
	} else {
		io_boundary = UINT32_MAX;
	}

	remaining = bdev_io->u.bdev.split_remaining_num_blocks;
	current_offset = bdev_io->u.bdev.split_current_offset_blocks;
	parent_offset = bdev_io->u.bdev.offset_blocks;
	parent_iov_offset = (current_offset - parent_offset) * blocklen;
	parent_iovcnt = bdev_io->u.bdev.iovcnt;

	for (parent_iovpos = 0; parent_iovpos < parent_iovcnt; parent_iovpos++) {
		parent_iov = &bdev_io->u.bdev.iovs[parent_iovpos];
		if (parent_iov_offset < parent_iov->iov_len) {
			break;
		}
		parent_iov_offset -= parent_iov->iov_len;
	}

	child_iovcnt = 0;
	while (remaining > 0 && parent_iovpos < parent_iovcnt &&
	       child_iovcnt < SPDK_BDEV_IO_NUM_CHILD_IOV) {
		to_next_boundary = _to_next_boundary(current_offset, io_boundary);
		to_next_boundary = spdk_min(remaining, to_next_boundary);
		to_next_boundary_bytes = to_next_boundary * blocklen;

		iov = &bdev_io->child_iov[child_iovcnt];
		iovcnt = 0;

		if (bdev_io->u.bdev.md_buf) {
			md_buf = (char *)bdev_io->u.bdev.md_buf +
				 (current_offset - parent_offset) * spdk_bdev_get_md_size(bdev);
		}

		child_iovsize = spdk_min(SPDK_BDEV_IO_NUM_CHILD_IOV - child_iovcnt, max_child_iovcnt);
		while (to_next_boundary_bytes > 0 && parent_iovpos < parent_iovcnt &&
		       iovcnt < child_iovsize) {
			parent_iov = &bdev_io->u.bdev.iovs[parent_iovpos];
			iov_len = parent_iov->iov_len - parent_iov_offset;

			iov_len = spdk_min(iov_len, max_segment_size);
			iov_len = spdk_min(iov_len, to_next_boundary_bytes);
			to_next_boundary_bytes -= iov_len;

			bdev_io->child_iov[child_iovcnt].iov_base = parent_iov->iov_base + parent_iov_offset;
			bdev_io->child_iov[child_iovcnt].iov_len = iov_len;

			if (iov_len < parent_iov->iov_len - parent_iov_offset) {
				parent_iov_offset += iov_len;
			} else {
				parent_iovpos++;
				parent_iov_offset = 0;
			}
			child_iovcnt++;
			iovcnt++;
		}

		if (to_next_boundary_bytes > 0) {
			/* We had to stop this child I/O early because we ran out of
			 * child_iov space or were limited by max_num_segments.
			 * Ensure the iovs to be aligned with block size and
			 * then adjust to_next_boundary before starting the
			 * child I/O.
			 */
			assert(child_iovcnt == SPDK_BDEV_IO_NUM_CHILD_IOV ||
			       iovcnt == child_iovsize);
			to_last_block_bytes = to_next_boundary_bytes % blocklen;
			if (to_last_block_bytes != 0) {
				uint32_t child_iovpos = child_iovcnt - 1;
				/* don't decrease child_iovcnt when it equals to SPDK_BDEV_IO_NUM_CHILD_IOV
				 * so the loop will naturally end
				 */

				to_last_block_bytes = blocklen - to_last_block_bytes;
				to_next_boundary_bytes += to_last_block_bytes;
				while (to_last_block_bytes > 0 && iovcnt > 0) {
					iov_len = spdk_min(to_last_block_bytes,
							   bdev_io->child_iov[child_iovpos].iov_len);
					bdev_io->child_iov[child_iovpos].iov_len -= iov_len;
					if (bdev_io->child_iov[child_iovpos].iov_len == 0) {
						child_iovpos--;
						if (--iovcnt == 0) {
							/* If the child IO is less than a block size just return.
							 * If the first child IO of any split round is less than
							 * a block size, an error exit.
							 */
							if (bdev_io->u.bdev.split_outstanding == 0) {
								SPDK_ERRLOG("The first child io was less than a block size\n");
								bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
								spdk_trace_record(TRACE_BDEV_IO_DONE, 0, 0, (uintptr_t)bdev_io, bdev_io->internal.caller_ctx);
								TAILQ_REMOVE(&bdev_io->internal.ch->io_submitted, bdev_io, internal.ch_link);
								bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
							}

							return;
						}
					}

					to_last_block_bytes -= iov_len;

					if (parent_iov_offset == 0) {
						parent_iovpos--;
						parent_iov_offset = bdev_io->u.bdev.iovs[parent_iovpos].iov_len;
					}
					parent_iov_offset -= iov_len;
				}

				assert(to_last_block_bytes == 0);
			}
			to_next_boundary -= to_next_boundary_bytes / blocklen;
		}

		rc = bdev_io_split_submit(bdev_io, iov, iovcnt, md_buf, to_next_boundary,
					  &current_offset, &remaining);
		if (spdk_unlikely(rc)) {
			return;
		}
	}
}

static void
bdev_unmap_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, unmap_blocks, remaining, max_unmap_blocks;
	uint32_t num_children_reqs = 0;
	int rc;

	offset = bdev_io->u.bdev.split_current_offset_blocks;
	remaining = bdev_io->u.bdev.split_remaining_num_blocks;
	max_unmap_blocks = bdev_io->bdev->max_unmap * bdev_io->bdev->max_unmap_segments;

	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS)) {
		unmap_blocks = spdk_min(remaining, max_unmap_blocks);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, unmap_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, write_zeroes_blocks, remaining;
	uint32_t num_children_reqs = 0;
	int rc;

	offset = bdev_io->u.bdev.split_current_offset_blocks;
	remaining = bdev_io->u.bdev.split_remaining_num_blocks;

	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_UNMAP_WRITE_ZEROES_REQS)) {
		write_zeroes_blocks = spdk_min(remaining, bdev_io->bdev->max_write_zeroes);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, write_zeroes_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
bdev_copy_split(struct spdk_bdev_io *bdev_io)
{
	uint64_t offset, copy_blocks, remaining;
	uint32_t num_children_reqs = 0;
	int rc;

	offset = bdev_io->u.bdev.split_current_offset_blocks;
	remaining = bdev_io->u.bdev.split_remaining_num_blocks;

	assert(bdev_io->bdev->max_copy != 0);
	while (remaining && (num_children_reqs < SPDK_BDEV_MAX_CHILDREN_COPY_REQS)) {
		copy_blocks = spdk_min(remaining, bdev_io->bdev->max_copy);

		rc = bdev_io_split_submit(bdev_io, NULL, 0, NULL, copy_blocks,
					  &offset, &remaining);
		if (spdk_likely(rc == 0)) {
			num_children_reqs++;
		} else {
			return;
		}
	}
}

static void
parent_bdev_io_complete(void *ctx, int rc)
{
	struct spdk_bdev_io *parent_io = ctx;

	if (rc) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	parent_io->internal.cb(parent_io, parent_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS,
			       parent_io->internal.caller_ctx);
}

static void
bdev_io_split_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		/* If any child I/O failed, stop further splitting process. */
		parent_io->u.bdev.split_current_offset_blocks += parent_io->u.bdev.split_remaining_num_blocks;
		parent_io->u.bdev.split_remaining_num_blocks = 0;
	}
	parent_io->u.bdev.split_outstanding--;
	if (parent_io->u.bdev.split_outstanding != 0) {
		return;
	}

	/*
	 * Parent I/O finishes when all blocks are consumed.
	 */
	if (parent_io->u.bdev.split_remaining_num_blocks == 0) {
		assert(parent_io->internal.cb != bdev_io_split_done);
		spdk_trace_record(TRACE_BDEV_IO_DONE, 0, 0, (uintptr_t)parent_io, bdev_io->internal.caller_ctx);
		TAILQ_REMOVE(&parent_io->internal.ch->io_submitted, parent_io, internal.ch_link);

		if (parent_io->internal.orig_iovcnt != 0) {
			_bdev_io_push_bounce_data_buffer(parent_io, parent_bdev_io_complete);
			/* bdev IO will be completed in the callback */
		} else {
			parent_bdev_io_complete(parent_io, 0);
		}
		return;
	}

	/*
	 * Continue with the splitting process.  This function will complete the parent I/O if the
	 * splitting is done.
	 */
	switch (parent_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		_bdev_rw_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_unmap_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_write_zeroes_split(parent_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_copy_split(parent_io);
		break;
	default:
		assert(false);
		break;
	}
}

static void bdev_rw_split_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
				     bool success);

static void
bdev_io_split(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.split_current_offset_blocks = bdev_io->u.bdev.offset_blocks;
	bdev_io->u.bdev.split_remaining_num_blocks = bdev_io->u.bdev.num_blocks;
	bdev_io->u.bdev.split_outstanding = 0;
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (_is_buf_allocated(bdev_io->u.bdev.iovs)) {
			_bdev_rw_split(bdev_io);
		} else {
			assert(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);
			spdk_bdev_io_get_buf(bdev_io, bdev_rw_split_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_unmap_split(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_write_zeroes_split(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_copy_split(bdev_io);
		break;
	default:
		assert(false);
		break;
	}
}

static void
bdev_rw_split_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_bdev_rw_split(bdev_io);
}

/* Explicitly mark this inline, since it's used as a function pointer and otherwise won't
 *  be inlined, at least on some compilers.
 */
static inline void
_bdev_io_submit(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;

	if (spdk_likely(bdev_ch->flags == 0)) {
		bdev_io_do_submit(bdev_ch, bdev_io);
		return;
	}

	if (bdev_ch->flags & BDEV_CH_RESET_IN_PROGRESS) {
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
	} else if (bdev_ch->flags & BDEV_CH_QOS_ENABLED) {
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT) &&
		    bdev_abort_queued_io(&bdev->internal.qos->queued, bdev_io->u.abort.bio_to_abort)) {
			_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			TAILQ_INSERT_TAIL(&bdev->internal.qos->queued, bdev_io, internal.link);
			bdev_qos_io_submit(bdev_ch, bdev->internal.qos);
		}
	} else {
		SPDK_ERRLOG("unknown bdev_ch flag %x found\n", bdev_ch->flags);
		_bdev_io_complete_in_submit(bdev_ch, bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

bool bdev_lba_range_overlapped(struct lba_range *range1, struct lba_range *range2);

bool
bdev_lba_range_overlapped(struct lba_range *range1, struct lba_range *range2)
{
	if (range1->length == 0 || range2->length == 0) {
		return false;
	}

	if (range1->offset + range1->length <= range2->offset) {
		return false;
	}

	if (range2->offset + range2->length <= range1->offset) {
		return false;
	}

	return true;
}

static bool
bdev_io_range_is_locked(struct spdk_bdev_io *bdev_io, struct lba_range *range)
{
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;
	struct lba_range r;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		/* Don't try to decode the NVMe command - just assume worst-case and that
		 * it overlaps a locked range.
		 */
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
	case SPDK_BDEV_IO_TYPE_COPY:
		r.offset = bdev_io->u.bdev.offset_blocks;
		r.length = bdev_io->u.bdev.num_blocks;
		if (!bdev_lba_range_overlapped(range, &r)) {
			/* This I/O doesn't overlap the specified LBA range. */
			return false;
		} else if (range->owner_ch == ch && range->locked_ctx == bdev_io->internal.caller_ctx) {
			/* This I/O overlaps, but the I/O is on the same channel that locked this
			 * range, and the caller_ctx is the same as the locked_ctx.  This means
			 * that this I/O is associated with the lock, and is allowed to execute.
			 */
			return false;
		} else {
			return true;
		}
	default:
		return false;
	}
}

void
bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_thread *thread = spdk_bdev_io_get_thread(bdev_io);
	struct spdk_bdev_channel *ch = bdev_io->internal.ch;

	assert(thread != NULL);
	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_PENDING);

	if (!TAILQ_EMPTY(&ch->locked_ranges)) {
		struct lba_range *range;

		TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
			if (bdev_io_range_is_locked(bdev_io, range)) {
				TAILQ_INSERT_TAIL(&ch->io_locked, bdev_io, internal.ch_link);
				return;
			}
		}
	}

	TAILQ_INSERT_TAIL(&ch->io_submitted, bdev_io, internal.ch_link);

	bdev_io->internal.submit_tsc = spdk_get_ticks();
	spdk_trace_record_tsc(bdev_io->internal.submit_tsc, TRACE_BDEV_IO_START, 0, 0,
			      (uintptr_t)bdev_io, (uint64_t)bdev_io->type, bdev_io->internal.caller_ctx,
			      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			      spdk_bdev_get_name(bdev));

	if (bdev_io_should_split(bdev_io)) {
		bdev_io_split(NULL, bdev_io);
		return;
	}

	if (ch->flags & BDEV_CH_QOS_ENABLED) {
		if ((thread == bdev->internal.qos->thread) || !bdev->internal.qos->thread) {
			_bdev_io_submit(bdev_io);
		} else {
			bdev_io->internal.io_submit_ch = ch;
			bdev_io->internal.ch = bdev->internal.qos->ch;
			spdk_thread_send_msg(bdev->internal.qos->thread, _bdev_io_submit, bdev_io);
		}
	} else {
		_bdev_io_submit(bdev_io);
	}
}

static inline void
_bdev_io_copy_ext_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	struct spdk_bdev_ext_io_opts *opts_copy = &bdev_io->internal.ext_opts_copy;

	/* Zero part we don't copy */
	memset(((char *)opts_copy) + opts->size, 0, sizeof(*opts) - opts->size);
	memcpy(opts_copy, opts, opts->size);
	opts_copy->size = sizeof(*opts_copy);
	opts_copy->metadata = bdev_io->u.bdev.md_buf;
	/* Save pointer to the copied ext_opts which will be used by bdev modules */
	bdev_io->u.bdev.ext_opts = opts_copy;
}

static inline void
_bdev_io_ext_use_bounce_buffer(struct spdk_bdev_io *bdev_io)
{
	/* bdev doesn't support memory domains, thereby buffers in this IO request can't
	 * be accessed directly. It is needed to allocate buffers before issuing IO operation.
	 * For write operation we need to pull buffers from memory domain before submitting IO.
	 * Once read operation completes, we need to use memory_domain push functionality to
	 * update data in original memory domain IO buffer
	 * This IO request will go through a regular IO flow, so clear memory domains pointers in
	 * the copied ext_opts */
	bdev_io->internal.ext_opts_copy.memory_domain = NULL;
	bdev_io->internal.ext_opts_copy.memory_domain_ctx = NULL;
	_bdev_memory_domain_io_get_buf(bdev_io, _bdev_memory_domain_get_io_cb,
				       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

static inline void
_bdev_io_submit_ext(struct spdk_bdev_desc *desc, struct spdk_bdev_io *bdev_io,
		    struct spdk_bdev_ext_io_opts *opts, bool copy_opts)
{
	if (opts) {
		bool use_pull_push = opts->memory_domain && !desc->memory_domains_supported;
		assert(opts->size <= sizeof(*opts));
		/*
		 * copy if size is smaller than opts struct to avoid having to check size
		 * on every access to bdev_io->u.bdev.ext_opts
		 */
		if (copy_opts || use_pull_push || opts->size < sizeof(*opts)) {
			_bdev_io_copy_ext_opts(bdev_io, opts);
			if (use_pull_push) {
				_bdev_io_ext_use_bounce_buffer(bdev_io);
				return;
			}
		}
	}
	bdev_io_submit(bdev_io);
}

static void
bdev_io_submit_reset(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	struct spdk_io_channel *ch = bdev_ch->channel;

	assert(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_PENDING);

	bdev_io->internal.in_submit_request = true;
	bdev->fn_table->submit_request(ch, bdev_io);
	bdev_io->internal.in_submit_request = false;
}

void
bdev_io_init(struct spdk_bdev_io *bdev_io,
	     struct spdk_bdev *bdev, void *cb_arg,
	     spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	bdev_io->internal.caller_ctx = cb_arg;
	bdev_io->internal.cb = cb;
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING;
	bdev_io->internal.in_submit_request = false;
	bdev_io->internal.buf = NULL;
	bdev_io->internal.io_submit_ch = NULL;
	bdev_io->internal.orig_iovs = NULL;
	bdev_io->internal.orig_iovcnt = 0;
	bdev_io->internal.orig_md_iov.iov_base = NULL;
	bdev_io->internal.error.nvme.cdw0 = 0;
	bdev_io->num_retries = 0;
	bdev_io->internal.get_buf_cb = NULL;
	bdev_io->internal.get_aux_buf_cb = NULL;
	bdev_io->internal.ext_opts = NULL;
	bdev_io->internal.data_transfer_cpl = NULL;
}

static bool
bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	return bdev->fn_table->io_type_supported(bdev->ctxt, io_type);
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	bool supported;

	supported = bdev_io_type_supported(bdev, io_type);

	if (!supported) {
		switch (io_type) {
		case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
			/* The bdev layer will emulate write zeroes as long as write is supported. */
			supported = bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE);
			break;
		default:
			break;
		}
	}

	return supported;
}

uint64_t
spdk_bdev_io_get_submit_tsc(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.submit_tsc;
}

int
spdk_bdev_dump_info_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	if (bdev->fn_table->dump_info_json) {
		return bdev->fn_table->dump_info_json(bdev->ctxt, w);
	}

	return 0;
}

static void
bdev_qos_update_max_quota_per_timeslice(struct spdk_bdev_qos *qos)
{
	uint32_t max_per_timeslice = 0;
	int i;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (qos->rate_limits[i].limit == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			qos->rate_limits[i].max_per_timeslice = 0;
			continue;
		}

		max_per_timeslice = qos->rate_limits[i].limit *
				    SPDK_BDEV_QOS_TIMESLICE_IN_USEC / SPDK_SEC_TO_USEC;

		qos->rate_limits[i].max_per_timeslice = spdk_max(max_per_timeslice,
							qos->rate_limits[i].min_per_timeslice);

		qos->rate_limits[i].remaining_this_timeslice = qos->rate_limits[i].max_per_timeslice;
	}

	bdev_qos_set_ops(qos);
}

static int
bdev_channel_poll_qos(void *arg)
{
	struct spdk_bdev_qos *qos = arg;
	uint64_t now = spdk_get_ticks();
	int i;

	if (now < (qos->last_timeslice + qos->timeslice_size)) {
		/* We received our callback earlier than expected - return
		 *  immediately and wait to do accounting until at least one
		 *  timeslice has actually expired.  This should never happen
		 *  with a well-behaved timer implementation.
		 */
		return SPDK_POLLER_IDLE;
	}

	/* Reset for next round of rate limiting */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		/* We may have allowed the IOs or bytes to slightly overrun in the last
		 * timeslice. remaining_this_timeslice is signed, so if it's negative
		 * here, we'll account for the overrun so that the next timeslice will
		 * be appropriately reduced.
		 */
		if (qos->rate_limits[i].remaining_this_timeslice > 0) {
			qos->rate_limits[i].remaining_this_timeslice = 0;
		}
	}

	while (now >= (qos->last_timeslice + qos->timeslice_size)) {
		qos->last_timeslice += qos->timeslice_size;
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			qos->rate_limits[i].remaining_this_timeslice +=
				qos->rate_limits[i].max_per_timeslice;
		}
	}

	return bdev_qos_io_submit(qos->ch, qos);
}

static void
bdev_channel_destroy_resource(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_shared_resource *shared_resource;
	struct lba_range *range;

	bdev_free_io_stat(ch->stat);
#ifdef SPDK_CONFIG_VTUNE
	bdev_free_io_stat(ch->prev_stat);
#endif

	while (!TAILQ_EMPTY(&ch->locked_ranges)) {
		range = TAILQ_FIRST(&ch->locked_ranges);
		TAILQ_REMOVE(&ch->locked_ranges, range, tailq);
		free(range);
	}

	spdk_put_io_channel(ch->channel);

	shared_resource = ch->shared_resource;

	assert(TAILQ_EMPTY(&ch->io_locked));
	assert(TAILQ_EMPTY(&ch->io_submitted));
	assert(ch->io_outstanding == 0);
	assert(shared_resource->ref > 0);
	shared_resource->ref--;
	if (shared_resource->ref == 0) {
		assert(shared_resource->io_outstanding == 0);
		TAILQ_REMOVE(&shared_resource->mgmt_ch->shared_resources, shared_resource, link);
		spdk_put_io_channel(spdk_io_channel_from_ctx(shared_resource->mgmt_ch));
		free(shared_resource);
	}
}

static void
bdev_enable_qos(struct spdk_bdev *bdev, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_qos	*qos = bdev->internal.qos;
	int			i;

	assert(spdk_spin_held(&bdev->internal.spinlock));

	/* Rate limiting on this bdev enabled */
	if (qos) {
		if (qos->ch == NULL) {
			struct spdk_io_channel *io_ch;

			SPDK_DEBUGLOG(bdev, "Selecting channel %p as QoS channel for bdev %s on thread %p\n", ch,
				      bdev->name, spdk_get_thread());

			/* No qos channel has been selected, so set one up */

			/* Take another reference to ch */
			io_ch = spdk_get_io_channel(__bdev_to_io_dev(bdev));
			assert(io_ch != NULL);
			qos->ch = ch;

			qos->thread = spdk_io_channel_get_thread(io_ch);

			TAILQ_INIT(&qos->queued);

			for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
				if (bdev_qos_is_iops_rate_limit(i) == true) {
					qos->rate_limits[i].min_per_timeslice =
						SPDK_BDEV_QOS_MIN_IO_PER_TIMESLICE;
				} else {
					qos->rate_limits[i].min_per_timeslice =
						SPDK_BDEV_QOS_MIN_BYTE_PER_TIMESLICE;
				}

				if (qos->rate_limits[i].limit == 0) {
					qos->rate_limits[i].limit = SPDK_BDEV_QOS_LIMIT_NOT_DEFINED;
				}
			}
			bdev_qos_update_max_quota_per_timeslice(qos);
			qos->timeslice_size =
				SPDK_BDEV_QOS_TIMESLICE_IN_USEC * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
			qos->last_timeslice = spdk_get_ticks();
			qos->poller = SPDK_POLLER_REGISTER(bdev_channel_poll_qos,
							   qos,
							   SPDK_BDEV_QOS_TIMESLICE_IN_USEC);
		}

		ch->flags |= BDEV_CH_QOS_ENABLED;
	}
}

struct poll_timeout_ctx {
	struct spdk_bdev_desc	*desc;
	uint64_t		timeout_in_sec;
	spdk_bdev_io_timeout_cb	cb_fn;
	void			*cb_arg;
};

static void
bdev_desc_free(struct spdk_bdev_desc *desc)
{
	spdk_spin_destroy(&desc->spinlock);
	free(desc->media_events_buffer);
	free(desc);
}

static void
bdev_channel_poll_timeout_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct poll_timeout_ctx *ctx  = _ctx;
	struct spdk_bdev_desc *desc = ctx->desc;

	free(ctx);

	spdk_spin_lock(&desc->spinlock);
	desc->refs--;
	if (desc->closed == true && desc->refs == 0) {
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);
}

static void
bdev_channel_poll_timeout_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			     struct spdk_io_channel *io_ch, void *_ctx)
{
	struct poll_timeout_ctx *ctx  = _ctx;
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);
	struct spdk_bdev_desc *desc = ctx->desc;
	struct spdk_bdev_io *bdev_io;
	uint64_t now;

	spdk_spin_lock(&desc->spinlock);
	if (desc->closed == true) {
		spdk_spin_unlock(&desc->spinlock);
		spdk_bdev_for_each_channel_continue(i, -1);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);

	now = spdk_get_ticks();
	TAILQ_FOREACH(bdev_io, &bdev_ch->io_submitted, internal.ch_link) {
		/* Exclude any I/O that are generated via splitting. */
		if (bdev_io->internal.cb == bdev_io_split_done) {
			continue;
		}

		/* Once we find an I/O that has not timed out, we can immediately
		 * exit the loop.
		 */
		if (now < (bdev_io->internal.submit_tsc +
			   ctx->timeout_in_sec * spdk_get_ticks_hz())) {
			goto end;
		}

		if (bdev_io->internal.desc == desc) {
			ctx->cb_fn(ctx->cb_arg, bdev_io);
		}
	}

end:
	spdk_bdev_for_each_channel_continue(i, 0);
}

static int
bdev_poll_timeout_io(void *arg)
{
	struct spdk_bdev_desc *desc = arg;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct poll_timeout_ctx *ctx;

	ctx = calloc(1, sizeof(struct poll_timeout_ctx));
	if (!ctx) {
		SPDK_ERRLOG("failed to allocate memory\n");
		return SPDK_POLLER_BUSY;
	}
	ctx->desc = desc;
	ctx->cb_arg = desc->cb_arg;
	ctx->cb_fn = desc->cb_fn;
	ctx->timeout_in_sec = desc->timeout_in_sec;

	/* Take a ref on the descriptor in case it gets closed while we are checking
	 * all of the channels.
	 */
	spdk_spin_lock(&desc->spinlock);
	desc->refs++;
	spdk_spin_unlock(&desc->spinlock);

	spdk_bdev_for_each_channel(bdev, bdev_channel_poll_timeout_io, ctx,
				   bdev_channel_poll_timeout_io_done);

	return SPDK_POLLER_BUSY;
}

int
spdk_bdev_set_timeout(struct spdk_bdev_desc *desc, uint64_t timeout_in_sec,
		      spdk_bdev_io_timeout_cb cb_fn, void *cb_arg)
{
	assert(desc->thread == spdk_get_thread());

	spdk_poller_unregister(&desc->io_timeout_poller);

	if (timeout_in_sec) {
		assert(cb_fn != NULL);
		desc->io_timeout_poller = SPDK_POLLER_REGISTER(bdev_poll_timeout_io,
					  desc,
					  SPDK_BDEV_IO_POLL_INTERVAL_IN_MSEC * SPDK_SEC_TO_USEC /
					  1000);
		if (desc->io_timeout_poller == NULL) {
			SPDK_ERRLOG("can not register the desc timeout IO poller\n");
			return -1;
		}
	}

	desc->cb_fn = cb_fn;
	desc->cb_arg = cb_arg;
	desc->timeout_in_sec = timeout_in_sec;

	return 0;
}

static int
bdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev		*bdev = __bdev_from_io_dev(io_device);
	struct spdk_bdev_channel	*ch = ctx_buf;
	struct spdk_io_channel		*mgmt_io_ch;
	struct spdk_bdev_mgmt_channel	*mgmt_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct lba_range		*range;

	ch->bdev = bdev;
	ch->channel = bdev->fn_table->get_io_channel(bdev->ctxt);
	if (!ch->channel) {
		return -1;
	}

	spdk_trace_record(TRACE_BDEV_IOCH_CREATE, 0, 0, 0, ch->bdev->name,
			  spdk_thread_get_id(spdk_io_channel_get_thread(ch->channel)));

	assert(ch->histogram == NULL);
	if (bdev->internal.histogram_enabled) {
		ch->histogram = spdk_histogram_data_alloc();
		if (ch->histogram == NULL) {
			SPDK_ERRLOG("Could not allocate histogram\n");
		}
	}

	mgmt_io_ch = spdk_get_io_channel(&g_bdev_mgr);
	if (!mgmt_io_ch) {
		spdk_put_io_channel(ch->channel);
		return -1;
	}

	mgmt_ch = __io_ch_to_bdev_mgmt_ch(mgmt_io_ch);
	TAILQ_FOREACH(shared_resource, &mgmt_ch->shared_resources, link) {
		if (shared_resource->shared_ch == ch->channel) {
			spdk_put_io_channel(mgmt_io_ch);
			shared_resource->ref++;
			break;
		}
	}

	if (shared_resource == NULL) {
		shared_resource = calloc(1, sizeof(*shared_resource));
		if (shared_resource == NULL) {
			spdk_put_io_channel(ch->channel);
			spdk_put_io_channel(mgmt_io_ch);
			return -1;
		}

		shared_resource->mgmt_ch = mgmt_ch;
		shared_resource->io_outstanding = 0;
		TAILQ_INIT(&shared_resource->nomem_io);
		shared_resource->nomem_threshold = 0;
		shared_resource->shared_ch = ch->channel;
		shared_resource->ref = 1;
		TAILQ_INSERT_TAIL(&mgmt_ch->shared_resources, shared_resource, link);
	}

	ch->io_outstanding = 0;
	TAILQ_INIT(&ch->queued_resets);
	TAILQ_INIT(&ch->locked_ranges);
	ch->flags = 0;
	ch->shared_resource = shared_resource;

	TAILQ_INIT(&ch->io_submitted);
	TAILQ_INIT(&ch->io_locked);

	ch->stat = bdev_alloc_io_stat(false);
	if (ch->stat == NULL) {
		bdev_channel_destroy_resource(ch);
		return -1;
	}

	ch->stat->ticks_rate = spdk_get_ticks_hz();

#ifdef SPDK_CONFIG_VTUNE
	{
		char *name;
		__itt_init_ittlib(NULL, 0);
		name = spdk_sprintf_alloc("spdk_bdev_%s_%p", ch->bdev->name, ch);
		if (!name) {
			bdev_channel_destroy_resource(ch);
			return -1;
		}
		ch->handle = __itt_string_handle_create(name);
		free(name);
		ch->start_tsc = spdk_get_ticks();
		ch->interval_tsc = spdk_get_ticks_hz() / 100;
		ch->prev_stat = bdev_alloc_io_stat(false);
		if (ch->prev_stat == NULL) {
			bdev_channel_destroy_resource(ch);
			return -1;
		}
	}
#endif

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_enable_qos(bdev, ch);

	TAILQ_FOREACH(range, &bdev->internal.locked_ranges, tailq) {
		struct lba_range *new_range;

		new_range = calloc(1, sizeof(*new_range));
		if (new_range == NULL) {
			spdk_spin_unlock(&bdev->internal.spinlock);
			bdev_channel_destroy_resource(ch);
			return -1;
		}
		new_range->length = range->length;
		new_range->offset = range->offset;
		new_range->locked_ctx = range->locked_ctx;
		TAILQ_INSERT_TAIL(&ch->locked_ranges, new_range, tailq);
	}

	spdk_spin_unlock(&bdev->internal.spinlock);

	return 0;
}

static int
bdev_abort_all_buf_io_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
			 void *cb_ctx)
{
	struct spdk_bdev_channel *bdev_ch = cb_ctx;
	struct spdk_bdev_io *bdev_io;
	uint64_t buf_len;

	bdev_io = SPDK_CONTAINEROF(entry, struct spdk_bdev_io, internal.iobuf);
	if (bdev_io->internal.ch == bdev_ch) {
		buf_len = bdev_io_get_max_buf_len(bdev_io, bdev_io->internal.buf_len);
		spdk_iobuf_entry_abort(ch, entry, buf_len);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
	}

	return 0;
}

/*
 * Abort I/O that are waiting on a data buffer.
 */
static void
bdev_abort_all_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, struct spdk_bdev_channel *ch)
{
	spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, &mgmt_ch->iobuf.small,
				  bdev_abort_all_buf_io_cb, ch);
	spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, &mgmt_ch->iobuf.large,
				  bdev_abort_all_buf_io_cb, ch);
}

/*
 * Abort I/O that are queued waiting for submission.  These types of I/O are
 *  linked using the spdk_bdev_io link TAILQ_ENTRY.
 */
static void
bdev_abort_all_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	TAILQ_FOREACH_SAFE(bdev_io, queue, internal.link, tmp) {
		if (bdev_io->internal.ch == ch) {
			TAILQ_REMOVE(queue, bdev_io, internal.link);
			/*
			 * spdk_bdev_io_complete() assumes that the completed I/O had
			 *  been submitted to the bdev module.  Since in this case it
			 *  hadn't, bump io_outstanding to account for the decrement
			 *  that spdk_bdev_io_complete() will do.
			 */
			if (bdev_io->type != SPDK_BDEV_IO_TYPE_RESET) {
				ch->io_outstanding++;
				ch->shared_resource->io_outstanding++;
			}
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
		}
	}
}

static bool
bdev_abort_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io;

	TAILQ_FOREACH(bdev_io, queue, internal.link) {
		if (bdev_io == bio_to_abort) {
			TAILQ_REMOVE(queue, bio_to_abort, internal.link);
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			return true;
		}
	}

	return false;
}

static int
bdev_abort_buf_io_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry, void *cb_ctx)
{
	struct spdk_bdev_io *bdev_io, *bio_to_abort = cb_ctx;
	uint64_t buf_len;

	bdev_io = SPDK_CONTAINEROF(entry, struct spdk_bdev_io, internal.iobuf);
	if (bdev_io == bio_to_abort) {
		buf_len = bdev_io_get_max_buf_len(bdev_io, bdev_io->internal.buf_len);
		spdk_iobuf_entry_abort(ch, entry, buf_len);
		spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
		return 1;
	}

	return 0;
}

static bool
bdev_abort_buf_io(struct spdk_bdev_mgmt_channel *mgmt_ch, struct spdk_bdev_io *bio_to_abort)
{
	int rc;

	rc = spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, &mgmt_ch->iobuf.small,
				       bdev_abort_buf_io_cb, bio_to_abort);
	if (rc == 1) {
		return true;
	}

	rc = spdk_iobuf_for_each_entry(&mgmt_ch->iobuf, &mgmt_ch->iobuf.large,
				       bdev_abort_buf_io_cb, bio_to_abort);
	return rc == 1;
}

static void
bdev_qos_channel_destroy(void *cb_arg)
{
	struct spdk_bdev_qos *qos = cb_arg;

	spdk_put_io_channel(spdk_io_channel_from_ctx(qos->ch));
	spdk_poller_unregister(&qos->poller);

	SPDK_DEBUGLOG(bdev, "Free QoS %p.\n", qos);

	free(qos);
}

static int
bdev_qos_destroy(struct spdk_bdev *bdev)
{
	int i;

	/*
	 * Cleanly shutting down the QoS poller is tricky, because
	 * during the asynchronous operation the user could open
	 * a new descriptor and create a new channel, spawning
	 * a new QoS poller.
	 *
	 * The strategy is to create a new QoS structure here and swap it
	 * in. The shutdown path then continues to refer to the old one
	 * until it completes and then releases it.
	 */
	struct spdk_bdev_qos *new_qos, *old_qos;

	old_qos = bdev->internal.qos;

	new_qos = calloc(1, sizeof(*new_qos));
	if (!new_qos) {
		SPDK_ERRLOG("Unable to allocate memory to shut down QoS.\n");
		return -ENOMEM;
	}

	/* Copy the old QoS data into the newly allocated structure */
	memcpy(new_qos, old_qos, sizeof(*new_qos));

	/* Zero out the key parts of the QoS structure */
	new_qos->ch = NULL;
	new_qos->thread = NULL;
	new_qos->poller = NULL;
	TAILQ_INIT(&new_qos->queued);
	/*
	 * The limit member of spdk_bdev_qos_limit structure is not zeroed.
	 * It will be used later for the new QoS structure.
	 */
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		new_qos->rate_limits[i].remaining_this_timeslice = 0;
		new_qos->rate_limits[i].min_per_timeslice = 0;
		new_qos->rate_limits[i].max_per_timeslice = 0;
	}

	bdev->internal.qos = new_qos;

	if (old_qos->thread == NULL) {
		free(old_qos);
	} else {
		spdk_thread_send_msg(old_qos->thread, bdev_qos_channel_destroy, old_qos);
	}

	/* It is safe to continue with destroying the bdev even though the QoS channel hasn't
	 * been destroyed yet. The destruction path will end up waiting for the final
	 * channel to be put before it releases resources. */

	return 0;
}

void
spdk_bdev_add_io_stat(struct spdk_bdev_io_stat *total, struct spdk_bdev_io_stat *add)
{
	total->bytes_read += add->bytes_read;
	total->num_read_ops += add->num_read_ops;
	total->bytes_written += add->bytes_written;
	total->num_write_ops += add->num_write_ops;
	total->bytes_unmapped += add->bytes_unmapped;
	total->num_unmap_ops += add->num_unmap_ops;
	total->bytes_copied += add->bytes_copied;
	total->num_copy_ops += add->num_copy_ops;
	total->read_latency_ticks += add->read_latency_ticks;
	total->write_latency_ticks += add->write_latency_ticks;
	total->unmap_latency_ticks += add->unmap_latency_ticks;
	total->copy_latency_ticks += add->copy_latency_ticks;
	if (total->max_read_latency_ticks < add->max_read_latency_ticks) {
		total->max_read_latency_ticks = add->max_read_latency_ticks;
	}
	if (total->min_read_latency_ticks > add->min_read_latency_ticks) {
		total->min_read_latency_ticks = add->min_read_latency_ticks;
	}
	if (total->max_write_latency_ticks < add->max_write_latency_ticks) {
		total->max_write_latency_ticks = add->max_write_latency_ticks;
	}
	if (total->min_write_latency_ticks > add->min_write_latency_ticks) {
		total->min_write_latency_ticks = add->min_write_latency_ticks;
	}
	if (total->max_unmap_latency_ticks < add->max_unmap_latency_ticks) {
		total->max_unmap_latency_ticks = add->max_unmap_latency_ticks;
	}
	if (total->min_unmap_latency_ticks > add->min_unmap_latency_ticks) {
		total->min_unmap_latency_ticks = add->min_unmap_latency_ticks;
	}
	if (total->max_copy_latency_ticks < add->max_copy_latency_ticks) {
		total->max_copy_latency_ticks = add->max_copy_latency_ticks;
	}
	if (total->min_copy_latency_ticks > add->min_copy_latency_ticks) {
		total->min_copy_latency_ticks = add->min_copy_latency_ticks;
	}
}

static void
bdev_get_io_stat(struct spdk_bdev_io_stat *to_stat, struct spdk_bdev_io_stat *from_stat)
{
	memcpy(to_stat, from_stat, offsetof(struct spdk_bdev_io_stat, io_error));

	if (to_stat->io_error != NULL && from_stat->io_error != NULL) {
		memcpy(to_stat->io_error, from_stat->io_error,
		       sizeof(struct spdk_bdev_io_error_stat));
	}
}

void
spdk_bdev_reset_io_stat(struct spdk_bdev_io_stat *stat, enum spdk_bdev_reset_stat_mode mode)
{
	stat->max_read_latency_ticks = 0;
	stat->min_read_latency_ticks = UINT64_MAX;
	stat->max_write_latency_ticks = 0;
	stat->min_write_latency_ticks = UINT64_MAX;
	stat->max_unmap_latency_ticks = 0;
	stat->min_unmap_latency_ticks = UINT64_MAX;
	stat->max_copy_latency_ticks = 0;
	stat->min_copy_latency_ticks = UINT64_MAX;

	if (mode != SPDK_BDEV_RESET_STAT_ALL) {
		return;
	}

	stat->bytes_read = 0;
	stat->num_read_ops = 0;
	stat->bytes_written = 0;
	stat->num_write_ops = 0;
	stat->bytes_unmapped = 0;
	stat->num_unmap_ops = 0;
	stat->bytes_copied = 0;
	stat->num_copy_ops = 0;
	stat->read_latency_ticks = 0;
	stat->write_latency_ticks = 0;
	stat->unmap_latency_ticks = 0;
	stat->copy_latency_ticks = 0;

	if (stat->io_error != NULL) {
		memset(stat->io_error, 0, sizeof(struct spdk_bdev_io_error_stat));
	}
}

struct spdk_bdev_io_stat *
bdev_alloc_io_stat(bool io_error_stat)
{
	struct spdk_bdev_io_stat *stat;

	stat = malloc(sizeof(struct spdk_bdev_io_stat));
	if (stat == NULL) {
		return NULL;
	}

	if (io_error_stat) {
		stat->io_error = malloc(sizeof(struct spdk_bdev_io_error_stat));
		if (stat->io_error == NULL) {
			free(stat);
			return NULL;
		}
	} else {
		stat->io_error = NULL;
	}

	spdk_bdev_reset_io_stat(stat, SPDK_BDEV_RESET_STAT_ALL);

	return stat;
}

void
bdev_free_io_stat(struct spdk_bdev_io_stat *stat)
{
	if (stat != NULL) {
		free(stat->io_error);
		free(stat);
	}
}

void
spdk_bdev_dump_io_stat_json(struct spdk_bdev_io_stat *stat, struct spdk_json_write_ctx *w)
{
	int i;

	spdk_json_write_named_uint64(w, "bytes_read", stat->bytes_read);
	spdk_json_write_named_uint64(w, "num_read_ops", stat->num_read_ops);
	spdk_json_write_named_uint64(w, "bytes_written", stat->bytes_written);
	spdk_json_write_named_uint64(w, "num_write_ops", stat->num_write_ops);
	spdk_json_write_named_uint64(w, "bytes_unmapped", stat->bytes_unmapped);
	spdk_json_write_named_uint64(w, "num_unmap_ops", stat->num_unmap_ops);
	spdk_json_write_named_uint64(w, "bytes_copied", stat->bytes_copied);
	spdk_json_write_named_uint64(w, "num_copy_ops", stat->num_copy_ops);
	spdk_json_write_named_uint64(w, "read_latency_ticks", stat->read_latency_ticks);
	spdk_json_write_named_uint64(w, "max_read_latency_ticks", stat->max_read_latency_ticks);
	spdk_json_write_named_uint64(w, "min_read_latency_ticks",
				     stat->min_read_latency_ticks != UINT64_MAX ?
				     stat->min_read_latency_ticks : 0);
	spdk_json_write_named_uint64(w, "write_latency_ticks", stat->write_latency_ticks);
	spdk_json_write_named_uint64(w, "max_write_latency_ticks", stat->max_write_latency_ticks);
	spdk_json_write_named_uint64(w, "min_write_latency_ticks",
				     stat->min_write_latency_ticks != UINT64_MAX ?
				     stat->min_write_latency_ticks : 0);
	spdk_json_write_named_uint64(w, "unmap_latency_ticks", stat->unmap_latency_ticks);
	spdk_json_write_named_uint64(w, "max_unmap_latency_ticks", stat->max_unmap_latency_ticks);
	spdk_json_write_named_uint64(w, "min_unmap_latency_ticks",
				     stat->min_unmap_latency_ticks != UINT64_MAX ?
				     stat->min_unmap_latency_ticks : 0);
	spdk_json_write_named_uint64(w, "copy_latency_ticks", stat->copy_latency_ticks);
	spdk_json_write_named_uint64(w, "max_copy_latency_ticks", stat->max_copy_latency_ticks);
	spdk_json_write_named_uint64(w, "min_copy_latency_ticks",
				     stat->min_copy_latency_ticks != UINT64_MAX ?
				     stat->min_copy_latency_ticks : 0);

	if (stat->io_error != NULL) {
		spdk_json_write_named_object_begin(w, "io_error");
		for (i = 0; i < -SPDK_MIN_BDEV_IO_STATUS; i++) {
			if (stat->io_error->error_status[i] != 0) {
				spdk_json_write_named_uint32(w, bdev_io_status_get_string(-(i + 1)),
							     stat->io_error->error_status[i]);
			}
		}
		spdk_json_write_object_end(w);
	}
}

static void
bdev_channel_abort_queued_ios(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_shared_resource *shared_resource = ch->shared_resource;
	struct spdk_bdev_mgmt_channel *mgmt_ch = shared_resource->mgmt_ch;

	bdev_abort_all_queued_io(&shared_resource->nomem_io, ch);
	bdev_abort_all_buf_io(mgmt_ch, ch);
	bdev_abort_all_buf_io(mgmt_ch, ch);
}

static void
bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel *ch = ctx_buf;

	SPDK_DEBUGLOG(bdev, "Destroying channel %p for bdev %s on thread %p\n", ch, ch->bdev->name,
		      spdk_get_thread());

	spdk_trace_record(TRACE_BDEV_IOCH_DESTROY, 0, 0, 0, ch->bdev->name,
			  spdk_thread_get_id(spdk_io_channel_get_thread(ch->channel)));

	/* This channel is going away, so add its statistics into the bdev so that they don't get lost. */
	spdk_spin_lock(&ch->bdev->internal.spinlock);
	spdk_bdev_add_io_stat(ch->bdev->internal.stat, ch->stat);
	spdk_spin_unlock(&ch->bdev->internal.spinlock);

	bdev_abort_all_queued_io(&ch->queued_resets, ch);

	bdev_channel_abort_queued_ios(ch);

	if (ch->histogram) {
		spdk_histogram_data_free(ch->histogram);
	}

	bdev_channel_destroy_resource(ch);
}

/*
 * If the name already exists in the global bdev name tree, RB_INSERT() returns a pointer
 * to it. Hence we do not have to call bdev_get_by_name() when using this function.
 */
static int
bdev_name_add(struct spdk_bdev_name *bdev_name, struct spdk_bdev *bdev, const char *name)
{
	struct spdk_bdev_name *tmp;

	bdev_name->name = strdup(name);
	if (bdev_name->name == NULL) {
		SPDK_ERRLOG("Unable to allocate bdev name\n");
		return -ENOMEM;
	}

	bdev_name->bdev = bdev;

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	tmp = RB_INSERT(bdev_name_tree, &g_bdev_mgr.bdev_names, bdev_name);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	if (tmp != NULL) {
		SPDK_ERRLOG("Bdev name %s already exists\n", name);
		free(bdev_name->name);
		return -EEXIST;
	}

	return 0;
}

static void
bdev_name_del_unsafe(struct spdk_bdev_name *bdev_name)
{
	RB_REMOVE(bdev_name_tree, &g_bdev_mgr.bdev_names, bdev_name);
	free(bdev_name->name);
}

static void
bdev_name_del(struct spdk_bdev_name *bdev_name)
{
	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev_name_del_unsafe(bdev_name);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

int
spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;
	int ret;

	if (alias == NULL) {
		SPDK_ERRLOG("Empty alias passed\n");
		return -EINVAL;
	}

	tmp = calloc(1, sizeof(*tmp));
	if (tmp == NULL) {
		SPDK_ERRLOG("Unable to allocate alias\n");
		return -ENOMEM;
	}

	ret = bdev_name_add(&tmp->alias, bdev, alias);
	if (ret != 0) {
		free(tmp);
		return ret;
	}

	TAILQ_INSERT_TAIL(&bdev->aliases, tmp, tailq);

	return 0;
}

static int
bdev_alias_del(struct spdk_bdev *bdev, const char *alias,
	       void (*alias_del_fn)(struct spdk_bdev_name *n))
{
	struct spdk_bdev_alias *tmp;

	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		if (strcmp(alias, tmp->alias.name) == 0) {
			TAILQ_REMOVE(&bdev->aliases, tmp, tailq);
			alias_del_fn(&tmp->alias);
			free(tmp);
			return 0;
		}
	}

	return -ENOENT;
}

int
spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias)
{
	int rc;

	rc = bdev_alias_del(bdev, alias, bdev_name_del);
	if (rc == -ENOENT) {
		SPDK_INFOLOG(bdev, "Alias %s does not exist\n", alias);
	}

	return rc;
}

void
spdk_bdev_alias_del_all(struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &bdev->aliases, tailq, tmp) {
		TAILQ_REMOVE(&bdev->aliases, p, tailq);
		bdev_name_del(&p->alias);
		free(p);
	}
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return spdk_get_io_channel(__bdev_to_io_dev(spdk_bdev_desc_get_bdev(desc)));
}

void *
spdk_bdev_get_module_ctx(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	void *ctx = NULL;

	if (bdev->fn_table->get_module_ctx) {
		ctx = bdev->fn_table->get_module_ctx(bdev->ctxt);
	}

	return ctx;
}

const char *
spdk_bdev_get_module_name(const struct spdk_bdev *bdev)
{
	return bdev->module->name;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

const char *
spdk_bdev_get_product_name(const struct spdk_bdev *bdev)
{
	return bdev->product_name;
}

const struct spdk_bdev_aliases_list *
spdk_bdev_get_aliases(const struct spdk_bdev *bdev)
{
	return &bdev->aliases;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

uint32_t
spdk_bdev_get_write_unit_size(const struct spdk_bdev *bdev)
{
	return bdev->write_unit_size;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

const char *
spdk_bdev_get_qos_rpc_type(enum spdk_bdev_qos_rate_limit_type type)
{
	return qos_rpc_type[type];
}

void
spdk_bdev_get_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits)
{
	int i;

	memset(limits, 0, sizeof(*limits) * SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES);

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.qos) {
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (bdev->internal.qos->rate_limits[i].limit !=
			    SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
				limits[i] = bdev->internal.qos->rate_limits[i].limit;
				if (bdev_qos_is_iops_rate_limit(i) == false) {
					/* Change from Byte to Megabyte which is user visible. */
					limits[i] = limits[i] / 1024 / 1024;
				}
			}
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
}

size_t
spdk_bdev_get_buf_align(const struct spdk_bdev *bdev)
{
	return 1 << bdev->required_alignment;
}

uint32_t
spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev)
{
	return bdev->optimal_io_boundary;
}

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return bdev->write_cache;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

uint16_t
spdk_bdev_get_acwu(const struct spdk_bdev *bdev)
{
	return bdev->acwu;
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

bool
spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && bdev->md_interleave;
}

bool
spdk_bdev_is_md_separate(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && !bdev->md_interleave;
}

bool
spdk_bdev_is_zoned(const struct spdk_bdev *bdev)
{
	return bdev->zoned;
}

uint32_t
spdk_bdev_get_data_block_size(const struct spdk_bdev *bdev)
{
	if (spdk_bdev_is_md_interleaved(bdev)) {
		return bdev->blocklen - bdev->md_len;
	} else {
		return bdev->blocklen;
	}
}

uint32_t
spdk_bdev_get_physical_block_size(const struct spdk_bdev *bdev)
{
	return bdev->phys_blocklen;
}

static uint32_t
_bdev_get_block_size_with_md(const struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_interleaved(bdev)) {
		return bdev->blocklen + bdev->md_len;
	} else {
		return bdev->blocklen;
	}
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_dif_type spdk_dif_type_t;

spdk_dif_type_t
spdk_bdev_get_dif_type(const struct spdk_bdev *bdev)
{
	if (bdev->md_len != 0) {
		return bdev->dif_type;
	} else {
		return SPDK_DIF_DISABLE;
	}
}

bool
spdk_bdev_is_dif_head_of_md(const struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		return bdev->dif_is_head_of_md;
	} else {
		return false;
	}
}

bool
spdk_bdev_is_dif_check_enabled(const struct spdk_bdev *bdev,
			       enum spdk_dif_check_type check_type)
{
	if (spdk_bdev_get_dif_type(bdev) == SPDK_DIF_DISABLE) {
		return false;
	}

	switch (check_type) {
	case SPDK_DIF_CHECK_TYPE_REFTAG:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) != 0;
	case SPDK_DIF_CHECK_TYPE_APPTAG:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) != 0;
	case SPDK_DIF_CHECK_TYPE_GUARD:
		return (bdev->dif_check_flags & SPDK_DIF_FLAGS_GUARD_CHECK) != 0;
	default:
		return false;
	}
}

uint32_t
spdk_bdev_get_max_copy(const struct spdk_bdev *bdev)
{
	return bdev->max_copy;
}

uint64_t
spdk_bdev_get_qd(const struct spdk_bdev *bdev)
{
	return bdev->internal.measured_queue_depth;
}

uint64_t
spdk_bdev_get_qd_sampling_period(const struct spdk_bdev *bdev)
{
	return bdev->internal.period;
}

uint64_t
spdk_bdev_get_weighted_io_time(const struct spdk_bdev *bdev)
{
	return bdev->internal.weighted_io_time;
}

uint64_t
spdk_bdev_get_io_time(const struct spdk_bdev *bdev)
{
	return bdev->internal.io_time;
}

static void bdev_update_qd_sampling_period(void *ctx);

static void
_calculate_measured_qd_cpl(struct spdk_bdev *bdev, void *_ctx, int status)
{
	bdev->internal.measured_queue_depth = bdev->internal.temporary_queue_depth;

	if (bdev->internal.measured_queue_depth) {
		bdev->internal.io_time += bdev->internal.period;
		bdev->internal.weighted_io_time += bdev->internal.period * bdev->internal.measured_queue_depth;
	}

	bdev->internal.qd_poll_in_progress = false;

	bdev_update_qd_sampling_period(bdev);
}

static void
_calculate_measured_qd(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		       struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(io_ch);

	bdev->internal.temporary_queue_depth += ch->io_outstanding;
	spdk_bdev_for_each_channel_continue(i, 0);
}

static int
bdev_calculate_measured_queue_depth(void *ctx)
{
	struct spdk_bdev *bdev = ctx;

	bdev->internal.qd_poll_in_progress = true;
	bdev->internal.temporary_queue_depth = 0;
	spdk_bdev_for_each_channel(bdev, _calculate_measured_qd, bdev, _calculate_measured_qd_cpl);
	return SPDK_POLLER_BUSY;
}

static void
bdev_update_qd_sampling_period(void *ctx)
{
	struct spdk_bdev *bdev = ctx;

	if (bdev->internal.period == bdev->internal.new_period) {
		return;
	}

	if (bdev->internal.qd_poll_in_progress) {
		return;
	}

	bdev->internal.period = bdev->internal.new_period;

	spdk_poller_unregister(&bdev->internal.qd_poller);
	if (bdev->internal.period != 0) {
		bdev->internal.qd_poller = SPDK_POLLER_REGISTER(bdev_calculate_measured_queue_depth,
					   bdev, bdev->internal.period);
	} else {
		spdk_bdev_close(bdev->internal.qd_desc);
		bdev->internal.qd_desc = NULL;
	}
}

static void
_tmp_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	SPDK_NOTICELOG("Unexpected event type: %d\n", type);
}

void
spdk_bdev_set_qd_sampling_period(struct spdk_bdev *bdev, uint64_t period)
{
	int rc;

	if (bdev->internal.new_period == period) {
		return;
	}

	bdev->internal.new_period = period;

	if (bdev->internal.qd_desc != NULL) {
		assert(bdev->internal.period != 0);

		spdk_thread_send_msg(bdev->internal.qd_desc->thread,
				     bdev_update_qd_sampling_period, bdev);
		return;
	}

	assert(bdev->internal.period == 0);

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, _tmp_bdev_event_cb,
				NULL, &bdev->internal.qd_desc);
	if (rc != 0) {
		return;
	}

	bdev->internal.period = period;
	bdev->internal.qd_poller = SPDK_POLLER_REGISTER(bdev_calculate_measured_queue_depth,
				   bdev, period);
}

struct bdev_get_current_qd_ctx {
	uint64_t current_qd;
	spdk_bdev_get_current_qd_cb cb_fn;
	void *cb_arg;
};

static void
bdev_get_current_qd_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct bdev_get_current_qd_ctx *ctx = _ctx;

	ctx->cb_fn(bdev, ctx->current_qd, ctx->cb_arg, 0);

	free(ctx);
}

static void
bdev_get_current_qd(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		    struct spdk_io_channel *io_ch, void *_ctx)
{
	struct bdev_get_current_qd_ctx *ctx = _ctx;
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);

	ctx->current_qd += bdev_ch->io_outstanding;

	spdk_bdev_for_each_channel_continue(i, 0);
}

void
spdk_bdev_get_current_qd(struct spdk_bdev *bdev, spdk_bdev_get_current_qd_cb cb_fn,
			 void *cb_arg)
{
	struct bdev_get_current_qd_ctx *ctx;

	assert(cb_fn != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(bdev, 0, cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_bdev_for_each_channel(bdev, bdev_get_current_qd, ctx, bdev_get_current_qd_done);
}

static void
_resize_notify(void *arg)
{
	struct spdk_bdev_desc *desc = arg;

	spdk_spin_lock(&desc->spinlock);
	desc->refs--;
	if (!desc->closed) {
		spdk_spin_unlock(&desc->spinlock);
		desc->callback.event_fn(SPDK_BDEV_EVENT_RESIZE,
					desc->bdev,
					desc->callback.ctx);
		return;
	} else if (0 == desc->refs) {
		/* This descriptor was closed after this resize_notify message was sent.
		 * spdk_bdev_close() could not free the descriptor since this message was
		 * in flight, so we free it now using bdev_desc_free().
		 */
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	struct spdk_bdev_desc *desc;
	int ret;

	if (size == bdev->blockcnt) {
		return 0;
	}

	spdk_spin_lock(&bdev->internal.spinlock);

	/* bdev has open descriptors */
	if (!TAILQ_EMPTY(&bdev->internal.open_descs) &&
	    bdev->blockcnt > size) {
		ret = -EBUSY;
	} else {
		bdev->blockcnt = size;
		TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
			spdk_spin_lock(&desc->spinlock);
			if (!desc->closed) {
				desc->refs++;
				spdk_thread_send_msg(desc->thread, _resize_notify, desc);
			}
			spdk_spin_unlock(&desc->spinlock);
		}
		ret = 0;
	}

	spdk_spin_unlock(&bdev->internal.spinlock);

	return ret;
}

/*
 * Convert I/O offset and length from bytes to blocks.
 *
 * Returns zero on success or non-zero if the byte parameters aren't divisible by the block size.
 */
static uint64_t
bdev_bytes_to_blocks(struct spdk_bdev *bdev, uint64_t offset_bytes, uint64_t *offset_blocks,
		     uint64_t num_bytes, uint64_t *num_blocks)
{
	uint32_t block_size = bdev->blocklen;
	uint8_t shift_cnt;

	/* Avoid expensive div operations if possible. These spdk_u32 functions are very cheap. */
	if (spdk_likely(spdk_u32_is_pow2(block_size))) {
		shift_cnt = spdk_u32log2(block_size);
		*offset_blocks = offset_bytes >> shift_cnt;
		*num_blocks = num_bytes >> shift_cnt;
		return (offset_bytes - (*offset_blocks << shift_cnt)) |
		       (num_bytes - (*num_blocks << shift_cnt));
	} else {
		*offset_blocks = offset_bytes / block_size;
		*num_blocks = num_bytes / block_size;
		return (offset_bytes % block_size) | (num_bytes % block_size);
	}
}

static bool
bdev_io_valid_blocks(struct spdk_bdev *bdev, uint64_t offset_blocks, uint64_t num_blocks)
{
	/* Return failure if offset_blocks + num_blocks is less than offset_blocks; indicates there
	 * has been an overflow and hence the offset has been wrapped around */
	if (offset_blocks + num_blocks < offset_blocks) {
		return false;
	}

	/* Return failure if offset_blocks + num_blocks exceeds the size of the bdev */
	if (offset_blocks + num_blocks > bdev->blockcnt) {
		return false;
	}

	return true;
}

static void
bdev_seek_complete_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx);
}

static int
bdev_seek(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	  uint64_t offset_blocks, enum spdk_bdev_io_type io_type,
	  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	assert(io_type == SPDK_BDEV_IO_TYPE_SEEK_DATA || io_type == SPDK_BDEV_IO_TYPE_SEEK_HOLE);

	/* Check if offset_blocks is valid looking at the validity of one block */
	if (!bdev_io_valid_blocks(bdev, offset_blocks, 1)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = io_type;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	if (!spdk_bdev_io_type_supported(bdev, io_type)) {
		/* In case bdev doesn't support seek to next data/hole offset,
		 * it is assumed that only data and no holes are present */
		if (io_type == SPDK_BDEV_IO_TYPE_SEEK_DATA) {
			bdev_io->u.bdev.seek.offset = offset_blocks;
		} else {
			bdev_io->u.bdev.seek.offset = UINT64_MAX;
		}

		spdk_thread_send_msg(spdk_get_thread(), bdev_seek_complete_cb, bdev_io);
		return 0;
	}

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_seek_data(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset_blocks,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_seek(desc, ch, offset_blocks, SPDK_BDEV_IO_TYPE_SEEK_DATA, cb, cb_arg);
}

int
spdk_bdev_seek_hole(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset_blocks,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_seek(desc, ch, offset_blocks, SPDK_BDEV_IO_TYPE_SEEK_HOLE, cb, cb_arg);
}

uint64_t
spdk_bdev_io_get_seek_offset(const struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.seek.offset;
}

static int
bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
			 void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.ext_opts = NULL;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_read_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(&iov)) {
		return -EINVAL;
	}

	return bdev_read_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					cb, cb_arg);
}

int
spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_readv_blocks(desc, ch, iov, iovcnt, offset_blocks, num_blocks, cb, cb_arg);
}

static int
bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt, void *md_buf, uint64_t offset_blocks,
			  uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			  struct spdk_bdev_ext_io_opts *opts, bool copy_opts)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->internal.ext_opts = opts;
	bdev_io->u.bdev.ext_opts = opts;

	_bdev_io_submit_ext(desc, bdev_io, opts, copy_opts);

	return 0;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					 num_blocks, cb, cb_arg, NULL, false);
}

int
spdk_bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, void *md_buf,
			       uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					 num_blocks, cb, cb_arg, NULL, false);
}

static inline bool
_bdev_io_check_opts(struct spdk_bdev_ext_io_opts *opts, struct iovec *iov)
{
	/*
	 * We check if opts size is at least of size when we first introduced
	 * spdk_bdev_ext_io_opts (ac6f2bdd8d) since access to those members
	 * are not checked internal.
	 */
	return opts->size >= offsetof(struct spdk_bdev_ext_io_opts, metadata) +
	       sizeof(opts->metadata) &&
	       opts->size <= sizeof(*opts) &&
	       /* When memory domain is used, the user must provide data buffers */
	       (!opts->memory_domain || (iov && iov[0].iov_base));
}

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	void *md = NULL;

	if (opts) {
		if (spdk_unlikely(!_bdev_io_check_opts(opts, iov))) {
			return -EINVAL;
		}
		md = opts->metadata;
	}

	if (md && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, md, offset_blocks,
					 num_blocks, cb, cb_arg, opts, false);
}

static int
bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.ext_opts = NULL;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_write_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks,
					 cb, cb_arg);
}

int
spdk_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(&iov)) {
		return -EINVAL;
	}

	return bdev_write_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					 cb, cb_arg);
}

static int
bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, void *md_buf,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts, bool copy_opts)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->internal.ext_opts = opts;
	bdev_io->u.bdev.ext_opts = opts;

	_bdev_io_submit_ext(desc, bdev_io, opts, copy_opts);

	return 0;
}

int
spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 len, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_writev_blocks(desc, ch, iov, iovcnt, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					  num_blocks, cb, cb_arg, NULL, false);
}

int
spdk_bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, void *md_buf,
				uint64_t offset_blocks, uint64_t num_blocks,
				spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					  num_blocks, cb, cb_arg, NULL, false);
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	void *md = NULL;

	if (opts) {
		if (spdk_unlikely(!_bdev_io_check_opts(opts, iov))) {
			return -EINVAL;
		}
		md = opts->metadata;
	}

	if (md && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, md, offset_blocks,
					  num_blocks, cb, cb_arg, opts, false);
}

static void
bdev_compare_do_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	struct spdk_bdev *bdev = parent_io->bdev;
	uint8_t *read_buf = bdev_io->u.bdev.iovs[0].iov_base;
	int i, rc = 0;

	if (!success) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
		spdk_bdev_free_io(bdev_io);
		return;
	}

	for (i = 0; i < parent_io->u.bdev.iovcnt; i++) {
		rc = memcmp(read_buf,
			    parent_io->u.bdev.iovs[i].iov_base,
			    parent_io->u.bdev.iovs[i].iov_len);
		if (rc) {
			break;
		}
		read_buf += parent_io->u.bdev.iovs[i].iov_len;
	}

	if (rc == 0 && parent_io->u.bdev.md_buf && spdk_bdev_is_md_separate(bdev)) {
		rc = memcmp(bdev_io->u.bdev.md_buf,
			    parent_io->u.bdev.md_buf,
			    spdk_bdev_get_md_size(bdev));
	}

	spdk_bdev_free_io(bdev_io);

	if (rc == 0) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
		parent_io->internal.cb(parent_io, true, parent_io->internal.caller_ctx);
	} else {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_MISCOMPARE;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
	}
}

static void
bdev_compare_do_read(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	int rc;

	rc = spdk_bdev_read_blocks(bdev_io->internal.desc,
				   spdk_io_channel_from_ctx(bdev_io->internal.ch), NULL,
				   bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				   bdev_compare_do_read_done, bdev_io);

	if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_do_read);
	} else if (rc != 0) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

static int
bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			     struct iovec *iov, int iovcnt, void *md_buf,
			     uint64_t offset_blocks, uint64_t num_blocks,
			     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	bdev_compare_do_read(bdev_io);

	return 0;
}

int
spdk_bdev_comparev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt,
			  uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_comparev_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks,
					    num_blocks, cb, cb_arg);
}

int
spdk_bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  struct iovec *iov, int iovcnt, void *md_buf,
				  uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(iov)) {
		return -EINVAL;
	}

	return bdev_comparev_blocks_with_md(desc, ch, iov, iovcnt, md_buf, offset_blocks,
					    num_blocks, cb, cb_arg);
}

static int
bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	bdev_compare_do_read(bdev_io);

	return 0;
}

int
spdk_bdev_compare_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			 void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_compare_blocks_with_md(desc, ch, buf, NULL, offset_blocks, num_blocks,
					   cb, cb_arg);
}

int
spdk_bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				 void *buf, void *md_buf, uint64_t offset_blocks, uint64_t num_blocks,
				 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct iovec iov = {
		.iov_base = buf,
	};

	if (md_buf && !spdk_bdev_is_md_separate(spdk_bdev_desc_get_bdev(desc))) {
		return -EINVAL;
	}

	if (md_buf && !_is_buf_allocated(&iov)) {
		return -EINVAL;
	}

	return bdev_compare_blocks_with_md(desc, ch, buf, md_buf, offset_blocks, num_blocks,
					   cb, cb_arg);
}

static void
bdev_comparev_and_writev_blocks_unlocked(void *ctx, int unlock_status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (unlock_status) {
		SPDK_ERRLOG("LBA range unlock failed\n");
	}

	bdev_io->internal.cb(bdev_io, bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS ? true :
			     false, bdev_io->internal.caller_ctx);
}

static void
bdev_comparev_and_writev_blocks_unlock(struct spdk_bdev_io *bdev_io, int status)
{
	bdev_io->internal.status = status;

	bdev_unlock_lba_range(bdev_io->internal.desc, spdk_io_channel_from_ctx(bdev_io->internal.ch),
			      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			      bdev_comparev_and_writev_blocks_unlocked, bdev_io);
}

static void
bdev_compare_and_write_do_write_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	if (!success) {
		SPDK_ERRLOG("Compare and write operation failed\n");
	}

	spdk_bdev_free_io(bdev_io);

	bdev_comparev_and_writev_blocks_unlock(parent_io,
					       success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
bdev_compare_and_write_do_write(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	int rc;

	rc = spdk_bdev_writev_blocks(bdev_io->internal.desc,
				     spdk_io_channel_from_ctx(bdev_io->internal.ch),
				     bdev_io->u.bdev.fused_iovs, bdev_io->u.bdev.fused_iovcnt,
				     bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				     bdev_compare_and_write_do_write_done, bdev_io);


	if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_and_write_do_write);
	} else if (rc != 0) {
		bdev_comparev_and_writev_blocks_unlock(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_compare_and_write_do_compare_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		bdev_comparev_and_writev_blocks_unlock(parent_io, SPDK_BDEV_IO_STATUS_MISCOMPARE);
		return;
	}

	bdev_compare_and_write_do_write(parent_io);
}

static void
bdev_compare_and_write_do_compare(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	int rc;

	rc = spdk_bdev_comparev_blocks(bdev_io->internal.desc,
				       spdk_io_channel_from_ctx(bdev_io->internal.ch), bdev_io->u.bdev.iovs,
				       bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
				       bdev_compare_and_write_do_compare_done, bdev_io);

	if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_compare_and_write_do_compare);
	} else if (rc != 0) {
		bdev_comparev_and_writev_blocks_unlock(bdev_io, SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED);
	}
}

static void
bdev_comparev_and_writev_blocks_locked(void *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx;

	if (status) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
		return;
	}

	bdev_compare_and_write_do_compare(bdev_io);
}

int
spdk_bdev_comparev_and_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     struct iovec *compare_iov, int compare_iovcnt,
				     struct iovec *write_iov, int write_iovcnt,
				     uint64_t offset_blocks, uint64_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	if (num_blocks > bdev->acwu) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE;
	bdev_io->u.bdev.iovs = compare_iov;
	bdev_io->u.bdev.iovcnt = compare_iovcnt;
	bdev_io->u.bdev.fused_iovs = write_iov;
	bdev_io->u.bdev.fused_iovcnt = write_iovcnt;
	bdev_io->u.bdev.md_buf = NULL;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	return bdev_lock_lba_range(desc, ch, offset_blocks, num_blocks,
				   bdev_comparev_and_writev_blocks_locked, bdev_io);
}

int
spdk_bdev_zcopy_start(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      struct iovec *iov, int iovcnt,
		      uint64_t offset_blocks, uint64_t num_blocks,
		      bool populate,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY)) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZCOPY;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = NULL;
	bdev_io->u.bdev.zcopy.populate = populate ? 1 : 0;
	bdev_io->u.bdev.zcopy.commit = 0;
	bdev_io->u.bdev.zcopy.start = 1;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	bdev_io_submit(bdev_io);

	return 0;
}

int
spdk_bdev_zcopy_end(struct spdk_bdev_io *bdev_io, bool commit,
		    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (bdev_io->type != SPDK_BDEV_IO_TYPE_ZCOPY) {
		return -EINVAL;
	}

	bdev_io->u.bdev.zcopy.commit = commit ? 1 : 0;
	bdev_io->u.bdev.zcopy.start = 0;
	bdev_io->internal.caller_ctx = cb_arg;
	bdev_io->internal.cb = cb;
	bdev_io->internal.status = SPDK_BDEV_IO_STATUS_PENDING;

	bdev_io_submit(bdev_io);

	return 0;
}

int
spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset, uint64_t len,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 len, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_write_zeroes_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	if (!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES) &&
	    !bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE)) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);

	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	if (bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		bdev_io_submit(bdev_io);
		return 0;
	}

	assert(bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE));
	assert(_bdev_get_block_size_with_md(bdev) <= ZERO_BUFFER_SIZE);
	bdev_io->u.bdev.split_remaining_num_blocks = num_blocks;
	bdev_io->u.bdev.split_current_offset_blocks = offset_blocks;
	bdev_write_zero_buffer_next(bdev_io);

	return 0;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_unmap_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Can't unmap 0 bytes\n");
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;

	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = NULL;
	bdev_io->u.bdev.iovs[0].iov_len = 0;
	bdev_io->u.bdev.iovcnt = 1;

	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);
	bdev_io->u.bdev.ext_opts = NULL;

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (bdev_bytes_to_blocks(spdk_bdev_desc_get_bdev(desc), offset, &offset_blocks,
				 length, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_flush_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	bdev_io->u.bdev.iovs = NULL;
	bdev_io->u.bdev.iovcnt = 0;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

static int bdev_reset_poll_for_outstanding_io(void *ctx);

static void
bdev_reset_check_outstanding_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_channel *ch = _ctx;
	struct spdk_bdev_io *bdev_io;

	bdev_io = TAILQ_FIRST(&ch->queued_resets);

	if (status == -EBUSY) {
		if (spdk_get_ticks() < bdev_io->u.reset.wait_poller.stop_time_tsc) {
			bdev_io->u.reset.wait_poller.poller = SPDK_POLLER_REGISTER(bdev_reset_poll_for_outstanding_io,
							      ch, BDEV_RESET_CHECK_OUTSTANDING_IO_PERIOD);
		} else {
			/* If outstanding IOs are still present and reset_io_drain_timeout seconds passed,
			 * start the reset. */
			TAILQ_REMOVE(&ch->queued_resets, bdev_io, internal.link);
			bdev_io_submit_reset(bdev_io);
		}
	} else {
		TAILQ_REMOVE(&ch->queued_resets, bdev_io, internal.link);
		SPDK_DEBUGLOG(bdev,
			      "Skipping reset for underlying device of bdev: %s - no outstanding I/O.\n",
			      ch->bdev->name);
		/* Mark the completion status as a SUCCESS and complete the reset. */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
}

static void
bdev_reset_check_outstanding_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *cur_ch = __io_ch_to_bdev_ch(io_ch);
	int status = 0;

	if (cur_ch->io_outstanding > 0) {
		/* If a channel has outstanding IO, set status to -EBUSY code. This will stop
		 * further iteration over the rest of the channels and pass non-zero status
		 * to the callback function. */
		status = -EBUSY;
	}
	spdk_bdev_for_each_channel_continue(i, status);
}

static int
bdev_reset_poll_for_outstanding_io(void *ctx)
{
	struct spdk_bdev_channel *ch = ctx;
	struct spdk_bdev_io *bdev_io;

	bdev_io = TAILQ_FIRST(&ch->queued_resets);

	spdk_poller_unregister(&bdev_io->u.reset.wait_poller.poller);
	spdk_bdev_for_each_channel(ch->bdev, bdev_reset_check_outstanding_io, ch,
				   bdev_reset_check_outstanding_io_done);

	return SPDK_POLLER_BUSY;
}

static void
bdev_reset_freeze_channel_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_channel *ch = _ctx;
	struct spdk_bdev_io *bdev_io;

	bdev_io = TAILQ_FIRST(&ch->queued_resets);

	if (bdev->reset_io_drain_timeout == 0) {
		TAILQ_REMOVE(&ch->queued_resets, bdev_io, internal.link);

		bdev_io_submit_reset(bdev_io);
		return;
	}

	bdev_io->u.reset.wait_poller.stop_time_tsc = spdk_get_ticks() +
			(ch->bdev->reset_io_drain_timeout * spdk_get_ticks_hz());

	/* In case bdev->reset_io_drain_timeout is not equal to zero,
	 * submit the reset to the underlying module only if outstanding I/O
	 * remain after reset_io_drain_timeout seconds have passed. */
	spdk_bdev_for_each_channel(ch->bdev, bdev_reset_check_outstanding_io, ch,
				   bdev_reset_check_outstanding_io_done);
}

static void
bdev_reset_freeze_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel	*channel;
	struct spdk_bdev_mgmt_channel	*mgmt_channel;
	struct spdk_bdev_shared_resource *shared_resource;
	bdev_io_tailq_t			tmp_queued;

	TAILQ_INIT(&tmp_queued);

	channel = __io_ch_to_bdev_ch(ch);
	shared_resource = channel->shared_resource;
	mgmt_channel = shared_resource->mgmt_ch;

	channel->flags |= BDEV_CH_RESET_IN_PROGRESS;

	if ((channel->flags & BDEV_CH_QOS_ENABLED) != 0) {
		/* The QoS object is always valid and readable while
		 * the channel flag is set, so the lock here should not
		 * be necessary. We're not in the fast path though, so
		 * just take it anyway. */
		spdk_spin_lock(&channel->bdev->internal.spinlock);
		if (channel->bdev->internal.qos->ch == channel) {
			TAILQ_SWAP(&channel->bdev->internal.qos->queued, &tmp_queued, spdk_bdev_io, internal.link);
		}
		spdk_spin_unlock(&channel->bdev->internal.spinlock);
	}

	bdev_abort_all_queued_io(&shared_resource->nomem_io, channel);
	bdev_abort_all_buf_io(mgmt_channel, channel);
	bdev_abort_all_buf_io(mgmt_channel, channel);
	bdev_abort_all_queued_io(&tmp_queued, channel);

	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_start_reset(void *ctx)
{
	struct spdk_bdev_channel *ch = ctx;

	spdk_bdev_for_each_channel(ch->bdev, bdev_reset_freeze_channel, ch,
				   bdev_reset_freeze_channel_done);
}

static void
bdev_channel_start_reset(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev *bdev = ch->bdev;

	assert(!TAILQ_EMPTY(&ch->queued_resets));

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.reset_in_progress == NULL) {
		bdev->internal.reset_in_progress = TAILQ_FIRST(&ch->queued_resets);
		/*
		 * Take a channel reference for the target bdev for the life of this
		 *  reset.  This guards against the channel getting destroyed while
		 *  spdk_bdev_for_each_channel() calls related to this reset IO are in
		 *  progress.  We will release the reference when this reset is
		 *  completed.
		 */
		bdev->internal.reset_in_progress->u.reset.ch_ref = spdk_get_io_channel(__bdev_to_io_dev(bdev));
		bdev_start_reset(ch);
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->internal.submit_tsc = spdk_get_ticks();
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	bdev_io->u.reset.ch_ref = NULL;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_spin_lock(&bdev->internal.spinlock);
	TAILQ_INSERT_TAIL(&channel->queued_resets, bdev_io, internal.link);
	spdk_spin_unlock(&bdev->internal.spinlock);

	TAILQ_INSERT_TAIL(&bdev_io->internal.ch->io_submitted, bdev_io,
			  internal.ch_link);

	bdev_channel_start_reset(channel);

	return 0;
}

void
spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		      struct spdk_bdev_io_stat *stat)
{
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	bdev_get_io_stat(stat, channel->stat);
}

static void
bdev_get_device_stat_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx = _ctx;

	bdev_iostat_ctx->cb(bdev, bdev_iostat_ctx->stat,
			    bdev_iostat_ctx->cb_arg, 0);
	free(bdev_iostat_ctx);
}

static void
bdev_get_each_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			   struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx = _ctx;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	spdk_bdev_add_io_stat(bdev_iostat_ctx->stat, channel->stat);
	spdk_bdev_for_each_channel_continue(i, 0);
}

void
spdk_bdev_get_device_stat(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
			  spdk_bdev_get_device_stat_cb cb, void *cb_arg)
{
	struct spdk_bdev_iostat_ctx *bdev_iostat_ctx;

	assert(bdev != NULL);
	assert(stat != NULL);
	assert(cb != NULL);

	bdev_iostat_ctx = calloc(1, sizeof(struct spdk_bdev_iostat_ctx));
	if (bdev_iostat_ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for spdk_bdev_iostat_ctx\n");
		cb(bdev, stat, cb_arg, -ENOMEM);
		return;
	}

	bdev_iostat_ctx->stat = stat;
	bdev_iostat_ctx->cb = cb;
	bdev_iostat_ctx->cb_arg = cb_arg;

	/* Start with the statistics from previously deleted channels. */
	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_get_io_stat(bdev_iostat_ctx->stat, bdev->internal.stat);
	spdk_spin_unlock(&bdev->internal.spinlock);

	/* Then iterate and add the statistics from each existing channel. */
	spdk_bdev_for_each_channel(bdev, bdev_get_each_channel_stat, bdev_iostat_ctx,
				   bdev_get_device_stat_done);
}

struct bdev_iostat_reset_ctx {
	enum spdk_bdev_reset_stat_mode mode;
	bdev_reset_device_stat_cb cb;
	void *cb_arg;
};

static void
bdev_reset_device_stat_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct bdev_iostat_reset_ctx *ctx = _ctx;

	ctx->cb(bdev, ctx->cb_arg, 0);

	free(ctx);
}

static void
bdev_reset_each_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			     struct spdk_io_channel *ch, void *_ctx)
{
	struct bdev_iostat_reset_ctx *ctx = _ctx;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	spdk_bdev_reset_io_stat(channel->stat, ctx->mode);

	spdk_bdev_for_each_channel_continue(i, 0);
}

void
bdev_reset_device_stat(struct spdk_bdev *bdev, enum spdk_bdev_reset_stat_mode mode,
		       bdev_reset_device_stat_cb cb, void *cb_arg)
{
	struct bdev_iostat_reset_ctx *ctx;

	assert(bdev != NULL);
	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate bdev_iostat_reset_ctx.\n");
		cb(bdev, cb_arg, -ENOMEM);
		return;
	}

	ctx->mode = mode;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	spdk_spin_lock(&bdev->internal.spinlock);
	spdk_bdev_reset_io_stat(bdev->internal.stat, mode);
	spdk_spin_unlock(&bdev->internal.spinlock);

	spdk_bdev_for_each_channel(bdev,
				   bdev_reset_each_channel_stat,
				   ctx,
				   bdev_reset_device_stat_done);
}

int
spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN))) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = NULL;
	bdev_io->u.nvme_passthru.md_len = 0;

	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO))) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = NULL;
	bdev_io->u.nvme_passthru.md_len = 0;

	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO_MD))) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO_MD;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = md_buf;
	bdev_io->u.nvme_passthru.md_len = md_len;

	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

static void bdev_abort_retry(void *ctx);
static void bdev_abort(struct spdk_bdev_io *parent_io);

static void
bdev_abort_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_channel *channel = bdev_io->internal.ch;
	struct spdk_bdev_io *parent_io = cb_arg;
	struct spdk_bdev_io *bio_to_abort, *tmp_io;

	bio_to_abort = bdev_io->u.abort.bio_to_abort;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* Check if the target I/O completed in the meantime. */
		TAILQ_FOREACH(tmp_io, &channel->io_submitted, internal.ch_link) {
			if (tmp_io == bio_to_abort) {
				break;
			}
		}

		/* If the target I/O still exists, set the parent to failed. */
		if (tmp_io != NULL) {
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	parent_io->u.bdev.split_outstanding--;
	if (parent_io->u.bdev.split_outstanding == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			bdev_abort_retry(parent_io);
		} else {
			bdev_io_complete(parent_io);
		}
	}
}

static int
bdev_abort_io(struct spdk_bdev_desc *desc, struct spdk_bdev_channel *channel,
	      struct spdk_bdev_io *bio_to_abort,
	      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;

	if (bio_to_abort->type == SPDK_BDEV_IO_TYPE_ABORT ||
	    bio_to_abort->type == SPDK_BDEV_IO_TYPE_RESET) {
		/* TODO: Abort reset or abort request. */
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (bdev_io == NULL) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ABORT;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	if (bdev->split_on_optimal_io_boundary && bdev_io_should_split(bio_to_abort)) {
		bdev_io->u.bdev.abort.bio_cb_arg = bio_to_abort;

		/* Parent abort request is not submitted directly, but to manage its
		 * execution add it to the submitted list here.
		 */
		bdev_io->internal.submit_tsc = spdk_get_ticks();
		TAILQ_INSERT_TAIL(&channel->io_submitted, bdev_io, internal.ch_link);

		bdev_abort(bdev_io);

		return 0;
	}

	bdev_io->u.abort.bio_to_abort = bio_to_abort;

	/* Submit the abort request to the underlying bdev module. */
	bdev_io_submit(bdev_io);

	return 0;
}

static uint32_t
_bdev_abort(struct spdk_bdev_io *parent_io)
{
	struct spdk_bdev_desc *desc = parent_io->internal.desc;
	struct spdk_bdev_channel *channel = parent_io->internal.ch;
	void *bio_cb_arg;
	struct spdk_bdev_io *bio_to_abort;
	uint32_t matched_ios;
	int rc;

	bio_cb_arg = parent_io->u.bdev.abort.bio_cb_arg;

	/* matched_ios is returned and will be kept by the caller.
	 *
	 * This function will be used for two cases, 1) the same cb_arg is used for
	 * multiple I/Os, 2) a single large I/O is split into smaller ones.
	 * Incrementing split_outstanding directly here may confuse readers especially
	 * for the 1st case.
	 *
	 * Completion of I/O abort is processed after stack unwinding. Hence this trick
	 * works as expected.
	 */
	matched_ios = 0;
	parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;

	TAILQ_FOREACH(bio_to_abort, &channel->io_submitted, internal.ch_link) {
		if (bio_to_abort->internal.caller_ctx != bio_cb_arg) {
			continue;
		}

		if (bio_to_abort->internal.submit_tsc > parent_io->internal.submit_tsc) {
			/* Any I/O which was submitted after this abort command should be excluded. */
			continue;
		}

		rc = bdev_abort_io(desc, channel, bio_to_abort, bdev_abort_io_done, parent_io);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				parent_io->internal.status = SPDK_BDEV_IO_STATUS_NOMEM;
			} else {
				parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			}
			break;
		}
		matched_ios++;
	}

	return matched_ios;
}

static void
bdev_abort_retry(void *ctx)
{
	struct spdk_bdev_io *parent_io = ctx;
	uint32_t matched_ios;

	matched_ios = _bdev_abort(parent_io);

	if (matched_ios == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			bdev_queue_io_wait_with_cb(parent_io, bdev_abort_retry);
		} else {
			/* For retry, the case that no target I/O was found is success
			 * because it means target I/Os completed in the meantime.
			 */
			bdev_io_complete(parent_io);
		}
		return;
	}

	/* Use split_outstanding to manage the progress of aborting I/Os. */
	parent_io->u.bdev.split_outstanding = matched_ios;
}

static void
bdev_abort(struct spdk_bdev_io *parent_io)
{
	uint32_t matched_ios;

	matched_ios = _bdev_abort(parent_io);

	if (matched_ios == 0) {
		if (parent_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM) {
			bdev_queue_io_wait_with_cb(parent_io, bdev_abort_retry);
		} else {
			/* The case the no target I/O was found is failure. */
			parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
			bdev_io_complete(parent_io);
		}
		return;
	}

	/* Use split_outstanding to manage the progress of aborting I/Os. */
	parent_io->u.bdev.split_outstanding = matched_ios;
}

int
spdk_bdev_abort(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *bio_cb_arg,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
	struct spdk_bdev_io *bdev_io;

	if (bio_cb_arg == NULL) {
		return -EINVAL;
	}

	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ABORT)) {
		return -ENOTSUP;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (bdev_io == NULL) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->internal.submit_tsc = spdk_get_ticks();
	bdev_io->type = SPDK_BDEV_IO_TYPE_ABORT;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io->u.bdev.abort.bio_cb_arg = bio_cb_arg;

	/* Parent abort request is not submitted directly, but to manage its execution,
	 * add it to the submitted list here.
	 */
	TAILQ_INSERT_TAIL(&channel->io_submitted, bdev_io, internal.ch_link);

	bdev_abort(bdev_io);

	return 0;
}

int
spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			struct spdk_bdev_io_wait_entry *entry)
{
	struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
	struct spdk_bdev_mgmt_channel *mgmt_ch = channel->shared_resource->mgmt_ch;

	if (bdev != entry->bdev) {
		SPDK_ERRLOG("bdevs do not match\n");
		return -EINVAL;
	}

	if (mgmt_ch->per_thread_cache_count > 0) {
		SPDK_ERRLOG("Cannot queue io_wait if spdk_bdev_io available in per-thread cache\n");
		return -EINVAL;
	}

	TAILQ_INSERT_TAIL(&mgmt_ch->io_wait_queue, entry, link);
	return 0;
}

static inline void
bdev_io_update_io_stat(struct spdk_bdev_io *bdev_io, uint64_t tsc_diff)
{
	enum spdk_bdev_io_status io_status = bdev_io->internal.status;
	struct spdk_bdev_io_stat *io_stat = bdev_io->internal.ch->stat;
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
	uint32_t blocklen = bdev_io->bdev->blocklen;

	if (spdk_likely(io_status == SPDK_BDEV_IO_STATUS_SUCCESS)) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			io_stat->bytes_read += num_blocks * blocklen;
			io_stat->num_read_ops++;
			io_stat->read_latency_ticks += tsc_diff;
			if (io_stat->max_read_latency_ticks < tsc_diff) {
				io_stat->max_read_latency_ticks = tsc_diff;
			}
			if (io_stat->min_read_latency_ticks > tsc_diff) {
				io_stat->min_read_latency_ticks = tsc_diff;
			}
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			io_stat->bytes_written += num_blocks * blocklen;
			io_stat->num_write_ops++;
			io_stat->write_latency_ticks += tsc_diff;
			if (io_stat->max_write_latency_ticks < tsc_diff) {
				io_stat->max_write_latency_ticks = tsc_diff;
			}
			if (io_stat->min_write_latency_ticks > tsc_diff) {
				io_stat->min_write_latency_ticks = tsc_diff;
			}
			break;
		case SPDK_BDEV_IO_TYPE_UNMAP:
			io_stat->bytes_unmapped += num_blocks * blocklen;
			io_stat->num_unmap_ops++;
			io_stat->unmap_latency_ticks += tsc_diff;
			if (io_stat->max_unmap_latency_ticks < tsc_diff) {
				io_stat->max_unmap_latency_ticks = tsc_diff;
			}
			if (io_stat->min_unmap_latency_ticks > tsc_diff) {
				io_stat->min_unmap_latency_ticks = tsc_diff;
			}
			break;
		case SPDK_BDEV_IO_TYPE_ZCOPY:
			/* Track the data in the start phase only */
			if (bdev_io->u.bdev.zcopy.start) {
				if (bdev_io->u.bdev.zcopy.populate) {
					io_stat->bytes_read += num_blocks * blocklen;
					io_stat->num_read_ops++;
					io_stat->read_latency_ticks += tsc_diff;
					if (io_stat->max_read_latency_ticks < tsc_diff) {
						io_stat->max_read_latency_ticks = tsc_diff;
					}
					if (io_stat->min_read_latency_ticks > tsc_diff) {
						io_stat->min_read_latency_ticks = tsc_diff;
					}
				} else {
					io_stat->bytes_written += num_blocks * blocklen;
					io_stat->num_write_ops++;
					io_stat->write_latency_ticks += tsc_diff;
					if (io_stat->max_write_latency_ticks < tsc_diff) {
						io_stat->max_write_latency_ticks = tsc_diff;
					}
					if (io_stat->min_write_latency_ticks > tsc_diff) {
						io_stat->min_write_latency_ticks = tsc_diff;
					}
				}
			}
			break;
		case SPDK_BDEV_IO_TYPE_COPY:
			io_stat->bytes_copied += num_blocks * blocklen;
			io_stat->num_copy_ops++;
			bdev_io->internal.ch->stat->copy_latency_ticks += tsc_diff;
			if (io_stat->max_copy_latency_ticks < tsc_diff) {
				io_stat->max_copy_latency_ticks = tsc_diff;
			}
			if (io_stat->min_copy_latency_ticks > tsc_diff) {
				io_stat->min_copy_latency_ticks = tsc_diff;
			}
			break;
		default:
			break;
		}
	} else if (io_status <= SPDK_BDEV_IO_STATUS_FAILED && io_status >= SPDK_MIN_BDEV_IO_STATUS) {
		io_stat = bdev_io->bdev->internal.stat;
		assert(io_stat->io_error != NULL);

		spdk_spin_lock(&bdev_io->bdev->internal.spinlock);
		io_stat->io_error->error_status[-io_status - 1]++;
		spdk_spin_unlock(&bdev_io->bdev->internal.spinlock);
	}

#ifdef SPDK_CONFIG_VTUNE
	uint64_t now_tsc = spdk_get_ticks();
	if (now_tsc > (bdev_io->internal.ch->start_tsc + bdev_io->internal.ch->interval_tsc)) {
		uint64_t data[5];
		struct spdk_bdev_io_stat *prev_stat = bdev_io->internal.ch->prev_stat;

		data[0] = io_stat->num_read_ops - prev_stat->num_read_ops;
		data[1] = io_stat->bytes_read - prev_stat->bytes_read;
		data[2] = io_stat->num_write_ops - prev_stat->num_write_ops;
		data[3] = io_stat->bytes_written - prev_stat->bytes_written;
		data[4] = bdev_io->bdev->fn_table->get_spin_time ?
			  bdev_io->bdev->fn_table->get_spin_time(spdk_bdev_io_get_io_channel(bdev_io)) : 0;

		__itt_metadata_add(g_bdev_mgr.domain, __itt_null, bdev_io->internal.ch->handle,
				   __itt_metadata_u64, 5, data);

		memcpy(prev_stat, io_stat, sizeof(struct spdk_bdev_io_stat));
		bdev_io->internal.ch->start_tsc = now_tsc;
	}
#endif
}

static inline void
bdev_io_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	uint64_t tsc, tsc_diff;

	if (spdk_unlikely(bdev_io->internal.in_submit_request)) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
				     bdev_io_complete, bdev_io);
		return;
	}

	tsc = spdk_get_ticks();
	tsc_diff = tsc - bdev_io->internal.submit_tsc;
	spdk_trace_record_tsc(tsc, TRACE_BDEV_IO_DONE, 0, 0, (uintptr_t)bdev_io,
			      bdev_io->internal.caller_ctx);

	TAILQ_REMOVE(&bdev_ch->io_submitted, bdev_io, internal.ch_link);

	if (bdev_io->internal.ch->histogram) {
		spdk_histogram_data_tally(bdev_io->internal.ch->histogram, tsc_diff);
	}

	bdev_io_update_io_stat(bdev_io, tsc_diff);

	assert(bdev_io->internal.cb != NULL);
	assert(spdk_get_thread() == spdk_bdev_io_get_thread(bdev_io));

	bdev_io->internal.cb(bdev_io, bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS,
			     bdev_io->internal.caller_ctx);
}

static void bdev_destroy_cb(void *io_device);

static void
bdev_reset_complete(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_io *bdev_io = _ctx;

	if (bdev_io->u.reset.ch_ref != NULL) {
		spdk_put_io_channel(bdev_io->u.reset.ch_ref);
		bdev_io->u.reset.ch_ref = NULL;
	}

	bdev_io_complete(bdev_io);

	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING &&
	    TAILQ_EMPTY(&bdev->internal.open_descs)) {
		spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb);
	}
}

static void
bdev_unfreeze_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		      struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_io *bdev_io = _ctx;
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct spdk_bdev_io *queued_reset;

	ch->flags &= ~BDEV_CH_RESET_IN_PROGRESS;
	while (!TAILQ_EMPTY(&ch->queued_resets)) {
		queued_reset = TAILQ_FIRST(&ch->queued_resets);
		TAILQ_REMOVE(&ch->queued_resets, queued_reset, internal.link);
		spdk_bdev_io_complete(queued_reset, bdev_io->internal.status);
	}

	spdk_bdev_for_each_channel_continue(i, 0);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->internal.ch;
	struct spdk_bdev_shared_resource *shared_resource = bdev_ch->shared_resource;

	bdev_io->internal.status = status;

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_RESET)) {
		bool unlock_channels = false;

		if (status == SPDK_BDEV_IO_STATUS_NOMEM) {
			SPDK_ERRLOG("NOMEM returned for reset\n");
		}
		spdk_spin_lock(&bdev->internal.spinlock);
		if (bdev_io == bdev->internal.reset_in_progress) {
			bdev->internal.reset_in_progress = NULL;
			unlock_channels = true;
		}
		spdk_spin_unlock(&bdev->internal.spinlock);

		if (unlock_channels) {
			spdk_bdev_for_each_channel(bdev, bdev_unfreeze_channel, bdev_io,
						   bdev_reset_complete);
			return;
		}
	} else {
		if (spdk_unlikely(bdev_io->internal.orig_iovcnt != 0)) {
			_bdev_io_push_bounce_data_buffer(bdev_io, _bdev_io_complete_push_bounce_done);
			/* bdev IO will be completed in the callback */
			return;
		}

		_bdev_io_decrement_outstanding(bdev_ch, shared_resource);
		if (spdk_unlikely(_bdev_io_handle_no_mem(bdev_io))) {
			return;
		}
	}

	bdev_io_complete(bdev_io);
}

void
spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				  enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	if (sc == SPDK_SCSI_STATUS_GOOD) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
		bdev_io->internal.error.scsi.sc = sc;
		bdev_io->internal.error.scsi.sk = sk;
		bdev_io->internal.error.scsi.asc = asc;
		bdev_io->internal.error.scsi.ascq = ascq;
	}

	spdk_bdev_io_complete(bdev_io, bdev_io->internal.status);
}

void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	assert(sc != NULL);
	assert(sk != NULL);
	assert(asc != NULL);
	assert(ascq != NULL);

	switch (bdev_io->internal.status) {
	case SPDK_BDEV_IO_STATUS_SUCCESS:
		*sc = SPDK_SCSI_STATUS_GOOD;
		*sk = SPDK_SCSI_SENSE_NO_SENSE;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	case SPDK_BDEV_IO_STATUS_NVME_ERROR:
		spdk_scsi_nvme_translate(bdev_io, sc, sk, asc, ascq);
		break;
	case SPDK_BDEV_IO_STATUS_SCSI_ERROR:
		*sc = bdev_io->internal.error.scsi.sc;
		*sk = bdev_io->internal.error.scsi.sk;
		*asc = bdev_io->internal.error.scsi.asc;
		*ascq = bdev_io->internal.error.scsi.ascq;
		break;
	default:
		*sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		*sk = SPDK_SCSI_SENSE_ABORTED_COMMAND;
		*asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		*ascq = SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE;
		break;
	}
}

void
spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result)
{
	if (aio_result == 0) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_AIO_ERROR;
	}

	bdev_io->internal.error.aio_result = aio_result;

	spdk_bdev_io_complete(bdev_io, bdev_io->internal.status);
}

void
spdk_bdev_io_get_aio_status(const struct spdk_bdev_io *bdev_io, int *aio_result)
{
	assert(aio_result != NULL);

	if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_AIO_ERROR) {
		*aio_result = bdev_io->internal.error.aio_result;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*aio_result = 0;
	} else {
		*aio_result = -EIO;
	}
}

void
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct, int sc)
{
	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_ABORTED;
	} else {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
	}

	bdev_io->internal.error.nvme.cdw0 = cdw0;
	bdev_io->internal.error.nvme.sct = sct;
	bdev_io->internal.error.nvme.sc = sc;

	spdk_bdev_io_complete(bdev_io, bdev_io->internal.status);
}

void
spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0, int *sct, int *sc)
{
	assert(sct != NULL);
	assert(sc != NULL);
	assert(cdw0 != NULL);

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT)) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;
		if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			*cdw0 = 0;
		} else {
			*cdw0 = 1U;
		}
		return;
	}

	if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		*sct = bdev_io->internal.error.nvme.sct;
		*sc = bdev_io->internal.error.nvme.sc;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	} else {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	*cdw0 = bdev_io->internal.error.nvme.cdw0;
}

void
spdk_bdev_io_get_nvme_fused_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
				   int *first_sct, int *first_sc, int *second_sct, int *second_sc)
{
	assert(first_sct != NULL);
	assert(first_sc != NULL);
	assert(second_sct != NULL);
	assert(second_sc != NULL);
	assert(cdw0 != NULL);

	if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		if (bdev_io->internal.error.nvme.sct == SPDK_NVME_SCT_MEDIA_ERROR &&
		    bdev_io->internal.error.nvme.sc == SPDK_NVME_SC_COMPARE_FAILURE) {
			*first_sct = bdev_io->internal.error.nvme.sct;
			*first_sc = bdev_io->internal.error.nvme.sc;
			*second_sct = SPDK_NVME_SCT_GENERIC;
			*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
		} else {
			*first_sct = SPDK_NVME_SCT_GENERIC;
			*first_sc = SPDK_NVME_SC_SUCCESS;
			*second_sct = bdev_io->internal.error.nvme.sct;
			*second_sc = bdev_io->internal.error.nvme.sc;
		}
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED) {
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_SUCCESS;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_SUCCESS;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED) {
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	} else if (bdev_io->internal.status == SPDK_BDEV_IO_STATUS_MISCOMPARE) {
		*first_sct = SPDK_NVME_SCT_MEDIA_ERROR;
		*first_sc = SPDK_NVME_SC_COMPARE_FAILURE;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	} else {
		*first_sct = SPDK_NVME_SCT_GENERIC;
		*first_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		*second_sct = SPDK_NVME_SCT_GENERIC;
		*second_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	*cdw0 = bdev_io->internal.error.nvme.cdw0;
}

struct spdk_thread *
spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io)
{
	return spdk_io_channel_get_thread(bdev_io->internal.ch->channel);
}

struct spdk_io_channel *
spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->internal.ch->channel;
}

static int
bdev_register(struct spdk_bdev *bdev)
{
	char *bdev_name;
	char uuid[SPDK_UUID_STRING_LEN];
	int ret;

	assert(bdev->module != NULL);

	if (!bdev->name) {
		SPDK_ERRLOG("Bdev name is NULL\n");
		return -EINVAL;
	}

	if (!strlen(bdev->name)) {
		SPDK_ERRLOG("Bdev name must not be an empty string\n");
		return -EINVAL;
	}

	/* Users often register their own I/O devices using the bdev name. In
	 * order to avoid conflicts, prepend bdev_. */
	bdev_name = spdk_sprintf_alloc("bdev_%s", bdev->name);
	if (!bdev_name) {
		SPDK_ERRLOG("Unable to allocate memory for internal bdev name.\n");
		return -ENOMEM;
	}

	bdev->internal.stat = bdev_alloc_io_stat(true);
	if (!bdev->internal.stat) {
		SPDK_ERRLOG("Unable to allocate I/O statistics structure.\n");
		free(bdev_name);
		return -ENOMEM;
	}

	bdev->internal.status = SPDK_BDEV_STATUS_READY;
	bdev->internal.measured_queue_depth = UINT64_MAX;
	bdev->internal.claim_type = SPDK_BDEV_CLAIM_NONE;
	memset(&bdev->internal.claim, 0, sizeof(bdev->internal.claim));
	bdev->internal.qd_poller = NULL;
	bdev->internal.qos = NULL;

	TAILQ_INIT(&bdev->internal.open_descs);
	TAILQ_INIT(&bdev->internal.locked_ranges);
	TAILQ_INIT(&bdev->internal.pending_locked_ranges);
	TAILQ_INIT(&bdev->aliases);

	ret = bdev_name_add(&bdev->internal.bdev_name, bdev, bdev->name);
	if (ret != 0) {
		bdev_free_io_stat(bdev->internal.stat);
		free(bdev_name);
		return ret;
	}

	/* UUID has to be specified by the user or defined by bdev itself.
	 * Otherwise this field must remain empty, to indicate that this
	 * value cannot be depended upon. */
	if (!spdk_mem_all_zero(&bdev->uuid, sizeof(bdev->uuid))) {
		/* Add the UUID alias only if it's different than the name */
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &bdev->uuid);
		if (strcmp(bdev->name, uuid) != 0) {
			ret = spdk_bdev_alias_add(bdev, uuid);
			if (ret != 0) {
				SPDK_ERRLOG("Unable to add uuid:%s alias for bdev %s\n", uuid, bdev->name);
				bdev_name_del(&bdev->internal.bdev_name);
				bdev_free_io_stat(bdev->internal.stat);
				free(bdev_name);
				return ret;
			}
		}
	}

	if (spdk_bdev_get_buf_align(bdev) > 1) {
		if (bdev->split_on_optimal_io_boundary) {
			bdev->optimal_io_boundary = spdk_min(bdev->optimal_io_boundary,
							     SPDK_BDEV_LARGE_BUF_MAX_SIZE / bdev->blocklen);
		} else {
			bdev->split_on_optimal_io_boundary = true;
			bdev->optimal_io_boundary = SPDK_BDEV_LARGE_BUF_MAX_SIZE / bdev->blocklen;
		}
	}

	/* If the user didn't specify a write unit size, set it to one. */
	if (bdev->write_unit_size == 0) {
		bdev->write_unit_size = 1;
	}

	/* Set ACWU value to the write unit size if bdev module did not set it (does not support it natively) */
	if (bdev->acwu == 0) {
		bdev->acwu = bdev->write_unit_size;
	}

	if (bdev->phys_blocklen == 0) {
		bdev->phys_blocklen = spdk_bdev_get_data_block_size(bdev);
	}

	bdev->internal.reset_in_progress = NULL;
	bdev->internal.qd_poll_in_progress = false;
	bdev->internal.period = 0;
	bdev->internal.new_period = 0;

	spdk_io_device_register(__bdev_to_io_dev(bdev),
				bdev_channel_create, bdev_channel_destroy,
				sizeof(struct spdk_bdev_channel),
				bdev_name);

	free(bdev_name);

	spdk_spin_init(&bdev->internal.spinlock);

	SPDK_DEBUGLOG(bdev, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdevs, bdev, internal.link);

	return 0;
}

static void
bdev_destroy_cb(void *io_device)
{
	int			rc;
	struct spdk_bdev	*bdev;
	spdk_bdev_unregister_cb	cb_fn;
	void			*cb_arg;

	bdev = __bdev_from_io_dev(io_device);

	if (bdev->internal.unregister_td != spdk_get_thread()) {
		spdk_thread_send_msg(bdev->internal.unregister_td, bdev_destroy_cb, io_device);
		return;
	}

	cb_fn = bdev->internal.unregister_cb;
	cb_arg = bdev->internal.unregister_ctx;

	spdk_spin_destroy(&bdev->internal.spinlock);
	free(bdev->internal.qos);
	bdev_free_io_stat(bdev->internal.stat);

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n");
	}
	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, rc);
	}
}

void
spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno)
{
	if (bdev->internal.unregister_cb != NULL) {
		bdev->internal.unregister_cb(bdev->internal.unregister_ctx, bdeverrno);
	}
}

static void
_remove_notify(void *arg)
{
	struct spdk_bdev_desc *desc = arg;

	spdk_spin_lock(&desc->spinlock);
	desc->refs--;

	if (!desc->closed) {
		spdk_spin_unlock(&desc->spinlock);
		desc->callback.event_fn(SPDK_BDEV_EVENT_REMOVE, desc->bdev, desc->callback.ctx);
		return;
	} else if (0 == desc->refs) {
		/* This descriptor was closed after this remove_notify message was sent.
		 * spdk_bdev_close() could not free the descriptor since this message was
		 * in flight, so we free it now using bdev_desc_free().
		 */
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
		return;
	}
	spdk_spin_unlock(&desc->spinlock);
}

/* returns: 0 - bdev removed and ready to be destructed.
 *          -EBUSY - bdev can't be destructed yet.  */
static int
bdev_unregister_unsafe(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc	*desc, *tmp;
	int			rc = 0;
	char			uuid[SPDK_UUID_STRING_LEN];

	assert(spdk_spin_held(&g_bdev_mgr.spinlock));
	assert(spdk_spin_held(&bdev->internal.spinlock));

	/* Notify each descriptor about hotremoval */
	TAILQ_FOREACH_SAFE(desc, &bdev->internal.open_descs, link, tmp) {
		rc = -EBUSY;
		spdk_spin_lock(&desc->spinlock);
		/*
		 * Defer invocation of the event_cb to a separate message that will
		 *  run later on its thread.  This ensures this context unwinds and
		 *  we don't recursively unregister this bdev again if the event_cb
		 *  immediately closes its descriptor.
		 */
		desc->refs++;
		spdk_thread_send_msg(desc->thread, _remove_notify, desc);
		spdk_spin_unlock(&desc->spinlock);
	}

	/* If there are no descriptors, proceed removing the bdev */
	if (rc == 0) {
		TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, internal.link);
		SPDK_DEBUGLOG(bdev, "Removing bdev %s from list done\n", bdev->name);

		/* Delete the name and the UUID alias */
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &bdev->uuid);
		bdev_name_del_unsafe(&bdev->internal.bdev_name);
		bdev_alias_del(bdev, uuid, bdev_name_del_unsafe);

		spdk_notify_send("bdev_unregister", spdk_bdev_get_name(bdev));

		if (bdev->internal.reset_in_progress != NULL) {
			/* If reset is in progress, let the completion callback for reset
			 * unregister the bdev.
			 */
			rc = -EBUSY;
		}
	}

	return rc;
}

static void
bdev_unregister_abort_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			      struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);

	bdev_channel_abort_queued_ios(bdev_ch);
	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_unregister(struct spdk_bdev *bdev, void *_ctx, int status)
{
	int rc;

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	spdk_spin_lock(&bdev->internal.spinlock);
	/*
	 * Set the status to REMOVING after completing to abort channels. Otherwise,
	 * the last spdk_bdev_close() may call spdk_io_device_unregister() while
	 * spdk_bdev_for_each_channel() is executed and spdk_io_device_unregister()
	 * may fail.
	 */
	bdev->internal.status = SPDK_BDEV_STATUS_REMOVING;
	rc = bdev_unregister_unsafe(bdev);
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	if (rc == 0) {
		spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb);
	}
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_thread	*thread;

	SPDK_DEBUGLOG(bdev, "Removing bdev %s from list\n", bdev->name);

	thread = spdk_get_thread();
	if (!thread) {
		/* The user called this from a non-SPDK thread. */
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOTSUP);
		}
		return;
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	if (bdev->internal.status == SPDK_BDEV_STATUS_UNREGISTERING ||
	    bdev->internal.status == SPDK_BDEV_STATUS_REMOVING) {
		spdk_spin_unlock(&g_bdev_mgr.spinlock);
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev->internal.status = SPDK_BDEV_STATUS_UNREGISTERING;
	bdev->internal.unregister_cb = cb_fn;
	bdev->internal.unregister_ctx = cb_arg;
	bdev->internal.unregister_td = thread;
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	spdk_bdev_set_qd_sampling_period(bdev, 0);

	spdk_bdev_for_each_channel(bdev, bdev_unregister_abort_channel, bdev,
				   bdev_unregister);
}

int
spdk_bdev_unregister_by_name(const char *bdev_name, struct spdk_bdev_module *module,
			     spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	rc = spdk_bdev_open_ext(bdev_name, false, _tmp_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev with name: %s\n", bdev_name);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	if (bdev->module != module) {
		spdk_bdev_close(desc);
		SPDK_ERRLOG("Bdev %s was not registered by the specified module.\n",
			    bdev_name);
		return -ENODEV;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);

	spdk_bdev_close(desc);

	return 0;
}

static int
bdev_start_qos(struct spdk_bdev *bdev)
{
	struct set_qos_limit_ctx *ctx;

	/* Enable QoS */
	if (bdev->internal.qos && bdev->internal.qos->thread == NULL) {
		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate memory for QoS context\n");
			return -ENOMEM;
		}
		ctx->bdev = bdev;
		spdk_bdev_for_each_channel(bdev, bdev_enable_qos_msg, ctx, bdev_enable_qos_done);
	}

	return 0;
}

static int
bdev_open(struct spdk_bdev *bdev, bool write, struct spdk_bdev_desc *desc)
{
	struct spdk_thread *thread;
	int rc = 0;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("Cannot open bdev from non-SPDK thread.\n");
		return -ENOTSUP;
	}

	SPDK_DEBUGLOG(bdev, "Opening descriptor %p for bdev %s on thread %p\n", desc, bdev->name,
		      spdk_get_thread());

	desc->bdev = bdev;
	desc->thread = thread;
	desc->write = write;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.status == SPDK_BDEV_STATUS_UNREGISTERING ||
	    bdev->internal.status == SPDK_BDEV_STATUS_REMOVING) {
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -ENODEV;
	}

	if (write && bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		SPDK_ERRLOG("Could not open %s - %s module already claimed it\n",
			    bdev->name, bdev->internal.claim.v1.module->name);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	rc = bdev_start_qos(bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to start QoS on bdev %s\n", bdev->name);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return rc;
	}

	TAILQ_INSERT_TAIL(&bdev->internal.open_descs, desc, link);

	spdk_spin_unlock(&bdev->internal.spinlock);

	return 0;
}

static int
bdev_desc_alloc(struct spdk_bdev *bdev, spdk_bdev_event_cb_t event_cb, void *event_ctx,
		struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	unsigned int event_id;

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for bdev descriptor\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&desc->pending_media_events);
	TAILQ_INIT(&desc->free_media_events);

	desc->memory_domains_supported = spdk_bdev_get_memory_domains(bdev, NULL, 0) > 0;
	desc->callback.event_fn = event_cb;
	desc->callback.ctx = event_ctx;
	spdk_spin_init(&desc->spinlock);

	if (bdev->media_events) {
		desc->media_events_buffer = calloc(MEDIA_EVENT_POOL_SIZE,
						   sizeof(*desc->media_events_buffer));
		if (desc->media_events_buffer == NULL) {
			SPDK_ERRLOG("Failed to initialize media event pool\n");
			bdev_desc_free(desc);
			return -ENOMEM;
		}

		for (event_id = 0; event_id < MEDIA_EVENT_POOL_SIZE; ++event_id) {
			TAILQ_INSERT_TAIL(&desc->free_media_events,
					  &desc->media_events_buffer[event_id], tailq);
		}
	}

	*_desc = desc;

	return 0;
}

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	if (event_cb == NULL) {
		SPDK_ERRLOG("Missing event callback function\n");
		return -EINVAL;
	}

	spdk_spin_lock(&g_bdev_mgr.spinlock);

	bdev = bdev_get_by_name(bdev_name);

	if (bdev == NULL) {
		SPDK_NOTICELOG("Currently unable to find bdev with name: %s\n", bdev_name);
		spdk_spin_unlock(&g_bdev_mgr.spinlock);
		return -ENODEV;
	}

	rc = bdev_desc_alloc(bdev, event_cb, event_ctx, &desc);
	if (rc != 0) {
		spdk_spin_unlock(&g_bdev_mgr.spinlock);
		return rc;
	}

	rc = bdev_open(bdev, write, desc);
	if (rc != 0) {
		bdev_desc_free(desc);
		desc = NULL;
	}

	*_desc = desc;

	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

static void
bdev_close(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc)
{
	int rc;

	spdk_spin_lock(&bdev->internal.spinlock);
	spdk_spin_lock(&desc->spinlock);

	TAILQ_REMOVE(&bdev->internal.open_descs, desc, link);

	desc->closed = true;

	if (0 == desc->refs) {
		spdk_spin_unlock(&desc->spinlock);
		bdev_desc_free(desc);
	} else {
		spdk_spin_unlock(&desc->spinlock);
	}

	/* If no more descriptors, kill QoS channel */
	if (bdev->internal.qos && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		SPDK_DEBUGLOG(bdev, "Closed last descriptor for bdev %s on thread %p. Stopping QoS.\n",
			      bdev->name, spdk_get_thread());

		if (bdev_qos_destroy(bdev)) {
			/* There isn't anything we can do to recover here. Just let the
			 * old QoS poller keep running. The QoS handling won't change
			 * cores when the user allocates a new channel, but it won't break. */
			SPDK_ERRLOG("Unable to shut down QoS poller. It will continue running on the current thread.\n");
		}
	}

	if (bdev->internal.status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->internal.open_descs)) {
		rc = bdev_unregister_unsafe(bdev);
		spdk_spin_unlock(&bdev->internal.spinlock);

		if (rc == 0) {
			spdk_io_device_unregister(__bdev_to_io_dev(bdev), bdev_destroy_cb);
		}
	} else {
		spdk_spin_unlock(&bdev->internal.spinlock);
	}
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	SPDK_DEBUGLOG(bdev, "Closing descriptor %p for bdev %s on thread %p\n", desc, bdev->name,
		      spdk_get_thread());

	assert(desc->thread == spdk_get_thread());

	spdk_poller_unregister(&desc->io_timeout_poller);

	spdk_spin_lock(&g_bdev_mgr.spinlock);

	bdev_close(bdev, desc);

	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

static void
bdev_register_finished(void *arg)
{
	struct spdk_bdev_desc *desc = arg;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);

	spdk_notify_send("bdev_register", spdk_bdev_get_name(bdev));

	spdk_spin_lock(&g_bdev_mgr.spinlock);

	bdev_close(bdev, desc);

	spdk_spin_unlock(&g_bdev_mgr.spinlock);
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;
	int rc;

	if (spdk_unlikely(spdk_thread_get_app_thread() != spdk_get_thread())) {
		SPDK_LOG_DEPRECATED(bdev_register_examine_thread);
	}

	rc = bdev_register(bdev);
	if (rc != 0) {
		return rc;
	}

	/* A descriptor is opened to prevent bdev deletion during examination */
	rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_bdev_unregister(bdev, NULL, NULL);
		return rc;
	}

	rc = bdev_open(bdev, false, desc);
	if (rc != 0) {
		bdev_desc_free(desc);
		spdk_bdev_unregister(bdev, NULL, NULL);
		return rc;
	}

	/* Examine configuration before initializing I/O */
	bdev_examine(bdev);

	rc = spdk_bdev_wait_for_examine(bdev_register_finished, desc);
	if (rc != 0) {
		bdev_close(bdev, desc);
		spdk_bdev_unregister(bdev, NULL, NULL);
	}

	return rc;
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	spdk_spin_lock(&bdev->internal.spinlock);

	if (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE) {
		SPDK_ERRLOG("bdev %s already claimed by module %s\n", bdev->name,
			    bdev->internal.claim.v1.module->name);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EPERM;
	}

	if (desc && !desc->write) {
		desc->write = true;
	}

	bdev->internal.claim_type = SPDK_BDEV_CLAIM_EXCL_WRITE;
	bdev->internal.claim.v1.module = module;

	spdk_spin_unlock(&bdev->internal.spinlock);
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	spdk_spin_lock(&bdev->internal.spinlock);

	assert(bdev->internal.claim.v1.module != NULL);
	assert(bdev->internal.claim_type == SPDK_BDEV_CLAIM_EXCL_WRITE);
	bdev->internal.claim_type = SPDK_BDEV_CLAIM_NONE;
	bdev->internal.claim.v1.module = NULL;

	spdk_spin_unlock(&bdev->internal.spinlock);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	assert(desc != NULL);
	return desc->bdev;
}

int
spdk_for_each_bdev(void *ctx, spdk_for_each_bdev_fn fn)
{
	struct spdk_bdev *bdev, *tmp;
	struct spdk_bdev_desc *desc;
	int rc = 0;

	assert(fn != NULL);

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev = spdk_bdev_first();
	while (bdev != NULL) {
		rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, &desc);
		if (rc != 0) {
			break;
		}
		rc = bdev_open(bdev, false, desc);
		if (rc != 0) {
			bdev_desc_free(desc);
			if (rc == -ENODEV) {
				/* Ignore the error and move to the next bdev. */
				rc = 0;
				bdev = spdk_bdev_next(bdev);
				continue;
			}
			break;
		}
		spdk_spin_unlock(&g_bdev_mgr.spinlock);

		rc = fn(ctx, bdev);

		spdk_spin_lock(&g_bdev_mgr.spinlock);
		tmp = spdk_bdev_next(bdev);
		bdev_close(bdev, desc);
		if (rc != 0) {
			break;
		}
		bdev = tmp;
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

int
spdk_for_each_bdev_leaf(void *ctx, spdk_for_each_bdev_fn fn)
{
	struct spdk_bdev *bdev, *tmp;
	struct spdk_bdev_desc *desc;
	int rc = 0;

	assert(fn != NULL);

	spdk_spin_lock(&g_bdev_mgr.spinlock);
	bdev = spdk_bdev_first_leaf();
	while (bdev != NULL) {
		rc = bdev_desc_alloc(bdev, _tmp_bdev_event_cb, NULL, &desc);
		if (rc != 0) {
			break;
		}
		rc = bdev_open(bdev, false, desc);
		if (rc != 0) {
			bdev_desc_free(desc);
			if (rc == -ENODEV) {
				/* Ignore the error and move to the next bdev. */
				rc = 0;
				bdev = spdk_bdev_next_leaf(bdev);
				continue;
			}
			break;
		}
		spdk_spin_unlock(&g_bdev_mgr.spinlock);

		rc = fn(ctx, bdev);

		spdk_spin_lock(&g_bdev_mgr.spinlock);
		tmp = spdk_bdev_next_leaf(bdev);
		bdev_close(bdev, desc);
		if (rc != 0) {
			break;
		}
		bdev = tmp;
	}
	spdk_spin_unlock(&g_bdev_mgr.spinlock);

	return rc;
}

void
spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp)
{
	struct iovec *iovs;
	int iovcnt;

	if (bdev_io == NULL) {
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		iovs = bdev_io->u.bdev.iovs;
		iovcnt = bdev_io->u.bdev.iovcnt;
		break;
	default:
		iovs = NULL;
		iovcnt = 0;
		break;
	}

	if (iovp) {
		*iovp = iovs;
	}
	if (iovcntp) {
		*iovcntp = iovcnt;
	}
}

void *
spdk_bdev_io_get_md_buf(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io == NULL) {
		return NULL;
	}

	if (!spdk_bdev_is_md_separate(bdev_io->bdev)) {
		return NULL;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ ||
	    bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		return bdev_io->u.bdev.md_buf;
	}

	return NULL;
}

void *
spdk_bdev_io_get_cb_arg(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io == NULL) {
		assert(false);
		return NULL;
	}

	return bdev_io->internal.caller_ctx;
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{

	if (spdk_bdev_module_list_find(bdev_module->name)) {
		SPDK_ERRLOG("ERROR: module '%s' already registered.\n", bdev_module->name);
		assert(false);
	}

	spdk_spin_init(&bdev_module->internal.spinlock);

	/*
	 * Modules with examine callbacks must be initialized first, so they are
	 *  ready to handle examine callbacks from later modules that will
	 *  register physical bdevs.
	 */
	if (bdev_module->examine_config != NULL || bdev_module->examine_disk != NULL) {
		TAILQ_INSERT_HEAD(&g_bdev_mgr.bdev_modules, bdev_module, internal.tailq);
	} else {
		TAILQ_INSERT_TAIL(&g_bdev_mgr.bdev_modules, bdev_module, internal.tailq);
	}
}

struct spdk_bdev_module *
spdk_bdev_module_list_find(const char *name)
{
	struct spdk_bdev_module *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, internal.tailq) {
		if (strcmp(name, bdev_module->name) == 0) {
			break;
		}
	}

	return bdev_module;
}

static void
bdev_write_zero_buffer_next(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	uint64_t num_bytes, num_blocks;
	void *md_buf = NULL;
	int rc;

	num_bytes = spdk_min(_bdev_get_block_size_with_md(bdev_io->bdev) *
			     bdev_io->u.bdev.split_remaining_num_blocks,
			     ZERO_BUFFER_SIZE);
	num_blocks = num_bytes / _bdev_get_block_size_with_md(bdev_io->bdev);
	num_blocks -= num_blocks % bdev_io->bdev->write_unit_size;

	if (spdk_bdev_is_md_separate(bdev_io->bdev)) {
		md_buf = (char *)g_bdev_mgr.zero_buffer +
			 spdk_bdev_get_block_size(bdev_io->bdev) * num_blocks;
	}

	rc = bdev_write_blocks_with_md(bdev_io->internal.desc,
				       spdk_io_channel_from_ctx(bdev_io->internal.ch),
				       g_bdev_mgr.zero_buffer, md_buf,
				       bdev_io->u.bdev.split_current_offset_blocks, num_blocks,
				       bdev_write_zero_buffer_done, bdev_io);
	if (rc == 0) {
		bdev_io->u.bdev.split_remaining_num_blocks -= num_blocks;
		bdev_io->u.bdev.split_current_offset_blocks += num_blocks;
	} else if (rc == -ENOMEM) {
		bdev_queue_io_wait_with_cb(bdev_io, bdev_write_zero_buffer_next);
	} else {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		bdev_io->internal.cb(bdev_io, false, bdev_io->internal.caller_ctx);
	}
}

static void
bdev_write_zero_buffer_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
		parent_io->internal.cb(parent_io, false, parent_io->internal.caller_ctx);
		return;
	}

	if (parent_io->u.bdev.split_remaining_num_blocks == 0) {
		parent_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
		parent_io->internal.cb(parent_io, true, parent_io->internal.caller_ctx);
		return;
	}

	bdev_write_zero_buffer_next(parent_io);
}

static void
bdev_set_qos_limit_done(struct set_qos_limit_ctx *ctx, int status)
{
	spdk_spin_lock(&ctx->bdev->internal.spinlock);
	ctx->bdev->internal.qos_mod_in_progress = false;
	spdk_spin_unlock(&ctx->bdev->internal.spinlock);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx);
}

static void
bdev_disable_qos_done(void *cb_arg)
{
	struct set_qos_limit_ctx *ctx = cb_arg;
	struct spdk_bdev *bdev = ctx->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_qos *qos;

	spdk_spin_lock(&bdev->internal.spinlock);
	qos = bdev->internal.qos;
	bdev->internal.qos = NULL;
	spdk_spin_unlock(&bdev->internal.spinlock);

	while (!TAILQ_EMPTY(&qos->queued)) {
		/* Send queued I/O back to their original thread for resubmission. */
		bdev_io = TAILQ_FIRST(&qos->queued);
		TAILQ_REMOVE(&qos->queued, bdev_io, internal.link);

		if (bdev_io->internal.io_submit_ch) {
			/*
			 * Channel was changed when sending it to the QoS thread - change it back
			 *  before sending it back to the original thread.
			 */
			bdev_io->internal.ch = bdev_io->internal.io_submit_ch;
			bdev_io->internal.io_submit_ch = NULL;
		}

		spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
				     _bdev_io_submit, bdev_io);
	}

	if (qos->thread != NULL) {
		spdk_put_io_channel(spdk_io_channel_from_ctx(qos->ch));
		spdk_poller_unregister(&qos->poller);
	}

	free(qos);

	bdev_set_qos_limit_done(ctx, 0);
}

static void
bdev_disable_qos_msg_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct set_qos_limit_ctx *ctx = _ctx;
	struct spdk_thread *thread;

	spdk_spin_lock(&bdev->internal.spinlock);
	thread = bdev->internal.qos->thread;
	spdk_spin_unlock(&bdev->internal.spinlock);

	if (thread != NULL) {
		spdk_thread_send_msg(thread, bdev_disable_qos_done, ctx);
	} else {
		bdev_disable_qos_done(ctx);
	}
}

static void
bdev_disable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		     struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);

	bdev_ch->flags &= ~BDEV_CH_QOS_ENABLED;

	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_update_qos_rate_limit_msg(void *cb_arg)
{
	struct set_qos_limit_ctx *ctx = cb_arg;
	struct spdk_bdev *bdev = ctx->bdev;

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_qos_update_max_quota_per_timeslice(bdev->internal.qos);
	spdk_spin_unlock(&bdev->internal.spinlock);

	bdev_set_qos_limit_done(ctx, 0);
}

static void
bdev_enable_qos_msg(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
		    struct spdk_io_channel *ch, void *_ctx)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);

	spdk_spin_lock(&bdev->internal.spinlock);
	bdev_enable_qos(bdev, bdev_ch);
	spdk_spin_unlock(&bdev->internal.spinlock);
	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_enable_qos_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct set_qos_limit_ctx *ctx = _ctx;

	bdev_set_qos_limit_done(ctx, status);
}

static void
bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits)
{
	int i;

	assert(bdev->internal.qos != NULL);

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] != SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			bdev->internal.qos->rate_limits[i].limit = limits[i];

			if (limits[i] == 0) {
				bdev->internal.qos->rate_limits[i].limit =
					SPDK_BDEV_QOS_LIMIT_NOT_DEFINED;
			}
		}
	}
}

void
spdk_bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits,
			      void (*cb_fn)(void *cb_arg, int status), void *cb_arg)
{
	struct set_qos_limit_ctx	*ctx;
	uint32_t			limit_set_complement;
	uint64_t			min_limit_per_sec;
	int				i;
	bool				disable_rate_limit = true;

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (limits[i] == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED) {
			continue;
		}

		if (limits[i] > 0) {
			disable_rate_limit = false;
		}

		if (bdev_qos_is_iops_rate_limit(i) == true) {
			min_limit_per_sec = SPDK_BDEV_QOS_MIN_IOS_PER_SEC;
		} else {
			/* Change from megabyte to byte rate limit */
			limits[i] = limits[i] * 1024 * 1024;
			min_limit_per_sec = SPDK_BDEV_QOS_MIN_BYTES_PER_SEC;
		}

		limit_set_complement = limits[i] % min_limit_per_sec;
		if (limit_set_complement) {
			SPDK_ERRLOG("Requested rate limit %" PRIu64 " is not a multiple of %" PRIu64 "\n",
				    limits[i], min_limit_per_sec);
			limits[i] += min_limit_per_sec - limit_set_complement;
			SPDK_ERRLOG("Round up the rate limit to %" PRIu64 "\n", limits[i]);
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->bdev = bdev;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.qos_mod_in_progress) {
		spdk_spin_unlock(&bdev->internal.spinlock);
		free(ctx);
		cb_fn(cb_arg, -EAGAIN);
		return;
	}
	bdev->internal.qos_mod_in_progress = true;

	if (disable_rate_limit == true && bdev->internal.qos) {
		for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
			if (limits[i] == SPDK_BDEV_QOS_LIMIT_NOT_DEFINED &&
			    (bdev->internal.qos->rate_limits[i].limit > 0 &&
			     bdev->internal.qos->rate_limits[i].limit !=
			     SPDK_BDEV_QOS_LIMIT_NOT_DEFINED)) {
				disable_rate_limit = false;
				break;
			}
		}
	}

	if (disable_rate_limit == false) {
		if (bdev->internal.qos == NULL) {
			bdev->internal.qos = calloc(1, sizeof(*bdev->internal.qos));
			if (!bdev->internal.qos) {
				spdk_spin_unlock(&bdev->internal.spinlock);
				SPDK_ERRLOG("Unable to allocate memory for QoS tracking\n");
				bdev_set_qos_limit_done(ctx, -ENOMEM);
				return;
			}
		}

		if (bdev->internal.qos->thread == NULL) {
			/* Enabling */
			bdev_set_qos_rate_limits(bdev, limits);

			spdk_bdev_for_each_channel(bdev, bdev_enable_qos_msg, ctx,
						   bdev_enable_qos_done);
		} else {
			/* Updating */
			bdev_set_qos_rate_limits(bdev, limits);

			spdk_thread_send_msg(bdev->internal.qos->thread,
					     bdev_update_qos_rate_limit_msg, ctx);
		}
	} else {
		if (bdev->internal.qos != NULL) {
			bdev_set_qos_rate_limits(bdev, limits);

			/* Disabling */
			spdk_bdev_for_each_channel(bdev, bdev_disable_qos_msg, ctx,
						   bdev_disable_qos_msg_done);
		} else {
			spdk_spin_unlock(&bdev->internal.spinlock);
			bdev_set_qos_limit_done(ctx, 0);
			return;
		}
	}

	spdk_spin_unlock(&bdev->internal.spinlock);
}

struct spdk_bdev_histogram_ctx {
	spdk_bdev_histogram_status_cb cb_fn;
	void *cb_arg;
	struct spdk_bdev *bdev;
	int status;
};

static void
bdev_histogram_disable_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_ctx *ctx = _ctx;

	spdk_spin_lock(&ctx->bdev->internal.spinlock);
	ctx->bdev->internal.histogram_in_progress = false;
	spdk_spin_unlock(&ctx->bdev->internal.spinlock);
	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

static void
bdev_histogram_disable_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			       struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);

	if (ch->histogram != NULL) {
		spdk_histogram_data_free(ch->histogram);
		ch->histogram = NULL;
	}
	spdk_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_histogram_enable_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_ctx *ctx = _ctx;

	if (status != 0) {
		ctx->status = status;
		ctx->bdev->internal.histogram_enabled = false;
		spdk_bdev_for_each_channel(ctx->bdev, bdev_histogram_disable_channel, ctx,
					   bdev_histogram_disable_channel_cb);
	} else {
		spdk_spin_lock(&ctx->bdev->internal.spinlock);
		ctx->bdev->internal.histogram_in_progress = false;
		spdk_spin_unlock(&ctx->bdev->internal.spinlock);
		ctx->cb_fn(ctx->cb_arg, ctx->status);
		free(ctx);
	}
}

static void
bdev_histogram_enable_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			      struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	int status = 0;

	if (ch->histogram == NULL) {
		ch->histogram = spdk_histogram_data_alloc();
		if (ch->histogram == NULL) {
			status = -ENOMEM;
		}
	}

	spdk_bdev_for_each_channel_continue(i, status);
}

void
spdk_bdev_histogram_enable(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
			   void *cb_arg, bool enable)
{
	struct spdk_bdev_histogram_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_bdev_histogram_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev = bdev;
	ctx->status = 0;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev->internal.histogram_in_progress) {
		spdk_spin_unlock(&bdev->internal.spinlock);
		free(ctx);
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	bdev->internal.histogram_in_progress = true;
	spdk_spin_unlock(&bdev->internal.spinlock);

	bdev->internal.histogram_enabled = enable;

	if (enable) {
		/* Allocate histogram for each channel */
		spdk_bdev_for_each_channel(bdev, bdev_histogram_enable_channel, ctx,
					   bdev_histogram_enable_channel_cb);
	} else {
		spdk_bdev_for_each_channel(bdev, bdev_histogram_disable_channel, ctx,
					   bdev_histogram_disable_channel_cb);
	}
}

struct spdk_bdev_histogram_data_ctx {
	spdk_bdev_histogram_data_cb cb_fn;
	void *cb_arg;
	struct spdk_bdev *bdev;
	/** merged histogram data from all channels */
	struct spdk_histogram_data	*histogram;
};

static void
bdev_histogram_get_channel_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_histogram_data_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, status, ctx->histogram);
	free(ctx);
}

static void
bdev_histogram_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			   struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct spdk_bdev_histogram_data_ctx *ctx = _ctx;
	int status = 0;

	if (ch->histogram == NULL) {
		status = -EFAULT;
	} else {
		spdk_histogram_data_merge(ctx->histogram, ch->histogram);
	}

	spdk_bdev_for_each_channel_continue(i, status);
}

void
spdk_bdev_histogram_get(struct spdk_bdev *bdev, struct spdk_histogram_data *histogram,
			spdk_bdev_histogram_data_cb cb_fn,
			void *cb_arg)
{
	struct spdk_bdev_histogram_data_ctx *ctx;

	ctx = calloc(1, sizeof(struct spdk_bdev_histogram_data_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM, NULL);
		return;
	}

	ctx->bdev = bdev;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	ctx->histogram = histogram;

	spdk_bdev_for_each_channel(bdev, bdev_histogram_get_channel, ctx,
				   bdev_histogram_get_channel_cb);
}

void
spdk_bdev_channel_get_histogram(struct spdk_io_channel *ch, spdk_bdev_histogram_data_cb cb_fn,
				void *cb_arg)
{
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(ch);
	int status = 0;

	assert(cb_fn != NULL);

	if (bdev_ch->histogram == NULL) {
		status = -EFAULT;
	}
	cb_fn(cb_arg, status, bdev_ch->histogram);
}

size_t
spdk_bdev_get_media_events(struct spdk_bdev_desc *desc, struct spdk_bdev_media_event *events,
			   size_t max_events)
{
	struct media_event_entry *entry;
	size_t num_events = 0;

	for (; num_events < max_events; ++num_events) {
		entry = TAILQ_FIRST(&desc->pending_media_events);
		if (entry == NULL) {
			break;
		}

		events[num_events] = entry->event;
		TAILQ_REMOVE(&desc->pending_media_events, entry, tailq);
		TAILQ_INSERT_TAIL(&desc->free_media_events, entry, tailq);
	}

	return num_events;
}

int
spdk_bdev_push_media_events(struct spdk_bdev *bdev, const struct spdk_bdev_media_event *events,
			    size_t num_events)
{
	struct spdk_bdev_desc *desc;
	struct media_event_entry *entry;
	size_t event_id;
	int rc = 0;

	assert(bdev->media_events);

	spdk_spin_lock(&bdev->internal.spinlock);
	TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
		if (desc->write) {
			break;
		}
	}

	if (desc == NULL || desc->media_events_buffer == NULL) {
		rc = -ENODEV;
		goto out;
	}

	for (event_id = 0; event_id < num_events; ++event_id) {
		entry = TAILQ_FIRST(&desc->free_media_events);
		if (entry == NULL) {
			break;
		}

		TAILQ_REMOVE(&desc->free_media_events, entry, tailq);
		TAILQ_INSERT_TAIL(&desc->pending_media_events, entry, tailq);
		entry->event = events[event_id];
	}

	rc = event_id;
out:
	spdk_spin_unlock(&bdev->internal.spinlock);
	return rc;
}

void
spdk_bdev_notify_media_management(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;

	spdk_spin_lock(&bdev->internal.spinlock);
	TAILQ_FOREACH(desc, &bdev->internal.open_descs, link) {
		if (!TAILQ_EMPTY(&desc->pending_media_events)) {
			desc->callback.event_fn(SPDK_BDEV_EVENT_MEDIA_MANAGEMENT, bdev,
						desc->callback.ctx);
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
}

struct locked_lba_range_ctx {
	struct lba_range		range;
	struct spdk_bdev		*bdev;
	struct lba_range		*current_range;
	struct lba_range		*owner_range;
	struct spdk_poller		*poller;
	lock_range_cb			cb_fn;
	void				*cb_arg;
};

static void
bdev_lock_error_cleanup_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, -ENOMEM);
	free(ctx);
}

static void bdev_unlock_lba_range_get_channel(struct spdk_bdev_channel_iter *i,
		struct spdk_bdev *bdev, struct spdk_io_channel *ch, void *_ctx);

static void
bdev_lock_lba_range_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	if (status == -ENOMEM) {
		/* One of the channels could not allocate a range object.
		 * So we have to go back and clean up any ranges that were
		 * allocated successfully before we return error status to
		 * the caller.  We can reuse the unlock function to do that
		 * clean up.
		 */
		spdk_bdev_for_each_channel(bdev, bdev_unlock_lba_range_get_channel, ctx,
					   bdev_lock_error_cleanup_cb);
		return;
	}

	/* All channels have locked this range and no I/O overlapping the range
	 * are outstanding!  Set the owner_ch for the range object for the
	 * locking channel, so that this channel will know that it is allowed
	 * to write to this range.
	 */
	ctx->owner_range->owner_ch = ctx->range.owner_ch;
	ctx->cb_fn(ctx->cb_arg, status);

	/* Don't free the ctx here.  Its range is in the bdev's global list of
	 * locked ranges still, and will be removed and freed when this range
	 * is later unlocked.
	 */
}

static int
bdev_lock_lba_range_check_io(void *_i)
{
	struct spdk_bdev_channel_iter *i = _i;
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i->i);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = i->ctx;
	struct lba_range *range = ctx->current_range;
	struct spdk_bdev_io *bdev_io;

	spdk_poller_unregister(&ctx->poller);

	/* The range is now in the locked_ranges, so no new IO can be submitted to this
	 * range.  But we need to wait until any outstanding IO overlapping with this range
	 * are completed.
	 */
	TAILQ_FOREACH(bdev_io, &ch->io_submitted, internal.ch_link) {
		if (bdev_io_range_is_locked(bdev_io, range)) {
			ctx->poller = SPDK_POLLER_REGISTER(bdev_lock_lba_range_check_io, i, 100);
			return SPDK_POLLER_BUSY;
		}
	}

	spdk_bdev_for_each_channel_continue(i, 0);
	return SPDK_POLLER_BUSY;
}

static void
bdev_lock_lba_range_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = _ctx;
	struct lba_range *range;

	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (range->length == ctx->range.length &&
		    range->offset == ctx->range.offset &&
		    range->locked_ctx == ctx->range.locked_ctx) {
			/* This range already exists on this channel, so don't add
			 * it again.  This can happen when a new channel is created
			 * while the for_each_channel operation is in progress.
			 * Do not check for outstanding I/O in that case, since the
			 * range was locked before any I/O could be submitted to the
			 * new channel.
			 */
			spdk_bdev_for_each_channel_continue(i, 0);
			return;
		}
	}

	range = calloc(1, sizeof(*range));
	if (range == NULL) {
		spdk_bdev_for_each_channel_continue(i, -ENOMEM);
		return;
	}

	range->length = ctx->range.length;
	range->offset = ctx->range.offset;
	range->locked_ctx = ctx->range.locked_ctx;
	ctx->current_range = range;
	if (ctx->range.owner_ch == ch) {
		/* This is the range object for the channel that will hold
		 * the lock.  Store it in the ctx object so that we can easily
		 * set its owner_ch after the lock is finally acquired.
		 */
		ctx->owner_range = range;
	}
	TAILQ_INSERT_TAIL(&ch->locked_ranges, range, tailq);
	bdev_lock_lba_range_check_io(i);
}

static void
bdev_lock_lba_range_ctx(struct spdk_bdev *bdev, struct locked_lba_range_ctx *ctx)
{
	assert(spdk_get_thread() == spdk_io_channel_get_thread(ctx->range.owner_ch->channel));

	/* We will add a copy of this range to each channel now. */
	spdk_bdev_for_each_channel(bdev, bdev_lock_lba_range_get_channel, ctx,
				   bdev_lock_lba_range_cb);
}

static bool
bdev_lba_range_overlaps_tailq(struct lba_range *range, lba_range_tailq_t *tailq)
{
	struct lba_range *r;

	TAILQ_FOREACH(r, tailq, tailq) {
		if (bdev_lba_range_overlapped(range, r)) {
			return true;
		}
	}
	return false;
}

static int
bdev_lock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
		    uint64_t offset, uint64_t length,
		    lock_range_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx;

	if (cb_arg == NULL) {
		SPDK_ERRLOG("cb_arg must not be NULL\n");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->range.offset = offset;
	ctx->range.length = length;
	ctx->range.owner_ch = ch;
	ctx->range.locked_ctx = cb_arg;
	ctx->bdev = bdev;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_spin_lock(&bdev->internal.spinlock);
	if (bdev_lba_range_overlaps_tailq(&ctx->range, &bdev->internal.locked_ranges)) {
		/* There is an active lock overlapping with this range.
		 * Put it on the pending list until this range no
		 * longer overlaps with another.
		 */
		TAILQ_INSERT_TAIL(&bdev->internal.pending_locked_ranges, &ctx->range, tailq);
	} else {
		TAILQ_INSERT_TAIL(&bdev->internal.locked_ranges, &ctx->range, tailq);
		bdev_lock_lba_range_ctx(bdev, ctx);
	}
	spdk_spin_unlock(&bdev->internal.spinlock);
	return 0;
}

static void
bdev_lock_lba_range_ctx_msg(void *_ctx)
{
	struct locked_lba_range_ctx *ctx = _ctx;

	bdev_lock_lba_range_ctx(ctx->bdev, ctx);
}

static void
bdev_unlock_lba_range_cb(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct locked_lba_range_ctx *ctx = _ctx;
	struct locked_lba_range_ctx *pending_ctx;
	struct lba_range *range, *tmp;

	spdk_spin_lock(&bdev->internal.spinlock);
	/* Check if there are any pending locked ranges that overlap with this range
	 * that was just unlocked.  If there are, check that it doesn't overlap with any
	 * other locked ranges before calling bdev_lock_lba_range_ctx which will start
	 * the lock process.
	 */
	TAILQ_FOREACH_SAFE(range, &bdev->internal.pending_locked_ranges, tailq, tmp) {
		if (bdev_lba_range_overlapped(range, &ctx->range) &&
		    !bdev_lba_range_overlaps_tailq(range, &bdev->internal.locked_ranges)) {
			TAILQ_REMOVE(&bdev->internal.pending_locked_ranges, range, tailq);
			pending_ctx = SPDK_CONTAINEROF(range, struct locked_lba_range_ctx, range);
			TAILQ_INSERT_TAIL(&bdev->internal.locked_ranges, range, tailq);
			spdk_thread_send_msg(spdk_io_channel_get_thread(pending_ctx->range.owner_ch->channel),
					     bdev_lock_lba_range_ctx_msg, pending_ctx);
		}
	}
	spdk_spin_unlock(&bdev->internal.spinlock);

	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}

static void
bdev_unlock_lba_range_get_channel(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
				  struct spdk_io_channel *_ch, void *_ctx)
{
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx = _ctx;
	TAILQ_HEAD(, spdk_bdev_io) io_locked;
	struct spdk_bdev_io *bdev_io;
	struct lba_range *range;

	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (ctx->range.offset == range->offset &&
		    ctx->range.length == range->length &&
		    ctx->range.locked_ctx == range->locked_ctx) {
			TAILQ_REMOVE(&ch->locked_ranges, range, tailq);
			free(range);
			break;
		}
	}

	/* Note: we should almost always be able to assert that the range specified
	 * was found.  But there are some very rare corner cases where a new channel
	 * gets created simultaneously with a range unlock, where this function
	 * would execute on that new channel and wouldn't have the range.
	 * We also use this to clean up range allocations when a later allocation
	 * fails in the locking path.
	 * So we can't actually assert() here.
	 */

	/* Swap the locked IO into a temporary list, and then try to submit them again.
	 * We could hyper-optimize this to only resubmit locked I/O that overlap
	 * with the range that was just unlocked, but this isn't a performance path so
	 * we go for simplicity here.
	 */
	TAILQ_INIT(&io_locked);
	TAILQ_SWAP(&ch->io_locked, &io_locked, spdk_bdev_io, internal.ch_link);
	while (!TAILQ_EMPTY(&io_locked)) {
		bdev_io = TAILQ_FIRST(&io_locked);
		TAILQ_REMOVE(&io_locked, bdev_io, internal.ch_link);
		bdev_io_submit(bdev_io);
	}

	spdk_bdev_for_each_channel_continue(i, 0);
}

static int
bdev_unlock_lba_range(struct spdk_bdev_desc *desc, struct spdk_io_channel *_ch,
		      uint64_t offset, uint64_t length,
		      lock_range_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_channel *ch = __io_ch_to_bdev_ch(_ch);
	struct locked_lba_range_ctx *ctx;
	struct lba_range *range;
	bool range_found = false;

	/* Let's make sure the specified channel actually has a lock on
	 * the specified range.  Note that the range must match exactly.
	 */
	TAILQ_FOREACH(range, &ch->locked_ranges, tailq) {
		if (range->offset == offset && range->length == length &&
		    range->owner_ch == ch && range->locked_ctx == cb_arg) {
			range_found = true;
			break;
		}
	}

	if (!range_found) {
		return -EINVAL;
	}

	spdk_spin_lock(&bdev->internal.spinlock);
	/* We confirmed that this channel has locked the specified range.  To
	 * start the unlock the process, we find the range in the bdev's locked_ranges
	 * and remove it.  This ensures new channels don't inherit the locked range.
	 * Then we will send a message to each channel (including the one specified
	 * here) to remove the range from its per-channel list.
	 */
	TAILQ_FOREACH(range, &bdev->internal.locked_ranges, tailq) {
		if (range->offset == offset && range->length == length &&
		    range->locked_ctx == cb_arg) {
			break;
		}
	}
	if (range == NULL) {
		assert(false);
		spdk_spin_unlock(&bdev->internal.spinlock);
		return -EINVAL;
	}
	TAILQ_REMOVE(&bdev->internal.locked_ranges, range, tailq);
	ctx = SPDK_CONTAINEROF(range, struct locked_lba_range_ctx, range);
	spdk_spin_unlock(&bdev->internal.spinlock);

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_bdev_for_each_channel(bdev, bdev_unlock_lba_range_get_channel, ctx,
				   bdev_unlock_lba_range_cb);
	return 0;
}

int
spdk_bdev_get_memory_domains(struct spdk_bdev *bdev, struct spdk_memory_domain **domains,
			     int array_size)
{
	if (!bdev) {
		return -EINVAL;
	}

	if (bdev->fn_table->get_memory_domains) {
		return bdev->fn_table->get_memory_domains(bdev->ctxt, domains, array_size);
	}

	return 0;
}

struct spdk_bdev_for_each_io_ctx {
	void *ctx;
	spdk_bdev_io_fn fn;
	spdk_bdev_for_each_io_cb cb;
};

static void
bdev_channel_for_each_io(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			 struct spdk_io_channel *io_ch, void *_ctx)
{
	struct spdk_bdev_for_each_io_ctx *ctx = _ctx;
	struct spdk_bdev_channel *bdev_ch = __io_ch_to_bdev_ch(io_ch);
	struct spdk_bdev_io *bdev_io;
	int rc = 0;

	TAILQ_FOREACH(bdev_io, &bdev_ch->io_submitted, internal.ch_link) {
		rc = ctx->fn(ctx->ctx, bdev_io);
		if (rc != 0) {
			break;
		}
	}

	spdk_bdev_for_each_channel_continue(i, rc);
}

static void
bdev_for_each_io_done(struct spdk_bdev *bdev, void *_ctx, int status)
{
	struct spdk_bdev_for_each_io_ctx *ctx = _ctx;

	ctx->cb(ctx->ctx, status);

	free(ctx);
}

void
spdk_bdev_for_each_bdev_io(struct spdk_bdev *bdev, void *_ctx, spdk_bdev_io_fn fn,
			   spdk_bdev_for_each_io_cb cb)
{
	struct spdk_bdev_for_each_io_ctx *ctx;

	assert(fn != NULL && cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate context.\n");
		cb(_ctx, -ENOMEM);
		return;
	}

	ctx->ctx = _ctx;
	ctx->fn = fn;
	ctx->cb = cb;

	spdk_bdev_for_each_channel(bdev, bdev_channel_for_each_io, ctx,
				   bdev_for_each_io_done);
}

void
spdk_bdev_for_each_channel_continue(struct spdk_bdev_channel_iter *iter, int status)
{
	spdk_for_each_channel_continue(iter->i, status);
}

static struct spdk_bdev *
io_channel_iter_get_bdev(struct spdk_io_channel_iter *i)
{
	void *io_device = spdk_io_channel_iter_get_io_device(i);

	return __bdev_from_io_dev(io_device);
}

static void
bdev_each_channel_msg(struct spdk_io_channel_iter *i)
{
	struct spdk_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev *bdev = io_channel_iter_get_bdev(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);

	iter->i = i;
	iter->fn(iter, bdev, ch, iter->ctx);
}

static void
bdev_each_channel_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev *bdev = io_channel_iter_get_bdev(i);

	iter->i = i;
	iter->cpl(bdev, iter->ctx, status);

	free(iter);
}

void
spdk_bdev_for_each_channel(struct spdk_bdev *bdev, spdk_bdev_for_each_channel_msg fn,
			   void *ctx, spdk_bdev_for_each_channel_done cpl)
{
	struct spdk_bdev_channel_iter *iter;

	assert(bdev != NULL && fn != NULL && ctx != NULL);

	iter = calloc(1, sizeof(struct spdk_bdev_channel_iter));
	if (iter == NULL) {
		SPDK_ERRLOG("Unable to allocate iterator\n");
		assert(false);
		return;
	}

	iter->fn = fn;
	iter->cpl = cpl;
	iter->ctx = ctx;

	spdk_for_each_channel(__bdev_to_io_dev(bdev), bdev_each_channel_msg,
			      iter, bdev_each_channel_cpl);
}

int
spdk_bdev_copy_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t dst_offset_blocks, uint64_t src_offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (spdk_unlikely(!bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COPY))) {
		SPDK_DEBUGLOG(bdev, "Copy IO type is not supported\n");
		return -ENOTSUP;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Can't copy 0 blocks\n");
		return -EINVAL;
	}

	if (!bdev_io_valid_blocks(bdev, dst_offset_blocks, num_blocks) ||
	    !bdev_io_valid_blocks(bdev, src_offset_blocks, num_blocks)) {
		SPDK_DEBUGLOG(bdev,
			      "Invalid offset or number of blocks: dst %lu, src %lu, count %lu\n",
			      dst_offset_blocks, src_offset_blocks, num_blocks);
		return -EINVAL;
	}

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_COPY;

	bdev_io->u.bdev.offset_blocks = dst_offset_blocks;
	bdev_io->u.bdev.copy.src_offset_blocks = src_offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.ext_opts = NULL;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(bdev)

SPDK_TRACE_REGISTER_FN(bdev_trace, "bdev", TRACE_GROUP_BDEV)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"BDEV_IO_START", TRACE_BDEV_IO_START,
			OWNER_BDEV, OBJECT_BDEV_IO, 1,
			{
				{ "type", SPDK_TRACE_ARG_TYPE_INT, 8 },
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "offset", SPDK_TRACE_ARG_TYPE_INT, 8 },
				{ "len", SPDK_TRACE_ARG_TYPE_INT, 8 },
				{ "name", SPDK_TRACE_ARG_TYPE_STR, 40}
			}
		},
		{
			"BDEV_IO_DONE", TRACE_BDEV_IO_DONE,
			OWNER_BDEV, OBJECT_BDEV_IO, 0,
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
		},
		{
			"BDEV_IOCH_CREATE", TRACE_BDEV_IOCH_CREATE,
			OWNER_BDEV, OBJECT_NONE, 1,
			{
				{ "name", SPDK_TRACE_ARG_TYPE_STR, 40 },
				{ "thread_id", SPDK_TRACE_ARG_TYPE_INT, 8}
			}
		},
		{
			"BDEV_IOCH_DESTROY", TRACE_BDEV_IOCH_DESTROY,
			OWNER_BDEV, OBJECT_NONE, 0,
			{
				{ "name", SPDK_TRACE_ARG_TYPE_STR, 40 },
				{ "thread_id", SPDK_TRACE_ARG_TYPE_INT, 8}
			}
		},
	};


	spdk_trace_register_owner(OWNER_BDEV, 'b');
	spdk_trace_register_object(OBJECT_BDEV_IO, 'i');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_tpoint_register_relation(TRACE_BDEV_NVME_IO_START, OBJECT_BDEV_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_NVME_IO_DONE, OBJECT_BDEV_IO, 0);
}
