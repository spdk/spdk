/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_VBDEV_LVOL_H
#define SPDK_VBDEV_LVOL_H

#include "spdk/lvol.h"
#include "spdk/bdev_module.h"
#include "spdk/blob_bdev.h"
#include "spdk/priority_class.h"

#include "spdk_internal/lvolstore.h"

struct lvol_store_bdev {
	struct spdk_lvol_store	*lvs;
	struct spdk_bdev	*bdev;
	struct spdk_lvs_req	*req;
	bool			removal_in_progress;

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
			  int8_t lvol_priority_class,
		      spdk_lvol_op_with_handle_complete cb_fn,
		      void *cb_arg);

int vbdev_lvs_dump(struct spdk_lvol_store *lvs, const char *file,
		      spdk_lvol_op_with_handle_complete cb_fn,
		      void *cb_arg);			  

void vbdev_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
				spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

void vbdev_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);
void vbdev_lvol_create_bdev_clone(const char *esnap_uuid,
				  struct spdk_lvol_store *lvs, const char *clone_name,
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

int vbdev_lvol_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
				const void *esnap_id, uint32_t id_len,
				struct spdk_bs_dev **_bs_dev);

/**
 * \brief Make a shallow copy of lvol over a bdev
 *
 * \param lvol Handle to lvol
 * \param bdev_name Name of the bdev to copy on
 * \param status_cb_fn Called repeatedly during operation with status updates
 * \param status_cb_arg Argument passed to function status_cb_fn.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 *
 * \return 0 if operation starts correctly, negative errno on failure.
 */
int vbdev_lvol_shallow_copy(struct spdk_lvol *lvol, const char *bdev_name,
			    spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			    spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Set an external snapshot as the parent of a lvol.
 *
 * \param lvol Handle to lvol
 * \param esnap_name Name of the bdev that acts as external snapshot
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void vbdev_lvol_set_external_parent(struct spdk_lvol *lvol, const char *esnap_name,
				    spdk_lvol_op_complete cb_fn, void *cb_arg);

/* Sets the upper NBITS_PRIORITY_CLASS bits of all future logical block addresses of the underlying blob to 
the lvol's priority class bits. These bits must be cleared when the I/O reaches the lvolstore and added
again when it exits the lvolstore so that no internal lvolstore operation sees these bits.
*/
void vbdev_lvol_set_io_priority_class(struct spdk_lvol* lvol);

#endif /* SPDK_VBDEV_LVOL_H */
