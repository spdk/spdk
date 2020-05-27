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
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "bdev_ocssd.h"

#define BDEV_OCSSD_DEFAULT_NSID 1

struct rpc_error_injection_nvme_controller {
	char        *name;
	bool        admin;
	uint16_t    opcode;
	bool        do_not_submit;
	uint64_t    timeout_in_us;
	uint32_t    err_count;
	uint16_t    sct;
	uint16_t    sc;
	bool        info;
};

static const struct spdk_json_object_decoder rpc_error_injection_nvme_controller_decoders[] = {
	{"name", offsetof(struct rpc_error_injection_nvme_controller, name), spdk_json_decode_string, true},
	{"admin", offsetof(struct rpc_error_injection_nvme_controller, admin), spdk_json_decode_bool},
	{"opcode", offsetof(struct rpc_error_injection_nvme_controller, opcode), spdk_json_decode_uint16},
	{"do_not_submit", offsetof(struct rpc_error_injection_nvme_controller, do_not_submit), spdk_json_decode_bool},
	{"timeout_in_us", offsetof(struct rpc_error_injection_nvme_controller, timeout_in_us), spdk_json_decode_uint64, true},
	{"err_count", offsetof(struct rpc_error_injection_nvme_controller, err_count), spdk_json_decode_uint32, true},
	{"sct", offsetof(struct rpc_error_injection_nvme_controller, sct), spdk_json_decode_uint16},
	{"sc", offsetof(struct rpc_error_injection_nvme_controller, sc), spdk_json_decode_uint16},
	{"info", offsetof(struct rpc_error_injection_nvme_controller, info), spdk_json_decode_bool},
};

static void
free_rpc_error_injection_nvme_controller(struct rpc_error_injection_nvme_controller *rpc)
{
	free(rpc->name);
}

struct rpc_error_injection_nvme_controller_ctx {
	struct spdk_jsonrpc_request	*request;
	struct rpc_error_injection_nvme_controller  rpc;
	struct nvme_bdev_ctrlr		*nvme_bdev_ctrlr;
};

static void
rpc_error_injection_nvme_controller_done( struct nvme_bdev_ctrlr *ctrlr, void *_ctx, struct spdk_json_write_ctx *w)
{
	struct rpc_error_injection_nvme_controller_ctx *ctx = _ctx;

	if (ctx->rpc.info != true) {
	    spdk_json_write_bool(w, true);
	}
	else {
		int nsid, num_ns = ctrlr->num_ns;
		struct nvme_bdev_ns *ns = NULL;
		struct nvme_bdev *nvme_bdev, *tmp;

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "nvme_bdev_name", ctrlr->name);
		spdk_json_write_named_array_begin(w, "namespaces");
		for (nsid = 0; nsid < num_ns; nsid++) {
			ns = ctrlr->namespaces[nsid];
			if (ns != NULL) {
				if (!TAILQ_EMPTY(&ns->bdevs)) {
					TAILQ_FOREACH_SAFE(nvme_bdev, &ns->bdevs, tailq, tmp) {
						spdk_json_write_object_begin(w);
						spdk_bdev_dump_info_json( &nvme_bdev->disk, w);
						spdk_json_write_object_end(w);
					}
				}
			}
		}
		spdk_json_write_array_end(w);
        spdk_json_write_object_end(w);
	}
}

static int set_error_injection_for_nvme_controller( struct nvme_bdev_ctrlr *ctrlr,
		const struct rpc_error_injection_nvme_controller *rpc)
{
    int  rc = -ENOMEM;
    struct spdk_nvme_qpair	*qpair = NULL;

    if (rpc->admin == true) {
    	/* Admin error injection at submission path */
    	rc = spdk_nvme_qpair_add_cmd_error_injection( ctrlr->ctrlr, NULL,
    	         rpc->opcode, rpc->do_not_submit, rpc->timeout_in_us, rpc->err_count,
    	         rpc->sct, rpc->sc);
    }
    else {
    	qpair = spdk_nvme_ctrlr_alloc_io_qpair( ctrlr->ctrlr, NULL, 0);
    	if (qpair != NULL) {
    	    /* IO error injection at completion path */
    	    rc = spdk_nvme_qpair_add_cmd_error_injection( ctrlr->ctrlr, qpair,
    			     rpc->opcode, rpc->do_not_submit, rpc->timeout_in_us, rpc->err_count,
				     rpc->sct, rpc->sc);
        } else {
    	    rc = -ENOMEM;
        }
    }

    if (qpair != NULL) {
    	/* Free the queues */
        spdk_nvme_ctrlr_free_io_qpair(qpair);
    }

    return rc;
}

static void
get_feature_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	printf ( "%p: get features failed as expected, sct = %d, sc = %d\n",
			cb_arg, cpl->status.sct, cpl->status.sc);
}

static void get_feature_test( struct spdk_nvme_ctrlr *ctrlr )
{
	struct spdk_nvme_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;

	if (spdk_nvme_ctrlr_cmd_admin_raw( ctrlr, &cmd, NULL, 0,
					  get_feature_test_cb, ctrlr) != 0) {
		printf("Error: failed to send Get Features command for controller=%p\n", ctrlr);
	}
}

static int
error_injection_set( struct nvme_bdev_ctrlr *ctrlr, const struct rpc_error_injection_nvme_controller *rpc)
{
	int rc = set_error_injection_for_nvme_controller( ctrlr, rpc);

	if (rc == 0) {
		// Test code
		get_feature_test( ctrlr->ctrlr );
	}

	return rc;
}

static void
rpc_nvme_controllers_error_injection_set(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_error_injection_nvme_controller_ctx *ctx;
	int	rc;
	struct spdk_json_write_ctx *w = NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_error_injection_nvme_controller_decoders,
				    SPDK_COUNTOF(rpc_error_injection_nvme_controller_decoders),
				    &ctx->rpc)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to parse the request");
		goto out;
	}

	if (ctx->rpc.name != NULL) {
	    ctx->nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(ctx->rpc.name);
		if (ctx->nvme_bdev_ctrlr == NULL) {
			SPDK_ERRLOG("Failed at device lookup\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed at device lookup");
			goto out;
		}
	    rc = error_injection_set( ctx->nvme_bdev_ctrlr, &ctx->rpc);
	    if (rc == 0) {
	    	w = spdk_jsonrpc_begin_result(ctx->request);
		    rpc_error_injection_nvme_controller_done( ctx->nvme_bdev_ctrlr, ctx, w);
	    }
	    else {
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to inject an error");
			goto out;
	    }
	}
	else {
		struct nvme_bdev_ctrlr *ctrlr = NULL;

		// ignore 'info' flag if device name not specified
		ctx->rpc.info = false;
		rc = -EINVAL;
	    for (ctrlr = nvme_bdev_first_ctrlr(); ctrlr; ctrlr = nvme_bdev_next_ctrlr(ctrlr))  {
	    	rc = error_injection_set( ctrlr, &ctx->rpc);
	    	if (rc != 0) break;
	    }
	    if (rc == 0) {
	    	w = spdk_jsonrpc_begin_result(ctx->request);
	        rpc_error_injection_nvme_controller_done( nvme_bdev_first_ctrlr(), ctx, w);
	    }
	    else {
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to inject an error");
			goto out;
	    }
	}

out:
    if (w != NULL)
	    spdk_jsonrpc_end_result(ctx->request, w);
    free_rpc_error_injection_nvme_controller(&ctx->rpc);
	free(ctx);
}

SPDK_RPC_REGISTER("nvme_controllers_error_injection", rpc_nvme_controllers_error_injection_set, SPDK_RPC_RUNTIME)

