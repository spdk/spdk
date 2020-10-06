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

#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"

#define SPDK_DEBUG_APP_CFG(...) SPDK_DEBUGLOG(app_config, __VA_ARGS__)

/* JSON configuration format is as follows
 *
 * {
 *  "subsystems" : [                          <<== *subsystems JSON array
 *    {                                       <<== *subsystems_it array entry pointer (iterator)
 *      "subsystem": "<< SUBSYSTEM NAME >>",
 *      "config": [                           <<== *config JSON array
 *         {                                  <<== *config_it array entry pointer (iterator)
 *           "method": "<< METHOD NAME >>",   <<== *method
 *           "params": { << PARAMS >> }       <<== *params
 *         },
 *         << MORE "config" ARRY ENTRIES >>
 *      ]
 *    },
 *    << MORE "subsystems" ARRAY ENTRIES >>
 *  ]
 *
 *  << ANYTHING ELSE IS IGNORRED IN ROOT OBJECT>>
 * }
 *
 */

struct load_json_config_ctx;
typedef void (*client_resp_handler)(struct load_json_config_ctx *,
				    struct spdk_jsonrpc_client_response *);

#define RPC_SOCKET_PATH_MAX sizeof(((struct sockaddr_un *)0)->sun_path)

/* 1s connections timeout */
#define RPC_CLIENT_CONNECT_TIMEOUT_US (1U * 1000U * 1000U)

/*
 * Currently there is no timeout in SPDK for any RPC command. This result that
 * we can't put a hard limit during configuration load as it most likely randomly fail.
 * So just print WARNLOG every 10s. */
#define RPC_CLIENT_REQUEST_TIMEOUT_US (10U * 1000 * 1000)

struct load_json_config_ctx {
	/* Thread used during configuration. */
	struct spdk_thread *thread;
	spdk_subsystem_init_fn cb_fn;
	void *cb_arg;
	bool stop_on_error;

	/* Current subsystem */
	struct spdk_json_val *subsystems; /* "subsystems" array */
	struct spdk_json_val *subsystems_it; /* current subsystem array position in "subsystems" array */

	struct spdk_json_val *subsystem_name; /* current subsystem name */

	/* Current "config" entry we are processing */
	struct spdk_json_val *config; /* "config" array */
	struct spdk_json_val *config_it; /* current config position in "config" array */

	/* Current request id we are sending. */
	uint32_t rpc_request_id;

	/* Whole configuration file read and parsed. */
	size_t json_data_size;
	char *json_data;

	size_t values_cnt;
	struct spdk_json_val *values;

	char rpc_socket_path_temp[RPC_SOCKET_PATH_MAX + 1];

	struct spdk_jsonrpc_client *client_conn;
	struct spdk_poller *client_conn_poller;

	client_resp_handler client_resp_cb;

	/* Timeout for current RPC client action. */
	uint64_t timeout;
};

static void app_json_config_load_subsystem(void *_ctx);

static void
app_json_config_load_done(struct load_json_config_ctx *ctx, int rc)
{
	spdk_poller_unregister(&ctx->client_conn_poller);
	if (ctx->client_conn != NULL) {
		spdk_jsonrpc_client_close(ctx->client_conn);
	}

	spdk_rpc_finish();

	SPDK_DEBUG_APP_CFG("Config load finished with rc %d\n", rc);
	ctx->cb_fn(rc, ctx->cb_arg);

	free(ctx->json_data);
	free(ctx->values);
	free(ctx);
}

static void
rpc_client_set_timeout(struct load_json_config_ctx *ctx, uint64_t timeout_us)
{
	ctx->timeout = spdk_get_ticks() + timeout_us * spdk_get_ticks_hz() / (1000 * 1000);
}

static int
rpc_client_check_timeout(struct load_json_config_ctx *ctx)
{
	if (ctx->timeout < spdk_get_ticks()) {
		SPDK_WARNLOG("RPC client command timeout.\n");
		return -ETIMEDOUT;
	}

	return 0;
}

struct json_write_buf {
	char data[1024];
	unsigned cur_off;
};

static int
json_write_stdout(void *cb_ctx, const void *data, size_t size)
{
	struct json_write_buf *buf = cb_ctx;
	size_t rc;

	rc = snprintf(buf->data + buf->cur_off, sizeof(buf->data) - buf->cur_off,
		      "%s", (const char *)data);
	if (rc > 0) {
		buf->cur_off += rc;
	}
	return rc == size ? 0 : -1;
}

