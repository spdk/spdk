/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_RPC_CONFIG_H_
#define SPDK_RPC_CONFIG_H_

#include "spdk/stdinc.h"

#include "spdk/jsonrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Verify correctness of registered RPC methods and aliases.
 *
 * Incorrect registrations include:
 * - multiple RPC methods registered with the same name
 * - RPC alias registered with a method that does not exist
 * - RPC alias registered that points to another alias
 *
 * \return true if registrations are all correct, false otherwise
 */
bool spdk_rpc_verify_methods(void);

/**
 * Start listening for RPC connections.
 *
 * \param listen_addr Listening address.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_rpc_listen(const char *listen_addr);

/**
 * Poll the RPC server.
 */
void spdk_rpc_accept(void);

/**
 * Stop listening for RPC connections.
 */
void spdk_rpc_close(void);

/**
 * Function signature for RPC request handlers.
 *
 * \param request RPC request to handle.
 * \param params Parameters associated with the RPC request.
 */
typedef void (*spdk_rpc_method_handler)(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params);

/**
 * Register an RPC method.
 *
 * \param method Name for the registered method.
 * \param func Function registered for this method to handle the RPC request.
 * \param state_mask State mask of the registered method. If the bit of the state of
 * the RPC server is set in the state_mask, the method is allowed. Otherwise, it is rejected.
 */
void spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func,
			      uint32_t state_mask);

/**
 * Register a deprecated alias for an RPC method.
 *
 * \param method Name for the registered method.
 * \param alias Alias for the registered method.
 */
void spdk_rpc_register_alias_deprecated(const char *method, const char *alias);

/**
 * Check if \c method is allowed for \c state_mask
 *
 * \param method Method name
 * \param state_mask state mask to check against
 * \return 0 if method is allowed or negative error code:
 * -EPERM method is not allowed
 * -ENOENT method not found
 */
int spdk_rpc_is_method_allowed(const char *method, uint32_t state_mask);

/**
 * Return state mask of the method
 *
 * \param method Method name
 * \param[out] state_mask State mask of the method
 * \retval 0 if method is found and \b state_mask is filled
 * \retval -ENOENT if method is not found
 */
int spdk_rpc_get_method_state_mask(const char *method, uint32_t *state_mask);

#define SPDK_RPC_STARTUP	0x1
#define SPDK_RPC_RUNTIME	0x2

/* Give SPDK_RPC_REGISTER a higher execution priority than
 * SPDK_RPC_REGISTER_ALIAS_DEPRECATED to ensure all of the RPCs are registered
 * before we try registering any aliases.  Some older versions of clang may
 * otherwise execute the constructors in a different order than
 * defined in the source file (see issue #892).
 */
#define SPDK_RPC_REGISTER(method, func, state_mask) \
static void __attribute__((constructor(1000))) rpc_register_##func(void) \
{ \
	spdk_rpc_register_method(method, func, state_mask); \
}

#define SPDK_RPC_REGISTER_ALIAS_DEPRECATED(method, alias) \
static void __attribute__((constructor(1001))) rpc_register_##alias(void) \
{ \
	spdk_rpc_register_alias_deprecated(#method, #alias); \
}

/**
 * Set the state mask of the RPC server. Any RPC method whose state mask is
 * equal to the state of the RPC server is allowed.
 *
 * \param state_mask New state mask of the RPC server.
 */
void spdk_rpc_set_state(uint32_t state_mask);

/**
 * Get the current state of the RPC server.
 *
 * \return The current state of the RPC server.
 */
uint32_t spdk_rpc_get_state(void);

/*
 * Mark only the given RPC methods as allowed.
 *
 * \param rpc_allowlist string array of method names, terminated with a NULL.
 */
void spdk_rpc_set_allowlist(const char **rpc_allowlist);

#ifdef __cplusplus
}
#endif

#endif
