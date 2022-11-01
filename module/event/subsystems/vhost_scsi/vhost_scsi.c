/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/vhost.h"

#include "spdk_internal/init.h"

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

static struct spdk_subsystem g_spdk_subsystem_vhost_scsi = {
	.name = "vhost_scsi",
	.init = vhost_scsi_subsystem_init,
	.fini = vhost_scsi_subsystem_fini,
	.write_config_json = spdk_vhost_scsi_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_vhost_scsi);
SPDK_SUBSYSTEM_DEPEND(vhost_scsi, scsi)
