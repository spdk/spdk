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

#include "spdk/lvol.h"
#include "spdk_internal/bdev.h"

#include "spdk_internal/lvolstore.h"

struct lvol_store_bdev {
	struct spdk_lvol_store	*lvs;
	struct spdk_bdev 	*bdev;
	struct spdk_lvs_req	*req;

	TAILQ_ENTRY(lvol_store_bdev)	lvol_stores;
};

int vbdev_lvs_create(struct spdk_bdev *base_bdev, const char *name, uint32_t cluster_sz,
		     spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);
void vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);
void vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

int vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, size_t sz,
		      bool thin_provisioned, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

void vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
				spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

void vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

int vbdev_lvol_resize(char *name, size_t sz, spdk_lvol_op_complete cb_fn, void *cb_arg);

void vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		       spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Search for handle lvolstore
 * \param uuid_str UUID of lvolstore
 * \return Handle to spdk_lvol_store or NULL if not found.
 */
struct spdk_lvol_store *vbdev_get_lvol_store_by_uuid(const char *uuid_str);

/**
 * \brief Search for handle to lvolstore
 * \param name name of lvolstore
 * \return Handle to spdk_lvol_store or NULL if not found.
 */
struct spdk_lvol_store *vbdev_get_lvol_store_by_name(const char *name);

/**
 * \brief Search for handle to lvol_store_bdev
 * \param lvs handle to lvolstore
 * \return Handle to lvol_store_bdev or NULL if not found.
 */
struct lvol_store_bdev *vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs);

struct spdk_lvol *vbdev_lvol_get_from_bdev(struct spdk_bdev *bdev);

#endif /* SPDK_VBDEV_LVOL_H */
