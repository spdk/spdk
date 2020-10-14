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
#include <sys/epoll.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#define SPDK_RBD_QUEUE_DEPTH 128
#define MAX_EVENTS_PER_POLL 128

static int bdev_rbd_count = 0;

struct bdev_rbd {
	struct spdk_bdev disk;
	char *rbd_name;
	char *user_id;
	char *pool_name;
	char **config;
	rbd_image_info_t info;
	TAILQ_ENTRY(bdev_rbd) tailq;
	struct spdk_poller *reset_timer;
	struct spdk_bdev_io *reset_bdev_io;
};

struct bdev_rbd_group_channel {
	struct spdk_poller *poller;
	int epoll_fd;
};

struct bdev_rbd_io_channel {
	rados_ioctx_t io_ctx;
	rados_t cluster;
	int pfd;
	rbd_image_t image;
	struct bdev_rbd *disk;
	struct bdev_rbd_group_channel *group_ch;
};

struct bdev_rbd_io {
	size_t	total_len;
};

static void
bdev_rbd_free(struct bdev_rbd *rbd)
{
	if (!rbd) {
		return;
	}

	free(rbd->disk.name);
	free(rbd->rbd_name);
	free(rbd->user_id);
	free(rbd->pool_name);
	bdev_rbd_free_config(rbd->config);
	free(rbd);
}

void
bdev_rbd_free_config(char **config)
{
	char **entry;

	if (config) {
		for (entry = config; *entry; entry++) {
			free(*entry);
		}
		free(config);
	}
}

