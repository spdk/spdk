/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/nbd.h"

#include "spdk_internal/init.h"

static void
nbd_subsystem_init(void)
{
	int rc;

	rc = spdk_nbd_init();

	spdk_subsystem_init_next(rc);
}

static void
nbd_subsystem_fini_done(void *arg)
{
	spdk_subsystem_fini_next();
}

static void
nbd_subsystem_fini(void)
{
	spdk_nbd_fini(nbd_subsystem_fini_done, NULL);
}

static void
nbd_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_nbd_write_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_nbd = {
	.name = "nbd",
	.init = nbd_subsystem_init,
	.fini = nbd_subsystem_fini,
	.write_config_json = nbd_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_nbd);
SPDK_SUBSYSTEM_DEPEND(nbd, bdev)
