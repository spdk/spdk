/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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


#ifndef __FTP_INTERNAL_H__
#define __FTP_INTERNAL_H__

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/thread.h"


#define SHIFT_4KB			12u
#define FTP_DATA_BUFFER_ALIGNMENT	(1u << SHIFT_4KB)
#define SPDK_FTP_TRADDR_MAX_LEN 256
#define MAX_COMMAND_LINE 1024
#define MAX_COMMAND 32
#define MAX_ARG 512
#define MAX_LINE 1024

struct spdk_ftp_poll_group {
	struct spdk_thread				*thread;
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_ftp_server_poll_group)	ftpd_pgs;

	/* All of the queue connections that belong to this poll group */
	TAILQ_HEAD(, spdk_ftp_conn)			conns;
};

struct spdk_ftp_server_poll_group {
	struct spdk_ftp_server				*ftpd;

	struct spdk_ftp_poll_group *group;
	TAILQ_ENTRY(spdk_ftp_server_poll_group)			link;
};

struct spdk_ftp_conn {
	struct spdk_ftp_poll_group *group;
	struct spdk_ftp_server *server;

	TAILQ_ENTRY(spdk_ftp_conn)		link;

};

void spdk_ftp_server_destroy(struct spdk_ftp_server *ftpd);
struct spdk_ftp_server *spdk_ftp_server_create(enum spdk_ftp_server_type type,
		struct spdk_ftp_server_opts *opts);


struct spdk_ftp_server_poll_group *spdk_ftp_server_poll_group_create(struct spdk_ftp_server *ftpd);
int spdk_ftp_server_poll_group_destroy(struct spdk_ftp_server_poll_group *fspg);
void spdk_ftp_server_accept(struct spdk_ftp_server *ftpd, new_conn_fn cb_fn);

int spdk_ftp_poll_group_add_server(struct spdk_ftp_poll_group *group,
				   struct spdk_ftp_server *server);
struct spdk_ftp_server *spdk_ftp_tgt_get_server(struct spdk_ftp_tgt *tgt,
		enum spdk_ftp_server_type type);
int spdk_ftp_server_poll_group_remove(struct spdk_ftp_server_poll_group *group,
				      struct spdk_ftp_conn *conn);
#endif /* __FTP_INTERNAL_H__ */
