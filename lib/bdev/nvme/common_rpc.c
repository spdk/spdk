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
#include "spdk/util.h"
#include "spdk/rpc.h"

#include "spdk_internal/log.h"

#include "common.h"

static void
spdk_rpc_dump_nvme_controller_info(struct spdk_json_write_ctx *w,
                                  struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
       struct spdk_nvme_transport_id   *trid;

       trid = &nvme_bdev_ctrlr->trid;

       spdk_json_write_object_begin(w);
       spdk_json_write_named_string(w, "name", nvme_bdev_ctrlr->name);

       spdk_json_write_named_object_begin(w, "trid");
       spdk_bdev_nvme_dump_trid_json(trid, w);
       spdk_json_write_object_end(w);

       spdk_json_write_object_end(w);
}

struct rpc_get_nvme_controllers {
       char *name;
};

static void
free_rpc_get_nvme_controllers(struct rpc_get_nvme_controllers *r)
{
       free(r->name);
}

static const struct spdk_json_object_decoder rpc_get_nvme_controllers_decoders[] = {
       {"name", offsetof(struct rpc_get_nvme_controllers, name), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_nvme_controllers(struct spdk_jsonrpc_request *request,
                             const struct spdk_json_val *params)
{
       struct rpc_get_nvme_controllers req = {};
       struct spdk_json_write_ctx *w;
       struct nvme_bdev_ctrlr *ctrlr = NULL;

       if (params && spdk_json_decode_object(params, rpc_get_nvme_controllers_decoders,
                                             SPDK_COUNTOF(rpc_get_nvme_controllers_decoders),
                                             &req)) {
               SPDK_ERRLOG("spdk_json_decode_object failed\n");
               goto invalid;
       }

       if (req.name) {
               ctrlr = spdk_bdev_nvme_ctrlr_get_by_name(req.name);
               if (ctrlr == NULL) {
                       SPDK_ERRLOG("ctrlr '%s' does not exist\n", req.name);
                       goto invalid;
               }
       }

       free_rpc_get_nvme_controllers(&req);
       w = spdk_jsonrpc_begin_result(request);
       if (w == NULL) {
               return;
       }

       spdk_json_write_array_begin(w);

       if (ctrlr != NULL) {
               spdk_rpc_dump_nvme_controller_info(w, ctrlr);
       } else {
               for (ctrlr = spdk_bdev_nvme_first_ctrlr(); ctrlr; ctrlr = spdk_bdev_nvme_next_ctrlr(ctrlr))  {
                       spdk_rpc_dump_nvme_controller_info(w, ctrlr);
               }
       }

       spdk_json_write_array_end(w);

       spdk_jsonrpc_end_result(request, w);

       return;

invalid:
       spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

       free_rpc_get_nvme_controllers(&req);
}
SPDK_RPC_REGISTER("get_nvme_controllers", spdk_rpc_get_nvme_controllers, SPDK_RPC_RUNTIME)
