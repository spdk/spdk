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

#include "spdk/stdinc.h"
#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/blobfs.h"
#include "spdk/assert.h"

#include "vhost_fs_internal.h"

struct spdk_fuse_op {
	/* Return 0, successfully submitted; or -errno if failed; 1 if completed without cb */
	int (*func)(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);
	const char *op_name;
};

static uint64_t
fs_task_get_fuse_unique(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;

	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	return fuse_in->unique;
}

static int
_send_reply(struct spdk_vhost_fs_task *task, int negtive_err)
{
	struct fuse_out_header *out = task->in_iovs[0].iov_base;

	assert(negtive_err > -1000 && negtive_err <= 0);

	task->used_len += sizeof(*out);

	out->unique = fs_task_get_fuse_unique(task);
	out->error = negtive_err;
	out->len = task->used_len;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse out header: len is 0x%x error is %d, unique is 0x%lx\n",
		      out->len, out->error, out->unique);

	fs_request_finish(task, -negtive_err);

	return 0;
}

static int
_fuse_reply_err(struct spdk_vhost_fs_task *task, int positive_err)
{
	return _send_reply(task, -positive_err);
}

static int do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);

struct spdk_fuse_op vhost_fs_fuse_ops[] = {
	[FUSE_INIT]	   = { do_nothing,	"INIT"},
	[FUSE_DESTROY]	   = { do_nothing,	"DESTROY"},
	[FUSE_STATFS]	   = { do_nothing,	"STATFS"},

	[FUSE_LOOKUP]	   = { do_nothing,	"LOOKUP"},
	[FUSE_FORGET]	   = { do_nothing,	"FORGET"},
	[FUSE_GETATTR]	   = { do_nothing,	"GETATTR"},
	[FUSE_SETATTR]	   = { do_nothing,	"SETATTR"},

	[FUSE_OPENDIR]	   = { do_nothing,	"OPENDIR"},
	[FUSE_READDIR]	   = { do_nothing,	"READDIR"},
	[FUSE_RELEASEDIR]  = { do_nothing,	"RELEASEDIR"},

	[FUSE_OPEN]	   = { do_nothing,	"OPEN"},
	[FUSE_READ]	   = { do_nothing,	"READ"},
	[FUSE_RELEASE]	   = { do_nothing,	"RELEASE"},
	[FUSE_FLUSH]	   = { do_nothing,	"FLUSH"},
	[FUSE_WRITE]	   = { do_nothing,	"WRITE"},
	[FUSE_CREATE]	   = { do_nothing,	"CREATE"},

	[FUSE_UNLINK]	   = { do_nothing,	"UNLINK"},
	[FUSE_RENAME]	   = { do_nothing,	"RENAME"},
	[FUSE_RENAME2]     = { do_nothing,	"RENAME2"},

	[FUSE_READLINK]	   = { do_nothing,	"READLINK"},
	[FUSE_SYMLINK]	   = { do_nothing,	"SYMLINK"},
	[FUSE_MKNOD]	   = { do_nothing,	"MKNOD"},
	[FUSE_MKDIR]	   = { do_nothing,	"MKDIR"},
	[FUSE_RMDIR]	   = { do_nothing,	"RMDIR"},
	[FUSE_LINK]	   = { do_nothing,	"LINK"},
	[FUSE_FSYNC]	   = { do_nothing,	"FSYNC"},
	[FUSE_SETXATTR]	   = { do_nothing,	"SETXATTR"},
	[FUSE_GETXATTR]	   = { do_nothing,	"GETXATTR"},
	[FUSE_LISTXATTR]   = { do_nothing,	"LISTXATTR"},
	[FUSE_REMOVEXATTR] = { do_nothing,	"REMOVEXATTR"},
	[FUSE_FSYNCDIR]	   = { do_nothing,	"FSYNCDIR"},
	[FUSE_GETLK]	   = { do_nothing,	"GETLK"},
	[FUSE_SETLK]	   = { do_nothing,	"SETLK"},
	[FUSE_SETLKW]	   = { do_nothing,	"SETLKW"},
	[FUSE_ACCESS]	   = { do_nothing,	"ACCESS"},
	[FUSE_INTERRUPT]   = { do_nothing,	"INTERRUPT"},
	[FUSE_BMAP]	   = { do_nothing,	"BMAP"},
	[FUSE_IOCTL]	   = { do_nothing,	"IOCTL"},
	[FUSE_POLL]	   = { do_nothing,	"POLL"},
	[FUSE_FALLOCATE]   = { do_nothing,	"FALLOCATE"},
	[FUSE_NOTIFY_REPLY] = { (void *) 1,	"NOTIFY_REPLY"},
	[FUSE_BATCH_FORGET] = { do_nothing,	"BATCH_FORGET"},
	[FUSE_READDIRPLUS] = { do_nothing,	"READDIRPLUS"},
	[FUSE_COPY_FILE_RANGE] = { do_nothing,	"COPY_FILE_RANGE"},
	[CUSE_INIT]	   = { do_nothing,	"CUSE_INIT"},
};

static int
do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;

	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	(void)fuse_in;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "FUSE request type '%"PRIu32"'(%s).\n", fuse_in->opcode,
		      vhost_fs_fuse_ops[fuse_in->opcode].op_name);

	_fuse_reply_err(task, ENOSYS);

	return -1;
}

int
spdk_vhost_fs_fuse_operate(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;
	void *fuse_arg_in;
	int rc;

	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	/* In general, argument for FUSE operation is the second readable iov.
	 * But for some brief cmds, like Forget, its argument is also in the end of
	 * first readable iov.
	 */
	fuse_arg_in = task->out_iovs[1].iov_base;
	if (task->out_iovs[0].iov_len > sizeof(struct fuse_in_header)) {
		fuse_arg_in = task->out_iovs[0].iov_base + sizeof(struct fuse_in_header);
	}

	rc = vhost_fs_fuse_ops[fuse_in->opcode].func(task, fuse_in->nodeid, fuse_arg_in);

	return rc;
}

int
spdk_vhost_fs_fuse_check(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;

	/*
	 * From FUSE protocol, at least there is one descriptor for host to read.
	 */
	if (spdk_unlikely(task->out_iovcnt == 0)) {
		return -EINVAL;
	}

	/* Check first writable iov if it has */
	if (task->in_iovcnt > 0) {
		iov = &task->in_iovs[0];
		if (spdk_unlikely(iov->iov_len != sizeof(struct fuse_out_header))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS,
				      "Last descriptor size is %zu but expected %ld (req_idx = %"PRIu16").\n",
				      iov->iov_len, sizeof(struct fuse_out_header), task->req_idx);

			return -EINVAL;
		}
	}

	/* Check first readable iov */
	iov = &task->out_iovs[0];
	if (spdk_unlikely(iov->iov_len < sizeof(struct fuse_in_header))) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS,
			      "First descriptor size is %zu but expected at least %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(struct fuse_in_header), task->req_idx);

		return -EINVAL;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vhost_fs_ops", SPDK_LOG_VHOST_FS_OPS)
