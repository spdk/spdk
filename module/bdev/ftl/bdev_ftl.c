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
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/ftl.h"
#include "spdk_internal/log.h"

#include "bdev_ftl.h"

#define FTL_COMPLETION_RING_SIZE 4096

struct ftl_bdev {
	struct spdk_bdev		bdev;

	struct spdk_ftl_dev		*dev;

	ftl_bdev_init_fn		init_cb;

	void				*init_arg;
};

struct ftl_io_channel {
	struct spdk_ftl_dev		*dev;

	struct spdk_poller		*poller;

#define FTL_MAX_COMPLETIONS 64
	struct ftl_bdev_io		*io[FTL_MAX_COMPLETIONS];

	/* Completion ring */
	struct spdk_ring		*ring;

	struct spdk_io_channel		*ioch;
};

struct ftl_bdev_io {
	struct ftl_bdev			*bdev;

	struct spdk_ring		*ring;

	int				status;
};

struct ftl_deferred_init {
	struct ftl_bdev_init_opts	opts;

	LIST_ENTRY(ftl_deferred_init)	entry;
};

static LIST_HEAD(, ftl_deferred_init)	g_deferred_init = LIST_HEAD_INITIALIZER(g_deferred_init);

static int bdev_ftl_initialize(void);
static void bdev_ftl_finish(void);
static void bdev_ftl_examine(struct spdk_bdev *bdev);

static int
bdev_ftl_get_ctx_size(void)
{
	return sizeof(struct ftl_bdev_io);
}

static struct spdk_bdev_module g_ftl_if = {
	.name		= "ftl",
	.module_init	= bdev_ftl_initialize,
	.module_fini	= bdev_ftl_finish,
	.examine_disk	= bdev_ftl_examine,
	.get_ctx_size	= bdev_ftl_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ftl, &g_ftl_if)

static void
bdev_ftl_free_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_bdev *ftl_bdev = ctx;

	spdk_io_device_unregister(ftl_bdev, NULL);

	spdk_bdev_destruct_done(&ftl_bdev->bdev, status);
	free(ftl_bdev->bdev.name);
	free(ftl_bdev);
}

static int
bdev_ftl_destruct(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;
	spdk_ftl_dev_free(ftl_bdev->dev, bdev_ftl_free_cb, ftl_bdev);

	/* return 1 to indicate that the destruction is asynchronous */
	return 1;
}

static void
bdev_ftl_complete_io(struct ftl_bdev_io *io, int rc)
{
	enum spdk_bdev_io_status status;

	switch (rc) {
	case 0:
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -ENOMEM:
		status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	default:
		status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), status);
}

static void
bdev_ftl_cb(void *arg, int status)
{
	struct ftl_bdev_io *io = arg;
	size_t cnt __attribute__((unused));

	io->status = status;

	cnt = spdk_ring_enqueue(io->ring, (void **)&io, 1, NULL);
	assert(cnt == 1);
}

static int
bdev_ftl_fill_bio(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
		  struct ftl_bdev_io *io)
{
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(ch);

	memset(io, 0, sizeof(*io));

	io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	io->ring = ioch->ring;
	io->bdev = ftl_bdev;
	return 0;
}

static int
bdev_ftl_readv(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
	       struct ftl_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(ch);
	int rc;

	bio = spdk_bdev_io_from_ctx(io);

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_read(ftl_bdev->dev,
			     ioch->ioch,
			     bio->u.bdev.offset_blocks,
			     bio->u.bdev.num_blocks,
			     bio->u.bdev.iovs, bio->u.bdev.iovcnt, bdev_ftl_cb, io);
}

static int
bdev_ftl_writev(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
		struct ftl_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ftl_io_channel *ioch;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);
	ioch = spdk_io_channel_get_ctx(ch);

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_write(ftl_bdev->dev,
			      ioch->ioch,
			      bio->u.bdev.offset_blocks,
			      bio->u.bdev.num_blocks,
			      bio->u.bdev.iovs,
			      bio->u.bdev.iovcnt, bdev_ftl_cb, io);
}