char **
bdev_rbd_dup_config(const char *const *config)
{
	size_t count;
	char **copy;

	if (!config) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {}
	copy = calloc(count + 1, sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {
		if (!(copy[count] = strdup(config[count]))) {
			bdev_rbd_free_config(copy);
			return NULL;
		}
	}
	return copy;
}

static int
bdev_rados_context_init(const char *user_id, const char *rbd_pool_name, const char *const *config,
			rados_t *cluster, rados_ioctx_t *io_ctx)
{
	int ret;

	ret = rados_create(cluster, user_id);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados_t struct\n");
		return -1;
	}

	if (config) {
		const char *const *entry = config;
		while (*entry) {
			ret = rados_conf_set(*cluster, entry[0], entry[1]);
			if (ret < 0) {
				SPDK_ERRLOG("Failed to set %s = %s\n", entry[0], entry[1]);
				rados_shutdown(*cluster);
				return -1;
			}
			entry += 2;
		}
	} else {
		ret = rados_conf_read_file(*cluster, NULL);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to read conf file\n");
			rados_shutdown(*cluster);
			return -1;
		}
	}

	ret = rados_connect(*cluster);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to connect to rbd_pool\n");
		rados_shutdown(*cluster);
		return -1;
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
bdev_rbd_init(const char *user_id, const char *rbd_pool_name, const char *const *config,
	      const char *rbd_name, rbd_image_info_t *info)
{
	int ret;
	rados_t cluster = NULL;
	rados_ioctx_t io_ctx = NULL;
	rbd_image_t image = NULL;

	ret = bdev_rados_context_init(user_id, rbd_pool_name, config, &cluster, &io_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados context for user_id=%s and rbd_pool=%s\n",
			    user_id ? user_id : "admin (the default)", rbd_pool_name);
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

	rados_ioctx_destroy(io_ctx);
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
bdev_rbd_start_aio(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		   struct iovec *iov, int iovcnt, uint64_t offset, size_t len)
{
	struct bdev_rbd_io_channel *rbdio_ch = spdk_io_channel_get_ctx(ch);
	int ret;
	rbd_completion_t comp;
	struct bdev_rbd_io *rbd_io;
	rbd_image_t image = rbdio_ch->image;

	ret = rbd_aio_create_completion(bdev_io, bdev_rbd_finish_aiocb,
					&comp);
	if (ret < 0) {
		return -1;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;
		rbd_io->total_len = len;
		if (spdk_likely(iovcnt == 1)) {
			ret = rbd_aio_read(image, offset, iov[0].iov_len, iov[0].iov_base, comp);
		} else {
			ret = rbd_aio_readv(image, iov, iovcnt, offset, comp);
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		if (spdk_likely(iovcnt == 1)) {
			ret = rbd_aio_write(image, offset, iov[0].iov_len, iov[0].iov_base, comp);
		} else {
			ret = rbd_aio_writev(image, iov, iovcnt, offset, comp);
		}
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

static void bdev_rbd_library_fini(void);

static int
bdev_rbd_get_ctx_size(void)
{
	return sizeof(struct bdev_rbd_io);
}

static struct spdk_bdev_module rbd_if = {
	.name = "rbd",
	.module_init = bdev_rbd_library_init,
	.module_fini = bdev_rbd_library_fini,
	.get_ctx_size = bdev_rbd_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(rbd, &rbd_if)

static int
bdev_rbd_reset_timer(void *arg)
{
	struct bdev_rbd *disk = arg;

	/*
	 * TODO: This should check if any I/O is still in flight before completing the reset.
	 * For now, just complete after the timer expires.
	 */
	spdk_bdev_io_complete(disk->reset_bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_poller_unregister(&disk->reset_timer);
	disk->reset_bdev_io = NULL;

	return SPDK_POLLER_BUSY;
}

static int
bdev_rbd_reset(struct bdev_rbd *disk, struct spdk_bdev_io *bdev_io)
{
	/*
	 * HACK: Since librbd doesn't provide any way to cancel outstanding aio, just kick off a
	 * timer to wait for in-flight I/O to complete.
	 */
	assert(disk->reset_bdev_io == NULL);
	disk->reset_bdev_io = bdev_io;
	disk->reset_timer = SPDK_POLLER_REGISTER(bdev_rbd_reset_timer, disk, 1 * 1000 * 1000);

	return 0;
}

static int
bdev_rbd_destruct(void *ctx)
{
	struct bdev_rbd *rbd = ctx;

	spdk_io_device_unregister(rbd, NULL);

	bdev_rbd_free(rbd);
	return 0;
}

static void
bdev_rbd_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	int ret;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	ret = bdev_rbd_start_aio(ch,
				 bdev_io,
				 bdev_io->u.bdev.iovs,
				 bdev_io->u.bdev.iovcnt,
				 bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
				 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);

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
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_rbd_start_aio(ch,
					  bdev_io,
					  bdev_io->u.bdev.iovs,
					  bdev_io->u.bdev.iovcnt,
					  bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
					  bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_rbd_reset((struct bdev_rbd *)bdev_io->bdev->ctxt,
				      bdev_io);

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
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	default:
		return false;
	}
}

static void
bdev_rbd_io_poll(struct bdev_rbd_io_channel *ch)
{
	int i, io_status, rc;
	rbd_completion_t comps[SPDK_RBD_QUEUE_DEPTH];
	struct spdk_bdev_io *bdev_io;
	struct bdev_rbd_io *rbd_io;
	enum spdk_bdev_io_status bio_status;

	rc = rbd_poll_io_events(ch->image, comps, SPDK_RBD_QUEUE_DEPTH);
	for (i = 0; i < rc; i++) {
		bdev_io = rbd_aio_get_arg(comps[i]);
		rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;
		io_status = rbd_aio_get_return_value(comps[i]);
		bio_status = SPDK_BDEV_IO_STATUS_SUCCESS;

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
			if ((int)rbd_io->total_len != io_status) {
				bio_status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		} else {
			/* For others, 0 means success */
			if (io_status != 0) {
				bio_status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		}

		rbd_aio_release(comps[i]);

		spdk_bdev_io_complete(bdev_io, bio_status);
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

	if (ch->pfd >= 0) {
		close(ch->pfd);
	}

	if (ch->group_ch) {
		spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
	}
}

static void *
bdev_rbd_handle(void *arg)
{
	struct bdev_rbd_io_channel *ch = arg;
	void *ret = arg;
	int rc;

	rc = bdev_rados_context_init(ch->disk->user_id, ch->disk->pool_name,
				     (const char *const *)ch->disk->config,
				     &ch->cluster, &ch->io_ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create rados context for user_id %s and rbd_pool=%s\n",
			    ch->disk->user_id ? ch->disk->user_id : "admin (the default)", ch->disk->pool_name);
		ret = NULL;
		goto end;
	}

	if (rbd_open(ch->io_ctx, ch->disk->rbd_name, &ch->image, NULL) < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		ret = NULL;
	}

end:
	return ret;
}

static int
bdev_rbd_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *ch = ctx_buf;
	int ret;
	struct epoll_event event;

	ch->disk = io_device;
	ch->image = NULL;
	ch->io_ctx = NULL;
	ch->pfd = -1;

	if (spdk_call_unaffinitized(bdev_rbd_handle, ch) == NULL) {
		goto err;
	}

	ch->pfd = eventfd(0, EFD_NONBLOCK);
	if (ch->pfd < 0) {
		SPDK_ERRLOG("Failed to get eventfd\n");
		goto err;
	}

	ret = rbd_set_image_notification(ch->image, ch->pfd, EVENT_TYPE_EVENTFD);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to set rbd image notification\n");
		goto err;
	}

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&rbd_if));
	assert(ch->group_ch != NULL);
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.ptr = ch;

	ret = epoll_ctl(ch->group_ch->epoll_fd, EPOLL_CTL_ADD, ch->pfd, &event);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to add the fd of ch(%p) to the epoll group from group_ch=%p\n", ch,
			    ch->group_ch);
		goto err;
	}

	return 0;

err:
	bdev_rbd_free_channel(ch);
	return -1;
}

