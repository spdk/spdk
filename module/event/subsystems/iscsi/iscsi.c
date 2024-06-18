/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "iscsi/iscsi.h"

#include "spdk_internal/init.h"

static void
iscsi_subsystem_init_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
iscsi_subsystem_init(void)
{
	spdk_iscsi_init(iscsi_subsystem_init_complete, NULL);
}

static void
iscsi_subsystem_fini_done(void *arg)
{
	spdk_subsystem_fini_next();
}

static void
iscsi_subsystem_fini(void)
{
	spdk_iscsi_fini(iscsi_subsystem_fini_done, NULL);
}

static void
iscsi_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_iscsi_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_iscsi = {
	.name = "iscsi",
	.init = iscsi_subsystem_init,
	.fini = iscsi_subsystem_fini,
	.write_config_json = iscsi_subsystem_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_iscsi);
SPDK_SUBSYSTEM_DEPEND(iscsi, scsi)
SPDK_SUBSYSTEM_DEPEND(iscsi, sock)
