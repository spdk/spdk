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

#ifndef SPDK_VBDEV_LVOL_H
#define SPDK_VBDEV_LVOL_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/lvol.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

typedef void (*vbdev_lvol_store_op_with_handle_complete)(void *cb_arg,
		struct spdk_lvol_store *lvol_store, int bserrno);
typedef void (*vbdev_lvol_store_op_complete)(void *cb_arg, int bserrno);
typedef void (*vbdev_lvol_op_complete)(void *cb_arg, int bserrno);

struct spdk_lvol_store_rpc_req {
	struct spdk_jsonrpc_server_conn *conn;
	const struct spdk_json_val      *id;
};

struct vbdev_lvol_store_req {
	vbdev_lvol_store_op_with_handle_complete	cb_fn;
	vbdev_lvol_store_op_complete	cb_fn2;
	void                            *cb_arg;
	struct spdk_lvol_store          *lvol_store;
	struct spdk_bs_dev              *bs_dev;
	struct spdk_bdev                *base_bdev;
};
struct vbdev_lvol_req {
	vbdev_lvol_op_complete		cb_fn;
	void                            *cb_arg;
};
struct spdk_lvol_store_guid;
int vbdev_construct_lvol_store(struct spdk_bdev *base_bdev,
			       vbdev_lvol_store_op_with_handle_complete cb_fn,
			       void *cb_arg);
void vbdev_destruct_lvol_store(struct spdk_lvol_store *lvol_store,
			       vbdev_lvol_store_op_complete cb_fn, void *cb_arg);
void spdk_rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store,
				      int bserrno);
void spdk_rpc_lvol_store_destroy_cb(void *cb_arg, int bserrno);
void vbdev_empty_destroy(void *cb_arg, int bserrno);
struct spdk_lvol_store *vbdev_lvol_store_first(void);
struct spdk_lvol_store *vbdev_lvol_store_next(struct spdk_lvol_store *);
struct spdk_lvol_store *vbdev_get_lvol_store_by_guid(struct spdk_lvol_store_guid *);

struct spdk_bdev *create_lvol_disk(struct spdk_lvol_store *ls, struct spdk_blob *blob, size_t num_blocks);
void vbdev_lvol_create_cb(void *cb_arg, int bserrno);
void vbdev_lvol_create(struct spdk_lvol_store_guid *guid, size_t sz, vbdev_lvol_op_complete cb_fn, void *cb_arg);
#endif /* SPDK_VBDEV_LVOL_H */
