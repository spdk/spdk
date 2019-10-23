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

#include "spdk/stdinc.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/nvme.h"

#include "spdk_internal/log.h"

#include "nvme_internal.h"
#include "nvme_cuse.h"

struct rpc_nvme_cuse_register {
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
};

static void
free_rpc_nvme_cuse_register(struct rpc_nvme_cuse_register *req)
{
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->subnqn);
}

static const struct spdk_json_object_decoder rpc_nvme_cuse_register_decoders[] = {
	{"trtype", offsetof(struct rpc_nvme_cuse_register, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_nvme_cuse_register, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_nvme_cuse_register, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_nvme_cuse_register, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_nvme_cuse_register, subnqn), spdk_json_decode_string, true},
};

static void
spdk_rpc_nvme_cuse_register(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_nvme_cuse_register req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	struct spdk_nvme_transport_id trid = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_nvme_cuse_register_decoders,
				    SPDK_COUNTOF(rpc_nvme_cuse_register_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     req.trtype);
		goto cleanup;
	}

	/* Parse traddr */
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", req.traddr);

	/* Parse adrfam */
	if (req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     req.adrfam);
			goto cleanup;
		}
	}

	/* Parse trsvcid */
	if (req.trsvcid) {
		snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", req.trsvcid);
	}

	/* Parse subnqn */
	if (req.subnqn) {
		snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", req.subnqn);
	}

	ctrlr = spdk_nvme_get_ctrlr_by_trid_unsafe(&trid);
	if (!ctrlr) {
		SPDK_ERRLOG("No such controller\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = nvme_cuse_register(ctrlr);
	if (rc) {
		SPDK_ERRLOG("Failed to register CUSE devices\n");
		spdk_jsonrpc_send_error_response(request, -rc, spdk_strerror(rc));
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_nvme_cuse_register(&req);
}
SPDK_RPC_REGISTER("nvme_cuse_register", spdk_rpc_nvme_cuse_register, SPDK_RPC_RUNTIME)
