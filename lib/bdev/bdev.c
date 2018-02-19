/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/scsi_spec.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"

#ifdef SPDK_CONFIG_VTUNE
#include "ittnotify.h"
#include "ittnotify_types.h"
int __itt_init_ittlib(const char *, __itt_group_id);
#endif

#define SPDK_BDEV_IO_POOL_SIZE	(64 * 1024)
#define SPDK_BDEV_IO_CACHE_SIZE	256
#define BUF_SMALL_POOL_SIZE	8192
#define BUF_LARGE_POOL_SIZE	1024
#define NOMEM_THRESHOLD_COUNT	8
#define ZERO_BUFFER_SIZE	0x100000

typedef TAILQ_HEAD(, spdk_bdev_io) bdev_io_tailq_t;
typedef STAILQ_HEAD(, spdk_bdev_io) bdev_io_stailq_t;

struct spdk_bdev_mgr {
	struct spdk_mempool *bdev_io_pool;

	struct spdk_mempool *buf_small_pool;
	struct spdk_mempool *buf_large_pool;

	void *zero_buffer;

	TAILQ_HEAD(, spdk_bdev_module_if) bdev_modules;

	TAILQ_HEAD(, spdk_bdev) bdevs;

	bool init_complete;
	bool module_init_complete;

#ifdef SPDK_CONFIG_VTUNE
	__itt_domain	*domain;
#endif
};

static struct spdk_bdev_mgr g_bdev_mgr = {
	.bdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdev_modules),
	.bdevs = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdevs),
	.init_complete = false,
	.module_init_complete = false,
};

static spdk_bdev_init_cb	g_init_cb_fn = NULL;
static void			*g_init_cb_arg = NULL;

static spdk_bdev_fini_cb	g_fini_cb_fn = NULL;
static void			*g_fini_cb_arg = NULL;
static struct spdk_thread	*g_fini_thread = NULL;


struct spdk_bdev_mgmt_channel {
	bdev_io_stailq_t need_buf_small;
	bdev_io_stailq_t need_buf_large;

	/*
	 * Each thread keeps a cache of bdev_io - this allows
	 *  bdev threads which are *not* DPDK threads to still
	 *  benefit from a per-thread bdev_io cache.  Without
	 *  this, non-DPDK threads fetching from the mempool
	 *  incur a cmpxchg on get and put.
	 */
	bdev_io_stailq_t per_thread_cache;
	uint32_t	per_thread_cache_count;

	TAILQ_HEAD(, spdk_bdev_module_channel) module_channels;
};

struct spdk_bdev_desc {
	struct spdk_bdev		*bdev;
	spdk_bdev_remove_cb_t		remove_cb;
	void				*remove_ctx;
	bool				write;
	TAILQ_ENTRY(spdk_bdev_desc)	link;
};

#define BDEV_CH_RESET_IN_PROGRESS	(1 << 0)

struct spdk_bdev_channel {
	struct spdk_bdev	*bdev;

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;

	/* Channel for the bdev manager */
	struct spdk_io_channel *mgmt_channel;

	struct spdk_bdev_io_stat stat;

	bdev_io_tailq_t		queued_resets;

	uint32_t		flags;

	/* Per-device channel */
	struct spdk_bdev_module_channel *module_ch;

#ifdef SPDK_CONFIG_VTUNE
	uint64_t		start_tsc;
	uint64_t		interval_tsc;
	__itt_string_handle	*handle;
#endif

};

#define __bdev_to_io_dev(bdev)		(((char *)bdev) + 1)
#define __bdev_from_io_dev(io_dev)	((struct spdk_bdev *)(((char *)io_dev) - 1))

/*
 * Per-module (or per-io_device) channel. Multiple bdevs built on the same io_device
 * will queue here their IO that awaits retry. It makes it posible to retry sending
 * IO to one bdev after IO from other bdev completes.
 */
struct spdk_bdev_module_channel {
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
	struct spdk_io_channel	*module_ch;

	uint32_t		ref;

	TAILQ_ENTRY(spdk_bdev_module_channel) link;
};

static void spdk_bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

struct spdk_bdev *
spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	if (bdev) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_NEXT(prev, link);
	if (bdev) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

static struct spdk_bdev *
_bdev_next_leaf(struct spdk_bdev *bdev)
{
	while (bdev != NULL) {
		if (TAILQ_EMPTY(&bdev->vbdevs)) {
			return bdev;
		} else {
			bdev = TAILQ_NEXT(bdev, link);
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
		SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next_leaf(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = _bdev_next_leaf(TAILQ_NEXT(prev, link));

	if (bdev) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev_alias *tmp;
	struct spdk_bdev *bdev = spdk_bdev_first();

	while (bdev != NULL) {
		if (strcmp(bdev_name, bdev->name) == 0) {
			return bdev;
		}

		TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
			if (strcmp(bdev_name, tmp->alias) == 0) {
				return bdev;
			}
		}

		bdev = spdk_bdev_next(bdev);
	}

	return NULL;
}

static void
spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf)
{
	assert(bdev_io->get_buf_cb != NULL);
	assert(buf != NULL);
	assert(bdev_io->u.bdev.iovs != NULL);

	bdev_io->buf = buf;
	bdev_io->u.bdev.iovs[0].iov_base = (void *)((unsigned long)((char *)buf + 512) & ~511UL);
	bdev_io->u.bdev.iovs[0].iov_len = bdev_io->buf_len;
	bdev_io->get_buf_cb(bdev_io->ch->channel, bdev_io);
}

static void
spdk_bdev_io_put_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_mempool *pool;
	struct spdk_bdev_io *tmp;
	void *buf;
	bdev_io_stailq_t *stailq;
	struct spdk_bdev_mgmt_channel *ch;

	assert(bdev_io->u.bdev.iovcnt == 1);

	buf = bdev_io->buf;
	ch = bdev_io->mgmt_ch;

	if (bdev_io->buf_len <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		stailq = &ch->need_buf_small;
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		stailq = &ch->need_buf_large;
	}

	if (STAILQ_EMPTY(stailq)) {
		spdk_mempool_put(pool, buf);
	} else {
		tmp = STAILQ_FIRST(stailq);
		STAILQ_REMOVE_HEAD(stailq, buf_link);
		spdk_bdev_io_set_buf(tmp, buf);
	}
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	struct spdk_mempool *pool;
	bdev_io_stailq_t *stailq;
	void *buf = NULL;
	struct spdk_bdev_mgmt_channel *ch;

	assert(cb != NULL);
	assert(bdev_io->u.bdev.iovs != NULL);

	if (spdk_unlikely(bdev_io->u.bdev.iovs[0].iov_base != NULL)) {
		/* Buffer already present */
		cb(bdev_io->ch->channel, bdev_io);
		return;
	}

	assert(len <= SPDK_BDEV_LARGE_BUF_MAX_SIZE);
	ch = spdk_io_channel_get_ctx(bdev_io->ch->mgmt_channel);

	bdev_io->buf_len = len;
	bdev_io->get_buf_cb = cb;
	if (len <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		stailq = &ch->need_buf_small;
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		stailq = &ch->need_buf_large;
	}

	buf = spdk_mempool_get(pool);

	if (!buf) {
		STAILQ_INSERT_TAIL(stailq, bdev_io, buf_link);
	} else {
		spdk_bdev_io_set_buf(bdev_io, buf);
	}
}

