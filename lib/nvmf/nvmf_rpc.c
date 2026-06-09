/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2026, Oracle and/or its affiliates.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/hexlify.h"
#include "spdk/util.h"
#include "spdk/bit_array.h"
#include "spdk/config.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/rpc_autogen.h"

#include "nvmf_internal.h"

static int
decode_hex_string_be(const char *str, uint8_t *out, size_t size)
{
	char *bin;

	if (strlen(str) != size * 2) {
		return -EINVAL;
	}

	bin = spdk_unhexlify(str);
	if (bin == NULL) {
		return -EINVAL;
	}

	memcpy(out, bin, size);
	free(bin);

	return 0;
}

static struct spdk_nvmf_subsystem *
_rpc_nvmf_get_subsystem(struct spdk_jsonrpc_request *request,
			const char *tgt_name, const char *nqn,
			struct spdk_nvmf_tgt **tgt_out)
{
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_subsystem *subsystem;

	if (tgt_out) {
		*tgt_out = NULL;
	}

	tgt = spdk_nvmf_get_tgt(tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response_fmt(request,
						     tgt_name ? SPDK_JSONRPC_ERROR_INVALID_PARAMS : SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     tgt_name ? "NVMf target '%s' not found" : "No default NVMf target found; specify tgt_name",
						     tgt_name);
		return NULL;
	}

	if (tgt_out) {
		*tgt_out = tgt;
	}

	if (!nqn) {
		return NULL;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem with NQN '%s' not found", nqn);
		return NULL;
	}

	return subsystem;
}

static int
_rpc_nvmf_subsystem_pause(struct spdk_jsonrpc_request *request,
			  const char *tgt_name, const char *nqn, uint32_t nsid,
			  spdk_nvmf_subsystem_state_change_done pause_cb,
			  void *cb_arg,
			  struct spdk_nvmf_subsystem **subsystem_out,
			  struct spdk_nvmf_tgt **tgt_out)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = _rpc_nvmf_get_subsystem(request, tgt_name, nqn, tgt_out);
	if (!subsystem) {
		return -ENODEV;
	}

	if (subsystem_out) {
		*subsystem_out = subsystem;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nsid, pause_cb, cb_arg)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		return -EIO;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_nvmf_get_subsystems_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_get_subsystems_ctx, nqn), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_nvmf_get_subsystems_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host			*host;
	struct spdk_nvmf_subsystem_listener	*listener;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_name(w, "subtype");
	if (subsystem->opts.type == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_string(w, "NVMe");
	} else {
		spdk_json_write_string(w, "Discovery");
	}

	spdk_json_write_named_array_begin(w, "listen_addresses");

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		spdk_json_write_object_begin(w);
		nvmf_transport_listen_dump_trid(listener->trid, w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_bool(w, "allow_any_host",
				   spdk_nvmf_subsystem_get_allow_any_host(subsystem));

	spdk_json_write_named_array_begin(w, "hosts");

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "nqn", spdk_nvmf_host_get_nqn(host));
		if (host->dhchap_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key));
		}
		if (host->dhchap_ctrlr_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key));
		}
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	if (subsystem->opts.type == SPDK_NVMF_SUBTYPE_NVME) {
		struct spdk_nvmf_ns *ns;
		struct spdk_nvmf_ns_opts ns_opts;

		spdk_json_write_named_string(w, "serial_number", subsystem->opts.sn);
		spdk_json_write_named_string(w, "model_number", subsystem->opts.mn);

		if (subsystem->opts.max_namespaces != 0) {
			spdk_json_write_named_uint32(w, "max_namespaces", subsystem->opts.max_namespaces);
		}

		spdk_json_write_named_bool(w, "passthrough", subsystem->opts.passthrough);

		spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem));
		spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem));
		spdk_json_write_named_uint32(w, "dmrsl", subsystem->opts.dmrsl);
		spdk_json_write_named_uint8(w, "wzsl", subsystem->opts.wzsl);

		spdk_json_write_named_array_begin(w, "namespaces");
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));
			spdk_json_write_object_begin(w);
			spdk_json_write_named_int32(w, "nsid", spdk_nvmf_ns_get_id(ns));
			spdk_json_write_named_string(w, "bdev_name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			spdk_json_write_named_string(w, "name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

			if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
				spdk_json_write_named_bytearray(w, "nguid", ns_opts.nguid, sizeof(ns_opts.nguid));
			}

			if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
				spdk_json_write_named_bytearray(w, "eui64", ns_opts.eui64, sizeof(ns_opts.eui64));
			}

			if (!spdk_uuid_is_null(&ns_opts.uuid)) {
				spdk_json_write_named_uuid(w, "uuid", &ns_opts.uuid);
			}

			if (subsystem->opts.ana_reporting) {
				spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
			}

			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}
	spdk_json_write_object_end(w);
}

