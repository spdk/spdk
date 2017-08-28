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
#include "spdk/log.h"
#include "spdk/nvme.h"

struct spdk_nvmf_tgt *g_tgt = NULL;

static struct spdk_poller *g_acceptor_poller = NULL;

static TAILQ_HEAD(, nvmf_tgt_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);
static bool g_subsystems_shutdown;

static void
shutdown_complete(void)
{
	spdk_app_stop(0);
}

static void
subsystem_delete_event(void *arg1, void *arg2)
{
	struct nvmf_tgt_subsystem *app_subsys = arg1;
	struct spdk_nvmf_subsystem *subsystem = app_subsys->subsystem;

	TAILQ_REMOVE(&g_subsystems, app_subsys, tailq);
	free(app_subsys);

	spdk_nvmf_delete_subsystem(subsystem);

	if (g_subsystems_shutdown && TAILQ_EMPTY(&g_subsystems)) {
		spdk_nvmf_tgt_destroy(g_tgt);
		/* Finished shutting down all subsystems - continue the shutdown process. */
		shutdown_complete();
	}
}

static void
nvmf_tgt_delete_subsystem(struct nvmf_tgt_subsystem *app_subsys)
{
	struct spdk_event *event;

	/*
	 * Unregister the poller - this starts a chain of events that will eventually free
	 * the subsystem's memory.
	 */
	event = spdk_event_allocate(spdk_env_get_current_core(), subsystem_delete_event,
				    app_subsys, NULL);
	spdk_poller_unregister(&app_subsys->poller, event);
}

static void
shutdown_subsystems(void)
{
	struct nvmf_tgt_subsystem *app_subsys, *tmp;

	g_subsystems_shutdown = true;
	TAILQ_FOREACH_SAFE(app_subsys, &g_subsystems, tailq, tmp) {
		nvmf_tgt_delete_subsystem(app_subsys);
	}
}

static void
acceptor_poller_unregistered_event(void *arg1, void *arg2)
{
	shutdown_subsystems();
}

static void
spdk_nvmf_shutdown_cb(void)
{
	struct spdk_event *event;

	fprintf(stdout, "\n=========================\n");
	fprintf(stdout, "   NVMF shutdown signal\n");
	fprintf(stdout, "=========================\n");

	event = spdk_event_allocate(spdk_env_get_current_core(), acceptor_poller_unregistered_event,
				    NULL, NULL);
	spdk_poller_unregister(&g_acceptor_poller, event);
}

static void
subsystem_poll(void *arg)
{
	struct nvmf_tgt_subsystem *app_subsys = arg;

	spdk_nvmf_subsystem_poll(app_subsys->subsystem);
}

static void
_nvmf_tgt_start_subsystem(void *arg1, void *arg2)
{
	struct nvmf_tgt_subsystem *app_subsys = arg1;
	struct spdk_nvmf_subsystem *subsystem = app_subsys->subsystem;
	int lcore = spdk_env_get_current_core();

	spdk_nvmf_subsystem_start(subsystem);

	spdk_poller_register(&app_subsys->poller, subsystem_poll, app_subsys, lcore, 0);
}

void
nvmf_tgt_start_subsystem(struct nvmf_tgt_subsystem *app_subsys)
{
	struct spdk_event *event;

	event = spdk_event_allocate(app_subsys->lcore, _nvmf_tgt_start_subsystem,
				    app_subsys, NULL);
	spdk_event_call(event);
}

struct nvmf_tgt_subsystem *
nvmf_tgt_create_subsystem(const char *name, enum spdk_nvmf_subtype subtype, uint32_t num_ns,
			  uint32_t lcore)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;

	if (spdk_nvmf_tgt_find_subsystem(g_tgt, name)) {
		SPDK_ERRLOG("Subsystem already exist\n");
		return NULL;
	}

	app_subsys = calloc(1, sizeof(*app_subsys));
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem allocation failed\n");
		return NULL;
	}

	subsystem = spdk_nvmf_create_subsystem(g_tgt, name, subtype, num_ns);
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

/* This function can only be used before the pollers are started. */
static void
nvmf_tgt_delete_subsystems(void)
{
	struct nvmf_tgt_subsystem *app_subsys, *tmp;
	struct spdk_nvmf_subsystem *subsystem;

	TAILQ_FOREACH_SAFE(app_subsys, &g_subsystems, tailq, tmp) {
		TAILQ_REMOVE(&g_subsystems, app_subsys, tailq);
		subsystem = app_subsys->subsystem;
		spdk_nvmf_delete_subsystem(subsystem);
		free(app_subsys);
	}
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
acceptor_poll(void *arg)
{
	struct spdk_nvmf_tgt *tgt = arg;

	spdk_nvmf_tgt_accept(tgt);
}

static void
spdk_nvmf_startup(void *arg1, void *arg2)
{
	int rc;

	rc = spdk_nvmf_parse_conf();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
		goto initialize_error;
	}

	if (((1ULL << g_spdk_nvmf_tgt_conf.acceptor_lcore) & spdk_app_get_core_mask()) == 0) {
		SPDK_ERRLOG("Invalid AcceptorCore setting\n");
		goto initialize_error;
	}

	spdk_poller_register(&g_acceptor_poller, acceptor_poll, g_tgt,
			     g_spdk_nvmf_tgt_conf.acceptor_lcore,
			     g_spdk_nvmf_tgt_conf.acceptor_poll_rate);

	SPDK_NOTICELOG("Acceptor running on core %u on socket %u\n", g_spdk_nvmf_tgt_conf.acceptor_lcore,
		       spdk_env_get_socket_id(g_spdk_nvmf_tgt_conf.acceptor_lcore));

	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}

	return;

initialize_error:
	nvmf_tgt_delete_subsystems();
	spdk_app_stop(rc);
}

int
spdk_nvmf_tgt_start(struct spdk_app_opts *opts)
{
	int rc;

	opts->shutdown_cb = spdk_nvmf_shutdown_cb;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(opts, spdk_nvmf_startup, NULL, NULL);

	spdk_app_fini();

	return rc;
}
