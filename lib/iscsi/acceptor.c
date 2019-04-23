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
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "iscsi/acceptor.h"
#include "iscsi/conn.h"
#include "iscsi/portal_grp.h"

static void
iscsi_portal_accept(void *arg, struct spdk_sock_group *group,
		    struct spdk_sock *sock)
{
	struct spdk_iscsi_portal	*portal = arg;
	struct spdk_sock		*accept_sock;
	int				rc;

	if (portal->sock == NULL) {
		return;
	}

	while (1) {
		accept_sock = spdk_sock_accept(portal->sock);
		if (accept_sock != NULL) {
			rc = spdk_iscsi_conn_construct(portal, accept_sock);
			if (rc < 0) {
				spdk_sock_close(&accept_sock);
				SPDK_ERRLOG("spdk_iscsi_connection_construct() failed\n");
				return;
			}
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			return;
		}
	}
}

void
spdk_iscsi_acceptor_start(struct spdk_iscsi_portal *p)
{
	struct spdk_iscsi_poll_group *poll_group;
	int rc;

	p->lcore = spdk_env_get_current_core();
	poll_group = &g_spdk_iscsi.poll_group[p->lcore];

	rc = spdk_sock_group_add_sock(poll_group->sock_group, p->sock,
				      iscsi_portal_accept, p);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add sock=%p of portal=%p\n", p->sock, p);
	}
}

void
spdk_iscsi_acceptor_stop(struct spdk_iscsi_portal *p)
{
	struct spdk_iscsi_poll_group *poll_group;
	int rc;

	assert(p->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_iscsi.poll_group[p->lcore];

	rc = spdk_sock_group_remove_sock(poll_group->sock_group, p->sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove sock=%p of portal=%p\n", p->sock, p);
	}
}
