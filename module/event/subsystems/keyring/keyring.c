/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/keyring.h"
#include "spdk_internal/init.h"

static void
keyring_subsystem_init(void)
{
	int rc = spdk_keyring_init();

	spdk_subsystem_init_next(rc);
}

static void
keyring_subsystem_fini(void)
{
	spdk_keyring_cleanup();
	spdk_subsystem_fini_next();
}

static void
keyring_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);
	spdk_keyring_write_config(w);
	spdk_json_write_array_end(w);
}

static struct spdk_subsystem g_subsystem_keyring = {
	.name = "keyring",
	.init = keyring_subsystem_init,
	.fini = keyring_subsystem_fini,
	.write_config_json = keyring_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_subsystem_keyring);
