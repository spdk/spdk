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

#include "nvmf_tgt.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/util.h"

struct nvmf_tgt_poll_group {
	struct spdk_nvmf_poll_group *group;
};

struct nvmf_tgt g_tgt = {};

static struct nvmf_tgt_poll_group *g_poll_groups = NULL;
static size_t g_num_poll_groups = 0;
static size_t g_active_poll_groups = 0;

static struct spdk_poller *g_acceptor_poller = NULL;

static void nvmf_tgt_advance_state(void *arg1, void *arg2);

static void
_spdk_nvmf_shutdown_cb(void *arg1, void *arg2)
{
	/* Still in initialization state, defer shutdown operation */
	if (g_tgt.state < NVMF_TGT_RUNNING) {
		spdk_event_call(spdk_event_allocate(spdk_env_get_current_core(),
						    _spdk_nvmf_shutdown_cb, NULL, NULL));
		return;
	} else if (g_tgt.state > NVMF_TGT_RUNNING) {
		/* Already in Shutdown status, ignore the signal */
		return;
	}

	g_tgt.state = NVMF_TGT_FINI_STOP_ACCEPTOR;
	nvmf_tgt_advance_state(NULL, NULL);
}

static void
spdk_nvmf_shutdown_cb(void)
{
	printf("\n=========================\n");
	printf("   NVMF shutdown signal\n");
	printf("=========================\n");

	/* Always let the first core to handle the case */
	if (spdk_env_get_current_core() != spdk_env_get_first_core()) {
		spdk_event_call(spdk_event_allocate(spdk_env_get_first_core(),
						    _spdk_nvmf_shutdown_cb, NULL, NULL));
	} else {
		_spdk_nvmf_shutdown_cb(NULL, NULL);
	}
}

struct spdk_nvmf_subsystem *
nvmf_tgt_create_subsystem(const char *name, enum spdk_nvmf_subtype subtype, uint32_t num_ns)
{
	struct spdk_nvmf_subsystem *subsystem;

	if (spdk_nvmf_tgt_find_subsystem(g_tgt.tgt, name)) {
		SPDK_ERRLOG("Subsystem already exist\n");
		return NULL;
	}

	subsystem = spdk_nvmf_subsystem_create(g_tgt.tgt, name, subtype, num_ns);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		return NULL;
	}

	SPDK_NOTICELOG("allocated subsystem %s\n", name);

	return subsystem;
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

	core = g_tgt.core;
	g_tgt.core = spdk_env_get_next_core(core);
	if (g_tgt.core == UINT32_MAX) {
		g_tgt.core = spdk_env_get_first_core();
	}

	pg = &g_poll_groups[core];
	assert(pg != NULL);

	event = spdk_event_allocate(core, nvmf_tgt_poll_group_add, qpair, pg);
	spdk_event_call(event);
}

static void
acceptor_poll(void *arg)
{
	struct spdk_nvmf_tgt *tgt = arg;

	spdk_nvmf_tgt_accept(tgt, new_qpair);
}

static void
nvmf_tgt_destroy_poll_group_done(void *ctx)
{
	g_tgt.state = NVMF_TGT_FINI_FREE_RESOURCES;
	nvmf_tgt_advance_state(NULL, NULL);
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
	g_tgt.state = NVMF_TGT_INIT_START_SUBSYSTEMS;
	nvmf_tgt_advance_state(NULL, NULL);
}

static void
nvmf_tgt_create_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg;

	pg = &g_poll_groups[spdk_env_get_current_core()];
	assert(pg != NULL);

	pg->group = spdk_nvmf_poll_group_create(g_tgt.tgt);
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

	g_tgt.state = NVMF_TGT_INIT_START_ACCEPTOR;
	nvmf_tgt_advance_state(NULL, NULL);
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

	g_tgt.state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
	nvmf_tgt_advance_state(NULL, NULL);
}

static void
nvmf_tgt_advance_state(void *arg1, void *arg2)
{
	enum nvmf_tgt_state prev_state;
	int rc = -1;

	do {
		prev_state = g_tgt.state;

		switch (g_tgt.state) {
		case NVMF_TGT_INIT_NONE: {
			g_tgt.state = NVMF_TGT_INIT_PARSE_CONFIG;

			/* Find the maximum core number */
			g_num_poll_groups = spdk_env_get_last_core() + 1;
			assert(g_num_poll_groups > 0);

			g_poll_groups = calloc(g_num_poll_groups, sizeof(*g_poll_groups));
			if (g_poll_groups == NULL) {
				g_tgt.state = NVMF_TGT_ERROR;
				rc = -ENOMEM;
				break;
			}

			g_tgt.core = spdk_env_get_first_core();
			break;
		}
		case NVMF_TGT_INIT_PARSE_CONFIG:
			rc = spdk_nvmf_parse_conf();
			if (rc < 0) {
				SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
				g_tgt.state = NVMF_TGT_ERROR;
				rc = -EINVAL;
				break;
			}
			g_tgt.state = NVMF_TGT_INIT_CREATE_POLL_GROUPS;
			break;
		case NVMF_TGT_INIT_CREATE_POLL_GROUPS:
			/* Send a message to each thread and create a poll group */
			spdk_for_each_thread(nvmf_tgt_create_poll_group,
					     NULL,
					     nvmf_tgt_create_poll_group_done);
			break;
		case NVMF_TGT_INIT_START_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_tgt.tgt);

			if (subsystem) {
				spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_started, NULL);
			} else {
				g_tgt.state = NVMF_TGT_INIT_START_ACCEPTOR;
			}
			break;
		}
		case NVMF_TGT_INIT_START_ACCEPTOR:
			g_acceptor_poller = spdk_poller_register(acceptor_poll, g_tgt.tgt,
					    g_spdk_nvmf_tgt_conf.acceptor_poll_rate);
			SPDK_NOTICELOG("Acceptor running\n");
			g_tgt.state = NVMF_TGT_RUNNING;
			break;
		case NVMF_TGT_RUNNING:
			if (getenv("MEMZONE_DUMP") != NULL) {
				spdk_memzone_dump(stdout);
				fflush(stdout);
			}
			break;
		case NVMF_TGT_FINI_STOP_ACCEPTOR:
			spdk_poller_unregister(&g_acceptor_poller);
			g_tgt.state = NVMF_TGT_FINI_STOP_SUBSYSTEMS;
			break;
		case NVMF_TGT_FINI_STOP_SUBSYSTEMS: {
			struct spdk_nvmf_subsystem *subsystem;

			subsystem = spdk_nvmf_subsystem_get_first(g_tgt.tgt);

			if (subsystem) {
				spdk_nvmf_subsystem_stop(subsystem, nvmf_tgt_subsystem_stopped, NULL);
			} else {
				g_tgt.state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
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
			spdk_nvmf_tgt_destroy(g_tgt.tgt);
			g_tgt.state = NVMF_TGT_STOPPED;
			break;
		case NVMF_TGT_STOPPED:
			spdk_app_stop(0);
			return;
		case NVMF_TGT_ERROR:
			spdk_app_stop(rc);
			return;
		}

	} while (g_tgt.state != prev_state);
}

int
spdk_nvmf_tgt_start(struct spdk_app_opts *opts)
{
	int rc;

	opts->shutdown_cb = spdk_nvmf_shutdown_cb;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(opts, nvmf_tgt_advance_state, NULL, NULL);

	spdk_app_fini();

	return rc;
}
