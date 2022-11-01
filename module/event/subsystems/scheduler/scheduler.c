/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/scheduler.h"

#include "spdk_internal/event.h"
#include "spdk_internal/init.h"

static void
scheduler_subsystem_init(void)
{
	int rc = 0;

	/* Set the defaults */
	if (spdk_scheduler_get() == NULL) {
		rc = spdk_scheduler_set("static");
	}

	spdk_subsystem_init_next(rc);
}

static void
scheduler_subsystem_fini(void)
{
	spdk_scheduler_set_period(0);
	spdk_scheduler_set(NULL);

	spdk_subsystem_fini_next();
}

static void
scheduler_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_scheduler *scheduler;
	uint64_t scheduler_period;

	assert(w != NULL);

	scheduler = spdk_scheduler_get();
	if (scheduler == NULL) {
		SPDK_ERRLOG("Unable to get scheduler info\n");
		return;
	}

	scheduler_period = spdk_scheduler_get_period();

	spdk_json_write_array_begin(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "framework_set_scheduler");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", scheduler->name);
	if (scheduler_period != 0) {
		spdk_json_write_named_uint32(w, "period", scheduler_period);
	}
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_array_end(w);
}

static struct spdk_subsystem g_spdk_subsystem_scheduler = {
	.name = "scheduler",
	.init = scheduler_subsystem_init,
	.fini = scheduler_subsystem_fini,
	.write_config_json = scheduler_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_scheduler);
