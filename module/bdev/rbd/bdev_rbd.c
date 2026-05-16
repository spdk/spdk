/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "bdev_rbd.h"

#include <rbd/librbd.h>
#include <rados/librados.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

static int bdev_rbd_count = 0;

struct bdev_rbd_pool_ctx {
	rados_t *cluster_p;
	char *name;
	rados_ioctx_t io_ctx;
	uint32_t ref;
	STAILQ_ENTRY(bdev_rbd_pool_ctx) link;
};

static STAILQ_HEAD(, bdev_rbd_pool_ctx) g_map_bdev_rbd_pool_ctx = STAILQ_HEAD_INITIALIZER(
			g_map_bdev_rbd_pool_ctx);

struct bdev_rbd {
	struct spdk_bdev disk;
	char *rbd_name;
	char *user_id;
	char *pool_name;
	char **config;

	rados_t cluster;
	rados_t *cluster_p;
	char *cluster_name;

	union rbd_ctx {
		rados_ioctx_t io_ctx;
		struct bdev_rbd_pool_ctx *ctx;
	} rados_ctx;

	rbd_image_t image;

	rbd_image_info_t info;
	struct spdk_thread *destruct_td;

	TAILQ_ENTRY(bdev_rbd) tailq;
	struct spdk_poller *reset_timer;
	struct spdk_bdev_io *reset_bdev_io;

	uint64_t rbd_watch_handle;
	bool rbd_read_only;
};

struct bdev_rbd_io_channel {
	struct bdev_rbd *disk;
	struct spdk_io_channel *group_ch;
};

struct bdev_rbd_io {
	struct			spdk_thread *submit_td;
	enum			spdk_bdev_io_status status;
	rbd_completion_t	comp;
	size_t			total_len;
};

struct bdev_rbd_cluster {
	char *name;
	char *user_id;
	char **config_param;
	char *config_file;
	char *key_file;
	char *core_mask;
	rados_t cluster;
	uint32_t ref;
	STAILQ_ENTRY(bdev_rbd_cluster) link;
};

static STAILQ_HEAD(, bdev_rbd_cluster) g_map_bdev_rbd_cluster = STAILQ_HEAD_INITIALIZER(
			g_map_bdev_rbd_cluster);
static pthread_mutex_t g_map_bdev_rbd_cluster_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_io_channel *bdev_rbd_get_io_channel(void *ctx);

static void
_rbd_update_callback(void *arg)
{
	struct bdev_rbd *rbd = arg;
	uint64_t current_size_in_bytes = 0;
	int rc;

	rc = rbd_get_size(rbd->image, &current_size_in_bytes);
	if (rc < 0) {
		SPDK_ERRLOG("Failed getting size %d\n", rc);
		return;
	}

	rc = spdk_bdev_notify_blockcnt_change(&rbd->disk, current_size_in_bytes / rbd->disk.blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
	}
}

static void
rbd_update_callback(void *arg)
{
	spdk_thread_send_msg(spdk_thread_get_app_thread(), _rbd_update_callback, arg);
}

static void
bdev_rbd_cluster_free(struct bdev_rbd_cluster *entry)
{
	assert(entry != NULL);

	bdev_rbd_free_config(entry->config_param);
	free(entry->config_file);
	free(entry->key_file);
	free(entry->user_id);
	free(entry->name);
	free(entry->core_mask);
	free(entry);
}

static void
bdev_rbd_put_cluster(rados_t **cluster)
{
	struct bdev_rbd_cluster *entry;

	assert(cluster != NULL);

	/* No need go through the map if *cluster equals to NULL */
	if (*cluster == NULL) {
		return;
	}

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		if (*cluster != &entry->cluster) {
			continue;
		}

		assert(entry->ref > 0);
		entry->ref--;
		*cluster = NULL;
		pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
		return;
	}

	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
	SPDK_ERRLOG("Cannot find the entry for cluster=%p\n", cluster);
}

static void
bdev_rbd_put_pool_ctx(struct bdev_rbd_pool_ctx *entry)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	assert(entry != NULL);
	assert(entry->ref > 0);
	entry->ref--;
	if (entry->ref == 0) {
		STAILQ_REMOVE(&g_map_bdev_rbd_pool_ctx, entry, bdev_rbd_pool_ctx, link);
		rados_ioctx_destroy(entry->io_ctx);
		free(entry->name);
		free(entry);
	}
}