static int
rpc_client_poller(void *arg)
{
	struct load_json_config_ctx *ctx = arg;
	struct spdk_jsonrpc_client_response *resp;
	client_resp_handler cb;
	int rc;

	assert(spdk_get_thread() == ctx->thread);

	rc = spdk_jsonrpc_client_poll(ctx->client_conn, 0);
	if (rc == 0) {
		rc = rpc_client_check_timeout(ctx);
		if (rc == -ETIMEDOUT) {
			rpc_client_set_timeout(ctx, RPC_CLIENT_REQUEST_TIMEOUT_US);
			rc = 0;
		}
	}

	if (rc == 0) {
		/* No response yet */
		return SPDK_POLLER_BUSY;
	} else if (rc < 0) {
		app_json_config_load_done(ctx, rc);
		return SPDK_POLLER_BUSY;
	}

	resp = spdk_jsonrpc_client_get_response(ctx->client_conn);
	assert(resp);

	if (resp->error) {
		struct json_write_buf buf = {};
		struct spdk_json_write_ctx *w = spdk_json_write_begin(json_write_stdout,
						&buf, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);

		if (w == NULL) {
			SPDK_ERRLOG("error response: (?)\n");
		} else {
			spdk_json_write_val(w, resp->error);
			spdk_json_write_end(w);
			SPDK_ERRLOG("error response: \n%s\n", buf.data);
		}
	}

	if (resp->error && ctx->stop_on_error) {
		spdk_jsonrpc_client_free_response(resp);
		app_json_config_load_done(ctx, -EINVAL);
	} else {
		/* We have response so we must have callback for it. */
		cb = ctx->client_resp_cb;
		assert(cb != NULL);

		/* Mark we are done with this handler. */
		ctx->client_resp_cb = NULL;
		cb(ctx, resp);
	}


	return SPDK_POLLER_BUSY;
}

