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

#include "spdk/gpt_spec.h"

struct spdk_bdev;
struct spdk_lvol_store;

typedef void (*spdk_lvol_store_op_complete)(void *cb_arg, struct spdk_lvol_store *lvol_store,
		int bserrno);
typedef void (*spdk_lvol_op_complete)(void *cb_arg, int bserrno);

struct spdk_lvol_store_req {
	union {
		struct {
			spdk_lvol_op_complete	cb_fn;
			void			*cb_arg;
		} lvol_basic;

		struct {
			spdk_lvol_store_op_complete	cb_fn;
			void				*cb_arg;
			struct spdk_lvol_store		*lvol_store;
		} lvol_handle;
	} u;
};

struct spdk_lvol_store {
	struct spdk_bs_dev		*bs_dev;
	struct spdk_bdev		*base_bdev;
	struct spdk_blob_store		*blobstore;
	struct spdk_gpt_guid		guid;

	TAILQ_ENTRY(spdk_lvol_store)	lvol_stores;
};

struct spdk_lvol {
	struct spdk_bdev		*disk;
	struct spdk_lvol_store		*lvol_store;
};


int spdk_lvol_store_init(struct spdk_bs_dev *bs_dev, spdk_lvol_store_op_complete cb_fn,
			 void *cb_arg);
int spdk_lvol_store_unload(struct spdk_lvol_store *lvol_store, spdk_lvol_op_complete cb_fn,
			   void *cb_arg);

#endif  /* SPDK_LVOL_H */
