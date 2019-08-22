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

#include "bdev_nvme.h"
#include "common.h"

#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"
#include "spdk/bdev_module.h"

struct open_descriptors {
	void *desc;
	struct  spdk_bdev *bdev;
	TAILQ_ENTRY(open_descriptors) tqlst;
};
typedef TAILQ_HEAD(, open_descriptors) open_descriptors_t;

static int
rpc_decode_action_on_timeout(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_timeout_action *action = out;

	if (spdk_json_strequal(val, "none") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE;
	} else if (spdk_json_strequal(val, "abort") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT;
	} else if (spdk_json_strequal(val, "reset") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: action_on_timeout\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_options_decoders[] = {
	{"action_on_timeout", offsetof(struct spdk_bdev_nvme_opts, action_on_timeout), rpc_decode_action_on_timeout, true},
	{"timeout_us", offsetof(struct spdk_bdev_nvme_opts, timeout_us), spdk_json_decode_uint64, true},
	{"retry_count", offsetof(struct spdk_bdev_nvme_opts, retry_count), spdk_json_decode_uint32, true},
	{"nvme_adminq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_adminq_poll_period_us), spdk_json_decode_uint64, true},
	{"nvme_ioq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_ioq_poll_period_us), spdk_json_decode_uint64, true},
	{"io_queue_requests", offsetof(struct spdk_bdev_nvme_opts, io_queue_requests), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_set_bdev_nvme_options(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_bdev_nvme_opts opts;
	struct spdk_json_write_ctx *w;
	int rc;

	spdk_bdev_nvme_get_opts(&opts);
	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_options_decoders),
					      &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_bdev_nvme_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	return;
}
SPDK_RPC_REGISTER("set_bdev_nvme_options", spdk_rpc_set_bdev_nvme_options, SPDK_RPC_STARTUP)

struct rpc_bdev_nvme_hotplug {
	bool enabled;
	uint64_t period_us;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_hotplug_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_nvme_hotplug, enabled), spdk_json_decode_bool, false},
	{"period_us", offsetof(struct rpc_bdev_nvme_hotplug, period_us), spdk_json_decode_uint64, true},
};

static void
rpc_set_bdev_nvme_hotplug_done(void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_set_bdev_nvme_hotplug(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_hotplug req = {false, 0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_hotplug_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_hotplug_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_bdev_nvme_set_hotplug(req.enabled, req.period_us, rpc_set_bdev_nvme_hotplug_done,
					request);
	if (rc) {
		goto invalid;
	}

	return;
invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("set_bdev_nvme_hotplug", spdk_rpc_set_bdev_nvme_hotplug, SPDK_RPC_RUNTIME)

struct rpc_create_nvme_bdev_ctx {
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	size_t count;
	spdk_rpc_construct_bdev_cb_fn cb_fn;
	void *cb_arg;
};

static void
spdk_rpc_construct_nvme_bdev_done(void *cb_ctx, int rc)
{
	struct rpc_create_nvme_bdev_ctx *ctx = cb_ctx;
	struct nvme_bdev_info bdev_info = {};

	bdev_info.count = ctx->count;
	memcpy(bdev_info.names, ctx->names, sizeof(char *)*NVME_MAX_BDEVS_PER_RPC);

	ctx->cb_fn(&bdev_info, ctx->cb_arg, rc);

	free(ctx);
}

static void
spdk_rpc_construct_generic_nvme_bdev(struct spdk_bdev_nvme_construct_opts *opts,
				     spdk_rpc_construct_bdev_cb_fn cb_fn, void *cb_arg)
{
	struct rpc_create_nvme_bdev_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto invalid;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	ctx->count = NVME_MAX_BDEVS_PER_RPC;
	rc = spdk_bdev_nvme_create(&opts->trid, &opts->hostid, opts->name, ctx->names,
				   &ctx->count, opts->hostnqn, opts->prchk_flags, spdk_rpc_construct_nvme_bdev_done, ctx);
	if (rc) {
		goto invalid;
	}

	return;

invalid:
	cb_fn(NULL, cb_arg, rc);
	free(ctx);
}

static int
spdk_rpc_parse_generic_bdev_args(struct rpc_construct_nvme *req,
				 struct spdk_bdev_nvme_construct_opts *opts)
{
	int rc;

	/* Parse adrfam */
	if (req->adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&opts->trid.adrfam, req->adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req->adrfam);
			return -EINVAL;
		}
	}

	/* Parse trsvcid */
	if (req->trsvcid) {
		snprintf(opts->trid.trsvcid, sizeof(opts->trid.trsvcid), "%s", req->trsvcid);
	}

	/* Parse subnqn */
	if (req->subnqn) {
		snprintf(opts->trid.subnqn, sizeof(opts->trid.subnqn), "%s", req->subnqn);
	}

	if (req->hostaddr) {
		snprintf(opts->hostid.hostaddr, sizeof(opts->hostid.hostaddr), "%s", req->hostaddr);
	}

	if (req->hostsvcid) {
		snprintf(opts->hostid.hostsvcid, sizeof(opts->hostid.hostsvcid), "%s", req->hostsvcid);
	}

	if (req->prchk_reftag) {
		opts->prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}

	if (req->prchk_guard) {
		opts->prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}

	opts->hostnqn = req->hostnqn;

	return 0;
}

SPDK_RPC_REGISTER_CONSTRUCT_FNS("generic", spdk_rpc_construct_generic_nvme_bdev,
				spdk_rpc_parse_generic_bdev_args)

struct spdk_construct_fn_list {
	const char *bdev_type;
	spdk_rpc_construct_bdev_fn construct_fn;
	spdk_rpc_parse_args_fn parse_fn;
	SLIST_ENTRY(spdk_construct_fn_list) slist;
};

static SLIST_HEAD(, spdk_construct_fn_list) g_construct_methods = SLIST_HEAD_INITIALIZER(
			g_construct_methods);

void
spdk_rpc_register_nvme_construct_methods(const char *bdev_type,
		spdk_rpc_construct_bdev_fn construct_fn, spdk_rpc_parse_args_fn parse_fn)
{
	struct spdk_construct_fn_list *m;

	m = calloc(1, sizeof(struct spdk_construct_fn_list));
	assert(m != NULL);

	m->bdev_type = strdup(bdev_type);
	assert(m->bdev_type != NULL);

	assert(construct_fn != NULL);
	m->construct_fn = construct_fn;
	m->parse_fn = parse_fn;

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_construct_methods, m, slist);
}

static void
free_rpc_construct_nvme(struct rpc_construct_nvme *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->subnqn);
	free(req->hostnqn);
	free(req->hostaddr);
	free(req->hostsvcid);
	free(req->punits);
	free(req->uuid);
	free(req->cache_bdev);
	free(req->mode);
}

static const struct spdk_json_object_decoder rpc_construct_nvme_mode_decoder[] = {
	{"mode", offsetof(struct rpc_construct_nvme, mode), spdk_json_decode_string, true}
};

static const struct spdk_json_object_decoder rpc_construct_basic_nvme_decoders[] = {
	{"name", offsetof(struct rpc_construct_nvme, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_nvme, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_nvme, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_construct_nvme, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_construct_nvme, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_construct_nvme, subnqn), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_construct_nvme, hostnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_construct_nvme, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_construct_nvme, hostsvcid), spdk_json_decode_string, true},

	{"prchk_reftag", offsetof(struct rpc_construct_nvme, prchk_reftag), spdk_json_decode_bool, true},
	{"prchk_guard", offsetof(struct rpc_construct_nvme, prchk_guard), spdk_json_decode_bool, true},
	{"mode", offsetof(struct rpc_construct_nvme, mode), spdk_json_decode_string, true}
};

static const struct spdk_json_object_decoder rpc_construct_ftl_nvme_decoders[] = {
	{"name", offsetof(struct rpc_construct_nvme, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_nvme, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_nvme, traddr), spdk_json_decode_string},
	{"punits", offsetof(struct rpc_construct_nvme, punits), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_nvme, uuid), spdk_json_decode_string, true},
	{"mode", offsetof(struct rpc_construct_nvme, mode), spdk_json_decode_string},
	{"cache", offsetof(struct rpc_construct_nvme, cache_bdev), spdk_json_decode_string, true},
	{
		"allow_open_bands", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, allow_open_bands), spdk_json_decode_bool, true
	},
	{
		"overprovisioning", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, lba_rsvd), spdk_json_decode_uint64, true
	},
	{
		"limit_crit", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_CRIT]) +
		offsetof(struct spdk_ftl_limit, limit),
		spdk_json_decode_uint64, true
	},
	{
		"limit_crit_threshold", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_CRIT]) +
		offsetof(struct spdk_ftl_limit, thld),
		spdk_json_decode_uint64, true
	},
	{
		"limit_high", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_HIGH]) +
		offsetof(struct spdk_ftl_limit, limit),
		spdk_json_decode_uint64, true
	},
	{
		"limit_high_threshold", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_HIGH]) +
		offsetof(struct spdk_ftl_limit, thld),
		spdk_json_decode_uint64, true
	},
	{
		"limit_low", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_LOW]) +
		offsetof(struct spdk_ftl_limit, limit),
		spdk_json_decode_uint64, true
	},
	{
		"limit_low_threshold", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_LOW]) +
		offsetof(struct spdk_ftl_limit, thld),
		spdk_json_decode_uint64, true
	},
	{
		"limit_start", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_START]) +
		offsetof(struct spdk_ftl_limit, limit),
		spdk_json_decode_uint64, true
	},
	{
		"limit_start_threshold", offsetof(struct rpc_construct_nvme, ftl_conf) +
		offsetof(struct spdk_ftl_conf, limits[SPDK_FTL_LIMIT_START]) +
		offsetof(struct spdk_ftl_limit, thld),
		spdk_json_decode_uint64, true
	},
};

