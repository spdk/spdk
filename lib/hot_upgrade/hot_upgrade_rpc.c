/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPDK Hot Upgrade Contributors.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/hot_upgrade.h"
#include "spdk/hot_upgrade_shared.h"
#include "spdk/jsonrpc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/init.h"
#include "spdk/event.h"
#include "spdk/thread.h"

#include <unistd.h>

struct rpc_primary_exit_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_thread *thread;
};

/* Prevent stale reactor events from re-entering after ctx is freed */
static bool g_suspend_in_progress = false;

static void
primary_suspend_done(void *arg)
{
	struct rpc_primary_exit_ctx *ctx = arg;
	struct spdk_json_write_ctx *w;

	if (g_suspend_in_progress) {
		return; /* stale event guard */
	}
	g_suspend_in_progress = true;

	spdk_hot_upgrade_set_state(SPDK_HU_PRIMARY_SUSPENDED);

	w = spdk_jsonrpc_begin_result(ctx->request);
	if (w == NULL) {
		free(ctx);
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "status", "primary_suspended");
	spdk_json_write_named_uint32(w, "pid", getpid());
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	/*
	 * === Memory barrier before suspending ===
	 * Ensure all shared memory writes (global pointers, conn state, mem
	 * regions) are globally visible to Secondary before we suspend.
	 */
	__sync_synchronize();

	/*
	 * Transition reactor to HU_PAUSED state. The reactor loop continues
	 * running but only polls the app thread (RPC), skipping IO threads.
	 * This allows rpc_primary_resume to work via RPC without external SIGUSR1.
	 */
	spdk_reactor_hu_pause();
	SPDK_NOTICELOG("Primary process entering HU_PAUSED state (RPC still active)\n");

	g_suspend_in_progress = false;
}

/* Prevent stale reactor events from re-entering drain_done */
static bool g_hu_drain_completed = false;

static void
primary_drain_io_done(void *arg)
{
	struct rpc_primary_exit_ctx *ctx = arg;
	struct spdk_hot_upgrade_shared_state state;
	int rc;

	if (g_hu_drain_completed) {
		return; /* stale event after SIGUSR1 wake, ignore */
	}
	g_hu_drain_completed = true;

	/* Save shared state before suspending */
	memset(&state, 0, sizeof(state));
	state.magic = SPDK_HU_STATE_MAGIC;
	state.version = SPDK_HU_STATE_VERSION;
	state.primary_pid = getpid();

	/* Populate DPDK environment fields */
	state.base_virtaddr = spdk_app_get_base_virtaddr();
	state.shm_id = spdk_app_get_shm_id();
	spdk_cpuset_copy(&state.core_mask, spdk_app_get_core_mask());

	/* IPC socket path */
	strncpy(state.ipc_sock_path, SPDK_HU_IPC_SOCK_PATH,
		sizeof(state.ipc_sock_path) - 1);

	/* RPC addresses */
	const char *rpc_addr = spdk_app_get_rpc_addr();
	if (rpc_addr) {
		strncpy(state.primary_rpc_addr, rpc_addr,
			sizeof(state.primary_rpc_addr) - 1);
		/* Construct secondary RPC address from primary's */
		snprintf(state.secondary_rpc_addr, sizeof(state.secondary_rpc_addr),
			 "%s_secondary", rpc_addr);
	}

	/*
	 * Subsystem pointer fields (bdev_mgr_addr, vhost_devices_root, etc.)
	 * will be populated by subsystem-specific primary_suspend callbacks
	 * in a future iteration. For POC, they remain zero.
	 */

	rc = spdk_hot_upgrade_state_save(&state);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to save shared state: %d\n", rc);
		spdk_hot_upgrade_set_state(SPDK_HU_FAILED);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to save state");
		free(ctx);
		return;
	}

	/*
	 * Call spdk_subsystem_primary_suspend() to invoke each subsystem's
	 * primary_suspend callback (bdev, vhost, etc.). These callbacks save
	 * subsystem-specific state (e.g. bdev TAILQ heads) to the shared state
	 * file. When all subsystems have suspended, primary_suspend_done is called.
	 *
	 * g_hu_fini_cb_fn was already cleared to NULL by primary_drain_io_next
	 * when drain traversal completed, so there is no stale callback issue.
	 */
		/* Create IPC listen socket for Secondary to connect during pre_init/takeover */
		rc = spdk_hot_upgrade_create_ipc_sock();
		if (rc < 0) {
			SPDK_ERRLOG("Failed to create IPC socket: %d\n", rc);
			spdk_hot_upgrade_set_state(SPDK_HU_FAILED);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Failed to create IPC socket");
			free(ctx);
			return;
		}

	spdk_subsystem_primary_suspend(primary_suspend_done, ctx);
}