static void
bdev_ftl_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx,
				     SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	int rc = bdev_ftl_readv((struct ftl_bdev *)bdev_io->bdev->ctxt,
				ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static int
bdev_ftl_flush(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch, struct ftl_bdev_io *io)
{
	int rc;

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_flush(ftl_bdev->dev, bdev_ftl_cb, io);
}

static int
_bdev_ftl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct ftl_bdev *ftl_bdev = (struct ftl_bdev *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_ftl_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_ftl_writev(ftl_bdev, ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_ftl_flush(ftl_bdev, ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return -ENOTSUP;
		break;
	}
}

static void
bdev_ftl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = _bdev_ftl_submit_request(ch, bdev_io);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static bool
bdev_ftl_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_ftl_get_io_channel(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;

	return spdk_get_io_channel(ftl_bdev);
}

static void
_bdev_ftl_write_config_info(struct ftl_bdev *ftl_bdev, struct spdk_json_write_ctx *w)
{
	struct spdk_ftl_attrs attrs = {};

	spdk_ftl_dev_get_attrs(ftl_bdev->dev, &attrs);

	spdk_json_write_named_string(w, "base_bdev", attrs.base_bdev);

	if (attrs.cache_bdev) {
		spdk_json_write_named_string(w, "cache", attrs.cache_bdev);
	}
}

static void
bdev_ftl_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct ftl_bdev *ftl_bdev = bdev->ctxt;
	struct spdk_ftl_attrs attrs;
	struct spdk_ftl_conf *conf = &attrs.conf;
	char uuid[SPDK_UUID_STRING_LEN];

	spdk_ftl_dev_get_attrs(ftl_bdev->dev, &attrs);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ftl_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", ftl_bdev->bdev.name);

	spdk_json_write_named_bool(w, "allow_open_bands", conf->allow_open_bands);
	spdk_json_write_named_uint64(w, "overprovisioning", conf->lba_rsvd);
	spdk_json_write_named_uint64(w, "limit_crit", conf->limits[SPDK_FTL_LIMIT_CRIT].limit);
	spdk_json_write_named_uint64(w, "limit_crit_threshold", conf->limits[SPDK_FTL_LIMIT_CRIT].thld);
	spdk_json_write_named_uint64(w, "limit_high", conf->limits[SPDK_FTL_LIMIT_HIGH].limit);
	spdk_json_write_named_uint64(w, "limit_high_threshold", conf->limits[SPDK_FTL_LIMIT_HIGH].thld);
	spdk_json_write_named_uint64(w, "limit_low", conf->limits[SPDK_FTL_LIMIT_LOW].limit);
	spdk_json_write_named_uint64(w, "limit_low_threshold", conf->limits[SPDK_FTL_LIMIT_LOW].thld);
	spdk_json_write_named_uint64(w, "limit_start", conf->limits[SPDK_FTL_LIMIT_START].limit);
	spdk_json_write_named_uint64(w, "limit_start_threshold", conf->limits[SPDK_FTL_LIMIT_START].thld);

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &attrs.uuid);
	spdk_json_write_named_string(w, "uuid", uuid);

	_bdev_ftl_write_config_info(ftl_bdev, w);

	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static int
bdev_ftl_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct ftl_bdev *ftl_bdev = ctx;
	struct spdk_ftl_attrs attrs;

	spdk_ftl_dev_get_attrs(ftl_bdev->dev, &attrs);

	spdk_json_write_named_object_begin(w, "ftl");

	_bdev_ftl_write_config_info(ftl_bdev, w);
	spdk_json_write_named_string_fmt(w, "num_zones", "%zu", attrs.num_zones);
	spdk_json_write_named_string_fmt(w, "zone_size", "%zu", attrs.zone_size);

	/* ftl */
	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table ftl_fn_table = {
	.destruct		= bdev_ftl_destruct,
	.submit_request		= bdev_ftl_submit_request,
	.io_type_supported	= bdev_ftl_io_type_supported,
	.get_io_channel		= bdev_ftl_get_io_channel,
	.write_config_json	= bdev_ftl_write_config_json,
	.dump_info_json		= bdev_ftl_dump_info_json,
};

static int
bdev_ftl_poll(void *arg)
{
	struct ftl_io_channel *ch = arg;
	size_t cnt, i;

	cnt = spdk_ring_dequeue(ch->ring, (void **)&ch->io, FTL_MAX_COMPLETIONS);

	for (i = 0; i < cnt; ++i) {
		bdev_ftl_complete_io(ch->io[i], ch->io[i]->status);
	}

	return cnt;
}

static int
bdev_ftl_io_channel_create_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel *ch = ctx;
	struct ftl_bdev *ftl_bdev = (struct ftl_bdev *)io_device;

	ch->dev = ftl_bdev->dev;
	ch->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, FTL_COMPLETION_RING_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);

	if (!ch->ring) {
		return -ENOMEM;
	}

	ch->poller = spdk_poller_register(bdev_ftl_poll, ch, 0);
	if (!ch->poller) {
		spdk_ring_free(ch->ring);
		return -ENOMEM;
	}

	ch->ioch = spdk_get_io_channel(ftl_bdev->dev);

	return 0;
}

