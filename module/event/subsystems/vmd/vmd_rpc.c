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
	int rc;

	rc = vmd_subsystem_init();

	spdk_jsonrpc_send_bool_response(request, rc == 0);
}

SPDK_RPC_REGISTER("enable_vmd", rpc_vmd_enable, SPDK_RPC_STARTUP)
