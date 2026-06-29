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
vhost_scsi_subsystem_init_done(int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
vhost_scsi_subsystem_init(void)
{
	spdk_vhost_scsi_init(vhost_scsi_subsystem_init_done);
}

static void
vhost_scsi_subsystem_fini_done(void)
{
	spdk_subsystem_fini_next();
}

static void
vhost_scsi_subsystem_fini(void)
{
	spdk_vhost_scsi_fini(vhost_scsi_subsystem_fini_done);
}

/* Hot upgrade callbacks */
int spdk_vhost_hu_primary_drain_all(void);
int spdk_vhost_hu_primary_suspend_all(void);
int spdk_vhost_hu_secondary_takeover_all(void);

static void
vhost_scsi_primary_drain_io(void *arg)
{ spdk_vhost_hu_primary_drain_all(); spdk_subsystem_primary_drain_io_next(0); }

static void
vhost_scsi_primary_suspend(void *arg)
{ spdk_vhost_hu_primary_suspend_all(); spdk_subsystem_primary_suspend_next(0); }

static void
vhost_scsi_secondary_pre_init(void *arg)
{ spdk_subsystem_secondary_pre_init_next(0); }

static void
vhost_scsi_secondary_takeover(void *arg)
{ spdk_vhost_hu_secondary_takeover_all(); spdk_subsystem_secondary_takeover_next(0); }


static struct spdk_subsystem g_spdk_subsystem_vhost_scsi = {
	.name = "vhost_scsi",
	.init = vhost_scsi_subsystem_init,
	.fini = vhost_scsi_subsystem_fini,
	.write_config_json = spdk_vhost_scsi_config_json,
	.primary_drain_io = vhost_scsi_primary_drain_io,
	.primary_suspend = vhost_scsi_primary_suspend,
	.secondary_pre_init = vhost_scsi_secondary_pre_init,
	.secondary_takeover = vhost_scsi_secondary_takeover,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_vhost_scsi);
SPDK_SUBSYSTEM_DEPEND(vhost_scsi, scsi)
