/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "libxnvme.h"
#include "libxnvme_pp.h"

#include "bdev_xnvme.h"

#include "spdk/stdinc.h"

#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"

struct bdev_xnvme_io_channel {
	struct xnvme_queue	*queue;
	struct spdk_poller	*poller;
};

struct bdev_xnvme_task {
	struct bdev_xnvme_io_channel *ch;
	TAILQ_ENTRY(bdev_xnvme_task) link;
};

struct bdev_xnvme {
	struct spdk_bdev	bdev;
	char			*filename;
	char			*io_mechanism;
	struct xnvme_dev	*dev;
	uint32_t		nsid;
	bool			conserve_cpu;

	TAILQ_ENTRY(bdev_xnvme) link;
};

static int bdev_xnvme_init(void);
static void bdev_xnvme_fini(void);
static void bdev_xnvme_free(struct bdev_xnvme *xnvme);
static TAILQ_HEAD(, bdev_xnvme) g_xnvme_bdev_head = TAILQ_HEAD_INITIALIZER(g_xnvme_bdev_head);

static int
bdev_xnvme_get_ctx_size(void)
{
	return sizeof(struct bdev_xnvme_task);
}

static int
bdev_xnvme_config_json(struct spdk_json_write_ctx *w)
{
	struct bdev_xnvme *xnvme;

	TAILQ_FOREACH(xnvme, &g_xnvme_bdev_head, link) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_xnvme_create");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", xnvme->bdev.name);
		spdk_json_write_named_string(w, "filename", xnvme->filename);
		spdk_json_write_named_string(w, "io_mechanism", xnvme->io_mechanism);
		spdk_json_write_named_bool(w, "conserve_cpu", xnvme->conserve_cpu);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	return 0;
}

static struct spdk_bdev_module xnvme_if = {
	.name		= "xnvme",
	.module_init	= bdev_xnvme_init,
	.module_fini	= bdev_xnvme_fini,
	.get_ctx_size	= bdev_xnvme_get_ctx_size,
	.config_json	= bdev_xnvme_config_json,
};

SPDK_BDEV_MODULE_REGISTER(xnvme, &xnvme_if)

static struct spdk_io_channel *
bdev_xnvme_get_io_channel(void *ctx)
{
	struct bdev_xnvme *xnvme = ctx;

	return spdk_get_io_channel(xnvme);
}

static bool
bdev_xnvme_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	default:
		return false;
	}
}

static void
bdev_xnvme_destruct_cb(void *io_device)
{
	struct bdev_xnvme *xnvme = io_device;

	TAILQ_REMOVE(&g_xnvme_bdev_head, xnvme, link);
	bdev_xnvme_free(xnvme);
}

static int
bdev_xnvme_destruct(void *ctx)
{
	struct bdev_xnvme *xnvme = ctx;

	spdk_io_device_unregister(xnvme, bdev_xnvme_destruct_cb);

	return 0;
}

