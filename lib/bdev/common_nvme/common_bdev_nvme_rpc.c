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

#include "common_bdev_nvme.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/** Includes for bdevs using common layer */
#include "../nvme/bdev_nvme_rpc.h"
#include "../nvme/bdev_nvme.h"
#include "../ftl/bdev_ftl_rpc.h"
#include "../ftl/bdev_ftl.h"

struct rpc_construct_nvme {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
	char *hostnqn;
	char *hostaddr;
	char *hostsvcid;
	char *punits;
	char *uuid;
	char *mode;
};

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
};

#define NVME_MAX_BDEVS_PER_RPC 128

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_nvme req = {};
	struct nvme_bdev_construct_opts opts = {};
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

	opts.name = req.name;
	opts.hostnqn = req.hostnqn;

#if defined(FTL)
	if (req.punits)	{
		if (bdev_ftl_parse_punits(&opts.range, req.punits)) {
			goto invalid;
		}
	}

	if (req.uuid) {
		opts.uuid = calloc(1, sizeof(struct spdk_uuid));
		if (spdk_uuid_parse(opts.uuid, req.uuid) < 0) {
			goto invalid;
		}
	}
#endif

	if (req.mode != NULL) {
		if (!strcasecmp(req.mode, "generic")) {
			spdk_rpc_construct_generic_nvme_bdev(&opts, request);
#if defined(FTL)
		} else if (!strcasecmp(req.mode, "ftl")) {
			if (req.punits == NULL) {
				goto invalid;
			}
			spdk_rpc_construct_ftl_bdev(&opts, request);
			free(opts.uuid);
#endif
		} else {
			goto invalid;
		}
	} else {
		spdk_rpc_construct_generic_nvme_bdev(&opts, request);
	}

	free_rpc_construct_nvme(&req);
	return;

invalid:
	free(opts.uuid);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev, SPDK_RPC_RUNTIME)
