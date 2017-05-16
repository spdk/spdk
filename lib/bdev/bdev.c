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

#include <rte_config.h>
#include <rte_lcore.h>
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/scsi_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#define SPDK_BDEV_IO_POOL_SIZE	(64 * 1024)
#define BUF_SMALL_POOL_SIZE	8192
#define BUF_LARGE_POOL_SIZE	1024

typedef TAILQ_HEAD(, spdk_bdev_io) need_buf_tailq_t;

struct spdk_bdev_mgr {
	struct spdk_mempool *bdev_io_pool;

	struct spdk_mempool *buf_small_pool;
	struct spdk_mempool *buf_large_pool;

	need_buf_tailq_t need_buf_small[RTE_MAX_LCORE];
	need_buf_tailq_t need_buf_large[RTE_MAX_LCORE];

	TAILQ_HEAD(, spdk_bdev_module_if) bdev_modules;
	TAILQ_HEAD(, spdk_bdev_module_if) vbdev_modules;

	TAILQ_HEAD(, spdk_bdev) bdevs;
};

static struct spdk_bdev_mgr g_bdev_mgr = {
	.bdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdev_modules),
	.vbdev_modules = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.vbdev_modules),
	.bdevs = TAILQ_HEAD_INITIALIZER(g_bdev_mgr.bdevs),
};

struct spdk_bdev_channel {
	struct spdk_bdev	*bdev;

	/* The channel for the underlying device */
	struct spdk_io_channel	*channel;
};

struct spdk_bdev *
spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_FIRST(&g_bdev_mgr.bdevs);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_NEXT(prev, link);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev = spdk_bdev_first();

	while (bdev != NULL) {
		if (strncmp(bdev_name, bdev->name, sizeof(bdev->name)) == 0) {
			return bdev;
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
	assert(bdev_io->u.read.iovs != NULL);

	bdev_io->buf = buf;
	bdev_io->u.read.iovs[0].iov_base = (void *)((unsigned long)((char *)buf + 512) & ~511UL);
	bdev_io->u.read.iovs[0].iov_len = bdev_io->u.read.len;
	bdev_io->get_buf_cb(bdev_io->ch->channel, bdev_io);
}

static void
spdk_bdev_io_put_buf(struct spdk_bdev_io *bdev_io)
{
	struct spdk_mempool *pool;
	struct spdk_bdev_io *tmp;
	void *buf;
	need_buf_tailq_t *tailq;
	uint64_t length;

	assert(bdev_io->u.read.iovcnt == 1);

	length = bdev_io->u.read.len;
	buf = bdev_io->buf;

	if (length <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		tailq = &g_bdev_mgr.need_buf_small[rte_lcore_id()];
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		tailq = &g_bdev_mgr.need_buf_large[rte_lcore_id()];
	}

	if (TAILQ_EMPTY(tailq)) {
		spdk_mempool_put(pool, buf);
	} else {
		tmp = TAILQ_FIRST(tailq);
		TAILQ_REMOVE(tailq, tmp, buf_link);
		spdk_bdev_io_set_buf(tmp, buf);
	}
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb)
{
	uint64_t len = bdev_io->u.read.len;
	struct spdk_mempool *pool;
	need_buf_tailq_t *tailq;
	void *buf = NULL;

	assert(cb != NULL);
	assert(bdev_io->u.read.iovs != NULL);

	if (spdk_unlikely(bdev_io->u.read.iovs[0].iov_base != NULL)) {
		/* Buffer already present */
		cb(bdev_io->ch->channel, bdev_io);
		return;
	}

	bdev_io->get_buf_cb = cb;
	if (len <= SPDK_BDEV_SMALL_BUF_MAX_SIZE) {
		pool = g_bdev_mgr.buf_small_pool;
		tailq = &g_bdev_mgr.need_buf_small[rte_lcore_id()];
	} else {
		pool = g_bdev_mgr.buf_large_pool;
		tailq = &g_bdev_mgr.need_buf_large[rte_lcore_id()];
	}

	buf = spdk_mempool_get(pool);

	if (!buf) {
		TAILQ_INSERT_TAIL(tailq, bdev_io, buf_link);
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

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.vbdev_modules, tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
}

static void
spdk_bdev_config_text(FILE *fp)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.vbdev_modules, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
}

static int
spdk_bdev_initialize(void)
{
	int i, cache_size;
	struct spdk_bdev_module_if *bdev_module;
	int rc = 0;

	g_bdev_mgr.bdev_io_pool = spdk_mempool_create("blockdev_io",
				  SPDK_BDEV_IO_POOL_SIZE,
				  sizeof(struct spdk_bdev_io) +
				  spdk_bdev_module_get_max_ctx_size(),
				  64,
				  SPDK_ENV_SOCKET_ID_ANY);

	if (g_bdev_mgr.bdev_io_pool == NULL) {
		SPDK_ERRLOG("could not allocate spdk_bdev_io pool");
		return -1;
	}

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		TAILQ_INIT(&g_bdev_mgr.need_buf_small[i]);
		TAILQ_INIT(&g_bdev_mgr.need_buf_large[i]);
	}

	/**
	 * Ensure no more than half of the total buffers end up local caches, by
	 *   using spdk_env_get_core_count() to determine how many local caches we need
	 *   to account for.
	 */
	cache_size = BUF_SMALL_POOL_SIZE / (2 * spdk_env_get_core_count());
	g_bdev_mgr.buf_small_pool = spdk_mempool_create("buf_small_pool",
				    BUF_SMALL_POOL_SIZE,
				    SPDK_BDEV_SMALL_BUF_MAX_SIZE + 512,
				    cache_size,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!g_bdev_mgr.buf_small_pool) {
		SPDK_ERRLOG("create rbuf small pool failed\n");
		return -1;
	}

	cache_size = BUF_LARGE_POOL_SIZE / (2 * spdk_env_get_core_count());
	g_bdev_mgr.buf_large_pool = spdk_mempool_create("buf_large_pool",
				    BUF_LARGE_POOL_SIZE,
				    SPDK_BDEV_LARGE_BUF_MAX_SIZE + 512,
				    cache_size,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!g_bdev_mgr.buf_large_pool) {
		SPDK_ERRLOG("create rbuf large pool failed\n");
		return -1;
	}

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		rc = bdev_module->module_init();
		if (rc) {
			return rc;
		}
	}
	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.vbdev_modules, tailq) {
		rc = bdev_module->module_init();
		if (rc) {
			return rc;
		}
	}

	return 0;
}

