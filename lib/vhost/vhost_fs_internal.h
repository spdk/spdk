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

#ifndef SPDK_VHOST_FS_INTERNAL_H
#define SPDK_VHOST_FS_INTERNAL_H

#include "spdk/stdinc.h"
#include <linux/virtio_blk.h>

// TODO: extract required structs from fuse_common.h
#define FUSE_USE_VERSION 30
#include "fuse.h"
#include "fuse_kernel.h"

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs.h"

#include "vhost_internal.h"

struct spdk_vhost_fs_task;

struct spdk_vhost_fs_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_filesystem *fs;

	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;

	/* Records, used by contruct callback */
	char *name;
	char *cpumask;
	bool readonly;

	spdk_vhost_fs_construct_cb cb_fn;
	void *cb_arg;
};

struct spdk_vhost_fs_session {
	/* The parent session must be the very first field in this struct */
	struct spdk_vhost_session vsession;
	struct spdk_vhost_fs_dev *fvdev;
	struct spdk_poller *requestq_poller;
	struct spdk_poller *stop_poller;

	struct spdk_io_channel *io_channel;

	struct fuse_conn_info cinfo;

	/* Tasks which are failed due to EBUSY, and ready to resubmit */
	TAILQ_HEAD(, spdk_vhost_fs_task) queued_task_list;
};

struct spdk_fuse_op {
	/* Return 0, successfully submitted; or -errno if failed; 1 if completed without cb */
	int (*func)(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);
	const char *op_name;
};

struct spdk_vhost_fs_task {
	struct spdk_vhost_fs_session *fvsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t req_idx;

	/* If set, the task is currently used for I/O processing. */
	bool used;

	/* In_XXX is used by fuse_out_header and its followings;
	 * Out_XXX is used by fuse in header and its followings.
	 * Generally, each out_iovs[0] only contains fuse_in_header, which is 40B;
	 * And each in_iovs[0] only contains fuse_out_header, which is 16B.
	 * For FORGET cmd, out_iovs[0] may be 64B, also contains fuse_forget_in or others,
	 * and FORGET has no in_iovs.
	 */
	uint16_t in_iovcnt;
	uint16_t out_iovcnt;
	struct iovec in_iovs[SPDK_VHOST_IOVS_MAX];
	struct iovec out_iovs[SPDK_VHOST_IOVS_MAX];

	/** Number of bytes that were written. */
	uint32_t used_len;

	/* CMD index -- fuse related */
	uint64_t unique;
	/* internal ctx for blobfs operation */
	struct {
		struct spdk_file *fp; // used by lookup
		uint64_t *file_offset_p; // used by read/write/release...
		char *filepath; // used by create
		/* In order to align with SPDK FUSE app, vhost-fs stores files with "/" as a prefix to their name */
		char *oldname; // used by unlink and rename
		char *newname; // used by rename
		struct fuse_write_in *arg; // used by write
	} u;

	/* Task which is failed due to EBUSY */
	TAILQ_ENTRY(spdk_vhost_fs_task)	tailq;
};

extern struct spdk_fuse_op *spdk_fuse_ll_ops;

void fs_request_finish(struct spdk_vhost_fs_task *task, int err);

#endif /* SPDK_VHOST_FS_INTERNAL_H */
