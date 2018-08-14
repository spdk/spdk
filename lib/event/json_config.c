#include "json_config.h"

#include "spdk/stdinc.h"

#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/event.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#define SPDK_DEBUG_APP_CFG(...) SPDK_DEBUGLOG(SPDK_LOG_APP_CONFIG, __VA_ARGS__)

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

struct load_json_config_ctx {
	/* Thread used during configuration */
	struct spdk_thread *thread;
	struct spdk_event *done_event;

	/* Current subsystem */
	struct spdk_json_val *subsystems; /* "subsystems" array */
	struct spdk_json_val *subsystems_it; /* current subsystem array entry */

	struct spdk_json_val *subsystem_name; /* current subsystem name */

	/* Current "config" entry we are processing */
	struct spdk_json_val *config; /* "config" array */
	struct spdk_json_val *config_it; /* current config entry entry */

	/* Current request we are sending */
	uint32_t rpc_request_id;

	/* Whole configuration file read and parsed */
	size_t json_data_size;
	char *json_data;

	size_t values_cnt;
	struct spdk_json_val *values;

	char rpc_socket_path_temp[108 + 1];

	struct spdk_jsonrpc_client *client_conn;
	struct spdk_poller *client_conn_poller;

	client_resp_handler client_resp_cb;
};

static void spdk_app_json_config_load_subsystem(void *_ctx);

static void
spdk_app_json_config_load_done(struct load_json_config_ctx *ctx, int rc)
{
	spdk_poller_unregister(&ctx->client_conn_poller);
	spdk_jsonrpc_client_close(ctx->client_conn);
	spdk_rpc_finish();

	if (rc) {
		SPDK_ERRLOG("Config load failed. Stopping SPDK application.\n");
		spdk_app_stop(rc);
	} else {
		spdk_event_call(ctx->done_event);
	}

	SPDK_DEBUG_APP_CFG("Config load finished\n");
	free(ctx->json_data);
	free(ctx->values);
	free(ctx);
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
		/* FIXME: add timeout */
		/* No response yet */
		return -1;
	}

	if (rc < 0) {
		spdk_app_json_config_load_done(ctx, rc);
		return -1;
	}

	resp = spdk_jsonrpc_client_get_response(ctx->client_conn);
	assert(resp);

	if (resp->error) {
		SPDK_ERRLOG("error response: %*s", (int)resp->error->len, (char *)resp->error->start);
		spdk_jsonrpc_client_free_response(resp);
		spdk_app_json_config_load_done(ctx, -EINVAL);
	} else {
		/* We have response so we must have callback for it. */
		cb = ctx->client_resp_cb;
		if (cb == NULL) {
			assert(false);
			/* What error to return? */
			spdk_app_json_config_load_done(ctx, -EPERM);
			return -1;
		}

		/* Mark we are done with this handler. */
		ctx->client_resp_cb = NULL;
		cb(ctx, resp);
	}


	return -1;
}

static int
rpc_client_connect_poller(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	int rc;

	rc = spdk_jsonrpc_client_poll(ctx->client_conn, 0);
	/* FIXME: Add connection timeout */
	if (rc != -ENOTCONN) {
		/* We are connected. Start regular poller and issue first request */
		spdk_poller_unregister(&ctx->client_conn_poller);
		ctx->client_conn_poller = spdk_poller_register(rpc_client_poller, ctx, 100);
		spdk_app_json_config_load_subsystem(ctx);
	}

	return -1;
}

