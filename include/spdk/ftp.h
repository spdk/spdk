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


#ifndef SPDK_NVMF_H
#define SPDK_NVMF_H

#include "spdk/stdinc.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/uuid.h"


#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvmf_tgt;
struct spdk_ftp_poll_group;

struct spdk_ftp_server_poll_group;
struct spdk_ftp_server;

struct spdk_ftp_server_opts {
	bool		spdk_ftpd_anonymous_enable;
	bool		spdk_ftpd_local_enable;
	bool		spdk_ftpd_log_enable;
	/* char *		spdk_ftpd_log_file; */
	/* char *		spdk_ftpd_banner; */
	bool		spdk_ftpd_deny_email_enable;
	/* char *		spdk_ftpd_banned_email_file; */
	uint16_t	spdk_ftpd_listen_port;
	/* ftp passive mode */
	bool		spdk_ftpd_pasv_enable;
	uint16_t	spdk_ftpd_pasv_min_port;
	uint16_t	spdk_ftpd_pasv_max_port;
	uint16_t	spdk_ftpd_idle_session_timeout;
	uint16_t	spdk_ftpd_connect_timeout;
	uint16_t	spdk_ftpd_max_clients;
	uint16_t	spdk_ftpd_max_per_ip;
};


struct spdk_ftp_server {
	struct spdk_ftp_server_opts *opts;

	TAILQ_ENTRY(spdk_ftp_server) link;
};

typedef void (spdk_ftp_tgt_destroy_done_fn)(void *ctx, int status);

struct spdk_ftp_tgt {
	TAILQ_HEAD(, spdk_ftp_server) ftpds;

	spdk_ftp_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;
};


void spdk_ftp_tgt_destroy_server(struct spdk_ftp_tgt *tgt);

struct spdk_ftp_tgt *spdk_ftp_tgt_create(void);

struct spdk_ftp_server *spdk_ftp_server_create(struct spdk_ftp_tgt *tgt, uint16_t listen_port);

void spdk_ftp_poll_group_destroy(struct spdk_ftp_poll_group *group);

#ifdef __cplusplus
}
#endif


#endif