static void
bdev_rbd_free(struct bdev_rbd *rbd)
{
	if (!rbd) {
		return;
	}

	if (rbd->image) {
		if (rbd->rbd_watch_handle) {
			rbd_update_unwatch(rbd->image, rbd->rbd_watch_handle);
		}
		rbd_flush(rbd->image);
		rbd_close(rbd->image);
	}

	free(rbd->disk.name);
	free(rbd->rbd_name);
	free(rbd->user_id);
	free(rbd->pool_name);
	bdev_rbd_free_config(rbd->config);

	if (rbd->cluster_name) {
		/* When rbd is destructed by bdev_rbd_destruct, it will not enter here
		 * because the ctx will already freed by bdev_rbd_free_cb in async manner.
		 * This path only happens during the rbd initialization procedure of rbd */
		if (rbd->rados_ctx.ctx) {
			bdev_rbd_put_pool_ctx(rbd->rados_ctx.ctx);
			rbd->rados_ctx.ctx = NULL;
		}

		bdev_rbd_put_cluster(&rbd->cluster_p);
		free(rbd->cluster_name);
	} else if (rbd->cluster) {
		if (rbd->rados_ctx.io_ctx) {
			rados_ioctx_destroy(rbd->rados_ctx.io_ctx);
		}
		rados_shutdown(rbd->cluster);
	}

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
bdev_rados_cluster_init(const char *user_id, const char *const *config,
			rados_t *cluster)
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
				*cluster = NULL;
				return -1;
			}
			entry += 2;
		}
	} else {
		ret = rados_conf_read_file(*cluster, NULL);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to read conf file\n");
			rados_shutdown(*cluster);
			*cluster = NULL;
			return -1;
		}
	}

	ret = rados_connect(*cluster);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to connect to rbd_pool\n");
		rados_shutdown(*cluster);
		*cluster = NULL;
		return -1;
	}

	return 0;
}

static int
bdev_rbd_get_cluster(const char *cluster_name, rados_t **cluster)
{
	struct bdev_rbd_cluster *entry;

	if (cluster == NULL) {
		SPDK_ERRLOG("cluster should not be NULL\n");
		return -1;
	}

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		if (strcmp(cluster_name, entry->name) == 0) {
			entry->ref++;
			*cluster = &entry->cluster;
			pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
			return 0;
		}
	}

	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
	return -1;
}

static int
bdev_rbd_shared_cluster_init(const char *cluster_name, rados_t **cluster)
{
	int ret;

	ret = bdev_rbd_get_cluster(cluster_name, cluster);
	if (ret < 0) {
		SPDK_ERRLOG("Failed to create rados_t struct\n");
		return -1;
	}

	return ret;
}

static void *
bdev_rbd_cluster_handle(void *arg)
{
	void *ret = arg;
	struct bdev_rbd *rbd = arg;
	int rc;

	rc = bdev_rados_cluster_init(rbd->user_id, (const char *const *)rbd->config,
				     &rbd->cluster);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create rados cluster for user_id=%s and rbd_pool=%s\n",
			    rbd->user_id ? rbd->user_id : "admin (the default)", rbd->pool_name);
		ret = NULL;
	}

	return ret;
}

