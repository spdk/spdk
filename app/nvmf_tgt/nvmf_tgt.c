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

static TAILQ_HEAD(, nvmf_tgt_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

static void nvmf_tgt_advance_state(void *arg1, void *arg2);

static void
nvmf_tgt_delete_subsystem(struct nvmf_tgt_subsystem *app_subsys)
{
	TAILQ_REMOVE(&g_subsystems, app_subsys, tailq);

	spdk_nvmf_delete_subsystem(app_subsys->subsystem);

	free(app_subsys);
}

static void
spdk_nvmf_shutdown_cb(void)
{
	fprintf(stdout, "\n=========================\n");
	fprintf(stdout, "   NVMF shutdown signal\n");
	fprintf(stdout, "=========================\n");

	g_tgt.state = NVMF_TGT_FINI_STOP_ACCEPTOR;
	nvmf_tgt_advance_state(NULL, NULL);
}

struct nvmf_tgt_subsystem *
nvmf_tgt_create_subsystem(const char *name, enum spdk_nvmf_subtype subtype, uint32_t num_ns,
			  uint32_t lcore)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;

	if (spdk_nvmf_tgt_find_subsystem(g_tgt.tgt, name)) {
		SPDK_ERRLOG("Subsystem already exist\n");
		return NULL;
	}

	app_subsys = calloc(1, sizeof(*app_subsys));
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem allocation failed\n");
		return NULL;
	}

	subsystem = spdk_nvmf_create_subsystem(g_tgt.tgt, name, subtype, num_ns);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		free(app_subsys);
		return NULL;
	}

	app_subsys->subsystem = subsystem;
	app_subsys->lcore = lcore;

	SPDK_NOTICELOG("allocated subsystem %s on lcore %u on socket %u\n", name, lcore,
		       spdk_env_get_socket_id(lcore));

	TAILQ_INSERT_TAIL(&g_subsystems, app_subsys, tailq);

	return app_subsys;
}

struct nvmf_tgt_subsystem *
nvmf_tgt_subsystem_first(void)
{
	return TAILQ_FIRST(&g_subsystems);
}

struct nvmf_tgt_subsystem *
nvmf_tgt_subsystem_next(struct nvmf_tgt_subsystem *subsystem)
{
	return TAILQ_NEXT(subsystem, tailq);
}

int
nvmf_tgt_shutdown_subsystem_by_nqn(const char *nqn)
{
	struct nvmf_tgt_subsystem *tgt_subsystem, *subsys_tmp;

	TAILQ_FOREACH_SAFE(tgt_subsystem, &g_subsystems, tailq, subsys_tmp) {
		if (strcmp(spdk_nvmf_subsystem_get_nqn(tgt_subsystem->subsystem), nqn) == 0) {
			nvmf_tgt_delete_subsystem(tgt_subsystem);
			return 0;
		}
	}
	return -1;
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
	g_tgt.state = NVMF_TGT_FINI_SHUTDOWN_SUBSYSTEMS;
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
	g_tgt.state = NVMF_TGT_INIT_START_ACCEPTOR;
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
nvmf_tgt_advance_state(void *arg1, void *arg2)
{
	enum nvmf_tgt_state prev_state;
	int rc = -1;

	do {
		prev_state = g_tgt.state;

		switch (g_tgt.state) {
		case NVMF_TGT_INIT_NONE: {
			uint32_t core;

			g_tgt.state = NVMF_TGT_INIT_PARSE_CONFIG;

			/* Find the maximum core number */
			SPDK_ENV_FOREACH_CORE(core) {
				g_num_poll_groups = spdk_max(g_num_poll_groups, core + 1);
			}
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
			g_tgt.state = NVMF_TGT_FINI_DESTROY_POLL_GROUPS;
			break;
		case NVMF_TGT_FINI_DESTROY_POLL_GROUPS:
			/* Send a message to each thread and destroy the poll group */
			spdk_for_each_thread(nvmf_tgt_destroy_poll_group,
					     NULL,
					     nvmf_tgt_destroy_poll_group_done);
			break;
		case NVMF_TGT_FINI_SHUTDOWN_SUBSYSTEMS: {
			struct nvmf_tgt_subsystem *app_subsys, *tmp;

			TAILQ_FOREACH_SAFE(app_subsys, &g_subsystems, tailq, tmp) {
				nvmf_tgt_delete_subsystem(app_subsys);
			}
			g_tgt.state = NVMF_TGT_FINI_FREE_RESOURCES;
			break;
		}
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
