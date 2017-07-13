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

/** \file
 * Logical Volume Interface
 */

#ifndef SPDK_LVOL_H
#define SPDK_LVOL_H

#include "spdk/queue.h"
#include "spdk/blob.h"

struct spdk_lvol_store;
struct spdk_lvol;

typedef void (*spdk_lvs_op_with_handle_complete)(void *cb_arg, struct spdk_lvol_store *lvol_store,
		int lvserrno);
typedef void (*spdk_lvs_op_complete)(void *cb_arg, int lvserrno);
typedef void (*spdk_lvol_op_complete)(void *cb_arg, int lvolerrno);
typedef void (*spdk_lvol_op_with_handle_complete)(void *cb_arg, struct spdk_lvol *lvol,
		int lvolerrno);

int spdk_lvs_init(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);
int spdk_lvs_unload(struct spdk_lvol_store *lvol_store, spdk_lvs_op_complete cb_fn, void *cb_arg);
void spdk_lvs_remove_own_lvols(struct spdk_lvol_store *lvol_store);
int spdk_lvol_create(struct spdk_lvol_store *lvs, size_t sz,
		     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);
void spdk_lvol_destroy(struct spdk_lvol *lvol);
struct lvol_store_bdev_pair *vbdev_get_lvs_pair_by_lvs(struct spdk_lvol_store *lvs_orig);
struct spdk_lvol_store *vbdev_get_lvol_store_by_guid(uuid_t uuid);

#endif  /* SPDK_LVOL_H */