static int
bdev_rbd_get_pool_ctx(rados_t *cluster_p, const char *name,  struct bdev_rbd_pool_ctx **ctx)
{
	struct bdev_rbd_pool_ctx *entry;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (name == NULL || ctx == NULL) {
		return -1;
	}

	STAILQ_FOREACH(entry, &g_map_bdev_rbd_pool_ctx, link) {
		if (strcmp(name, entry->name) == 0 && cluster_p == entry->cluster_p) {
			entry->ref++;
			*ctx = entry;
			return 0;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		SPDK_ERRLOG("Cannot allocate an entry for name=%s\n", name);
		return -1;
	}

	entry->name = strdup(name);
	if (entry->name == NULL) {
		SPDK_ERRLOG("Failed to allocate the name =%s space on entry =%p\n", name, entry);
		goto err_handle;
	}

	if (rados_ioctx_create(*cluster_p, name, &entry->io_ctx) < 0) {
		goto err_handle1;
	}

	entry->cluster_p = cluster_p;
	entry->ref = 1;
	*ctx = entry;
	STAILQ_INSERT_TAIL(&g_map_bdev_rbd_pool_ctx, entry, link);

	return 0;

err_handle1:
	free(entry->name);
err_handle:
	free(entry);

	return -1;
}

static void *
bdev_rbd_init_context(void *arg)
{
	struct bdev_rbd *rbd = arg;
	int rc;
	rados_ioctx_t *io_ctx = NULL;

	if (rbd->cluster_name) {
		if (bdev_rbd_get_pool_ctx(rbd->cluster_p, rbd->pool_name, &rbd->rados_ctx.ctx) < 0) {
			SPDK_ERRLOG("Failed to create ioctx on rbd=%p with cluster_name=%s\n",
				    rbd, rbd->cluster_name);
			return NULL;
		}
		io_ctx = &rbd->rados_ctx.ctx->io_ctx;
	} else {
		if (rados_ioctx_create(*(rbd->cluster_p), rbd->pool_name, &rbd->rados_ctx.io_ctx) < 0) {
			SPDK_ERRLOG("Failed to create ioctx on rbd=%p\n", rbd);
			return NULL;
		}
		io_ctx = &rbd->rados_ctx.io_ctx;
	}

	assert(io_ctx != NULL);
	if (rbd->rbd_read_only) {
		SPDK_DEBUGLOG(bdev_rbd, "Will open RBD image %s/%s as read-only\n", rbd->pool_name, rbd->rbd_name);
		rc = rbd_open_read_only(*io_ctx, rbd->rbd_name, &rbd->image, NULL);
	} else {
		rc = rbd_open(*io_ctx, rbd->rbd_name, &rbd->image, NULL);
	}
	if (rc < 0) {
		SPDK_ERRLOG("Failed to open specified rbd device\n");
		return NULL;
	}

	rc = rbd_update_watch(rbd->image, &rbd->rbd_watch_handle, rbd_update_callback, (void *)rbd);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to set up watch %d\n", rc);
	}

	rc = rbd_stat(rbd->image, &rbd->info, sizeof(rbd->info));
	if (rc < 0) {
		SPDK_ERRLOG("Failed to stat specified rbd device\n");
		return NULL;
	}

	return arg;
}

static int
bdev_rbd_init(struct bdev_rbd *rbd)
{
	int ret = 0;

	if (!rbd->cluster_name) {
		rbd->cluster_p = &rbd->cluster;
		/* Cluster should be created in non-SPDK thread to avoid conflict between
		 * Rados and SPDK thread */
		if (spdk_call_unaffinitized(bdev_rbd_cluster_handle, rbd) == NULL) {
			SPDK_ERRLOG("Cannot create the rados object on rbd=%p\n", rbd);
			return -1;
		}
	} else {
		ret = bdev_rbd_shared_cluster_init(rbd->cluster_name, &rbd->cluster_p);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to create rados object for rbd =%p on cluster_name=%s\n",
				    rbd, rbd->cluster_name);
			return -1;
		}
	}

	if (spdk_call_unaffinitized(bdev_rbd_init_context, rbd) == NULL) {
		SPDK_ERRLOG("Cannot init rbd context for rbd=%p\n", rbd);
		return -1;
	}

	return ret;
}

static void
_bdev_rbd_io_complete(void *_rbd_io)
{
	struct bdev_rbd_io *rbd_io = _rbd_io;

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(rbd_io), rbd_io->status);
}

static void
bdev_rbd_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct bdev_rbd_io *rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;
	struct spdk_thread *current_thread = spdk_get_thread();

	rbd_io->status = status;
	assert(rbd_io->submit_td != NULL);
	if (rbd_io->submit_td != current_thread) {
		spdk_thread_send_msg(rbd_io->submit_td, _bdev_rbd_io_complete, rbd_io);
	} else {
		_bdev_rbd_io_complete(rbd_io);
	}
}

