/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/config.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk/log.h"
#include "spdk/bdev_module.h"

struct open_descriptors {
	void *desc;
	struct  spdk_bdev *bdev;
	TAILQ_ENTRY(open_descriptors) tqlst;
	struct spdk_thread *thread;
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
	{"arbitration_burst", offsetof(struct spdk_bdev_nvme_opts, arbitration_burst), spdk_json_decode_uint32, true},
	{"low_priority_weight", offsetof(struct spdk_bdev_nvme_opts, low_priority_weight), spdk_json_decode_uint32, true},
	{"medium_priority_weight", offsetof(struct spdk_bdev_nvme_opts, medium_priority_weight), spdk_json_decode_uint32, true},
	{"high_priority_weight", offsetof(struct spdk_bdev_nvme_opts, high_priority_weight), spdk_json_decode_uint32, true},
	{"nvme_adminq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_adminq_poll_period_us), spdk_json_decode_uint64, true},
	{"nvme_ioq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_ioq_poll_period_us), spdk_json_decode_uint64, true},
	{"io_queue_requests", offsetof(struct spdk_bdev_nvme_opts, io_queue_requests), spdk_json_decode_uint32, true},
	{"delay_cmd_submit", offsetof(struct spdk_bdev_nvme_opts, delay_cmd_submit), spdk_json_decode_bool, true},
};

static void
rpc_bdev_nvme_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_bdev_nvme_opts opts;
	int rc;

	bdev_nvme_get_opts(&opts);
	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_options_decoders),
					      &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = bdev_nvme_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);

	return;
}
SPDK_RPC_REGISTER("bdev_nvme_set_options", rpc_bdev_nvme_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_set_options, set_bdev_nvme_options)

struct rpc_bdev_nvme_hotplug {
	bool enabled;
	uint64_t period_us;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_hotplug_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_nvme_hotplug, enabled), spdk_json_decode_bool, false},
	{"period_us", offsetof(struct rpc_bdev_nvme_hotplug, period_us), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_nvme_set_hotplug_done(void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_nvme_set_hotplug(struct spdk_jsonrpc_request *request,
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

	rc = bdev_nvme_set_hotplug(req.enabled, req.period_us, rpc_bdev_nvme_set_hotplug_done,
				   request);
	if (rc) {
		goto invalid;
	}

	return;
invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("bdev_nvme_set_hotplug", rpc_bdev_nvme_set_hotplug, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_set_hotplug, set_bdev_nvme_hotplug)

struct rpc_bdev_nvme_attach_controller {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *priority;
	char *subnqn;
	char *hostnqn;
	char *hostaddr;
	char *hostsvcid;
	bool prchk_reftag;
	bool prchk_guard;
};

static void
free_rpc_bdev_nvme_attach_controller(struct rpc_bdev_nvme_attach_controller *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->priority);
	free(req->subnqn);
	free(req->hostnqn);
	free(req->hostaddr);
	free(req->hostsvcid);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_attach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_attach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_attach_controller, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_bdev_nvme_attach_controller, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_bdev_nvme_attach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, trsvcid), spdk_json_decode_string, true},
	{"priority", offsetof(struct rpc_bdev_nvme_attach_controller, priority), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_attach_controller, subnqn), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_attach_controller, hostnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_bdev_nvme_attach_controller, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, hostsvcid), spdk_json_decode_string, true},

	{"prchk_reftag", offsetof(struct rpc_bdev_nvme_attach_controller, prchk_reftag), spdk_json_decode_bool, true},
	{"prchk_guard", offsetof(struct rpc_bdev_nvme_attach_controller, prchk_guard), spdk_json_decode_bool, true}
};

#define NVME_MAX_BDEVS_PER_RPC 128

struct rpc_bdev_nvme_attach_controller_ctx {
	struct rpc_bdev_nvme_attach_controller req;
	uint32_t count;
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	struct spdk_jsonrpc_request *request;
};