struct rpc_construct_nvme_bdev_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_construct_nvme req;
};

static void
spdk_rpc_construct_nvme_bdev_cb(struct nvme_bdev_info *bdev_info, void *cb_ctx, int rc)
{
	struct rpc_construct_nvme_bdev_ctx *ctx = cb_ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (rc < 0) {
		spdk_jsonrpc_send_error_response_fmt(ctx->request, rc,
						     "Failed to create nvme bdev(s): %s",
						     spdk_strerror(-rc));
		goto exit;
	}

	w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_array_begin(w);
	for (i = 0; i < bdev_info->count; i++) {
		spdk_json_write_string(w, bdev_info->names[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(ctx->request, w);

exit:
	free_rpc_construct_nvme(&ctx->req);
	free(ctx);
}

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_nvme mode_req = {};
	struct rpc_construct_nvme req = {};
	struct spdk_bdev_nvme_construct_opts opts = {};
	struct rpc_construct_nvme_bdev_ctx *ctx;
	struct spdk_construct_fn_list *m;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate memory\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM,
						 "Failed to allocate memory");
		goto cleanup;
	}

	if (spdk_json_decode_object_lenient(params, rpc_construct_nvme_mode_decoder,
					    SPDK_COUNTOF(rpc_construct_nvme_mode_decoder),
					    &mode_req)) {
		SPDK_ERRLOG("spdk_json_decode_object_lenient failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object_lenient failed");
		goto cleanup;
	}

	if (mode_req.mode == NULL || !strcasecmp(mode_req.mode, "generic")) {
		if (spdk_json_decode_object(params, rpc_construct_basic_nvme_decoders,
					    SPDK_COUNTOF(rpc_construct_basic_nvme_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}
	} else if (!strcasecmp(mode_req.mode, "ftl")) {
		if (spdk_json_decode_object(params, rpc_construct_ftl_nvme_decoders,
					    SPDK_COUNTOF(rpc_construct_ftl_nvme_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			goto cleanup;
		}
	} else {
		SPDK_ERRLOG("Unknown NVMe bdev type\n");
		goto cleanup;
	}

	/* Parse common arguments */

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&opts.trid.trtype, req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
		goto cleanup;
	}

	/* Parse traddr */
	snprintf(opts.trid.traddr, sizeof(opts.trid.traddr), "%s", req.traddr);

	opts.name = req.name;

	if (req.mode != NULL) {
		SLIST_FOREACH(m, &g_construct_methods, slist) {
			if (!strcasecmp(req.mode, m->bdev_type)) {
				if (m->parse_fn && m->parse_fn(&req, &opts)) {
					goto cleanup;
				}
				m->construct_fn(&opts, spdk_rpc_construct_nvme_bdev_cb, request);
				break;
			}
		}
		if (m == NULL) {
			SPDK_ERRLOG("Unknown construct method\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Unknown construct method");
			goto cleanup;
		}
	} else {
		ctx->req = req;
		ctx->request = request;
		spdk_rpc_construct_generic_nvme_bdev(&opts, spdk_rpc_construct_nvme_bdev_cb, ctx);
	}

	free(mode_req.mode);
	return;

cleanup:
	free(ctx);
	free(mode_req.mode);
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev, SPDK_RPC_RUNTIME)

static void
spdk_rpc_dump_nvme_controller_info(struct spdk_json_write_ctx *w,
				   struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	struct spdk_nvme_transport_id	*trid;

	trid = &nvme_bdev_ctrlr->trid;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", nvme_bdev_ctrlr->name);

	spdk_json_write_named_object_begin(w, "trid");
	nvme_bdev_dump_trid_json(trid, w);
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
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.name) {
		ctrlr = nvme_bdev_ctrlr_get_by_name(req.name);
		if (ctrlr == NULL) {
			SPDK_ERRLOG("ctrlr '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Controller %s does not exist", req.name);
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (ctrlr != NULL) {
		spdk_rpc_dump_nvme_controller_info(w, ctrlr);
	} else {
		for (ctrlr = nvme_bdev_first_ctrlr(); ctrlr; ctrlr = nvme_bdev_next_ctrlr(ctrlr))  {
			spdk_rpc_dump_nvme_controller_info(w, ctrlr);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_get_nvme_controllers(&req);
}
SPDK_RPC_REGISTER("get_nvme_controllers", spdk_rpc_get_nvme_controllers, SPDK_RPC_RUNTIME)

struct rpc_delete_nvme {
	char *name;
};

static void
free_rpc_delete_nvme(struct rpc_delete_nvme *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_nvme_decoders[] = {
	{"name", offsetof(struct rpc_delete_nvme, name), spdk_json_decode_string},
};

static void
spdk_rpc_delete_nvme_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_delete_nvme req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_delete_nvme_decoders,
				    SPDK_COUNTOF(rpc_delete_nvme_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_nvme_delete(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_delete_nvme(&req);
}
SPDK_RPC_REGISTER("delete_nvme_controller", spdk_rpc_delete_nvme_controller, SPDK_RPC_RUNTIME)

struct rpc_apply_firmware {
	char *filename;
	char *bdev_name;
};

static void
free_rpc_apply_firmware(struct rpc_apply_firmware *req)
{
	free(req->filename);
	free(req->bdev_name);
}

static const struct spdk_json_object_decoder rpc_apply_firmware_decoders[] = {
	{"filename", offsetof(struct rpc_apply_firmware, filename), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_apply_firmware, bdev_name), spdk_json_decode_string},
};

struct firmware_update_info {
	void				*fw_image;
	void				*p;
	unsigned int			size;
	unsigned int			size_remaining;
	unsigned int			offset;
	unsigned int			transfer;

	void				*desc;
	struct spdk_io_channel		*ch;
	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_ctrlr		*ctrlr;
	open_descriptors_t		desc_head;
	struct rpc_apply_firmware	*req;
};

static void
apply_firmware_cleanup(void *cb_arg)
{
	struct open_descriptors			*opt, *tmp;
	struct firmware_update_info *firm_ctx = cb_arg;

	if (!firm_ctx) {
		return;
	}

	if (firm_ctx->fw_image) {
		spdk_free(firm_ctx->fw_image);
	}

	if (firm_ctx->req) {
		free_rpc_apply_firmware(firm_ctx->req);
		free(firm_ctx->req);
	}
	TAILQ_FOREACH_SAFE(opt, &firm_ctx->desc_head, tqlst, tmp) {
		TAILQ_REMOVE(&firm_ctx->desc_head, opt, tqlst);
		spdk_bdev_close(opt->desc);
		free(opt);
	}
	free(firm_ctx);
}

static void
apply_firmware_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	int					rc;
	struct spdk_json_write_ctx		*w;
	struct firmware_update_info *firm_ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware commit failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	if ((rc = spdk_nvme_ctrlr_reset(firm_ctx->ctrlr)) != 0) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Controller reset failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(firm_ctx->request);
	spdk_json_write_string(w, "firmware commit succeeded. Controller reset in progress.");
	spdk_jsonrpc_end_result(firm_ctx->request, w);
	apply_firmware_cleanup(firm_ctx);
}

static void
apply_firmware_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvme_cmd			cmd = {};
	struct spdk_nvme_fw_commit		fw_commit;
	int					slot = 0;
	int					rc;
	struct firmware_update_info *firm_ctx = cb_arg;
	enum spdk_nvme_fw_commit_action commit_action = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;

	if (!success) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware download failed .");
		spdk_bdev_free_io(bdev_io);
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	firm_ctx->p += firm_ctx->transfer;
	firm_ctx->offset += firm_ctx->transfer;
	firm_ctx->size_remaining -= firm_ctx->transfer;

	switch (firm_ctx->size_remaining) {
	case 0:
		/* firmware download completed. Commit firmware */
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.fs = slot;
		fw_commit.ca = commit_action;

		cmd.opc = SPDK_NVME_OPC_FIRMWARE_COMMIT;
		memcpy(&cmd.cdw10, &fw_commit, sizeof(uint32_t));
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, NULL, 0,
						   apply_firmware_complete_reset, firm_ctx);
		if (rc) {
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware commit failed.");
			spdk_bdev_free_io(bdev_io);
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	default:
		firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);
		cmd.opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

		cmd.cdw10 = (firm_ctx->transfer >> 2) - 1;
		cmd.cdw11 = firm_ctx->offset >> 2;
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, firm_ctx->p,
						   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
		if (rc) {
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware download failed.");
			spdk_bdev_free_io(bdev_io);
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	}
}

static void
spdk_rpc_apply_nvme_firmware(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	int					rc;
	int					fd = -1;
	struct stat				fw_stat;
	struct spdk_nvme_ctrlr			*ctrlr;
	char					msg[1024];
	struct spdk_bdev			*bdev;
	struct spdk_bdev			*bdev2;
	struct open_descriptors			*opt;
	struct spdk_bdev_desc			*desc;
	struct spdk_nvme_cmd			*cmd;
	struct firmware_update_info		*firm_ctx;

	firm_ctx = malloc(sizeof(struct firmware_update_info));
	if (!firm_ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return;
	}
	firm_ctx->fw_image = NULL;
	TAILQ_INIT(&firm_ctx->desc_head);
	firm_ctx->request = request;

	firm_ctx->req = malloc(sizeof(struct rpc_apply_firmware));
	if (!firm_ctx->req) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		free(firm_ctx);
		return;
	}

	if (spdk_json_decode_object(params, rpc_apply_firmware_decoders,
				    SPDK_COUNTOF(rpc_apply_firmware_decoders), firm_ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed.");
		free(firm_ctx->req);
		free(firm_ctx);
		return;
	}

	if ((bdev = spdk_bdev_get_by_name(firm_ctx->req->bdev_name)) == NULL) {
		snprintf(msg, sizeof(msg), "bdev %s were not found", firm_ctx->req->bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	if ((ctrlr = spdk_bdev_nvme_get_ctrlr(bdev)) == NULL) {
		snprintf(msg, sizeof(msg), "Controller information for %s were not found.",
			 firm_ctx->req->bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		apply_firmware_cleanup(firm_ctx);
		return;
	}
	firm_ctx->ctrlr = ctrlr;

	for (bdev2 = spdk_bdev_first(); bdev2; bdev2 = spdk_bdev_next(bdev2)) {

		if (spdk_bdev_nvme_get_ctrlr(bdev2) != ctrlr) {
			continue;
		}

		if (!(opt = malloc(sizeof(struct open_descriptors)))) {
			snprintf(msg, sizeof(msg), "Memory allocation error.");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
			apply_firmware_cleanup(firm_ctx);
			return;
		}

		if ((rc = spdk_bdev_open(bdev2, true, NULL, NULL, &desc)) != 0) {
			snprintf(msg, sizeof(msg), "Device %s is in use.", firm_ctx->req->bdev_name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
			free(opt);
			apply_firmware_cleanup(firm_ctx);
			return;
		}

		opt->desc = desc;
		opt->bdev = bdev;
		TAILQ_INSERT_TAIL(&firm_ctx->desc_head, opt, tqlst);
	}

	/*
	 * find a descriptor associated with our bdev
	 */
	firm_ctx->desc = NULL;
	TAILQ_FOREACH(opt, &firm_ctx->desc_head, tqlst) {
		if (opt->bdev == bdev) {
			firm_ctx->desc = opt->desc;
			break;
		}
	}

	if (!firm_ctx->desc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No descriptor were found.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	firm_ctx->ch = spdk_bdev_get_io_channel(firm_ctx->desc);
	if (!firm_ctx->ch) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No channels were found.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	fd = open(firm_ctx->req->filename, O_RDONLY);
	if (fd < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "open file failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "fstat failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	firm_ctx->size = fw_stat.st_size;
	if (fw_stat.st_size % 4) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Firmware image size is not multiple of 4.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	firm_ctx->fw_image = spdk_zmalloc(firm_ctx->size, 4096, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!firm_ctx->fw_image) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}
	firm_ctx->p = firm_ctx->fw_image;

	if (read(fd, firm_ctx->p, firm_ctx->size) != ((ssize_t)(firm_ctx->size))) {
		close(fd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Read firmware image failed!");
		apply_firmware_cleanup(firm_ctx);
		return;
	}
	close(fd);

	firm_ctx->offset = 0;
	firm_ctx->size_remaining = firm_ctx->size;
	firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);

	cmd = malloc(sizeof(struct spdk_nvme_cmd));
	if (!cmd) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}
	memset(cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

	cmd->cdw10 = (firm_ctx->transfer >> 2) - 1;
	cmd->cdw11 = firm_ctx->offset >> 2;

	rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, cmd, firm_ctx->p,
					   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
	if (rc) {
		free(cmd);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Read firmware image failed!");
		apply_firmware_cleanup(firm_ctx);
		return;
	}
}
SPDK_RPC_REGISTER("apply_nvme_firmware", spdk_rpc_apply_nvme_firmware, SPDK_RPC_RUNTIME)
