/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/ftl.h"
#include "spdk/log.h"

#include "bdev_ftl.h"

struct ftl_bdev {
	struct spdk_bdev	bdev;
	struct spdk_ftl_dev	*dev;
	ftl_bdev_init_fn	init_cb;
	void			*init_arg;
	int			rc;
	struct spdk_bdev_desc	*base_bdev_desc;
	struct spdk_bdev_desc	*cache_bdev_desc;
};

struct bdev_ftl_action {
	struct spdk_bdev_desc	*ftl_bdev_desc;
	struct ftl_bdev		*ftl_bdev_dev;
	spdk_ftl_fn		cb_fn;
	void			*cb_arg;
	int			rc;
	size_t			ctx_size;
	char			ctx[0];
};

struct ftl_deferred_init {
	struct spdk_ftl_conf		conf;

	LIST_ENTRY(ftl_deferred_init)	entry;
};

static LIST_HEAD(, ftl_deferred_init)	g_deferred_init = LIST_HEAD_INITIALIZER(g_deferred_init);

static int bdev_ftl_initialize(void);
static void bdev_ftl_finish(void);
static void bdev_ftl_examine(struct spdk_bdev *bdev);

static void bdev_ftl_action_finish_cb(void *cb_arg, int status);
static struct bdev_ftl_action *bdev_ftl_action_start(const char *bdev_name,
		size_t ctx_size, spdk_ftl_fn cb_fn, void *cb_arg);
static void bdev_ftl_action_finish(struct bdev_ftl_action *action);
static void *bdev_ftl_action_ctx(struct bdev_ftl_action *action, size_t size);

static int
bdev_ftl_get_ctx_size(void)
{
	return spdk_ftl_io_size();
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
bdev_ftl_free(struct ftl_bdev *ftl_bdev)
{
	spdk_bdev_close(ftl_bdev->base_bdev_desc);
	spdk_bdev_close(ftl_bdev->cache_bdev_desc);
	free(ftl_bdev->bdev.name);
	free(ftl_bdev);
}

static void
bdev_ftl_dev_free_cb(void *ctx, int status)
{
	struct ftl_bdev *ftl_bdev = ctx;

	spdk_bdev_destruct_done(&ftl_bdev->bdev, status);
	bdev_ftl_free(ftl_bdev);
}

static int
bdev_ftl_destruct(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;

	spdk_ftl_dev_free(ftl_bdev->dev, bdev_ftl_dev_free_cb, ftl_bdev);

	/* return 1 to indicate that the destruction is asynchronous */
	return 1;
}

static void
bdev_ftl_cb(void *arg, int rc)
{
	struct spdk_bdev_io *bdev_io = arg;
	enum spdk_bdev_io_status status;

	switch (rc) {
	case 0:
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -EAGAIN:
	case -ENOMEM:
		status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	default:
		status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	spdk_bdev_io_complete(bdev_io, status);
}

static void
bdev_ftl_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	struct ftl_bdev *ftl_bdev;
	int rc;

	ftl_bdev = bdev_io->bdev->ctxt;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_ftl_readv(ftl_bdev->dev, (struct ftl_io *)bdev_io->driver_ctx,
			    ch,
			    bdev_io->u.bdev.offset_blocks,
			    bdev_io->u.bdev.num_blocks,
			    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_ftl_cb, bdev_io);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_cb(bdev_io, rc);
	}
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
		return spdk_ftl_writev(ftl_bdev->dev, (struct ftl_io *)bdev_io->driver_ctx,
				       ch, bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.iovs,
				       bdev_io->u.bdev.iovcnt, bdev_ftl_cb, bdev_io);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return spdk_ftl_unmap(ftl_bdev->dev, (struct ftl_io *)bdev_io->driver_ctx,
				      ch, bdev_io->u.bdev.offset_blocks,
				      bdev_io->u.bdev.num_blocks, bdev_ftl_cb, bdev_io);
	case SPDK_BDEV_IO_TYPE_FLUSH:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static void
bdev_ftl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = _bdev_ftl_submit_request(ch, bdev_io);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_cb(bdev_io, rc);
	}
}