static void
bdev_rbd_finish_aiocb(rbd_completion_t cb, void *arg)
{
	int io_status;
	struct spdk_bdev_io *bdev_io;
	struct bdev_rbd_io *rbd_io;
	enum spdk_bdev_io_status bio_status;

	bdev_io = rbd_aio_get_arg(cb);
	rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;
	io_status = rbd_aio_get_return_value(cb);
	bio_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		if ((int)rbd_io->total_len != io_status) {
			bio_status = SPDK_BDEV_IO_STATUS_FAILED;
		}
#ifdef LIBRBD_SUPPORTS_COMPARE_AND_WRITE_IOVEC
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE && io_status == -EILSEQ) {
		bio_status = SPDK_BDEV_IO_STATUS_MISCOMPARE;
#endif
	} else if (io_status != 0) { /* For others, 0 means success */
		bio_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	rbd_aio_release(cb);

	bdev_rbd_io_complete(bdev_io, bio_status);
}

static void
_bdev_rbd_start_aio(struct bdev_rbd *disk, struct spdk_bdev_io *bdev_io,
		    struct iovec *iov, int iovcnt, uint64_t offset, size_t len)
{
	int ret;
	struct bdev_rbd_io *rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;
	rbd_image_t image = disk->image;

	ret = rbd_aio_create_completion(bdev_io, bdev_rbd_finish_aiocb,
					&rbd_io->comp);
	if (ret < 0) {
		goto err;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		rbd_io->total_len = len;
		if (spdk_likely(iovcnt == 1)) {
			ret = rbd_aio_read(image, offset, iov[0].iov_len, iov[0].iov_base,
					   rbd_io->comp);
		} else {
			ret = rbd_aio_readv(image, iov, iovcnt, offset, rbd_io->comp);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (spdk_likely(iovcnt == 1)) {
			ret = rbd_aio_write(image, offset, iov[0].iov_len, iov[0].iov_base,
					    rbd_io->comp);
		} else {
			ret = rbd_aio_writev(image, iov, iovcnt, offset, rbd_io->comp);
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		ret = rbd_aio_discard(image, offset, len, rbd_io->comp);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		ret = rbd_aio_flush(image, rbd_io->comp);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		ret = rbd_aio_write_zeroes(image, offset, len, rbd_io->comp, /* zero_flags */ 0,
					   /* op_flags */ 0);
		break;
#ifdef LIBRBD_SUPPORTS_COMPARE_AND_WRITE_IOVEC
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		ret = rbd_aio_compare_and_writev(image, offset, iov /* cmp */, iovcnt,
						 bdev_io->u.bdev.fused_iovs /* write */,
						 bdev_io->u.bdev.fused_iovcnt,
						 rbd_io->comp, NULL,
						 /* op_flags */ 0);
		break;
#endif
	default:
		/* This should not happen.
		 * Function should only be called with supported io types in bdev_rbd_submit_request
		 */
		SPDK_ERRLOG("Unsupported IO type =%d\n", bdev_io->type);
		ret = -ENOTSUP;
		break;
	}

	if (ret < 0) {
		rbd_aio_release(rbd_io->comp);
		goto err;
	}

	return;

err:
	bdev_rbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
bdev_rbd_start_aio(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct bdev_rbd *disk = (struct bdev_rbd *)bdev_io->bdev->ctxt;

	_bdev_rbd_start_aio(disk,
			    bdev_io,
			    bdev_io->u.bdev.iovs,
			    bdev_io->u.bdev.iovcnt,
			    bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
			    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
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

static int bdev_rbd_reset_timer(void *arg);

static void
bdev_rbd_check_outstanding_ios(struct spdk_bdev *bdev, uint64_t current_qd,
			       void *cb_arg, int rc)
{
	struct bdev_rbd *disk = cb_arg;
	enum spdk_bdev_io_status bio_status;

	if (rc == 0 && current_qd > 0) {
		disk->reset_timer = SPDK_POLLER_REGISTER(bdev_rbd_reset_timer, disk, 1000);
		return;
	}

	if (rc != 0) {
		bio_status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		bio_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	}

	bdev_rbd_io_complete(disk->reset_bdev_io, bio_status);
	disk->reset_bdev_io = NULL;
}

static int
bdev_rbd_reset_timer(void *arg)
{
	struct bdev_rbd *disk = arg;

	spdk_poller_unregister(&disk->reset_timer);

	spdk_bdev_get_current_qd(&disk->disk, bdev_rbd_check_outstanding_ios, disk);

	return SPDK_POLLER_BUSY;
}

static void
bdev_rbd_reset(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct bdev_rbd *disk = (struct bdev_rbd *)bdev_io->bdev->ctxt;

	/*
	 * HACK: Since librbd doesn't provide any way to cancel outstanding aio, just kick off a
	 * poller to wait for in-flight I/O to complete.
	 */
	assert(disk->reset_bdev_io == NULL);
	disk->reset_bdev_io = bdev_io;

	bdev_rbd_reset_timer(disk);
}

static void
_bdev_rbd_destruct_done(void *io_device)
{
	struct bdev_rbd *rbd = io_device;

	assert(rbd != NULL);

	spdk_bdev_destruct_done(&rbd->disk, 0);
	bdev_rbd_free(rbd);
}

static void
bdev_rbd_free_cb(void *io_device)
{
	struct bdev_rbd *rbd = io_device;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	/* free the ctx */
	if (rbd->cluster_name && rbd->rados_ctx.ctx) {
		bdev_rbd_put_pool_ctx(rbd->rados_ctx.ctx);
		rbd->rados_ctx.ctx = NULL;
	}

	/* The io device has been unregistered.  Send a message back to the
	 * original thread that started the destruct operation, so that the
	 * bdev unregister callback is invoked on the same thread that started
	 * this whole process.
	 */
	spdk_thread_send_msg(rbd->destruct_td, _bdev_rbd_destruct_done, rbd);
}

static void
_bdev_rbd_destruct(void *ctx)
{
	struct bdev_rbd *rbd = ctx;

	spdk_io_device_unregister(rbd, bdev_rbd_free_cb);
}

static int
bdev_rbd_destruct(void *ctx)
{
	struct bdev_rbd *rbd = ctx;

	/* Start the destruct operation on the rbd bdev's
	 * main thread.  This guarantees it will only start
	 * executing after any messages related to channel
	 * deletions have finished completing.  *Always*
	 * send a message, even if this function gets called
	 * from the main thread, in case there are pending
	 * channel delete messages in flight to this thread.
	 */
	assert(rbd->destruct_td == NULL);
	rbd->destruct_td = spdk_get_thread();
	spdk_thread_send_msg(spdk_thread_get_app_thread(), _bdev_rbd_destruct, rbd);

	/* Return 1 to indicate the destruct path is asynchronous. */
	return 1;
}

static void
bdev_rbd_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		bdev_rbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_rbd_start_aio(bdev_io);
}

static void
bdev_rbd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_thread *submit_td = spdk_io_channel_get_thread(ch);
	struct bdev_rbd_io *rbd_io = (struct bdev_rbd_io *)bdev_io->driver_ctx;

	rbd_io->submit_td = submit_td;
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_rbd_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
#ifdef LIBRBD_SUPPORTS_COMPARE_AND_WRITE_IOVEC
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
#endif
		bdev_rbd_start_aio(bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_thread_exec_msg(spdk_thread_get_app_thread(), bdev_rbd_reset, bdev_io);
		break;

	default:
		SPDK_ERRLOG("Unsupported IO type =%d\n", bdev_io->type);
		bdev_rbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
bdev_rbd_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct bdev_rbd *rbd = (struct bdev_rbd *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
#ifdef LIBRBD_SUPPORTS_COMPARE_AND_WRITE_IOVEC
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
#endif
		return !rbd->rbd_read_only;
	default:
		return false;
	}
}

static int
bdev_rbd_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *ch = ctx_buf;
	struct bdev_rbd *disk = io_device;

	ch->disk = disk;
	ch->group_ch = spdk_get_io_channel(&rbd_if);
	assert(ch->group_ch != NULL);

	return 0;
}

static void
bdev_rbd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_rbd_io_channel *ch = ctx_buf;

	spdk_put_io_channel(ch->group_ch);
}

static struct spdk_io_channel *
bdev_rbd_get_io_channel(void *ctx)
{
	struct bdev_rbd *rbd_bdev = ctx;

	return spdk_get_io_channel(rbd_bdev);
}

static void
bdev_rbd_cluster_dump_entry(const char *cluster_name, struct spdk_json_write_ctx *w)
{
	struct bdev_rbd_cluster *entry;

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		if (strcmp(cluster_name, entry->name)) {
			continue;
		}
		if (entry->user_id) {
			spdk_json_write_named_string(w, "user_id", entry->user_id);
		}

		if (entry->config_param) {
			char **config_entry = entry->config_param;

			spdk_json_write_named_object_begin(w, "config_param");
			while (*config_entry) {
				spdk_json_write_named_string(w, config_entry[0], config_entry[1]);
				config_entry += 2;
			}
			spdk_json_write_object_end(w);
		}
		if (entry->config_file) {
			spdk_json_write_named_string(w, "config_file", entry->config_file);
		}
		if (entry->key_file) {
			spdk_json_write_named_string(w, "key_file", entry->key_file);
		}

		pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
		return;
	}

	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
}

static int
bdev_rbd_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_rbd *rbd_bdev = ctx;

