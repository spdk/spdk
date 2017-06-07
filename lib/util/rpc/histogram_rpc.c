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
#include "spdk/histogram.h"
#include "spdk/jsonrpc.h"
#include "spdk/json.h"

static void
hist_rpc_help(struct spdk_jsonrpc_server_conn *conn,
	      const struct spdk_json_val *params,
	      const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);
	spdk_json_write_string_asis(w, "usage : rpc.py hist_help           { /* histogram help */ }\n");
	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_list_ids       { /* Show briefly all histogram info */ }\n");

	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_clear id       { /* Clear histogram with id as input */ }\n");
	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_clear_all      { /* Clear all histogram */ }\n");

	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_disable id     { /* Disable histogram with id as input */ }\n");

	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_enable id      { /* Enable histogram with id as input */ }\n");

	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_show_all level { /* Show all histogram details with level as input */ }\n");
	spdk_json_write_string_asis(w,
				    "usage : rpc.py hist_show id level  { /* Show histogram Details with id level as input */ }\n");
	spdk_json_write_string_asis(w,
				    "\tid = histogram id\n\tlevel = information level details of histogram\n");
	spdk_json_write_string_asis(w,
				    "\t\t0 = histogram name (summary)\n\t\t1 = histogram name (details)\n");
	spdk_json_write_string_asis(w,
				    "\t\t2 = histogram name and tally summary\n\t\t3 = histogram name and bucket percentages\n");
	spdk_json_write_string_asis(w,
				    "\t\t4 = histogram name and tallies (details)\n\t\t5 = histogram name and tallies (full)\n");
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("hist_help", hist_rpc_help)

static void
hist_list_ids(struct spdk_jsonrpc_server_conn *conn,
	      const struct spdk_json_val *params,
	      const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);
	spdk_hist_list_ids(w);
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
}

SPDK_RPC_REGISTER("hist_list_ids", hist_list_ids)

struct spdk_hist_rpc_info {
	uint32_t hist_id;
	uint32_t level;
};

static const struct spdk_json_object_decoder spdk_hist_show_decoders[] = {
	{"hist_id", offsetof(struct spdk_hist_rpc_info, hist_id), spdk_json_decode_uint32},
	{"level",  offsetof(struct spdk_hist_rpc_info, level), spdk_json_decode_uint32},
};


static void
hist_rpc_show(struct spdk_jsonrpc_server_conn *conn,
	      const struct spdk_json_val *params,
	      const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_info req = {};
	if (spdk_json_decode_object(params, spdk_hist_show_decoders,
				    sizeof(spdk_hist_show_decoders) / sizeof(*spdk_hist_show_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);

	histogram *hg = spdk_histogram_find(req.hist_id);
	if (hg)
		spdk_histogram_show(hg, req.level, w);

	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid :
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_show", hist_rpc_show)

struct spdk_hist_rpc_id {
	uint32_t hist_id;
};

static const struct spdk_json_object_decoder spdk_hist_id_decoders[] = {
	{"hist_id", offsetof(struct spdk_hist_rpc_id, hist_id), spdk_json_decode_uint32},
};

static void
hist_rpc_enable(struct spdk_jsonrpc_server_conn *conn,
		const struct spdk_json_val *params,
		const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);
	spdk_json_write_uint32_asis(w, req.hist_id);
	histogram *hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		spdk_histogram_enable(hg);
		spdk_json_write_string_asis(w, " histogram enabled\n");
	} else spdk_json_write_string_asis(w, " histogram doesn't exist\n");
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid :
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_enable", hist_rpc_enable)

static void
hist_rpc_disable(struct spdk_jsonrpc_server_conn *conn,
		 const struct spdk_json_val *params,
		 const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);
	spdk_json_write_uint32_asis(w, req.hist_id);
	histogram *hg = spdk_histogram_find(req.hist_id);
	if (hg) {
		spdk_histogram_disable(hg);
		spdk_json_write_string_asis(w, " histogram disabled\n");
	} else spdk_json_write_string_asis(w, " histogram doesn't exist\n");
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid :
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_disable", hist_rpc_disable)

static void
hist_rpc_clear(struct spdk_jsonrpc_server_conn *conn,
	       const struct spdk_json_val *params,
	       const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_id req = {};
	if (spdk_json_decode_object(params, spdk_hist_id_decoders,
				    sizeof(spdk_hist_id_decoders) / sizeof(*spdk_hist_id_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}
	w = spdk_jsonrpc_begin_result(conn, id);
	histogram *hg = spdk_histogram_find(req.hist_id);
	spdk_json_write_quote(w);
	spdk_json_write_uint32_asis(w, req.hist_id);
	if (hg) {
		spdk_histogram_clear(hg);
		spdk_json_write_string_asis(w, " histogram content cleared\n");
	} else {
		spdk_json_write_string_asis(w, " histogram doesn't exist\n");
	}
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid :
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_clear", hist_rpc_clear)

static void
hist_rpc_clear_all(struct spdk_jsonrpc_server_conn *conn,
		   const struct spdk_json_val *params,
		   const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_histogram_clear_all();
	spdk_json_write_quote(w);
	spdk_json_write_string_asis(w, " All histogram are cleared\n");
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
}
SPDK_RPC_REGISTER("hist_clear_all", hist_rpc_clear_all)


struct spdk_hist_rpc_level {
	uint32_t level;
};

static const struct spdk_json_object_decoder spdk_hist_level_decoders[] = {
	{"level", offsetof(struct spdk_hist_rpc_level, level), spdk_json_decode_uint32},
};

static void
hist_rpc_show_all(struct spdk_jsonrpc_server_conn *conn,
		  const struct spdk_json_val *params,
		  const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_hist_rpc_level req = {};
	if (spdk_json_decode_object(params, spdk_hist_level_decoders,
				    sizeof(spdk_hist_level_decoders) / sizeof(*spdk_hist_level_decoders), &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_quote(w);
	spdk_histogram_show_all(req.level, w);
	spdk_json_write_quote(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid :
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("hist_show_all", hist_rpc_show_all)