static bool
bdev_ftl_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_ftl_get_io_channel(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;

	return spdk_ftl_get_io_channel(ftl_bdev->dev);
}

static void
bdev_ftl_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct ftl_bdev *ftl_bdev = bdev->ctxt;
	struct spdk_ftl_conf conf;

	spdk_ftl_dev_get_conf(ftl_bdev->dev, &conf, sizeof(conf));

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ftl_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", ftl_bdev->bdev.name);

	spdk_json_write_named_uint64(w, "overprovisioning", conf.overprovisioning);
	spdk_json_write_named_uint64(w, "l2p_dram_limit", conf.l2p_dram_limit);

	if (conf.core_mask) {
		spdk_json_write_named_string(w, "core_mask", conf.core_mask);
	}

	spdk_json_write_named_uuid(w, "uuid", &conf.uuid);

	spdk_json_write_named_bool(w, "fast_shutdown", conf.fast_shutdown);

	spdk_json_write_named_string(w, "base_bdev", conf.base_bdev);

	if (conf.cache_bdev) {
		spdk_json_write_named_string(w, "cache", conf.cache_bdev);
	}

	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static int
bdev_ftl_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct ftl_bdev *ftl_bdev = ctx;
	struct spdk_ftl_attrs attrs;
	struct spdk_ftl_conf conf;

	spdk_ftl_dev_get_attrs(ftl_bdev->dev, &attrs, sizeof(attrs));
	spdk_ftl_dev_get_conf(ftl_bdev->dev, &conf, sizeof(conf));

	spdk_json_write_named_object_begin(w, "ftl");

	spdk_json_write_named_string(w, "base_bdev", conf.base_bdev);

	if (conf.cache_bdev) {
		spdk_json_write_named_string(w, "cache", conf.cache_bdev);
	}

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

static void
bdev_ftl_create_err_complete(struct ftl_bdev *ftl_bdev)
{
	ftl_bdev_init_fn init_cb = ftl_bdev->init_cb;
	void *init_arg = ftl_bdev->init_arg;
	int rc = ftl_bdev->rc;

	bdev_ftl_free(ftl_bdev);

	assert(rc);
	init_cb(NULL, init_arg, rc);
}

static void
bdev_ftl_create_err_cleanup_cb(void *ctx, int status)
{
	struct ftl_bdev *ftl_bdev = ctx;

	if (status) {
		SPDK_ERRLOG("Fatal ERROR of FTL cleanup, name %s\n", ftl_bdev->bdev.name);
	}

	bdev_ftl_create_err_complete(ftl_bdev);
}

static void
bdev_ftl_create_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_bdev		*ftl_bdev = ctx;
	struct ftl_bdev_info	info = {};
	struct spdk_ftl_attrs	attrs;
	struct spdk_ftl_conf	conf;
	ftl_bdev_init_fn	init_cb = ftl_bdev->init_cb;
	void			*init_arg = ftl_bdev->init_arg;

	if (status) {
		SPDK_ERRLOG("Failed to create FTL device (%d)\n", status);
		ftl_bdev->rc = status;
		goto error;
	}

	spdk_ftl_dev_get_attrs(dev, &attrs, sizeof(attrs));
	spdk_ftl_dev_get_conf(dev, &conf, sizeof(conf));

	ftl_bdev->dev = dev;
	ftl_bdev->bdev.product_name = "FTL disk";
	ftl_bdev->bdev.write_cache = 0;
	ftl_bdev->bdev.blocklen = attrs.block_size;
	ftl_bdev->bdev.blockcnt = attrs.num_blocks;
	ftl_bdev->bdev.uuid = conf.uuid;
	ftl_bdev->bdev.optimal_io_boundary = attrs.optimum_io_size;
	ftl_bdev->bdev.split_on_optimal_io_boundary = true;

	SPDK_DEBUGLOG(bdev_ftl, "Creating bdev %s:\n", ftl_bdev->bdev.name);
	SPDK_DEBUGLOG(bdev_ftl, "\tblock_len:\t%zu\n", attrs.block_size);
	SPDK_DEBUGLOG(bdev_ftl, "\tnum_blocks:\t%"PRIu64"\n", attrs.num_blocks);

	ftl_bdev->bdev.ctxt = ftl_bdev;
	ftl_bdev->bdev.fn_table = &ftl_fn_table;
	ftl_bdev->bdev.module = &g_ftl_if;

	status = spdk_bdev_register(&ftl_bdev->bdev);
	if (status) {
		ftl_bdev->rc = status;
		goto error;
	}

	info.name = ftl_bdev->bdev.name;
	info.uuid = ftl_bdev->bdev.uuid;

	init_cb(&info, init_arg, 0);
	return;

