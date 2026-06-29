/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"

#include "spdk_internal/init.h"
#include "spdk/log.h"
#include "spdk/hot_upgrade.h"
#include "spdk/hot_upgrade_shared.h"
#include "spdk/env.h"

static void
bdev_initialize_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
bdev_subsystem_initialize(void)
{
	spdk_bdev_initialize(bdev_initialize_complete, NULL);
}

static void
bdev_subsystem_finish_done(void *cb_arg)
{
	spdk_subsystem_fini_next();
}

static void
bdev_subsystem_finish(void)
{
	spdk_bdev_finish(bdev_subsystem_finish_done, NULL);
}

static void
bdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_bdev_subsystem_config_json(w);
}

/* Hot upgrade callbacks */
static void
bdev_primary_drain_io(void *arg)
{ spdk_subsystem_primary_drain_io_next(0); }

static void
bdev_primary_suspend(void *arg)
{
	struct spdk_hot_upgrade_shared_state *loaded;
	struct spdk_hot_upgrade_shared_state local_state;

	/*
	 * state_save() uses O_TRUNC which zeros the mmap'd region returned
	 * by state_load(). So we must copy to a local struct, modify it,
	 * and save the local copy — never save the mmap'd pointer directly.
	 */
	if (spdk_hot_upgrade_state_load(&loaded) == 0) {
		memcpy(&local_state, loaded, sizeof(local_state));

		local_state.bdevs_first = spdk_bdev_hu_get_bdevs_first();
		local_state.bdevs_last = spdk_bdev_hu_get_bdevs_last();
		spdk_bdev_hu_save_bdev_infos(local_state.bdev_infos,
					     SPDK_HU_MAX_BDEVS,
					     &local_state.num_bdev_infos);
		spdk_hot_upgrade_state_save(&local_state);
		SPDK_NOTICELOG("HU: bdev_primary_suspend saved %u bdevs "
			       "first=%p last=%p\n",
			       local_state.num_bdev_infos,
			       (void *)local_state.bdevs_first,
			       (void *)local_state.bdevs_last);
	} else {
		SPDK_ERRLOG("HU: bdev_primary_suspend failed to load state\n");
	}
	spdk_subsystem_primary_suspend_next(0);
}

static void
bdev_secondary_pre_init(void *arg)
{
	struct spdk_hot_upgrade_shared_state *state;

	if (spdk_hot_upgrade_state_load(&state) == 0 && state->bdev_mgr_addr != 0) {
		SPDK_NOTICELOG("bdev: g_bdev_mgr at %p via shared state\n",
			       (void *)state->bdev_mgr_addr);
	}
	spdk_subsystem_secondary_pre_init_next(0);
}

static void
bdev_secondary_takeover(void *arg)
{
	struct spdk_hot_upgrade_shared_state *state;

	if (spdk_hot_upgrade_state_load(&state) == 0) {
		spdk_bdev_hu_set_bdevs(state->bdevs_first, state->bdevs_last);
		spdk_bdev_hu_fixup_inherited_bdevs(state->bdev_infos,
						  state->num_bdev_infos);
		SPDK_NOTICELOG("HU: bdev_secondary_takeover restored %u bdevs\n",
			       state->num_bdev_infos);
	} else {
		SPDK_ERRLOG("HU: bdev_secondary_takeover failed to load state\n");
	}
	spdk_subsystem_secondary_takeover_next(0);
}


static struct spdk_subsystem g_spdk_subsystem_bdev = {
	.name = "bdev",
	.init = bdev_subsystem_initialize,
	.fini = bdev_subsystem_finish,
	.write_config_json = bdev_subsystem_config_json,
	.primary_drain_io = bdev_primary_drain_io,
	.primary_suspend = bdev_primary_suspend,
	.secondary_pre_init = bdev_secondary_pre_init,
	.secondary_takeover = bdev_secondary_takeover,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_bdev);
SPDK_SUBSYSTEM_DEPEND(bdev, accel)
SPDK_SUBSYSTEM_DEPEND(bdev, vmd)
SPDK_SUBSYSTEM_DEPEND(bdev, sock)
SPDK_SUBSYSTEM_DEPEND(bdev, iobuf)
