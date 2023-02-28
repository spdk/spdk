/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_LVOL_H
#define SPDK_VBDEV_LVOL_H

#include "spdk/lvol.h"
#include "spdk/bdev_module.h"

#include "spdk_internal/lvolstore.h"

struct lvol_store_bdev {
	struct spdk_lvol_store	*lvs;
	struct spdk_bdev	*bdev;
	struct spdk_lvs_req	*req;

	TAILQ_ENTRY(lvol_store_bdev)	lvol_stores;
};

struct lvol_bdev {
	struct spdk_bdev	bdev;
	struct spdk_lvol	*lvol;
	struct lvol_store_bdev	*lvs_bdev;
};

int vbdev_lvs_create(const char *base_bdev_name, const char *name, uint32_t cluster_sz,
		     enum lvs_clear_method clear_method, uint32_t num_md_pages_per_cluster_ratio,
		     spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);
void vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);
void vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

int vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		      bool thin_provisioned, enum lvol_clear_method clear_method,
		      spdk_lvol_op_with_handle_complete cb_fn,
		      void *cb_arg);

void vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
				spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

void vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * \brief Change size of lvol
 * \param lvol Handle to lvol
 * \param sz Size of lvol to change
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 * \return error
 */
void vbdev_lvol_resize(struct spdk_lvol *lvol, uint64_t sz, spdk_lvol_op_complete cb_fn,
		       void *cb_arg);

/**
 * \brief Mark lvol as read only
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void vbdev_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

void vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		       spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Destroy a logical volume
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void vbdev_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Renames given lvolstore.
 *
 * \param lvs Pointer to lvolstore
 * \param new_name New name of lvs
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void vbdev_lvs_rename(struct spdk_lvol_store *lvs, const char *new_lvs_name,
		      spdk_lvs_op_complete cb_fn, void *cb_arg);

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

/**
 * \brief Grow given lvolstore.
 *
 * \param lvs Pointer to lvolstore
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void vbdev_lvs_grow(struct spdk_lvol_store *lvs,
		    spdk_lvs_op_complete cb_fn, void *cb_arg);

#endif /* SPDK_VBDEV_LVOL_H */
