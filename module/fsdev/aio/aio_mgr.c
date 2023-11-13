/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "aio_mgr.h"

#define REQS_PER_AIO 10

struct spdk_aio_mgr_req {
	struct aiocb io;
	TAILQ_ENTRY(spdk_aio_mgr_req) link;
};

struct spdk_aio_mgr_io {
	TAILQ_ENTRY(spdk_aio_mgr_io) link;
	TAILQ_HEAD(, spdk_aio_mgr_req) reqs;
	struct spdk_aio_mgr *mgr;
	fsdev_aio_done_cb clb;
	void *ctx;
	uint32_t data_size;
	int err;
};

struct spdk_aio_mgr {
	TAILQ_HEAD(, spdk_aio_mgr_io) in_flight;
	struct {
		struct spdk_aio_mgr_req *arr;
		uint32_t size;
		TAILQ_HEAD(, spdk_aio_mgr_req) pool;
	} reqs;
	struct {
		struct spdk_aio_mgr_io *arr;
		uint32_t size;
		TAILQ_HEAD(, spdk_aio_mgr_io) pool;
	} aios;
};

static inline struct spdk_aio_mgr_req *
aio_mgr_get_aio_req(struct spdk_aio_mgr *mgr)
{
	struct spdk_aio_mgr_req *req = TAILQ_FIRST(&mgr->reqs.pool);

	if (req) {
		TAILQ_REMOVE(&mgr->reqs.pool, req, link);
	}

	return req;
}

static inline void
aio_mgr_put_aio_req(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_req *req)
{
	TAILQ_INSERT_TAIL(&mgr->reqs.pool, req, link);
}

static uint32_t
fsdev_aio_submit(struct spdk_aio_mgr_io *aio, int fd, uint64_t offs, uint32_t size,
		 struct iovec *iovs,
		 uint32_t iovcnt, int (*aio_submit_func)(struct aiocb *), const char *aio_type)
{
	uint32_t bytes_handled = 0;
	uint32_t iov_idx = 0;

	assert(aio->mgr);
	assert(!aio->err);
	assert(iovs != NULL);
	assert(iovcnt != 0);

	while (size && iov_idx < iovcnt) {
		struct spdk_aio_mgr_req *req;
		struct iovec *iov = &iovs[iov_idx];
		size_t ho_handle = spdk_min(iov->iov_len, size);

		req = aio_mgr_get_aio_req(aio->mgr);
		if (!req) {
			SPDK_ERRLOG("cannot get aio req\n");
			aio->err = EINVAL;
			break;
		}

		memset(&req->io, 0, sizeof(req->io));

		req->io.aio_nbytes = ho_handle;
		req->io.aio_buf = iov->iov_base;
		req->io.aio_offset = offs + bytes_handled;
		req->io.aio_fildes = fd;

		SPDK_DEBUGLOG(spdk_aio_mgr_io,
			      "aio to %s: aio=%p req=%p aio_nbytes=%zu aio_buf=%p aio_offset=%" PRIu64 " aio_fildes=%d\n",
			      aio_type, aio, req, req->io.aio_nbytes, req->io.aio_buf, (uint64_t)req->io.aio_offset,
			      req->io.aio_fildes);

		if (aio_submit_func(&req->io)) {
			aio->err = errno;
			SPDK_ERRLOG("aio_%s of io[%" PRIu32 "] at offset %" PRIu64 " failed with err=%d\n",
				    aio_type, iov_idx, offs, aio->err);
			aio_mgr_put_aio_req(aio->mgr, req);
			break;
		}

		TAILQ_INSERT_TAIL(&aio->reqs, req, link);

		bytes_handled += ho_handle;
		size -= ho_handle;

		iov_idx++;
	}

	return bytes_handled;
}

static void
fsdev_aio_cancel(struct spdk_aio_mgr_io *aio)
{
	struct spdk_aio_mgr_req *req;
	TAILQ_FOREACH(req, &aio->reqs, link) {
		aio_cancel(req->io.aio_fildes, &req->io);
	}
}

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
		TAILQ_INIT(&aio->reqs);
		TAILQ_REMOVE(&mgr->aios.pool, aio, link);
	}

	return aio;
}

static inline void
aio_mgr_put_aio(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_io *aio)
{
	TAILQ_INSERT_TAIL(&aio->mgr->aios.pool, aio, link);
}

