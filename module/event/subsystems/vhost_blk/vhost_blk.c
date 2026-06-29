/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/vhost.h"

#include "spdk_internal/init.h"
#include "spdk/log.h"
#include "spdk/hot_upgrade.h"
#include "spdk/hot_upgrade_shared.h"

static void
vhost_blk_subsystem_init_done(int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
vhost_blk_subsystem_init(void)
{
	spdk_vhost_blk_init(vhost_blk_subsystem_init_done);
}

static void
vhost_blk_subsystem_fini_done(void)
{
	spdk_subsystem_fini_next();
}

static void
vhost_blk_subsystem_fini(void)
{
	spdk_vhost_blk_fini(vhost_blk_subsystem_fini_done);
}

/* Hot upgrade callbacks */
int spdk_vhost_hu_primary_drain_all(void);
int spdk_vhost_hu_primary_suspend_all(void);
int spdk_vhost_hu_secondary_takeover_all(void);

static void
vhost_blk_primary_drain_io(void *arg)
{ spdk_vhost_hu_primary_drain_all(); spdk_subsystem_primary_drain_io_next(0); }

static void
vhost_blk_primary_suspend(void *arg)
{ spdk_vhost_hu_primary_suspend_all(); spdk_subsystem_primary_suspend_next(0); }

static void
vhost_blk_secondary_pre_init(void *arg)
{ spdk_subsystem_secondary_pre_init_next(0); }

static void
vhost_blk_secondary_takeover(void *arg)
{ spdk_vhost_hu_secondary_takeover_all(); spdk_subsystem_secondary_takeover_next(0); }


static struct spdk_subsystem g_spdk_subsystem_vhost_blk = {
	.name = "vhost_blk",
	.init = vhost_blk_subsystem_init,
	.fini = vhost_blk_subsystem_fini,
	.write_config_json = spdk_vhost_blk_config_json,
	.primary_drain_io = vhost_blk_primary_drain_io,
	.primary_suspend = vhost_blk_primary_suspend,
	.secondary_pre_init = vhost_blk_secondary_pre_init,
	.secondary_takeover = vhost_blk_secondary_takeover,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_vhost_blk);
SPDK_SUBSYSTEM_DEPEND(vhost_blk, bdev)
