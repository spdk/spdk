/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#include "scsi_internal.h"

#include "spdk/rpc.h"

static void
spdk_rpc_get_luns(struct spdk_jsonrpc_server_conn *conn,
		  const struct spdk_json_val *params,
		  const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_lun_db_entry *current;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_luns requires no parameters");
		return;
	}

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);

	current = spdk_scsi_lun_list_head;
	while (current != NULL) {
		struct spdk_scsi_lun *lun = current->lun;

		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "claimed");
		spdk_json_write_bool(w, current->claimed);
		spdk_json_write_name(w, "name");
		spdk_json_write_string(w, lun->name);
		spdk_json_write_object_end(w);

		current = current->next;
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("get_luns", spdk_rpc_get_luns)

struct rpc_delete_lun {
	char *name;
};

static void
free_rpc_delete_lun(struct rpc_delete_lun *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_lun_decoders[] = {
	{"name", offsetof(struct rpc_delete_lun, name), spdk_json_decode_string},
};

static void
spdk_rpc_delete_lun(struct spdk_jsonrpc_server_conn *conn,
		    const struct spdk_json_val *params,
		    const struct spdk_json_val *id)
{
	struct rpc_delete_lun req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_delete_lun_decoders,
				    sizeof(rpc_delete_lun_decoders) / sizeof(*rpc_delete_lun_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_scsi_lun_deletable(req.name)) {
		goto invalid;
	}

	spdk_scsi_lun_delete(req.name);
	free_rpc_delete_lun(&req);

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_lun(&req);
}
SPDK_RPC_REGISTER("delete_lun", spdk_rpc_delete_lun)

static void
spdk_rpc_get_scsi_devices(struct spdk_jsonrpc_server_conn *conn,
			  const struct spdk_json_val *params,
			  const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_scsi_dev *devs = spdk_scsi_dev_get_list();
	int i;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_scsi_devices requires no parameters");
		return;
	}

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		struct spdk_scsi_dev *dev = &devs[i];

		if (!dev->is_allocated) {
			continue;
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "id");
		spdk_json_write_int32(w, dev->id);

		spdk_json_write_name(w, "device_name");
		spdk_json_write_string(w, dev->name);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("get_scsi_devices", spdk_rpc_get_scsi_devices)
