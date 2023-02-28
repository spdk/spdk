/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

#define RPC_SELECT_INTERVAL	4000 /* 4ms */

static struct spdk_poller *g_rpc_poller = NULL;

static int
rpc_subsystem_poll(void *arg)
{
	spdk_rpc_accept();
	return SPDK_POLLER_BUSY;
}

int
spdk_rpc_initialize(const char *listen_addr)
{
	int rc;

	if (listen_addr == NULL) {
		/* Not treated as an error */
		return 0;
	}

	if (!spdk_rpc_verify_methods()) {
		return -EINVAL;
	}

	/* Listen on the requested address */
	rc = spdk_rpc_listen(listen_addr);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to start RPC service at %s\n", listen_addr);
		/* TODO: Eventually, treat this as an error. But it historically has not
		 * been and many tests rely on this gracefully failing. */
		return 0;
	}

	spdk_rpc_set_state(SPDK_RPC_STARTUP);

	/* Register a poller to periodically check for RPCs */
	g_rpc_poller = SPDK_POLLER_REGISTER(rpc_subsystem_poll, NULL, RPC_SELECT_INTERVAL);

	return 0;
}

void
spdk_rpc_finish(void)
{
	spdk_rpc_close();
	spdk_poller_unregister(&g_rpc_poller);
}
