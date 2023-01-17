/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */
#include "spdk/stdinc.h"

#include "spdk_internal/mock.h"

#include "spdk/thread.h"

DEFINE_STUB(spdk_iobuf_initialize, int, (void), 0);
DEFINE_STUB(spdk_iobuf_register_module, int, (const char *name), 0);
DEFINE_STUB(spdk_iobuf_unregister_module, int, (const char *name), 0);
DEFINE_STUB_V(spdk_iobuf_channel_fini, (struct spdk_iobuf_channel *ch));
DEFINE_STUB(spdk_iobuf_for_each_entry, int, (struct spdk_iobuf_channel *ch,
		struct spdk_iobuf_pool *pool, spdk_iobuf_for_each_entry_fn cb_fn, void *cb_ctx), 0);
DEFINE_STUB_V(spdk_iobuf_entry_abort, (struct spdk_iobuf_channel *ch,
				       struct spdk_iobuf_entry *entry, uint64_t len));

struct ut_iobuf {
	struct spdk_iobuf_opts	opts;
	uint32_t		small_pool_count;
	uint32_t		large_pool_count;
};

static struct ut_iobuf g_iobuf = {
	.small_pool_count = 32,
	.large_pool_count = 32
};
static spdk_iobuf_entry_stailq_t g_iobuf_entries;

int
spdk_iobuf_set_opts(const struct spdk_iobuf_opts *opts)
{
	g_iobuf.opts = *opts;
	g_iobuf.small_pool_count = opts->small_pool_count;
	g_iobuf.large_pool_count = opts->large_pool_count;
	return 0;
}

void
spdk_iobuf_get_opts(struct spdk_iobuf_opts *opts)
{
	*opts = g_iobuf.opts;
}

void
spdk_iobuf_finish(spdk_iobuf_finish_cb cb_fn, void *cb_arg)
{
	cb_fn(cb_arg);
}

int
spdk_iobuf_channel_init(struct spdk_iobuf_channel *ch, const char *name,
			uint32_t small_cache_size, uint32_t large_cache_size)
{
	STAILQ_INIT(&g_iobuf_entries);
	ch->small.cache_count = small_cache_size;
	ch->small.cache_size = small_cache_size;
	ch->large.cache_count = large_cache_size;
	ch->large.cache_size = large_cache_size;
	return 0;
}

DEFINE_RETURN_MOCK(spdk_iobuf_get, void *);
void *
spdk_iobuf_get(struct spdk_iobuf_channel *ch, uint64_t len,
	       struct spdk_iobuf_entry *entry, spdk_iobuf_get_cb cb_fn)
{
	struct spdk_iobuf_pool *pool;
	uint32_t *count;
	void *buf;

	HANDLE_RETURN_MOCK(spdk_iobuf_get);

	if (len > g_iobuf.opts.small_bufsize) {
		pool = &ch->large;
		count = &g_iobuf.large_pool_count;
	} else {
		pool = &ch->small;
		count = &g_iobuf.small_pool_count;
	}

	if (pool->cache_count > 0) {
		buf = calloc(1, len);
		CU_ASSERT(buf != NULL);
		pool->cache_count--;
		return buf;
	}

	if (*count == 0) {
		if (entry) {
			entry->cb_fn = cb_fn;
			STAILQ_INSERT_TAIL(&g_iobuf_entries, entry, stailq);
		}

		return NULL;
	}

	buf = calloc(1, len);
	CU_ASSERT(buf != NULL);
	(*count)--;
	return buf;
}

void
spdk_iobuf_put(struct spdk_iobuf_channel *ch, void *buf, uint64_t len)
{
	struct spdk_iobuf_entry *entry;
	struct spdk_iobuf_pool *pool;
	uint32_t *count;

	if (len > g_iobuf.opts.small_bufsize) {
		pool = &ch->large;
		count = &g_iobuf.large_pool_count;
	} else {
		pool = &ch->small;
		count = &g_iobuf.small_pool_count;
	}

	if (!STAILQ_EMPTY(&g_iobuf_entries)) {
		entry = STAILQ_FIRST(&g_iobuf_entries);
		STAILQ_REMOVE_HEAD(&g_iobuf_entries, stailq);
		entry->cb_fn(entry, buf);
		return;
	}

	if (pool->cache_count < pool->cache_size) {
		pool->cache_count++;
	} else {
		(*count)++;
	}

	free(buf);
}
