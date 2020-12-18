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
#include "spdk/log.h"

struct rpc_nbd_start_disk {
	char *bdev_name;
	char *nbd_device;
	/* Used to search one available nbd device */
	int nbd_idx;
	bool nbd_idx_specified;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_nbd_start_disk(struct rpc_nbd_start_disk *req)
{
	free(req->bdev_name);
	free(req->nbd_device);
	free(req);
}

static const struct spdk_json_object_decoder rpc_nbd_start_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_nbd_start_disk, bdev_name), spdk_json_decode_string},
	{"nbd_device", offsetof(struct rpc_nbd_start_disk, nbd_device), spdk_json_decode_string, true},
};

/* Return 0 to indicate the nbd_device might be available,
 * or non-zero to indicate the nbd_device is invalid or in use.
 */
static int
check_available_nbd_disk(char *nbd_device)
{
	char nbd_block_path[256];
	char tail[2];
	int rc;
	unsigned int nbd_idx;
	struct spdk_nbd_disk *nbd;

	/* nbd device path must be in format of /dev/nbd<num>, with no tail. */
	rc = sscanf(nbd_device, "/dev/nbd%u%1s", &nbd_idx, tail);
	if (rc != 1) {
		return -errno;
	}

	/* make sure nbd_device is not registered inside SPDK */
	nbd = nbd_disk_find_by_nbd_path(nbd_device);
	if (nbd) {
		/* nbd_device is in use */
		return -EBUSY;
	}

	/* A valid pid file in /sys/block indicates the device is in use */
	snprintf(nbd_block_path, 256, "/sys/block/nbd%u/pid", nbd_idx);

	rc = open(nbd_block_path, O_RDONLY);
	if (rc < 0) {
		if (errno == ENOENT) {
			/* nbd_device might be available */
			return 0;
		} else {
			SPDK_ERRLOG("Failed to check PID file %s: %s\n", nbd_block_path, spdk_strerror(errno));
			return -errno;
		}
	}

	close(rc);

	/* nbd_device is in use */
	return -EBUSY;
}

static char *
find_available_nbd_disk(int nbd_idx, int *next_nbd_idx)
{
	int i, rc;
	char nbd_device[20];

	for (i = nbd_idx; ; i++) {
		snprintf(nbd_device, 20, "/dev/nbd%d", i);
		/* Check whether an nbd device exists in order to reach the last one nbd device */
		rc = access(nbd_device, F_OK);
		if (rc != 0) {
			break;
		}

		rc = check_available_nbd_disk(nbd_device);
		if (rc == 0) {
			if (next_nbd_idx != NULL) {
				*next_nbd_idx = i + 1;
			}

			return strdup(nbd_device);
		}
	}

	return NULL;
}

static void
rpc_start_nbd_done(void *cb_arg, struct spdk_nbd_disk *nbd, int rc)
{
	struct rpc_nbd_start_disk *req = cb_arg;
	struct spdk_jsonrpc_request *request = req->request;
	struct spdk_json_write_ctx *w;

	/* Check whether it's automatic nbd-device assignment */
	if (rc == -EBUSY && req->nbd_idx_specified == false) {
		free(req->nbd_device);

		req->nbd_device = find_available_nbd_disk(req->nbd_idx, &req->nbd_idx);
		if (req->nbd_device != NULL) {
			spdk_nbd_start(req->bdev_name, req->nbd_device,
				       rpc_start_nbd_done, req);
			return;
		}

		SPDK_INFOLOG(nbd, "There is no available nbd device.\n");
	}

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_nbd_start_disk(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_nbd_get_path(nbd));
	spdk_jsonrpc_end_result(request, w);

	free_rpc_nbd_start_disk(req);
}

static void
rpc_nbd_start_disk(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_nbd_start_disk *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate nbd_start_disk request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nbd_start_disk_decoders,
				    SPDK_COUNTOF(rpc_nbd_start_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	if (req->bdev_name == NULL) {
		goto invalid;
	}

	if (req->nbd_device != NULL) {
		req->nbd_idx_specified = true;
		rc = check_available_nbd_disk(req->nbd_device);
		if (rc == -EBUSY) {
			SPDK_DEBUGLOG(nbd, "NBD device %s is in use.\n", req->nbd_device);
			spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(-rc));
			goto invalid;
		}

		if (rc != 0) {
			SPDK_DEBUGLOG(nbd, "Illegal nbd_device %s.\n", req->nbd_device);
			spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
							     "illegal nbd device %s", req->nbd_device);
			goto invalid;
		}
	} else {
		req->nbd_idx = 0;
		req->nbd_device = find_available_nbd_disk(req->nbd_idx, &req->nbd_idx);
		if (req->nbd_device == NULL) {
			SPDK_INFOLOG(nbd, "There is no available nbd device.\n");
			spdk_jsonrpc_send_error_response(request, -ENODEV,
							 "nbd device not found");
			goto invalid;
		}
	}

	req->request = request;
	spdk_nbd_start(req->bdev_name, req->nbd_device,
		       rpc_start_nbd_done, req);

	return;

