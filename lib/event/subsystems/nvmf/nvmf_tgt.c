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

#include "event_nvmf.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/util.h"

enum nvmf_tgt_state {
	NVMF_TGT_INIT_NONE = 0,
	NVMF_TGT_INIT_PARSE_CONFIG,
	NVMF_TGT_INIT_CREATE_POLL_GROUPS,
	NVMF_TGT_INIT_START_SUBSYSTEMS,
	NVMF_TGT_INIT_START_ACCEPTOR,
	NVMF_TGT_RUNNING,
	NVMF_TGT_FINI_STOP_ACCEPTOR,
	NVMF_TGT_FINI_DESTROY_POLL_GROUPS,
	NVMF_TGT_FINI_STOP_SUBSYSTEMS,
	NVMF_TGT_FINI_FREE_RESOURCES,
	NVMF_TGT_STOPPED,
	NVMF_TGT_ERROR,
};

struct nvmf_tgt_poll_group {
	struct spdk_nvmf_poll_group *group;
};

struct spdk_nvmf_tgt *g_spdk_nvmf_tgt = NULL;

static enum nvmf_tgt_state g_tgt_state;

static uint32_t g_tgt_core; /* Round-robin tracking of cores for qpair assignment */

static struct nvmf_tgt_poll_group *g_poll_groups = NULL;
static size_t g_num_poll_groups = 0;
static size_t g_active_poll_groups = 0;

static struct spdk_poller *g_acceptor_poller = NULL;

static void nvmf_tgt_advance_state(void);

static void
_spdk_nvmf_shutdown_cb(void *arg1, void *arg2)
{
	/* Still in initialization state, defer shutdown operation */
	if (g_tgt_state < NVMF_TGT_RUNNING) {
		spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(),
						    _spdk_nvmf_shutdown_cb, NULL, NULL));
		return;
	} else if (g_tgt_state > NVMF_TGT_RUNNING) {
		/* Already in Shutdown status, ignore the signal */
		return;
	}

	g_tgt_state = NVMF_TGT_FINI_STOP_ACCEPTOR;
	nvmf_tgt_advance_state();
}

static void
spdk_nvmf_subsystem_fini(void)
{
	/* Always let the first core to handle the case */
	if (spdk_env_get_current_core() != spdk_env_get_first_core()) {
		spdk_event_call(spdk_event_allocate(spdk_env_get_first_core(),
						    _spdk_nvmf_shutdown_cb, NULL, NULL));
	} else {
		_spdk_nvmf_shutdown_cb(NULL, NULL);
	}
}

static void
nvmf_tgt_poll_group_add(void *arg1, void *arg2)
{
	struct spdk_nvmf_qpair *qpair = arg1;
	struct nvmf_tgt_poll_group *pg = arg2;

	spdk_nvmf_poll_group_add(pg->group, qpair);
}

static void
new_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_event *event;
	struct nvmf_tgt_poll_group *pg;
	uint32_t core;

	core = g_tgt_core;
	g_tgt_core = spdk_env_get_next_core(core);
	if (g_tgt_core == UINT32_MAX) {
		g_tgt_core = spdk_env_get_first_core();
	}

	pg = &g_poll_groups[core];
	assert(pg != NULL);

	event = spdk_event_allocate(core, nvmf_tgt_poll_group_add, qpair, pg);
	spdk_event_call(event);
}

static int
acceptor_poll(void *arg)
{
	struct spdk_nvmf_tgt *tgt = arg;

	spdk_nvmf_tgt_accept(tgt, new_qpair);

	return -1;
}

static void
nvmf_tgt_destroy_poll_group_done(void *ctx)
{
	g_tgt_state = NVMF_TGT_FINI_FREE_RESOURCES;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_destroy_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg;

	pg = &g_poll_groups[spdk_env_get_current_core()];
	assert(pg != NULL);

	spdk_nvmf_poll_group_destroy(pg->group);
	pg->group = NULL;

	assert(g_active_poll_groups > 0);
	g_active_poll_groups--;
}

static void
nvmf_tgt_create_poll_group_done(void *ctx)
{
	g_tgt_state = NVMF_TGT_INIT_START_SUBSYSTEMS;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_create_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg;

	pg = &g_poll_groups[spdk_env_get_current_core()];
	assert(pg != NULL);

	pg->group = spdk_nvmf_poll_group_create(g_spdk_nvmf_tgt);
	if (pg->group == NULL) {
		SPDK_ERRLOG("Failed to create poll group for core %u\n", spdk_env_get_current_core());
	}

	g_active_poll_groups++;
}