static void
rpc_nvmf_get_subsystems(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_nvmf_get_subsystems_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_subsystems_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_subsystems_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	subsystem = _rpc_nvmf_get_subsystem(request, req.tgt_name, req.nqn, &tgt);
	if (!tgt || (req.nqn && !subsystem)) {
		free_rpc_nvmf_get_subsystems(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
	} else {
		NVMF_SUBSYSTEM_FOREACH(tgt, subsystem) {
			dump_nvmf_subsystem(w, subsystem);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_nvmf_get_subsystems(&req);
}
SPDK_RPC_REGISTER("nvmf_get_subsystems", rpc_nvmf_get_subsystems, SPDK_RPC_RUNTIME)

SPDK_LOG_DEPRECATION_REGISTER(nvmf_create_subsystem_max_discard_size_kib,
			      "use dmrsl instead", "v26.09", SPDK_LOG_DEPRECATION_EVERY_24H);

SPDK_LOG_DEPRECATION_REGISTER(nvmf_create_subsystem_max_write_zeroes_size_kib,
			      "use wzsl instead", "v26.09", SPDK_LOG_DEPRECATION_EVERY_24H);

/*
 * X-macro list of fields shared between rpc_nvmf_create_subsystem_ctx
 * and spdk_nvmf_subsystem_opts.  Each entry is X(field).
 * serial_number/model_number are excluded because the ctx uses char*
 * while opts uses fixed-size char arrays (sn/mn).
 */
#define NVMF_CREATE_SUBSYSTEM_OPTS_FIELDS(X) \
	X(max_namespaces)                    \
	X(ana_reporting)                     \
	X(dmrsl)                             \
	X(wzsl)                              \
	X(passthrough)                       \
	X(enable_nssr)

/* Bump and audit NVMF_CREATE_SUBSYSTEM_OPTS_FIELDS when this size changes. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_subsystem_opts) == 88,
		   "opts grew -- update NVMF_CREATE_SUBSYSTEM_OPTS_FIELDS");

static const struct spdk_json_object_decoder rpc_nvmf_create_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_create_subsystem_ctx, nqn), spdk_json_decode_string},
	{"serial_number", offsetof(struct rpc_nvmf_create_subsystem_ctx, serial_number), spdk_json_decode_string, true},
	{"model_number", offsetof(struct rpc_nvmf_create_subsystem_ctx, model_number), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_nvmf_create_subsystem_ctx, tgt_name), spdk_json_decode_string, true},
	{"max_namespaces", offsetof(struct rpc_nvmf_create_subsystem_ctx, max_namespaces), spdk_json_decode_uint32, true},
	{"allow_any_host", offsetof(struct rpc_nvmf_create_subsystem_ctx, allow_any_host), spdk_json_decode_bool, true},
	{"ana_reporting", offsetof(struct rpc_nvmf_create_subsystem_ctx, ana_reporting), spdk_json_decode_bool, true},
	{"min_cntlid", offsetof(struct rpc_nvmf_create_subsystem_ctx, min_cntlid), spdk_json_decode_uint16, true},
	{"max_cntlid", offsetof(struct rpc_nvmf_create_subsystem_ctx, max_cntlid), spdk_json_decode_uint16, true},
	{"max_discard_size_kib", offsetof(struct rpc_nvmf_create_subsystem_ctx, max_discard_size_kib), rpc_decode_max_discard_size_kib, true},
	{"max_write_zeroes_size_kib", offsetof(struct rpc_nvmf_create_subsystem_ctx, max_write_zeroes_size_kib), rpc_decode_max_write_zeroes_size_kib, true},
	{"dmrsl", offsetof(struct rpc_nvmf_create_subsystem_ctx, dmrsl), spdk_json_decode_uint32, true},
	{"wzsl", offsetof(struct rpc_nvmf_create_subsystem_ctx, wzsl), spdk_json_decode_uint8, true},
	{"passthrough", offsetof(struct rpc_nvmf_create_subsystem_ctx, passthrough), spdk_json_decode_bool, true},
	{"enable_nssr", offsetof(struct rpc_nvmf_create_subsystem_ctx, enable_nssr), spdk_json_decode_bool, true},
};

static void
rpc_nvmf_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (!status) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Subsystem %s start failed",
						     subsystem->subnqn);
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}

static void
rpc_nvmf_create_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_nvmf_create_subsystem_ctx req = {};
	struct spdk_nvmf_subsystem_opts opts;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_tgt *tgt;
	int rc = -1;

	req.min_cntlid = NVMF_MIN_CNTLID;
	req.max_cntlid = NVMF_MAX_CNTLID;
	req.max_discard_size_kib = UINT64_MAX;
	req.max_write_zeroes_size_kib = UINT64_MAX;

	spdk_nvmf_subsystem_opts_init(SPDK_NVMF_SUBTYPE_NVME, &opts, sizeof(opts));
#define X(f) req.f = opts.f;
	NVMF_CREATE_SUBSYSTEM_OPTS_FIELDS(X)
#undef X

	if (spdk_json_decode_object(params, rpc_nvmf_create_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find target %s\n", req.tgt_name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to find target %s", req.tgt_name);
		goto cleanup;
	}

#define X(f) opts.f = req.f;
	NVMF_CREATE_SUBSYSTEM_OPTS_FIELDS(X)
#undef X

	if (req.serial_number != NULL) {
		rc = nvmf_subsystem_copy_sn(opts.sn, req.serial_number, sizeof(opts.sn));
		if (rc) {
			SPDK_ERRLOG("Invalid SN '%s'\n", req.serial_number);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto cleanup;
		}
	}

	if (req.model_number != NULL) {
		rc = nvmf_subsystem_copy_mn(opts.mn, req.model_number, sizeof(opts.mn));
		if (rc) {
			SPDK_ERRLOG("Invalid MN '%s'\n", req.model_number);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto cleanup;
		}
	}

	if (req.max_discard_size_kib != UINT64_MAX) {
		/* Convert KiB to logical blocks assuming 512B block size. */
		opts.dmrsl = req.max_discard_size_kib << 1;
	}

	if (req.max_write_zeroes_size_kib != UINT64_MAX) {
		/* Convert max_write_zeroes_size_kib to wzsl.
		 * wzsl is in units of minimum memory page size (4 KiB when mpsmin=0),
		 * reported as a power of two (2^wzsl). Valid KiB values: 0 (no limit)
		 * or power of 2 >= 8.
		 */
		if (req.max_write_zeroes_size_kib == 0) {
			opts.wzsl = 0;
		} else if (req.max_write_zeroes_size_kib >= 8 &&
			   spdk_u64_is_pow2(req.max_write_zeroes_size_kib)) {
			opts.wzsl = spdk_u64log2(req.max_write_zeroes_size_kib >> 2);
		} else {
			SPDK_ERRLOG("Subsystem %s: invalid "
				    "max_write_zeroes_size_kib %"PRIu64"\n",
				    req.nqn, req.max_write_zeroes_size_kib);
			spdk_jsonrpc_send_error_response_fmt(request,
							     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid max_write_zeroes_size_kib %"PRIu64,
							     req.max_write_zeroes_size_kib);
			goto cleanup;
		}
	}

	subsystem = spdk_nvmf_subsystem_create_ext(tgt, req.nqn, SPDK_NVMF_SUBTYPE_NVME, &opts);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to create subsystem %s\n", req.nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to create subsystem %s", req.nqn);
		goto cleanup;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req.allow_any_host);

	if (spdk_nvmf_subsystem_set_cntlid_range(subsystem, req.min_cntlid, req.max_cntlid)) {
		SPDK_ERRLOG("Subsystem %s: invalid cntlid range [%u-%u]\n", req.nqn, req.min_cntlid,
			    req.max_cntlid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cntlid range [%u-%u]", req.min_cntlid, req.max_cntlid);
		goto cleanup;
	}

	rc = spdk_nvmf_subsystem_start(subsystem,
				       rpc_nvmf_subsystem_started,
				       request);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to start subsystem");
	}

cleanup:
	free_rpc_nvmf_create_subsystem(&req);

	if (rc && subsystem) {
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}
SPDK_RPC_REGISTER("nvmf_create_subsystem", rpc_nvmf_create_subsystem, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_destroy_complete_cb(void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	int rc;

	/* If the stop was cancelled (e.g., subsystem is being destroyed), don't proceed */
	if (status != 0) {
		SPDK_ERRLOG("Subsystem stop failed or was cancelled, status %d\n", status);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Subsystem stop failed or was cancelled, status %d", status);
		return;
	}

	nvmf_subsystem_remove_all_listeners(subsystem, true);
	rc = spdk_nvmf_subsystem_destroy(subsystem, rpc_nvmf_subsystem_destroy_complete_cb, request);
	if (rc) {
		if (rc == -EINPROGRESS) {
			/* response will be sent in completion callback */
			return;
		} else {
			SPDK_ERRLOG("Subsystem destruction failed, rc %d\n", rc);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Subsystem destruction failed, rc %d", rc);
			return;
		}
	}
	spdk_jsonrpc_send_bool_response(request, true);
}

static const struct spdk_json_object_decoder rpc_nvmf_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_delete_subsystem_ctx, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_delete_subsystem_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_delete_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_nvmf_delete_subsystem_ctx req = {};
	struct spdk_nvmf_subsystem *subsystem;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_delete_subsystem_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nqn == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, req.tgt_name, req.nqn, NULL);
	if (!subsystem) {
		goto invalid_custom_response;
	}

	free_rpc_nvmf_delete_subsystem(&req);

	rc = spdk_nvmf_subsystem_stop_for_destroy(subsystem,
			rpc_nvmf_subsystem_stopped,
			request);
	if (rc == -ENODEV) {
		SPDK_ERRLOG("Subsystem is already being destroyed.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Subsystem is already being destroyed.");
	} else if (rc == -EBUSY) {
		SPDK_ERRLOG("Subsystem currently in another state change try again later.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Subsystem currently in another state change try again later.");
	} else if (rc != 0) {
		SPDK_ERRLOG("Unable to change state on subsystem. rc=%d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to change state on subsystem. rc=%d", rc);
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
invalid_custom_response:
	free_rpc_nvmf_delete_subsystem(&req);
}
SPDK_RPC_REGISTER("nvmf_delete_subsystem", rpc_nvmf_delete_subsystem, SPDK_RPC_RUNTIME)

enum nvmf_rpc_listen_op {
	NVMF_RPC_LISTEN_ADD,
	NVMF_RPC_LISTEN_REMOVE,
	NVMF_RPC_LISTEN_SET_ANA_STATE,
};

/* TODO: replace with rpc_nvmf_subsystem_add_listener_ctx */
struct nvmf_rpc_listener_ctx {
	char				*nqn;
	char				*tgt_name;
	struct spdk_nvmf_tgt		*tgt;
	struct spdk_nvmf_transport	*transport;
	struct spdk_nvmf_subsystem	*subsystem;
	struct rpc_nvmf_listen_address	address;
	enum spdk_nvme_ana_state	ana_state;
	uint32_t			anagrpid;

	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_transport_id	trid;
	enum nvmf_rpc_listen_op		op;
	bool				response_sent;
	struct spdk_nvmf_listen_opts	opts;

	/* Hole at bytes 705-711 */
	uint8_t reserved1[7];

	/* Additional options for listener creation.
	 * Must be 8-byte aligned. */
	struct spdk_nvmf_listener_opts	listener_opts;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), rpc_decode_nvmf_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.secure_channel), spdk_json_decode_bool, true},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state), rpc_decode_nvme_ana_state, true},
	{"sock_impl", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.sock_impl), spdk_json_decode_string, true},
};

static void
nvmf_rpc_listener_ctx_free(struct nvmf_rpc_listener_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free_rpc_nvmf_listen_address(&ctx->address);
	free(ctx);
}

