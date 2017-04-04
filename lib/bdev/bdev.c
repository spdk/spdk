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

#include "spdk/bdev.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_version.h>

#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#define SPDK_BDEV_IO_POOL_SIZE	(64 * 1024)
#define RBUF_SMALL_POOL_SIZE	8192
#define RBUF_LARGE_POOL_SIZE	1024

static struct rte_mempool *spdk_bdev_g_io_pool = NULL;
static struct rte_mempool *g_rbuf_small_pool = NULL;
static struct rte_mempool *g_rbuf_large_pool = NULL;

typedef TAILQ_HEAD(, spdk_bdev_io) need_rbuf_tailq_t;
static need_rbuf_tailq_t g_need_rbuf_small[RTE_MAX_LCORE];
static need_rbuf_tailq_t g_need_rbuf_large[RTE_MAX_LCORE];

static TAILQ_HEAD(, spdk_bdev_module_if) spdk_bdev_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_bdev_module_list);
static TAILQ_HEAD(, spdk_bdev_module_if) spdk_vbdev_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_vbdev_module_list);

static TAILQ_HEAD(, spdk_bdev) spdk_bdev_list =
	TAILQ_HEAD_INITIALIZER(spdk_bdev_list);

struct spdk_bdev *spdk_bdev_first(void)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_FIRST(&spdk_bdev_list);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *spdk_bdev_next(struct spdk_bdev *prev)
{
	struct spdk_bdev *bdev;

	bdev = TAILQ_NEXT(prev, link);
	if (bdev) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing bdev iteration at %s\n", bdev->name);
	}

	return bdev;
}

struct spdk_bdev *spdk_bdev_get_by_name(const char *bdev_name)
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
spdk_bdev_io_set_rbuf(struct spdk_bdev_io *bdev_io, void *buf)
{
	assert(bdev_io->get_rbuf_cb != NULL);
	assert(buf != NULL);
	assert(bdev_io->u.read.iovs != NULL);

	bdev_io->u.read.buf_unaligned = buf;
	bdev_io->u.read.iovs[0].iov_base = (void *)((unsigned long)((char *)buf + 512) & ~511UL);
	bdev_io->u.read.iovs[0].iov_len = bdev_io->u.read.len;
	bdev_io->u.read.put_rbuf = true;
	bdev_io->get_rbuf_cb(bdev_io);
}

static void
spdk_bdev_io_put_rbuf(struct spdk_bdev_io *bdev_io)
{
	struct rte_mempool *pool;
	struct spdk_bdev_io *tmp;
	void *buf;
	need_rbuf_tailq_t *tailq;
	uint64_t length;

	assert(bdev_io->u.read.iovcnt == 1);

	length = bdev_io->u.read.len;
	buf = bdev_io->u.read.buf_unaligned;

	if (length <= SPDK_BDEV_SMALL_RBUF_MAX_SIZE) {
		pool = g_rbuf_small_pool;
		tailq = &g_need_rbuf_small[rte_lcore_id()];
	} else {
		pool = g_rbuf_large_pool;
		tailq = &g_need_rbuf_large[rte_lcore_id()];
	}

	if (TAILQ_EMPTY(tailq)) {
		rte_mempool_put(pool, buf);
	} else {
		tmp = TAILQ_FIRST(tailq);
		TAILQ_REMOVE(tailq, tmp, rbuf_link);
		spdk_bdev_io_set_rbuf(tmp, buf);
	}
}

static int spdk_initialize_rbuf_pool(void)
{
	int cache_size;

	/**
	 * Ensure no more than half of the total buffers end up local caches, by
	 *   using spdk_event_get_active_core_count() to determine how many local caches we need
	 *   to account for.
	 */
	cache_size = RBUF_SMALL_POOL_SIZE / (2 * spdk_env_get_core_count());
	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE)
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
	g_rbuf_small_pool = rte_mempool_create("rbuf_small_pool",
					       RBUF_SMALL_POOL_SIZE,
					       SPDK_BDEV_SMALL_RBUF_MAX_SIZE + 512,
					       cache_size, 0, NULL, NULL, NULL, NULL,
					       SOCKET_ID_ANY, 0);
	if (!g_rbuf_small_pool) {
		SPDK_ERRLOG("create rbuf small pool failed\n");
		return -1;
	}

	cache_size = RBUF_LARGE_POOL_SIZE / (2 * spdk_env_get_core_count());
	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE)
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
	g_rbuf_large_pool = rte_mempool_create("rbuf_large_pool",
					       RBUF_LARGE_POOL_SIZE,
					       SPDK_BDEV_LARGE_RBUF_MAX_SIZE + 512,
					       cache_size, 0, NULL, NULL, NULL, NULL,
					       SOCKET_ID_ANY, 0);
	if (!g_rbuf_large_pool) {
		SPDK_ERRLOG("create rbuf large pool failed\n");
		return -1;
	}

	return 0;
}

