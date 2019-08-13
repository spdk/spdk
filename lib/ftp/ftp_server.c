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

static TAILQ_HEAD(, spdk_ftp_server) g_ftpds = TAILQ_HEAD_INITIALIZER(g_ftpds);


struct spdk_ftp_server *
spdk_ftp_server_create(struct spdk_ftp_tgt *tgt, uint16_t listen_port)
{
	struct spdk_ftp_server *new_ftpd;
	struct spdk_ftp_server *temp_ftpd;
	struct spdk_ftp_server_opts *ftpd_opts;

	TAILQ_FOREACH(temp_ftpd, &g_ftpds, link) {
		if (temp_ftpd) {
			if (temp_ftpd->opts->spdk_ftpd_listen_port == listen_port) {
				SPDK_ERRLOG("Ftpd %d: already used this port\n", listen_port);
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
		SPDK_ERRLOG("calloc() failed for spdk_ftp_server_opts!\n");
		return NULL;
	}

	ftpd_opts->spdk_ftpd_listen_port = listen_port;
	new_ftpd->opts = ftpd_opts;

	TAILQ_INSERT_TAIL(&g_ftpds, new_ftpd, link);

	return new_ftpd;
}

void
spdk_ftp_server_destroy(struct spdk_ftp_server *ftpd)
{
	if (ftpd == NULL) {
		return ;
	}

	free(ftpd->opts);
	ftpd->opts = NULL;

	free(ftpd);
	ftpd = NULL;

	return ;
}


static int
spdk_ftp_server_poll_group_poll(void *ctx)
{
	/* struct spdk_ftp_server_poll_group *group = ctx; */
	int count = 0;

	/* todo poll */

	return count;
}

struct spdk_ftp_server_poll_group *
spdk_ftp_server_poll_group_create(struct spdk_ftp_server *ftpd)
{
	struct spdk_ftp_server_poll_group *pg;

	pg = calloc(1, sizeof(*pg));
	pg->ftpd = ftpd;
	pg->poller = spdk_poller_register(spdk_ftp_server_poll_group_poll, pg, 0);

	return pg;
}


int
spdk_ftp_server_poll_group_destroy(struct spdk_ftp_server_poll_group *fspg)
{
	int rc = 0;

	fspg->ftpd = NULL;
	spdk_poller_unregister(&fspg->poller);

	return rc;
}
