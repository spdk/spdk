/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Logical Volume Interface
 */

#ifndef SPDK_LVOL_H
#define SPDK_LVOL_H

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bs_dev;
struct spdk_lvol_store;
struct spdk_lvol;

enum lvol_clear_method {
	LVOL_CLEAR_WITH_DEFAULT = BLOB_CLEAR_WITH_DEFAULT,
	LVOL_CLEAR_WITH_NONE = BLOB_CLEAR_WITH_NONE,
	LVOL_CLEAR_WITH_UNMAP = BLOB_CLEAR_WITH_UNMAP,
	LVOL_CLEAR_WITH_WRITE_ZEROES = BLOB_CLEAR_WITH_WRITE_ZEROES,
};

enum lvs_clear_method {
	LVS_CLEAR_WITH_UNMAP = BS_CLEAR_WITH_UNMAP,
	LVS_CLEAR_WITH_WRITE_ZEROES = BS_CLEAR_WITH_WRITE_ZEROES,
	LVS_CLEAR_WITH_NONE = BS_CLEAR_WITH_NONE,
};

/* Must include null terminator. */
#define SPDK_LVS_NAME_MAX	64
#define SPDK_LVOL_NAME_MAX	64

/**
 * Parameters for lvolstore initialization.
 */
struct spdk_lvs_opts {
	/** Size of cluster in bytes. Must be multiple of 4KiB page size. */
	uint32_t		cluster_sz;

	/** Clear method */
	enum lvs_clear_method	clear_method;

	/** Name of the lvolstore */
	char			name[SPDK_LVS_NAME_MAX];

	/** num_md_pages_per_cluster_ratio = 100 means 1 page per cluster */
	uint32_t		num_md_pages_per_cluster_ratio;

	/**
	 * The size of spdk_lvol_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default
	 * values. After that, new added fields should be put in the end of the struct.
	 */
	uint32_t		opts_size;

	/**
	 * A function to be called to load external snapshots. If this is NULL while the lvolstore
	 * is being loaded, the lvolstore will not support external snapshots.
	 */
	spdk_bs_esnap_dev_create esnap_bs_dev_create;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_lvs_opts) == 88, "Incorrect size");

/**
 * Initialize an spdk_lvs_opts structure to the defaults.
 *
 * \param opts Pointer to the spdk_lvs_opts structure to initialize.
 */
void spdk_lvs_opts_init(struct spdk_lvs_opts *opts);

/**
 * Callback definition for lvolstore operations, including handle to lvs.
 *
 * \param cb_arg Custom arguments
 * \param lvol_store Handle to lvol_store or NULL when lvserrno is set
 * \param lvserrno Error
 */
typedef void (*spdk_lvs_op_with_handle_complete)(void *cb_arg, struct spdk_lvol_store *lvol_store,
		int lvserrno);

/**
 * Callback definition for lvolstore operations without handle.
 *
 * \param cb_arg Custom arguments
 * \param lvserrno Error
 */
typedef void (*spdk_lvs_op_complete)(void *cb_arg, int lvserrno);


/**
 * Callback definition for lvol operations with handle to lvol.
 *
 * \param cb_arg Custom arguments
 * \param lvol Handle to lvol or NULL when lvserrno is set
 * \param lvolerrno Error
 */
typedef void (*spdk_lvol_op_with_handle_complete)(void *cb_arg, struct spdk_lvol *lvol,
		int lvolerrno);

/**
 * Callback definition for lvol operations without handle to lvol.
 *
 * \param cb_arg Custom arguments
 * \param lvolerrno Error
 */
typedef void (*spdk_lvol_op_complete)(void *cb_arg, int lvolerrno);

/**
 * Callback definition for spdk_lvol_iter_clones.
 *
 * \param lvol An iterated lvol.
 * \param cb_arg Opaque context passed to spdk_lvol_iter_clone().
 * \return 0 to continue iterating, any other value to stop iterating.
 */
typedef int (*spdk_lvol_iter_cb)(void *cb_arg, struct spdk_lvol *lvol);

