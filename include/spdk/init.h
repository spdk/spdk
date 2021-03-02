/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.  All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
