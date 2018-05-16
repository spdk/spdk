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
#include "spdk/log.h"

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"
#include "spdk/env.h"

struct rpc_dd {
	uint64_t bs;
	uint64_t count;

	char *if_bdev_name;
	uint64_t ibs;

	struct  {

	} iflag;

	uint64_t obs;
	char *of_bdev_name;

	struct {

	} oflag;

	uint64_t seek;
	uint64_t skip;

	/* IO params */
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request;

	struct spdk_bdev *if_bdev;
	struct spdk_bdev_desc *if_desc;
	struct spdk_io_channel *if_ch;

	struct spdk_bdev *of_bdev;
	struct spdk_bdev_desc *of_desc;
	struct spdk_io_channel *of_ch;

	size_t if_offset; /* position in if_bdev */
	size_t if_count; /* Input block count */
	uint64_t if_bytes; /* Total bytes to read */
	uint64_t if_read; /* Total bytes already read */

	size_t of_offset; /* Position in of_bdev */

	char *ibs_buffer; /* Input buffer of size ibs */
	size_t ibs_size; /* Size of last read <= obs */
	size_t ibs_pos; /* Position of not consumed data from ibs_buffer <= ibs_size */

	char *obs_buffer; /* Output buffer of size obs */
	size_t obs_size; /* Amount of data written to obs_buffer */
};

static const struct rpc_dd rpc_dd_default = {
	.bs = 512,
	.count = 0,
	.if_bdev_name = NULL,
	.ibs = 0,
	.iflag = { },
	.obs = 0,
	.of_bdev_name = NULL,
	.seek = 0,
	.skip = 0
};

static const struct spdk_json_object_decoder rpc_dd_decoders[] = {
	{"bs", offsetof(struct rpc_dd, bs), spdk_json_decode_uint64, true},
	{"count", offsetof(struct rpc_dd, count), spdk_json_decode_uint64, true},
	{"if", offsetof(struct rpc_dd, if_bdev_name), spdk_json_decode_string},
	{"ibs", offsetof(struct rpc_dd, ibs), spdk_json_decode_uint64, true},
	/* No iflag yet */
	{"obs", offsetof(struct rpc_dd, obs), spdk_json_decode_uint64, true},
	{"of", offsetof(struct rpc_dd, of_bdev_name), spdk_json_decode_string},
	/* No oflag yet */
	{"seek", offsetof(struct rpc_dd, seek), spdk_json_decode_uint64, true},
	{"skip", offsetof(struct rpc_dd, skip), spdk_json_decode_uint64, true},
};

static void
free_rpc_dd_req(struct rpc_dd *req)
{
	if (req->if_desc) {
		spdk_bdev_close(req->if_desc);
	}

	if (req->of_desc) {
		spdk_bdev_close(req->of_desc);
	}

	if (req->if_ch) {
		spdk_put_io_channel(req->if_ch);
	}

	if (req->of_ch) {
		spdk_put_io_channel(req->of_ch);
	}

	spdk_dma_free(req->ibs_buffer);

	free(req->if_bdev_name);
	free(req->of_bdev_name);
	free(req);
}

/* TODO: better error reporting */
static void
dd_done(struct rpc_dd *req, bool success)
{
	if (req->w != NULL) {
		spdk_json_write_bool(req->w, success);
		spdk_jsonrpc_end_result(req->request, req->w);
	}

	free_rpc_dd_req(req);
}

static void dd_write(struct rpc_dd *req);
static void dd_read(struct rpc_dd *req);

static void
dd_append_ib(struct rpc_dd *req)
{
	size_t ibs_left = req->ibs_size - req->ibs_pos;
	size_t obs_need = req->obs - req->obs_size;
	size_t copy_size = spdk_min(ibs_left, obs_need);

	/* Copy to the output buffer what left in input buffer */
	memcpy(req->obs_buffer + req->obs_size, req->ibs_buffer + req->ibs_pos, copy_size);

	req->ibs_pos += copy_size;
	req->obs_size += copy_size;
	assert(req->ibs_pos <= req->ibs_size);
	assert(req->obs_size <= req->obs);

	if (req->obs_size == req->obs) {
		dd_write(req);
	} else {
		dd_read(req);
	}
}

static void
dd_write_cpl(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct rpc_dd *req = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		dd_done(req, false);
		return;
	}

	req->of_offset += req->obs;
	req->obs_size = 0;

	dd_append_ib(req);
}

static void
dd_write(struct rpc_dd *req)
{
	int rc;

	rc = spdk_bdev_write(req->of_desc, req->of_ch, req->obs_buffer, req->of_offset, req->obs_size,
			     dd_write_cpl, req);
	if (rc) {
		dd_done(req, false);
	}
}

