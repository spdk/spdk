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

#include "spdk/stdinc.h"
#include "spdk/blob.h"

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
	uint32_t		cluster_sz;
	enum lvs_clear_method	clear_method;
	char			name[SPDK_LVS_NAME_MAX];
};

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

#ifdef __cplusplus
}
#endif

#endif  /* SPDK_LVOL_H */
