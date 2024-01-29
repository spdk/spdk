/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"

#define IOBUF_MIN_SMALL_POOL_SIZE	64
#define IOBUF_MIN_LARGE_POOL_SIZE	8
#define IOBUF_DEFAULT_SMALL_POOL_SIZE	8192
#define IOBUF_DEFAULT_LARGE_POOL_SIZE	1024
#define IOBUF_ALIGNMENT			4096
#define IOBUF_MIN_SMALL_BUFSIZE		4096
#define IOBUF_MIN_LARGE_BUFSIZE		8192
#define IOBUF_DEFAULT_SMALL_BUFSIZE	(8 * 1024)
/* 132k is a weird choice at first, but this needs to be large enough to accomodate
 * the default maximum size (128k) plus metadata everywhere. For code paths that
 * are explicitly configured, the math is instead done properly. This is only
 * for the default. */
#define IOBUF_DEFAULT_LARGE_BUFSIZE	(132 * 1024)
#define IOBUF_MAX_CHANNELS		64

SPDK_STATIC_ASSERT(sizeof(struct spdk_iobuf_buffer) <= IOBUF_MIN_SMALL_BUFSIZE,
		   "Invalid data offset");

static bool g_iobuf_is_initialized = false;

struct iobuf_channel {
	spdk_iobuf_entry_stailq_t	small_queue;
	spdk_iobuf_entry_stailq_t	large_queue;
	struct spdk_iobuf_channel	*channels[IOBUF_MAX_CHANNELS];
};

struct iobuf_module {
	char				*name;
	TAILQ_ENTRY(iobuf_module)	tailq;
};

struct iobuf {
	struct spdk_ring		*small_pool;
	struct spdk_ring		*large_pool;
	void				*small_pool_base;
	void				*large_pool_base;
	struct spdk_iobuf_opts		opts;
	TAILQ_HEAD(, iobuf_module)	modules;
	spdk_iobuf_finish_cb		finish_cb;
	void				*finish_arg;
};

static struct iobuf g_iobuf = {
	.modules = TAILQ_HEAD_INITIALIZER(g_iobuf.modules),
	.small_pool = NULL,
	.large_pool = NULL,
	.small_pool_base = NULL,
	.large_pool_base = NULL,
	.opts = {
		.small_pool_count = IOBUF_DEFAULT_SMALL_POOL_SIZE,
		.large_pool_count = IOBUF_DEFAULT_LARGE_POOL_SIZE,
		.small_bufsize = IOBUF_DEFAULT_SMALL_BUFSIZE,
		.large_bufsize = IOBUF_DEFAULT_LARGE_BUFSIZE,
	},
};

struct iobuf_get_stats_ctx {
	struct spdk_iobuf_module_stats	*modules;
	uint32_t			num_modules;
	spdk_iobuf_get_stats_cb		cb_fn;
	void				*cb_arg;
};

static int
iobuf_channel_create_cb(void *io_device, void *ctx)
{
	struct iobuf_channel *ch = ctx;

	STAILQ_INIT(&ch->small_queue);
	STAILQ_INIT(&ch->large_queue);

	return 0;
}

static void
iobuf_channel_destroy_cb(void *io_device, void *ctx)
{
	struct iobuf_channel *ch __attribute__((unused)) = ctx;

	assert(STAILQ_EMPTY(&ch->small_queue));
	assert(STAILQ_EMPTY(&ch->large_queue));
}