static int
spdk_bdev_module_get_max_ctx_size(void)
{
	struct spdk_bdev_module_if *bdev_module;
	int max_bdev_module_size = 0;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
}

void
spdk_bdev_config_text(FILE *fp)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
}

static int
spdk_bdev_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;

	STAILQ_INIT(&ch->need_buf_small);
	STAILQ_INIT(&ch->need_buf_large);

	STAILQ_INIT(&ch->per_thread_cache);
	ch->per_thread_cache_count = 0;

	TAILQ_INIT(&ch->module_channels);

	return 0;
}

static void
spdk_bdev_mgmt_channel_free_resources(struct spdk_bdev_mgmt_channel *ch)
{
	struct spdk_bdev_io *bdev_io;

	if (!STAILQ_EMPTY(&ch->need_buf_small) || !STAILQ_EMPTY(&ch->need_buf_large)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel free\n");
	}

	while (!STAILQ_EMPTY(&ch->per_thread_cache)) {
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, buf_link);
		ch->per_thread_cache_count--;
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
	}

	assert(ch->per_thread_cache_count == 0);
}

static void
spdk_bdev_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_mgmt_channel *ch = ctx_buf;

	spdk_bdev_mgmt_channel_free_resources(ch);
}

static void
spdk_bdev_init_complete(int rc)
{
	spdk_bdev_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;

	g_bdev_mgr.init_complete = true;
	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

static void
spdk_bdev_module_action_complete(void)
{
	struct spdk_bdev_module_if *m;

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
	TAILQ_FOREACH(m, &g_bdev_mgr.bdev_modules, tailq) {
		if (m->action_in_progress > 0) {
			return;
		}
	}

	/*
	 * Modules already finished initialization - now that all
	 * the bdev modules have finished their asynchronous I/O
	 * processing, the entire bdev layer can be marked as complete.
	 */
	spdk_bdev_init_complete(0);
}

static void
spdk_bdev_module_action_done(struct spdk_bdev_module_if *module)
{
	assert(module->action_in_progress > 0);
	module->action_in_progress--;
	spdk_bdev_module_action_complete();
}

void
spdk_bdev_module_init_done(struct spdk_bdev_module_if *module)
{
	spdk_bdev_module_action_done(module);
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module_if *module)
{
	spdk_bdev_module_action_done(module);
}

static int
spdk_bdev_modules_init(void)
{
	struct spdk_bdev_module_if *module;
	int rc = 0;

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, tailq) {
		rc = module->module_init();
		if (rc != 0) {
			break;
		}
	}

	g_bdev_mgr.module_init_complete = true;
	return rc;
}
void
spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg)
{
	int cache_size;
	int rc = 0;
	char mempool_name[32];

	assert(cb_fn != NULL);

	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;

	snprintf(mempool_name, sizeof(mempool_name), "bdev_io_%d", getpid());

	g_bdev_mgr.bdev_io_pool = spdk_mempool_create(mempool_name,
				  SPDK_BDEV_IO_POOL_SIZE,
				  sizeof(struct spdk_bdev_io) +
				  spdk_bdev_module_get_max_ctx_size(),
				  0,
				  SPDK_ENV_SOCKET_ID_ANY);

	if (g_bdev_mgr.bdev_io_pool == NULL) {
		SPDK_ERRLOG("could not allocate spdk_bdev_io pool\n");
		spdk_bdev_init_complete(-1);
		return;
	}

	/**
	 * Ensure no more than half of the total buffers end up local caches, by
	 *   using spdk_env_get_core_count() to determine how many local caches we need
	 *   to account for.
	 */
	cache_size = BUF_SMALL_POOL_SIZE / (2 * spdk_env_get_core_count());
	snprintf(mempool_name, sizeof(mempool_name), "buf_small_pool_%d", getpid());

	g_bdev_mgr.buf_small_pool = spdk_mempool_create(mempool_name,
				    BUF_SMALL_POOL_SIZE,
				    SPDK_BDEV_SMALL_BUF_MAX_SIZE + 512,
				    cache_size,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!g_bdev_mgr.buf_small_pool) {
		SPDK_ERRLOG("create rbuf small pool failed\n");
		spdk_bdev_init_complete(-1);
		return;
	}

	cache_size = BUF_LARGE_POOL_SIZE / (2 * spdk_env_get_core_count());
	snprintf(mempool_name, sizeof(mempool_name), "buf_large_pool_%d", getpid());

	g_bdev_mgr.buf_large_pool = spdk_mempool_create(mempool_name,
				    BUF_LARGE_POOL_SIZE,
				    SPDK_BDEV_LARGE_BUF_MAX_SIZE + 512,
				    cache_size,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!g_bdev_mgr.buf_large_pool) {
		SPDK_ERRLOG("create rbuf large pool failed\n");
		spdk_bdev_init_complete(-1);
		return;
	}

	g_bdev_mgr.zero_buffer = spdk_dma_zmalloc(ZERO_BUFFER_SIZE, ZERO_BUFFER_SIZE,
				 NULL);
	if (!g_bdev_mgr.zero_buffer) {
		SPDK_ERRLOG("create bdev zero buffer failed\n");
		spdk_bdev_init_complete(-1);
		return;
	}

#ifdef SPDK_CONFIG_VTUNE
	g_bdev_mgr.domain = __itt_domain_create("spdk_bdev");
#endif

	spdk_io_device_register(&g_bdev_mgr, spdk_bdev_mgmt_channel_create,
				spdk_bdev_mgmt_channel_destroy,
				sizeof(struct spdk_bdev_mgmt_channel));

	rc = spdk_bdev_modules_init();
	if (rc != 0) {
		SPDK_ERRLOG("bdev modules init failed\n");
		spdk_bdev_init_complete(-1);
		return;
	}

	spdk_bdev_module_action_complete();
}