static int
spdk_bdev_finish(void)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.vbdev_modules, tailq) {
		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}
	}

	TAILQ_FOREACH(bdev_module, &g_bdev_mgr.bdev_modules, tailq) {
		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}
	}

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

	return 0;
}

struct spdk_bdev_io *
spdk_bdev_get_io(void)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = spdk_mempool_get(g_bdev_mgr.bdev_io_pool);
	if (!bdev_io) {
		SPDK_ERRLOG("Unable to get spdk_bdev_io\n");
		abort();
	}

	memset(bdev_io, 0, sizeof(*bdev_io));

	return bdev_io;
}

static void
spdk_bdev_put_io(struct spdk_bdev_io *bdev_io)
{
	if (!bdev_io) {
		return;
	}

	if (bdev_io->buf != NULL) {
		spdk_bdev_io_put_buf(bdev_io);
	}

	spdk_mempool_put(g_bdev_mgr.bdev_io_pool, (void *)bdev_io);
}

static void
spdk_bdev_cleanup_pending_buf_io(struct spdk_bdev *bdev)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	TAILQ_FOREACH_SAFE(bdev_io, &g_bdev_mgr.need_buf_small[rte_lcore_id()], buf_link, tmp) {
		if (bdev_io->bdev == bdev) {
			TAILQ_REMOVE(&g_bdev_mgr.need_buf_small[rte_lcore_id()], bdev_io, buf_link);
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	TAILQ_FOREACH_SAFE(bdev_io, &g_bdev_mgr.need_buf_large[rte_lcore_id()], buf_link, tmp) {
		if (bdev_io->bdev == bdev) {
			TAILQ_REMOVE(&g_bdev_mgr.need_buf_large[rte_lcore_id()], bdev_io, buf_link);
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}
}

static void
__submit_request(struct spdk_bdev *bdev, struct spdk_bdev_io *bdev_io)
{
	struct spdk_io_channel *ch;

	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		spdk_bdev_cleanup_pending_buf_io(bdev);
		ch = NULL;
	} else {
		ch = bdev_io->ch->channel;
	}

	bdev_io->in_submit_request = true;
	bdev->fn_table->submit_request(ch, bdev_io);
	bdev_io->in_submit_request = false;
}

static int
spdk_bdev_io_submit(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;

	__submit_request(bdev, bdev_io);
	return 0;
}

void
spdk_bdev_io_resubmit(struct spdk_bdev_io *bdev_io, struct spdk_bdev *new_bdev)
{
	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);
	bdev_io->bdev = new_bdev;

	/*
	 * These fields are normally set during spdk_bdev_io_init(), but since bdev is
	 * being switched, they need to be reinitialized.
	 */
	bdev_io->gencnt = new_bdev->gencnt;

	__submit_request(new_bdev, bdev_io);
}

