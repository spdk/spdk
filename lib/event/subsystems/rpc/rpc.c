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
#include "spdk/log.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"

#define RPC_SELECT_INTERVAL	4000 /* 4ms */
#define RPC_DEFAULT_LISTEN_ADDR	"127.0.0.1:5260"

static struct spdk_poller *g_rpc_poller = NULL;

static int
enable_rpc(void)
{
	struct spdk_conf_section	*sp;

	sp = spdk_conf_find_section(NULL, "Rpc");
	if (sp == NULL) {
		return 0;
	}

	return spdk_conf_section_get_boolval(sp, "Enable", false);
}

static const char *
rpc_get_listen_addr(void)
{
	struct spdk_conf_section *sp;
	const char *val;

	sp = spdk_conf_find_section(NULL, "Rpc");
	if (sp == NULL) {
		return RPC_DEFAULT_LISTEN_ADDR;
	}

	val = spdk_conf_section_get_val(sp, "Listen");
	if (val == NULL) {
		val = RPC_DEFAULT_LISTEN_ADDR;
	}

	return val;
}

static void
spdk_rpc_subsystem_poll(void *arg)
{
	spdk_rpc_accept();
}

static void
spdk_rpc_subsystem_setup(void *arg)
{
	const char *listen_addr;
	int rc;

	/* Unregister the poller */
	spdk_poller_unregister(&g_rpc_poller, NULL);

	if (!enable_rpc()) {
		return;
	}

	listen_addr = rpc_get_listen_addr();
	if (listen_addr == NULL) {
		listen_addr = RPC_DEFAULT_LISTEN_ADDR;
	}

	/* Listen on the requested address */
	rc = spdk_rpc_listen(listen_addr);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to start RPC service at %s\n", listen_addr);
		return;
	}

	/* Register a poller to periodically check for RPCs */
	spdk_poller_register(&g_rpc_poller, spdk_rpc_subsystem_poll, NULL, spdk_env_get_current_core(),
			     RPC_SELECT_INTERVAL);
}

static void
spdk_rpc_subsystem_initialize(void)
{
	/*
	 * Defer setup of the RPC service until the reactor has started.  This
	 *  allows us to detect the RPC listen socket as a suitable proxy for determining
	 *  when the SPDK application has finished initialization and ready for logins
	 *  or RPC commands.
	 */
	spdk_poller_register(&g_rpc_poller, spdk_rpc_subsystem_setup, NULL, spdk_env_get_current_core(), 0);

	spdk_subsystem_init_next(0);
}

static int
spdk_rpc_subsystem_finish(void)
{
	spdk_rpc_close();

	return 0;
}

static void
spdk_rpc_subsystem_config_text(FILE *fp)
{
	fprintf(fp,
		"\n"
		"[Rpc]\n"
		"  # Defines whether to enable configuration via RPC.\n"
		"  # Default is disabled.  Note that the RPC interface is not\n"
		"  # authenticated, so users should be careful about enabling\n"
		"  # RPC in non-trusted environments.\n"
		"  Enable %s\n"
		"  # Listen address for the RPC service.\n"
		"  # May be an IP address or an absolute path to a Unix socket.\n"
		"  Listen %s\n",
		enable_rpc() ? "Yes" : "No", rpc_get_listen_addr());
}

SPDK_SUBSYSTEM_REGISTER(spdk_rpc, spdk_rpc_subsystem_initialize,
			spdk_rpc_subsystem_finish,
			spdk_rpc_subsystem_config_text)