int
spdk_iobuf_initialize(void)
{
	struct spdk_iobuf_opts *opts = &g_iobuf.opts;
	int rc = 0;
	uint64_t i;
	struct spdk_iobuf_buffer *buf;

	g_iobuf.small_pool = spdk_ring_create(SPDK_RING_TYPE_MP_MC, opts->small_pool_count,
					      SPDK_ENV_SOCKET_ID_ANY);
	if (!g_iobuf.small_pool) {
		SPDK_ERRLOG("Failed to create small iobuf pool\n");
		rc = -ENOMEM;
		goto error;
	}

	/* Round up to the nearest alignment so that each element remains aligned */
	opts->small_bufsize = SPDK_ALIGN_CEIL(opts->small_bufsize, IOBUF_ALIGNMENT);
	g_iobuf.small_pool_base = spdk_malloc(opts->small_bufsize * opts->small_pool_count, IOBUF_ALIGNMENT,
					      NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_iobuf.small_pool_base == NULL) {
		SPDK_ERRLOG("Unable to allocate requested small iobuf pool size\n");
		rc = -ENOMEM;
		goto error;
	}

	g_iobuf.large_pool = spdk_ring_create(SPDK_RING_TYPE_MP_MC, opts->large_pool_count,
					      SPDK_ENV_SOCKET_ID_ANY);
	if (!g_iobuf.large_pool) {
		SPDK_ERRLOG("Failed to create large iobuf pool\n");
		rc = -ENOMEM;
		goto error;
	}

	/* Round up to the nearest alignment so that each element remains aligned */
	opts->large_bufsize = SPDK_ALIGN_CEIL(opts->large_bufsize, IOBUF_ALIGNMENT);
	g_iobuf.large_pool_base = spdk_malloc(opts->large_bufsize * opts->large_pool_count, IOBUF_ALIGNMENT,
					      NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_iobuf.large_pool_base == NULL) {
		SPDK_ERRLOG("Unable to allocate requested large iobuf pool size\n");
		rc = -ENOMEM;
		goto error;
	}

	for (i = 0; i < opts->small_pool_count; i++) {
		buf = g_iobuf.small_pool_base + i * opts->small_bufsize;
		spdk_ring_enqueue(g_iobuf.small_pool, (void **)&buf, 1, NULL);
	}

	for (i = 0; i < opts->large_pool_count; i++) {
		buf = g_iobuf.large_pool_base + i * opts->large_bufsize;
		spdk_ring_enqueue(g_iobuf.large_pool, (void **)&buf, 1, NULL);
	}

	spdk_io_device_register(&g_iobuf, iobuf_channel_create_cb, iobuf_channel_destroy_cb,
				sizeof(struct iobuf_channel), "iobuf");
	g_iobuf_is_initialized = true;

	return 0;
error:
	spdk_free(g_iobuf.small_pool_base);
	spdk_ring_free(g_iobuf.small_pool);
	spdk_free(g_iobuf.large_pool_base);
	spdk_ring_free(g_iobuf.large_pool);

	return rc;
}