static int
spdk_bdev_module_get_max_ctx_size(void)
{
	struct spdk_bdev_module_if *bdev_module;
	int max_bdev_module_size = 0;

	TAILQ_FOREACH(bdev_module, &spdk_bdev_module_list, tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	TAILQ_FOREACH(bdev_module, &spdk_vbdev_module_list, tailq) {
		if (bdev_module->get_ctx_size && bdev_module->get_ctx_size() > max_bdev_module_size) {
			max_bdev_module_size = bdev_module->get_ctx_size();
		}
	}

	return max_bdev_module_size;
}

static int
spdk_bdev_module_initialize(void)
{
	struct spdk_bdev_module_if *bdev_module;
	int rc = 0;

	TAILQ_FOREACH(bdev_module, &spdk_bdev_module_list, tailq) {
		rc = bdev_module->module_init();
		if (rc)
			return rc;
	}
	TAILQ_FOREACH(bdev_module, &spdk_vbdev_module_list, tailq) {
		rc = bdev_module->module_init();
		if (rc)
			return rc;
	}
	return rc;
}

static void
spdk_bdev_module_finish(void)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &spdk_vbdev_module_list, tailq) {
		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}
	}

	TAILQ_FOREACH(bdev_module, &spdk_bdev_module_list, tailq) {
		if (bdev_module->module_fini) {
			bdev_module->module_fini();
		}
	}
}

static void
spdk_bdev_config_text(FILE *fp)
{
	struct spdk_bdev_module_if *bdev_module;

	TAILQ_FOREACH(bdev_module, &spdk_bdev_module_list, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
	TAILQ_FOREACH(bdev_module, &spdk_vbdev_module_list, tailq) {
		if (bdev_module->config_text) {
			bdev_module->config_text(fp);
		}
	}
}

static int
spdk_bdev_initialize(void)
{
	int i;

	if (spdk_bdev_module_initialize()) {
		SPDK_ERRLOG("bdev module initialize failed");
		return -1;
	}

	spdk_bdev_g_io_pool = rte_mempool_create("blockdev_io",
			      SPDK_BDEV_IO_POOL_SIZE,
			      sizeof(struct spdk_bdev_io) +
			      spdk_bdev_module_get_max_ctx_size(),
			      64, 0,
			      NULL, NULL, NULL, NULL,
			      SOCKET_ID_ANY, 0);

	if (spdk_bdev_g_io_pool == NULL) {
		SPDK_ERRLOG("could not allocate spdk_bdev_io pool");
		return -1;
	}

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		TAILQ_INIT(&g_need_rbuf_small[i]);
		TAILQ_INIT(&g_need_rbuf_large[i]);
	}

	return spdk_initialize_rbuf_pool();
}

/*
 * Wrapper to provide rte_mempool_avail_count() on older DPDK versions.
 * Drop this if the minimum DPDK version is raised to at least 16.07.
 */
#if RTE_VERSION < RTE_VERSION_NUM(16, 7, 0, 1)
static unsigned rte_mempool_avail_count(const struct rte_mempool *pool)
{
	return rte_mempool_count(pool);
}
#endif

static int
spdk_bdev_check_pool(struct rte_mempool *pool, uint32_t count)
{
	if (rte_mempool_avail_count(pool) != count) {
		SPDK_ERRLOG("rte_mempool_avail_count(%s) == %d, should be %d\n",
			    pool->name, rte_mempool_avail_count(pool), count);
		return -1;
	} else {
		return 0;
	}
}

static int
spdk_bdev_finish(void)
{
	int rc = 0;

	spdk_bdev_module_finish();

	rc += spdk_bdev_check_pool(g_rbuf_small_pool, RBUF_SMALL_POOL_SIZE);
	rc += spdk_bdev_check_pool(g_rbuf_large_pool, RBUF_LARGE_POOL_SIZE);

	return (rc != 0);
}

struct spdk_bdev_io *spdk_bdev_get_io(void)
{
	struct spdk_bdev_io *bdev_io;
	int rc;

	rc = rte_mempool_get(spdk_bdev_g_io_pool, (void **)&bdev_io);
	if (rc < 0 || !bdev_io) {
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

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ && bdev_io->u.read.put_rbuf) {
		spdk_bdev_io_put_rbuf(bdev_io);
	}

	rte_mempool_put(spdk_bdev_g_io_pool, bdev_io);
}

static void
_spdk_bdev_io_get_rbuf(struct spdk_bdev_io *bdev_io)
{
	uint64_t len = bdev_io->u.read.len;
	struct rte_mempool *pool;
	need_rbuf_tailq_t *tailq;
	int rc;
	void *buf = NULL;

	if (len <= SPDK_BDEV_SMALL_RBUF_MAX_SIZE) {
		pool = g_rbuf_small_pool;
		tailq = &g_need_rbuf_small[rte_lcore_id()];
	} else {
		pool = g_rbuf_large_pool;
		tailq = &g_need_rbuf_large[rte_lcore_id()];
	}

	rc = rte_mempool_get(pool, (void **)&buf);
	if (rc < 0 || !buf) {
		TAILQ_INSERT_TAIL(tailq, bdev_io, rbuf_link);
	} else {
		spdk_bdev_io_set_rbuf(bdev_io, buf);
	}
}