static void
rpc_primary_exit(struct spdk_jsonrpc_request *request,
		 const struct spdk_json_val *params)
{
	struct rpc_primary_exit_ctx *ctx;

	if (spdk_hot_upgrade_get_state() != SPDK_HU_IDLE) {
		SPDK_ERRLOG("Cannot primary_exit: current state is %s\n",
			    spdk_hot_upgrade_state_str(spdk_hot_upgrade_get_state()));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_STATE,
						 "Not in IDLE state");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;
	ctx->thread = spdk_get_thread();
	g_hu_drain_completed = false;
	spdk_hot_upgrade_set_state(SPDK_HU_PRIMARY_DRAINING);

	/* Start draining I/O across all subsystems */
	/* Note: P5-02 IO drain timeout not yet implemented in POC.
	 * In production, use spdk_thread_send_msg with delay for cancelable timeout. */
	spdk_subsystem_primary_drain_io(primary_drain_io_done, ctx);
}
SPDK_RPC_REGISTER("primary_exit", rpc_primary_exit, SPDK_RPC_RUNTIME)

static void
rpc_secondary_init(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (spdk_hot_upgrade_get_state() != SPDK_HU_SECONDARY_PRE_INIT_DONE) {
		SPDK_ERRLOG("Cannot secondary_init: current state is %s\n",
			    spdk_hot_upgrade_state_str(spdk_hot_upgrade_get_state()));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_STATE,
						 "Not in SECONDARY_PRE_INIT_DONE state");
		return;
	}

	spdk_hot_upgrade_set_state(SPDK_HU_SECONDARY_TAKEOVER);

	/*
	 * Call spdk_subsystem_secondary_takeover() to invoke each subsystem's
	 * secondary_takeover callback (bdev, vhost, etc.). These callbacks
	 * restore subsystem-specific state (e.g. bdev TAILQ heads, pointer
	 * fixup) from the shared state file. The callbacks are synchronous
	 * and complete before this function returns.
	 */
	spdk_subsystem_secondary_takeover(NULL, NULL);

	/*
	 * Call spdk_app_secondary_full_init()
	 * 1. Receive FDs from Primary via IPC socket
	 * 2. Restore Guest memory mappings
	 * 3. Restore DPDK rte_vhost connection state
	 * 4. Inject FDs into Reactor epoll
	 * 5. Register vhost pollers
	 * 6. Rebuild I/O channels
	 */
	spdk_app_secondary_full_init(NULL, NULL);

	spdk_hot_upgrade_set_state(SPDK_HU_COMPLETE);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "status", "secondary_active");
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("secondary_init", rpc_secondary_init, SPDK_RPC_RUNTIME)

static void
rpc_hot_upgrade_status(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "state",
				     spdk_hot_upgrade_state_str(spdk_hot_upgrade_get_state()));
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("hot_upgrade_status", rpc_hot_upgrade_status, SPDK_RPC_RUNTIME)

static void
rpc_primary_resume(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (spdk_hot_upgrade_get_state() != SPDK_HU_PRIMARY_SUSPENDED) {
		SPDK_ERRLOG("Cannot primary_resume: current state is %s\n",
			    spdk_hot_upgrade_state_str(spdk_hot_upgrade_get_state()));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_STATE,
						 "Not in PRIMARY_SUSPENDED state");
		return;
	}

	/* Restore hot upgrade state */
	spdk_hot_upgrade_set_state(SPDK_HU_IDLE);

	/* Restore reactor to RUNNING — resumes all IO threads and pollers */
	spdk_reactor_hu_resume();

	SPDK_NOTICELOG("Primary process resumed, reactor back to RUNNING\n");

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "status", "primary_resumed");
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("primary_resume", rpc_primary_resume, SPDK_RPC_RUNTIME)