error:
	if (ftl_bdev->dev) {
		/* Cleanup all FTL */
		spdk_ftl_dev_set_fast_shutdown(ftl_bdev->dev, false);

		/* FTL was created, but we have got an error, so we need to delete it */
		spdk_ftl_dev_free(dev, bdev_ftl_create_err_cleanup_cb, ftl_bdev);
	} else {
		bdev_ftl_create_err_complete(ftl_bdev);
	}
}

static void
bdev_ftl_defer_free(struct ftl_deferred_init *init)
{
	spdk_ftl_conf_deinit(&init->conf);
	free(init);
}

int
bdev_ftl_defer_init(const struct spdk_ftl_conf *conf)
{
	struct ftl_deferred_init *init;
	int rc;

	init = calloc(1, sizeof(*init));
	if (!init) {
		return -ENOMEM;
	}

	rc = spdk_ftl_conf_copy(&init->conf, conf);
	if (rc) {
		free(init);
		return -ENOMEM;
	}

	LIST_INSERT_HEAD(&g_deferred_init, init, entry);

	return 0;
}

static void
bdev_ftl_create_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_ftl_create_bdev(const struct spdk_ftl_conf *conf, ftl_bdev_init_fn cb, void *cb_arg)
{
	struct ftl_bdev *ftl_bdev;
	struct spdk_bdev_desc *base_bdev_desc, *cache_bdev_desc;
	int rc;

	rc = spdk_bdev_open_ext(conf->base_bdev, false, bdev_ftl_create_bdev_event_cb, NULL,
				&base_bdev_desc);
	if (rc) {
		return rc;
	}
	rc = spdk_bdev_open_ext(conf->cache_bdev, false, bdev_ftl_create_bdev_event_cb, NULL,
				&cache_bdev_desc);
	if (rc) {
		spdk_bdev_close(base_bdev_desc);
		return rc;
	}

	ftl_bdev = calloc(1, sizeof(*ftl_bdev));
	if (!ftl_bdev) {
		SPDK_ERRLOG("Could not allocate ftl_bdev\n");
		spdk_bdev_close(base_bdev_desc);
		spdk_bdev_close(cache_bdev_desc);
		return -ENOMEM;
	}

	ftl_bdev->base_bdev_desc = base_bdev_desc;
	ftl_bdev->cache_bdev_desc = cache_bdev_desc;

	ftl_bdev->bdev.name = strdup(conf->name);
	if (!ftl_bdev->bdev.name) {
		rc = -ENOMEM;
		goto error;
	}

	ftl_bdev->init_cb = cb;
	ftl_bdev->init_arg = cb_arg;

	rc = spdk_ftl_dev_init(conf, bdev_ftl_create_cb, ftl_bdev);
	if (rc) {
		SPDK_ERRLOG("Could not create FTL device\n");
		goto error;
	}

	return 0;

error:
	bdev_ftl_free(ftl_bdev);
	return rc;
}

static int
bdev_ftl_initialize(void)
{
	return spdk_ftl_init();
}

static void
bdev_ftl_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

void
bdev_ftl_delete_bdev(const char *name, bool fast_shutdown, spdk_bdev_unregister_cb cb_fn,
		     void *cb_arg)
{
	struct spdk_bdev_desc	*ftl_bdev_desc;
	struct spdk_bdev *bdev;
	struct ftl_bdev *ftl;
	int rc;

	rc = spdk_bdev_open_ext(name, false, bdev_ftl_event_cb, NULL, &ftl_bdev_desc);

	if (rc) {
		goto not_found;
	}

	bdev = spdk_bdev_desc_get_bdev(ftl_bdev_desc);

	if (bdev->module != &g_ftl_if) {
		goto bdev_opened;
	}

	ftl = bdev->ctxt;
	assert(ftl);
	spdk_ftl_dev_set_fast_shutdown(ftl->dev, fast_shutdown);
	spdk_bdev_close(ftl_bdev_desc);

	rc = spdk_bdev_unregister_by_name(name, &g_ftl_if, cb_fn, cb_arg);
	if (rc) {
		cb_fn(cb_arg, rc);
	}

	return;
bdev_opened:
	spdk_bdev_close(ftl_bdev_desc);
not_found:
	cb_fn(cb_arg, -ENODEV);
}

void
bdev_ftl_unmap(const char *name, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb_fn,
	       void *cb_arg)
{
	struct bdev_ftl_action *action;

	action = bdev_ftl_action_start(name, 0, cb_fn, cb_arg);
	if (!action) {
		return;
	}

	/* It's ok to pass NULL as IO channel - FTL will detect this and use it's internal IO channel for management operations */
	action->rc = spdk_ftl_unmap(action->ftl_bdev_dev->dev, NULL, NULL, lba, num_blocks,
				    bdev_ftl_action_finish_cb, action);
	if (action->rc) {
		bdev_ftl_action_finish(action);
	}
}

static void
bdev_ftl_get_stats_cb(struct ftl_stats *stats, void *cb_arg)
{
	struct bdev_ftl_action *action = cb_arg;

	bdev_ftl_action_finish(action);
}

void
bdev_ftl_get_stats(const char *name, spdk_ftl_fn cb, struct rpc_ftl_stats_ctx *ftl_stats_ctx)
{
	struct bdev_ftl_action *action;

	action = bdev_ftl_action_start(name, 0, cb, ftl_stats_ctx);
	if (!action) {
		return;
	}
	ftl_stats_ctx->ftl_bdev_desc = action->ftl_bdev_desc;
	action->rc = spdk_ftl_get_stats(action->ftl_bdev_dev->dev, &ftl_stats_ctx->ftl_stats,
					bdev_ftl_get_stats_cb, action);
	if (action->rc) {
		bdev_ftl_action_finish(action);
	}
}

void
bdev_ftl_get_properties(const char *name, spdk_ftl_fn cb_fn, struct spdk_jsonrpc_request *request)
{
	struct bdev_ftl_action *action;

	action = bdev_ftl_action_start(name, 0, cb_fn, request);
	if (!action) {
		return;
	}

	action->rc = spdk_ftl_get_properties(action->ftl_bdev_dev->dev, request,
					     bdev_ftl_action_finish_cb, action);
	if (action->rc) {
		bdev_ftl_action_finish(action);
	}
}

struct bdev_ftl_set_property_args {
	char *property;
	char *value;
};

static void
bdev_ftl_set_property_cb(void *cb_arg, int status)
{
	struct bdev_ftl_action *action = cb_arg;
	struct bdev_ftl_set_property_args *args = bdev_ftl_action_ctx(action, sizeof(*args));

	free(args->property);
	free(args->value);
	args->property = NULL;
	args->value = NULL;
	bdev_ftl_action_finish_cb(cb_arg, status);
}