static void
spdk_bdev_cleanup_pending_rbuf_io(struct spdk_bdev *bdev)
{
	struct spdk_bdev_io *bdev_io, *tmp;

	TAILQ_FOREACH_SAFE(bdev_io, &g_need_rbuf_small[rte_lcore_id()], rbuf_link, tmp) {
		if (bdev_io->bdev == bdev) {
			TAILQ_REMOVE(&g_need_rbuf_small[rte_lcore_id()], bdev_io, rbuf_link);
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	TAILQ_FOREACH_SAFE(bdev_io, &g_need_rbuf_large[rte_lcore_id()], rbuf_link, tmp) {
		if (bdev_io->bdev == bdev) {
			TAILQ_REMOVE(&g_need_rbuf_large[rte_lcore_id()], bdev_io, rbuf_link);
			bdev_io->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}
}

static void
__submit_request(struct spdk_bdev *bdev, struct spdk_bdev_io *bdev_io)
{
	assert(bdev_io->status == SPDK_BDEV_IO_STATUS_PENDING);
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		spdk_bdev_cleanup_pending_rbuf_io(bdev);
	}
	bdev_io->in_submit_request = true;
	bdev->fn_table->submit_request(bdev_io);
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
	bdev_io->ctx = new_bdev->ctxt;

	__submit_request(new_bdev, bdev_io);
}

static void
spdk_bdev_io_init(struct spdk_bdev_io *bdev_io,
		  struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb)
{
	bdev_io->bdev = bdev;
	bdev_io->ctx = bdev->ctxt;
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
	if (child->type == SPDK_BDEV_IO_TYPE_READ) {
		child->u.read.put_rbuf = false;
	}
	child->get_rbuf_cb = NULL;
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

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	return bdev->fn_table->get_io_channel(bdev->ctxt, priority);
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

	bdev_io->ch = ch;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.read.iov.iov_base = buf;
	bdev_io->u.read.iov.iov_len = nbytes;
	bdev_io->u.read.iovs = &bdev_io->u.read.iov;
	bdev_io->u.read.iovcnt = 1;
	bdev_io->u.read.len = nbytes;
	bdev_io->u.read.offset = offset;
	bdev_io->u.read.put_rbuf = false;
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

	bdev_io->ch = ch;
	bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	bdev_io->u.read.iovs = iov;
	bdev_io->u.read.iovcnt = iovcnt;
	bdev_io->u.read.len = nbytes;
	bdev_io->u.read.offset = offset;
	bdev_io->u.read.put_rbuf = false;
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

	bdev_io->ch = ch;
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

	bdev_io->ch = ch;
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

	bdev_io->ch = ch;
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
	int rc;

	assert(bdev->status != SPDK_BDEV_STATUS_UNCLAIMED);
	bdev_io = spdk_bdev_get_io();
	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing flush\n");
		return NULL;
	}

	bdev_io->ch = ch;
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
		 * Child I/O may have an rbuf that needs to be returned to a pool
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
spdk_bdev_io_set_scsi_error(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
			    enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq)
{
	bdev_io->status = SPDK_BDEV_IO_STATUS_SCSI_ERROR;
	bdev_io->error.scsi.sc = sc;
	bdev_io->error.scsi.sk = sk;
	bdev_io->error.scsi.asc = asc;
	bdev_io->error.scsi.ascq = ascq;
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

	pthread_mutex_init(&bdev->mutex, NULL);
	bdev->status = SPDK_BDEV_STATUS_UNCLAIMED;
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Inserting bdev %s into list\n", bdev->name);
	TAILQ_INSERT_TAIL(&spdk_bdev_list, bdev, link);
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

	TAILQ_REMOVE(&spdk_bdev_list, bdev, link);
	pthread_mutex_unlock(&bdev->mutex);

	pthread_mutex_destroy(&bdev->mutex);

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
spdk_bdev_io_get_rbuf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_rbuf_cb cb)
{
	assert(cb != NULL);
	assert(bdev_io->u.read.iovs != NULL);

	if (bdev_io->u.read.iovs[0].iov_base == NULL) {
		bdev_io->get_rbuf_cb = cb;
		_spdk_bdev_io_get_rbuf(bdev_io);
	} else {
		cb(bdev_io);
	}
}

void spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module)
{
	TAILQ_INSERT_TAIL(&spdk_bdev_module_list, bdev_module, tailq);
}

void spdk_vbdev_module_list_add(struct spdk_bdev_module_if *vbdev_module)
{
	TAILQ_INSERT_TAIL(&spdk_vbdev_module_list, vbdev_module, tailq);
}
SPDK_SUBSYSTEM_REGISTER(bdev, spdk_bdev_initialize, spdk_bdev_finish, spdk_bdev_config_text)
SPDK_SUBSYSTEM_DEPEND(bdev, copy)