static void
bdev_rbd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *io_channel = ctx_buf;
	int rc;

	rc = epoll_ctl(io_channel->group_ch->epoll_fd, EPOLL_CTL_DEL,
		       io_channel->pfd, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove fd on io_channel=%p from the polling group=%p\n",
			    io_channel, io_channel->group_ch);
	}

	bdev_rbd_free_channel(io_channel);
}

static struct spdk_io_channel *
bdev_rbd_get_io_channel(void *ctx)
{
	struct bdev_rbd *rbd_bdev = ctx;

	return spdk_get_io_channel(rbd_bdev);
}

static int
bdev_rbd_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_rbd *rbd_bdev = ctx;

	spdk_json_write_named_object_begin(w, "rbd");

	spdk_json_write_named_string(w, "pool_name", rbd_bdev->pool_name);

	spdk_json_write_named_string(w, "rbd_name", rbd_bdev->rbd_name);

	if (rbd_bdev->user_id) {
		spdk_json_write_named_string(w, "user_id", rbd_bdev->user_id);
	}

	if (rbd_bdev->config) {
		char **entry = rbd_bdev->config;

		spdk_json_write_named_object_begin(w, "config");
		while (*entry) {
			spdk_json_write_named_string(w, entry[0], entry[1]);
			entry += 2;
		}
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_rbd_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_rbd *rbd = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_rbd_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "pool_name", rbd->pool_name);
	spdk_json_write_named_string(w, "rbd_name", rbd->rbd_name);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	if (rbd->user_id) {
		spdk_json_write_named_string(w, "user_id", rbd->user_id);
	}

	if (rbd->config) {
		char **entry = rbd->config;

		spdk_json_write_named_object_begin(w, "config");
		while (*entry) {
			spdk_json_write_named_string(w, entry[0], entry[1]);
			entry += 2;
		}
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table rbd_fn_table = {
	.destruct		= bdev_rbd_destruct,
	.submit_request		= bdev_rbd_submit_request,
	.io_type_supported	= bdev_rbd_io_type_supported,
	.get_io_channel		= bdev_rbd_get_io_channel,
	.dump_info_json		= bdev_rbd_dump_info_json,
	.write_config_json	= bdev_rbd_write_config_json,
};

int
bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		const char *pool_name,
		const char *const *config,
		const char *rbd_name,
		uint32_t block_size)
{
	struct bdev_rbd *rbd;
	int ret;

	if ((pool_name == NULL) || (rbd_name == NULL)) {
		return -EINVAL;
	}

	rbd = calloc(1, sizeof(struct bdev_rbd));
	if (rbd == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_rbd struct\n");
		return -ENOMEM;
	}

	rbd->rbd_name = strdup(rbd_name);
	if (!rbd->rbd_name) {
		bdev_rbd_free(rbd);
		return -ENOMEM;
	}

	if (user_id) {
		rbd->user_id = strdup(user_id);
		if (!rbd->user_id) {
			bdev_rbd_free(rbd);
			return -ENOMEM;
		}
	}

	rbd->pool_name = strdup(pool_name);
	if (!rbd->pool_name) {
		bdev_rbd_free(rbd);
		return -ENOMEM;
	}

	if (config && !(rbd->config = bdev_rbd_dup_config(config))) {
		bdev_rbd_free(rbd);
		return -ENOMEM;
	}

	ret = bdev_rbd_init(rbd->user_id, rbd->pool_name,
			    (const char *const *)rbd->config,
			    rbd_name, &rbd->info);
	if (ret < 0) {
		bdev_rbd_free(rbd);
		SPDK_ERRLOG("Failed to init rbd device\n");
		return ret;
	}

	if (name) {
		rbd->disk.name = strdup(name);
	} else {
		rbd->disk.name = spdk_sprintf_alloc("Ceph%d", bdev_rbd_count);
	}
	if (!rbd->disk.name) {
		bdev_rbd_free(rbd);
		return -ENOMEM;
	}
	rbd->disk.product_name = "Ceph Rbd Disk";
	bdev_rbd_count++;

	rbd->disk.write_cache = 0;
	rbd->disk.blocklen = block_size;
	rbd->disk.blockcnt = rbd->info.size / rbd->disk.blocklen;
	rbd->disk.ctxt = rbd;
	rbd->disk.fn_table = &rbd_fn_table;
	rbd->disk.module = &rbd_if;

	SPDK_NOTICELOG("Add %s rbd disk to lun\n", rbd->disk.name);

	spdk_io_device_register(rbd, bdev_rbd_create_cb,
				bdev_rbd_destroy_cb,
				sizeof(struct bdev_rbd_io_channel),
				rbd_name);
	ret = spdk_bdev_register(&rbd->disk);
	if (ret) {
		spdk_io_device_unregister(rbd, NULL);
		bdev_rbd_free(rbd);
		return ret;
	}

	*bdev = &(rbd->disk);

	return ret;
}

