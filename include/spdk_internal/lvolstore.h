/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_LVOLSTORE_H
#define SPDK_INTERNAL_LVOLSTORE_H

#include "spdk/blob.h"
#include "spdk/lvol.h"
#include "spdk/queue.h"
#include "spdk/uuid.h"

/* Default size of blobstore cluster */
#define SPDK_LVS_OPTS_CLUSTER_SZ (4 * 1024 * 1024)

/* UUID + '_' + blobid (20 characters for uint64_t).
 * Null terminator is already included in SPDK_UUID_STRING_LEN. */
#define SPDK_LVOL_UNIQUE_ID_MAX (SPDK_UUID_STRING_LEN + 1 + 20)

struct spdk_lvs_req {
	spdk_lvs_op_complete    cb_fn;
	void                    *cb_arg;
	struct spdk_lvol_store		*lvol_store;
	int				lvserrno;
};

struct spdk_lvs_grow_req {
	struct spdk_lvs_req	base;
	spdk_lvs_op_complete	cb_fn;
	void			*cb_arg;
	struct lvol_store_bdev	*lvs_bdev;
	int			lvol_cnt;
};

struct spdk_lvol_req {
	spdk_lvol_op_complete   cb_fn;
	void                    *cb_arg;
	struct spdk_lvol	*lvol;
	size_t			sz;
	struct spdk_io_channel	*channel;
	char			name[SPDK_LVOL_NAME_MAX];
};

struct spdk_lvs_with_handle_req {
	spdk_lvs_op_with_handle_complete cb_fn;
	void				*cb_arg;
	struct spdk_lvol_store		*lvol_store;
	struct spdk_bs_dev		*bs_dev;
	struct spdk_bdev		*base_bdev;
	int				lvserrno;
};

struct spdk_lvs_destroy_req {
	spdk_lvs_op_complete    cb_fn;
	void                    *cb_arg;
	struct spdk_lvol_store	*lvs;
};

struct spdk_lvol_with_handle_req {
	spdk_lvol_op_with_handle_complete cb_fn;
	void				*cb_arg;
	struct spdk_lvol		*lvol;
};

struct spdk_lvol_store {
	struct spdk_bs_dev		*bs_dev;
	struct spdk_blob_store		*blobstore;
	struct spdk_blob		*super_blob;
	spdk_blob_id			super_blob_id;
	struct spdk_uuid		uuid;
	int				lvol_count;
	int				lvols_opened;
	TAILQ_HEAD(, spdk_lvol)		lvols;
	TAILQ_HEAD(, spdk_lvol)		pending_lvols;
	bool				on_list;
	TAILQ_ENTRY(spdk_lvol_store)	link;
	char				name[SPDK_LVS_NAME_MAX];
	char				new_name[SPDK_LVS_NAME_MAX];
};

struct spdk_lvol {
	struct spdk_lvol_store		*lvol_store;
	struct spdk_blob		*blob;
	spdk_blob_id			blob_id;
	char				unique_id[SPDK_LVOL_UNIQUE_ID_MAX];
	char				name[SPDK_LVOL_NAME_MAX];
	struct spdk_uuid		uuid;
	char				uuid_str[SPDK_UUID_STRING_LEN];
	bool				thin_provision;
	struct spdk_bdev		*bdev;
	int				ref_count;
	bool				action_in_progress;
	enum blob_clear_method		clear_method;
	TAILQ_ENTRY(spdk_lvol) link;
};

struct lvol_store_bdev *vbdev_lvol_store_first(void);
struct lvol_store_bdev *vbdev_lvol_store_next(struct lvol_store_bdev *prev);

void spdk_lvol_resize(struct spdk_lvol *lvol, uint64_t sz, spdk_lvol_op_complete cb_fn,
		      void *cb_arg);

void spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn,
			     void *cb_arg);

#endif /* SPDK_INTERNAL_LVOLSTORE_H */
