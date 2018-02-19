/*-
 *   BSD LICENSE
 *
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

#include "bdev_rbd.h"

#include <rbd/librbd.h>
#include <rados/librados.h>
#include <sys/eventfd.h>

#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/string.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#define SPDK_RBD_QUEUE_DEPTH 128

static int bdev_rbd_count = 0;

struct bdev_rbd {
	struct spdk_bdev disk;
	char *rbd_name;
	char *pool_name;
	rbd_image_info_t info;
	TAILQ_ENTRY(bdev_rbd) tailq;
};

struct bdev_rbd_io_channel {
	rados_ioctx_t io_ctx;
	rados_t cluster;
	struct pollfd pfd;
	rbd_image_t image;
	struct bdev_rbd *disk;
	struct spdk_poller *poller;
};

static void
bdev_rbd_free(struct bdev_rbd *rbd)
{
	if (!rbd) {
		return;
	}

	free(rbd->disk.name);
	free(rbd->rbd_name);
	free(rbd->pool_name);
	free(rbd);
}

static int
bdev_rados_context_init(const char *rbd_pool_name, rados_t *cluster,
			rados_ioctx_t *io_ctx)
{
	int ret;

	ret = rados_create(cluster, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados_t struct\n");
		return -1;
	}

	ret = rados_conf_read_file(*cluster, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to read conf file\n");
		rados_shutdown(*cluster);
		return -1;
	}

	ret = rados_connect(*cluster);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to connect rbd_pool\n");
		rados_shutdown(*cluster);
	}

	ret = rados_ioctx_create(*cluster, rbd_pool_name, io_ctx);

	if (ret < 0) {
		SPDK_ERRLOG("Failed to create ioctx\n");
		rados_shutdown(*cluster);
		return -1;
	}

	return 0;
}

static int
bdev_rbd_init(const char *rbd_pool_name, const char *rbd_name, rbd_image_info_t *info)
{
	int ret;
	rados_t cluster = NULL;
	rados_ioctx_t io_ctx = NULL;
	rbd_image_t image = NULL;

	ret = bdev_rados_context_init(rbd_pool_name, &cluster, &io_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados context for rbd_pool=%s\n",
			    rbd_name);
		return -1;
	}

	ret = rbd_open(io_ctx, rbd_name, &image, NULL);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		goto err;
	}
	ret = rbd_stat(image, info, sizeof(*info));
	rbd_close(image);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to stat specified rbd device\n");
		goto err;
	}

	return 0;
err:
	rados_ioctx_destroy(io_ctx);
	rados_shutdown(cluster);
	return -1;
}

static void
bdev_rbd_exit(rbd_image_t image)
{
	rbd_flush(image);
	rbd_close(image);
}

static void
bdev_rbd_finish_aiocb(rbd_completion_t cb, void *arg)
{
	/* Doing nothing here */
}

static int
bdev_rbd_start_aio(rbd_image_t image, struct spdk_bdev_io *bdev_io,
		   void *buf, uint64_t offset, size_t len)
{
	int ret;
	rbd_completion_t comp;

	ret = rbd_aio_create_completion(bdev_io, bdev_rbd_finish_aiocb,
					&comp);
	if (ret < 0) {
		return -1;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = rbd_aio_read(image, offset, len,
				   buf, comp);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = rbd_aio_write(image, offset, len,
				    buf, comp);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_FLUSH) {
		ret = rbd_aio_flush(image, comp);
	}

	if (ret < 0) {
		rbd_aio_release(comp);
		return -1;
	}

	return 0;
}

static int bdev_rbd_library_init(void);

SPDK_BDEV_MODULE_REGISTER(rbd, bdev_rbd_library_init, NULL, NULL,
			  NULL, NULL)

static int64_t
bdev_rbd_rw(struct bdev_rbd *disk, struct spdk_io_channel *ch,
	    struct spdk_bdev_io *bdev_io, struct iovec *iov,
	    int iovcnt, size_t len, uint64_t offset)
{
	struct bdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);

	if (iovcnt != 1 || iov->iov_len != len) {
		return -1;
	}

	return bdev_rbd_start_aio(rbdio_ch->image, bdev_io, iov->iov_base, offset, len);
}

static int64_t
bdev_rbd_flush(struct bdev_rbd *disk, struct spdk_io_channel *ch,
	       struct spdk_bdev_io *bdev_io, uint64_t offset, uint64_t nbytes)
{
	struct bdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);

	return bdev_rbd_start_aio(rbdio_ch->image, bdev_io, NULL, offset, nbytes);
}

