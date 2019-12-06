/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/ftp.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "ftp_internal.h"
#include "spdk/log.h"
struct spdk_ftp_tgt_add_server_ctx {
	struct spdk_ftp_tgt *tgt;
	struct spdk_ftp_server *server;
	spdk_ftp_tgt_add_server_done_fn cb_fn;
	void *cb_arg;
};

struct spdk_ftp_poll_group *
spdk_ftp_poll_group_create(struct spdk_ftp_tgt *tgt)
{
        struct spdk_io_channel *ch;

        ch = spdk_get_io_channel(tgt);
        if (!ch) {
                SPDK_ERRLOG("Unable to get I/O channel for target\n");
                return NULL;
        }

        return spdk_io_channel_get_ctx(ch);
}

static int
spdk_ftp_poll_group_poll(void *ctx)
{
	struct spdk_ftp_poll_group *group = ctx;
	int rc, count = 0;
	struct spdk_ftp_server_poll_group *sgroup;

	TAILQ_FOREACH(sgroup, &group->ftpd_pgs, link) {
		rc = spdk_ftp_server_poll_group_poll(sgroup);
		if (rc < 0) {
			return -1;
		}
		count += rc;
	}

	return count;
}

static int
spdk_ftp_tgt_create_poll_group(void *io_device, void *ctx_buf)
{

	struct spdk_ftp_tgt *tgt = io_device;
	struct spdk_ftp_poll_group *group = ctx_buf;
	struct spdk_ftp_server *temp_ftpd;
	//struct spdk_ftp_server_poll_group *fpg;

	TAILQ_INIT(&group->ftpd_pgs);
	TAILQ_INIT(&group->conns);
	group->thread = spdk_get_thread();

	TAILQ_FOREACH(temp_ftpd, &tgt->ftpds, link) {
		spdk_ftp_poll_group_add_server(group, temp_ftpd);
		// fpg = spdk_ftp_server_poll_group_create(temp_ftpd);
		// TAILQ_INSERT_TAIL(&group->ftpd_pgs, fpg, link);
	}

	group->poller = spdk_poller_register(spdk_ftp_poll_group_poll, group, 0);
	group->thread = spdk_get_thread();

	printf("poll group is created\n");

	return 0;
}


static void
spdk_ftp_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_ftp_poll_group *group = ctx_buf;
	struct spdk_ftp_server_poll_group *temp_fpg;
	spdk_poller_unregister(&group->poller);
	TAILQ_FOREACH(temp_fpg, &group->ftpd_pgs, link) {
		TAILQ_REMOVE(&group->ftpd_pgs, temp_fpg, link);
		spdk_ftp_server_poll_group_destroy(temp_fpg);
	}

}


void
spdk_ftp_poll_group_destroy(struct spdk_ftp_poll_group *group)
{
	/* destroy io_channel invoke
	 * this will call spdk_ftp_tgt_destroy_poll_group , because io_channel begin to be destroy
	 */
	spdk_put_io_channel(spdk_io_channel_from_ctx(group));
}


void
spdk_ftp_tgt_destroy_server(struct spdk_ftp_tgt *tgt)
{
	struct spdk_ftp_server *temp_ftpd;

	TAILQ_FOREACH(temp_ftpd, &tgt->ftpds, link) {
		TAILQ_REMOVE(&tgt->ftpds, temp_ftpd, link);
		spdk_ftp_server_destroy(temp_ftpd);
	}

}


struct spdk_ftp_tgt *
spdk_ftp_tgt_create(void)
{

	struct spdk_ftp_tgt *tgt;

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	TAILQ_INIT(&tgt->ftpds);
	printf("before spdk_io_device_register\n ");
	/* register io_channel */
	spdk_io_device_register(tgt,
				spdk_ftp_tgt_create_poll_group,	/* io_channel create function */
				spdk_ftp_tgt_destroy_poll_group,	/* io_channel destroy function */
				sizeof(struct spdk_ftp_poll_group),
				"ftp_tgt");
	printf("end spdk_io_device_register\n ");
	return tgt;
}

void
spdk_ftp_tgt_accept(struct spdk_ftp_tgt *tgt, new_conn_fn cb_fn)
{
	struct spdk_ftp_server *ftpds, *tmp;
	TAILQ_FOREACH_SAFE(ftpds, &tgt->ftpds, link, tmp) {
		spdk_ftp_server_accept(ftpds, cb_fn);
	}
}

static void
_spdk_ftp_tgt_add_server_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_ftp_tgt_add_server_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}