static int
rpc_client_connect_poller(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	int rc;

	rc = spdk_jsonrpc_client_poll(ctx->client_conn, 0);
	if (rc != -ENOTCONN) {
		/* We are connected. Start regular poller and issue first request */
		spdk_poller_unregister(&ctx->client_conn_poller);
		ctx->client_conn_poller = SPDK_POLLER_REGISTER(rpc_client_poller, ctx, 100);
		app_json_config_load_subsystem(ctx);
	} else {
		rc = rpc_client_check_timeout(ctx);
		if (rc) {
			app_json_config_load_done(ctx, rc);
		}

		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static int
client_send_request(struct load_json_config_ctx *ctx, struct spdk_jsonrpc_client_request *request,
		    client_resp_handler client_resp_cb)
{
	int rc;

	assert(spdk_get_thread() == ctx->thread);

	ctx->client_resp_cb = client_resp_cb;
	rpc_client_set_timeout(ctx, RPC_CLIENT_REQUEST_TIMEOUT_US);
	rc = spdk_jsonrpc_client_send_request(ctx->client_conn, request);

	if (rc) {
		SPDK_DEBUG_APP_CFG("Sending request to client failed (%d)\n", rc);
	}

	return rc;
}

static int
cap_string(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_STRING) {
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}

static int
cap_object(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}


static int
cap_array_or_null(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_ARRAY_BEGIN && val->type != SPDK_JSON_VAL_NULL) {
		return -EINVAL;
	}

	*vptr = val;
	return 0;
}

struct config_entry {
	char *method;
	struct spdk_json_val *params;
};

static struct spdk_json_object_decoder jsonrpc_cmd_decoders[] = {
	{"method", offsetof(struct config_entry, method), spdk_json_decode_string},
	{"params", offsetof(struct config_entry, params), cap_object, true}
};

static void app_json_config_load_subsystem_config_entry(void *_ctx);

static void
app_json_config_load_subsystem_config_entry_next(struct load_json_config_ctx *ctx,
		struct spdk_jsonrpc_client_response *resp)
{
	/* Don't care about the response */
	spdk_jsonrpc_client_free_response(resp);

	ctx->config_it = spdk_json_next(ctx->config_it);
	app_json_config_load_subsystem_config_entry(ctx);
}

/* Load "config" entry */
static void
app_json_config_load_subsystem_config_entry(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	struct spdk_jsonrpc_client_request *rpc_request;
	struct spdk_json_write_ctx *w;
	struct config_entry cfg = {};
	struct spdk_json_val *params_end;
	size_t params_len = 0;
	int rc;

	if (ctx->config_it == NULL) {
		SPDK_DEBUG_APP_CFG("Subsystem '%.*s': configuration done.\n", ctx->subsystem_name->len,
				   (char *)ctx->subsystem_name->start);
		ctx->subsystems_it = spdk_json_next(ctx->subsystems_it);
		/* Invoke later to avoid recurrency */
		spdk_thread_send_msg(ctx->thread, app_json_config_load_subsystem, ctx);
		return;
	}

	if (spdk_json_decode_object(ctx->config_it, jsonrpc_cmd_decoders,
				    SPDK_COUNTOF(jsonrpc_cmd_decoders), &cfg)) {
		SPDK_ERRLOG("Failed to decode config entry\n");
		app_json_config_load_done(ctx, -EINVAL);
		goto out;
	}

	rc = spdk_rpc_is_method_allowed(cfg.method, spdk_rpc_get_state());
	if (rc == -EPERM) {
		SPDK_DEBUG_APP_CFG("Method '%s' not allowed -> skipping\n", cfg.method);
		/* Invoke later to avoid recurrency */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(ctx->thread, app_json_config_load_subsystem_config_entry, ctx);
		goto out;
	}

	SPDK_DEBUG_APP_CFG("\tmethod: %s\n", cfg.method);

	if (cfg.params) {
		/* Get _END by skipping params and going back by one element. */
		params_end = cfg.params + spdk_json_val_len(cfg.params) - 1;

		/* Need to add one character to include '}' */
		params_len = params_end->start - cfg.params->start + 1;

		SPDK_DEBUG_APP_CFG("\tparams: %.*s\n", (int)params_len, (char *)cfg.params->start);
	}

	rpc_request = spdk_jsonrpc_client_create_request();
	if (!rpc_request) {
		app_json_config_load_done(ctx, -errno);
		goto out;
	}

	w = spdk_jsonrpc_begin_request(rpc_request, ctx->rpc_request_id, NULL);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);
		app_json_config_load_done(ctx, -ENOMEM);
		goto out;
	}

	spdk_json_write_named_string(w, "method", cfg.method);

	if (cfg.params) {
		/* No need to parse "params". Just dump the whole content of "params"
		 * directly into the request and let the remote side verify it. */
		spdk_json_write_name(w, "params");
		spdk_json_write_val_raw(w, cfg.params->start, params_len);
	}

	spdk_jsonrpc_end_request(rpc_request, w);

	rc = client_send_request(ctx, rpc_request, app_json_config_load_subsystem_config_entry_next);
	if (rc != 0) {
		app_json_config_load_done(ctx, -rc);
		goto out;
	}
out:
	free(cfg.method);
}

static void
subsystem_init_done(int rc, void *arg1)
{
	struct load_json_config_ctx *ctx = arg1;

	if (rc) {
		app_json_config_load_done(ctx, rc);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	/* Another round. This time for RUNTIME methods */
	SPDK_DEBUG_APP_CFG("'framework_start_init' done - continuing configuration\n");

	assert(ctx != NULL);
	if (ctx->subsystems) {
		ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);
	}

	app_json_config_load_subsystem(ctx);
}

static struct spdk_json_object_decoder subsystem_decoders[] = {
	{"subsystem", offsetof(struct load_json_config_ctx, subsystem_name), cap_string},
	{"config", offsetof(struct load_json_config_ctx, config), cap_array_or_null}
};

/*
 * Start loading subsystem pointed by ctx->subsystems_it. This must point to the
 * beginning of the "subsystem" object in "subsystems" array or be NULL. If it is
 * NULL then no more subsystems to load.
 *
 * There are two iterations:
 *
 * In first iteration only STARTUP RPC methods are used, other methods are ignored. When
 * allsubsystems are walked the ctx->subsystems_it became NULL and "framework_start_init"
 * is called to let the SPDK move to RUNTIME state (initialize all subsystems) and
 * second iteration begins.
 *
 * In second iteration "subsystems" array is walked through again, this time only
 * RUNTIME RPC methods are used. When ctx->subsystems_it became NULL second time it
 * indicate that there is no more subsystems to load. The cb_fn is called to finish
 * configuration.
 */
