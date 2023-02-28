/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/scsi.h"

#include "spdk_internal/init.h"

static void
scsi_subsystem_init(void)
{
	int rc;

	rc = spdk_scsi_init();

	spdk_subsystem_init_next(rc);
}

static void
scsi_subsystem_fini(void)
{
	spdk_scsi_fini();
	spdk_subsystem_fini_next();
}

static struct spdk_subsystem g_spdk_subsystem_scsi = {
	.name = "scsi",
	.init = scsi_subsystem_init,
	.fini = scsi_subsystem_fini,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_scsi);
SPDK_SUBSYSTEM_DEPEND(scsi, bdev)
