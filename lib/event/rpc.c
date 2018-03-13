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

#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"

#define RPC_SELECT_INTERVAL	4000 /* 4ms */

static struct spdk_poller *g_rpc_poller = NULL;

static int
spdk_rpc_subsystem_poll(void *arg)
{
	spdk_rpc_accept();
	return -1;
}

void
spdk_rpc_initialize(const char *listen_addr)
{
	int rc;

	if (listen_addr == NULL) {
		return;
	}

	/* Listen on the requested address */
	rc = spdk_rpc_listen(listen_addr);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to start RPC service at %s\n", listen_addr);
		return;
	}

	/* Register a poller to periodically check for RPCs */
	g_rpc_poller = spdk_poller_register(spdk_rpc_subsystem_poll, NULL, RPC_SELECT_INTERVAL);
}

void
spdk_rpc_finish(void)
{
	spdk_rpc_close();
	spdk_poller_unregister(&g_rpc_poller);
}