static void
dd_read_cpl(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct rpc_dd *req = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		dd_done(req, false);
		return;
	}

	req->if_count++;
	req->if_read += req->ibs_size;
	req->if_offset += req->ibs_size;
	req->ibs_pos = 0;
	dd_append_ib(req);
}

static void
dd_read(struct rpc_dd *req)
{
	int rc;
	if (req->count > 0) {
		/* Are we done? */
		if (req->if_count == req->count) {
			dd_write(req);
			return;
		}

		/* Read full blocks here no mether what */
		req->ibs_size = req->ibs;
	} else {
		assert(req->if_read <= req->if_bytes);
		/* Are we done? */
		if (req->if_read == req->if_bytes) {
			dd_write(req);
			return;
		}

		/* Read up to EOF */
		req->ibs_size = spdk_min(req->ibs, req->if_bytes - req->if_read);
	}

	assert(req->ibs_pos == req->ibs);
	rc = spdk_bdev_read(req->if_desc, req->if_ch, req->ibs_buffer, req->if_offset, req->ibs_size,
			    dd_read_cpl, req);
	if (rc) {
		dd_done(req, false);
	}
}

static void
spdk_rpc_dd(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_dd *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("%s\n", spdk_strerror(ENOMEM));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(ENOMEM));
		return;
	}

	memcpy(req, &rpc_dd_default, sizeof(*req));
	if (spdk_json_decode_object(params, rpc_dd_decoders, SPDK_COUNTOF(rpc_dd_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	req->if_bdev = spdk_bdev_get_by_name(req->if_bdev_name);
	req->of_bdev = spdk_bdev_get_by_name(req->of_bdev_name);
	if (!req->if_bdev || !req->of_bdev) {
		rc = -ENODEV;
		goto invalid;
	}

	req->ibs = req->ibs ? req->ibs : req->bs;
	req->obs = req->obs ? req->obs : req->bs;

	if (req->bs == 0 || req->ibs || req->obs) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Zero is not valid for bs, ibs, obs.\n");
		goto free_req;
	}

	if (req->ibs % spdk_bdev_get_block_size(req->if_bdev) != 0 ||
	    req->obs % spdk_bdev_get_block_size(req->of_bdev) != 0) {
		SPDK_WARNLOG("bs, ibs or obs not multiple of bdev block size - this will fail\n");
	}

	req->if_offset = req->ibs * req->skip;
	req->of_offset = req->obs * req->seek;
	req->if_count = req->count;

	if (req->count > 0) {
		req->if_bytes = req->count * req->ibs;
	} else {
		/* Read whole disk */
		req->if_bytes = spdk_bdev_get_num_blocks(req->if_bdev) * spdk_bdev_get_block_size(
					req->if_bdev) - req->if_offset;
	}

	if (req->if_offset + req->if_bytes > spdk_bdev_get_num_blocks(req->if_bdev) *
	    spdk_bdev_get_block_size(req->if_bdev) ||
	    req->of_offset + req->if_bytes > spdk_bdev_get_num_blocks(req->of_bdev) * spdk_bdev_get_block_size(
		    req->of_bdev)) {
		SPDK_WARNLOG("Input or output IO size outside of bdev capacity- this will fail\n");
	}

	rc = spdk_bdev_open(req->if_bdev, false, NULL, NULL, &req->if_desc);
	rc = rc ? rc : spdk_bdev_open(req->of_bdev, true, NULL, NULL, &req->of_desc);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Can't open '%s'\n",
						     req->if_desc == NULL ? req->if_bdev_name : req->of_bdev_name);
		goto free_req;
	}

	req->if_ch = spdk_bdev_get_io_channel(req->if_desc);
	req->of_ch = spdk_bdev_get_io_channel(req->of_desc);

	if (req->if_ch == NULL || req->of_ch == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Can't get IO channel for '%s'\n",
						     req->if_ch == NULL ? req->if_bdev_name : req->of_bdev_name);
		goto free_req;
	}

	req->ibs_buffer = spdk_dma_malloc(req->ibs, 4096, NULL);
	req->ibs_pos = req->ibs_size = 0;; /* No data yet */

	req->obs_buffer = spdk_dma_malloc(req->obs, 4096, NULL);
	req->obs_size = 0; /* No data yet */

	if (req->ibs_buffer == NULL || req->obs_buffer == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(ENOMEM));
		goto free_req;
	}

	req->request = request;
	req->w = spdk_jsonrpc_begin_result(request);

	dd_read(req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
free_req:
	free_rpc_dd_req(req);
}
SPDK_RPC_REGISTER("dd", spdk_rpc_dd, SPDK_RPC_RUNTIME)