/**
 * Initialize lvolstore on given bs_bdev.
 *
 * \param bs_dev This is created on the given bdev by using spdk_bdev_create_bs_dev()
 * beforehand.
 * \param o Options for lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Rename the given lvolstore.
 *
 * \param lvs Pointer to lvolstore.
 * \param new_name New name of lvs.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		     spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Unload lvolstore.
 *
 * All lvols have to be closed beforehand, when doing unload.
 *
 * \param lvol_store Handle to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_lvs_unload(struct spdk_lvol_store *lvol_store,
		    spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Destroy lvolstore.
 *
 * All lvols have to be closed beforehand, when doing destroy.
 *
 * \param lvol_store Handle to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_lvs_destroy(struct spdk_lvol_store *lvol_store,
		     spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Create lvol on given lvolstore with specified size.
 *
 * \param lvs Handle to lvolstore.
 * \param name Name of lvol.
 * \param sz size of lvol in bytes.
 * \param thin_provisioned Enables thin provisioning.
 * \param clear_method Changes default data clusters clear method
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		     bool thin_provisioned, enum lvol_clear_method clear_method,
		     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);
/**
 * Create snapshot of given lvol.
 *
 * \param lvol Handle to lvol.
 * \param snapshot_name Name of created snapshot.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Create clone of given snapshot.
 *
 * \param lvol Handle to lvol snapshot.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
			    spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Create clone of given non-lvol device.
 *
 * The bdev that is being cloned is commonly called an external snapshot or esnap. The clone is
 * commonly called an esnap clone.
 *
 * \param esnap_id The identifier that will be passed to the spdk_bs_esnap_dev_create callback.
 * \param id_len The length of esnap_id, in bytes.
 * \param size_bytes The size of the external snapshot device, in bytes. This must be an integer
 * multiple of the lvolstore's cluster size. See \c cluster_sz in \struct spdk_lvs_opts.
 * \param lvs Handle to lvolstore.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 * \return 0 if parameters pass verification checks and the esnap creation is started, in which case
 * the \c cb_fn will be used to report the completion status. If an error is encountered, a negative
 * errno will be returned and \c cb_fn will not be called.
 */
int spdk_lvol_create_esnap_clone(const void *esnap_id, uint32_t id_len, uint64_t size_bytes,
				 struct spdk_lvol_store *lvs, const char *clone_name,
				 spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

int spdk_lvol_copy_blob(struct spdk_lvol *lvol);

/**
 * Rename lvol with new_name.
 *
 * \param lvol Handle to lvol.
 * \param new_name new name for lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * \brief Returns if it is possible to delete an lvol (i.e. lvol is not a snapshot that have at least one clone).
 * \param lvol Handle to lvol
 */
bool spdk_lvol_deletable(struct spdk_lvol *lvol);

/**
 * Close lvol and remove information about lvol from its lvolstore.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Close lvol, but information is kept on lvolstore.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Iterate clones of an lvol.
 *
 * Iteration stops if cb_fn(cb_arg, clone_lvol) returns non-zero.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Function to call for each lvol that clones this lvol.
 * \param cb_arg Context to pass wtih cb_fn.
 * \return -ENOMEM if memory allocation failed, non-zero return from cb_fn(), or 0.
 */
int spdk_lvol_iter_immediate_clones(struct spdk_lvol *lvol, spdk_lvol_iter_cb cb_fn, void *cb_arg);

/**
 * Get the lvol that has a particular UUID.
 *
 * \param uuid The lvol's UUID.
 * \return A pointer to the requested lvol on success, else NULL.
 */
struct spdk_lvol *spdk_lvol_get_by_uuid(const struct spdk_uuid *uuid);

void
spdk_lvol_update_on_failover(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol);
void
lvol_update_on_failover(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol, bool send_msg);
void
spdk_lvs_update_on_failover(struct spdk_lvol_store *lvs);
bool
spdk_lvs_check_active_process(struct spdk_lvol_store *lvs);
/**
 * Get the lvol that has a particular UUID.
 *
 * \param uuid The lvs's UUID.
 * \param leader The lvs's flag to set as leader or non leader.
 * \return A pointer to the requested lvol on success, else NULL.
 */
void spdk_lvs_set_leader_by_uuid(const struct spdk_uuid *uuid, bool leader);

/**
 * Get the lvol that has a particular UUID.
 *
 * \param uuid The lvol's UUID.
 * \param leader The lvs's flag to set as leader or non leader.
 * \return A pointer to the requested lvol on success, else NULL.
 */
void spdk_lvol_set_leader_by_uuid(const struct spdk_uuid *uuid, bool leader);

/**
 * set the leadership for all lvs and lvol.
 *
 * \param leader The lvs's flag to set as leader or non leader.
 */
void spdk_set_leader_all(struct spdk_lvol_store *t_lvs, bool leader);

/**
 * Get the lvol that has the specified name in the specified lvolstore.
 *
 * \param lvs_name Name of the lvolstore.
 * \param lvol_name Name ofthe lvol.
 * \return A pointer to the requested lvol on success, else NULL.
 */
struct spdk_lvol *spdk_lvol_get_by_names(const char *lvs_name, const char *lvol_name);

/**
 * Get I/O channel of bdev associated with specified lvol.
 *
 * \param lvol Handle to lvol.
 *
 * \return a pointer to the I/O channel.
 */
struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol);