static int
bdev_rbd_destruct(void *ctx)
{
	struct bdev_rbd *rbd = ctx;

	bdev_rbd_free(rbd);
	return 0;
}

static void bdev_rbd_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = bdev_rbd_rw(bdev_io->bdev->ctxt,
			  ch,
			  bdev_io,
			  bdev_io->u.bdev.iovs,
			  bdev_io->u.bdev.iovcnt,
			  bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			  bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);

	if (ret != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int _bdev_rbd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_rbd_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_rbd_rw((struct bdev_rbd *)bdev_io->bdev->ctxt,
				   ch,
				   bdev_io,
				   bdev_io->u.bdev.iovs,
				   bdev_io->u.bdev.iovcnt,
				   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				   bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_rbd_flush((struct bdev_rbd *)bdev_io->bdev->ctxt,
				      ch,
				      bdev_io,
				      bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
				      bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
	default:
		return -1;
	}
	return 0;
}

static void bdev_rbd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_rbd_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_rbd_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;

	default:
		return false;
	}
}

static void
bdev_rbd_io_poll(void *arg)
{
	struct bdev_rbd_io_channel *ch = arg;
	int i, io_status, rc;
	rbd_completion_t comps[SPDK_RBD_QUEUE_DEPTH];
	struct spdk_bdev_io *bdev_io;
	enum spdk_bdev_io_status status;

	rc = poll(&ch->pfd, 1, 0);

	/* check the return value of poll since we have only one fd for each channel */
	if (rc != 1) {
		return;
	}

	rc = rbd_poll_io_events(ch->image, comps, SPDK_RBD_QUEUE_DEPTH);
	for (i = 0; i < rc; i++) {
		bdev_io = rbd_aio_get_arg(comps[i]);
		io_status = rbd_aio_get_return_value(comps[i]);
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
			if ((int)(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen) == io_status) {
				status = SPDK_BDEV_IO_STATUS_SUCCESS;
			} else {
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		} else {
			/* For others, 0 means success */
			if (!io_status) {
				status = SPDK_BDEV_IO_STATUS_SUCCESS;
			} else {
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		}
		rbd_aio_release(comps[i]);
		spdk_bdev_io_complete(bdev_io, status);
	}
}

static void
bdev_rbd_free_channel(struct bdev_rbd_io_channel *ch)
{
	if (!ch) {
		return;
	}

	if (ch->image) {
		bdev_rbd_exit(ch->image);
	}

	if (ch->io_ctx) {
		rados_ioctx_destroy(ch->io_ctx);
	}

	if (ch->cluster) {
		rados_shutdown(ch->cluster);
	}

	if (ch->pfd.fd >= 0) {
		close(ch->pfd.fd);
	}
}

static void *
bdev_rbd_handle(void *arg)
{
	struct bdev_rbd_io_channel *ch = arg;
	void *ret = arg;

	if (rbd_open(ch->io_ctx, ch->disk->rbd_name, &ch->image, NULL) < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		ret = NULL;
	}

	return ret;
}

static int
bdev_rbd_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *ch = ctx_buf;
	int ret;

	ch->disk = (struct bdev_rbd *)((uintptr_t)io_device - offsetof(struct bdev_rbd, info));
	ch->image = NULL;
	ch->io_ctx = NULL;
	ch->pfd.fd = -1;

	ret = bdev_rados_context_init(ch->disk->pool_name, &ch->cluster, &ch->io_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados context for rbd_pool=%s\n",
			    ch->disk->pool_name);
		goto err;
	}

	if (spdk_call_unaffinitized(bdev_rbd_handle, ch) == NULL) {
		goto err;
	}

	ch->pfd.fd = eventfd(0, EFD_NONBLOCK);
	if (ch->pfd.fd < 0) {
		SPDK_ERRLOG("Failed to get eventfd\n");
		goto err;
	}

	ch->pfd.events = POLLIN;
	ret = rbd_set_image_notification(ch->image, ch->pfd.fd, EVENT_TYPE_EVENTFD);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to set rbd image notification\n");
		goto err;
	}

	ch->poller = spdk_poller_register(bdev_rbd_io_poll, ch, 0);

	return 0;

err:
	bdev_rbd_free_channel(ch);
	return -1;
}

static void
bdev_rbd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *io_channel = ctx_buf;

	bdev_rbd_free_channel(io_channel);

	spdk_poller_unregister(&io_channel->poller);
}

static struct spdk_io_channel *
bdev_rbd_get_io_channel(void *ctx)
{
	struct bdev_rbd *rbd_bdev = ctx;

	return spdk_get_io_channel(rbd_bdev);
}