static void
app_json_config_load_subsystem(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;

	if (ctx->subsystems_it == NULL) {
		if (spdk_rpc_get_state() == SPDK_RPC_STARTUP) {
			SPDK_DEBUG_APP_CFG("No more entries for current state, calling 'framework_start_init'\n");
			spdk_subsystem_init(subsystem_init_done, ctx);
		} else {
			app_json_config_load_done(ctx, 0);
		}

		return;
	}

	/* Capture subsystem name and config array */
	if (spdk_json_decode_object(ctx->subsystems_it, subsystem_decoders,
				    SPDK_COUNTOF(subsystem_decoders), ctx)) {
		SPDK_ERRLOG("Failed to parse subsystem configuration\n");
		app_json_config_load_done(ctx, -EINVAL);
		return;
	}

	SPDK_DEBUG_APP_CFG("Loading subsystem '%.*s' configuration\n", ctx->subsystem_name->len,
			   (char *)ctx->subsystem_name->start);

	/* Get 'config' array first configuration entry */
	ctx->config_it = spdk_json_array_first(ctx->config);
	app_json_config_load_subsystem_config_entry(ctx);
}

static void *
read_file(const char *filename, size_t *size)
{
	FILE *file = fopen(filename, "r");
	void *data;

	if (file == NULL) {
		/* errno is set by fopen */
		return NULL;
	}

	data = spdk_posix_file_load(file, size);
	fclose(file);
	return data;
}

static int
app_json_config_read(const char *config_file, struct load_json_config_ctx *ctx)
{
	struct spdk_json_val *values = NULL;
	void *json = NULL, *end;
	ssize_t values_cnt, rc;
	size_t json_size;

	json = read_file(config_file, &json_size);
	if (!json) {
		return -errno;
	}

	rc = spdk_json_parse(json, json_size, NULL, 0, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc < 0) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	values_cnt = rc;
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	if (values == NULL) {
		SPDK_ERRLOG("Out of memory\n");
		goto err;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc != values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	ctx->json_data = json;
	ctx->json_data_size = json_size;

	ctx->values = values;
	ctx->values_cnt = values_cnt;

	return 0;
err:
	free(json);
	free(values);
	return rc;
}

void
spdk_app_json_config_load(const char *json_config_file, const char *rpc_addr,
			  spdk_subsystem_init_fn cb_fn, void *cb_arg,
			  bool stop_on_error)
{
	struct load_json_config_ctx *ctx = calloc(1, sizeof(*ctx));
	int rc;

	assert(cb_fn);
	if (!ctx) {
		cb_fn(-ENOMEM, cb_arg);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->stop_on_error = stop_on_error;
	ctx->thread = spdk_get_thread();

	rc = app_json_config_read(json_config_file, ctx);
	if (rc) {
		goto fail;
	}

	/* Capture subsystems array */
	rc = spdk_json_find_array(ctx->values, "subsystems", NULL, &ctx->subsystems);
	if (rc) {
		SPDK_WARNLOG("No 'subsystems' key JSON configuration file.\n");
	} else {
		/* Get first subsystem */
		ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);
		if (ctx->subsystems_it == NULL) {
			SPDK_NOTICELOG("'subsystems' configuration is empty\n");
		}
	}

	/* If rpc_addr is not an Unix socket use default address as prefix. */
	if (rpc_addr == NULL || rpc_addr[0] != '/') {
		rpc_addr = SPDK_DEFAULT_RPC_ADDR;
	}

	/* FIXME: rpc client should use socketpair() instead of this temporary socket nonsense */
	rc = snprintf(ctx->rpc_socket_path_temp, sizeof(ctx->rpc_socket_path_temp), "%s.%d_config",
		      rpc_addr, getpid());
	if (rc >= (int)sizeof(ctx->rpc_socket_path_temp)) {
		SPDK_ERRLOG("Socket name create failed\n");
		goto fail;
	}

	/* FIXME: spdk_rpc_initialize() function should return error code. */
	spdk_rpc_initialize(ctx->rpc_socket_path_temp);
	ctx->client_conn = spdk_jsonrpc_client_connect(ctx->rpc_socket_path_temp, AF_UNIX);
	if (ctx->client_conn == NULL) {
		SPDK_ERRLOG("Failed to connect to '%s'\n", ctx->rpc_socket_path_temp);
		goto fail;
	}

	rpc_client_set_timeout(ctx, RPC_CLIENT_CONNECT_TIMEOUT_US);
	ctx->client_conn_poller = SPDK_POLLER_REGISTER(rpc_client_connect_poller, ctx, 100);
	return;

fail:
	app_json_config_load_done(ctx, -EINVAL);
}

SPDK_LOG_REGISTER_COMPONENT(app_config)
