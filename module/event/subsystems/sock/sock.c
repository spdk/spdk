/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk_internal/init.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk_internal/sock_module.h"
#include "spdk/string.h"

static void
sock_subsystem_init(void)
{
	const char *sock_impl_override = getenv("SPDK_SOCK_IMPL_DEFAULT");
	struct spdk_sock_initialize_opts init_opts;
	int rc = 0;

	/* Initialize net implementations with interrupt mode status */
	spdk_sock_get_default_initialize_opts(&init_opts, sizeof(init_opts));
	init_opts.enable_interrupt_mode = spdk_interrupt_mode_is_enabled();
	rc = spdk_sock_initialize(&init_opts);
	if (rc) {
		SPDK_ERRLOG("Failed to initialize sock net implementations: %d\n", rc);
		spdk_subsystem_init_next(rc);
		return;
	}

	if (sock_impl_override) {
		rc = spdk_sock_set_default_impl(sock_impl_override);
		if (rc < 0) {
			SPDK_ERRLOG("Could not override socket implementation with: %s,"
				    " set by SPDK_SOCK_IMPL_DEFAULT environment variable, rc %d: %s\n", sock_impl_override, rc,
				    spdk_strerror(-rc));
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
