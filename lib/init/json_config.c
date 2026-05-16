/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, 2023, 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/init.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

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
 *         << MORE "config" ARRAY ENTRIES >>
 *      ]
 *    },
 *    << MORE "subsystems" ARRAY ENTRIES >>
 *  ]
 *
 *  << ANYTHING ELSE IS IGNORED IN ROOT OBJECT>>
 * }
 *
 */

struct load_json_config_ctx;
typedef void (*client_resp_handler)(struct load_json_config_ctx *,
				    struct spdk_jsonrpc_client_response *);

#define RPC_SOCKET_PATH_MAX SPDK_SIZEOF_MEMBER(struct sockaddr_un, sun_path)

/* 1s connections timeout */
#define RPC_CLIENT_CONNECT_TIMEOUT_US (1U * 1000U * 1000U)

/*
 * Currently there is no timeout in SPDK for any RPC command. This result that
 * we can't put a hard limit during configuration load as it most likely randomly fail.
 * So just print WARNLOG every 10s. */
#define RPC_CLIENT_REQUEST_TIMEOUT_US (10U * 1000 * 1000)

struct load_json_config_ctx {
	/* Thread used during configuration. */
	spdk_subsystem_init_fn cb_fn;
	void *cb_arg;
	bool stop_on_error;

	/* Current subsystem */
	struct spdk_json_val *subsystems; /* "subsystems" array */
	struct spdk_json_val *subsystems_it; /* current subsystem array position in "subsystems" array */

	struct spdk_json_val *subsystem_name; /* current subsystem name */
	char subsystem_name_str[128];

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

	/* Signals that the code should follow deprecated path of execution. */
	bool initalize_subsystems;
};

static void app_json_config_load_subsystem(void *_ctx);