static struct spdk_aio_mgr_io *
spdk_aio_mgr_submit_io(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx,
		       int fd, uint64_t offs, uint32_t size, struct iovec *iovs, uint32_t iovcnt,
		       int (*aio_submit_func)(struct aiocb *), const char *aio_type)
{
	struct spdk_aio_mgr_io *aio;
	uint32_t bytes_handled;

	SPDK_DEBUGLOG(spdk_aio_mgr_io, "%s: fd=%d offs=%" PRIu64 " size=%" PRIu32 " iovcnt=%" PRIu32 "\n",
		      aio_type, fd, offs, size, iovcnt);

	aio = aio_mgr_get_aio(mgr, clb, ctx);
	if (!aio) {
		SPDK_ERRLOG("Cannot get aio\n");
		clb(ctx, 0, EFAULT);
		return NULL;
	}

	bytes_handled = fsdev_aio_submit(aio, fd, offs, size, iovs, iovcnt, aio_submit_func, aio_type);
	SPDK_DEBUGLOG(spdk_aio_mgr_io, "%s: aio=%p: handled %" PRIu32 " bytes\n", aio_type, aio,
		      bytes_handled);
	if (bytes_handled) {
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
	uint32_t i;

	mgr = calloc(1, sizeof(*mgr));
	if (!mgr) {
		SPDK_ERRLOG("cannot alloc mgr of %zu bytes\n", sizeof(*mgr));
		return NULL;
	}

	mgr->reqs.arr = calloc(max_aios * REQS_PER_AIO, sizeof(mgr->reqs.arr[0]));
	if (!mgr->reqs.arr) {
		SPDK_ERRLOG("cannot alloc req pool of %" PRIu32 " * %d\n", max_aios, REQS_PER_AIO);
		free(mgr);
		return NULL;
	}

	mgr->aios.arr = calloc(max_aios, sizeof(mgr->aios.arr[0]));
	if (!mgr->aios.arr) {
		SPDK_ERRLOG("cannot alloc aios pool of %" PRIu32 "\n", max_aios);
		free(mgr->reqs.arr);
		free(mgr);
		return NULL;
	}

	TAILQ_INIT(&mgr->in_flight);
	TAILQ_INIT(&mgr->reqs.pool);
	TAILQ_INIT(&mgr->aios.pool);

	for (i = 0; i < max_aios * REQS_PER_AIO; i++) {
		TAILQ_INSERT_TAIL(&mgr->reqs.pool, &mgr->reqs.arr[i], link);
	}

	for (i = 0; i < max_aios; i++) {
		TAILQ_INSERT_TAIL(&mgr->aios.pool, &mgr->aios.arr[i], link);
	}

	return mgr;
}

struct spdk_aio_mgr_io *
spdk_aio_mgr_read(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx,
		  int fd, uint64_t offs, uint32_t size, struct iovec *iovs, uint32_t iovcnt)
{
	return spdk_aio_mgr_submit_io(mgr, clb, ctx, fd, offs, size, iovs, iovcnt, aio_read, "read");
}

struct spdk_aio_mgr_io *
spdk_aio_mgr_write(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb, void *ctx,
		   int fd, uint64_t offs, uint32_t size, const struct iovec *iovs, uint32_t iovcnt)
{
	return spdk_aio_mgr_submit_io(mgr, clb, ctx, fd, offs, size, (struct iovec *)iovs, iovcnt,
				      aio_write, "write");
}


void
spdk_aio_mgr_cancel(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_io *aio)
{
	assert(mgr == aio->mgr);

	SPDK_DEBUGLOG(spdk_aio_mgr_io, "aio=%p cancelled\n", aio);
	fsdev_aio_cancel(aio);
}

void
spdk_aio_mgr_poll(struct spdk_aio_mgr *mgr)
{
	struct spdk_aio_mgr_io *aio, *tmp_aio;
	TAILQ_FOREACH_SAFE(aio, &mgr->in_flight, link, tmp_aio) {
		struct spdk_aio_mgr_req *req, *tmp_req;
		TAILQ_FOREACH_SAFE(req, &aio->reqs, link, tmp_req) {
			ssize_t ret;
			int err = aio_error(&req->io);
			if (err == EINPROGRESS) { /* the request has not been completed yet */
				break; /* stop checking completions for this aio */
			}

			if (!err) { /* the request completed successfull */
				;
			} else if (err == ECANCELED) { /* the request was canceled */
				SPDK_WARNLOG("aio processing was cancelled\n");
				aio->err = EAGAIN;
			} else {
				SPDK_ERRLOG("aio processing failed with err=%d\n", err);
				aio->err = err;
			}

			ret = aio_return(&req->io);
			if (ret > 0) {
				aio->data_size += ret;
			}

			SPDK_DEBUGLOG(spdk_aio_mgr_io, "aio completed: aio=%p req=%p err=%d ret=%zd\n", aio, req, err, ret);

			/* the request processing is done */
			TAILQ_REMOVE(&aio->reqs, req, link); /* remove the req from the aio */
			TAILQ_INSERT_TAIL(&mgr->reqs.pool, req, link); /* return the req to the pool */
			if (TAILQ_EMPTY(&aio->reqs)) { /* all the aio's requests have been processed */
				SPDK_DEBUGLOG(spdk_aio_mgr_io, "aio=%p is done (data_size=%" PRIu32 ")\n", aio, aio->data_size);
				aio->clb(aio->ctx, aio->data_size, aio->err); /* call the user's callback */
				TAILQ_REMOVE(&mgr->in_flight, aio, link); /* remove the aio from the in_flight aios list */
				TAILQ_INSERT_TAIL(&mgr->aios.pool, aio, link); /* return the aio to the pool */
			}
		}
	}
}

void
spdk_aio_mgr_delete(struct spdk_aio_mgr *mgr)
{
	assert(TAILQ_EMPTY(&mgr->in_flight));
	free(mgr->aios.arr);
	free(mgr->reqs.arr);
	free(mgr);
}

SPDK_LOG_REGISTER_COMPONENT(spdk_aio_mgr_io)
