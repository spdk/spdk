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
	if (spdk_scheduler_get_period() == 0) {
		spdk_scheduler_set_period(SPDK_SEC_TO_USEC);
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
