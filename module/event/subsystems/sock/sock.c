/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk_internal/init.h"
#include "spdk/log.h"

static void
sock_subsystem_init(void)
{
	const char *sock_impl_override = getenv("SPDK_SOCK_IMPL_DEFAULT");
	int rc = 0;

	if (sock_impl_override) {
		rc = spdk_sock_set_default_impl(sock_impl_override);
		if (rc) {
			SPDK_ERRLOG("Could not override socket implementation with: %s,"
				    " set by SPDK_SOCK_IMPL_DEFAULT environment variable\n",
				    sock_impl_override);
		} else {
			SPDK_NOTICELOG("Default socket implementation override: %s\n",
				       sock_impl_override);
		}
	}

	spdk_subsystem_init_next(rc);
}

static void
sock_subsystem_fini(void)
{
	spdk_subsystem_fini_next();
}

static void
sock_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_sock_write_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_sock = {
	.name = "sock",
	.init = sock_subsystem_init,
	.fini = sock_subsystem_fini,
	.write_config_json = sock_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_sock);
