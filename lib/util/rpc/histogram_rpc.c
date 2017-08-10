/*-
 *   BSD LICENSE
 *
 *   Copyright (c) NetApp, Inc.
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
#include "spdk_internal/log.h"
#include "spdk/histogram_data.h"
#include "spdk/jsonrpc.h"
#include "spdk/json.h"

static void
hist_list_ids(struct spdk_jsonrpc_request *request,
	      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "hist_list_ids requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_hist_list_ids(w);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("hist_list_ids", hist_list_ids)

struct spdk_hist_rpc_id {
	uint32_t hist_id;
};

static const struct spdk_json_object_decoder spdk_hist_id_decoders[] = {
	{"hist_id", offsetof(struct spdk_hist_rpc_id, hist_id), spdk_json_decode_uint32},
};

static void
hist_rpc_enable(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	struct spdk_histogram_data *hg;

	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}


	hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_histogram_enable(hg);
		spdk_json_write_string_fmt(w, "histogram with ID %u enabled", req.hist_id);
		spdk_jsonrpc_end_result(request, w);
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "histogram with ID %u do not exist", req.hist_id);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_enable", hist_rpc_enable)

static void
hist_rpc_disable(struct spdk_jsonrpc_request *request,
		 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	struct spdk_histogram_data *hg;

	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}


	hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_histogram_disable(hg);
		spdk_json_write_string_fmt(w, "histogram with ID %u disabled", req.hist_id);
		spdk_jsonrpc_end_result(request, w);
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "histogram with ID %u doesn't exist", req.hist_id);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_disable", hist_rpc_disable)

static void
hist_rpc_clear(struct spdk_jsonrpc_request *request,
	       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	struct spdk_histogram_data *hg;

	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}


	hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_histogram_data_reset(hg);
		spdk_json_write_string_fmt(w, "histogram with ID %u content cleared", req.hist_id);
		spdk_jsonrpc_end_result(request, w);
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "histogram with ID %u doesn't exist", req.hist_id);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_clear", hist_rpc_clear)

static void
hist_rpc_clear_all(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "hist_clear_all requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_histogram_data_reset_all();
	spdk_json_write_string(w, "All histograms are cleared");
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("hist_clear_all", hist_rpc_clear_all)

static void
hist_rpc_get_stats(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	struct spdk_histogram_data *hg;

	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_histogram_dump_json(w, hg);
		spdk_jsonrpc_end_result(request, w);
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "histogram with ID %u doesn't exist\n", req.hist_id);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_get_stats", hist_rpc_get_stats)
