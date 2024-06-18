/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_ISCSI_H
#define SPDK_BDEV_ISCSI_H

#include "spdk/bdev.h"

struct spdk_bdev_iscsi_opts {
	uint64_t timeout_sec;
	uint64_t timeout_poller_period_us;
};

typedef void (*spdk_delete_iscsi_complete)(void *cb_arg, int bdeverrno);

/**
 * SPDK bdev iSCSI callback type.
 *
 * \param cb_arg Completion callback custom arguments
 * \param bdev created bdev
 * \param status operation status. Zero on success.
 */
typedef void (*spdk_bdev_iscsi_create_cb)(void *cb_arg, struct spdk_bdev *bdev, int status);

/**
 * Create new iSCSI bdev.
 *
 * \warning iSCSI URL allow providing login and password. Be careful because
 * they will show up in configuration dump.
 *
 * \param name name for new bdev.
 * \param url iSCSI URL string.
 * \param initiator_iqn connection iqn name we identify to target as
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 * \return 0 on success or negative error code. If success bdev with provided name was created.
 */
int create_iscsi_disk(const char *bdev_name, const char *url, const char *initiator_iqn,
		      spdk_bdev_iscsi_create_cb cb_fn, void *cb_arg);

/**
 * Delete iSCSI bdev.
 *
 * \param bdev_name Name of iSCSI bdev.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void delete_iscsi_disk(const char *bdev_name, spdk_delete_iscsi_complete cb_fn, void *cb_arg);
void bdev_iscsi_get_opts(struct spdk_bdev_iscsi_opts *opts);
int bdev_iscsi_set_opts(struct spdk_bdev_iscsi_opts *opts);
#endif /* SPDK_BDEV_ISCSI_H */