static void
nvmf_rpc_listen_resumed(struct spdk_nvmf_subsystem *subsystem,
			void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;

	request = ctx->request;
	if (ctx->response_sent) {
		/* If an error occurred, the response has already been sent. */
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	nvmf_rpc_listener_ctx_free(ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_subsystem_listen(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		/* Destroy the listener that we just created. Ignore the error code because
		 * the RPC is failing already anyway. */
		spdk_nvmf_tgt_stop_listen(ctx->tgt, &ctx->trid);

		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}
static void
nvmf_rpc_stop_listen_async_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to stop listener.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error stopping listener: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_set_ana_state_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to set ANA state.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error setting ANA state: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_listen_paused(struct spdk_nvmf_subsystem *subsystem,
		       void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	int rc;

	switch (ctx->op) {
	case NVMF_RPC_LISTEN_ADD:
		if (nvmf_subsystem_find_listener(subsystem, &ctx->trid)) {
			SPDK_ERRLOG("Listener already exists\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		rc = spdk_nvmf_tgt_listen_ext(ctx->tgt, &ctx->trid, &ctx->opts);
		if (rc) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_subsystem_add_listener_ext(ctx->subsystem, &ctx->trid, nvmf_rpc_subsystem_listen, ctx,
						     &ctx->listener_opts);
		return;
	case NVMF_RPC_LISTEN_REMOVE:
		rc = spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);
		if (rc) {
			SPDK_ERRLOG("Unable to remove listener, rc %d\n", rc);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_transport_stop_listen_async(ctx->transport, &ctx->trid, subsystem,
						      nvmf_rpc_stop_listen_async_done, ctx);
		return;
	case NVMF_RPC_LISTEN_SET_ANA_STATE:
		spdk_nvmf_subsystem_set_ana_state(subsystem, &ctx->trid, ctx->ana_state, ctx->anagrpid,
						  nvmf_rpc_set_ana_state_done, ctx);
		return;
	default:
		SPDK_UNREACHABLE();
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}

		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static int
rpc_listen_address_to_trid(const struct rpc_nvmf_listen_address *address,
			   struct spdk_nvme_transport_id *trid)
{
	size_t len;

	memset(trid, 0, sizeof(*trid));

	if (spdk_nvme_transport_id_populate_trstring(trid, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype string: %s\n", address->trtype);
		return -EINVAL;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype type: %s\n", address->trtype);
		return -EINVAL;
	}

	if (address->adrfam) {
		if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, address->adrfam)) {
			SPDK_ERRLOG("Invalid adrfam: %s\n", address->adrfam);
			return -EINVAL;
		}
	} else if (trid->trtype == SPDK_NVME_TRANSPORT_TCP || trid->trtype == SPDK_NVME_TRANSPORT_RDMA) {
		/**
		 * For backward compatibility, if adrfam is not specified for TCP or RDMA transport, assume IPv4.
		 */
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	}

	len = strlen(address->traddr);
	if (len > sizeof(trid->traddr) - 1) {
		SPDK_ERRLOG("Transport address longer than %zu characters: %s\n",
			    sizeof(trid->traddr) - 1, address->traddr);
		return -EINVAL;
	}
	memcpy(trid->traddr, address->traddr, len + 1);

	trid->trsvcid[0] = '\0';
	if (address->trsvcid) {
		len = strlen(address->trsvcid);
		if (len > sizeof(trid->trsvcid) - 1) {
			SPDK_ERRLOG("Transport service id longer than %zu characters: %s\n",
				    sizeof(trid->trsvcid) - 1, address->trsvcid);
			return -EINVAL;
		}
		memcpy(trid->trsvcid, address->trsvcid, len + 1);
	}

	return 0;
}

static void
rpc_nvmf_subsystem_add_listener(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	spdk_nvmf_subsystem_listener_opts_init(&ctx->listener_opts, sizeof(ctx->listener_opts));

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_listener_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_listener_decoders),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx->tgt_name, ctx->nqn, &tgt);
	if (!subsystem) {
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;
	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_ADD;
	spdk_nvmf_listen_opts_init(&ctx->opts, sizeof(ctx->opts));
	ctx->opts.transport_specific = params;
	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem) && ctx->listener_opts.secure_channel) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Cannot establish secure channel, when 'allow_any_host' is set");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->opts.secure_channel = ctx->listener_opts.secure_channel;

	if (ctx->ana_state) {
		ctx->listener_opts.ana_state = ctx->ana_state;
	}

	ctx->opts.sock_impl = ctx->listener_opts.sock_impl;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", rpc_nvmf_subsystem_add_listener,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_listener_decoders[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), rpc_decode_nvmf_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_remove_listener(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_listener_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_listener_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx->tgt_name, ctx->nqn, &tgt);
	if (!subsystem) {
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;
	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->transport = spdk_nvmf_tgt_get_transport(tgt, ctx->trid.trstring);
	if (!ctx->transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    ctx->trid.trstring);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_REMOVE;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", rpc_nvmf_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_discovery_add_referral_decoders[] = {
	{"address", offsetof(struct rpc_nvmf_discovery_add_referral_ctx, address), rpc_decode_nvmf_listen_address},
	{"tgt_name", offsetof(struct rpc_nvmf_discovery_add_referral_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct rpc_nvmf_discovery_add_referral_ctx, secure_channel), spdk_json_decode_bool, true},
	{"subnqn", offsetof(struct rpc_nvmf_discovery_add_referral_ctx, subnqn), spdk_json_decode_string, true},
};

static void
rpc_nvmf_discovery_add_referral(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_nvmf_discovery_add_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_referral_opts opts = {};
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_discovery_add_referral_decoders,
					    SPDK_COUNTOF(rpc_nvmf_discovery_add_referral_decoders),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_discovery_add_referral(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_discovery_add_referral(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_nvmf_discovery_add_referral(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			free_rpc_nvmf_discovery_add_referral(&ctx);
			return;
		}
	}

	if ((trid.trtype == SPDK_NVME_TRANSPORT_TCP ||
	     trid.trtype == SPDK_NVME_TRANSPORT_RDMA) &&
	    !strlen(trid.trsvcid)) {
		SPDK_ERRLOG("Service ID is required.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Service ID is required.");
		free_rpc_nvmf_discovery_add_referral(&ctx);
		return;
	}

	opts.size = SPDK_SIZEOF(&opts, allow_any_host);
	opts.trid = trid;
	opts.secure_channel = ctx.secure_channel;
	opts.allow_any_host = true;

	rc = spdk_nvmf_tgt_add_referral(tgt, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		free_rpc_nvmf_discovery_add_referral(&ctx);
		return;
	}

	free_rpc_nvmf_discovery_add_referral(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_add_referral", rpc_nvmf_discovery_add_referral, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_discovery_remove_referral_decoders[] = {
	{"address", offsetof(struct rpc_nvmf_discovery_remove_referral_ctx, address), rpc_decode_nvmf_listen_address},
	{"tgt_name", offsetof(struct rpc_nvmf_discovery_remove_referral_ctx, tgt_name), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_nvmf_discovery_remove_referral_ctx, subnqn), spdk_json_decode_string, true},
};

static void
rpc_nvmf_discovery_remove_referral(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_nvmf_discovery_remove_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_referral_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_discovery_remove_referral_decoders,
				    SPDK_COUNTOF(rpc_nvmf_discovery_remove_referral_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_discovery_remove_referral(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_discovery_remove_referral(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_nvmf_discovery_remove_referral(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			free_rpc_nvmf_discovery_remove_referral(&ctx);
			return;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, secure_channel);
	opts.trid = trid;

	if (spdk_nvmf_tgt_remove_referral(tgt, &opts)) {
		SPDK_ERRLOG("Failed to remove referral.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to remove a referral.");
		free_rpc_nvmf_discovery_remove_referral(&ctx);
		return;
	}

	free_rpc_nvmf_discovery_remove_referral(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_remove_referral", rpc_nvmf_discovery_remove_referral,
		  SPDK_RPC_RUNTIME);

static void
dump_nvmf_referral(struct spdk_json_write_ctx *w,
		   struct spdk_nvmf_referral *referral)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(&referral->trid, w);
	spdk_json_write_object_end(w);
	spdk_json_write_named_bool(w, "secure_channel",
				   referral->entry.treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED);
	spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn);

	spdk_json_write_object_end(w);
}

static const struct spdk_json_object_decoder rpc_nvmf_discovery_get_referrals_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_discovery_get_referrals_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_discovery_get_referrals(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_nvmf_discovery_get_referrals_ctx req = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_referral *referral;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_discovery_get_referrals_decoders,
					    SPDK_COUNTOF(rpc_nvmf_discovery_get_referrals_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			free_rpc_nvmf_discovery_get_referrals(&req);
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_nvmf_discovery_get_referrals(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		dump_nvmf_referral(w, referral);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	free_rpc_nvmf_discovery_get_referrals(&req);
}
SPDK_RPC_REGISTER("nvmf_discovery_get_referrals", rpc_nvmf_discovery_get_referrals,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_listener_set_ana_state_decoders[] =
{
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), rpc_decode_nvmf_listen_address},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state), rpc_decode_nvme_ana_state},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"anagrpid", offsetof(struct nvmf_rpc_listener_ctx, anagrpid), spdk_json_decode_uint32, true},
};

static void
rpc_nvmf_subsystem_listener_set_ana_state(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_listener_set_ana_state_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_listener_set_ana_state_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_SET_ANA_STATE;

	if (_rpc_nvmf_subsystem_pause(request, ctx->tgt_name, ctx->nqn, 0,
				      nvmf_rpc_listen_paused, ctx, &ctx->subsystem, &ctx->tgt)) {
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_listener_set_ana_state",
		  rpc_nvmf_subsystem_listener_set_ana_state, SPDK_RPC_RUNTIME);

SPDK_LOG_DEPRECATION_REGISTER(nvmf_namespace_hide_metadata,
			      "use transport dif_insert_or_strip option instead",
			      "v26.09", SPDK_LOG_DEPRECATION_EVERY_24H);

struct rpc_nvmf_subsystem_add_ns_ext {
	struct rpc_nvmf_subsystem_add_ns_ctx req;
	const struct spdk_json_val *params;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_ns_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_add_ns_ctx, nqn), spdk_json_decode_string},
	{"namespace", offsetof(struct rpc_nvmf_subsystem_add_ns_ctx, namespace), rpc_decode_nvmf_namespace},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_add_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_subsystem_add_ns_ext(struct rpc_nvmf_subsystem_add_ns_ext *ctx)
{
	free_rpc_nvmf_subsystem_add_ns(&ctx->req);
	free(ctx);
}

static void
nvmf_rpc_ns_failback_resumed(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_add_ns_ext *ereq = cb_arg;

	spdk_jsonrpc_send_error_response_fmt(ereq->req.request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					     "Unable to add ns, subsystem in %s state",
					     status ? "invalid" : "active");

	free_rpc_nvmf_subsystem_add_ns_ext(ereq);
}

static void
rpc_nvmf_subsystem_add_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
				  void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_add_ns_ext *ereq = cb_arg;
	struct spdk_jsonrpc_request *request = ereq->req.request;
	uint32_t nsid = ereq->req.namespace.nsid;
	bool response_sent = ereq->response_sent;
	struct spdk_json_write_ctx *w;
	int rc;

	/* The case where the call to add the namespace was successful, but the subsystem couldn't be resumed. */
	if (status && !ereq->response_sent) {
		rc = spdk_nvmf_subsystem_remove_ns(subsystem, nsid);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to add ns, subsystem in invalid state");
			free_rpc_nvmf_subsystem_add_ns_ext(ereq);
			return;
		}

		rc = spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_failback_resumed, ereq);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
			free_rpc_nvmf_subsystem_add_ns_ext(ereq);
			return;
		}

		return;
	}

	free_rpc_nvmf_subsystem_add_ns_ext(ereq);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uint32(w, nsid);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_nvmf_subsystem_add_ns_paused(struct spdk_nvmf_subsystem *subsystem,
				 void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_add_ns_ext *ereq = cb_arg;
	struct rpc_nvmf_subsystem_add_ns_ctx *ctx = &ereq->req;
	struct spdk_nvmf_ns_opts ns_opts;

	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = ctx->namespace.nsid;
	ns_opts.transport_specific = ereq->params;

	if (ctx->namespace.nguid != NULL) {
		if (decode_hex_string_be(ctx->namespace.nguid, ns_opts.nguid, sizeof(ns_opts.nguid))) {
			SPDK_ERRLOG("Invalid nguid: %s\n", ctx->namespace.nguid);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid nguid");
			ereq->response_sent = true;
			goto resume;
		}
	}

	if (ctx->namespace.eui64 != NULL) {
		if (decode_hex_string_be(ctx->namespace.eui64, ns_opts.eui64, sizeof(ns_opts.eui64))) {
			SPDK_ERRLOG("Invalid eui64: %s\n", ctx->namespace.eui64);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid eui64");
			ereq->response_sent = true;
			goto resume;
		}
	}

	if (!spdk_uuid_is_null(&ctx->namespace.uuid)) {
		ns_opts.uuid = ctx->namespace.uuid;
	}

	ns_opts.anagrpid = ctx->namespace.anagrpid;
	ns_opts.no_auto_visible = ctx->namespace.no_auto_visible;
	ns_opts.hide_metadata = ctx->namespace.hide_metadata;

	ctx->namespace.nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, ctx->namespace.bdev_name,
			      &ns_opts, sizeof(ns_opts),
			      ctx->namespace.ptpl_file);
	if (ctx->namespace.nsid == 0) {
		SPDK_ERRLOG("Unable to add namespace\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ereq->response_sent = true;
		goto resume;
	}

resume:
	if (spdk_nvmf_subsystem_resume(subsystem, rpc_nvmf_subsystem_add_ns_resumed, ereq)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		free_rpc_nvmf_subsystem_add_ns_ext(ereq);
	}
}

static void
rpc_nvmf_subsystem_add_ns(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_add_ns_ext *ereq;
	struct rpc_nvmf_subsystem_add_ns_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req = &ereq->req;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_ns_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_ns_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_subsystem_add_ns_ext(ereq);
		return;
	}

	req->request = request;
	ereq->params = params;
	ereq->response_sent = false;

	if (_rpc_nvmf_subsystem_pause(request, req->tgt_name, req->nqn, req->namespace.nsid,
				      rpc_nvmf_subsystem_add_ns_paused, ereq, NULL, NULL)) {
		free_rpc_nvmf_subsystem_add_ns_ext(ereq);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", rpc_nvmf_subsystem_add_ns, SPDK_RPC_RUNTIME)

struct rpc_nvmf_subsystem_set_ns_ana_group_ext {
	struct rpc_nvmf_subsystem_set_ns_ana_group_ctx req;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_set_ns_ana_group_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_set_ns_ana_group_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_nvmf_subsystem_set_ns_ana_group_ctx, nsid), spdk_json_decode_uint32},
	{"anagrpid", offsetof(struct rpc_nvmf_subsystem_set_ns_ana_group_ctx, anagrpid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_set_ns_ana_group_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_subsystem_set_ns_ana_group_ext(struct rpc_nvmf_subsystem_set_ns_ana_group_ext *ereq)
{
	free_rpc_nvmf_subsystem_set_ns_ana_group(&ereq->req);
	free(ereq);
}

static void
nvmf_rpc_anagrpid_resumed(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_set_ns_ana_group_ext *ereq = cb_arg;

	if (!ereq->response_sent) {
		spdk_jsonrpc_send_bool_response(ereq->req.request, true);
	}

	free_rpc_nvmf_subsystem_set_ns_ana_group_ext(ereq);
}

static void
nvmf_rpc_ana_group(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_set_ns_ana_group_ext *ereq = cb_arg;
	struct rpc_nvmf_subsystem_set_ns_ana_group_ctx *req = &ereq->req;
	struct spdk_jsonrpc_request *request = req->request;
	int rc;

	rc = spdk_nvmf_subsystem_set_ns_ana_group(subsystem, req->nsid, req->anagrpid);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to change ANA group ID\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ereq->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_anagrpid_resumed, ereq)) {
		if (!ereq->response_sent) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		free_rpc_nvmf_subsystem_set_ns_ana_group_ext(ereq);
	}
}

static void
rpc_nvmf_subsystem_set_ns_ana_group(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_set_ns_ana_group_ext *ereq;
	struct rpc_nvmf_subsystem_set_ns_ana_group_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req = &ereq->req;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_set_ns_ana_group_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_set_ns_ana_group_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_subsystem_set_ns_ana_group_ext(ereq);
		return;
	}

	req->request = request;
	ereq->response_sent = false;

	if (_rpc_nvmf_subsystem_pause(request, req->tgt_name, req->nqn, req->nsid,
				      nvmf_rpc_ana_group, ereq, NULL, NULL)) {
		free_rpc_nvmf_subsystem_set_ns_ana_group_ext(ereq);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_ns_ana_group", rpc_nvmf_subsystem_set_ns_ana_group,
		  SPDK_RPC_RUNTIME)

struct rpc_nvmf_subsystem_remove_ns_ext {
	struct rpc_nvmf_subsystem_remove_ns_ctx req;
	bool response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_ns_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_remove_ns_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_nvmf_subsystem_remove_ns_ctx, nsid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_remove_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_subsystem_remove_ns_ext(struct rpc_nvmf_subsystem_remove_ns_ext *ereq)
{
	free_rpc_nvmf_subsystem_remove_ns(&ereq->req);
	free(ereq);
}

static void
rpc_nvmf_subsystem_remove_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
				     void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_remove_ns_ext *ereq = cb_arg;

	if (!ereq->response_sent) {
		spdk_jsonrpc_send_bool_response(ereq->req.request, true);
	}

	free_rpc_nvmf_subsystem_remove_ns_ext(ereq);
}

static void
rpc_nvmf_subsystem_remove_ns_paused(struct spdk_nvmf_subsystem *subsystem,
				    void *cb_arg, int status)
{
	struct rpc_nvmf_subsystem_remove_ns_ext *ereq = cb_arg;
	struct rpc_nvmf_subsystem_remove_ns_ctx *req = &ereq->req;
	struct spdk_jsonrpc_request *request = req->request;
	int ret;

	ret = spdk_nvmf_subsystem_remove_ns(subsystem, req->nsid);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", req->nsid);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ereq->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, rpc_nvmf_subsystem_remove_ns_resumed, ereq)) {
		if (!ereq->response_sent) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		free_rpc_nvmf_subsystem_remove_ns_ext(ereq);
	}
}