static void
bdev_ftl_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ftl_io_channel *ch = ctx_buf;

	spdk_ring_free(ch->ring);
	spdk_poller_unregister(&ch->poller);
	spdk_put_io_channel(ch->ioch);
}

static void
bdev_ftl_create_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_bdev		*ftl_bdev = ctx;
	struct ftl_bdev_info	info = {};
	struct spdk_ftl_attrs	attrs;
	ftl_bdev_init_fn	init_cb = ftl_bdev->init_cb;
	void			*init_arg = ftl_bdev->init_arg;
	int			rc = -ENODEV;

	if (status) {
		SPDK_ERRLOG("Failed to create FTL device (%d)\n", status);
		rc = status;
		goto error_dev;
	}

	spdk_ftl_dev_get_attrs(dev, &attrs);

	ftl_bdev->dev = dev;
	ftl_bdev->bdev.product_name = "FTL disk";
	ftl_bdev->bdev.write_cache = 0;
	ftl_bdev->bdev.blocklen = attrs.block_size;
	ftl_bdev->bdev.blockcnt = attrs.num_blocks;
	ftl_bdev->bdev.uuid = attrs.uuid;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "Creating bdev %s:\n", ftl_bdev->bdev.name);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "\tblock_len:\t%zu\n", attrs.block_size);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "\tnum_blocks:\t%"PRIu64"\n", attrs.num_blocks);

	ftl_bdev->bdev.ctxt = ftl_bdev;
	ftl_bdev->bdev.fn_table = &ftl_fn_table;
	ftl_bdev->bdev.module = &g_ftl_if;

	spdk_io_device_register(ftl_bdev, bdev_ftl_io_channel_create_cb,
				bdev_ftl_io_channel_destroy_cb,
				sizeof(struct ftl_io_channel),
				ftl_bdev->bdev.name);

	if (spdk_bdev_register(&ftl_bdev->bdev)) {
		goto error_unregister;
	}

	info.name = ftl_bdev->bdev.name;
	info.uuid = ftl_bdev->bdev.uuid;

	init_cb(&info, init_arg, 0);
	return;

error_unregister:
	spdk_io_device_unregister(ftl_bdev, NULL);
error_dev:
	free(ftl_bdev->bdev.name);
	free(ftl_bdev);

	init_cb(NULL, init_arg, rc);
}

static void
bdev_ftl_defer_free(struct ftl_deferred_init *init)
{
	free((char *)init->opts.name);
	free((char *)init->opts.base_bdev);
	free((char *)init->opts.cache_bdev);
	free(init);
}

static int
bdev_ftl_defer_init(const struct ftl_bdev_init_opts *opts)
{
	struct ftl_deferred_init *init;

	init = calloc(1, sizeof(*init));
	if (!init) {
		return -ENOMEM;
	}

	init->opts.mode = opts->mode;
	init->opts.uuid = opts->uuid;
	init->opts.ftl_conf = opts->ftl_conf;

	init->opts.name = strdup(opts->name);
	if (!init->opts.name) {
		SPDK_ERRLOG("Could not allocate bdev name\n");
		goto error;
	}

	init->opts.base_bdev = strdup(opts->base_bdev);
	if (!init->opts.base_bdev) {
		SPDK_ERRLOG("Could not allocate base bdev name\n");
		goto error;
	}

	if (opts->cache_bdev) {
		init->opts.cache_bdev = strdup(opts->cache_bdev);
		if (!init->opts.cache_bdev) {
			SPDK_ERRLOG("Could not allocate cache bdev name\n");
			goto error;
		}
	}

	LIST_INSERT_HEAD(&g_deferred_init, init, entry);

	return 0;

error:
	bdev_ftl_defer_free(init);
	return -ENOMEM;
}