static void
spdk_bdev_module_finish_cb(void *io_device)
{
	spdk_bdev_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

static void
spdk_bdev_module_finish_complete(struct spdk_io_channel_iter *i, int status)
{
	if (spdk_mempool_count(g_bdev_mgr.bdev_io_pool) != SPDK_BDEV_IO_POOL_SIZE) {
		SPDK_ERRLOG("bdev IO pool count is %zu but should be %u\n",
			    spdk_mempool_count(g_bdev_mgr.bdev_io_pool),
			    SPDK_BDEV_IO_POOL_SIZE);
	}

	if (spdk_mempool_count(g_bdev_mgr.buf_small_pool) != BUF_SMALL_POOL_SIZE) {
		SPDK_ERRLOG("Small buffer pool count is %zu but should be %u\n",
			    spdk_mempool_count(g_bdev_mgr.buf_small_pool),
			    BUF_SMALL_POOL_SIZE);
		assert(false);
	}

	if (spdk_mempool_count(g_bdev_mgr.buf_large_pool) != BUF_LARGE_POOL_SIZE) {
		SPDK_ERRLOG("Large buffer pool count is %zu but should be %u\n",
			    spdk_mempool_count(g_bdev_mgr.buf_large_pool),
			    BUF_LARGE_POOL_SIZE);
		assert(false);
	}

	spdk_mempool_free(g_bdev_mgr.bdev_io_pool);
	spdk_mempool_free(g_bdev_mgr.buf_small_pool);
	spdk_mempool_free(g_bdev_mgr.buf_large_pool);
	spdk_dma_free(g_bdev_mgr.zero_buffer);

	spdk_io_device_unregister(&g_bdev_mgr, spdk_bdev_module_finish_cb);
}

static void
mgmt_channel_free_resources(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_bdev_mgmt_channel *ch = spdk_io_channel_get_ctx(_ch);

	spdk_bdev_mgmt_channel_free_resources(ch);
	spdk_for_each_channel_continue(i, 0);
}

static void
spdk_bdev_module_finish_iter(void *arg)
{
	/* Notice that this variable is static. It is saved between calls to
	 * this function. */
	static struct spdk_bdev_module_if *resume_bdev_module = NULL;
	struct spdk_bdev_module_if *bdev_module;

	/* Start iterating from the last touched module */
	if (!resume_bdev_module) {
		bdev_module = TAILQ_FIRST(&g_bdev_mgr.bdev_modules);
	} else {
		bdev_module = TAILQ_NEXT(resume_bdev_module, tailq);
	}

	while (bdev_module) {
		if (bdev_module->async_fini) {
			/* Save our place so we can resume later. We must
			 * save the variable here, before calling module_fini()
			 * below, because in some cases the module may immediately
			 * call spdk_bdev_module_finish_done() and re-enter
			 * this function to continue iterating. */
			resume_bdev_module = bdev_module;
		}

		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}

		if (bdev_module->async_fini) {
			return;
		}

		bdev_module = TAILQ_NEXT(bdev_module, tailq);
	}

	resume_bdev_module = NULL;
	spdk_for_each_channel(&g_bdev_mgr, mgmt_channel_free_resources, NULL,
			      spdk_bdev_module_finish_complete);
}

void
spdk_bdev_module_finish_done(void)
{
	if (spdk_get_thread() != g_fini_thread) {
		spdk_thread_send_msg(g_fini_thread, spdk_bdev_module_finish_iter, NULL);
	} else {
		spdk_bdev_module_finish_iter(NULL);
	}
}

static void
_spdk_bdev_finish_unregister_bdevs_iter(void *cb_arg, int bdeverrno)
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
		TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, link);
	}

	if (TAILQ_EMPTY(&g_bdev_mgr.bdevs)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Done unregistering bdevs\n");
		spdk_bdev_module_finish_iter(NULL);
		return;
	}

	/*
	 * Unregister the first bdev in the list.
	 *
	 * spdk_bdev_unregister() will handle the case where the bdev has open descriptors by
	 *  calling the remove_cb of the descriptors first.
	 *
	 * Once this bdev and all of its open descriptors have been cleaned up, this function
	 *  will be called again via the unregister completion callback to continue the cleanup
	 *  process with the next bdev.
	 */
	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Unregistering bdev '%s'\n", bdev->name);
	spdk_bdev_unregister(bdev, _spdk_bdev_finish_unregister_bdevs_iter, bdev);
}

static void
_spdk_bdev_finish_unregister_bdevs(void)
{
	_spdk_bdev_finish_unregister_bdevs_iter(NULL, 0);
}

void
spdk_bdev_finish(spdk_bdev_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);

	g_fini_thread = spdk_get_thread();

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	_spdk_bdev_finish_unregister_bdevs();
}

static struct spdk_bdev_io *
spdk_bdev_get_io(struct spdk_io_channel *_ch)
{
	struct spdk_bdev_mgmt_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_io *bdev_io;

	if (ch->per_thread_cache_count > 0) {
		bdev_io = STAILQ_FIRST(&ch->per_thread_cache);
		STAILQ_REMOVE_HEAD(&ch->per_thread_cache, buf_link);
		ch->per_thread_cache_count--;
	} else {
		bdev_io = spdk_mempool_get(g_bdev_mgr.bdev_io_pool);
		if (!bdev_io) {
			SPDK_ERRLOG("Unable to get spdk_bdev_io\n");
			abort();
		}
	}

	bdev_io->mgmt_ch = ch;

	return bdev_io;
}

static void
spdk_bdev_put_io(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_mgmt_channel *ch = bdev_io->mgmt_ch;

	if (bdev_io->buf != NULL) {
		spdk_bdev_io_put_buf(bdev_io);
	}

	if (ch->per_thread_cache_count < SPDK_BDEV_IO_CACHE_SIZE) {
		ch->per_thread_cache_count++;
		STAILQ_INSERT_TAIL(&ch->per_thread_cache, bdev_io, buf_link);
	} else {
		spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
	}
}

static void
spdk_bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->ch;
	struct spdk_io_channel *ch = bdev_ch->channel;
	struct spdk_bdev_module_channel	*shared_ch = bdev_ch->module_ch;

	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);

	bdev_io->submit_tsc = spdk_get_ticks();
	shared_ch->io_outstanding++;
	bdev_io->in_submit_request = true;
	if (spdk_likely(bdev_ch->flags == 0)) {
		if (spdk_likely(TAILQ_EMPTY(&shared_ch->nomem_io))) {
			bdev->fn_table->submit_request(ch, bdev_io);
		} else {
			shared_ch->io_outstanding--;
			TAILQ_INSERT_TAIL(&shared_ch->nomem_io, bdev_io, link);
		}
	} else if (bdev_ch->flags & BDEV_CH_RESET_IN_PROGRESS) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		SPDK_ERRLOG("unknown bdev_ch flag %x found\n", bdev_ch->flags);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
	bdev_io->in_submit_request = false;
}

static void
spdk_bdev_io_submit_reset(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->ch;
	struct spdk_io_channel *ch = bdev_ch->channel;

	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);

	bdev_io->in_submit_request = true;
	bdev->fn_table->submit_request(ch, bdev_io);
	bdev_io->in_submit_request = false;
}

static void
spdk_bdev_io_init(struct spdk_bdev_io *bdev_io,
		  struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	bdev_io->caller_ctx = cb_arg;
	bdev_io->cb = cb;
	bdev_io->status = SPDK_BDEV_IO_STATUS_PENDING;
	bdev_io->in_submit_request = false;
	bdev_io->buf = NULL;
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	return bdev->fn_table->io_type_supported(bdev->ctxt, io_type);
}

int
spdk_bdev_dump_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	if (bdev->fn_table->dump_config_json) {
		return bdev->fn_table->dump_config_json(bdev->ctxt, w);
	}

	return 0;
}