static void
client_send_request(struct load_json_config_ctx *ctx, struct spdk_jsonrpc_client_request *request,
		    client_resp_handler client_resp_cb)
{
	assert(spdk_get_thread() == ctx->thread);

	ctx->client_resp_cb = client_resp_cb;
	spdk_jsonrpc_client_send_request(ctx->client_conn, request);
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

static void spdk_app_json_config_load_subsystem_config_entry(void *_ctx);

static void
spdk_app_json_config_load_subsystem_config_entry_next(struct load_json_config_ctx *ctx,
		struct spdk_jsonrpc_client_response *resp)
{
	/* Don't care about the response as long it is not
	 * an error (which is validated by poller) */
	spdk_jsonrpc_client_free_response(resp);

	ctx->config_it = spdk_json_next(ctx->config_it);
	spdk_app_json_config_load_subsystem_config_entry(ctx);
}

/* Load "cofnig" entry */
static void
spdk_app_json_config_load_subsystem_config_entry(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	struct spdk_jsonrpc_client_request *rpc_request;
	struct spdk_json_write_ctx *w;
	struct config_entry cfg = {};
	struct spdk_json_val *params_end;
	size_t params_len;
	int rc;

	if (ctx->config_it == NULL) {
		SPDK_DEBUG_APP_CFG("Subsystem '%.*s': configuration done.\n", ctx->subsystem_name->len,
				   (char *)ctx->subsystem_name->start);
		ctx->subsystems_it = spdk_json_next(ctx->subsystems_it);
		/* Invoke later to avoid recurency */
		spdk_thread_send_msg(ctx->thread, spdk_app_json_config_load_subsystem, ctx);
		return;
	}

	if (spdk_json_decode_object(ctx->config_it, jsonrpc_cmd_decoders,
				    SPDK_COUNTOF(jsonrpc_cmd_decoders), &cfg)) {
		params_end = spdk_json_next(ctx->config_it);
		params_len = params_end->start - ctx->config->start + 1;
		SPDK_ERRLOG("Failed to decode config entry: %*s!\n", (int)params_len, (char *)ctx->config_it);
		spdk_app_json_config_load_done(ctx, -EINVAL);
		goto out;
	}

	rc = spdk_rpc_is_method_allowed(cfg.method, spdk_rpc_get_state());
	if (rc == -EPERM) {
		SPDK_DEBUG_APP_CFG("Method '%s' not allowed -> skipping\n", cfg.method);
		/* Invoke later to avoid recurency */
		ctx->config_it = spdk_json_next(ctx->config_it);
		spdk_thread_send_msg(ctx->thread, spdk_app_json_config_load_subsystem_config_entry, ctx);
		goto out;
	}

	/* Ger _END by skipping params and going back by one element. */
	params_end = cfg.params + spdk_json_val_len(cfg.params) - 1;

	/* Need to add one character to include '}' */
	params_len = params_end->start - cfg.params->start + 1;

	SPDK_DEBUG_APP_CFG("\tmethod: %s\n", cfg.method);
	SPDK_DEBUG_APP_CFG("\tparams: %.*s\n", (int)params_len, (char *)cfg.params->start);

	rpc_request = spdk_jsonrpc_client_create_request();
	if (!rpc_request) {
		spdk_app_json_config_load_done(ctx, -errno);
		goto out;
	}

	w = spdk_jsonrpc_begin_request(rpc_request, ctx->rpc_request_id, NULL);
	if (!w) {
		spdk_jsonrpc_client_free_request(rpc_request);
		spdk_app_json_config_load_done(ctx, -ENOMEM);
		goto out;
	}

	spdk_json_write_named_string(w, "method", cfg.method);

	/* No need to parse "params". Just dump the whole content of "params"
	 * directly into the request and let the remote side verify it. */
	spdk_json_write_name(w, "params");
	spdk_json_write_val_raw(w, cfg.params->start, params_len);
	spdk_jsonrpc_end_request(rpc_request, w);

	spdk_jsonrpc_client_send_request(ctx->client_conn, rpc_request);

	client_send_request(ctx, rpc_request, spdk_app_json_config_load_subsystem_config_entry_next);
out:
	free(cfg.method);
}

static void
subsystem_init_done_resp_cb(struct load_json_config_ctx *ctx,
			    struct spdk_jsonrpc_client_response *resp)
{
	spdk_jsonrpc_client_free_response(resp);

	/* Another round. This time for RUNTIME methods */
	SPDK_DEBUG_APP_CFG("'start_subsystem_init' done - continuing configuration\n");
	ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);
	spdk_app_json_config_load_subsystem(ctx);
}

static struct spdk_json_object_decoder subsystem_decoders[] = {
	{"subsystem", offsetof(struct load_json_config_ctx, subsystem_name), cap_string},
	{"config", offsetof(struct load_json_config_ctx, config), cap_array_or_null}
};

/* Start loading next subsystem.
 * ctx->subsystems_it must point to the begining of the "subsystem" object.
 */
