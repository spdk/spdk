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

#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include <linux/nbd.h>

#include "nbd_internal.h"
#include "spdk_internal/log.h"

struct rpc_start_nbd_disk {
	char *bdev_name;
	char *nbd_device;
};

static void
free_rpc_start_nbd_disk(struct rpc_start_nbd_disk *req)
{
	free(req->bdev_name);
	free(req->nbd_device);
}

static const struct spdk_json_object_decoder rpc_start_nbd_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_start_nbd_disk, bdev_name), spdk_json_decode_string},
	{"nbd_device", offsetof(struct rpc_start_nbd_disk, nbd_device), spdk_json_decode_string},
};

static void
spdk_rpc_start_nbd_disk(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_start_nbd_disk req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nbd_disk *nbd;

	if (spdk_json_decode_object(params, rpc_start_nbd_disk_decoders,
				    SPDK_COUNTOF(rpc_start_nbd_disk_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nbd_device == NULL || req.bdev_name == NULL) {
		goto invalid;
	}

	/* make sure nbd_device is not registered */
	nbd = spdk_nbd_disk_find_by_nbd_path(req.nbd_device);
	if (nbd) {
		goto invalid;
	}

	nbd = spdk_nbd_start(req.bdev_name, req.nbd_device);
	if (!nbd) {
		goto invalid;
	}

	free_rpc_start_nbd_disk(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	free_rpc_start_nbd_disk(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}

SPDK_RPC_REGISTER("start_nbd_disk", spdk_rpc_start_nbd_disk)

struct rpc_stop_nbd_disk {
	char *nbd_device;
};

static void
free_rpc_stop_nbd_disk(struct rpc_stop_nbd_disk *req)
{
	free(req->nbd_device);
}

static const struct spdk_json_object_decoder rpc_stop_nbd_disk_decoders[] = {
	{"nbd_device", offsetof(struct rpc_stop_nbd_disk, nbd_device), spdk_json_decode_string},
};

struct nbd_disconnect_arg {
	struct spdk_jsonrpc_request *request;
	struct spdk_nbd_disk *nbd;
};

static void *
nbd_disconnect_thread(void *arg)
{
	struct nbd_disconnect_arg *thd_arg = arg;
	struct spdk_json_write_ctx *w;

	spdk_unaffinitize_thread();

	nbd_disconnect(thd_arg->nbd);

	w = spdk_jsonrpc_begin_result(thd_arg->request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(thd_arg->request, w);

out:
	free(thd_arg);
	pthread_exit(NULL);
}

static void
spdk_rpc_stop_nbd_disk(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_stop_nbd_disk req = {};
	struct spdk_nbd_disk *nbd;
	pthread_t tid;
	struct nbd_disconnect_arg *thd_arg = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_stop_nbd_disk_decoders,
				    SPDK_COUNTOF(rpc_stop_nbd_disk_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (req.nbd_device == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	/* make sure nbd_device is registered */
	nbd = spdk_nbd_disk_find_by_nbd_path(req.nbd_device);
	if (!nbd) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	/*
	 * thd_arg should be freed by created thread
	 * if thread is created successfully.
	 */
	thd_arg = malloc(sizeof(*thd_arg));
	if (!thd_arg) {
		SPDK_ERRLOG("could not allocate nbd disconnect thread arg\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		goto out;
	}

	thd_arg->request = request;
	thd_arg->nbd = nbd;

	/*
	 * NBD ioctl of disconnect will block until data are flushed.
	 * Create separate thread to execute it.
	 */
	rc = pthread_create(&tid, NULL, nbd_disconnect_thread, (void *)thd_arg);
	if (rc != 0) {
		SPDK_ERRLOG("could not create nbd disconnect thread: %s\n", spdk_strerror(rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(rc));
		free(thd_arg);
		goto out;
	}

	rc = pthread_detach(tid);
	if (rc != 0) {
		SPDK_ERRLOG("could not detach nbd disconnect thread: %s\n", spdk_strerror(rc));
		goto out;
	}

out:
	free_rpc_stop_nbd_disk(&req);
}

SPDK_RPC_REGISTER("stop_nbd_disk", spdk_rpc_stop_nbd_disk)

static void
spdk_rpc_dump_nbd_info(struct spdk_json_write_ctx *w,
		       struct spdk_nbd_disk *nbd)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "nbd_device");
	spdk_json_write_string(w, spdk_nbd_disk_get_nbd_path(nbd));

	spdk_json_write_name(w, "bdev_name");
	spdk_json_write_string(w, spdk_nbd_disk_get_bdev_name(nbd));

	spdk_json_write_object_end(w);
}

struct rpc_get_nbd_disks {
	char *nbd_device;
};

static void
free_rpc_get_nbd_disks(struct rpc_get_nbd_disks *r)
{
	free(r->nbd_device);
}

static const struct spdk_json_object_decoder rpc_get_nbd_disks_decoders[] = {
	{"nbd_device", offsetof(struct rpc_get_nbd_disks, nbd_device), spdk_json_decode_string},
};

static void
spdk_rpc_get_nbd_disks(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_get_nbd_disks req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nbd_disk *nbd = NULL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_nbd_disks_decoders,
					    SPDK_COUNTOF(rpc_get_nbd_disks_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			goto invalid;
		} else {
			if (req.nbd_device == NULL) {
				SPDK_ERRLOG("missing nbd_device param\n");
				goto invalid;
			}

			nbd = spdk_nbd_disk_find_by_nbd_path(req.nbd_device);
			if (nbd == NULL) {
				SPDK_ERRLOG("nbd device '%s' does not exist\n", req.nbd_device);
				goto invalid;
			}

			free_rpc_get_nbd_disks(&req);
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	if (nbd != NULL) {
		spdk_rpc_dump_nbd_info(w, nbd);
	} else {
		for (nbd = spdk_nbd_disk_first(); nbd != NULL; nbd = spdk_nbd_disk_next(nbd)) {
			spdk_rpc_dump_nbd_info(w, nbd);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

	free_rpc_get_nbd_disks(&req);
}
SPDK_RPC_REGISTER("get_nbd_disks", spdk_rpc_get_nbd_disks)