void
bdev_rbd_delete(struct spdk_bdev *bdev, spdk_delete_rbd_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &rbd_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

int
bdev_rbd_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_mb)
{
	struct spdk_io_channel *ch;
	struct bdev_rbd_io_channel *rbd_io_ch;
	int rc;
	uint64_t new_size_in_byte;
	uint64_t current_size_in_mb;

	if (bdev->module != &rbd_if) {
		return -EINVAL;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (current_size_in_mb > new_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must be lager than current bdev size.\n");
		return -EINVAL;
	}

	ch = bdev_rbd_get_io_channel(bdev);
	rbd_io_ch = spdk_io_channel_get_ctx(ch);
	new_size_in_byte = new_size_in_mb * 1024 * 1024;

	rc = rbd_resize(rbd_io_ch->image, new_size_in_byte);
	if (rc != 0) {
		SPDK_ERRLOG("failed to resize the ceph bdev.\n");
		return rc;
	}

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
		return rc;
	}

	return rc;
}

static int
bdev_rbd_group_poll(void *arg)
{
	struct bdev_rbd_group_channel *group_ch = arg;
	struct epoll_event events[MAX_EVENTS_PER_POLL];
	int num_events, i;

	num_events = epoll_wait(group_ch->epoll_fd, events, MAX_EVENTS_PER_POLL, 0);

	if (num_events <= 0) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < num_events; i++) {
		bdev_rbd_io_poll((struct bdev_rbd_io_channel *)events[i].data.ptr);
	}

	return SPDK_POLLER_BUSY;
}

static int
bdev_rbd_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_group_channel *ch = ctx_buf;

	ch->epoll_fd = epoll_create1(0);
	if (ch->epoll_fd < 0) {
		SPDK_ERRLOG("Could not create epoll fd on io device=%p\n", io_device);
		return -1;
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_rbd_group_poll, ch, 0);

	return 0;
}

static void
bdev_rbd_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_group_channel *ch = ctx_buf;

	if (ch->epoll_fd >= 0) {
		close(ch->epoll_fd);
	}

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_rbd_library_init(void)
{
	spdk_io_device_register(&rbd_if, bdev_rbd_group_create_cb, bdev_rbd_group_destroy_cb,
				sizeof(struct bdev_rbd_group_channel), "bdev_rbd_poll_groups");

	return 0;
}

static void
bdev_rbd_library_fini(void)
{
	spdk_io_device_unregister(&rbd_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_rbd)