static void
nvmf_tgt_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);

	if (subsystem) {
		spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_started, NULL);
		return;
	}

	g_tgt_state = NVMF_TGT_INIT_START_ACCEPTOR;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);

	if (subsystem) {
		spdk_nvmf_subsystem_stop(subsystem, nvmf_tgt_subsystem_stopped, NULL);
		return;
	}

	g_tgt_state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
	nvmf_tgt_advance_state();
}

static void
nvmf_tgt_advance_state(void)
{
	enum nvmf_tgt_state prev_state;
	int rc = -1;

	do {
		prev_state = g_tgt_state;

		switch (g_tgt_state) {
		case NVMF_TGT_INIT_NONE: {
			g_tgt_state = NVMF_TGT_INIT_PARSE_CONFIG;

			/* Find the maximum core number */
			g_num_poll_groups = spdk_env_get_last_core() + 1;
			assert(g_num_poll_groups > 0);

			g_poll_groups = calloc(g_num_poll_groups, sizeof(*g_poll_groups));
			if (g_poll_groups == NULL) {
				g_tgt_state = NVMF_TGT_ERROR;
				rc = -ENOMEM;
				break;
			}

			g_tgt_core = spdk_env_get_first_core();
			break;
		}
		case NVMF_TGT_INIT_PARSE_CONFIG:
			rc = spdk_nvmf_parse_conf();
			if (rc < 0) {
				SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
				g_tgt_state = NVMF_TGT_ERROR;
				rc = -EINVAL;
				break;
			}
			g_tgt_state = NVMF_TGT_INIT_CREATE_POLL_GROUPS;
			break;
		case NVMF_TGT_INIT_CREATE_POLL_GROUPS:
			/* Send a message to each thread and create a poll group */
			spdk_for_each_thread(nvmf_tgt_create_poll_group,
					     NULL,
					     nvmf_tgt_create_poll_group_done);
			break;
		case NVMF_TGT_INIT_START_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);

			if (subsystem) {
				spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_started, NULL);
			} else {
				g_tgt_state = NVMF_TGT_INIT_START_ACCEPTOR;
			}
			break;
		}
		case NVMF_TGT_INIT_START_ACCEPTOR:
			g_acceptor_poller = spdk_poller_register(acceptor_poll, g_spdk_nvmf_tgt,
					    g_spdk_nvmf_tgt_conf.acceptor_poll_rate);
			SPDK_INFOLOG(SPDK_LOG_NVMF, "Acceptor running\n");
			g_tgt_state = NVMF_TGT_RUNNING;
			break;
		case NVMF_TGT_RUNNING:
			spdk_subsystem_init_next(0);
			break;
		case NVMF_TGT_FINI_STOP_ACCEPTOR:
			spdk_poller_unregister(&g_acceptor_poller);
			g_tgt_state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
			break;
		case NVMF_TGT_FINI_STOP_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);

			if (subsystem) {
				spdk_nvmf_subsystem_stop(subsystem, nvmf_tgt_subsystem_stopped, NULL);
			} else {
				g_tgt_state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
			}
			break;
		}
		case NVMF_TGT_FINI_DESTROY_POLL_GROUPS:
			/* Send a message to each thread and destroy the poll group */
			spdk_for_each_thread(nvmf_tgt_destroy_poll_group,
					     NULL,
					     nvmf_tgt_destroy_poll_group_done);
			break;
		case NVMF_TGT_FINI_FREE_RESOURCES:
			spdk_nvmf_tgt_destroy(g_spdk_nvmf_tgt);
			g_tgt_state = NVMF_TGT_STOPPED;
			break;
		case NVMF_TGT_STOPPED:
			spdk_subsystem_fini_next();
			return;
		case NVMF_TGT_ERROR:
			spdk_subsystem_init_next(rc);
			return;
		}

	} while (g_tgt_state != prev_state);
}

static void
spdk_nvmf_subsystem_init(void)
{
	g_tgt_state = NVMF_TGT_INIT_NONE;
	nvmf_tgt_advance_state();
}

static struct spdk_subsystem g_spdk_subsystem_nvmf = {
	.name = "nvmf",
	.init = spdk_nvmf_subsystem_init,
	.fini = spdk_nvmf_subsystem_fini,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_nvmf)
SPDK_SUBSYSTEM_DEPEND(nvmf, bdev)
