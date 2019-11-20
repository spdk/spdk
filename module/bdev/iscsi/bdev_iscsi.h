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

#ifndef SPDK_BDEV_ISCSI_H
#define SPDK_BDEV_ISCSI_H

#include "spdk/bdev.h"

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
 * \param bdev Pointer to iSCSI bdev.
 * \param cb_fn Completion callback
 * \param cb_arg Completion callback custom arguments
 */
void delete_iscsi_disk(struct spdk_bdev *bdev, spdk_delete_iscsi_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_ISCSI_H */
