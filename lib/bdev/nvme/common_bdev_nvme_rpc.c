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
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

#include "bdev_nvme_rpc.h"
#include "common_bdev_nvme.h"

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
	free(req->mode);
}

static const struct spdk_json_object_decoder rpc_construct_nvme_decoders[] = {
	{"name", offsetof(struct rpc_construct_nvme, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_nvme, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_nvme, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_construct_nvme, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_construct_nvme, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_construct_nvme, subnqn), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_construct_nvme, hostnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_construct_nvme, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_construct_nvme, hostsvcid), spdk_json_decode_string, true},
	{"punits", offsetof(struct rpc_construct_nvme, punits), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_construct_nvme, uuid), spdk_json_decode_string, true},
	{"mode", offsetof(struct rpc_construct_nvme, mode), spdk_json_decode_string, true},

	{"prchk_reftag", offsetof(struct rpc_construct_nvme, prchk_reftag), spdk_json_decode_bool, true},
	{"prchk_guard", offsetof(struct rpc_construct_nvme, prchk_guard), spdk_json_decode_bool, true}
};

#define NVME_MAX_BDEVS_PER_RPC 128

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_nvme req = {};
	struct spdk_bdev_nvme_construct_opts opts = {};
	struct spdk_construct_fn_list *m;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_nvme_decoders,
				    SPDK_COUNTOF(rpc_construct_nvme_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&opts.trid.trtype, req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
		goto invalid;
	}

	/* Parse traddr */
	snprintf(opts.trid.traddr, sizeof(opts.trid.traddr), "%s", req.traddr);

	/* Parse adrfam */
	if (req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&opts.trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			goto invalid;
		}
	}

	/* Parse trsvcid */
	if (req.trsvcid) {
		snprintf(opts.trid.trsvcid, sizeof(opts.trid.trsvcid), "%s", req.trsvcid);
	}

	/* Parse subnqn */
	if (req.subnqn) {
		snprintf(opts.trid.subnqn, sizeof(opts.trid.subnqn), "%s", req.subnqn);
	}

	if (req.hostaddr) {
		snprintf(opts.hostid.hostaddr, sizeof(opts.hostid.hostaddr), "%s", req.hostaddr);
	}

	if (req.hostsvcid) {
		snprintf(opts.hostid.hostsvcid, sizeof(opts.hostid.hostsvcid), "%s", req.hostsvcid);
	}

	if (req.prchk_reftag) {
		opts.prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}

	if (req.prchk_guard) {
		opts.prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}

	opts.name = req.name;
	opts.hostnqn = req.hostnqn;

	if (req.mode != NULL) {
		SLIST_FOREACH(m, &g_construct_methods, slist) {
			if (!strcasecmp(req.mode, m->bdev_type)) {
				if (m->parse_fn && m->parse_fn(&req, &opts)) {
					goto invalid;
				}
				m->construct_fn(&opts, request);
				break;
			}
		}
		if (m == NULL) {
			goto invalid;
		}
	} else {
		spdk_rpc_construct_generic_nvme_bdev(&opts, request);
	}

	free_rpc_construct_nvme(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev, SPDK_RPC_RUNTIME)

static void
spdk_rpc_dump_nvme_controller_info(struct spdk_json_write_ctx *w,
				   struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_transport_id	*trid;

	trid = &nvme_ctrlr->trid;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", nvme_ctrlr->name);

	spdk_json_write_named_object_begin(w, "trid");
	spdk_bdev_nvme_dump_trid_json(trid, w);
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
	struct nvme_ctrlr *ctrlr = NULL;

	if (params && spdk_json_decode_object(params, rpc_get_nvme_controllers_decoders,
					      SPDK_COUNTOF(rpc_get_nvme_controllers_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name) {
		ctrlr = spdk_bdev_nvme_ctrlr_get_by_name(req.name);
		if (ctrlr == NULL) {
			SPDK_ERRLOG("ctrlr '%s' does not exist\n", req.name);
			goto invalid;
		}
	}

	free_rpc_get_nvme_controllers(&req);
	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	if (ctrlr != NULL) {
		spdk_rpc_dump_nvme_controller_info(w, ctrlr);
	} else {
		for (ctrlr = spdk_bdev_nvme_first_ctrlr(); ctrlr; ctrlr = spdk_bdev_nvme_next_ctrlr(ctrlr))  {
			spdk_rpc_dump_nvme_controller_info(w, ctrlr);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

	free_rpc_get_nvme_controllers(&req);
}
SPDK_RPC_REGISTER("get_nvme_controllers", spdk_rpc_get_nvme_controllers, SPDK_RPC_RUNTIME)
