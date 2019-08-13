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
#include "spdk/sock.h"
#include "spdk/util.h"


static TAILQ_HEAD(, spdk_ftp_server) g_ftpds = TAILQ_HEAD_INITIALIZER(g_ftpds);

static const struct spdk_ftp_server_ops *const g_server_ops[] = {
#ifdef SPDK_CONFIG_RDMA
	&spdk_ftp_server_rdma,
#endif
	&spdk_ftp_server_tcp,

};

#define NUM_TRANSPORTS (SPDK_COUNTOF(g_server_ops))
#define MAX_MEMPOOL_NAME_LENGTH 40


static inline const struct spdk_ftp_server_ops *
spdk_ftp_get_server_ops(enum spdk_ftp_server_type type)
{
	size_t i;
	for (i = 0; i != NUM_TRANSPORTS; i++) {
		if (g_server_ops[i]->type == type) {
			return g_server_ops[i];
		}
	}
	return NULL;

}
struct spdk_ftp_server *
spdk_ftp_server_create(enum spdk_ftp_server_type type, struct spdk_ftp_server_opts *opts)
{
	/*struct spdk_ftp_server *new_ftpd;
	struct spdk_ftp_server *temp_ftpd;
	struct spdk_ftp_server_opts *ftpd_opts=opts;
	const struct spdk_ftp_server_ops *ops=NULL;

	TAILQ_FOREACH(temp_ftpd, &g_ftpds, link) {
		if (temp_ftpd) {
			if (temp_ftpd->opts->spdk_ftpd_listen_port == opts->spdk_ftpd_listen_port) {
				SPDK_ERRLOG("Ftpd %d: already used this port\n", opts->spdk_ftpd_listen_port);
				return NULL;
			}
		}
	}

	new_ftpd = calloc(1, sizeof(*new_ftpd));
	if (!new_ftpd) {
		SPDK_ERRLOG("calloc() failed for spdk_ftp_server!\n");
		return NULL;
	}

	ftpd_opts = calloc(1, sizeof(*ftpd_opts));
	if (!ftpd_opts) {
		free(new_ftpd);
		SPDK_ERRLOG("calloc() failed for spdk_ftp_server_opts!\n");
		return NULL;
	}

	ftpd_opts->spdk_ftpd_listen_port = opts->spdk_ftpd_listen_port;
	new_ftpd->opts = ftpd_opts;
	 new_ftpd->listen_sock = spdk_sock_listen(opts->ipaddr, opts->spdk_ftpd_listen_port);
	if(new_ftpd->listen_sock == NULL)
	{
		printf("listen sock error\n");
	}else{
		printf("listen at %s \n", opts->ipaddr);
	}


	TAILQ_INSERT_TAIL(&g_ftpds, new_ftpd, link);*/

	const struct spdk_ftp_server_ops *ops = NULL;
	struct spdk_ftp_server *server;
	char spdk_mempool_name[MAX_MEMPOOL_NAME_LENGTH];
	int chars_written;

	ops = spdk_ftp_get_server_ops(type);
	if (!ops) {
		SPDK_ERRLOG("server type '%s' unavailable.\n",
			    spdk_ftp_server_trtype_str(type));
		return NULL;
	}
	server = ops->create(opts);
	if (!server) {
		SPDK_ERRLOG("Unable to create new server of type %s\n",
			    spdk_ftp_server_trtype_str(type));
		return NULL;
	}
	server->ops = ops;
	server->opts = *opts;

	chars_written = snprintf(spdk_mempool_name, MAX_MEMPOOL_NAME_LENGTH, "%s_%s_%s", "spdk_ftp",
				 spdk_ftp_server_trtype_str(type), "data");
	if (chars_written < 0) {
		SPDK_ERRLOG("Unable to generate server data buffer pool name.\n");
		ops->destroy(server);
		return NULL;
	}

	/*server->data_buf_pool = spdk_mempool_create(spdk_mempool_name,
				   opts->num_shared_buffers,
				   opts->io_unit_size + FTP_DATA_BUFFER_ALIGNMENT,
				   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_SOCKET_ID_ANY);

	if (!server->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
		ops->destroy(server);
		return NULL;
	}*/




	return server;
}

void
spdk_ftp_server_destroy(struct spdk_ftp_server *ftpd)
{
	if (ftpd == NULL) {
		return ;
	}



	free(ftpd);
	ftpd = NULL;

	return ;
}


int
spdk_ftp_server_poll_group_poll(struct spdk_ftp_server_poll_group *group)
{
	// struct spdk_ftp_poll_group *group = ctx;
	// struct spdk_ftp_server_poll_group *fgroup;

	/* TAILQ_FOREACH(fgroup,&group->ftpd_pgs, link){

	} */

	/* todo poll */

	return group->ftpd->ops->poll_group_poll(group);
}

struct spdk_ftp_server_poll_group *
spdk_ftp_server_poll_group_create(struct spdk_ftp_server *ftpd)
{
	struct spdk_ftp_server_poll_group *pg;
	pg = ftpd->ops->poll_group_create(ftpd);
	if (!pg) {
		return NULL;
	}
	pg->ftpd = ftpd;

	// RUI TODO:
	// need buf_cache?
	//pg->poller = spdk_poller_register(spdk_ftp_server_poll_group_poll, pg, 0);

	return pg;
}


int
spdk_ftp_server_poll_group_destroy(struct spdk_ftp_server_poll_group *fspg)
{
	int rc = 0;

	fspg->ftpd = NULL;
	//spdk_poller_unregister(&fspg->poller);

	return rc;
}

void spdk_ftp_server_accept(struct spdk_ftp_server *ftpd, new_conn_fn cb_fn)
{
	return ftpd->ops->accept(ftpd, cb_fn);
}

bool spdk_ftp_server_opts_init(enum spdk_ftp_server_type type,
			       struct spdk_ftp_server_opts *opts)
{
	const struct spdk_ftp_server_ops *ops;
	ops = spdk_ftp_get_server_ops(type);
	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n",
			    spdk_ftp_server_trtype_str(type));
		return false;
	}

	ops->opts_init(opts);
	return true;
}

int spdk_ftp_server_listen(struct spdk_ftp_server *server)
{
	return server->ops->listen(server);
}
int spdk_ftp_server_poll_group_add(struct spdk_ftp_server_poll_group *group,
				   struct spdk_ftp_conn *conn)
{
	if (conn->server) {
		assert(conn->server == group->ftpd);
		if (conn->server != group->ftpd) {
			return -1;
		}
	} else {
		conn->server = group->ftpd;
	}
	return group->ftpd->ops->poll_group_add(group, conn);
}
