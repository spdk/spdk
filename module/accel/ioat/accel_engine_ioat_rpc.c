/*-
 *   BSD LICENSE
 *
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

#include "accel_engine_ioat.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"

struct rpc_pci_whitelist {
	size_t num_bdfs;
	char *bdfs[IOAT_MAX_CHANNELS];
};

static int
decode_rpc_pci_whitelist(const struct spdk_json_val *val, void *out)
{
	struct rpc_pci_whitelist *pci_whitelist = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, pci_whitelist->bdfs,
				      IOAT_MAX_CHANNELS, &pci_whitelist->num_bdfs, sizeof(char *));
}

static void
free_rpc_pci_whitelist(struct rpc_pci_whitelist *list)
{
	size_t i;

	for (i = 0; i < list->num_bdfs; i++) {
		free(list->bdfs[i]);
	}
}

struct rpc_ioat_scan_accel_engine {
	struct rpc_pci_whitelist pci_whitelist;
};

static void
free_rpc_ioat_scan_accel_engine(struct rpc_ioat_scan_accel_engine *p)
{
	free_rpc_pci_whitelist(&p->pci_whitelist);
}

static const struct spdk_json_object_decoder rpc_ioat_scan_accel_engine_decoder[] = {
	{"pci_whitelist", offsetof(struct rpc_ioat_scan_accel_engine, pci_whitelist), decode_rpc_pci_whitelist},
};

static void
rpc_ioat_scan_accel_engine(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_ioat_scan_accel_engine req = {};
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_ioat_scan_accel_engine_decoder,
					    SPDK_COUNTOF(rpc_ioat_scan_accel_engine_decoder),
					    &req)) {
			free_rpc_ioat_scan_accel_engine(&req);
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}

		rc = accel_engine_ioat_add_whitelist_devices((const char **)req.pci_whitelist.bdfs,
				req.pci_whitelist.num_bdfs);
		free_rpc_ioat_scan_accel_engine(&req);
		if (rc < 0) {
			SPDK_ERRLOG("accel_engine_ioat_add_whitelist_devices() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	accel_engine_ioat_enable_probe();

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("ioat_scan_accel_engine", rpc_ioat_scan_accel_engine, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(ioat_scan_accel_engine, ioat_scan_copy_engine)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(ioat_scan_accel_engine, scan_ioat_copy_engine)