static void
bdev_xnvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct bdev_xnvme_task *xnvme_task = (struct bdev_xnvme_task *)bdev_io->driver_ctx;
	struct bdev_xnvme *xnvme = (struct bdev_xnvme *)bdev_io->bdev->ctxt;
	struct bdev_xnvme_io_channel *xnvme_ch = spdk_io_channel_get_ctx(ch);
	struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(xnvme_ch->queue);
	int err;

	if (!success) {
		xnvme_queue_put_cmd_ctx(xnvme_ch->queue, ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(xnvme, "bdev_io : %p, iov_cnt : %d, bdev_xnvme_task : %p\n",
		      bdev_io, bdev_io->u.bdev.iovcnt, (struct bdev_xnvme_task *)bdev_io->driver_ctx);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		ctx->cmd.common.opcode = XNVME_SPEC_NVM_OPC_READ;
		ctx->cmd.common.nsid = xnvme->nsid;
		ctx->cmd.nvm.nlb = bdev_io->u.bdev.num_blocks - 1;
		ctx->cmd.nvm.slba = bdev_io->u.bdev.offset_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ctx->cmd.common.opcode = XNVME_SPEC_NVM_OPC_WRITE;
		ctx->cmd.common.nsid = xnvme->nsid;
		ctx->cmd.nvm.nlb = bdev_io->u.bdev.num_blocks - 1;
		ctx->cmd.nvm.slba = bdev_io->u.bdev.offset_blocks;
		break;

	default:
		SPDK_ERRLOG("Wrong io type\n");

		xnvme_queue_put_cmd_ctx(xnvme_ch->queue, ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	xnvme_task->ch = xnvme_ch;
	ctx->async.cb_arg = xnvme_task;

	err = xnvme_cmd_passv(ctx, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.num_blocks * xnvme->bdev.blocklen, NULL, 0, 0);

	switch (err) {
	/* Submission success! */
	case 0:
		SPDK_DEBUGLOG(xnvme, "io_channel : %p, iovcnt:%d, nblks: %lu off: %#lx\n",
			      xnvme_ch, bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.offset_blocks);
		return;

	/* Submission failed: queue is full or no memory  => Queue the I/O in bdev layer */
	case -EBUSY:
	case -EAGAIN:
	case -ENOMEM:
		SPDK_WARNLOG("Start to queue I/O for xnvme bdev\n");

		xnvme_queue_put_cmd_ctx(xnvme_ch->queue, ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;

	/* Submission failed: unexpected error, put the command-context back in the queue */
	default:
		SPDK_ERRLOG("bdev_xnvme_cmd_passv : Submission failed: unexpected error\n");

		xnvme_queue_put_cmd_ctx(xnvme_ch->queue, ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static void
bdev_xnvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_xnvme_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static const struct spdk_bdev_fn_table xnvme_fn_table = {
	.destruct		= bdev_xnvme_destruct,
	.submit_request		= bdev_xnvme_submit_request,
	.io_type_supported	= bdev_xnvme_io_type_supported,
	.get_io_channel		= bdev_xnvme_get_io_channel,
};

static void
bdev_xnvme_free(struct bdev_xnvme *xnvme)
{
	assert(xnvme != NULL);

	xnvme_dev_close(xnvme->dev);
	free(xnvme->io_mechanism);
	free(xnvme->filename);
	free(xnvme->bdev.name);
	free(xnvme);
}

static void
bdev_xnvme_cmd_cb(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct bdev_xnvme_task *xnvme_task = ctx->async.cb_arg;
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_DEBUGLOG(xnvme, "xnvme_task : %p\n", xnvme_task);

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		SPDK_ERRLOG("xNVMe I/O Failed\n");
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(xnvme_task), status);

	/* Completed: Put the command- context back in the queue */
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

static int
bdev_xnvme_poll(void *arg)
{
	struct bdev_xnvme_io_channel *ch = arg;
	int rc;

	rc = xnvme_queue_poke(ch->queue, 0);
	if (rc < 0) {
		SPDK_ERRLOG("xnvme_queue_poke failure rc : %d\n", rc);
		return SPDK_POLLER_BUSY;
	}

	return xnvme_queue_get_outstanding(ch->queue) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
bdev_xnvme_queue_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_xnvme *xnvme = io_device;
	struct bdev_xnvme_io_channel *ch = ctx_buf;
	int rc;
	int qd = 512;

	rc = xnvme_queue_init(xnvme->dev, qd, 0, &ch->queue);
	if (rc) {
		SPDK_ERRLOG("xnvme_queue_init failure: %d\n", rc);
		return 1;
	}

	xnvme_queue_set_cb(ch->queue, bdev_xnvme_cmd_cb, ch);

	ch->poller = SPDK_POLLER_REGISTER(bdev_xnvme_poll, ch, 0);

	return 0;
}

static void
bdev_xnvme_queue_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_xnvme_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);

	xnvme_queue_term(ch->queue);
}

struct spdk_bdev *
create_xnvme_bdev(const char *name, const char *filename, const char *io_mechanism,
		  bool conserve_cpu)
{
	struct bdev_xnvme *xnvme;
	uint32_t block_size;
	uint64_t bdev_size;
	int rc;
	struct xnvme_opts opts = xnvme_opts_default();

	xnvme = calloc(1, sizeof(*xnvme));
	if (!xnvme) {
		SPDK_ERRLOG("Unable to allocate enough memory for xNVMe backend\n");
		return NULL;
	}

	opts.direct = 1;
	opts.async = io_mechanism;
	if (!opts.async) {
		goto error_return;
	}
	xnvme->io_mechanism = strdup(io_mechanism);
	if (!xnvme->io_mechanism) {
		goto error_return;
	}

	if (!conserve_cpu) {
		if (!strcmp(xnvme->io_mechanism, "libaio")) {
			opts.poll_io = 1;
		} else if (!strcmp(xnvme->io_mechanism, "io_uring")) {
			opts.poll_io = 1;
		} else if (!strcmp(xnvme->io_mechanism, "io_uring_cmd")) {
			opts.poll_sq = 1;
		}
	}

	xnvme->filename = strdup(filename);
	if (!xnvme->filename) {
		goto error_return;
	}

	xnvme->dev = xnvme_dev_open(xnvme->filename, &opts);
	if (!xnvme->dev) {
		SPDK_ERRLOG("Unable to open xNVMe device %s\n", filename);
		goto error_return;
	}

	xnvme->nsid = xnvme_dev_get_nsid(xnvme->dev);

	bdev_size = xnvme_dev_get_geo(xnvme->dev)->tbytes;
	block_size = xnvme_dev_get_geo(xnvme->dev)->nbytes;

	xnvme->bdev.name = strdup(name);
	if (!xnvme->bdev.name) {
		goto error_return;
	}

	xnvme->bdev.product_name = "xNVMe bdev";
	xnvme->bdev.module = &xnvme_if;

	xnvme->bdev.write_cache = 0;

	if (block_size == 0) {
		SPDK_ERRLOG("Block size could not be auto-detected\n");
		goto error_return;
	}

	if (block_size < 512) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		goto error_return;
	}

	SPDK_DEBUGLOG(xnvme, "bdev_name : %s, bdev_size : %lu, block_size : %d\n",
		      xnvme->bdev.name, bdev_size, block_size);

	xnvme->bdev.blocklen = block_size;
	xnvme->bdev.required_alignment = spdk_u32log2(block_size);

	if (bdev_size % xnvme->bdev.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    bdev_size, xnvme->bdev.blocklen);
		goto error_return;
	}

	xnvme->bdev.blockcnt = bdev_size / xnvme->bdev.blocklen;
	xnvme->bdev.ctxt = xnvme;

	xnvme->bdev.fn_table = &xnvme_fn_table;

	spdk_io_device_register(xnvme, bdev_xnvme_queue_create_cb, bdev_xnvme_queue_destroy_cb,
				sizeof(struct bdev_xnvme_io_channel),
				xnvme->bdev.name);
	rc = spdk_bdev_register(&xnvme->bdev);
	if (rc) {
		spdk_io_device_unregister(xnvme, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_xnvme_bdev_head, xnvme, link);

	return &xnvme->bdev;

error_return:
	bdev_xnvme_free(xnvme);
	return NULL;
}

struct delete_xnvme_bdev_ctx {
	struct bdev_xnvme *xnvme;
	spdk_delete_xnvme_complete cb_fn;
	void *cb_arg;
};

static void
xnvme_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_xnvme_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
delete_xnvme_bdev(struct spdk_bdev *bdev, spdk_delete_xnvme_complete cb_fn, void *cb_arg)
{
	struct delete_xnvme_bdev_ctx *ctx;
	struct bdev_xnvme *xnvme = (struct bdev_xnvme *)bdev->ctxt;

	if (!bdev || bdev->module != &xnvme_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->xnvme = xnvme;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	spdk_bdev_unregister(bdev, xnvme_bdev_unregister_cb, ctx);
}

static int
bdev_xnvme_module_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_xnvme_module_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
bdev_xnvme_init(void)
{
	spdk_io_device_register(&xnvme_if, bdev_xnvme_module_create_cb, bdev_xnvme_module_destroy_cb,
				0, "xnvme_module");

	return 0;
}

static void
bdev_xnvme_fini(void)
{
	spdk_io_device_unregister(&xnvme_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(xnvme)