	spdk_json_write_named_object_begin(w, "rbd");

	spdk_json_write_named_string(w, "pool_name", rbd_bdev->pool_name);

	spdk_json_write_named_string(w, "rbd_name", rbd_bdev->rbd_name);

	if (rbd_bdev->cluster_name) {
		bdev_rbd_cluster_dump_entry(rbd_bdev->cluster_name, w);
		goto end;
	}

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

end:
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

	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
dump_single_cluster_entry(struct bdev_rbd_cluster *entry, struct spdk_json_write_ctx *w)
{
	assert(entry != NULL);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "cluster_name", entry->name);

	if (entry->user_id) {
		spdk_json_write_named_string(w, "user_id", entry->user_id);
	}

	if (entry->config_param) {
		char **config_entry = entry->config_param;

		spdk_json_write_named_object_begin(w, "config_param");
		while (*config_entry) {
			spdk_json_write_named_string(w, config_entry[0], config_entry[1]);
			config_entry += 2;
		}
		spdk_json_write_object_end(w);
	}
	if (entry->config_file) {
		spdk_json_write_named_string(w, "config_file", entry->config_file);
	}
	if (entry->key_file) {
		spdk_json_write_named_string(w, "key_file", entry->key_file);
	}

	if (entry->core_mask) {
		spdk_json_write_named_string(w, "core_mask", entry->core_mask);
	}