static int
_spdk_bdev_channel_create(struct spdk_bdev_channel *ch, void *io_device)
{
	struct spdk_bdev		*bdev = __bdev_from_io_dev(io_device);
	struct spdk_bdev_mgmt_channel	*mgmt_ch;
	struct spdk_bdev_module_channel	*shared_ch;

	ch->bdev = bdev;
	ch->channel = bdev->fn_table->get_io_channel(bdev->ctxt);
	if (!ch->channel) {
		return -1;
	}

	ch->mgmt_channel = spdk_get_io_channel(&g_bdev_mgr);
	if (!ch->mgmt_channel) {
		return -1;
	}

	mgmt_ch = spdk_io_channel_get_ctx(ch->mgmt_channel);
	TAILQ_FOREACH(shared_ch, &mgmt_ch->module_channels, link) {
		if (shared_ch->module_ch == ch->channel) {
			shared_ch->ref++;
			break;
		}
	}

	if (shared_ch == NULL) {
		shared_ch = calloc(1, sizeof(*shared_ch));
		if (!shared_ch) {
			return -1;
		}

		shared_ch->io_outstanding = 0;
		TAILQ_INIT(&shared_ch->nomem_io);
		shared_ch->nomem_threshold = 0;
		shared_ch->module_ch = ch->channel;
		shared_ch->ref = 1;
		TAILQ_INSERT_TAIL(&mgmt_ch->module_channels, shared_ch, link);
	}

	memset(&ch->stat, 0, sizeof(ch->stat));
	TAILQ_INIT(&ch->queued_resets);
	ch->flags = 0;
	ch->module_ch = shared_ch;

	return 0;
}

static void
_spdk_bdev_channel_destroy_resource(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_mgmt_channel	*mgmt_channel;
	struct spdk_bdev_module_channel	*shared_ch = ch->module_ch;

	if (ch->channel) {
		spdk_put_io_channel(ch->channel);
	}

	if (ch->mgmt_channel) {
		if (shared_ch) {
			assert(shared_ch->ref > 0);
			shared_ch->ref--;
			if (shared_ch->ref == 0) {
				mgmt_channel = spdk_io_channel_get_ctx(ch->mgmt_channel);
				assert(shared_ch->io_outstanding == 0);
				TAILQ_REMOVE(&mgmt_channel->module_channels, shared_ch, link);
				free(shared_ch);
			}
		}
		spdk_put_io_channel(ch->mgmt_channel);
	}
}

static int
spdk_bdev_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel	*ch = ctx_buf;

	if (_spdk_bdev_channel_create(ch, io_device) != 0) {
		_spdk_bdev_channel_destroy_resource(ch);
		return -1;
	}

#ifdef SPDK_CONFIG_VTUNE
	{
		char *name;
		__itt_init_ittlib(NULL, 0);
		name = spdk_sprintf_alloc("spdk_bdev_%s_%p", ch->bdev->name, ch);
		if (!name) {
			_spdk_bdev_channel_destroy_resource(ch);
			return -1;
		}
		ch->handle = __itt_string_handle_create(name);
		free(name);
		ch->start_tsc = spdk_get_ticks();
		ch->interval_tsc = spdk_get_ticks_hz() / 100;
	}
#endif

	return 0;
}

/*
 * Abort I/O that are waiting on a data buffer.  These types of I/O are
 *  linked using the spdk_bdev_io buf_link TAILQ_ENTRY.
 */
static void
_spdk_bdev_abort_buf_io(bdev_io_stailq_t *queue, struct spdk_bdev_channel *ch)
{
	bdev_io_stailq_t tmp;
	struct spdk_bdev_io *bdev_io;

	STAILQ_INIT(&tmp);

	while (!STAILQ_EMPTY(queue)) {
		bdev_io = STAILQ_FIRST(queue);
		STAILQ_REMOVE_HEAD(queue, buf_link);
		if (bdev_io->ch == ch) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			STAILQ_INSERT_TAIL(&tmp, bdev_io, buf_link);
		}
	}

	STAILQ_SWAP(&tmp, queue, spdk_bdev_io);
}

/*
 * Abort I/O that are queued waiting for submission.  These types of I/O are
 *  linked using the spdk_bdev_io link TAILQ_ENTRY.
 */
