/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "scsi_internal.h"

#include "spdk/rpc.h"
#include "spdk/util.h"

static void
rpc_scsi_get_devices(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_scsi_dev *devs = scsi_dev_get_list();
	int i;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "scsi_get_devices requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		struct spdk_scsi_dev *dev = &devs[i];

		if (!dev->is_allocated) {
			continue;
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_named_int32(w, "id", dev->id);

		spdk_json_write_named_string(w, "device_name", dev->name);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("scsi_get_devices", rpc_scsi_get_devices, SPDK_RPC_RUNTIME)