static void
spdk_app_json_config_load_subsystem(void *_ctx)
{
	struct load_json_config_ctx *ctx = _ctx;
	struct spdk_jsonrpc_client_request *req;
	struct spdk_json_write_ctx *w;

	if (ctx->subsystems_it == NULL) {
		if (spdk_rpc_get_state() == SPDK_RPC_STARTUP) {
			SPDK_DEBUG_APP_CFG("No more entries for current state, calling 'start_subsystem_init'\n");
			req = spdk_jsonrpc_client_create_request();
			w = spdk_jsonrpc_begin_request(req, ctx->rpc_request_id++, "start_subsystem_init");
			if (!w) {
				spdk_jsonrpc_client_free_request(req);
				spdk_app_json_config_load_done(ctx, -ENOMEM);
				return;
			}
			spdk_jsonrpc_end_request(req, w);

			client_send_request(ctx, req, subsystem_init_done_resp_cb);
		} else {
			spdk_app_json_config_load_done(ctx, 0);
		}

		return;
	}

	/* Capture subsystem name and config array */
	if (spdk_json_decode_object(ctx->subsystems_it, subsystem_decoders,
				    SPDK_COUNTOF(subsystem_decoders), ctx)) {
		SPDK_ERRLOG("Failed to parse subsystem configuration\n");
		spdk_app_json_config_load_done(ctx, -EINVAL);
		return;
	}

	SPDK_DEBUG_APP_CFG("Loading subsystem '%.*s' configuration\n", ctx->subsystem_name->len,
			   (char *)ctx->subsystem_name->start);

	/* Get 'config' array first configuration entry */
	ctx->config_it = spdk_json_array_first(ctx->config);
	spdk_app_json_config_load_subsystem_config_entry(ctx);
}


static void *
read_file(const char *filename, size_t *size)
{
	FILE *file = fopen(filename, "r");
	void *data = NULL;
	long int rc = 0;

	if (file == NULL) {
		/* errno is set by fopen */
		return NULL;
	}

	rc = fseek(file, 0, SEEK_END);
	if (rc == 0) {
		rc = ftell(file);
		rewind(file);
	}

	if (rc != -1) {
		*size = rc;
		data = malloc(*size);
	}

	if (data != NULL) {
		rc = fread(data, 1, *size, file);
		if (rc != (long int)*size) {
			free(data);
			data = NULL;
			errno = EIO;
		}
	}

	fclose(file);
	return data;
}

static int
spdk_app_json_config_read(const char *config_file, struct load_json_config_ctx *ctx)
{
	struct spdk_json_val *values = NULL;
	void *json = NULL, *end;
	ssize_t values_cnt, rc;
	size_t json_size;

	json = read_file(config_file, &json_size);
	if (!json) {
		return -errno;
	}

	/* First try happy path. */
	values_cnt = 1024;
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	if (values == NULL) {
		rc = -ENOMEM;
		goto err;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
	if (rc < 0) {
		fprintf(stderr, "Parsing JSON configuration failed (%zd)\n", rc);
		goto err;
	}

	/* If there is more values that anticipated - try again. */
	if (rc > values_cnt) {
		free(values);
		values_cnt = rc;

		values = calloc(values_cnt, sizeof(struct spdk_json_val));
		if (values == NULL) {
			fprintf(stderr, "Out of memory\n");
			goto err;
		}

		rc = spdk_json_parse(json, json_size, NULL, 0, &end, SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS);
		if (rc != values_cnt) {
			fprintf(stderr, "Parsing JSON configuration failed (%zd)\n", rc);
			goto err;
		}
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
spdk_app_json_config_load_cb(void *_json_path, void *_done_event)
{
	const char *json_path = _json_path;
	struct spdk_event *done_event = _done_event;
	struct load_json_config_ctx *ctx = calloc(1, sizeof(*ctx));
	int rc;

	assert(done_event);
	if (!ctx) {
		spdk_app_stop(-ENOMEM);
		return;
	}

	ctx->done_event = done_event;
	ctx->thread = spdk_get_thread();

	rc = spdk_app_json_config_read(json_path, ctx);
	if (rc) {
		goto fail;
	}

	/* Capture subsystems array */
	rc = spdk_json_find_array(ctx->values, "subsystems", NULL, &ctx->subsystems);
	if (rc) {
		SPDK_ERRLOG("Failed to find 'subsystems' in JSON configuration file\n");
		spdk_app_json_config_load_done(ctx, -EINVAL);
		return;
	}

	/* Get first subsystem */
	ctx->subsystems_it = spdk_json_array_first(ctx->subsystems);
	if (ctx->subsystems_it == NULL) {
		spdk_app_json_config_load_done(ctx, 0);
		return;
	}

	/* FIXME: rpc client should use socketpair() instead of this temporary socket nonsense */
	rc = snprintf(ctx->rpc_socket_path_temp, sizeof(ctx->rpc_socket_path_temp), "%s.%d_config",
		      SPDK_DEFAULT_RPC_ADDR, getpid());
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

	ctx->client_conn_poller = spdk_poller_register(rpc_client_connect_poller, ctx, 100);
	return;

fail:
	spdk_app_json_config_load_done(ctx, -EINVAL);
}

SPDK_LOG_REGISTER_COMPONENT("app_config", SPDK_LOG_APP_CONFIG)