static void
_spdk_bdev_abort_queued_io(bdev_io_tailq_t *queue, struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	TAILQ_FOREACH_SAFE(bdev_io, queue, link, tmp) {
		if (bdev_io->ch == ch) {
			TAILQ_REMOVE(queue, bdev_io, link);
			/*
			 * spdk_bdev_io_complete() assumes that the completed I/O had
			 *  been submitted to the bdev module.  Since in this case it
			 *  hadn't, bump io_outstanding to account for the decrement
			 *  that spdk_bdev_io_complete() will do.
			 */
			if (bdev_io->type != SPDK_BDEV_IO_TYPE_RESET) {
				ch->module_ch->io_outstanding++;
			}
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
_spdk_bdev_channel_destroy(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_mgmt_channel	*mgmt_channel;
	struct spdk_bdev_module_channel	*shared_ch = ch->module_ch;

	mgmt_channel = spdk_io_channel_get_ctx(ch->mgmt_channel);

	_spdk_bdev_abort_queued_io(&ch->queued_resets, ch);
	_spdk_bdev_abort_queued_io(&shared_ch->nomem_io, ch);
	_spdk_bdev_abort_buf_io(&mgmt_channel->need_buf_small, ch);
	_spdk_bdev_abort_buf_io(&mgmt_channel->need_buf_large, ch);

	_spdk_bdev_channel_destroy_resource(ch);
}

static void
spdk_bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel	*ch = ctx_buf;

	_spdk_bdev_channel_destroy(ch);
}

int
spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	if (alias == NULL) {
		SPDK_ERRLOG("Empty alias passed\n");
		return -EINVAL;
	}

	if (spdk_bdev_get_by_name(alias)) {
		SPDK_ERRLOG("Bdev name/alias: %s already exists\n", alias);
		return -EEXIST;
	}

	tmp = calloc(1, sizeof(*tmp));
	if (tmp == NULL) {
		SPDK_ERRLOG("Unable to allocate alias\n");
		return -ENOMEM;
	}

	tmp->alias = strdup(alias);
	if (tmp->alias == NULL) {
		free(tmp);
		SPDK_ERRLOG("Unable to allocate alias\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&bdev->aliases, tmp, tailq);

	return 0;
}

int
spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		if (strcmp(alias, tmp->alias) == 0) {
			TAILQ_REMOVE(&bdev->aliases, tmp, tailq);
			free(tmp->alias);
			free(tmp);
			return 0;
		}
	}

	SPDK_INFOLOG(SPDK_LOG_BDEV, "Alias %s does not exists\n", alias);

	return -ENOENT;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return spdk_get_io_channel(__bdev_to_io_dev(desc->bdev));
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

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

size_t
spdk_bdev_get_buf_align(const struct spdk_bdev *bdev)
{
	/* TODO: push this logic down to the bdev modules */
	if (bdev->need_aligned_buffer) {
		return bdev->blocklen;
	}

	return 1;
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

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	int ret;

	pthread_mutex_lock(&bdev->mutex);

	/* bdev has open descriptors */
	if (!TAILQ_EMPTY(&bdev->open_descs) &&
	    bdev->blockcnt > size) {
		ret = -EBUSY;
	} else {
		bdev->blockcnt = size;
		ret = 0;
	}

	pthread_mutex_unlock(&bdev->mutex);

	return ret;
}

/*
 * Convert I/O offset and length from bytes to blocks.
 *
 * Returns zero on success or non-zero if the byte parameters aren't divisible by the block size.
 */
static uint64_t
spdk_bdev_bytes_to_blocks(struct spdk_bdev *bdev, uint64_t offset_bytes, uint64_t *offset_blocks,
			  uint64_t num_bytes, uint64_t *num_blocks)
{
	uint32_t block_size = bdev->blocklen;

	*offset_blocks = offset_bytes / block_size;
	*num_blocks = num_bytes / block_size;

	return (offset_bytes % block_size) | (num_bytes % block_size);
}

static bool
spdk_bdev_io_valid_blocks(struct spdk_bdev *bdev, uint64_t offset_blocks, uint64_t num_blocks)
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

int
spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed duing read\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.bdev.iov.iov_base = buf;
	bdev_io->u.bdev.iov.iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovs = &bdev_io->u.bdev.iov;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_readv_blocks(desc, ch, iov, iovcnt, offset_blocks, num_blocks, cb, cb_arg);
}

int spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed duing read\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing write\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.bdev.iov.iov_base = buf;
	bdev_io->u.bdev.iov.iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovs = &bdev_io->u.bdev.iov;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, len, &num_blocks) != 0) {
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
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing writev\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset, uint64_t len,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, len, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_write_zeroes_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	uint64_t len;
	bool split_request = false;

	if (num_blocks > UINT64_MAX / spdk_bdev_get_block_size(bdev)) {
		SPDK_ERRLOG("length argument out of range in write_zeroes\n");
		return -ERANGE;
	}

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing write_zeroes\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->u.bdev.offset_blocks = offset_blocks;

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
		bdev_io->u.bdev.num_blocks = num_blocks;
		bdev_io->u.bdev.iovs = NULL;
		bdev_io->u.bdev.iovcnt = 0;

	} else {
		assert(spdk_bdev_get_block_size(bdev) <= ZERO_BUFFER_SIZE);

		len = spdk_bdev_get_block_size(bdev) * num_blocks;

		if (len > ZERO_BUFFER_SIZE) {
			split_request = true;
			len = ZERO_BUFFER_SIZE;
		}

		bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
		bdev_io->u.bdev.iov.iov_base = g_bdev_mgr.zero_buffer;
		bdev_io->u.bdev.iov.iov_len = len;
		bdev_io->u.bdev.iovs = &bdev_io->u.bdev.iov;
		bdev_io->u.bdev.iovcnt = 1;
		bdev_io->u.bdev.num_blocks = len / spdk_bdev_get_block_size(bdev);
		bdev_io->split_remaining_num_blocks = num_blocks - bdev_io->u.bdev.num_blocks;
		bdev_io->split_current_offset_blocks = offset_blocks + bdev_io->u.bdev.num_blocks;
	}

	if (split_request) {
		bdev_io->stored_user_cb = cb;
		spdk_bdev_io_init(bdev_io, bdev, cb_arg, spdk_bdev_write_zeroes_split);
	} else {
		spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);
	}
	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, nbytes, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_unmap_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Can't unmap 0 bytes\n");
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing unmap\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	bdev_io->u.bdev.iov.iov_base = NULL;
	bdev_io->u.bdev.iov.iov_len = 0;
	bdev_io->u.bdev.iovs = &bdev_io->u.bdev.iov;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	uint64_t offset_blocks, num_blocks;

	if (spdk_bdev_bytes_to_blocks(desc->bdev, offset, &offset_blocks, length, &num_blocks) != 0) {
		return -EINVAL;
	}

	return spdk_bdev_flush_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	if (!spdk_bdev_io_valid_blocks(bdev, offset_blocks, num_blocks)) {
		return -EINVAL;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing flush\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	bdev_io->u.bdev.iovs = NULL;
	bdev_io->u.bdev.iovcnt = 0;
	bdev_io->u.bdev.offset_blocks = offset_blocks;
	bdev_io->u.bdev.num_blocks = num_blocks;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

static void
_spdk_bdev_reset_dev(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_channel *ch = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev_io *bdev_io;

	bdev_io = TAILQ_FIRST(&ch->queued_resets);
	TAILQ_REMOVE(&ch->queued_resets, bdev_io, link);
	spdk_bdev_io_submit_reset(bdev_io);
}

static void
_spdk_bdev_reset_freeze_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel 		*ch;
	struct spdk_bdev_channel	*channel;
	struct spdk_bdev_mgmt_channel	*mgmt_channel;
	struct spdk_bdev_module_channel	*shared_ch;

	ch = spdk_io_channel_iter_get_channel(i);
	channel = spdk_io_channel_get_ctx(ch);
	mgmt_channel = spdk_io_channel_get_ctx(channel->mgmt_channel);
	shared_ch = channel->module_ch;

	channel->flags |= BDEV_CH_RESET_IN_PROGRESS;

	_spdk_bdev_abort_queued_io(&shared_ch->nomem_io, channel);
	_spdk_bdev_abort_buf_io(&mgmt_channel->need_buf_small, channel);
	_spdk_bdev_abort_buf_io(&mgmt_channel->need_buf_large, channel);

	spdk_for_each_channel_continue(i, 0);
}

static void
_spdk_bdev_start_reset(void *ctx)
{
	struct spdk_bdev_channel *ch = ctx;

	spdk_for_each_channel(__bdev_to_io_dev(ch->bdev), _spdk_bdev_reset_freeze_channel,
			      ch, _spdk_bdev_reset_dev);
}

static void
_spdk_bdev_channel_start_reset(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev *bdev = ch->bdev;

	assert(!TAILQ_EMPTY(&ch->queued_resets));

	pthread_mutex_lock(&bdev->mutex);
	if (bdev->reset_in_progress == NULL) {
		bdev->reset_in_progress = TAILQ_FIRST(&ch->queued_resets);
		/*
		 * Take a channel reference for the target bdev for the life of this
		 *  reset.  This guards against the channel getting destroyed while
		 *  spdk_for_each_channel() calls related to this reset IO are in
		 *  progress.  We will release the reference when this reset is
		 *  completed.
		 */
		bdev->reset_in_progress->u.reset.ch_ref = spdk_get_io_channel(__bdev_to_io_dev(bdev));
		_spdk_bdev_start_reset(ch);
	}
	pthread_mutex_unlock(&bdev->mutex);
}

int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing reset\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	bdev_io->u.reset.ch_ref = NULL;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	pthread_mutex_lock(&bdev->mutex);
	TAILQ_INSERT_TAIL(&channel->queued_resets, bdev_io, link);
	pthread_mutex_unlock(&bdev->mutex);

	_spdk_bdev_channel_start_reset(channel);

	return 0;
}

void
spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		      struct spdk_bdev_io_stat *stat)
{
#ifdef SPDK_CONFIG_VTUNE
	SPDK_ERRLOG("Calling spdk_bdev_get_io_stat is not allowed when VTune integration is enabled.\n");
	memset(stat, 0, sizeof(*stat));
	return;
#endif

	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	channel->stat.ticks_rate = spdk_get_ticks_hz();
	*stat = channel->stat;
	memset(&channel->stat, 0, sizeof(channel->stat));
}