/**
 * Load lvolstore from the given blobstore device.
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
		   void *cb_arg);

/**
 * Load lvolstore from the given blobstore device with options.
 *
 * If lvs_opts is not NULL, it should be initalized with spdk_lvs_opts_init().
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param lvs_opts lvolstore options.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 * blobstore.
 */
void spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *lvs_opts,
		       spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Grow a lvstore to fill the underlying device.
 * Cannot be used on loaded lvstore.
 *
 * \param bs_dev Pointer to the blobstore device.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
		   void *cb_arg);

/**
 * Grow a loaded lvstore to fill the underlying device.
 *
 * \param lvs Pointer to lvolstore.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvs_grow_live(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

void spdk_lvs_update_live(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg);

/**
 * Open a lvol.
 *
 * \param lvol Handle to lvol.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Inflate lvol
 *
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void spdk_lvol_inflate(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Decouple parent of lvol
 *
 * \param lvol Handle to lvol
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void spdk_lvol_decouple_parent(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Determine if an lvol is degraded. A degraded lvol cannot perform IO.
 *
 * \param lvol Handle to lvol
 * \return true if the lvol has no open blob or the lvol's blob is degraded, else false.
 */
bool spdk_lvol_is_degraded(const struct spdk_lvol *lvol);

/**
 * Make a shallow copy of lvol on given bs_dev.
 *
 * Lvol must be read only and lvol size must be less or equal than bs_dev size.
 *
 * \param lvol Handle to lvol
 * \param ext_dev The bs_dev to copy on. This is created on the given bdev by using
 * spdk_bdev_create_bs_dev_ext() beforehand
 * \param status_cb_fn Called repeatedly during operation with status updates
 * \param status_cb_arg Argument passed to function status_cb_fn.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 *
 * \return 0 if operation starts correctly, negative errno on failure.
 */
int spdk_lvol_shallow_copy(struct spdk_lvol *lvol, struct spdk_bs_dev *ext_dev,
			   spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			   spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Set a snapshot as the parent of a lvol
 *
 * This call set a snapshot as the parent of a lvol, making the lvol a clone of this snapshot.
 * The previous parent of the lvol, if any, can be another snapshot or an external snapshot; if
 * the lvol is not a clone, it must be thin-provisioned.
 * Lvol and parent snapshot must have the same size and must belong to the same lvol store.
 *
 * \param lvol Handle to lvol
 * \param snapshot Handle to the parent snapshot
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void spdk_lvol_set_parent(struct spdk_lvol *lvol, struct spdk_lvol *snapshot,
			  spdk_lvol_op_complete cb_fn, void *cb_arg);

/**
 * Set an external snapshot as the parent of a lvol
 *
 * This call set an external snapshot as the parent of a lvol, making the lvol a clone of this
 * external snapshot.
 * The previous parent of the lvol, if any, can be another external snapshot or a snapshot; if
 * the lvol is not a clone, it must be thin-provisioned.
 * The size of the external snapshot device must be an integer multiple of cluster size of
 * lvol's lvolstore.
 *
 * \param lvol Handle to lvol
 * \param esnap_id The identifier of the external snapshot.
 * \param esnap_id_len The length of esnap_id, in bytes.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void spdk_lvol_set_external_parent(struct spdk_lvol *lvol, const void *esnap_id,
				   uint32_t esnap_id_len,
				   spdk_lvol_op_complete cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif  /* SPDK_LVOL_H */
