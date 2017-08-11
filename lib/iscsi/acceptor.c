/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/net.h"
#include "spdk/string.h"
#include "iscsi/acceptor.h"
#include "iscsi/conn.h"
#include "iscsi/portal_grp.h"

#define ACCEPT_TIMEOUT_US 1000 /* 1ms */

static struct spdk_poller *g_acceptor_poller;

static void
spdk_iscsi_portal_accept(struct spdk_iscsi_portal *portal)
{
	int				rc, sock;
	char buf[64];

	if (portal->sock < 0) {
		return;
	}

	while (1) {
		rc = spdk_sock_accept(portal->sock);
		if (rc >= 0) {
			sock = rc;
			rc = spdk_iscsi_conn_construct(portal, sock);
			if (rc < 0) {
				close(sock);
				SPDK_ERRLOG("spdk_iscsi_connection_construct() failed\n");
				break;
			}
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				spdk_strerror_r(errno, buf, sizeof(buf));
				SPDK_ERRLOG("accept error(%d): %s\n", errno, buf);
			}
			break;
		}
	}
}

static void
spdk_acceptor(void *arg)
{
	struct spdk_iscsi_globals		*iscsi = arg;
	struct spdk_iscsi_portal_grp	*portal_group;
	struct spdk_iscsi_portal	*portal;

	TAILQ_FOREACH(portal_group, &iscsi->pg_head, tailq) {
		TAILQ_FOREACH(portal, &portal_group->head, tailq) {
			spdk_iscsi_portal_accept(portal);
		}
	}
}

void
spdk_iscsi_acceptor_start(void)
{
	spdk_poller_register(&g_acceptor_poller, spdk_acceptor, &g_spdk_iscsi, spdk_env_get_current_core(),
			     ACCEPT_TIMEOUT_US);
}

void
spdk_iscsi_acceptor_stop(void)
{
	spdk_poller_unregister(&g_acceptor_poller, NULL);
}
