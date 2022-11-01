/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk_internal/init.h"

static void
sock_subsystem_init(void)
{
	spdk_subsystem_init_next(0);
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