static void
rpc_nvmf_subsystem_remove_ns(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_remove_ns_ext *ereq;
	struct rpc_nvmf_subsystem_remove_ns_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req = &ereq->req;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_ns_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_ns_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_subsystem_remove_ns_ext(ereq);
		return;
	}

	req->request = request;
	ereq->response_sent = false;

	if (_rpc_nvmf_subsystem_pause(request, req->tgt_name, req->nqn, req->nsid,
				      rpc_nvmf_subsystem_remove_ns_paused, ereq, NULL, NULL)) {
		free_rpc_nvmf_subsystem_remove_ns_ext(ereq);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", rpc_nvmf_subsystem_remove_ns, SPDK_RPC_RUNTIME)

struct rpc_nvmf_ns_add_host_ext {
	struct rpc_nvmf_ns_add_host_ctx	req;
	bool				response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_ns_add_host_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_ns_add_host_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_nvmf_ns_add_host_ctx, nsid), spdk_json_decode_uint32},
	{"host", offsetof(struct rpc_nvmf_ns_add_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_ns_add_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_ns_add_host_ext(struct rpc_nvmf_ns_add_host_ext *ereq)
{
	free_rpc_nvmf_ns_add_host(&ereq->req);
	free(ereq);
}

static void
rpc_nvmf_ns_add_host_resumed(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	struct rpc_nvmf_ns_add_host_ext *ereq = cb_arg;

	if (!ereq->response_sent) {
		spdk_jsonrpc_send_bool_response(ereq->req.request, true);
	}

	free_rpc_nvmf_ns_add_host_ext(ereq);
}

static void
rpc_nvmf_ns_add_host_paused(struct spdk_nvmf_subsystem *subsystem,
			    void *cb_arg, int status)
{
	struct rpc_nvmf_ns_add_host_ext *ereq = cb_arg;
	struct rpc_nvmf_ns_add_host_ctx *req = &ereq->req;
	int ret;

	ret = spdk_nvmf_ns_add_host(subsystem, req->nsid, req->host, 0);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to add %s to namespace ID %u\n", req->host, req->nsid);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ereq->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, rpc_nvmf_ns_add_host_resumed, ereq)) {
		if (!ereq->response_sent) {
			spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		free_rpc_nvmf_ns_add_host_ext(ereq);
	}
}

static void
rpc_nvmf_ns_add_host(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_nvmf_ns_add_host_ext *ereq;
	struct rpc_nvmf_ns_add_host_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req = &ereq->req;

	if (spdk_json_decode_object(params, rpc_nvmf_ns_add_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_ns_add_host_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_ns_add_host_ext(ereq);
		return;
	}
	req->request = request;

	if (_rpc_nvmf_subsystem_pause(request, req->tgt_name, req->nqn, req->nsid,
				      rpc_nvmf_ns_add_host_paused, ereq, NULL, NULL)) {
		free_rpc_nvmf_ns_add_host_ext(ereq);
	}
}
SPDK_RPC_REGISTER("nvmf_ns_add_host", rpc_nvmf_ns_add_host, SPDK_RPC_RUNTIME)

struct rpc_nvmf_ns_remove_host_ext {
	struct rpc_nvmf_ns_remove_host_ctx	req;
	bool					response_sent;
};

static const struct spdk_json_object_decoder rpc_nvmf_ns_remove_host_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_ns_remove_host_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_nvmf_ns_remove_host_ctx, nsid), spdk_json_decode_uint32},
	{"host", offsetof(struct rpc_nvmf_ns_remove_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_ns_remove_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_ns_remove_host_ext(struct rpc_nvmf_ns_remove_host_ext *ereq)
{
	free_rpc_nvmf_ns_remove_host(&ereq->req);
	free(ereq);
}

static void
rpc_nvmf_ns_remove_host_resumed(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct rpc_nvmf_ns_remove_host_ext *ereq = cb_arg;

	if (!ereq->response_sent) {
		spdk_jsonrpc_send_bool_response(ereq->req.request, true);
	}

	free_rpc_nvmf_ns_remove_host_ext(ereq);
}

static void
rpc_nvmf_ns_remove_host_paused(struct spdk_nvmf_subsystem *subsystem,
			       void *cb_arg, int status)
{
	struct rpc_nvmf_ns_remove_host_ext *ereq = cb_arg;
	struct rpc_nvmf_ns_remove_host_ctx *req = &ereq->req;
	int ret;

	ret = spdk_nvmf_ns_remove_host(subsystem, req->nsid, req->host, 0);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to remove %s from namespace ID %u\n", req->host, req->nsid);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ereq->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, rpc_nvmf_ns_remove_host_resumed, ereq)) {
		if (!ereq->response_sent) {
			spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		free_rpc_nvmf_ns_remove_host_ext(ereq);
	}
}

static void
rpc_nvmf_ns_remove_host(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_nvmf_ns_remove_host_ext *ereq;
	struct rpc_nvmf_ns_remove_host_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req = &ereq->req;

	if (spdk_json_decode_object(params, rpc_nvmf_ns_remove_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_ns_remove_host_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_ns_remove_host_ext(ereq);
		return;
	}
	req->request = request;

	if (_rpc_nvmf_subsystem_pause(request, req->tgt_name, req->nqn, req->nsid,
				      rpc_nvmf_ns_remove_host_paused, ereq, NULL, NULL)) {
		free_rpc_nvmf_ns_remove_host_ext(ereq);
	}
}
SPDK_RPC_REGISTER("nvmf_ns_remove_host", rpc_nvmf_ns_remove_host, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_add_host_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_add_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct rpc_nvmf_subsystem_add_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_add_host_ctx, tgt_name), spdk_json_decode_string, true},
	{"dhchap_key", offsetof(struct rpc_nvmf_subsystem_add_host_ctx, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct rpc_nvmf_subsystem_add_host_ctx, dhchap_ctrlr_key), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_add_host(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_add_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_host_opts opts = {};
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_subsystem_add_host_decoders,
					    SPDK_COUNTOF(rpc_nvmf_subsystem_add_host_decoders),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx.tgt_name, ctx.nqn, NULL);
	if (!subsystem) {
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.params = params;
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_add_host_ext(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	free_rpc_nvmf_subsystem_add_host(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_host", rpc_nvmf_subsystem_add_host, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_remove_host_done(void *_ctx, int status)
{
	struct rpc_nvmf_subsystem_remove_host_ctx *ctx = _ctx;

	if (status != 0) {
		if (status == -ETIMEDOUT) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Timeout reached during host removal");
		} else {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}
	free_rpc_nvmf_subsystem_remove_host(ctx);
	free(ctx);
}

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_remove_host_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_remove_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct rpc_nvmf_subsystem_remove_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_remove_host_ctx, tgt_name), spdk_json_decode_string, true},
	{"timeout_ms", offsetof(struct rpc_nvmf_subsystem_remove_host_ctx, timeout_ms), spdk_json_decode_uint64, true},
};

static void
rpc_nvmf_subsystem_remove_host(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_remove_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate context to perform RPC\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_remove_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_remove_host_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_subsystem_remove_host(ctx);
		free(ctx);
		return;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx->tgt_name, ctx->nqn, NULL);
	if (!subsystem) {
		free_rpc_nvmf_subsystem_remove_host(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_remove_host(subsystem, ctx->host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		free_rpc_nvmf_subsystem_remove_host(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_disconnect_host(subsystem, ctx->host,
			rpc_nvmf_subsystem_remove_host_done,
			ctx, ctx->timeout_ms);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		free_rpc_nvmf_subsystem_remove_host(ctx);
		free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_host", rpc_nvmf_subsystem_remove_host,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_set_keys_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_set_keys_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct rpc_nvmf_subsystem_set_keys_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_set_keys_ctx, tgt_name), spdk_json_decode_string, true},
	{"dhchap_key", offsetof(struct rpc_nvmf_subsystem_set_keys_ctx, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct rpc_nvmf_subsystem_set_keys_ctx, dhchap_ctrlr_key), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_set_keys(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_set_keys_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_key_opts opts = {};
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_set_keys_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_set_keys_decoders), &ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx.tgt_name, ctx.nqn, NULL);
	if (!subsystem) {
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}
	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_set_keys(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	free_rpc_nvmf_subsystem_set_keys(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_keys", rpc_nvmf_subsystem_set_keys, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_subsystem_allow_any_host_decoders[] = {
	{"nqn", offsetof(struct rpc_nvmf_subsystem_allow_any_host_ctx, nqn), spdk_json_decode_string},
	{"allow_any_host", offsetof(struct rpc_nvmf_subsystem_allow_any_host_ctx, allow_any_host), spdk_json_decode_bool},
	{"tgt_name", offsetof(struct rpc_nvmf_subsystem_allow_any_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_allow_any_host(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_nvmf_subsystem_allow_any_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvmf_subsystem_allow_any_host_decoders,
				    SPDK_COUNTOF(rpc_nvmf_subsystem_allow_any_host_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_subsystem_allow_any_host(&ctx);
		return;
	}

	subsystem = _rpc_nvmf_get_subsystem(request, ctx.tgt_name, ctx.nqn, NULL);
	if (!subsystem) {
		free_rpc_nvmf_subsystem_allow_any_host(&ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_set_allow_any_host(subsystem, ctx.allow_any_host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		free_rpc_nvmf_subsystem_allow_any_host(&ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_nvmf_subsystem_allow_any_host(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_allow_any_host", rpc_nvmf_subsystem_allow_any_host,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_create_target_decoders[] = {
	{"name", offsetof(struct rpc_nvmf_create_target_ctx, name), spdk_json_decode_string},
	{"max_subsystems", offsetof(struct rpc_nvmf_create_target_ctx, max_subsystems), spdk_json_decode_uint32, true},
	{"discovery_filter", offsetof(struct rpc_nvmf_create_target_ctx, discovery_filter), rpc_decode_nvmf_discovery_filters, true},
	{"dup_host_policy", offsetof(struct rpc_nvmf_create_target_ctx, dup_host_policy), rpc_decode_nvmf_dup_host_policy, true},
};

static void
rpc_nvmf_create_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_nvmf_target_opts		opts;
	struct rpc_nvmf_create_target_ctx	ctx = {};
	struct spdk_nvmf_tgt			*tgt;
	struct spdk_json_write_ctx		*w;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_create_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_create_target_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	snprintf(opts.name, NVMF_TGT_NAME_MAX_LENGTH, "%s", ctx.name);
	opts.max_subsystems = ctx.max_subsystems;
	opts.discovery_filter = ctx.discovery_filter;
	opts.dup_host_policy = (enum spdk_nvmf_subsystem_dup_host_policy)ctx.dup_host_policy;
	opts.size = SPDK_SIZEOF(&opts, dup_host_policy);

	if (spdk_nvmf_get_tgt(opts.name) != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Target already exists.");
		goto out;
	}

	tgt = spdk_nvmf_tgt_create(&opts);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create the requested target.");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_nvmf_tgt_get_name(tgt));
	spdk_jsonrpc_end_result(request, w);
out:
	free_rpc_nvmf_create_target(&ctx);
}
/* private */ SPDK_RPC_REGISTER("nvmf_create_target", rpc_nvmf_create_target, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_delete_target_decoders[] = {
	{"name", offsetof(struct rpc_nvmf_delete_target_ctx, name), spdk_json_decode_string},
};

static void
nvmf_rpc_destroy_target_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request	*request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_delete_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_nvmf_delete_target_ctx	ctx = {};
	struct spdk_nvmf_tgt			*tgt;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, rpc_nvmf_delete_target_decoders,
				    SPDK_COUNTOF(rpc_nvmf_delete_target_decoders),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_delete_target(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.name);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The specified target doesn't exist, cannot delete it.");
		free_rpc_nvmf_delete_target(&ctx);
		return;
	}

	spdk_nvmf_tgt_destroy(tgt, nvmf_rpc_destroy_target_done, request);
	free_rpc_nvmf_delete_target(&ctx);
}
/* private */ SPDK_RPC_REGISTER("nvmf_delete_target", rpc_nvmf_delete_target, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_get_targets(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx	*w;
	struct spdk_nvmf_tgt		*tgt;
	const char			*name;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "nvmf_get_targets has no parameters.");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	tgt = spdk_nvmf_get_first_tgt();

	while (tgt != NULL) {
		name = spdk_nvmf_tgt_get_name(tgt);
		spdk_json_write_string(w, name);
		tgt = spdk_nvmf_get_next_tgt(tgt);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
/* private */ SPDK_RPC_REGISTER("nvmf_get_targets", rpc_nvmf_get_targets, SPDK_RPC_RUNTIME);

/* TODO: replace with rpc_nvmf_create_transport_ctx */
struct rpc_nvmf_create_transport_ext {
	char				*trtype;
	char				*tgt_name;
	uint16_t			max_queue_depth;
	uint16_t			max_qpairs_per_ctrlr;
	uint32_t			in_capsule_data_size;
	uint32_t			max_io_size;
	uint32_t			io_unit_size;
	uint32_t			max_aq_depth;
	uint32_t			num_shared_buffers;
	union {
		uint32_t		buf_cache_size;
		uint32_t		iobuf_small_cache_size;
	};
	uint32_t			iobuf_large_cache_size;
	bool				dif_insert_or_strip;
	uint32_t			abort_timeout_sec;
	bool				zcopy;
	uint32_t			acceptor_poll_rate;
	uint32_t			ack_timeout;
	uint32_t			data_wr_pool_size;
	bool				disable_command_passthru;
	uint16_t			kas;
	uint32_t			min_kato;
	uint16_t			masked_oncs;
	uint16_t			masked_fuses;
	struct spdk_jsonrpc_request	*request;
	struct spdk_nvmf_transport	*transport;
	int				status;
};

/**
 * `max_qpairs_per_ctrlr` represents both admin and IO qpairs, that confuses
 * users when they configure a transport using RPC. So it was decided to
 * deprecate `max_qpairs_per_ctrlr` RPC parameter and use `max_io_qpairs_per_ctrlr`
 * But internal logic remains unchanged and SPDK expects that
 * spdk_nvmf_transport_opts::max_qpairs_per_ctrlr includes an admin qpair.
 * This function parses the number of IO qpairs and adds +1 for admin qpair.
 */
static int
nvmf_rpc_decode_max_io_qpairs(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;
	int rc;

	rc = spdk_json_number_to_uint16(val, i);
	if (rc == 0) {
		(*i)++;
	}

	return rc;
}

SPDK_LOG_DEPRECATION_REGISTER(nvmf_create_transport_num_shared_buffers,
			      "Use iobuf_large_cache_size and iobuf_small_cache_size instead", "v26.09",
			      SPDK_LOG_DEPRECATION_ALWAYS);

SPDK_LOG_DEPRECATION_REGISTER(nvmf_create_transport_buf_cache_size,
			      "buf_cache_size is deprecated", "v26.09", SPDK_LOG_DEPRECATION_ALWAYS);

SPDK_LOG_DEPRECATION_REGISTER(nvmf_create_transport_io_unit_size,
			      "io_unit_size is deprecated", "v26.09", SPDK_LOG_DEPRECATION_ALWAYS);

static const struct spdk_json_object_decoder rpc_nvmf_create_transport_decoders[] = {
	{"trtype", offsetof(struct rpc_nvmf_create_transport_ext, trtype), spdk_json_decode_string},
	{"max_queue_depth", offsetof(struct rpc_nvmf_create_transport_ext, max_queue_depth), spdk_json_decode_uint16, true},
	{"max_io_qpairs_per_ctrlr", offsetof(struct rpc_nvmf_create_transport_ext, max_qpairs_per_ctrlr), nvmf_rpc_decode_max_io_qpairs, true},
	{"in_capsule_data_size", offsetof(struct rpc_nvmf_create_transport_ext, in_capsule_data_size), spdk_json_decode_uint32, true},
	{"max_io_size", offsetof(struct rpc_nvmf_create_transport_ext, max_io_size), spdk_json_decode_uint32, true},
	{"io_unit_size", offsetof(struct rpc_nvmf_create_transport_ext, io_unit_size), rpc_decode_io_unit_size, true},
	{"max_aq_depth", offsetof(struct rpc_nvmf_create_transport_ext, max_aq_depth), spdk_json_decode_uint32, true},
	{"num_shared_buffers", offsetof(struct rpc_nvmf_create_transport_ext, num_shared_buffers), rpc_decode_num_shared_buffers, true},
	{"buf_cache_size", offsetof(struct rpc_nvmf_create_transport_ext, buf_cache_size), rpc_decode_buf_cache_size, true},
	{"iobuf_small_cache_size", offsetof(struct rpc_nvmf_create_transport_ext, iobuf_small_cache_size), spdk_json_decode_uint32, true},
	{"iobuf_large_cache_size", offsetof(struct rpc_nvmf_create_transport_ext, iobuf_large_cache_size), spdk_json_decode_uint32, true},
	{"dif_insert_or_strip", offsetof(struct rpc_nvmf_create_transport_ext, dif_insert_or_strip), spdk_json_decode_bool, true},
	{"abort_timeout_sec", offsetof(struct rpc_nvmf_create_transport_ext, abort_timeout_sec), spdk_json_decode_uint32, true},
	{"zcopy", offsetof(struct rpc_nvmf_create_transport_ext, zcopy), spdk_json_decode_bool, true},
	{"tgt_name", offsetof(struct rpc_nvmf_create_transport_ext, tgt_name), spdk_json_decode_string, true},
	{"acceptor_poll_rate", offsetof(struct rpc_nvmf_create_transport_ext, acceptor_poll_rate), spdk_json_decode_uint32, true},
	{"ack_timeout", offsetof(struct rpc_nvmf_create_transport_ext, ack_timeout), spdk_json_decode_uint32, true},
	{"data_wr_pool_size", offsetof(struct rpc_nvmf_create_transport_ext, data_wr_pool_size), spdk_json_decode_uint32, true},
	{"disable_command_passthru", offsetof(struct rpc_nvmf_create_transport_ext, disable_command_passthru), spdk_json_decode_bool, true},
	{"kas", offsetof(struct rpc_nvmf_create_transport_ext, kas), spdk_json_decode_uint16, true},
	{"min_kato", offsetof(struct rpc_nvmf_create_transport_ext, min_kato), spdk_json_decode_uint32, true},
	{"masked_oncs", offsetof(struct rpc_nvmf_create_transport_ext, masked_oncs), rpc_decode_oncs_features, true},
	{"masked_fuses", offsetof(struct rpc_nvmf_create_transport_ext, masked_fuses), rpc_decode_fuses_features, true},
};

/* TODO: replace with free_rpc_nvmf_create_transport */
static void
free_rpc_nvmf_create_transport_ext(struct rpc_nvmf_create_transport_ext *ereq)
{
	free(ereq->trtype);
	free(ereq->tgt_name);
	free(ereq);
}

static void
nvmf_rpc_transport_destroy_done_cb(void *cb_arg)
{
	struct rpc_nvmf_create_transport_ext *ereq = cb_arg;

	spdk_jsonrpc_send_error_response_fmt(ereq->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					     "Failed to add transport to tgt.(%d)", ereq->status);
	free_rpc_nvmf_create_transport_ext(ereq);
}

static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct rpc_nvmf_create_transport_ext *ereq = cb_arg;

	if (status) {
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		ereq->status = status;
		spdk_nvmf_transport_destroy(ereq->transport, nvmf_rpc_transport_destroy_done_cb, ereq);
		return;
	}

	spdk_jsonrpc_send_bool_response(ereq->request, true);
	free_rpc_nvmf_create_transport_ext(ereq);
}

static void
nvmf_rpc_create_transport_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct rpc_nvmf_create_transport_ext *ereq = cb_arg;

	if (!transport) {
		SPDK_ERRLOG("Failed to create transport.\n");
		spdk_jsonrpc_send_error_response(ereq->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create transport.");
		free_rpc_nvmf_create_transport_ext(ereq);
		return;
	}

	ereq->transport = transport;

	spdk_nvmf_tgt_add_transport(spdk_nvmf_get_tgt(ereq->tgt_name), transport,
				    nvmf_rpc_tgt_add_transport_done, ereq);
}

static void
rpc_nvmf_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_nvmf_create_transport_ext *req;
	struct spdk_nvmf_transport_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    req)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_create_transport_ext(req);
		return;
	}

	tgt = spdk_nvmf_get_tgt(req->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_create_transport_ext(req);
		return;
	}

	/* Initialize all the transport options (based on transport type) and decode the
	 * parameters again to update any options passed in rpc create transport call.
	 */
	if (!spdk_nvmf_transport_opts_init(req->trtype, &opts, sizeof(opts))) {
		/* This can happen if user specifies PCIE transport type which isn't valid for
		 * NVMe-oF.
		 */
		SPDK_ERRLOG("Invalid transport type '%s'\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'", req->trtype);
		free_rpc_nvmf_create_transport_ext(req);
		return;
	}

	req->max_queue_depth = opts.max_queue_depth;
	req->max_qpairs_per_ctrlr = opts.max_qpairs_per_ctrlr;
	req->in_capsule_data_size = opts.in_capsule_data_size;
	req->max_io_size = opts.max_io_size;
	req->io_unit_size = opts.io_unit_size;
	req->max_aq_depth = opts.max_aq_depth;
	req->num_shared_buffers = opts.num_shared_buffers;
	req->iobuf_small_cache_size = opts.iobuf_small_cache_size;
	req->iobuf_large_cache_size = opts.iobuf_large_cache_size;
	req->dif_insert_or_strip = opts.dif_insert_or_strip;
	req->abort_timeout_sec = opts.abort_timeout_sec;
	req->zcopy = opts.zcopy;
	req->acceptor_poll_rate = opts.acceptor_poll_rate;
	req->ack_timeout = opts.ack_timeout;
	req->data_wr_pool_size = opts.data_wr_pool_size;
	req->disable_command_passthru = opts.disable_command_passthru;
	req->kas = opts.kas;
	req->min_kato = opts.min_kato;
	req->masked_oncs = opts.oncs.raw;
	req->masked_fuses = opts.fuses.raw;

	if (spdk_json_decode_object_relaxed(params, rpc_nvmf_create_transport_decoders,
					    SPDK_COUNTOF(rpc_nvmf_create_transport_decoders),
					    req)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_nvmf_create_transport_ext(req);
		return;
	}

	opts.max_queue_depth = req->max_queue_depth;
	opts.max_qpairs_per_ctrlr = req->max_qpairs_per_ctrlr;
	opts.in_capsule_data_size = req->in_capsule_data_size;
	opts.max_io_size = req->max_io_size;
	opts.io_unit_size = req->io_unit_size;
	opts.max_aq_depth = req->max_aq_depth;
	opts.num_shared_buffers = req->num_shared_buffers;
	opts.iobuf_small_cache_size = req->iobuf_small_cache_size;
	opts.iobuf_large_cache_size = req->iobuf_large_cache_size;
	opts.dif_insert_or_strip = req->dif_insert_or_strip;
	opts.abort_timeout_sec = req->abort_timeout_sec;
	opts.zcopy = req->zcopy;
	opts.acceptor_poll_rate = req->acceptor_poll_rate;
	opts.ack_timeout = req->ack_timeout;
	opts.data_wr_pool_size = req->data_wr_pool_size;
	opts.disable_command_passthru = req->disable_command_passthru;
	opts.kas = req->kas;
	opts.min_kato = req->min_kato;
	opts.oncs.raw = req->masked_oncs;
	opts.fuses.raw = req->masked_fuses;

	if (spdk_nvmf_tgt_get_transport(tgt, req->trtype)) {
		SPDK_ERRLOG("Transport type '%s' already exists\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' already exists", req->trtype);
		free_rpc_nvmf_create_transport_ext(req);
		return;
	}

	/* Transport can parse additional params themselves */
	opts.transport_specific = params;
	req->request = request;

	rc = spdk_nvmf_transport_create_async(req->trtype, &opts, nvmf_rpc_create_transport_done, req);
	if (rc) {
		SPDK_ERRLOG("Transport type '%s' create failed\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed", req->trtype);
		free_rpc_nvmf_create_transport_ext(req);
	}
}
SPDK_RPC_REGISTER("nvmf_create_transport", rpc_nvmf_create_transport, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_nvmf_get_transports_decoders[] = {
	{"trtype", offsetof(struct rpc_nvmf_get_transports_ctx, trtype), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_nvmf_get_transports_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_get_transports(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_nvmf_get_transports_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_transport *transport = NULL;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_transports_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_transports_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_get_transports(&req);
		return;
	}

	if (req.trtype) {
		transport = spdk_nvmf_tgt_get_transport(tgt, req.trtype);
		if (transport == NULL) {
			SPDK_ERRLOG("transport '%s' does not exist\n", req.trtype);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free_rpc_nvmf_get_transports(&req);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (transport) {
		nvmf_transport_dump_opts(transport, w, false);
	} else {
		for (transport = spdk_nvmf_transport_get_first(tgt); transport != NULL;
		     transport = spdk_nvmf_transport_get_next(transport)) {
			nvmf_transport_dump_opts(transport, w, false);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_nvmf_get_transports(&req);
}
SPDK_RPC_REGISTER("nvmf_get_transports", rpc_nvmf_get_transports, SPDK_RPC_RUNTIME)

struct rpc_nvmf_get_stats_ctx_ext {
	struct rpc_nvmf_get_stats_ctx req;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_nvmf_get_stats_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_get_stats_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_nvmf_get_stats_ext(struct rpc_nvmf_get_stats_ctx_ext *ereq)
{
	free_rpc_nvmf_get_stats(&ereq->req);
	free(ereq);
}

static void
rpc_nvmf_get_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_nvmf_get_stats_ctx_ext *ereq = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ereq->w);
	spdk_json_write_object_end(ereq->w);
	spdk_jsonrpc_end_result(ereq->req.request, ereq->w);
	free_rpc_nvmf_get_stats_ext(ereq);
}

static void
_rpc_nvmf_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_nvmf_get_stats_ctx_ext *ereq = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ch = spdk_get_io_channel(ereq->tgt);
	group = spdk_io_channel_get_ctx(ch);

	spdk_nvmf_poll_group_dump_stat(group, ereq->w);

	spdk_put_io_channel(ch);
	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_nvmf_get_stats(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_nvmf_get_stats_ctx_ext *ereq;
	struct rpc_nvmf_get_stats_ctx *req;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	req = &ereq->req;
	req->request = request;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_get_stats_decoders,
					    SPDK_COUNTOF(rpc_nvmf_get_stats_decoders),
					    req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			free_rpc_nvmf_get_stats_ext(ereq);
			return;
		}
	}

	ereq->tgt = spdk_nvmf_get_tgt(req->tgt_name);
	if (!ereq->tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_get_stats_ext(ereq);
		return;
	}

	ereq->w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(ereq->w);
	spdk_json_write_named_uint64(ereq->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(ereq->w, "poll_groups");

	spdk_for_each_channel(ereq->tgt,
			      _rpc_nvmf_get_stats,
			      ereq,
			      rpc_nvmf_get_stats_done);
}

SPDK_RPC_REGISTER("nvmf_get_stats", rpc_nvmf_get_stats, SPDK_RPC_RUNTIME)

static const char *
nvmf_cntrltype_str(enum spdk_nvme_ctrlr_type type)
{
	switch (type) {
	case SPDK_NVME_CTRLR_IO:
		return "io";
	case SPDK_NVME_CTRLR_DISCOVERY:
		return "discovery";
	case SPDK_NVME_CTRLR_ADMINISTRATIVE:
		return "administrative";
	default:
		return "unknown";
	}
}

static void
dump_nvmf_ctrlr(struct spdk_json_write_ctx *w, struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t count;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", ctrlr->cntlid);
	spdk_json_write_named_string(w, "cntrltype", nvmf_cntrltype_str(ctrlr->cdata.cntrltype));
	spdk_json_write_named_string(w, "hostnqn", ctrlr->hostnqn);
	spdk_json_write_named_uuid(w, "hostid", &ctrlr->hostid);

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	spdk_json_write_named_uint32(w, "num_io_qpairs", count);

	spdk_json_write_object_end(w);
}

static const char *
nvmf_qpair_state_str(enum spdk_nvmf_qpair_state state)
{
	switch (state) {
	case SPDK_NVMF_QPAIR_UNINITIALIZED:
		return "uninitialized";
	case SPDK_NVMF_QPAIR_CONNECTING:
		return "connecting";
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		return "authenticating";
	case SPDK_NVMF_QPAIR_ENABLED:
		return "enabled";
	case SPDK_NVMF_QPAIR_DEACTIVATING:
		return "deactivating";
	case SPDK_NVMF_QPAIR_ERROR:
		return "error";
	default:
		return NULL;
	}
}

static void
dump_nvmf_qpair(struct spdk_json_write_ctx *w, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid = {};

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", qpair->ctrlr->cntlid);
	spdk_json_write_named_uint32(w, "qid", qpair->qid);
	spdk_json_write_named_string(w, "state", nvmf_qpair_state_str(qpair->state));
	spdk_json_write_named_string(w, "thread", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_string(w, "hostnqn", qpair->ctrlr->hostnqn);

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "listen_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
		if (qpair->transport->ops->listen_dump_opts) {
			qpair->transport->ops->listen_dump_opts(qpair->transport, &trid, w);
		}
	}

	memset(&trid, 0, sizeof(trid));
	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "peer_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
	}

	nvmf_qpair_auth_dump(qpair, w);
	spdk_json_write_object_end(w);
}

static const char *
nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss";
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";
	default:
		return NULL;
	}
}

static void
dump_nvmf_subsystem_listener(struct spdk_json_write_ctx *w,
			     struct spdk_nvmf_subsystem_listener *listener)
{
	uint32_t i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(listener->trid, w);
	spdk_json_write_object_end(w);

	if (listener->subsystem->opts.ana_reporting) {
		spdk_json_write_named_array_begin(w, "ana_states");
		for (i = 0; i < listener->subsystem->max_nsid; i++) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "ana_group", i + 1);
			spdk_json_write_named_string(w, "ana_state",
						     nvme_ana_state_str(listener->ana_state[i]));
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}

	spdk_json_write_object_end(w);
}

/* TODO: replace with rpc_nvmf_subsystem_get_controllers_ctx */
struct rpc_subsystem_query_ctx {
	char *nqn;
	char *tgt_name;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_subsystem_query_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_query_ctx, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_subsystem_query_ctx, tgt_name), spdk_json_decode_string, true},
};

/* TODO: replace with free_rpc_nvmf_subsystem_get_controllers */
static void
free_rpc_subsystem_query_ctx(struct rpc_subsystem_query_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_controllers_paused(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_ctrlr *ctrlr;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctrlr, &ctx->subsystem->ctrlrs, link) {
		dump_nvmf_ctrlr(w, ctrlr);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs(struct spdk_io_channel_iter *i)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_qpair *qpair;

	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr && qpair->ctrlr->subsys == ctx->subsystem) {
			dump_nvmf_qpair(ctx->w, qpair);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_nvmf_get_qpairs_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(ctx->w);

	spdk_for_each_channel(ctx->subsystem->tgt,
			      rpc_nvmf_get_qpairs,
			      ctx,
			      rpc_nvmf_get_qpairs_done);
}

static void
rpc_nvmf_get_listeners_paused(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem_listener *listener;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		dump_nvmf_subsystem_listener(w, listener);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
_rpc_nvmf_subsystem_query(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params,
			  spdk_nvmf_subsystem_state_change_done cb_fn)
{
	struct rpc_subsystem_query_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_subsystem_query_decoders,
				    SPDK_COUNTOF(rpc_subsystem_query_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	if (_rpc_nvmf_subsystem_pause(request, ctx->tgt_name, ctx->nqn, 0,
				      cb_fn, ctx, &ctx->subsystem, NULL)) {
		free_rpc_subsystem_query_ctx(ctx);
	}
}

static void
rpc_nvmf_subsystem_get_controllers(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_controllers_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_controllers", rpc_nvmf_subsystem_get_controllers,
		  SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_qpairs(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_qpairs_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_qpairs", rpc_nvmf_subsystem_get_qpairs, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_listeners(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_listeners_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_listeners", rpc_nvmf_subsystem_get_listeners,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_publish_mdns_prr_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_publish_mdns_prr_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_publish_mdns_prr(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;
	struct rpc_nvmf_publish_mdns_prr_ctx req = {};
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_publish_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_publish_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_publish_mdns_prr(&req);
		return;
	}

	rc = nvmf_publish_mdns_prr(tgt);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_nvmf_publish_mdns_prr(&req);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_nvmf_publish_mdns_prr(&req);
}
SPDK_RPC_REGISTER("nvmf_publish_mdns_prr", rpc_nvmf_publish_mdns_prr, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_nvmf_stop_mdns_prr_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_stop_mdns_prr_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_stop_mdns_prr(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_nvmf_stop_mdns_prr_ctx req = {};
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_nvmf_stop_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_nvmf_stop_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_rpc_nvmf_stop_mdns_prr(&req);
		return;
	}

	nvmf_tgt_stop_mdns_prr(tgt);

	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_nvmf_stop_mdns_prr(&req);
}
SPDK_RPC_REGISTER("nvmf_stop_mdns_prr", rpc_nvmf_stop_mdns_prr, SPDK_RPC_RUNTIME);