static void
rpc_bdev_nvme_attach_controller_done(void *cb_ctx, size_t bdev_count, int rc)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx = cb_ctx;
	struct spdk_jsonrpc_request *request = ctx->request;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto exit;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	for (i = 0; i < bdev_count; i++) {
		spdk_json_write_string(w, ctx->names[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

exit:
	free_rpc_bdev_nvme_attach_controller(&ctx->req);
	free(ctx);
}

static void
rpc_bdev_nvme_attach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx;
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_host_id hostid = {};
	uint32_t prchk_flags = 0;
	struct nvme_bdev_ctrlr *ctrlr = NULL;
	size_t len, maxlen;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_attach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_attach_controller_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Parse trstring */
	rc = spdk_nvme_transport_id_populate_trstring(&trid, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	assert(rc == 0);

	ctrlr = nvme_bdev_ctrlr_get_by_name(ctx->req.name);

	/* Parse traddr */
	maxlen = sizeof(trid.traddr);
	len = strnlen(ctx->req.traddr, maxlen);
	if (len == maxlen) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
						     ctx->req.traddr);
		goto cleanup;
	}
	memcpy(trid.traddr, ctx->req.traddr, len + 1);

	/* Parse adrfam */
	if (ctx->req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, ctx->req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", ctx->req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     ctx->req.adrfam);
			goto cleanup;
		}
	}

	/* Parse trsvcid */
	if (ctx->req.trsvcid) {
		maxlen = sizeof(trid.trsvcid);
		len = strnlen(ctx->req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     ctx->req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, ctx->req.trsvcid, len + 1);
	}

	/* Parse priority for the NVMe-oF transport connection */
	if (ctx->req.priority) {
		trid.priority = spdk_strtol(ctx->req.priority, 10);
	}

	/* Parse subnqn */
	if (ctx->req.subnqn) {
		maxlen = sizeof(trid.subnqn);
		len = strnlen(ctx->req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     ctx->req.subnqn);
			goto cleanup;
		}
		memcpy(trid.subnqn, ctx->req.subnqn, len + 1);
	}

	if (ctrlr && (ctx->req.hostaddr || ctx->req.hostnqn || ctx->req.hostsvcid || ctx->req.prchk_guard ||
		      ctx->req.prchk_reftag)) {
		goto conflicting_arguments;
	}

	if (ctx->req.hostaddr) {
		maxlen = sizeof(hostid.hostaddr);
		len = strnlen(ctx->req.hostaddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostaddr too long: %s",
							     ctx->req.hostaddr);
			goto cleanup;
		}
		memcpy(hostid.hostaddr, ctx->req.hostaddr, len + 1);
	}

	if (ctx->req.hostsvcid) {
		maxlen = sizeof(hostid.hostsvcid);
		len = strnlen(ctx->req.hostsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostsvcid too long: %s",
							     ctx->req.hostsvcid);
			goto cleanup;
		}
		memcpy(hostid.hostsvcid, ctx->req.hostsvcid, len + 1);
	}

	if (ctx->req.prchk_reftag) {
		prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}

	if (ctx->req.prchk_guard) {
		prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}

	ctx->request = request;
	ctx->count = NVME_MAX_BDEVS_PER_RPC;
	rc = bdev_nvme_create(&trid, &hostid, ctx->req.name, ctx->names, ctx->count, ctx->req.hostnqn,
			      prchk_flags, rpc_bdev_nvme_attach_controller_done, ctx);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

conflicting_arguments:
	spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
					     "Invalid agrgument list. Existing controller name cannot be combined with host information or PI options.\n");
