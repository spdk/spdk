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
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "iscsi/acceptor.h"
#include "iscsi/conn.h"
#include "iscsi/portal_grp.h"

#define ACCEPT_TIMEOUT_US 1000 /* 1ms */

static void
spdk_iscsi_portal_accept(void *arg)
{
	struct spdk_iscsi_portal	*portal = arg;
	struct spdk_sock		*sock;
	int				rc;

	if (portal->sock == NULL) {
		return;
	}

	while (1) {
		sock = spdk_sock_accept(portal->sock);
		if (sock != NULL) {
			rc = spdk_iscsi_conn_construct(portal, sock);
			if (rc < 0) {
				spdk_sock_close(&sock);
				SPDK_ERRLOG("spdk_iscsi_connection_construct() failed\n");
				break;
			}
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			break;
		}
	}
}

void
spdk_iscsi_acceptor_start(struct spdk_iscsi_portal *p)
{
	p->acceptor_poller = spdk_poller_register(spdk_iscsi_portal_accept, p, ACCEPT_TIMEOUT_US);
}

void
spdk_iscsi_acceptor_stop(struct spdk_iscsi_portal *p)
{
	spdk_poller_unregister(&p->acceptor_poller);
}
