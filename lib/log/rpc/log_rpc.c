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

#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct rpc_trace_flag {
	char *flag;
};

struct rpc_log_level {
	char *level;
};

static void
free_rpc_trace_flag(struct rpc_trace_flag *p)
{
	free(p->flag);
}

static void
free_rpc_log_level(struct rpc_log_level *p)
{
	free(p->level);
}

static const struct spdk_json_object_decoder rpc_trace_flag_decoders[] = {
	{"flag", offsetof(struct rpc_trace_flag, flag), spdk_json_decode_string},
};

static const struct spdk_json_object_decoder rpc_log_level_decoders[] = {
	{"level", offsetof(struct rpc_log_level, level), spdk_json_decode_string},
};

static int
_parse_log_level(char *level)
{
	if (!strcasecmp(level, "ERROR")) {
		return SPDK_LOG_ERROR;
	} else if (!strcasecmp(level, "WARNING")) {
		return SPDK_LOG_WARN;
	} else if (!strcasecmp(level, "NOTICE")) {
		return SPDK_LOG_NOTICE;
	} else if (!strcasecmp(level, "INFO")) {
		return SPDK_LOG_INFO;
	} else if (!strcasecmp(level, "DEBUG")) {
		return SPDK_LOG_DEBUG;
	}
	return -1;
}

static const char *
_get_log_level_name(int level)
{
	if (level == SPDK_LOG_ERROR) {
		return "ERROR";
	} else if (level == SPDK_LOG_WARN) {
		return "WARNING";
	} else if (level == SPDK_LOG_NOTICE) {
		return "NOTICE";
	} else if (level == SPDK_LOG_INFO) {
		return "INFO";
	} else if (level == SPDK_LOG_DEBUG) {
		return "DEBUG";
	}
	return NULL;
}

static void
spdk_rpc_set_log_print_level(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_log_level req = {};
	int level;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_log_level_decoders,
				    SPDK_COUNTOF(rpc_log_level_decoders), &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	level = _parse_log_level(req.level);
	if (level == -1) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "try to set invalid log level\n");
		goto invalid;
	}

	spdk_log_set_print_level(level);
	free_rpc_log_level(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_log_level(&req);
}
SPDK_RPC_REGISTER("set_log_print_level", spdk_rpc_set_log_print_level)

static void
spdk_rpc_get_log_print_level(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	int level;
	const char *name;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_trace_flags requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	level = spdk_log_get_print_level();
	name = _get_log_level_name(level);
	spdk_json_write_string(w, name);


	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_log_print_level", spdk_rpc_get_log_print_level)

static void
spdk_rpc_set_log_level(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_log_level req = {};
	int level;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_log_level_decoders,
				    SPDK_COUNTOF(rpc_log_level_decoders), &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	level = _parse_log_level(req.level);
	if (level == -1) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "try to set invalid log level\n");
		goto invalid;
	}


	spdk_log_set_level(level);
	free_rpc_log_level(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_log_level(&req);
}
SPDK_RPC_REGISTER("set_log_level", spdk_rpc_set_log_level)

static void
spdk_rpc_get_log_level(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	int level;
	const char *name;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_trace_flags requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	level = spdk_log_get_level();
	name = _get_log_level_name(level);
	spdk_json_write_string(w, name);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_log_level", spdk_rpc_get_log_level)

static void
spdk_rpc_set_trace_flag(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_trace_flag req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_trace_flag_decoders,
				    SPDK_COUNTOF(rpc_trace_flag_decoders), &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.flag == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "flag was 0\n");
		goto invalid;
	}

	spdk_log_set_trace_flag(req.flag);
	free_rpc_trace_flag(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_trace_flag(&req);
}
SPDK_RPC_REGISTER("set_trace_flag", spdk_rpc_set_trace_flag)

static void
spdk_rpc_clear_trace_flag(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_trace_flag req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_trace_flag_decoders,
				    SPDK_COUNTOF(rpc_trace_flag_decoders), &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.flag == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_LOG, "flag was 0\n");
		goto invalid;
	}

	spdk_log_clear_trace_flag(req.flag);
	free_rpc_trace_flag(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_trace_flag(&req);
}
SPDK_RPC_REGISTER("clear_trace_flag", spdk_rpc_clear_trace_flag)

static void
spdk_rpc_get_trace_flags(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_trace_flag *flag;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_trace_flags requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	flag = spdk_log_get_first_trace_flag();
	while (flag) {
		spdk_json_write_name(w, flag->name);
		spdk_json_write_bool(w, flag->enabled);
		flag = spdk_log_get_next_trace_flag(flag);
	}
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_trace_flags", spdk_rpc_get_trace_flags)