cleanup:
	free_rpc_bdev_nvme_attach_controller(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_attach_controller", rpc_bdev_nvme_attach_controller,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_attach_controller, construct_nvme_bdev)

static void
rpc_dump_nvme_controller_info(struct spdk_json_write_ctx *w,
			      struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	struct spdk_nvme_transport_id	*trid;

	trid = nvme_bdev_ctrlr->connected_trid;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", nvme_bdev_ctrlr->name);

#ifdef SPDK_CONFIG_NVME_CUSE
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	int rc = spdk_nvme_cuse_get_ctrlr_name(nvme_bdev_ctrlr->ctrlr, cuse_name, &cuse_name_size);
	if (rc == 0) {
		spdk_json_write_named_string(w, "cuse_device", cuse_name);
	}
#endif

	spdk_json_write_named_object_begin(w, "trid");
	nvme_bdev_dump_trid_json(trid, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

struct rpc_bdev_nvme_get_controllers {
	char *name;
};

static void
free_rpc_bdev_nvme_get_controllers(struct rpc_bdev_nvme_get_controllers *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_get_controllers_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_get_controllers, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_nvme_get_controllers(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_get_controllers req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_bdev_ctrlr *ctrlr = NULL;

	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_get_controllers_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_get_controllers_decoders),
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
		rpc_dump_nvme_controller_info(w, ctrlr);
	} else {
		for (ctrlr = nvme_bdev_first_ctrlr(); ctrlr; ctrlr = nvme_bdev_next_ctrlr(ctrlr))  {
			rpc_dump_nvme_controller_info(w, ctrlr);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_nvme_get_controllers(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_get_controllers", rpc_bdev_nvme_get_controllers, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_get_controllers, get_nvme_controllers)

struct rpc_bdev_nvme_detach_controller {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
};

static void
free_rpc_bdev_nvme_detach_controller(struct rpc_bdev_nvme_detach_controller *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->subnqn);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_detach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_detach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_detach_controller, trtype), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_bdev_nvme_detach_controller, traddr), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_bdev_nvme_detach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_detach_controller, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_detach_controller, subnqn), spdk_json_decode_string, true},
};

static void
rpc_bdev_nvme_detach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_detach_controller req = {NULL};
	struct spdk_nvme_transport_id trid = {};
	size_t len, maxlen;
	int rc = 0;
	bool all_trid_entries, one_trid_entry;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_detach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_detach_controller_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	all_trid_entries = req.trtype && req.traddr && req.adrfam && req.trsvcid && req.subnqn;
	one_trid_entry = req.trtype || req.traddr || req.adrfam || req.trsvcid || req.subnqn;

	if (all_trid_entries ^ one_trid_entry) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "trtype, traddr, adrfam, trsvcid, subnqn must all be provided together or not at all.");
		goto cleanup;
	}

	if (all_trid_entries) {
		/* Parse trtype */
		rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, req.trtype);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
							     req.trtype);
			goto cleanup;
		}

		/* Parse traddr */
		maxlen = sizeof(trid.traddr);
		len = strnlen(req.traddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
							     req.traddr);
			goto cleanup;
		}
		memcpy(trid.traddr, req.traddr, len + 1);

		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     req.adrfam);
			goto cleanup;
		}

		maxlen = sizeof(trid.trsvcid);
		len = strnlen(req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, req.trsvcid, len + 1);

		maxlen = sizeof(trid.subnqn);
		len = strnlen(req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     req.subnqn);
			goto cleanup;
		}
		memcpy(trid.subnqn, req.subnqn, len + 1);
		rc = bdev_nvme_remove_trid(req.name, &trid);
	} else {
		rc = bdev_nvme_delete(req.name);
	}

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_nvme_detach_controller(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_detach_controller", rpc_bdev_nvme_detach_controller,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_detach_controller, delete_nvme_controller)

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
_apply_firmware_cleanup(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

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

	if (firm_ctx->ch) {
		spdk_put_io_channel(firm_ctx->ch);
	}

	TAILQ_FOREACH_SAFE(opt, &firm_ctx->desc_head, tqlst, tmp) {
		TAILQ_REMOVE(&firm_ctx->desc_head, opt, tqlst);
		/* Close the underlying bdev on its same opened thread. */
		if (opt->thread && opt->thread != spdk_get_thread()) {
			spdk_thread_send_msg(opt->thread, _apply_firmware_cleanup, opt->desc);
		} else {
			spdk_bdev_close(opt->desc);
		}
		free(opt);
	}
	free(firm_ctx);
}

static void
apply_firmware_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_json_write_ctx		*w;
	struct firmware_update_info *firm_ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware commit failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	if (spdk_nvme_ctrlr_reset(firm_ctx->ctrlr) != 0) {
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
rpc_bdev_nvme_apply_firmware(struct spdk_jsonrpc_request *request,
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

	firm_ctx = calloc(1, sizeof(struct firmware_update_info));
	if (!firm_ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return;
	}
	firm_ctx->fw_image = NULL;
	TAILQ_INIT(&firm_ctx->desc_head);
	firm_ctx->request = request;

	firm_ctx->req = calloc(1, sizeof(struct rpc_apply_firmware));
	if (!firm_ctx->req) {
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}

	if (spdk_json_decode_object(params, rpc_apply_firmware_decoders,
				    SPDK_COUNTOF(rpc_apply_firmware_decoders), firm_ctx->req)) {
		snprintf(msg, sizeof(msg), "spdk_json_decode_object failed.");
		goto err;
	}

	if ((bdev = spdk_bdev_get_by_name(firm_ctx->req->bdev_name)) == NULL) {
		snprintf(msg, sizeof(msg), "bdev %s were not found", firm_ctx->req->bdev_name);
		goto err;
	}

	if ((ctrlr = bdev_nvme_get_ctrlr(bdev)) == NULL) {
		snprintf(msg, sizeof(msg), "Controller information for %s were not found.",
			 firm_ctx->req->bdev_name);
		goto err;
	}
	firm_ctx->ctrlr = ctrlr;

	for (bdev2 = spdk_bdev_first(); bdev2; bdev2 = spdk_bdev_next(bdev2)) {

		if (bdev_nvme_get_ctrlr(bdev2) != ctrlr) {
			continue;
		}

		if (!(opt = malloc(sizeof(struct open_descriptors)))) {
			snprintf(msg, sizeof(msg), "Memory allocation error.");
			goto err;
		}

		if (spdk_bdev_open(bdev2, true, NULL, NULL, &desc) != 0) {
			snprintf(msg, sizeof(msg), "Device %s is in use.", firm_ctx->req->bdev_name);
			free(opt);
			goto err;
		}

		/* Save the thread where the base device is opened */
		opt->thread = spdk_get_thread();

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
		snprintf(msg, sizeof(msg), "No descriptor were found.");
		goto err;
	}

	firm_ctx->ch = spdk_bdev_get_io_channel(firm_ctx->desc);
	if (!firm_ctx->ch) {
		snprintf(msg, sizeof(msg), "No channels were found.");
		goto err;
	}

	fd = open(firm_ctx->req->filename, O_RDONLY);
	if (fd < 0) {
		snprintf(msg, sizeof(msg), "open file failed.");
		goto err;
	}

	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		close(fd);
		snprintf(msg, sizeof(msg), "fstat failed.");
		goto err;
	}

	firm_ctx->size = fw_stat.st_size;
	if (fw_stat.st_size % 4) {
		close(fd);
		snprintf(msg, sizeof(msg), "Firmware image size is not multiple of 4.");
		goto err;
	}

	firm_ctx->fw_image = spdk_zmalloc(firm_ctx->size, 4096, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!firm_ctx->fw_image) {
		close(fd);
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}
	firm_ctx->p = firm_ctx->fw_image;

	if (read(fd, firm_ctx->p, firm_ctx->size) != ((ssize_t)(firm_ctx->size))) {
		close(fd);
		snprintf(msg, sizeof(msg), "Read firmware image failed!");
		goto err;
	}
	close(fd);

	firm_ctx->offset = 0;
	firm_ctx->size_remaining = firm_ctx->size;
	firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);

	cmd = malloc(sizeof(struct spdk_nvme_cmd));
	if (!cmd) {
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}
	memset(cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

	cmd->cdw10 = (firm_ctx->transfer >> 2) - 1;
	cmd->cdw11 = firm_ctx->offset >> 2;

	rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, cmd, firm_ctx->p,
					   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
	if (rc == 0) {
		/* normal return here. */
		return;
	}

	free(cmd);
	snprintf(msg, sizeof(msg), "Read firmware image failed!");
err:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
	apply_firmware_cleanup(firm_ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_apply_firmware", rpc_bdev_nvme_apply_firmware, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_apply_firmware, apply_nvme_firmware)