static void
iobuf_unregister_cb(void *io_device)
{
	struct iobuf_module *module;

	while (!TAILQ_EMPTY(&g_iobuf.modules)) {
		module = TAILQ_FIRST(&g_iobuf.modules);
		TAILQ_REMOVE(&g_iobuf.modules, module, tailq);
		free(module->name);
		free(module);
	}

	if (spdk_ring_count(g_iobuf.small_pool) != g_iobuf.opts.small_pool_count) {
		SPDK_ERRLOG("small iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_ring_count(g_iobuf.small_pool), g_iobuf.opts.small_pool_count);
	}

	if (spdk_ring_count(g_iobuf.large_pool) != g_iobuf.opts.large_pool_count) {
		SPDK_ERRLOG("large iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_ring_count(g_iobuf.large_pool), g_iobuf.opts.large_pool_count);
	}

	spdk_free(g_iobuf.small_pool_base);
	g_iobuf.small_pool_base = NULL;
	spdk_ring_free(g_iobuf.small_pool);
	g_iobuf.small_pool = NULL;

	spdk_free(g_iobuf.large_pool_base);
	g_iobuf.large_pool_base = NULL;
	spdk_ring_free(g_iobuf.large_pool);
	g_iobuf.large_pool = NULL;

	if (g_iobuf.finish_cb != NULL) {
		g_iobuf.finish_cb(g_iobuf.finish_arg);
	}
}

void
spdk_iobuf_finish(spdk_iobuf_finish_cb cb_fn, void *cb_arg)
{
	if (!g_iobuf_is_initialized) {
		cb_fn(cb_arg);
		return;
	}

	g_iobuf_is_initialized = false;
	g_iobuf.finish_cb = cb_fn;
	g_iobuf.finish_arg = cb_arg;

	spdk_io_device_unregister(&g_iobuf, iobuf_unregister_cb);
}

int
spdk_iobuf_set_opts(const struct spdk_iobuf_opts *opts)
{
	if (opts->small_pool_count < IOBUF_MIN_SMALL_POOL_SIZE) {
		SPDK_ERRLOG("small_pool_count must be at least %" PRIu32 "\n",
			    IOBUF_MIN_SMALL_POOL_SIZE);
		return -EINVAL;
	}
	if (opts->large_pool_count < IOBUF_MIN_LARGE_POOL_SIZE) {
		SPDK_ERRLOG("large_pool_count must be at least %" PRIu32 "\n",
			    IOBUF_MIN_LARGE_POOL_SIZE);
		return -EINVAL;
	}

	g_iobuf.opts = *opts;

	if (opts->small_bufsize < IOBUF_MIN_SMALL_BUFSIZE) {
		SPDK_ERRLOG("small_bufsize must be at least %" PRIu32 ". Automatically increasing.\n",
			    IOBUF_MIN_SMALL_BUFSIZE);
		g_iobuf.opts.small_bufsize = IOBUF_MIN_SMALL_BUFSIZE;
	}

	if (opts->large_bufsize < IOBUF_MIN_LARGE_BUFSIZE) {
		SPDK_WARNLOG("large_bufsize must be at least %" PRIu32 ". Automatically increasing.\n",
			     IOBUF_MIN_LARGE_BUFSIZE);
		g_iobuf.opts.large_bufsize = IOBUF_MIN_LARGE_BUFSIZE;
	}

	return 0;
}

void
spdk_iobuf_get_opts(struct spdk_iobuf_opts *opts)
{
	*opts = g_iobuf.opts;
}

int
spdk_iobuf_channel_init(struct spdk_iobuf_channel *ch, const char *name,
			uint32_t small_cache_size, uint32_t large_cache_size)
{
	struct spdk_io_channel *ioch;
	struct iobuf_channel *iobuf_ch;
	struct iobuf_module *module;
	struct spdk_iobuf_buffer *buf;
	uint32_t i;

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {
		if (strcmp(name, module->name) == 0) {
			break;
		}
	}

	if (module == NULL) {
		SPDK_ERRLOG("Couldn't find iobuf module: '%s'\n", name);
		return -ENODEV;
	}

	ioch = spdk_get_io_channel(&g_iobuf);
	if (ioch == NULL) {
		SPDK_ERRLOG("Couldn't get iobuf IO channel\n");
		return -ENOMEM;
	}

	iobuf_ch = spdk_io_channel_get_ctx(ioch);

	for (i = 0; i < IOBUF_MAX_CHANNELS; ++i) {
		if (iobuf_ch->channels[i] == NULL) {
			iobuf_ch->channels[i] = ch;
			break;
		}
	}

	if (i == IOBUF_MAX_CHANNELS) {
		SPDK_ERRLOG("Max number of iobuf channels (%" PRIu32 ") exceeded.\n", i);
		goto error;
	}

	ch->small.queue = &iobuf_ch->small_queue;
	ch->large.queue = &iobuf_ch->large_queue;
	ch->small.pool = g_iobuf.small_pool;
	ch->large.pool = g_iobuf.large_pool;
	ch->small.bufsize = g_iobuf.opts.small_bufsize;
	ch->large.bufsize = g_iobuf.opts.large_bufsize;
	ch->parent = ioch;
	ch->module = module;
	ch->small.cache_size = small_cache_size;
	ch->large.cache_size = large_cache_size;
	ch->small.cache_count = 0;
	ch->large.cache_count = 0;

	STAILQ_INIT(&ch->small.cache);
	STAILQ_INIT(&ch->large.cache);

	for (i = 0; i < small_cache_size; ++i) {
		if (spdk_ring_dequeue(g_iobuf.small_pool, (void **)&buf, 1) == 0) {
			SPDK_ERRLOG("Failed to populate iobuf small buffer cache. "
				    "You may need to increase spdk_iobuf_opts.small_pool_count (%"PRIu64")\n",
				    g_iobuf.opts.small_pool_count);
			SPDK_ERRLOG("See scripts/calc-iobuf.py for guidance on how to calculate "
				    "this value.\n");
			goto error;
		}
		STAILQ_INSERT_TAIL(&ch->small.cache, buf, stailq);
		ch->small.cache_count++;
	}
	for (i = 0; i < large_cache_size; ++i) {
		if (spdk_ring_dequeue(g_iobuf.large_pool, (void **)&buf, 1) == 0) {
			SPDK_ERRLOG("Failed to populate iobuf large buffer cache. "
				    "You may need to increase spdk_iobuf_opts.large_pool_count (%"PRIu64")\n",
				    g_iobuf.opts.large_pool_count);
			SPDK_ERRLOG("See scripts/calc-iobuf.py for guidance on how to calculate "
				    "this value.\n");
			goto error;
		}
		STAILQ_INSERT_TAIL(&ch->large.cache, buf, stailq);
		ch->large.cache_count++;
	}

	return 0;
error:
	spdk_iobuf_channel_fini(ch);

	return -ENOMEM;
}

void
spdk_iobuf_channel_fini(struct spdk_iobuf_channel *ch)
{
	struct spdk_iobuf_entry *entry __attribute__((unused));
	struct spdk_iobuf_buffer *buf;
	struct iobuf_channel *iobuf_ch;
	uint32_t i;

	/* Make sure none of the wait queue entries are coming from this module */
	STAILQ_FOREACH(entry, ch->small.queue, stailq) {
		assert(entry->module != ch->module);
	}
	STAILQ_FOREACH(entry, ch->large.queue, stailq) {
		assert(entry->module != ch->module);
	}

	/* Release cached buffers back to the pool */
	while (!STAILQ_EMPTY(&ch->small.cache)) {
		buf = STAILQ_FIRST(&ch->small.cache);
		STAILQ_REMOVE_HEAD(&ch->small.cache, stailq);
		spdk_ring_enqueue(g_iobuf.small_pool, (void **)&buf, 1, NULL);
		ch->small.cache_count--;
	}
	while (!STAILQ_EMPTY(&ch->large.cache)) {
		buf = STAILQ_FIRST(&ch->large.cache);
		STAILQ_REMOVE_HEAD(&ch->large.cache, stailq);
		spdk_ring_enqueue(g_iobuf.large_pool, (void **)&buf, 1, NULL);
		ch->large.cache_count--;
	}

	assert(ch->small.cache_count == 0);
	assert(ch->large.cache_count == 0);

	iobuf_ch = spdk_io_channel_get_ctx(ch->parent);
	for (i = 0; i < IOBUF_MAX_CHANNELS; ++i) {
		if (iobuf_ch->channels[i] == ch) {
			iobuf_ch->channels[i] = NULL;
			break;
		}
	}

	spdk_put_io_channel(ch->parent);
	ch->parent = NULL;
}

int
spdk_iobuf_register_module(const char *name)
{
	struct iobuf_module *module;

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {
		if (strcmp(name, module->name) == 0) {
			return -EEXIST;
		}
	}

	module = calloc(1, sizeof(*module));
	if (module == NULL) {
		return -ENOMEM;
	}

	module->name = strdup(name);
	if (module->name == NULL) {
		free(module);
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_iobuf.modules, module, tailq);

	return 0;
}

int
spdk_iobuf_unregister_module(const char *name)
{
	struct iobuf_module *module;

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {
		if (strcmp(name, module->name) == 0) {
			TAILQ_REMOVE(&g_iobuf.modules, module, tailq);
			free(module->name);
			free(module);
			return 0;
		}
	}

	return -ENOENT;
}

int
spdk_iobuf_for_each_entry(struct spdk_iobuf_channel *ch, struct spdk_iobuf_pool *pool,
			  spdk_iobuf_for_each_entry_fn cb_fn, void *cb_ctx)
{
	struct spdk_iobuf_entry *entry, *tmp;
	int rc;

	STAILQ_FOREACH_SAFE(entry, pool->queue, stailq, tmp) {
		/* We only want to iterate over the entries requested by the module which owns ch */
		if (entry->module != ch->module) {
			continue;
		}

		rc = cb_fn(ch, entry, cb_ctx);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

void
spdk_iobuf_entry_abort(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
		       uint64_t len)
{
	struct spdk_iobuf_pool *pool;

	if (len <= ch->small.bufsize) {
		pool = &ch->small;
	} else {
		assert(len <= ch->large.bufsize);
		pool = &ch->large;
	}

	STAILQ_REMOVE(pool->queue, entry, spdk_iobuf_entry, stailq);
}

#define IOBUF_BATCH_SIZE 32

void *
spdk_iobuf_get(struct spdk_iobuf_channel *ch, uint64_t len,
	       struct spdk_iobuf_entry *entry, spdk_iobuf_get_cb cb_fn)
{
	struct spdk_iobuf_pool *pool;
	void *buf;

	assert(spdk_io_channel_get_thread(ch->parent) == spdk_get_thread());
	if (len <= ch->small.bufsize) {
		pool = &ch->small;
	} else {
		assert(len <= ch->large.bufsize);
		pool = &ch->large;
	}

	buf = (void *)STAILQ_FIRST(&pool->cache);
	if (buf) {
		STAILQ_REMOVE_HEAD(&pool->cache, stailq);
		assert(pool->cache_count > 0);
		pool->cache_count--;
		pool->stats.cache++;
	} else {
		struct spdk_iobuf_buffer *bufs[IOBUF_BATCH_SIZE];
		size_t sz, i;

		/* If we're going to dequeue, we may as well dequeue a batch. */
		sz = spdk_ring_dequeue(pool->pool, (void **)bufs, spdk_min(IOBUF_BATCH_SIZE,
				       spdk_max(pool->cache_size, 1)));
		if (sz == 0) {
			if (entry) {
				STAILQ_INSERT_TAIL(pool->queue, entry, stailq);
				entry->module = ch->module;
				entry->cb_fn = cb_fn;
				pool->stats.retry++;
			}

			return NULL;
		}

		pool->stats.main++;
		for (i = 0; i < (sz - 1); i++) {
			STAILQ_INSERT_HEAD(&pool->cache, bufs[i], stailq);
			pool->cache_count++;
		}

		/* The last one is the one we'll return */
		buf = bufs[i];
	}

	return (char *)buf;
}

void
spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len)
{
	struct spdk_iobuf_entry *entry;
	struct spdk_iobuf_buffer *iobuf_buf;
	struct spdk_iobuf_pool *pool;
	size_t sz;

	assert(spdk_io_channel_get_thread(ch->parent) == spdk_get_thread());
	if (len <= ch->small.bufsize) {
		pool = &ch->small;
	} else {
		pool = &ch->large;
	}

	if (STAILQ_EMPTY(pool->queue)) {
		if (pool->cache_size == 0) {
			spdk_ring_enqueue(pool->pool, (void **)&buf, 1, NULL);
			return;
		}

		iobuf_buf = (struct spdk_iobuf_buffer *)buf;

		STAILQ_INSERT_HEAD(&pool->cache, iobuf_buf, stailq);
		pool->cache_count++;

		/* The cache size may exceed the configured amount. We always dequeue from the
		 * central pool in batches of known size, so wait until at least a batch
		 * has been returned to actually return the buffers to the central pool. */
		sz = spdk_min(IOBUF_BATCH_SIZE, pool->cache_size);
		if (pool->cache_count >= pool->cache_size + sz) {
			struct spdk_iobuf_buffer *bufs[IOBUF_BATCH_SIZE];
			size_t i;

			for (i = 0; i < sz; i++) {
				bufs[i] = STAILQ_FIRST(&pool->cache);
				STAILQ_REMOVE_HEAD(&pool->cache, stailq);
				assert(pool->cache_count > 0);
				pool->cache_count--;
			}

			spdk_ring_enqueue(pool->pool, (void **)bufs, sz, NULL);
		}
	} else {
		entry = STAILQ_FIRST(pool->queue);
		STAILQ_REMOVE_HEAD(pool->queue, stailq);
		entry->cb_fn(entry, buf);
	}
}

static void
iobuf_get_channel_stats_done(struct spdk_io_channel_iter *iter, int status)
{
	struct iobuf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);

	ctx->cb_fn(ctx->modules, ctx->num_modules, ctx->cb_arg);
	free(ctx->modules);
	free(ctx);
}

static void
iobuf_get_channel_stats(struct spdk_io_channel_iter *iter)
{
	struct iobuf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(iter);
	struct iobuf_channel *iobuf_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_iobuf_channel *channel;
	struct iobuf_module *module;
	struct spdk_iobuf_module_stats *it;
	uint32_t i, j;

	for (i = 0; i < ctx->num_modules; ++i) {
		for (j = 0; j < IOBUF_MAX_CHANNELS; ++j) {
			channel = iobuf_ch->channels[j];
			if (channel == NULL) {
				continue;
			}

			it = &ctx->modules[i];
			module = (struct iobuf_module *)channel->module;
			if (strcmp(it->module, module->name) == 0) {
				it->small_pool.cache += channel->small.stats.cache;
				it->small_pool.main += channel->small.stats.main;
				it->small_pool.retry += channel->small.stats.retry;
				it->large_pool.cache += channel->large.stats.cache;
				it->large_pool.main += channel->large.stats.main;
				it->large_pool.retry += channel->large.stats.retry;
				break;
			}
		}
	}

	spdk_for_each_channel_continue(iter, 0);
}

int
spdk_iobuf_get_stats(spdk_iobuf_get_stats_cb cb_fn, void *cb_arg)
{
	struct iobuf_module *module;
	struct iobuf_get_stats_ctx *ctx;
	uint32_t i;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {
		++ctx->num_modules;
	}

	ctx->modules = calloc(ctx->num_modules, sizeof(struct spdk_iobuf_module_stats));
	if (ctx->modules == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	i = 0;
	TAILQ_FOREACH(module, &g_iobuf.modules, tailq) {
		ctx->modules[i].module = module->name;
		++i;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(&g_iobuf, iobuf_get_channel_stats, ctx,
			      iobuf_get_channel_stats_done);
	return 0;
}