invalid:
	free_rpc_nbd_start_disk(req);
}

SPDK_RPC_REGISTER("nbd_start_disk", rpc_nbd_start_disk, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nbd_start_disk, start_nbd_disk)

struct rpc_nbd_stop_disk {
	char *nbd_device;
};

static void
free_rpc_nbd_stop_disk(struct rpc_nbd_stop_disk *req)
{
	free(req->nbd_device);
}

static const struct spdk_json_object_decoder rpc_nbd_stop_disk_decoders[] = {
	{"nbd_device", offsetof(struct rpc_nbd_stop_disk, nbd_device), spdk_json_decode_string},
};

struct nbd_disconnect_arg {
	struct spdk_jsonrpc_request *request;
	struct spdk_nbd_disk *nbd;
};

static void *
nbd_disconnect_thread(void *arg)
{
	struct nbd_disconnect_arg *thd_arg = arg;

	spdk_unaffinitize_thread();

	nbd_disconnect(thd_arg->nbd);

	spdk_jsonrpc_send_bool_response(thd_arg->request, true);

	free(thd_arg);
	pthread_exit(NULL);
}

static void
rpc_nbd_stop_disk(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nbd_stop_disk req = {};
	struct spdk_nbd_disk *nbd;
	pthread_t tid;
	struct nbd_disconnect_arg *thd_arg = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_nbd_stop_disk_decoders,
				    SPDK_COUNTOF(rpc_nbd_stop_disk_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	if (req.nbd_device == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "invalid nbd device");
		goto out;
	}

	/* make sure nbd_device is registered */
	nbd = nbd_disk_find_by_nbd_path(req.nbd_device);
	if (!nbd) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
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
	free_rpc_nbd_stop_disk(&req);
}

SPDK_RPC_REGISTER("nbd_stop_disk", rpc_nbd_stop_disk, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nbd_stop_disk, stop_nbd_disk)

static void
rpc_dump_nbd_info(struct spdk_json_write_ctx *w,
		  struct spdk_nbd_disk *nbd)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "nbd_device", nbd_disk_get_nbd_path(nbd));

	spdk_json_write_named_string(w, "bdev_name", nbd_disk_get_bdev_name(nbd));

	spdk_json_write_object_end(w);
}

struct rpc_nbd_get_disks {
	char *nbd_device;
};

static void
free_rpc_nbd_get_disks(struct rpc_nbd_get_disks *r)
{
	free(r->nbd_device);
}

static const struct spdk_json_object_decoder rpc_nbd_get_disks_decoders[] = {
	{"nbd_device", offsetof(struct rpc_nbd_get_disks, nbd_device), spdk_json_decode_string, true},
};

static void
rpc_nbd_get_disks(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nbd_get_disks req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_nbd_disk *nbd = NULL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_nbd_get_disks_decoders,
					    SPDK_COUNTOF(rpc_nbd_get_disks_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto invalid;
		}

		if (req.nbd_device) {
			nbd = nbd_disk_find_by_nbd_path(req.nbd_device);
			if (nbd == NULL) {
				SPDK_ERRLOG("nbd device '%s' does not exist\n", req.nbd_device);
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				goto invalid;
			}

			free_rpc_nbd_get_disks(&req);
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (nbd != NULL) {
		rpc_dump_nbd_info(w, nbd);
	} else {
		for (nbd = nbd_disk_first(); nbd != NULL; nbd = nbd_disk_next(nbd)) {
			rpc_dump_nbd_info(w, nbd);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	return;

invalid:
	free_rpc_nbd_get_disks(&req);
}
SPDK_RPC_REGISTER("nbd_get_disks", rpc_nbd_get_disks, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nbd_get_disks, get_nbd_disks)