int
spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = NULL;
	bdev_io->u.nvme_passthru.md_len = 0;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
			   spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = NULL;
	bdev_io->u.nvme_passthru.md_len = 0;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	if (!desc->write) {
		/*
		 * Do not try to parse the NVMe command - we could maybe use bits in the opcode
		 *  to easily determine if the command is a read or write, but for now just
		 *  do not allow io_passthru with a read-only descriptor.
		 */
		return -EBADF;
	}

	bdev_io = spdk_bdev_get_io(channel->mgmt_channel);
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed during nvme_admin_passthru\n");
		return -ENOMEM;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_IO_MD;
	bdev_io->u.nvme_passthru.cmd = *cmd;
	bdev_io->u.nvme_passthru.buf = buf;
	bdev_io->u.nvme_passthru.nbytes = nbytes;
	bdev_io->u.nvme_passthru.md_buf = md_buf;
	bdev_io->u.nvme_passthru.md_len = md_len;

	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io is NULL\n");
		return -1;
	}

	if (bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING) {
		SPDK_ERRLOG("bdev_io is in pending state\n");
		assert(false);
		return -1;
	}

	spdk_bdev_put_io(bdev_io);

	return 0;
}

static void
_spdk_bdev_ch_retry_io(struct spdk_bdev_channel *bdev_ch)
{
	struct spdk_bdev *bdev = bdev_ch->bdev;
	struct spdk_bdev_module_channel	*shared_ch = bdev_ch->module_ch;
	struct spdk_bdev_io *bdev_io;

	if (shared_ch->io_outstanding > shared_ch->nomem_threshold) {
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

	while (!TAILQ_EMPTY(&shared_ch->nomem_io)) {
		bdev_io = TAILQ_FIRST(&shared_ch->nomem_io);
		TAILQ_REMOVE(&shared_ch->nomem_io, bdev_io, link);
		shared_ch->io_outstanding++;
		bdev_io->status = SPDK_BDEV_IO_STATUS_PENDING;
		bdev->fn_table->submit_request(bdev_io->ch->channel, bdev_io);
		if (bdev_io->status == SPDK_BDEV_IO_STATUS_NOMEM) {
			break;
		}
	}
}

static void
_spdk_bdev_io_complete(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	assert(bdev_io->cb != NULL);
	bdev_io->cb(bdev_io, bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS, bdev_io->caller_ctx);
}

static void
_spdk_bdev_reset_complete(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_bdev_io *bdev_io = spdk_io_channel_iter_get_ctx(i);

	if (bdev_io->u.reset.ch_ref != NULL) {
		spdk_put_io_channel(bdev_io->u.reset.ch_ref);
		bdev_io->u.reset.ch_ref = NULL;
	}

	_spdk_bdev_io_complete(bdev_io);
}

static void
_spdk_bdev_unfreeze_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);

	ch->flags &= ~BDEV_CH_RESET_IN_PROGRESS;
	if (!TAILQ_EMPTY(&ch->queued_resets)) {
		_spdk_bdev_channel_start_reset(ch);
	}

	spdk_for_each_channel_continue(i, 0);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_bdev_channel *bdev_ch = bdev_io->ch;
	struct spdk_bdev_module_channel	*shared_ch = bdev_ch->module_ch;

	bdev_io->status = status;

	if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_RESET)) {
		bool unlock_channels = false;

		if (status == SPDK_BDEV_IO_STATUS_NOMEM) {
			SPDK_ERRLOG("NOMEM returned for reset\n");
		}
		pthread_mutex_lock(&bdev->mutex);
		if (bdev_io == bdev->reset_in_progress) {
			bdev->reset_in_progress = NULL;
			unlock_channels = true;
		}
		pthread_mutex_unlock(&bdev->mutex);

		if (unlock_channels) {
			spdk_for_each_channel(__bdev_to_io_dev(bdev), _spdk_bdev_unfreeze_channel,
					      bdev_io, _spdk_bdev_reset_complete);
			return;
		}
	} else {
		assert(shared_ch->io_outstanding > 0);
		shared_ch->io_outstanding--;
		if (spdk_likely(status != SPDK_BDEV_IO_STATUS_NOMEM)) {
			if (spdk_unlikely(!TAILQ_EMPTY(&shared_ch->nomem_io))) {
				_spdk_bdev_ch_retry_io(bdev_ch);
			}
		} else {
			TAILQ_INSERT_HEAD(&shared_ch->nomem_io, bdev_io, link);
			/*
			 * Wait for some of the outstanding I/O to complete before we
			 *  retry any of the nomem_io.  Normally we will wait for
			 *  NOMEM_THRESHOLD_COUNT I/O to complete but for low queue
			 *  depth channels we will instead wait for half to complete.
			 */
			shared_ch->nomem_threshold = spdk_max((int64_t)shared_ch->io_outstanding / 2,
							      (int64_t)shared_ch->io_outstanding - NOMEM_THRESHOLD_COUNT);
			return;
		}
	}

	if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
			bdev_ch->stat.bytes_read += bdev_io->u.bdev.num_blocks * bdev->blocklen;
			bdev_ch->stat.num_read_ops++;
			bdev_ch->stat.read_latency_ticks += (spdk_get_ticks() - bdev_io->submit_tsc);
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			bdev_ch->stat.bytes_written += bdev_io->u.bdev.num_blocks * bdev->blocklen;
			bdev_ch->stat.num_write_ops++;
			bdev_ch->stat.write_latency_ticks += (spdk_get_ticks() - bdev_io->submit_tsc);
			break;
		default:
			break;
		}
	}

#ifdef SPDK_CONFIG_VTUNE
	uint64_t now_tsc = spdk_get_ticks();
	if (now_tsc > (bdev_ch->start_tsc + bdev_ch->interval_tsc)) {
		uint64_t data[5];

		data[0] = bdev_ch->stat.num_read_ops;
		data[1] = bdev_ch->stat.bytes_read;
		data[2] = bdev_ch->stat.num_write_ops;
		data[3] = bdev_ch->stat.bytes_written;
		data[4] = bdev->fn_table->get_spin_time ?
			  bdev->fn_table->get_spin_time(bdev_ch->channel) : 0;

		__itt_metadata_add(g_bdev_mgr.domain, __itt_null, bdev_ch->handle,
				   __itt_metadata_u64, 5, data);

		memset(&bdev_ch->stat, 0, sizeof(bdev_ch->stat));
		bdev_ch->start_tsc = now_tsc;
	}
