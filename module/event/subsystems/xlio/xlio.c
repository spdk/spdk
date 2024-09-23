/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"

#include "spdk/init.h"
#include "spdk_internal/xlio.h"

static void
xlio_subsystem_init(void)
{
	if (spdk_xlio_init()) {
		spdk_subsystem_init_next(-1);
		return;
	}

	spdk_subsystem_init_next(0);
}

static void
xlio_subsystem_fini(void)
{
	spdk_xlio_fini();
	spdk_subsystem_fini_next();
}

static struct spdk_subsystem g_spdk_subsystem_xlio = {
	.name = "xlio",
	.init = xlio_subsystem_init,
	.fini = xlio_subsystem_fini,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_xlio);