int
bdev_ftl_create_bdev(const struct ftl_bdev_init_opts *bdev_opts,
		     ftl_bdev_init_fn cb, void *cb_arg)
{
	struct ftl_bdev *ftl_bdev = NULL;
	struct spdk_ftl_dev_init_opts opts = {};
	int rc;

	ftl_bdev = calloc(1, sizeof(*ftl_bdev));
	if (!ftl_bdev) {
		SPDK_ERRLOG("Could not allocate ftl_bdev\n");
		return -ENOMEM;
	}

	ftl_bdev->bdev.name = strdup(bdev_opts->name);
	if (!ftl_bdev->bdev.name) {
		rc = -ENOMEM;
		goto error_bdev;
	}

	if (spdk_bdev_get_by_name(bdev_opts->base_bdev) == NULL ||
	    (bdev_opts->cache_bdev && spdk_bdev_get_by_name(bdev_opts->cache_bdev) == NULL)) {
		rc = bdev_ftl_defer_init(bdev_opts);
		if (rc == 0) {
			rc = -ENODEV;
		}
		goto error_name;
	}

	ftl_bdev->init_cb = cb;
	ftl_bdev->init_arg = cb_arg;

	opts.mode = bdev_opts->mode;
	opts.uuid = bdev_opts->uuid;
	opts.name = ftl_bdev->bdev.name;
	opts.base_bdev = bdev_opts->base_bdev;
	opts.cache_bdev = bdev_opts->cache_bdev;
	opts.conf = &bdev_opts->ftl_conf;

	/* TODO: set threads based on config */
	opts.core_thread = opts.read_thread = spdk_get_thread();

	rc = spdk_ftl_dev_init(&opts, bdev_ftl_create_cb, ftl_bdev);
	if (rc) {
		SPDK_ERRLOG("Could not create FTL device\n");
		goto error_name;
	}

	return 0;

error_name:
	free(ftl_bdev->bdev.name);
error_bdev:
	free(ftl_bdev);
	return rc;
}

static int
bdev_ftl_initialize(void)
{
	return 0;
}

void
bdev_ftl_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(name);
	if (bdev) {
		spdk_bdev_unregister(bdev, cb_fn, cb_arg);
		return;
	}

	cb_fn(cb_arg, -ENODEV);
}

static void
bdev_ftl_finish(void)
{
}

static void
bdev_ftl_create_defered_cb(const struct ftl_bdev_info *info, void *ctx, int status)
{
	struct ftl_deferred_init *opts = ctx;

	if (status) {
		SPDK_ERRLOG("Failed to initialize FTL bdev '%s'\n", opts->opts.name);
	}

	bdev_ftl_defer_free(opts);

	spdk_bdev_module_examine_done(&g_ftl_if);
}

static void
bdev_ftl_examine(struct spdk_bdev *bdev)
{
	struct ftl_deferred_init *opts;

	LIST_FOREACH(opts, &g_deferred_init, entry) {
		if (spdk_bdev_get_by_name(opts->opts.base_bdev) == NULL) {
			continue;
		}

		if (opts->opts.cache_bdev && spdk_bdev_get_by_name(opts->opts.base_bdev) == NULL) {
			continue;
		}

		LIST_REMOVE(opts, entry);

		/* spdk_bdev_module_examine_done will be called by bdev_ftl_create_defered_cb */
		if (bdev_ftl_create_bdev(&opts->opts, bdev_ftl_create_defered_cb, opts)) {
			SPDK_ERRLOG("Failed to initialize FTL bdev '%s'\n", opts->opts.name);
			bdev_ftl_defer_free(opts);
			break;
		}
		return;
	}

	spdk_bdev_module_examine_done(&g_ftl_if);
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ftl", SPDK_LOG_BDEV_FTL)
