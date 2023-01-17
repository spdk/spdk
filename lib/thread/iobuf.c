/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#define IOBUF_MIN_SMALL_POOL_SIZE	8191
#define IOBUF_MIN_LARGE_POOL_SIZE	1023
#define IOBUF_ALIGNMENT			512
#define IOBUF_MIN_SMALL_BUFSIZE		(SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_BDEV_SMALL_BUF_MAX_SIZE) + \
					 IOBUF_ALIGNMENT)
#define IOBUF_MIN_LARGE_BUFSIZE		(SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_BDEV_LARGE_BUF_MAX_SIZE) + \
					 IOBUF_ALIGNMENT)

SPDK_STATIC_ASSERT(sizeof(struct spdk_iobuf_buffer) <= IOBUF_MIN_SMALL_BUFSIZE,
		   "Invalid data offset");

struct iobuf_channel {
	spdk_iobuf_entry_stailq_t small_queue;
	spdk_iobuf_entry_stailq_t large_queue;
};

struct iobuf_module {
	char				*name;
	TAILQ_ENTRY(iobuf_module)	tailq;
};

struct iobuf {
	struct spdk_mempool		*small_pool;
	struct spdk_mempool		*large_pool;
	struct spdk_iobuf_opts		opts;
	TAILQ_HEAD(, iobuf_module)	modules;
	spdk_iobuf_finish_cb		finish_cb;
	void				*finish_arg;
};

static struct iobuf g_iobuf = {
	.modules = TAILQ_HEAD_INITIALIZER(g_iobuf.modules),
	.opts = {
		.small_pool_count = IOBUF_MIN_SMALL_POOL_SIZE,
		.large_pool_count = IOBUF_MIN_LARGE_POOL_SIZE,
		.small_bufsize = IOBUF_MIN_SMALL_BUFSIZE,
		.large_bufsize = IOBUF_MIN_LARGE_BUFSIZE,
	},
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

	g_iobuf.small_pool = spdk_mempool_create("iobuf_small_pool", opts->small_pool_count,
			     opts->small_bufsize, 0, SPDK_ENV_SOCKET_ID_ANY);
	if (!g_iobuf.small_pool) {
		SPDK_ERRLOG("Failed to create small iobuf pool\n");
		rc = -ENOMEM;
		goto error;
	}

	g_iobuf.large_pool = spdk_mempool_create("iobuf_large_pool", opts->large_pool_count,
			     opts->large_bufsize, 0, SPDK_ENV_SOCKET_ID_ANY);
	if (!g_iobuf.large_pool) {
		SPDK_ERRLOG("Failed to create large iobuf pool\n");
		rc = -ENOMEM;
		goto error;
	}

	spdk_io_device_register(&g_iobuf, iobuf_channel_create_cb, iobuf_channel_destroy_cb,
				sizeof(struct iobuf_channel), "iobuf");

	return 0;
error:
	spdk_mempool_free(g_iobuf.small_pool);
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

	if (spdk_mempool_count(g_iobuf.small_pool) != g_iobuf.opts.small_pool_count) {
		SPDK_ERRLOG("small iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_mempool_count(g_iobuf.small_pool), g_iobuf.opts.small_pool_count);
	}

	if (spdk_mempool_count(g_iobuf.large_pool) != g_iobuf.opts.large_pool_count) {
		SPDK_ERRLOG("large iobuf pool count is %zu, expected %"PRIu64"\n",
			    spdk_mempool_count(g_iobuf.large_pool), g_iobuf.opts.large_pool_count);
	}

	spdk_mempool_free(g_iobuf.small_pool);
	spdk_mempool_free(g_iobuf.large_pool);

	if (g_iobuf.finish_cb != NULL) {
		g_iobuf.finish_cb(g_iobuf.finish_arg);
	}
}

void
spdk_iobuf_finish(spdk_iobuf_finish_cb cb_fn, void *cb_arg)
{
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
	if (opts->small_bufsize < IOBUF_MIN_SMALL_BUFSIZE) {
		SPDK_ERRLOG("small_bufsize must be at least %" PRIu32 "\n",
			    IOBUF_MIN_SMALL_BUFSIZE);
		return -EINVAL;
	}
	if (opts->large_bufsize < IOBUF_MIN_LARGE_BUFSIZE) {
		SPDK_ERRLOG("large_bufsize must be at least %" PRIu32 "\n",
			    IOBUF_MIN_LARGE_BUFSIZE);
		return -EINVAL;
	}

	g_iobuf.opts = *opts;

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
		buf = spdk_mempool_get(g_iobuf.small_pool);
		if (buf == NULL) {
			SPDK_ERRLOG("Failed to populate iobuf small buffer cache. "
				    "You may need to increase spdk_iobuf_opts.small_pool_count\n");
			goto error;
		}
		STAILQ_INSERT_TAIL(&ch->small.cache, buf, stailq);
		ch->small.cache_count++;
	}
	for (i = 0; i < large_cache_size; ++i) {
		buf = spdk_mempool_get(g_iobuf.large_pool);
		if (buf == NULL) {
			SPDK_ERRLOG("Failed to populate iobuf large buffer cache. "
				    "You may need to increase spdk_iobuf_opts.large_pool_count\n");
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
		spdk_mempool_put(ch->small.pool, buf);
		ch->small.cache_count--;
	}
	while (!STAILQ_EMPTY(&ch->large.cache)) {
		buf = STAILQ_FIRST(&ch->large.cache);
		STAILQ_REMOVE_HEAD(&ch->large.cache, stailq);
		spdk_mempool_put(ch->large.pool, buf);
		ch->large.cache_count--;
	}

	assert(ch->small.cache_count == 0);
	assert(ch->large.cache_count == 0);

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
	} else {
		buf = spdk_mempool_get(pool->pool);
		if (!buf) {
			STAILQ_INSERT_TAIL(pool->queue, entry, stailq);
			entry->module = ch->module;
			entry->cb_fn = cb_fn;

			return NULL;
		}
	}

	return (char *)buf;
}

void
spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len)
{
	struct spdk_iobuf_entry *entry;
	struct spdk_iobuf_pool *pool;

	assert(spdk_io_channel_get_thread(ch->parent) == spdk_get_thread());
	if (len <= ch->small.bufsize) {
		pool = &ch->small;
	} else {
		pool = &ch->large;
	}

	if (STAILQ_EMPTY(pool->queue)) {
		if (pool->cache_count < pool->cache_size) {
			STAILQ_INSERT_HEAD(&pool->cache, (struct spdk_iobuf_buffer *)buf, stailq);
			pool->cache_count++;
		} else {
			spdk_mempool_put(pool->pool, buf);
		}
	} else {
		entry = STAILQ_FIRST(pool->queue);
		STAILQ_REMOVE_HEAD(pool->queue, stailq);
		entry->cb_fn(entry, buf);
	}
}
