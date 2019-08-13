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

#include "conf.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/ftp.h"
#include "spdk_internal/event.h"
#include "spdk/queue.h"

enum ftp_tgt_state {
	FTP_TGT_INIT_NONE = 0,
	FTP_TGT_INIT_PARSE_CONFIG,
	FTP_TGT_INIT_CREATE_POLL_GROUPS,
	FTP_TGT_INIT_START_ACCEPTOR,
	FTP_TGT_RUNNING,
	FTP_TGT_FINI_DESTROY_POLL_GROUPS,
	FTP_TGT_FINI_STOP_ACCEPTOR,
	FTP_TGT_FINI_FREE_RESOURCES,
	FTP_TGT_STOPPED,
	FTP_TGT_ERROR,
};

struct ftp_tgt_poll_group {
	struct spdk_ftp_poll_group		*group;
	struct spdk_thread			*thread;
	TAILQ_ENTRY(ftp_tgt_poll_group)	link;
};

struct spdk_ftp_tgt_conf *g_spdk_ftp_tgt_conf = NULL;

static enum ftp_tgt_state g_ftp_tgt_state;
struct spdk_ftp_tgt *g_spdk_ftp_tgt;

static struct ftp_tgt_poll_group *g_next_poll_group = NULL;
static TAILQ_HEAD(, ftp_tgt_poll_group) g_poll_groups = TAILQ_HEAD_INITIALIZER(g_poll_groups);
static size_t g_num_poll_groups = 0;

static struct spdk_poller *g_acceptor_poller = NULL;

static void ftp_tgt_advance_state(void);
static struct spdk_ftp_poll_group *ftp_tgt_poll_group_create(struct spdk_ftp_tgt *tgt);
static void spdk_ftp_tgt_destroy(struct spdk_ftp_tgt *tgt, spdk_ftp_tgt_destroy_done_fn cb_fn,
				 void *cb_arg);


static void
_spdk_ftp_shutdown_cb(void *arg1)
{
	if (g_ftp_tgt_state > FTP_TGT_RUNNING) {
		return;
	}

	if (g_ftp_tgt_state < FTP_TGT_RUNNING) {
		spdk_thread_send_msg(spdk_get_thread(), _spdk_ftp_shutdown_cb, NULL);
		return;
	}

	if (g_ftp_tgt_state == FTP_TGT_RUNNING) {
		g_ftp_tgt_state = FTP_TGT_FINI_DESTROY_POLL_GROUPS;
		ftp_tgt_advance_state();
		return;
	}

	g_ftp_tgt_state = FTP_TGT_ERROR;
	ftp_tgt_advance_state();
}

static void
spdk_ftp_subsystem_init(void)
{
	g_ftp_tgt_state = FTP_TGT_INIT_NONE;
	ftp_tgt_advance_state();
}

static void
spdk_ftp_subsystem_fini(void)
{
	_spdk_ftp_shutdown_cb(NULL);
}


/* FTP_TGT_INIT_PARSE_CONFIG */

static void
ftp_tgt_parse_conf_done(int status)
{
	g_ftp_tgt_state = (status == 0) ? FTP_TGT_INIT_CREATE_POLL_GROUPS : FTP_TGT_ERROR;
	ftp_tgt_advance_state();
}


static void
ftp_tgt_parse_conf(void *ctx)
{
	if (spdk_ftp_parse_conf(ftp_tgt_parse_conf_done)) {
		SPDK_ERRLOG("spdk_ftp_parse_conf() failed\n");
		g_ftp_tgt_state = FTP_TGT_ERROR;
		ftp_tgt_advance_state();
	}
}

/* FTP_TGT_INIT_CREATE_POLL_GROUPS */

static struct spdk_ftp_poll_group *
ftp_tgt_poll_group_create(struct spdk_ftp_tgt *tgt)
{
	struct spdk_io_channel *ch;

	ch = spdk_get_io_channel(tgt);
	if (!ch) {
		SPDK_ERRLOG("Unable to get I/O channel for target\n");
		return NULL;
	}

	return spdk_io_channel_get_ctx(ch);
}


static void
ftp_tgt_create_poll_group(void *ctx)
{
	struct ftp_tgt_poll_group *pg;
	struct spdk_ftp_poll_group *fpg;

	pg = calloc(1, sizeof(*pg));
	if (!pg) {
		SPDK_ERRLOG("Not enough memory to allocate poll groups\n");
		spdk_app_stop(-ENOMEM);
		return;
	}

	pg->thread = spdk_get_thread();
	fpg = ftp_tgt_poll_group_create(g_spdk_ftp_tgt);
	if (fpg == NULL) {
		free(pg);
		SPDK_ERRLOG("ftp_tgt poll group create failed!\n");
		spdk_app_stop(-ENOMEM);	 /* todo check this close */
		return;
	}
	pg->group = fpg;
	TAILQ_INSERT_TAIL(&g_poll_groups, pg, link);
	g_num_poll_groups++;

	if (g_next_poll_group == NULL) {
		g_next_poll_group = pg;
	}
}


static void
ftp_tgt_create_poll_group_done(void *ctx)
{
	g_ftp_tgt_state = FTP_TGT_RUNNING;
	ftp_tgt_advance_state();
}