#endif

	if (bdev_io->in_submit_request) {
		/*
		 * Defer completion to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		spdk_thread_send_msg(spdk_io_channel_get_thread(bdev_ch->channel),
				     _spdk_bdev_io_complete, bdev_io);
	} else {
		_spdk_bdev_io_complete(bdev_io);
	}
}

void
spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				  enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	if (sc == SPDK_SCSI_STATUS_GOOD) {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
		bdev_io->error.scsi.sc = sc;
		bdev_io->error.scsi.sk = sk;
		bdev_io->error.scsi.asc = asc;
		bdev_io->error.scsi.ascq = ascq;
	}

	spdk_bdev_io_complete(bdev_io, bdev_io->status);
}

void
spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
			     int *sc, int *sk, int *asc, int *ascq)
{
	assert(sc != NULL);
	assert(sk != NULL);
	assert(asc != NULL);
	assert(ascq != NULL);

	switch (bdev_io->status) {
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
		*sc = bdev_io->error.scsi.sc;
		*sk = bdev_io->error.scsi.sk;
		*asc = bdev_io->error.scsi.asc;
		*ascq = bdev_io->error.scsi.ascq;
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
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc)
{
	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		bdev_io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->error.nvme.sct = sct;
		bdev_io->error.nvme.sc = sc;
		bdev_io->status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
	}

	spdk_bdev_io_complete(bdev_io, bdev_io->status);
}

void
spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc)
{
	assert(sct != NULL);
	assert(sc != NULL);

	if (bdev_io->status == SPDK_BDEV_IO_STATUS_NVME_ERROR) {
		*sct = bdev_io->error.nvme.sct;
		*sc = bdev_io->error.nvme.sc;
	} else if (bdev_io->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_SUCCESS;
	} else {
		*sct = SPDK_NVME_SCT_GENERIC;
		*sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
}

struct spdk_thread *
spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io)
{
	return spdk_io_channel_get_thread(bdev_io->ch->channel);
}

static int
_spdk_bdev_register(struct spdk_bdev *bdev)
{
	struct spdk_bdev_module_if *module;

	assert(bdev->module != NULL);

	if (!bdev->name) {
		SPDK_ERRLOG("Bdev name is NULL\n");
		return -EINVAL;
	}

	if (spdk_bdev_get_by_name(bdev->name)) {
		SPDK_ERRLOG("Bdev name:%s already exists\n", bdev->name);
		return -EEXIST;
	}

	bdev->status = SPDK_BDEV_STATUS_READY;

	TAILQ_INIT(&bdev->open_descs);

	TAILQ_INIT(&bdev->vbdevs);
	TAILQ_INIT(&bdev->base_bdevs);

	TAILQ_INIT(&bdev->aliases);

	bdev->reset_in_progress = NULL;

	spdk_io_device_register(__bdev_to_io_dev(bdev),
				spdk_bdev_channel_create, spdk_bdev_channel_destroy,
				sizeof(struct spdk_bdev_channel));

	pthread_mutex_init(&bdev->mutex, NULL);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdevs, bdev, link);

	TAILQ_FOREACH(module, &g_bdev_mgr.bdev_modules, tailq) {
		if (module->examine) {
			module->action_in_progress++;
			module->examine(bdev);
		}
	}

	return 0;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	return _spdk_bdev_register(bdev);
}

int
spdk_vbdev_register(struct spdk_bdev *vbdev, struct spdk_bdev **base_bdevs, int base_bdev_count)
{
	int i, rc;

	rc = _spdk_bdev_register(vbdev);
	if (rc) {
		return rc;
	}

	for (i = 0; i < base_bdev_count; i++) {
		assert(base_bdevs[i] != NULL);
		TAILQ_INSERT_TAIL(&vbdev->base_bdevs, base_bdevs[i], base_bdev_link);
		TAILQ_INSERT_TAIL(&base_bdevs[i]->vbdevs, vbdev, vbdev_link);
	}

	return 0;
}

void
spdk_bdev_unregister_done(struct spdk_bdev *bdev, int bdeverrno)
{
	if (bdev->unregister_cb != NULL) {
		bdev->unregister_cb(bdev->unregister_ctx, bdeverrno);
	}
}

static void
_remove_notify(void *arg)
{
	struct spdk_bdev_desc *desc = arg;

	desc->remove_cb(desc->remove_ctx);
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc	*desc, *tmp;
	int			rc;
	bool			do_destruct = true;
	struct spdk_bdev	*base_bdev;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV, "Removing bdev %s from list\n", bdev->name);

	pthread_mutex_lock(&bdev->mutex);

	if (!TAILQ_EMPTY(&bdev->base_bdevs)) {
		TAILQ_FOREACH(base_bdev, &bdev->base_bdevs, base_bdev_link) {
			TAILQ_REMOVE(&base_bdev->vbdevs, bdev, vbdev_link);
		}
	}

	bdev->status = SPDK_BDEV_STATUS_REMOVING;
	bdev->unregister_cb = cb_fn;
	bdev->unregister_ctx = cb_arg;

	TAILQ_FOREACH_SAFE(desc, &bdev->open_descs, link, tmp) {
		if (desc->remove_cb) {
			do_destruct = false;
			/*
			 * Defer invocation of the remove_cb to a separate message that will
			 *  run later on this thread.  This ensures this context unwinds and
			 *  we don't recursively unregister this bdev again if the remove_cb
			 *  immediately closes its descriptor.
			 */
			spdk_thread_send_msg(spdk_get_thread(), _remove_notify, desc);
		}
	}

	if (!do_destruct) {
		pthread_mutex_unlock(&bdev->mutex);
		return;
	}

	TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, link);
	pthread_mutex_unlock(&bdev->mutex);

	pthread_mutex_destroy(&bdev->mutex);

	spdk_io_device_unregister(__bdev_to_io_dev(bdev), NULL);

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n");
	}
	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, rc);
	}
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for bdev descriptor\n");
		return -ENOMEM;
	}

	pthread_mutex_lock(&bdev->mutex);

	if (write && bdev->claim_module) {
		SPDK_INFOLOG(SPDK_LOG_BDEV, "Could not open %s - already claimed\n", bdev->name);
		free(desc);
		pthread_mutex_unlock(&bdev->mutex);
		return -EPERM;
	}

	TAILQ_INSERT_TAIL(&bdev->open_descs, desc, link);

	desc->bdev = bdev;
	desc->remove_cb = remove_cb;
	desc->remove_ctx = remove_ctx;
	desc->write = write;
	*_desc = desc;

	pthread_mutex_unlock(&bdev->mutex);

	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;
	bool do_unregister = false;

	pthread_mutex_lock(&bdev->mutex);

	TAILQ_REMOVE(&bdev->open_descs, desc, link);
	free(desc);

	if (bdev->status == SPDK_BDEV_STATUS_REMOVING && TAILQ_EMPTY(&bdev->open_descs)) {
		do_unregister = true;
	}
	pthread_mutex_unlock(&bdev->mutex);

	if (do_unregister == true) {
		spdk_bdev_unregister(bdev, bdev->unregister_cb, bdev->unregister_ctx);
	}
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module_if *module)
{
	if (bdev->claim_module != NULL) {
		SPDK_ERRLOG("bdev %s already claimed by module %s\n", bdev->name,
			    bdev->claim_module->name);
		return -EPERM;
	}

	if (desc && !desc->write) {
		desc->write = true;
	}

	bdev->claim_module = module;
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
	assert(bdev->claim_module != NULL);
	bdev->claim_module = NULL;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
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
		iovs = bdev_io->u.bdev.iovs;
		iovcnt = bdev_io->u.bdev.iovcnt;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
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

void
spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module)
{
	/*
	 * Modules with examine callbacks must be initialized first, so they are
	 *  ready to handle examine callbacks from later modules that will
	 *  register physical bdevs.
	 */
	if (bdev_module->examine != NULL) {
		TAILQ_INSERT_HEAD(&g_bdev_mgr.bdev_modules, bdev_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&g_bdev_mgr.bdev_modules, bdev_module, tailq);
	}
}

