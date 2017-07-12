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

#ifndef SPDK_LVOLSTORE_H
#define SPDK_LVOLSTORE_H

#include "spdk/lvol.h"

/* Length of string returned from uuid_unparse() */
#define UUID_STRING_LEN 37

struct spdk_lvol_store_req {
	union {
		struct {
			spdk_lvs_op_complete    cb_fn;
			void                    *cb_arg;
			struct spdk_bdev	*base_bdev;
		} lvs_basic;

		struct {
			spdk_lvs_op_with_handle_complete cb_fn;
			void                            *cb_arg;
			struct spdk_lvol_store          *lvol_store;
			struct spdk_bs_dev		*bs_dev;
			struct spdk_bdev		*base_bdev;
		} lvs_handle;

		struct {
			spdk_lvol_op_complete    cb_fn;
			void                    *cb_arg;
		} lvol_basic;

		struct {
			spdk_lvol_op_with_handle_complete cb_fn;
			void                            *cb_arg;
			struct spdk_lvol		*lvol;
		} lvol_handle;
	} u;
};

struct spdk_lvol_store {
	struct spdk_bs_dev              *bs_dev;
	struct spdk_blob_store          *blobstore;
	uuid_t				uuid;
	struct spdk_io_channel		*channel;
	TAILQ_ENTRY(spdk_lvol)		lvols;
};

struct spdk_lvol {
	struct spdk_lvol_store		*lvol_store;
	struct spdk_blob		*blob;
	size_t				sz;
	char				*name;
	struct spdk_bdev		*bdev; // Shouldn't be here ?
};

#endif /* SPDK_LVOLSTORE_H */
