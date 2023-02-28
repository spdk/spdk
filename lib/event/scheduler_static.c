/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/scheduler.h"

#include "spdk_internal/event.h"

static int
init_static(void)
{
	/* There is no scheduling performed by static scheduler,
	 * do not set the scheduling period. */
	spdk_scheduler_set_period(0);
	return 0;
}

static void
deinit_static(void)
{
}

static void
balance_static(struct spdk_scheduler_core_info *cores, uint32_t core_count)
{
}

static int
set_opts_static(const struct spdk_json_val *opts)
{
	return 0;
}

static void
get_opts_static(struct spdk_json_write_ctx *ctx)
{
}

static struct spdk_scheduler scheduler = {
	.name = "static",
	.init = init_static,
	.deinit = deinit_static,
	.balance = balance_static,
	.set_opts = set_opts_static,
	.get_opts = get_opts_static,
};
SPDK_SCHEDULER_REGISTER(scheduler);