static int
bdev_rbd_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_rbd *rbd_bdev = ctx;

	spdk_json_write_name(w, "rbd");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "pool_name");
	spdk_json_write_string(w, rbd_bdev->pool_name);

	spdk_json_write_name(w, "rbd_name");
	spdk_json_write_string(w, rbd_bdev->rbd_name);

	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table rbd_fn_table = {
	.destruct		= bdev_rbd_destruct,
	.submit_request		= bdev_rbd_submit_request,
	.io_type_supported	= bdev_rbd_io_type_supported,
	.get_io_channel		= bdev_rbd_get_io_channel,
	.dump_config_json	= bdev_rbd_dump_config_json,
};

struct spdk_bdev *
spdk_bdev_rbd_create(const char *pool_name, const char *rbd_name, uint32_t block_size)
{
	struct bdev_rbd *rbd;
	int ret;

	if ((pool_name == NULL) || (rbd_name == NULL)) {
		return NULL;
	}

	rbd = calloc(1, sizeof(struct bdev_rbd));
	if (rbd == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_rbd struct\n");
		return NULL;
	}

	rbd->rbd_name = strdup(rbd_name);
	if (!rbd->rbd_name) {
		bdev_rbd_free(rbd);
		return NULL;
	}

	rbd->pool_name = strdup(pool_name);
	if (!rbd->pool_name) {
		bdev_rbd_free(rbd);
		return NULL;
	}

	ret = bdev_rbd_init(rbd->pool_name, rbd_name, &rbd->info);
	if (ret < 0) {
		bdev_rbd_free(rbd);
		SPDK_ERRLOG("Failed to init rbd device\n");
		return NULL;
	}

	rbd->disk.name = spdk_sprintf_alloc("Ceph%d", bdev_rbd_count);
	if (!rbd->disk.name) {
		bdev_rbd_free(rbd);
		return NULL;
	}
	rbd->disk.product_name = "Ceph Rbd Disk";
	bdev_rbd_count++;

	rbd->disk.write_cache = 0;
	rbd->disk.blocklen = block_size;
	rbd->disk.blockcnt = rbd->info.size / rbd->disk.blocklen;
	rbd->disk.ctxt = rbd;
	rbd->disk.fn_table = &rbd_fn_table;
	rbd->disk.module = SPDK_GET_BDEV_MODULE(rbd);

	SPDK_NOTICELOG("Add %s rbd disk to lun\n", rbd->disk.name);

	spdk_io_device_register(rbd, bdev_rbd_create_cb,
				bdev_rbd_destroy_cb,
				sizeof(struct bdev_rbd_io_channel));
	ret = spdk_bdev_register(&rbd->disk);
	if (ret) {
		spdk_io_device_unregister(rbd, NULL);
		bdev_rbd_free(rbd);
		return NULL;
	}

	return &rbd->disk;
}

static int
bdev_rbd_library_init(void)
{
	int i, rc = 0;
	const char *val;
	const char *pool_name;
	const char *rbd_name;
	uint32_t block_size;

	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Ceph");

	if (sp == NULL) {
		/*
		 * Ceph section not found.  Do not initialize any rbd LUNS.
		 */
		goto end;
	}

	/* Init rbd block devices */
	for (i = 0; ; i++) {
		val = spdk_conf_section_get_nval(sp, "Ceph", i);
		if (val == NULL) {
			break;
		}

		/* get the Rbd_pool name */
		pool_name = spdk_conf_section_get_nmval(sp, "Ceph", i, 0);
		if (pool_name == NULL) {
			SPDK_ERRLOG("Ceph%d: rbd pool name needs to be provided\n", i);
			rc = -1;
			goto end;
		}

		rbd_name = spdk_conf_section_get_nmval(sp, "Ceph", i, 1);
		if (rbd_name == NULL) {
			SPDK_ERRLOG("Ceph%d: format error\n", i);
			rc = -1;
			goto end;
		}

		val = spdk_conf_section_get_nmval(sp, "Ceph", i, 2);

		if (val == NULL) {
			block_size = 512; /* default value */
		} else {
			block_size = (int)strtol(val, NULL, 10);
			if (block_size & 0x1ff) {
				SPDK_ERRLOG("current block_size = %d, it should be multiple of 512\n",
					    block_size);
				rc = -1;
				goto end;
			}
		}

		if (spdk_bdev_rbd_create(pool_name, rbd_name, block_size) == NULL) {
			rc = -1;
			goto end;
		}
	}

end:
	return rc;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_rbd", SPDK_LOG_BDEV_RBD)
