/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "aio_mgr.h"
#include <libaio.h>

#define MAX_EVENTS 1024

struct spdk_aio_mgr_io {
	struct spdk_aio_mgr *mgr;
	TAILQ_ENTRY(spdk_aio_mgr_io) link;
	struct iocb io;
	fsdev_aio_done_cb clb;
	void *ctx;
	uint32_t data_size;
	int err;
};

struct spdk_aio_mgr {
	TAILQ_HEAD(, spdk_aio_mgr_io) in_flight;
	io_context_t io_ctx;
	struct {
		struct spdk_aio_mgr_io *arr;
		uint32_t size;
		TAILQ_HEAD(, spdk_aio_mgr_io) pool;
	} aios;
};

static struct spdk_aio_mgr_io *
aio_mgr_get_aio(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx)
{
	struct spdk_aio_mgr_io *aio = TAILQ_FIRST(&mgr->aios.pool);

	if (aio) {
		aio->mgr = mgr;
		aio->clb = clb;
		aio->ctx = ctx;
		aio->err = 0;
		aio->data_size = 0;
		TAILQ_REMOVE(&mgr->aios.pool, aio, link);
	}

	return aio;
}

static inline void
aio_mgr_put_aio(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_io *aio)
{
	TAILQ_INSERT_TAIL(&aio->mgr->aios.pool, aio, link);
}

static void
spdk_aio_mgr_io_cpl_cb(io_context_t ctx, struct iocb *iocb, long res, long res2)
{
	struct spdk_aio_mgr_io *aio = SPDK_CONTAINEROF(iocb, struct spdk_aio_mgr_io, io);

	TAILQ_REMOVE(&aio->mgr->in_flight, aio, link);

	aio->clb(aio->ctx, res, -res2);

	aio_mgr_put_aio(aio->mgr, aio);
}

static struct spdk_aio_mgr_io *
spdk_aio_mgr_submit_io(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx, int fd,
		       uint64_t offs, uint32_t size, struct iovec *iovs, uint32_t iovcnt, bool read)
{
	struct spdk_aio_mgr_io *aio;
	int res;
	struct iocb *ios[1];

	SPDK_DEBUGLOG(spdk_aio_mgr_io, "%s: fd=%d offs=%" PRIu64 " size=%" PRIu32 " iovcnt=%" PRIu32 "\n",
		      read ? "read" : "write", fd, offs, size, iovcnt);

	aio = aio_mgr_get_aio(mgr, clb, ctx);
	if (!aio) {
		SPDK_ERRLOG("Cannot get aio\n");
		clb(ctx, 0, EFAULT);
		return NULL;
	}

	if (read) {
		io_prep_preadv(&aio->io, fd, iovs, iovcnt, offs);
	} else {
		io_prep_pwritev(&aio->io, fd, iovs, iovcnt, offs);
	}
	io_set_callback(&aio->io, spdk_aio_mgr_io_cpl_cb);


	ios[0] = &aio->io;
	res = io_submit(mgr->io_ctx, 1, ios);
	SPDK_DEBUGLOG(spdk_aio_mgr_io, "%s: aio=%p submitted with res=%d\n", read ? "read" : "write", aio,
		      res);
	if (res) {
		TAILQ_INSERT_TAIL(&aio->mgr->in_flight, aio, link);
		return aio;
	} else {
		aio->clb(aio->ctx, 0, aio->err);
		aio_mgr_put_aio(mgr, aio);
		return NULL;
	}

}

struct spdk_aio_mgr *
spdk_aio_mgr_create(uint32_t max_aios)
{
	struct spdk_aio_mgr *mgr;
	int res;
	uint32_t i;

	mgr = calloc(1, sizeof(*mgr));
	if (!mgr) {
		SPDK_ERRLOG("cannot alloc mgr of %zu bytes\n", sizeof(*mgr));
		return NULL;
	}

	res = io_queue_init(max_aios, &mgr->io_ctx);
	if (res) {
		SPDK_ERRLOG("io_setup(%" PRIu32 ") failed with %d\n", max_aios, res);
		free(mgr);
		return NULL;
	}

	mgr->aios.arr = calloc(max_aios, sizeof(mgr->aios.arr[0]));
	if (!mgr->aios.arr) {
		SPDK_ERRLOG("cannot alloc aios pool of %" PRIu32 "\n", max_aios);
		io_queue_release(mgr->io_ctx);
		free(mgr);
		return NULL;
	}

	TAILQ_INIT(&mgr->in_flight);
	TAILQ_INIT(&mgr->aios.pool);

	for (i = 0; i < max_aios; i++) {
		TAILQ_INSERT_TAIL(&mgr->aios.pool, &mgr->aios.arr[i], link);
	}

	return mgr;
}

struct spdk_aio_mgr_io *
spdk_aio_mgr_read(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx,
		  int fd, uint64_t offs, uint32_t size, struct iovec *iovs, uint32_t iovcnt)
{
	return spdk_aio_mgr_submit_io(mgr, clb, ctx, fd, offs, size, iovs, iovcnt, true);
}

struct spdk_aio_mgr_io *
spdk_aio_mgr_write(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx,
		   int fd, uint64_t offs, uint32_t size, const struct iovec *iovs, uint32_t iovcnt)
{
	return spdk_aio_mgr_submit_io(mgr, clb, ctx, fd, offs, size, (struct iovec *)iovs, iovcnt, false);
}

void
spdk_aio_mgr_cancel(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_io *aio)
{
	int res;
	struct io_event result;

	assert(mgr == aio->mgr);

	res = io_cancel(mgr->io_ctx, &aio->io, &result);
	if (res) {
		SPDK_DEBUGLOG(spdk_aio_mgr_io, "aio=%p cancelled\n", aio);
		spdk_aio_mgr_io_cpl_cb(mgr->io_ctx, &aio->io, ECANCELED, 0);
	} else {
		SPDK_WARNLOG("aio=%p cancellation failed with err=%d\n", aio, res);
	}
}

void
spdk_aio_mgr_poll(struct spdk_aio_mgr *mgr)
{
	int res;

	res = io_queue_run(mgr->io_ctx);
	if (res) {
		SPDK_WARNLOG("polling failed with err=%d\n", res);
	}
}

void
spdk_aio_mgr_delete(struct spdk_aio_mgr *mgr)
{
	assert(TAILQ_EMPTY(&mgr->in_flight));
	free(mgr->aios.arr);
	io_queue_release(mgr->io_ctx);
	free(mgr);
}

SPDK_LOG_REGISTER_COMPONENT(spdk_aio_mgr_io)
