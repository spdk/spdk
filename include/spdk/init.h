/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.  All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * SPDK Initialization Helper
 */

#ifndef SPDK_INIT_H
#define SPDK_INIT_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_DEFAULT_RPC_ADDR "/var/tmp/spdk.sock"

/**
 * Structure with optional parameters for the JSON-RPC server initialization.
 */
struct spdk_rpc_opts {
	/* Size of this structure in bytes. */
	size_t size;
	/*
	 * A JSON-RPC log file pointer. The default value is NULL and used
	 * when options are omitted.
	 */
	FILE *log_file;
	/*
	 * JSON-RPC log level. Default value is SPDK_LOG_DISABLED and used
	 * when options are omitted.
	 */
	enum spdk_log_level log_level;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_rpc_opts) == 24, "Incorrect size");

/**
 * Create SPDK JSON-RPC server listening at provided address and start polling it for connections.
 *
 * The RPC server is optional and is independent of subsystem initialization.
 * The RPC server can be started and stopped at any time.
 *
 * \param listen_addr Path to a unix domain socket to listen on
 * \param opts Options for JSON-RPC server initialization. If NULL, default values are used.
 *
 * \return Negated errno on failure. 0 on success.
 */
int spdk_rpc_initialize(const char *listen_addr,
			const struct spdk_rpc_opts *opts);

/**
 * Stop SPDK JSON-RPC servers and stop polling for new connections on all addresses.
 */
void spdk_rpc_finish(void);

/**
 * Stop SPDK JSON-RPC server and stop polling for new connections on provided address.
 *
 * \param listen_addr Path to a unix domain socket.
 */
void spdk_rpc_server_finish(const char *listen_addr);

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
 * Deprecated - will be removed in 24.09 SPDK release. Please use
 * \ref spdk_subsystem_load_config.
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

/**
 * Loads RPC configuration from provided JSON for current RPC state. The function will
 * automatically start a JSON RPC server for configuration purposes and then stop it.
 * JSON data will be copied, so parsing will not disturb the original memory.
 *
 * \param json Raw JSON data.
 * \param json_size Size of JSON data.
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn.
 * \param stop_on_error Whether to stop initialization if one of the JSON RPCs fails.
 */
void spdk_subsystem_load_config(void *json, ssize_t json_size, spdk_subsystem_init_fn cb_fn,
				void *cb_arg, bool stop_on_error);

typedef void (*spdk_subsystem_fini_fn)(void *ctx);

/**
 * Tear down all of the subsystems in the correct order.
 *
 * \param cb_fn Function called when the process is complete.
 * \param cb_arg User context passed to cb_fn
 */
void spdk_subsystem_fini(spdk_subsystem_fini_fn cb_fn, void *cb_arg);

/**
 * Pause polling RPC server with given address.
 *
 * \param listen_addr Address, on which RPC server listens for connections.
 */
void spdk_rpc_server_pause(const char *listen_addr);

/**
 * Resume polling RPC server with given address.
 *
 * \param listen_addr Address, on which RPC server listens for connections.
 */
void spdk_rpc_server_resume(const char *listen_addr);

#ifdef __cplusplus
}
#endif

#endif