void
bdev_ftl_set_property(const char *name, const char *property, const char *value,
		      spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct bdev_ftl_action *action;
	struct bdev_ftl_set_property_args *args;

	action = bdev_ftl_action_start(name, sizeof(*args), cb_fn, cb_arg);
	if (!action) {
		return;
	}

	args = bdev_ftl_action_ctx(action, sizeof(*args));
	args->property = strdup(property);
	args->value = strdup(value);

	if (!args->property || !args->value) {
		free(args->property);
		free(args->value);
		action->rc = -ENOMEM;
		bdev_ftl_action_finish(action);
		return;
	}

	action->rc = spdk_ftl_set_property(action->ftl_bdev_dev->dev,
					   args->property, args->value, strlen(args->value) + 1,
					   bdev_ftl_set_property_cb, action);
	if (action->rc) {
		bdev_ftl_action_finish(action);
	}
}

static void
bdev_ftl_finish(void)
{
	spdk_ftl_fini();
}

static void
bdev_ftl_create_deferred_cb(const struct ftl_bdev_info *info, void *ctx, int status)
{
	struct ftl_deferred_init *opts = ctx;

	if (status) {
		SPDK_ERRLOG("Failed to initialize FTL bdev '%s'\n", opts->conf.name);
	}

	bdev_ftl_defer_free(opts);

	spdk_bdev_module_examine_done(&g_ftl_if);
}

static void
bdev_ftl_examine(struct spdk_bdev *bdev)
{
	struct ftl_deferred_init *opts;
	int rc;

	LIST_FOREACH(opts, &g_deferred_init, entry) {
		/* spdk_bdev_module_examine_done will be called by bdev_ftl_create_deferred_cb */
		rc = bdev_ftl_create_bdev(&opts->conf, bdev_ftl_create_deferred_cb, opts);
		if (rc == -ENODEV) {
			continue;
		}

		LIST_REMOVE(opts, entry);

		if (rc) {
			bdev_ftl_create_deferred_cb(NULL, opts, rc);
		}
		return;
	}

	spdk_bdev_module_examine_done(&g_ftl_if);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_ftl)

/*
 * Generic function to execute an action on the FTL bdev
 */
static void
bdev_ftl_action_finish(struct bdev_ftl_action *action)
{
	action->cb_fn(action->cb_arg, action->rc);
	if (action->ftl_bdev_desc) {
		spdk_bdev_close(action->ftl_bdev_desc);
	}
	free(action);
}

static struct bdev_ftl_action *
bdev_ftl_action_start(const char *bdev_name, size_t ctx_size, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct bdev_ftl_action *action = calloc(1, sizeof(*action) + ctx_size);

	if (NULL == action) {
		cb_fn(cb_arg, -ENOMEM);
		return NULL;
	}
	action->cb_arg = cb_arg;
	action->cb_fn = cb_fn;
	action->ctx_size = ctx_size;

	action->rc = spdk_bdev_open_ext(bdev_name, false, bdev_ftl_event_cb, NULL, &action->ftl_bdev_desc);
	if (action->rc) {
		goto error;
	}

	bdev = spdk_bdev_desc_get_bdev(action->ftl_bdev_desc);
	if (bdev->module != &g_ftl_if) {
		action->rc = -ENODEV;
		goto error;
	}

	action->ftl_bdev_dev = bdev->ctxt;
	assert(action->ftl_bdev_dev);

	return action;
error:
	bdev_ftl_action_finish(action);
	return NULL;
}

static void
bdev_ftl_action_finish_cb(void *cb_arg, int status)
{
	struct bdev_ftl_action *action = cb_arg;

	action->rc = status;
	bdev_ftl_action_finish(action);
}

static void *
bdev_ftl_action_ctx(struct bdev_ftl_action *action, size_t size)
{
	assert(action->ctx_size);
	assert(size == action->ctx_size);
	return action->ctx;
}