/* FTP_TGT_FINI_DESTROY_POLL_GROUPS */


static int
ftp_tgt_acceptor_poll(void *ctx)
{
	/* todo
	 * accepte the request and deal it
	 * then give it to ftp_server return back to client
	 * this method associated with ftp_tgt_poll_group
	 * we can just use the round-robin method to Schedule the request
	 */
	return -1;
}


/* FTP_TGT_FINI_DESTROY_POLL_GROUPS */

static void
ftp_tgt_destroy_poll_group(void *ctx)
{
	struct ftp_tgt_poll_group *pg, *tpg;
	struct spdk_thread *thread;

	thread = spdk_get_thread();

	TAILQ_FOREACH_SAFE(pg, &g_poll_groups, link, tpg) {
		if (pg->thread == thread) {
			TAILQ_REMOVE(&g_poll_groups, pg, link);
			spdk_ftp_poll_group_destroy(pg->group);
			free(pg);
			assert(g_num_poll_groups > 0);
			g_num_poll_groups--;

			return;
		}
	}

}

static void
ftp_tgt_destroy_poll_group_done(void *ctx)
{
	g_ftp_tgt_state = FTP_TGT_FINI_FREE_RESOURCES;
	ftp_tgt_advance_state();
}

/* FTP_TGT_FINI_FREE_RESOURCES */

static void
spdk_ftp_tgt_destroy_cb(void *io_device)
{
	struct spdk_ftp_tgt *tgt = io_device;
	spdk_ftp_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;

	destroy_cb_fn = tgt->destroy_cb_fn;
	destroy_cb_arg = tgt->destroy_cb_arg;

	if (destroy_cb_fn) {
		destroy_cb_fn(destroy_cb_arg, 0);
	}
}


static void
spdk_ftp_tgt_destroy(struct spdk_ftp_tgt *tgt,
		     spdk_ftp_tgt_destroy_done_fn cb_fn,
		     void *cb_arg)
{
	spdk_ftp_tgt_destroy_server(tgt);
	tgt->destroy_cb_fn = cb_fn;
	tgt->destroy_cb_arg = cb_arg;

	spdk_io_device_unregister(tgt, spdk_ftp_tgt_destroy_cb);
}


static void
ftp_tgt_destroy_done(void *ctx, int status)
{

	/* todo free config */
	g_ftp_tgt_state = FTP_TGT_STOPPED;
	ftp_tgt_advance_state();
}




static void
ftp_tgt_advance_state(void)
{
	enum ftp_tgt_state prev_state;
	int rc = -1;

	do {
		prev_state = g_ftp_tgt_state;

		switch (g_ftp_tgt_state) {
		case FTP_TGT_INIT_NONE: {
			g_ftp_tgt_state = FTP_TGT_INIT_PARSE_CONFIG;
			break;
		}
		case FTP_TGT_INIT_PARSE_CONFIG: {
			spdk_thread_send_msg(spdk_get_thread(), ftp_tgt_parse_conf, NULL);
			break;
		}
		case FTP_TGT_INIT_CREATE_POLL_GROUPS: {
			/* Send a message to each thread and create a poll group */
			spdk_for_each_thread(ftp_tgt_create_poll_group, NULL,
					     ftp_tgt_create_poll_group_done);
			break;
		}

		case FTP_TGT_INIT_START_ACCEPTOR: {
			g_acceptor_poller = spdk_poller_register(ftp_tgt_acceptor_poll, g_spdk_ftp_tgt,
					    g_spdk_ftp_tgt_conf->acceptor_poll_rate);
			g_ftp_tgt_state = FTP_TGT_RUNNING;
			break;
		}
		case FTP_TGT_RUNNING: {
			spdk_subsystem_init_next(0);
			break;
		}
		case FTP_TGT_FINI_DESTROY_POLL_GROUPS: {
			spdk_for_each_thread(ftp_tgt_destroy_poll_group, NULL,
					     ftp_tgt_destroy_poll_group_done);
			break;
		}
		case FTP_TGT_FINI_STOP_ACCEPTOR : {
			spdk_poller_unregister(&g_acceptor_poller);
			g_ftp_tgt_state = FTP_TGT_FINI_FREE_RESOURCES;
			break;
		}
		case FTP_TGT_FINI_FREE_RESOURCES: {
			spdk_ftp_tgt_destroy(g_spdk_ftp_tgt, ftp_tgt_destroy_done, NULL);
			break;
		}
		case FTP_TGT_STOPPED: {
			spdk_subsystem_fini_next();
			return;
		}
		case FTP_TGT_ERROR: {
			spdk_subsystem_init_next(rc);
			return;
		}
		}
	} while (g_ftp_tgt_state != prev_state);
}


static void
spdk_ftp_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{

}


static struct spdk_subsystem g_spdk_subsystem_ftp = {
	.name = "ftp",
	.init = spdk_ftp_subsystem_init,
	.fini = spdk_ftp_subsystem_fini,
	.write_config_json = spdk_ftp_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_ftp)
SPDK_SUBSYSTEM_DEPEND(ftp, bdev)
