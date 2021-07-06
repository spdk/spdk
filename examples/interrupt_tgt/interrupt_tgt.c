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
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/vhost.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/env.h"

#include "spdk_internal/event.h"

struct rpc_reactor_set_interrupt_mode {
	int32_t lcore;
	bool disable_interrupt;
};

static const struct spdk_json_object_decoder rpc_reactor_set_interrupt_mode_decoders[] = {
	{"lcore", offsetof(struct rpc_reactor_set_interrupt_mode, lcore), spdk_json_decode_int32},
	{"disable_interrupt", offsetof(struct rpc_reactor_set_interrupt_mode, disable_interrupt), spdk_json_decode_bool},
};

static void
rpc_reactor_set_interrupt_mode_cb(void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	SPDK_NOTICELOG("complete reactor switch\n");

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_reactor_set_interrupt_mode(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_reactor_set_interrupt_mode req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_reactor_set_interrupt_mode_decoders,
				    SPDK_COUNTOF(rpc_reactor_set_interrupt_mode_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	if (!spdk_interrupt_mode_is_enabled()) {
		SPDK_ERRLOG("Interrupt mode is not set when staring the application\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}


	SPDK_NOTICELOG("RPC Start to %s interrupt mode on reactor %d.\n",
		       req.disable_interrupt ? "disable" : "enable", req.lcore);
	if (req.lcore >= (int64_t)spdk_env_get_first_core() &&
	    req.lcore <= (int64_t)spdk_env_get_last_core()) {
		rc = spdk_reactor_set_interrupt_mode(req.lcore, !req.disable_interrupt,
						     rpc_reactor_set_interrupt_mode_cb, request);
		if (rc)	{
			goto err;
		}
	} else {
		goto err;
	}

	return;

err:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
}
/* private */ SPDK_RPC_REGISTER("reactor_set_interrupt_mode", rpc_reactor_set_interrupt_mode,
				SPDK_RPC_RUNTIME)

static void
interrupt_tgt_usage(void)
{
	printf(" -E                        Set interrupt mode\n");
	printf(" -S <path>                 directory where to create vhost sockets (default: pwd)\n");
}

static int
interrupt_tgt_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'S':
		spdk_vhost_set_socket_path(arg);
		break;
	case 'E':
		spdk_interrupt_mode_enable();
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
interrupt_tgt_started(void *arg1)
{
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "interrupt_tgt";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "S:E", NULL,
				      interrupt_tgt_parse_arg, interrupt_tgt_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, interrupt_tgt_started, NULL);

	spdk_app_fini();

	return rc;
}