static void
app_json_config_load_done(struct load_json_config_ctx *ctx, int rc)
{
	spdk_poller_unregister(&ctx->client_conn_poller);
	if (ctx->client_conn != NULL) {
		spdk_jsonrpc_client_close(ctx->client_conn);
	}

	spdk_rpc_server_finish(ctx->rpc_socket_path_temp);

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

static void
log_rpc_error(struct spdk_json_val *error, const char *prefix)
{
	struct json_write_buf buf = {};
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(json_write_stdout, &buf, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (w == NULL) {
		SPDK_ERRLOG("%s: (?)\n", prefix);
	} else {
		spdk_json_write_val(w, error);
		spdk_json_write_end(w);
		SPDK_ERRLOG("%s:\n%s\n", prefix, buf.data);
	}
}

static int
rpc_client_poller(void *arg)
{
	struct load_json_config_ctx *ctx = arg;
	struct spdk_jsonrpc_client_response *resp;
	client_resp_handler cb;
	int rc;

	assert(spdk_thread_is_app_thread(NULL));

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
		log_rpc_error(resp->error, "error response");
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

	assert(spdk_thread_is_app_thread(NULL));

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

/* Context for batch element encoding callback */
struct batch_encode_ctx {
	struct spdk_jsonrpc_client_request *request;
	uint32_t count;
	uint32_t allowed_states;  /* Intersection of all methods' allowed states */
	bool stop_on_error;
	const char *subsystem_name;
};

static int
encode_batch_element(const struct spdk_json_val *val, void *out)
{
	struct batch_encode_ctx *enc_ctx = out;
	struct config_entry cfg = {};
	struct spdk_json_write_ctx *w;
	struct spdk_json_val *params_end;
	size_t params_len;
	uint32_t method_state_mask;
	int rc;

	if (val->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		SPDK_ERRLOG("Batch array element is not an object\n");
		return enc_ctx->stop_on_error ? -1 : 0;
	}

	if (spdk_json_decode_object(val, jsonrpc_cmd_decoders,
				    SPDK_COUNTOF(jsonrpc_cmd_decoders), &cfg)) {
		SPDK_ERRLOG("Failed to decode batch entry\n");
		free(cfg.method);
		return enc_ctx->stop_on_error ? -1 : 0;
	}

	/* Narrow down allowed states based on this method's requirements */
	method_state_mask = 0;
	rc = spdk_rpc_get_method_state_mask(cfg.method, &method_state_mask);
	if (rc == -ENOENT) {
		/* By default, skip this element, continue with others */
		rc = 0;
		if (enc_ctx->stop_on_error) {
			if (!spdk_subsystem_exists(enc_ctx->subsystem_name)) {
				SPDK_NOTICELOG("Skipping batch method '%s' because its subsystem '%s' "
					       "is not linked into this application.\n",
					       cfg.method, enc_ctx->subsystem_name);
			} else {
				rc = -1;
			}
		}
		free(cfg.method);
		return rc;
	}

	assert(rc == 0);
	enc_ctx->allowed_states &= method_state_mask;

	SPDK_DEBUG_APP_CFG("\tBatch[%u]: method=%s\n", enc_ctx->count, cfg.method);

	w = spdk_jsonrpc_begin_request(enc_ctx->request, -1, cfg.method);
	enc_ctx->count++;

	if (cfg.params) {
		params_end = cfg.params + spdk_json_val_len(cfg.params) - 1;
		params_len = params_end->start - cfg.params->start + 1;
		spdk_json_write_name(w, "params");
		spdk_json_write_val_raw(w, cfg.params->start, params_len);
	}

	spdk_jsonrpc_end_request(enc_ctx->request, w);
	free(cfg.method);

	return 0;
}

/*
 * Send an explicit batch array from the config file.
 * The batch_array points to the ARRAY_BEGIN token.
 * Returns 0 on success, positive to skip, negative error code on failure.
 */
static int
send_batch_array(struct load_json_config_ctx *ctx, struct spdk_json_val *batch_array)
{
	struct spdk_jsonrpc_client_request *request;
	struct batch_encode_ctx enc_ctx = { .allowed_states = SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME };
	size_t out_size;
	int rc;

	assert(batch_array->type == SPDK_JSON_VAL_ARRAY_BEGIN);

	/* Skip empty arrays - nothing to send */
	if (batch_array[1].type == SPDK_JSON_VAL_ARRAY_END) {
		return 1;
	}

	request = spdk_jsonrpc_client_create_request();
	if (!request) {
		return -ENOMEM;
	}

	rc = spdk_jsonrpc_begin_batch(request);
	if (rc != 0) {
		spdk_jsonrpc_client_free_request(request);
		return rc;
	}

	enc_ctx.request = request;
	enc_ctx.stop_on_error = ctx->stop_on_error;
	enc_ctx.subsystem_name = ctx->subsystem_name_str;

	/*
	 * Process batch elements using spdk_json_decode_array with stride=0.
	 * This keeps enc_ctx as the 'out' pointer for all iterations,
	 * effectively using it as a context rather than output storage.
	 * Each method's state_mask is ANDed with allowed_states to find
	 * the intersection of states where all methods can run.
	 */
	rc = spdk_json_decode_array(batch_array, encode_batch_element,
				    &enc_ctx, batch_array->len + 1, &out_size, 0);

	spdk_jsonrpc_end_batch(request);

	if (rc != 0 || enc_ctx.count == 0) {
		SPDK_ERRLOG("Empty or invalid batch array\n");
		spdk_jsonrpc_client_free_request(request);
		return -EINVAL;
	}

	if (enc_ctx.allowed_states == 0) {
		SPDK_ERRLOG("Batch contains methods with incompatible state requirements\n");
		spdk_jsonrpc_client_free_request(request);
		return -EINVAL;
	}

	if (spdk_rpc_get_state() == SPDK_RPC_STARTUP) {
		if (!(enc_ctx.allowed_states & SPDK_RPC_STARTUP)) {
			SPDK_DEBUG_APP_CFG("Batch requires RUNTIME state, deferring\n");
			spdk_jsonrpc_client_free_request(request);
			return 1;
		}
	} else if (enc_ctx.allowed_states & SPDK_RPC_STARTUP) {
		SPDK_DEBUG_APP_CFG("Batch already executed in STARTUP state, skipping\n");
		spdk_jsonrpc_client_free_request(request);
		return 1;
	}

	SPDK_DEBUG_APP_CFG("Sending batch of %u requests\n", enc_ctx.count);
	return client_send_request(ctx, request, app_json_config_load_subsystem_config_entry_next);
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
	uint32_t state_mask = 0, cur_state_mask, startup_runtime = SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME;
	int rc;

	if (ctx->config_it == NULL) {
		SPDK_DEBUG_APP_CFG("Subsystem '%.*s': configuration done.\n", ctx->subsystem_name->len,
				   (char *)ctx->subsystem_name->start);
		ctx->subsystems_it = spdk_json_next(ctx->subsystems_it);
		/* Invoke later to avoid recursion */
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem, ctx);
		return;
	}

	/*
	 * Check if this config entry is an array (explicit batch).
	 * Arrays in the config are sent as JSON-RPC batch requests.
	 */
	if (ctx->config_it->type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		SPDK_DEBUG_APP_CFG("Processing explicit batch array\n");
		rc = send_batch_array(ctx, ctx->config_it);
		if (rc < 0) {
			app_json_config_load_done(ctx, rc);
		} else if (rc > 0) {
			/* Batch methods not allowed in current state - skip for now */
			ctx->config_it = spdk_json_next(ctx->config_it);
			spdk_thread_send_msg(spdk_thread_get_app_thread(),
					     app_json_config_load_subsystem_config_entry, ctx);
		}
		return;
	}

	if (spdk_json_decode_object(ctx->config_it, jsonrpc_cmd_decoders,
				    SPDK_COUNTOF(jsonrpc_cmd_decoders), &cfg)) {
		SPDK_ERRLOG("Failed to decode config entry\n");
		app_json_config_load_done(ctx, -EINVAL);
		goto out;
	}

	rc = spdk_rpc_get_method_state_mask(cfg.method, &state_mask);
	if (rc == -ENOENT) {
		if (!ctx->stop_on_error) {
			/* Invoke later to avoid recursion */
			ctx->config_it = spdk_json_next(ctx->config_it);
			spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
					     ctx);
		} else if (!spdk_subsystem_exists(ctx->subsystem_name_str)) {
			/* If the subsystem does not exist, just skip it, even
			 * if we are supposed to stop_on_error. Users may generate
			 * a JSON config from one application, and want to use parts
			 * of it in another application that may not have all of the
			 * same subsystems linked - for example, nvmf_tgt => bdevperf.
			 * That's OK, we don't need to throw an error, since any nvmf
			 * configuration wouldn't be used by bdevperf anyways. That is
			 * different than if some subsystem does exist in bdevperf and
			 * one of its RPCs fails.
			 */
			SPDK_NOTICELOG("Skipping method '%s' because its subsystem '%s' "
				       "is not linked into this application.\n",
				       cfg.method, ctx->subsystem_name_str);
			/* Invoke later to avoid recursion */
			ctx->config_it = spdk_json_next(ctx->config_it);
			spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
					     ctx);
		} else {
			SPDK_ERRLOG("Method '%s' was not found\n", cfg.method);
			app_json_config_load_done(ctx, rc);
		}
		goto out;
	}
	cur_state_mask = spdk_rpc_get_state();
	if ((state_mask & cur_state_mask) != cur_state_mask) {
		SPDK_DEBUG_APP_CFG("Method '%s' not allowed -> skipping\n", cfg.method);
		/* Invoke later to avoid recursion */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
				     ctx);
		goto out;
	}
	if ((state_mask & startup_runtime) == startup_runtime && cur_state_mask == SPDK_RPC_RUNTIME) {
		/* Some methods are allowed to be run in both STARTUP and RUNTIME states.
		 * We should not call such methods twice, so ignore the second attempt in RUNTIME state */
		SPDK_DEBUG_APP_CFG("Method '%s' has already been run in STARTUP state\n", cfg.method);
		/* Invoke later to avoid recursion */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(spdk_thread_get_app_thread(), app_json_config_load_subsystem_config_entry,
				     ctx);
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
 * If "initalize_subsystems" is unset, then the function performs one iteration
 * and does not call subsystem initialization.
 *
 * There are two iterations, when "initalize_subsystems" context flag is set:
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
		if (ctx->initalize_subsystems && spdk_rpc_get_state() == SPDK_RPC_STARTUP) {
			SPDK_DEBUG_APP_CFG("No more entries for current state, calling 'framework_start_init'\n");
			spdk_subsystem_init(subsystem_init_done, ctx);
		} else {
			SPDK_DEBUG_APP_CFG("No more entries for current state\n");
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

	snprintf(ctx->subsystem_name_str, sizeof(ctx->subsystem_name_str),
		 "%.*s", ctx->subsystem_name->len, (char *)ctx->subsystem_name->start);

	SPDK_DEBUG_APP_CFG("Loading subsystem '%s' configuration\n", ctx->subsystem_name_str);

	/* Get 'config' array first configuration entry */
	ctx->config_it = spdk_json_array_first(ctx->config);
	app_json_config_load_subsystem_config_entry(ctx);
}

static int
parse_json(void *json, ssize_t json_size, struct load_json_config_ctx *ctx)
{
	void *end;
	ssize_t rc;

	if (!json || json_size <= 0) {
		SPDK_ERRLOG("JSON data cannot be empty\n");
		goto err;
	}

	ctx->json_data = calloc(1, json_size);
	if (!ctx->json_data) {
		goto err;
	}
	memcpy(ctx->json_data, json, json_size);
	ctx->json_data_size = json_size;

	rc = spdk_json_parse(ctx->json_data, ctx->json_data_size, NULL, 0, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc < 0) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	ctx->values_cnt = rc;
	ctx->values = calloc(ctx->values_cnt, sizeof(struct spdk_json_val));
	if (ctx->values == NULL) {
		SPDK_ERRLOG("Out of memory\n");
		goto err;
	}

	rc = spdk_json_parse(ctx->json_data, ctx->json_data_size, ctx->values,
			     ctx->values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if ((size_t)rc != ctx->values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	return 0;
err:
	free(ctx->values);
	return -EINVAL;
}

static void
json_config_prepare_ctx(spdk_subsystem_init_fn cb_fn, void *cb_arg, bool stop_on_error, void *json,
			ssize_t json_size, bool initalize_subsystems)
{
	struct load_json_config_ctx *ctx = calloc(1, sizeof(*ctx));
	int rc;

	if (!ctx) {
		cb_fn(-ENOMEM, cb_arg);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->stop_on_error = stop_on_error;
	ctx->initalize_subsystems = initalize_subsystems;

	rc = parse_json(json, json_size, ctx);
	if (rc < 0) {
		goto fail;
	}

	/* Capture subsystems array */
	rc = spdk_json_find_array(ctx->values, "subsystems", NULL, &ctx->subsystems);
	switch (rc) {
	case 0:
		/* Get first subsystem */
		ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);
		if (ctx->subsystems_it == NULL) {
			SPDK_NOTICELOG("'subsystems' configuration is empty\n");
		}
		break;
	case -EPROTOTYPE:
		SPDK_ERRLOG("Invalid JSON configuration: not enclosed in {}.\n");
		goto fail;
	case -ENOENT:
		SPDK_WARNLOG("No 'subsystems' key JSON configuration file.\n");
		break;
	case -EDOM:
		SPDK_ERRLOG("Invalid JSON configuration: 'subsystems' should be an array.\n");
		goto fail;
	default:
		SPDK_ERRLOG("Failed to parse JSON configuration.\n");
		goto fail;
	}

	/* FIXME: rpc client should use socketpair() instead of this temporary socket nonsense */
	rc = snprintf(ctx->rpc_socket_path_temp, sizeof(ctx->rpc_socket_path_temp),
		      "%s.%d_%"PRIu64"_config", SPDK_DEFAULT_RPC_ADDR, getpid(), spdk_get_ticks());
	if (rc >= (int)sizeof(ctx->rpc_socket_path_temp)) {
		SPDK_ERRLOG("Socket name create failed\n");
		goto fail;
	}

	rc = spdk_rpc_initialize(ctx->rpc_socket_path_temp, NULL);
	if (rc) {
		goto fail;
	}

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

void
spdk_subsystem_load_config(void *json, ssize_t json_size, spdk_subsystem_init_fn cb_fn,
			   void *cb_arg, bool stop_on_error)
{
	assert(cb_fn);
	assert(spdk_thread_is_app_thread(NULL));

	json_config_prepare_ctx(cb_fn, cb_arg, stop_on_error, json, json_size, false);
}

SPDK_LOG_REGISTER_COMPONENT(app_config)