static void
_spdk_ftp_tgt_add_server(struct spdk_io_channel_iter *i)
{
	printf("begin _spdk_ftp_tgt_add_server \n ");
	struct spdk_ftp_tgt_add_server_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_ftp_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;
	rc = spdk_ftp_poll_group_add_server(group, ctx->server);
	spdk_for_each_channel_continue(i, rc);
}


void spdk_ftp_tgt_add_server(struct spdk_ftp_tgt *tgt,
			     struct spdk_ftp_server *server,
			     spdk_ftp_tgt_add_server_done_fn cb_fn,
			     void *cb_arg)
{
	struct spdk_ftp_tgt_add_server_ctx *ctx;


	if (spdk_ftp_tgt_get_server(tgt, server->ops->type)) {
		cb_fn(cb_arg, -EEXIST);
		return; /* transport already created */
	}
	server->tgt = tgt;
	TAILQ_INSERT_TAIL(&tgt->ftpds, server, link);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->tgt = tgt;
	ctx->server = server;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	// spdk_ftp_poll_group_add_server(group, ctx->server);
	spdk_for_each_channel(tgt,
			      _spdk_ftp_tgt_add_server,
			      ctx,
			      _spdk_ftp_tgt_add_server_done);
}

int spdk_ftp_poll_group_add_server(struct spdk_ftp_poll_group *group,
				   struct spdk_ftp_server *server)
{
	struct spdk_ftp_server_poll_group *sgroup;

	TAILQ_FOREACH(sgroup, &group->ftpd_pgs, link) {
		if (sgroup->ftpd == server) {
			/* server already in the poll group */
			return 0;
		}
	}

	sgroup = spdk_ftp_server_poll_group_create(server);
	if (!sgroup) {
		SPDK_ERRLOG("Unable to create poll group for server\n");
		return -1;
	}
	sgroup-> group = group;
	TAILQ_INSERT_TAIL(&group->ftpd_pgs, sgroup, link);
	return 0;

}
int spdk_ftp_server_parse_type(enum spdk_ftp_server_type *stype, const char *str)
{
	if (stype == NULL || str == NULL) {
		return -EINVAL;
	}
	if (strcasecmp(str, "TCP") == 0) {
		*stype = SPDK_FTP_TCP;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*stype = SPDK_FTP_RDMA;
	} else {
		return -ENOENT;
	}
	return 0;

}

const char *
spdk_ftp_server_trtype_str(enum spdk_ftp_server_type stype)
{
	switch (stype) {
	case SPDK_FTP_TCP:
		return "FTP";
		break;
	case SPDK_FTP_RDMA:
		return "RDMA";
		break;

	default:
		return NULL;
	}
}

struct spdk_ftp_server *spdk_ftp_tgt_get_server(struct spdk_ftp_tgt *tgt,
		enum spdk_ftp_server_type type)
{
	struct spdk_ftp_server *server;

	TAILQ_FOREACH(server, &tgt->ftpds, link) {
		if (server->ops->type == type) {
			return server;
		}
	}
	return NULL;

}


void spdk_ftp_tgt_listen(struct spdk_ftp_tgt *tgt,
			 enum spdk_ftp_server_type type,
			 spdk_ftp_tgt_listen_done_fn cb_fn,
			 void *cb_arg)
{
	struct spdk_ftp_server *server;
	const char *stype;
	int rc;

	server = spdk_ftp_tgt_get_server(tgt, type);
	if (!server) {
		stype = spdk_ftp_server_trtype_str(type);
		if (stype == NULL) {
			SPDK_ERRLOG("Unable to listen on transport %s. The transport must be created first.\n", stype);
		} else {
			SPDK_ERRLOG("The specified stype %d is unknown. Please make sure that it is properly registered.\n",
				    type);
		}
		cb_fn(cb_arg, -EINVAL);
		return;
	}
	rc = spdk_ftp_server_listen(server);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", server->opts.ipaddr);
		cb_fn(cb_arg, rc);
		return;
	}
	cb_fn(cb_arg, 0);

}

int spdk_ftp_conn_disconnect(struct spdk_ftp_conn *conn, ftp_conn_disconnect_cb cb_fn,
			     void *ctx)
{
	return 0;
}

int
spdk_ftp_poll_group_add(struct spdk_ftp_poll_group *group,
			struct spdk_ftp_conn *conn)
{
	int rc = -1;
	struct spdk_ftp_server_poll_group *sgroup;

	conn->group = group;

	TAILQ_FOREACH(sgroup, &group->ftpd_pgs, link) {
		if (sgroup->ftpd == conn->server) {
			rc = spdk_ftp_server_poll_group_add(sgroup, conn);
			break;
		}

	}
	if (rc == 0) {
		TAILQ_INSERT_TAIL(&group->conns, conn, link);

	}
	return rc;
}
