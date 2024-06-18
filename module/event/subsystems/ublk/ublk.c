/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/ublk.h"
#include "spdk_internal/init.h"

static void
ublk_subsystem_init(void)
{
	spdk_ublk_init();
	spdk_subsystem_init_next(0);
}

static void
ublk_subsystem_fini_done(void *arg)
{
	spdk_subsystem_fini_next();
}

static void
ublk_subsystem_fini(void)
{
	int rc;

	rc = spdk_ublk_fini(ublk_subsystem_fini_done, NULL);
	if (rc != 0) {
		ublk_subsystem_fini_done(NULL);
	}
}

static void
ublk_subsystem_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_ublk_write_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_ublk = {
	.name = "ublk",
	.init = ublk_subsystem_init,
	.fini = ublk_subsystem_fini,
	.write_config_json = ublk_subsystem_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_ublk);
SPDK_SUBSYSTEM_DEPEND(ublk, bdev)
