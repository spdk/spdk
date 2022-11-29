/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <linux/ublk_cmd.h>
#include <liburing.h>

#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/ublk.h"
#include "spdk/thread.h"

struct ublk_tgt {

	spdk_ublk_fini_cb fini_cb_fn;
	void *fini_cb_arg;
};

static TAILQ_HEAD(, spdk_ublk_dev) g_ublk_bdevs = TAILQ_HEAD_INITIALIZER(g_ublk_bdevs);
static struct ublk_tgt g_ublk_tgt;

void
spdk_ublk_init(void)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());
}

int
spdk_ublk_fini(spdk_ublk_fini_cb cb_fn, void *cb_arg)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	g_ublk_tgt.fini_cb_fn = cb_fn;
	g_ublk_tgt.fini_cb_arg = cb_arg;
	g_ublk_tgt.fini_cb_fn(g_ublk_tgt.fini_cb_arg);

	return 0;
}

void
spdk_ublk_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);

	spdk_json_write_array_end(w);
}
