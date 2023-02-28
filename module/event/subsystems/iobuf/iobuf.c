/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk_internal/init.h"

int iobuf_set_opts(struct spdk_iobuf_opts *opts);

static struct spdk_iobuf_opts g_opts;
static bool g_opts_set;

int
iobuf_set_opts(struct spdk_iobuf_opts *opts)
{
	int rc;

	rc = spdk_iobuf_set_opts(opts);
	if (rc != 0) {
		return rc;
	}

	g_opts = *opts;
	g_opts_set = true;

	return 0;
}

static void
iobuf_subsystem_initialize(void)
{
	int rc;

	if (g_opts_set) {
		/* We want to allow users to keep using bdev layer's spdk_bdev_opts to specify the
		 * sizes of the pools, but want to have iobuf_set_opts to take precedence over what
		 * was set by the spdk_bdev_opts.  So, reset the opts here in case bdev layer set
		 * them after iobuf_set_opts.
		 */
		rc = spdk_iobuf_set_opts(&g_opts);
		if (rc != 0) {
			/* This should never happen, we've already validated these options */
			assert(0);
			goto finish;
		}
	}

	rc = spdk_iobuf_initialize();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize iobuf\n");
	}
finish:
	spdk_subsystem_init_next(rc);
}

static void
iobuf_finish_cb(void *ctx)
{
	spdk_subsystem_fini_next();
}

static void
iobuf_subsystem_finish(void)
{
	spdk_iobuf_finish(iobuf_finish_cb, NULL);
}

static void
iobuf_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iobuf_opts opts;

	spdk_iobuf_get_opts(&opts);

	spdk_json_write_array_begin(w);
	/* Make sure we don't override the options from spdk_bdev_opts, unless iobuf_set_options
	 * has been executed
	 */
	if (g_opts_set) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "iobuf_set_options");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint64(w, "small_pool_count", opts.small_pool_count);
		spdk_json_write_named_uint64(w, "large_pool_count", opts.large_pool_count);
		spdk_json_write_named_uint32(w, "small_bufsize", opts.small_bufsize);
		spdk_json_write_named_uint32(w, "large_bufsize", opts.large_bufsize);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

static struct spdk_subsystem g_subsystem_iobuf = {
	.name = "iobuf",
	.init = iobuf_subsystem_initialize,
	.fini = iobuf_subsystem_finish,
	.write_config_json = iobuf_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_subsystem_iobuf);