	spdk_json_write_object_end(w);
}

int
bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name)
{
	struct bdev_rbd_cluster *entry;
	struct spdk_json_write_ctx *w;

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);

	if (STAILQ_EMPTY(&g_map_bdev_rbd_cluster)) {
		pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
		return -ENOENT;
	}

	/* If cluster name is provided */
	if (name) {
		STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
			if (strcmp(name, entry->name) == 0) {
				w = spdk_jsonrpc_begin_result(request);
				dump_single_cluster_entry(entry, w);
				spdk_jsonrpc_end_result(request, w);

				pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
				return 0;
			}
		}

		pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
		return -ENOENT;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		dump_single_cluster_entry(entry, w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);

	return 0;
}

static const struct spdk_bdev_fn_table rbd_fn_table = {
	.destruct		= bdev_rbd_destruct,
	.submit_request		= bdev_rbd_submit_request,
	.io_type_supported	= bdev_rbd_io_type_supported,
	.get_io_channel		= bdev_rbd_get_io_channel,
	.dump_info_json		= bdev_rbd_dump_info_json,
	.write_config_json	= bdev_rbd_write_config_json,
};

static int
rbd_thread_set_cpumask(struct spdk_cpuset *set)
{
#ifdef __linux__
	uint32_t lcore;
	cpu_set_t mask;

	assert(set != NULL);
	CPU_ZERO(&mask);

	/* get the core id on current spdk_cpuset and set to cpu_set_t */
	for (lcore = 0; lcore < SPDK_CPUSET_SIZE; lcore++) {
		if (spdk_cpuset_get_cpu(set, lcore)) {
			CPU_SET(lcore, &mask);
		}
	}

	/* change current thread core mask */
	if (sched_setaffinity(0, sizeof(mask), &mask) < 0) {
		SPDK_ERRLOG("Set non SPDK thread cpu mask error (errno=%d)\n", errno);
		return -1;
	}

	return 0;
#else
	SPDK_ERRLOG("SPDK non spdk thread cpumask setup supports only Linux platform now.\n");
	return -ENOTSUP;
#endif
}


