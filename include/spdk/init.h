/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 */

/**
 * \file
 * SPDK Initialization Helper
 */

#ifndef SPDK_INIT_H
#define SPDK_INIT_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_DEFAULT_RPC_ADDR "/var/tmp/spdk.sock"

/**
 * Create the SPDK JSON-RPC server and listen at the provided address. The RPC server is optional and is
 * independent of subsystem initialization. The RPC server can be started and stopped at any time.
 *
 * \param listen_addr Path to a unix domain socket to listen on
 *
 * \return Negated errno on failure. 0 on success.
 */
int spdk_rpc_initialize(const char *listen_addr);

/**
 * Shut down the SPDK JSON-RPC target
 */
void spdk_rpc_finish(void);

typedef void (*spdk_subsystem_init_fn)(int rc, void *ctx);

/**
 * Begin the initialization process for all SPDK subsystems. SPDK is divided into subsystems at a macro-level
 * and each subsystem automatically registers itself with this library at start up using a C
 * constructor. Further, each subsystem can declare other subsystems that it depends on.
 * Calling this function will correctly initialize all subsystems that are present, in the
 * required order.
 *
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn.
 */
void spdk_subsystem_init(spdk_subsystem_init_fn cb_fn, void *cb_arg);

/**
 * Like spdk_subsystem_init, but additionally configure each subsystem using the provided JSON config
 * file. This will automatically start a JSON RPC server and then stop it.
 *
 * \param json_config_file Path to a JSON config file.
 * \param rpc_addr Path to a unix domain socket to send configuration RPCs to.
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn.
 * \param stop_on_error Whether to stop initialization if one of the JSON RPCs fails.
 */
void spdk_subsystem_init_from_json_config(const char *json_config_file, const char *rpc_addr,
		spdk_subsystem_init_fn cb_fn, void *cb_arg,
		bool stop_on_error);

typedef void (*spdk_subsystem_fini_fn)(void *ctx);

/**
 * Tear down all of the subsystems in the correct order.
 *
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn
 */
void spdk_subsystem_fini(spdk_subsystem_fini_fn cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif
