/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/vmd.h"

#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk/log.h"
#include "event_vmd.h"

static void
rpc_vmd_enable(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	vmd_subsystem_enable();

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("vmd_enable", rpc_vmd_enable, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(vmd_enable, enable_vmd)