static int
rbd_register_cluster(const char *name, const char *user_id, const char *const *config_param,
		     const char *config_file, const char *key_file, const char *core_mask)
{
	struct bdev_rbd_cluster *entry;
	struct spdk_cpuset rbd_core_mask = {};
	int rc;

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		if (strcmp(name, entry->name) == 0) {
			SPDK_ERRLOG("Cluster name=%s already exists\n", name);
			pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
			return -1;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		SPDK_ERRLOG("Cannot allocate an entry for name=%s\n", name);
		pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
		return -1;
	}

	entry->name = strdup(name);
	if (entry->name == NULL) {
		SPDK_ERRLOG("Failed to save the name =%s on entry =%p\n", name, entry);
		goto err_handle;
	}

	if (user_id) {
		entry->user_id = strdup(user_id);
		if (entry->user_id == NULL) {
			SPDK_ERRLOG("Failed to save the str =%s on entry =%p\n", user_id, entry);
			goto err_handle;
		}
	}

	/* Support specify config_param or config_file separately, or both of them. */
	if (config_param) {
		entry->config_param = bdev_rbd_dup_config(config_param);
		if (entry->config_param == NULL) {
			SPDK_ERRLOG("Failed to save the config_param=%p on entry = %p\n", config_param, entry);
			goto err_handle;
		}
	}

	if (config_file) {
		entry->config_file = strdup(config_file);
		if (entry->config_file == NULL) {
			SPDK_ERRLOG("Failed to save the config_file=%s on entry = %p\n", config_file, entry);
			goto err_handle;
		}
	}

	if (key_file) {
		entry->key_file = strdup(key_file);
		if (entry->key_file == NULL) {
			SPDK_ERRLOG("Failed to save the key_file=%s on entry = %p\n", key_file, entry);
			goto err_handle;
		}
	}

	if (core_mask) {
		entry->core_mask = strdup(core_mask);
		if (entry->core_mask == NULL) {
			SPDK_ERRLOG("Core_mask=%s allocation failed on entry = %p\n", core_mask, entry);
			goto err_handle;
		}

		if (spdk_cpuset_parse(&rbd_core_mask, entry->core_mask) < 0) {
			SPDK_ERRLOG("Invalid cpumask=%s on entry = %p\n", entry->core_mask, entry);
			goto err_handle;
		}

		if (rbd_thread_set_cpumask(&rbd_core_mask) < 0) {
			SPDK_ERRLOG("Failed to change rbd threads to core_mask %s on entry = %p\n", core_mask, entry);
			goto err_handle;
		}
	}


	/* If rbd thread core mask is given, rados_create() must execute with
	 * the affinity set by rbd_thread_set_cpumask(). The affinity set
	 * by rbd_thread_set_cpumask() will be reverted once rbd_register_cluster() returns
	 * and when we leave the spdk_call_unaffinitized context. */
	rc = rados_create(&entry->cluster, user_id);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create rados_t struct\n");
		goto err_handle;
	}

	/* Try default location when entry->config_file is NULL, but ignore failure when it is NULL */
	rc = rados_conf_read_file(entry->cluster, entry->config_file);
	if (entry->config_file && rc < 0) {
		SPDK_ERRLOG("Failed to read conf file %s\n", entry->config_file);
		rados_shutdown(entry->cluster);
		goto err_handle;
	}

	if (config_param) {
		const char *const *config_entry = config_param;
		while (*config_entry) {
			rc = rados_conf_set(entry->cluster, config_entry[0], config_entry[1]);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to set %s = %s\n", config_entry[0], config_entry[1]);
				rados_shutdown(entry->cluster);
				goto err_handle;
			}
			config_entry += 2;
		}
	}

	if (key_file) {
		rc = rados_conf_set(entry->cluster, "keyring", key_file);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to set keyring = %s\n", key_file);
			rados_shutdown(entry->cluster);
			goto err_handle;
		}
	}

	rc = rados_connect(entry->cluster);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect to rbd_pool on cluster=%p\n", entry->cluster);
		rados_shutdown(entry->cluster);
		goto err_handle;
	}

	STAILQ_INSERT_TAIL(&g_map_bdev_rbd_cluster, entry, link);
	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);

	return 0;