static void
spdk_bdev_io_init(struct spdk_bdev_io *bdev_io,
		  struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	bdev_io->caller_ctx = cb_arg;
	bdev_io->cb = cb;
	bdev_io->gencnt = bdev->gencnt;
	bdev_io->status = SPDK_BDEV_IO_STATUS_PENDING;
	bdev_io->in_submit_request = false;
	TAILQ_INIT(&bdev_io->child_io);
}

struct spdk_bdev_io *
spdk_bdev_get_child_io(struct spdk_bdev_io *parent,
		       struct spdk_bdev *bdev,
		       spdk_bdev_io_completion_cb cb,
		       void *cb_arg)
{
	struct spdk_bdev_io *child;

	child = spdk_bdev_get_io();
	if (!child) {
		SPDK_ERRLOG("Unable to get spdk_bdev_io\n");
		return NULL;
	}

	if (cb_arg == NULL) {
		cb_arg = child;
	}

	spdk_bdev_io_init(child, bdev, cb_arg, cb);

	child->type = parent->type;
	memcpy(&child->u, &parent->u, sizeof(child->u));
	child->buf = NULL;
	child->get_buf_cb = NULL;
	child->parent = parent;

	TAILQ_INSERT_TAIL(&parent->child_io, child, link);

	return child;
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
spdk_bdev_channel_create(void *io_device, uint32_t priority, void *ctx_buf,
			 void *unique_ctx)
{
	struct spdk_bdev		*bdev = io_device;
	struct spdk_bdev_channel	*ch = ctx_buf;

	ch->bdev = io_device;
	ch->channel = bdev->fn_table->get_io_channel(bdev->ctxt, priority);

	return 0;
}

static void
spdk_bdev_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_channel	*ch = ctx_buf;

	spdk_put_io_channel(ch->channel);
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	return spdk_get_io_channel(bdev, priority, false, NULL);
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

uint32_t
spdk_bdev_get_max_unmap_descriptors(const struct spdk_bdev *bdev)
{
	return bdev->max_unmap_bdesc_count;
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

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return bdev->write_cache;
}

static int
spdk_bdev_io_valid(struct spdk_bdev *bdev, uint64_t offset, uint64_t nbytes)
{
	/* Return failure if nbytes is not a multiple of bdev->blocklen */
	if (nbytes % bdev->blocklen) {
		return -1;
	}

	/* Return failure if offset + nbytes is less than offset; indicates there
	 * has been an overflow and hence the offset has been wrapped around */
	if (offset + nbytes < offset) {
		return -1;
	}

	/* Return failure if offset + nbytes exceeds the size of the blockdev */
	if (offset + nbytes > bdev->blockcnt * bdev->blocklen) {
		return -1;
	}

	return 0;
}

struct spdk_bdev_io *
spdk_bdev_read(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
	       void *buf, uint64_t offset, uint64_t nbytes,
	       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed duing read\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.read.iov.iov_base = buf;
	bdev_io->u.read.iov.iov_len = nbytes;
	bdev_io->u.read.iovs = &bdev_io->u.read.iov;
	bdev_io->u.read.iovcnt = 1;
	bdev_io->u.read.len = nbytes;
	bdev_io->u.read.offset = offset;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

struct spdk_bdev_io *
spdk_bdev_readv(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("spdk_bdev_io memory allocation failed duing read\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.read.iovs = iov;
	bdev_io->u.read.iovcnt = iovcnt;
	bdev_io->u.read.len = nbytes;
	bdev_io->u.read.offset = offset;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

struct spdk_bdev_io *
spdk_bdev_write(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		void *buf, uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	if (spdk_bdev_io_valid(bdev, offset, nbytes) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("blockdev_io memory allocation failed duing write\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.write.iov.iov_base = buf;
	bdev_io->u.write.iov.iov_len = nbytes;
	bdev_io->u.write.iovs = &bdev_io->u.write.iov;
	bdev_io->u.write.iovcnt = 1;
	bdev_io->u.write.len = nbytes;
	bdev_io->u.write.offset = offset;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

struct spdk_bdev_io *
spdk_bdev_writev(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		 struct iovec *iov, int iovcnt,
		 uint64_t offset, uint64_t len,
		 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	if (spdk_bdev_io_valid(bdev, offset, len) != 0) {
		return NULL;
	}

	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing writev\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	bdev_io->u.write.iovs = iov;
	bdev_io->u.write.iovcnt = iovcnt;
	bdev_io->u.write.len = len;
	bdev_io->u.write.offset = offset;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

struct spdk_bdev_io *
spdk_bdev_unmap(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_scsi_unmap_bdesc *unmap_d,
		uint16_t bdesc_count,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	if (bdesc_count == 0) {
		SPDK_ERRLOG("Invalid bdesc_count 0\n");
		return NULL;
	}

	if (bdesc_count > bdev->max_unmap_bdesc_count) {
		SPDK_ERRLOG("Invalid bdesc_count %u > max_unmap_bdesc_count %u\n",
			    bdesc_count, bdev->max_unmap_bdesc_count);
		return NULL;
	}

	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing unmap\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	bdev_io->u.unmap.unmap_bdesc = unmap_d;
	bdev_io->u.unmap.bdesc_count = bdesc_count;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

struct spdk_bdev_io *
spdk_bdev_flush(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		uint64_t offset, uint64_t length,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing flush\n");
		return NULL;
	}

	bdev_io->ch = channel;
	bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	bdev_io->u.flush.offset = offset;
	bdev_io->u.flush.length = length;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		return NULL;
	}

	return bdev_io;
}

int
spdk_bdev_reset(struct spdk_bdev *bdev, enum spdk_bdev_reset_type reset_type,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing reset\n");
		return -1;
	}

	bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	bdev_io->u.reset.type = reset_type;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	rc = spdk_bdev_io_submit(bdev_io);
	if (rc < 0) {
		spdk_bdev_put_io(bdev_io);
		SPDK_ERRLOG("reset failed\n");
	}

	return rc;
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *child_io, *tmp;

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io is NULL\n");
		return -1;
	}

	if (bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING) {
		SPDK_ERRLOG("bdev_io is in pending state\n");
		assert(false);
		return -1;
	}

	TAILQ_FOREACH_SAFE(child_io, &bdev_io->child_io, link, tmp) {
		/*
		 * Make sure no references to the parent I/O remain, since it is being
		 * returned to the free pool.
		 */
		child_io->parent = NULL;
		TAILQ_REMOVE(&bdev_io->child_io, child_io, link);

		/*
		 * Child I/O may have a buf that needs to be returned to a pool
		 *  on a different core, so free it through the request submission
		 *  process rather than calling put_io directly here.
		 */
		spdk_bdev_free_io(child_io);
	}

	spdk_bdev_put_io(bdev_io);

	return 0;
}

static void
bdev_io_deferred_completion(void *arg1, void *arg2)
{
	struct spdk_bdev_io *bdev_io = arg1;
	enum spdk_bdev_io_status status = (enum spdk_bdev_io_status)arg2;

	assert(bdev_io->in_submit_request == false);

	spdk_bdev_io_complete(bdev_io, status);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	if (bdev_io->in_submit_request) {
		/*
		 * Defer completion via an event to avoid potential infinite recursion if the
		 * user's completion callback issues a new I/O.
		 */
		spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(),
						    bdev_io_deferred_completion,
						    bdev_io,
						    (void *)status));
		return;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		/* Successful reset */
		if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
			/* Increase the blockdev generation if it is a hard reset */
			if (bdev_io->u.reset.type == SPDK_BDEV_RESET_HARD) {
				bdev_io->bdev->gencnt++;
			}
		}
	} else {
		/*
		 * Check the gencnt, to see if this I/O was issued before the most
		 * recent reset. If the gencnt is not equal, then just free the I/O
		 * without calling the callback, since the caller will have already
		 * freed its context for this I/O.
		 */
		if (bdev_io->bdev->gencnt != bdev_io->gencnt) {
			spdk_bdev_put_io(bdev_io);
			return;
		}
	}

	bdev_io->status = status;

	assert(bdev_io->cb != NULL);
	bdev_io->cb(bdev_io, status, bdev_io->caller_ctx);
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

void
spdk_bdev_register(struct spdk_bdev *bdev)
{
	/* initialize the reset generation value to zero */
	bdev->gencnt = 0;

	spdk_io_device_register(bdev, spdk_bdev_channel_create, spdk_bdev_channel_destroy,
				sizeof(struct spdk_bdev_channel));

	pthread_mutex_init(&bdev->mutex, NULL);
	bdev->status = SPDK_BDEV_STATUS_UNCLAIMED;
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdevs, bdev, link);
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev)
{
	int			rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Removing bdev %s from list\n", bdev->name);

	pthread_mutex_lock(&bdev->mutex);
	assert(bdev->status == SPDK_BDEV_STATUS_CLAIMED || bdev->status == SPDK_BDEV_STATUS_UNCLAIMED);
	if (bdev->status == SPDK_BDEV_STATUS_CLAIMED) {
		if (bdev->remove_cb) {
			bdev->status = SPDK_BDEV_STATUS_REMOVING;
			pthread_mutex_unlock(&bdev->mutex);
			bdev->remove_cb(bdev->remove_ctx);
			return;
		} else {
			bdev->status = SPDK_BDEV_STATUS_UNCLAIMED;
		}
	}

	TAILQ_REMOVE(&g_bdev_mgr.bdevs, bdev, link);
	pthread_mutex_unlock(&bdev->mutex);

	pthread_mutex_destroy(&bdev->mutex);

	spdk_io_device_unregister(bdev);

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc < 0) {
		SPDK_ERRLOG("destruct failed\n");
	}
}

bool
spdk_bdev_claim(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb,
		void *remove_ctx)
{
	bool success;

	pthread_mutex_lock(&bdev->mutex);

	if (bdev->status != SPDK_BDEV_STATUS_CLAIMED) {
		/* Take ownership of bdev. */
		bdev->remove_cb = remove_cb;
		bdev->remove_ctx = remove_ctx;
		bdev->status = SPDK_BDEV_STATUS_CLAIMED;
		success = true;
	} else {
		/* bdev is already claimed. */
		success = false;
	}

	pthread_mutex_unlock(&bdev->mutex);

	return success;
}

void
spdk_bdev_unclaim(struct spdk_bdev *bdev)
{
	bool do_unregister = false;

	pthread_mutex_lock(&bdev->mutex);
	assert(bdev->status == SPDK_BDEV_STATUS_CLAIMED || bdev->status == SPDK_BDEV_STATUS_REMOVING);
	if (bdev->status == SPDK_BDEV_STATUS_REMOVING) {
		do_unregister = true;
	}
	bdev->remove_cb = NULL;
	bdev->remove_ctx = NULL;
	bdev->status = SPDK_BDEV_STATUS_UNCLAIMED;
	pthread_mutex_unlock(&bdev->mutex);

	if (do_unregister == true) {
		spdk_bdev_unregister(bdev);
	}
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
		iovs = bdev_io->u.read.iovs;
		iovcnt = bdev_io->u.read.iovcnt;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		iovs = bdev_io->u.write.iovs;
		iovcnt = bdev_io->u.write.iovcnt;
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
	TAILQ_INSERT_TAIL(&g_bdev_mgr.bdev_modules, bdev_module, tailq);
}

void
spdk_vbdev_module_list_add(struct spdk_bdev_module_if *vbdev_module)
{
	TAILQ_INSERT_TAIL(&g_bdev_mgr.vbdev_modules, vbdev_module, tailq);
}
SPDK_SUBSYSTEM_REGISTER(bdev, spdk_bdev_initialize, spdk_bdev_finish, spdk_bdev_config_text)
SPDK_SUBSYSTEM_DEPEND(bdev, copy)