void
spdk_bdev_part_base_free(struct spdk_bdev_part_base *base)
{
	if (base->desc) {
		spdk_bdev_close(base->desc);
		base->desc = NULL;
	}
	base->base_free_fn(base);
}

void
spdk_bdev_part_free(struct spdk_bdev_part *part)
{
	struct spdk_bdev_part_base *base;

	assert(part);
	assert(part->base);

	base = part->base;
	spdk_io_device_unregister(part, NULL);
	TAILQ_REMOVE(base->tailq, part, tailq);
	free(part->bdev.name);
	free(part);

	if (__sync_sub_and_fetch(&base->ref, 1) == 0) {
		spdk_bdev_module_release_bdev(base->bdev);
		spdk_bdev_part_base_free(base);
	}
}

void
spdk_bdev_part_base_hotremove(struct spdk_bdev *base_bdev, struct bdev_part_tailq *tailq)
{
	struct spdk_bdev_part *part, *tmp;

	TAILQ_FOREACH_SAFE(part, tailq, tailq, tmp) {
		if (part->base->bdev == base_bdev) {
			spdk_bdev_unregister(&part->bdev, NULL, NULL);
		}
	}
}

static bool
spdk_bdev_part_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = _part;

	return part->base->bdev->fn_table->io_type_supported(part->base->bdev, io_type);
}

static struct spdk_io_channel *
spdk_bdev_part_get_io_channel(void *_part)
{
	struct spdk_bdev_part *part = _part;

	return spdk_get_io_channel(part);
}

static void
spdk_bdev_part_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
spdk_bdev_write_zeroes_split(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	uint64_t len;

	if (!success) {
		bdev_io->cb = bdev_io->stored_user_cb;
		_spdk_bdev_io_complete(bdev_io);
		return;
	}

	/* no need to perform the error checking from write_zeroes_blocks because this request already passed those checks. */
	len = spdk_min(spdk_bdev_get_block_size(bdev_io->bdev) * bdev_io->split_remaining_num_blocks,
		       ZERO_BUFFER_SIZE);

	bdev_io->u.bdev.offset_blocks = bdev_io->split_current_offset_blocks;
	bdev_io->u.bdev.iov.iov_len = len;
	bdev_io->u.bdev.num_blocks = len / spdk_bdev_get_block_size(bdev_io->bdev);
	bdev_io->split_remaining_num_blocks -= bdev_io->u.bdev.num_blocks;
	bdev_io->split_current_offset_blocks += bdev_io->u.bdev.num_blocks;

	/* if this round completes the i/o, change the callback to be the original user callback */
	if (bdev_io->split_remaining_num_blocks == 0) {
		spdk_bdev_io_init(bdev_io, bdev_io->bdev, cb_arg, bdev_io->stored_user_cb);
	} else {
		spdk_bdev_io_init(bdev_io, bdev_io->bdev, cb_arg, spdk_bdev_write_zeroes_split);
	}
	spdk_bdev_io_submit(bdev_io);
}

void
spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_part *part = ch->part;
	struct spdk_io_channel *base_ch = ch->base_ch;
	struct spdk_bdev_desc *base_desc = part->base->desc;
	uint64_t offset;
	int rc = 0;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		offset = bdev_io->u.bdev.offset_blocks + part->offset_blocks;
		rc = spdk_bdev_readv_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, offset,
					    bdev_io->u.bdev.num_blocks, spdk_bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		offset = bdev_io->u.bdev.offset_blocks + part->offset_blocks;
		rc = spdk_bdev_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, offset,
					     bdev_io->u.bdev.num_blocks, spdk_bdev_part_complete_io,
					     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		offset = bdev_io->u.bdev.offset_blocks + part->offset_blocks;
		rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
						   spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		offset = bdev_io->u.bdev.offset_blocks + part->offset_blocks;
		rc = spdk_bdev_unmap_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
					    spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		offset = bdev_io->u.bdev.offset_blocks + part->offset_blocks;
		rc = spdk_bdev_flush_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
					    spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				     spdk_bdev_part_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("split: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}
static int
spdk_bdev_part_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = SPDK_CONTAINEROF(io_device, struct spdk_bdev_part, base);
	struct spdk_bdev_part_channel *ch = ctx_buf;

	ch->part = part;
	ch->base_ch = spdk_bdev_get_io_channel(part->base->desc);
	if (ch->base_ch == NULL) {
		return -1;
	}

	if (part->base->ch_create_cb) {
		return part->base->ch_create_cb(io_device, ctx_buf);
	} else {
		return 0;
	}
}

static void
spdk_bdev_part_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = SPDK_CONTAINEROF(io_device, struct spdk_bdev_part, base);
	struct spdk_bdev_part_channel *ch = ctx_buf;

	if (part->base->ch_destroy_cb) {
		part->base->ch_destroy_cb(io_device, ctx_buf);
	}
	spdk_put_io_channel(ch->base_ch);
}

int
spdk_bdev_part_base_construct(struct spdk_bdev_part_base *base, struct spdk_bdev *bdev,
			      spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module_if *module,
			      struct spdk_bdev_fn_table *fn_table, struct bdev_part_tailq *tailq,
			      spdk_bdev_part_base_free_fn free_fn,
			      uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
			      spdk_io_channel_destroy_cb ch_destroy_cb)
{
	int rc;

	fn_table->get_io_channel = spdk_bdev_part_get_io_channel;
	fn_table->io_type_supported = spdk_bdev_part_io_type_supported;

	base->bdev = bdev;
	base->desc = NULL;
	base->ref = 0;
	base->module = module;
	base->fn_table = fn_table;
	base->tailq = tailq;
	base->claimed = false;
	base->channel_size = channel_size;
	base->ch_create_cb = ch_create_cb;
	base->ch_destroy_cb = ch_destroy_cb;
	base->base_free_fn = free_fn;

	rc = spdk_bdev_open(bdev, false, remove_cb, bdev, &base->desc);
	if (rc) {
		spdk_bdev_part_base_free(base);
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
		return -1;
	}

	return 0;
}

int
spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			 char *name, uint64_t offset_blocks, uint64_t num_blocks,
			 char *product_name)
{
	part->bdev.name = name;
	part->bdev.blocklen = base->bdev->blocklen;
	part->bdev.blockcnt = num_blocks;
	part->offset_blocks = offset_blocks;

	part->bdev.write_cache = base->bdev->write_cache;
	part->bdev.need_aligned_buffer = base->bdev->need_aligned_buffer;
	part->bdev.product_name = product_name;
	part->bdev.ctxt = part;
	part->bdev.module = base->module;
	part->bdev.fn_table = base->fn_table;

	__sync_fetch_and_add(&base->ref, 1);
	part->base = base;

	if (!base->claimed) {
		int rc;

		rc = spdk_bdev_module_claim_bdev(base->bdev, base->desc, base->module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base->bdev));
			free(part->bdev.name);
			return -1;
		}
		base->claimed = true;
	}

	spdk_io_device_register(part, spdk_bdev_part_channel_create_cb,
				spdk_bdev_part_channel_destroy_cb,
				base->channel_size);
	spdk_vbdev_register(&part->bdev, &base->bdev, 1);
	TAILQ_INSERT_TAIL(base->tailq, part, tailq);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("bdev", SPDK_LOG_BDEV)