err_handle:
	bdev_rbd_cluster_free(entry);
	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
	return -1;
}

int
bdev_rbd_unregister_cluster(const char *name)
{
	struct bdev_rbd_cluster *entry;
	int rc = 0;

	if (name == NULL) {
		return -1;
	}

	pthread_mutex_lock(&g_map_bdev_rbd_cluster_mutex);
	STAILQ_FOREACH(entry, &g_map_bdev_rbd_cluster, link) {
		if (strcmp(name, entry->name) == 0) {
			if (entry->ref == 0) {
				STAILQ_REMOVE(&g_map_bdev_rbd_cluster, entry, bdev_rbd_cluster, link);
				rados_shutdown(entry->cluster);
				bdev_rbd_cluster_free(entry);
			} else {
				SPDK_ERRLOG("Cluster with name=%p is still used and we cannot delete it\n",
					    entry->name);
				rc = -1;
			}

			pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);
			return rc;
		}
	}

	pthread_mutex_unlock(&g_map_bdev_rbd_cluster_mutex);

	SPDK_ERRLOG("Could not find the cluster name =%p\n", name);

	return -1;
}

static void *
_bdev_rbd_register_cluster(void *arg)
{
	struct cluster_register_info *info = arg;
	void *ret = arg;
	int rc;

	rc = rbd_register_cluster((const char *)info->name, (const char *)info->user_id,
				  (const char *const *)info->config_param, (const char *)info->config_file,
				  (const char *)info->key_file, info->core_mask);
	if (rc) {
		ret = NULL;
	}

	return ret;
}

int
bdev_rbd_register_cluster(struct cluster_register_info *info)
{
	assert(info != NULL);

	/* Rados cluster info need to be created in non SPDK-thread to avoid CPU
	 * resource contention */
	if (spdk_call_unaffinitized(_bdev_rbd_register_cluster, info) == NULL) {
		return -1;
	}

	return 0;
}

int
bdev_rbd_create(struct spdk_bdev **bdev, const char *name, const char *user_id,
		const char *pool_name,
		const char *const *config,
		const char *rbd_name,
		uint32_t block_size,
		const char *cluster_name,
		const struct spdk_uuid *uuid,
		bool read_only)
{
	struct bdev_rbd *rbd;
	int ret;

	if ((pool_name == NULL) || (rbd_name == NULL) || (block_size == 0)) {
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

	if (cluster_name) {
		rbd->cluster_name = strdup(cluster_name);
		if (!rbd->cluster_name) {
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

	rbd->rbd_read_only = read_only;
	ret = bdev_rbd_init(rbd);
	if (ret < 0) {
		bdev_rbd_free(rbd);
		SPDK_ERRLOG("Failed to init rbd device\n");
		return ret;
	}

	rbd->disk.uuid = *uuid;
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
bdev_rbd_delete(const char *name, spdk_delete_rbd_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &rbd_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_rbd_resize(const char *name, const uint64_t new_size_in_mb)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct bdev_rbd *rbd;
	int rc = 0;
	uint64_t new_size_in_byte;
	uint64_t current_size_in_mb;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	if (bdev->module != &rbd_if) {
		rc = -EINVAL;
		goto exit;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (current_size_in_mb > new_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must be larger than current bdev size.\n");
		rc = -EINVAL;
		goto exit;
	}

	rbd = SPDK_CONTAINEROF(bdev, struct bdev_rbd, disk);
	new_size_in_byte = new_size_in_mb * 1024 * 1024;
	rc = rbd_resize(rbd->image, new_size_in_byte);
	if (rc != 0) {
		SPDK_ERRLOG("failed to resize the ceph bdev.\n");
		goto exit;
	}

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

static int
bdev_rbd_group_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_rbd_group_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
bdev_rbd_library_init(void)
{
	spdk_io_device_register(&rbd_if, bdev_rbd_group_create_cb, bdev_rbd_group_destroy_cb,
				0, "bdev_rbd_poll_groups");
	return 0;
}

static void
bdev_rbd_library_fini(void)
{
	spdk_io_device_unregister(&rbd_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_rbd)
