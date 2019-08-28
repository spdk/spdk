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

/* internal ctx for blobfs async operation */
struct spdk_fuse_op_args {
	struct spdk_file *fp; /* used by lookup */
	char *filepath; /* used by create */
	/* In order to align with SPDK FUSE app, vhost-fs stores files with "/" as a prefix to their name */
	char *ori_name; /* used by unlink and rename */
	char *new_name; /* used by rename */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_fuse_op_args) <= sizeof(struct vhost_fs_op_dummy_args),
		   "size of struct fuse_op_args exceeds");

static inline struct spdk_fuse_op_args *
fs_task_get_fuse_op_args(struct spdk_vhost_fs_task *task)
{
	return (struct spdk_fuse_op_args *)&task->dummy_args;
}

static inline uint64_t
fs_task_get_fuse_unique(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;

	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	return fuse_in->unique;
}

static int
_send_reply_none(struct spdk_vhost_fs_task *task, int error)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse out none: error is %d, unique is 0x%lx\n",
		      error, fs_task_get_fuse_unique(task));

	fs_request_finish(task, error);
	return 0;
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
_fuse_reply_ok(struct spdk_vhost_fs_task *task)
{
	return _send_reply(task, 0);
}

static int
_fuse_reply_err(struct spdk_vhost_fs_task *task, int positive_err)
{
	return _send_reply(task, -positive_err);
}


/* TODO: can be optimized later; it is aiming to readdir and so on */
static int
_fuse_reply_buf(struct spdk_vhost_fs_task *task, char *buf, uint32_t bufsize)
{
	int i;
	uint32_t bufoff = 0;
	uint32_t bufrem = bufsize;
	uint32_t size;

	for (i = 1; i < task->in_iovcnt || bufrem > 0; i++) {
		size = task->in_iovs[i].iov_len;
		if (bufrem < task->in_iovs[i].iov_len) {
			size = bufrem;
		}
		memcpy(task->in_iovs[i].iov_base, buf + bufoff, size);
		bufoff += size;
		bufrem -= size;
	}

	if (bufrem != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "Failed to send whole buf by in_iovs! Remain 0x%x bytes",
			      bufrem);
	}

	task->used_len = bufsize;

	return _send_reply(task, 0);
}

#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atim.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctim.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtim.tv_nsec)

static unsigned long
calc_timeout_sec(double t)
{
	if (t > (double) ULONG_MAX) {
		return ULONG_MAX;
	} else if (t < 0.0) {
		return 0;
	} else {
		return (unsigned long) t;
	}
}

static unsigned int
calc_timeout_nsec(double t)
{
	double f = t - (double) calc_timeout_sec(t);
	if (f < 0.0) {
		return 0;
	} else if (f >= 0.999999999) {
		return 999999999;
	} else {
		return (unsigned int)(f * 1.0e9);
	}
}

static void
convert_stat(const struct stat *stbuf, struct fuse_attr *attr)
{
	attr->ino	= stbuf->st_ino;
	attr->mode	= stbuf->st_mode;
	attr->nlink	= stbuf->st_nlink;
	attr->uid	= stbuf->st_uid;
	attr->gid	= stbuf->st_gid;
	attr->rdev	= stbuf->st_rdev;
	attr->size	= stbuf->st_size;
	attr->blksize	= stbuf->st_blksize;
	attr->blocks	= stbuf->st_blocks;
	attr->atime	= stbuf->st_atime;
	attr->mtime	= stbuf->st_mtime;
	attr->ctime	= stbuf->st_ctime;
	attr->atimensec = ST_ATIM_NSEC(stbuf);
	attr->mtimensec = ST_MTIM_NSEC(stbuf);
	attr->ctimensec = ST_CTIM_NSEC(stbuf);
}

static int
_fuse_reply_attr(struct spdk_vhost_fs_task *task, const struct stat *attr,
		 double attr_timeout)
{
	struct fuse_attr_out *outarg;

	outarg = task->in_iovs[1].iov_base;
	memset(outarg, 0, sizeof(*outarg));
	outarg->attr_valid = calc_timeout_sec(attr_timeout);
	outarg->attr_valid_nsec = calc_timeout_nsec(attr_timeout);
	convert_stat(attr, &outarg->attr);

	task->used_len = sizeof(*outarg);

	return _send_reply(task, 0);
}

static void
file_stat_async_cb(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct stat stbuf = {};

	if (fserrno) {
		_fuse_reply_err(task, -fserrno);
		return;
	}

	stbuf.st_mode = S_IFREG | 0644;
	stbuf.st_nlink = 1;
	stbuf.st_size = stat->size;
	stbuf.st_ino = (uint64_t)stbuf.st_ino;

	_fuse_reply_attr(task, &stbuf, 0);
}

static int
do_getattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_getattr_in *arg = (struct fuse_getattr_in *) in_arg;
	struct spdk_file *file;
	const char *file_path;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_getattr: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "getattr_flags=0x%x\n", arg->getattr_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "dummy=0x%x\n", arg->dummy);

	if (task->fvsession->info.minor < 9) {
		SPDK_ERRLOG("Client Fuse Version is not compatible\n");
		_fuse_reply_err(task, EPROTONOSUPPORT);

		return 0;
	}

	if (node_id == 1) {
		struct stat stbuf = {};

		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 2;
		stbuf.st_ino = 0x12345;
		_fuse_reply_attr(task, &stbuf, 0);
	} else {
		file = (struct spdk_file *)node_id;
		file_path = spdk_file_get_name(file);

		spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, file_stat_async_cb, task);
	}

	return 0;
}

static int
do_setattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_setattr_in *arg = (struct fuse_setattr_in *) in_arg;
	struct spdk_file *file;
	const char *file_path;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_setattr: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "valid=0x%x\n", arg->valid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "mode=0x%x\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "size=0x%lx\n", arg->size);

	/* TODO: current setattr does nothing */

	if (node_id == 1) {
		_fuse_reply_err(task, EINVAL);
	} else {
		file = (struct spdk_file *)node_id;
		file_path = spdk_file_get_name(file);

		spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, file_stat_async_cb, task);
	}

	return 0;
}

static void
_do_read_read(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	if (fserrno) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "_do_read_read: failed %d\n", fserrno);
		_fuse_reply_err(task, -fserrno);
		return;
	}

	_fuse_reply_ok(task);
	return;
}

static int
do_read(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct spdk_file *file;
	struct spdk_io_channel *io_channel;
	struct iovec *data_iovs;
	uint64_t valid_len;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_read: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "offset=0x%lx\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "size=0x%x\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "read_flags=0x%x\n", arg->read_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%x\n", arg->flags);

	file = (struct spdk_file *)node_id;
	valid_len = spdk_file_get_length(file);
	if (arg->offset >= valid_len) {
		_fuse_reply_ok(task);
		return 1;
	}

	valid_len -= arg->offset;
	valid_len = spdk_min(valid_len, arg->size);
	task->used_len = valid_len;

	data_iovs = &task->in_iovs[1];
	io_channel = task->fvsession->io_channel;
	spdk_file_readv_async(file, io_channel, data_iovs, task->in_iovcnt - 1, arg->offset, valid_len,
			      _do_read_read, task);

	return 0;
}

static void
_do_write_write(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	if (fserrno) {
		if (fserrno == -EBUSY) {
			_send_reply_none(task, EBUSY);
			return;
		}

		SPDK_ERRLOG("_do_write_write: failed %d\n", fserrno);
		_fuse_reply_err(task, -fserrno);
		return;
	}

	_fuse_reply_ok(task);
	return;
}

static int
do_write(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_write_in *arg = (struct fuse_write_in *) in_arg;
	struct spdk_file *file;
	struct spdk_io_channel *io_channel;
	struct iovec *data_iovs;
	struct fuse_write_out *woarg = task->in_iovs[1].iov_base;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_write: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "offset=0x%lx\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "size=0x%x\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%x\n", arg->flags);

	file = (struct spdk_file *)node_id;

	data_iovs = &task->out_iovs[2];
	io_channel = task->fvsession->io_channel;
	spdk_file_writev_async(file, io_channel, data_iovs, task->out_iovcnt - 2,
			       arg->offset, arg->size, _do_write_write, task);

	woarg->size = arg->size;
	task->used_len = sizeof(struct fuse_write_out);

	return 0;
}

static void
_do_open_open(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct fuse_open_out *o_arg = task->in_iovs[1].iov_base;

	if (fserrno != 0) {
		_fuse_reply_err(task, -fserrno);
		return;
	}

	task->used_len = sizeof(*o_arg);
	_fuse_reply_ok(task);
}

static int
do_open(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *arg = (struct fuse_open_in *) in_arg;
	struct spdk_file *file;
	const char *file_path;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_open: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%x\n", arg->flags);

	file = (struct spdk_file *)node_id;
	file_path = spdk_file_get_name(file);

	/* TODO: adopt open flags, especially for o_create */
	(void)arg->flags;

	spdk_fs_open_file_async(task->fvsession->fvdev->fs, file_path, 0, _do_open_open, task);

	return 0;
}

static void
_do_release_close(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	if (fserrno) {
		SPDK_ERRLOG("do_release_close: failed %d\n", fserrno);
	}

	_fuse_reply_err(task, -fserrno);
	return;
}

static int
do_release(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;
	struct spdk_file *file;
	const char *file_path;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_release: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "release_flags=0x%x\n", arg->release_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%x\n", arg->flags);

	file = (struct spdk_file *)node_id;
	file_path = spdk_file_get_name(file);
	(void)file_path;

	spdk_file_close_async(file, _do_release_close, task);

	return 0;
}

static void
_do_flush_sync(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	if (fserrno) {
		SPDK_ERRLOG("_do_flush_sync: failed %d\n", fserrno);
	}

	_fuse_reply_err(task, -fserrno);
	return;
}

static int
do_flush(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_flush_in *arg = (struct fuse_flush_in *) in_arg;
	struct spdk_file *file;
	struct spdk_io_channel *io_channel;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_flush: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "lock_owner=0x%lx\n", arg->lock_owner);

	io_channel = task->fvsession->io_channel;
	file = (struct spdk_file *)node_id;
	spdk_file_sync_async(file, io_channel, _do_flush_sync, task);

	return 0;
}

static void
_do_unlink_delete(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	if (fserrno) {
		SPDK_ERRLOG("_do_unlink_delete: failed %d\n", fserrno);
	}

	free(args->ori_name);

	_fuse_reply_err(task, -fserrno);
	return;
}

static int
do_unlink(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *)in_arg;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_unlink: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "name is %s\n", name);

	args->ori_name = malloc(strlen(name) + 1 + 1); /* 1 for '/' prefix, 1 for '\0' */
	args->ori_name[0] = '/';
	strcpy(&args->ori_name[1], name);

	spdk_fs_delete_file_async(task->fvsession->fvdev->fs, args->ori_name, _do_unlink_delete, task);

	return 0;
}

static void
_do_rename_rename(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	if (fserrno) {
		SPDK_ERRLOG("_do_rename_rename: failed %d\n", fserrno);
	}

	free(args->ori_name);
	free(args->new_name);

	_fuse_reply_err(task, -fserrno);
	return;
}

static int
do_rename(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename_in *arg = (struct fuse_rename_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_rename: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "newdir = 0x%lx\n", arg->newdir);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "ori_name is %s\n", ori_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "new_name is %s\n", new_name);

	args->ori_name = malloc(strlen(ori_name) + 1 + 1); /* 1 for '/' prefix, 1 for '\0' */
	args->ori_name[0] = '/';
	strcpy(&args->ori_name[1], ori_name);

	args->new_name = malloc(strlen(new_name) + 1 + 1); /* 1 for '/' prefix, 1 for '\0' */
	args->new_name[0] = '/';
	strcpy(&args->new_name[1], new_name);

	spdk_fs_rename_file_async(task->fvsession->fvdev->fs, args->ori_name, args->new_name,
				  _do_rename_rename, task);

	return 0;
}

static int
do_rename2(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename2_in *arg = (struct fuse_rename2_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_rename2: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "newdir = 0x%lx\n", arg->newdir);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags = 0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "ori_name is %s\n", ori_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "new_name is %s\n", new_name);

	args->ori_name = malloc(strlen(ori_name) + 1 + 1); /* 1 for '/' prefix, 1 for '\0' */
	args->ori_name[0] = '/';
	strcpy(&args->ori_name[1], ori_name);

	args->new_name = malloc(strlen(new_name) + 1 + 1); /* 1 for '/' prefix, 1 for '\0' */
	args->new_name[0] = '/';
	strcpy(&args->new_name[1], new_name);

	spdk_fs_rename_file_async(task->fvsession->fvdev->fs, args->ori_name, args->new_name,
				  _do_rename_rename, task);

	return 0;
}

static void
_do_create_open(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct fuse_entry_out *earg;
	struct fuse_open_out *oarg;

	if (fserrno) {
		SPDK_ERRLOG("_do_create_open: failed %d\n", fserrno);
		_fuse_reply_err(task, -fserrno);
		return;
	}

	earg = (struct fuse_entry_out *)task->in_iovs[1].iov_base;
	oarg = (struct fuse_open_out *)(task->in_iovs[1].iov_base + sizeof(struct fuse_entry_out));

	(void)earg;
	(void)oarg;

	/* TODO: add content for open_out */

	_fuse_reply_ok(task);

	return;
}

static void
_do_create_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	const char *file_path;
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);
	size_t entry_size;
	struct fuse_entry_out *earg;
	struct fuse_open_out *oarg;

	if (fserrno) {
		SPDK_ERRLOG("_do_create_stat: failed %d\n", fserrno);
		_fuse_reply_err(task, -fserrno);
		return;
	}

	earg = (struct fuse_entry_out *)task->in_iovs[1].iov_base;
	oarg = (struct fuse_open_out *)(task->in_iovs[1].iov_base + sizeof(struct fuse_entry_out));

	(void)earg;
	(void)oarg;

	entry_size = sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out);
	assert(task->in_iovs[1].iov_len >= entry_size);
	memset(task->in_iovs[1].iov_base, 0, entry_size);
	task->used_len = entry_size;

	/* Set nodeid to be the memaddr of spdk-file */
	earg->nodeid = (uint64_t)args->fp;
	earg->attr_valid = 0;
	earg->entry_valid = 0;

	earg->attr.mode = S_IFREG | 0644;
	earg->attr.nlink = 1;
	earg->attr.ino = stat->blobid;
	earg->attr.size = stat->size;
	earg->attr.blksize = 4096;
	earg->attr.blocks = (stat->size + 4095) / 4096;


	file_path = spdk_file_get_name(args->fp);
	spdk_fs_open_file_async(task->fvsession->fvdev->fs, file_path, 0, _do_create_open, task);
	return;
}

static void
_do_create_create(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);
	const char *file_path;

	if (fserrno != 0) {
		SPDK_ERRLOG("_do_create_create: failed %d\n", fserrno);
		_fuse_reply_err(task, -fserrno);
		return;
	}

	file_path = spdk_file_get_name(f);
	args->fp = f;
	free(args->filepath);

	spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, _do_create_stat, task);

	return;
}

static int
do_create(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct vhost_fuse_info *info = &task->fvsession->info;
	struct fuse_create_in *arg = (struct fuse_create_in *) in_arg;
	char *name = (char *)in_arg + sizeof(struct fuse_create_in);
	char *path;
	uint32_t flags;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);

	if (info->minor < 12) {
		SPDK_ERRLOG("proto_min(%d) < 12\n", info->minor);
		name = (char *)in_arg + sizeof(struct fuse_open_in);
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_create: nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "mode=0x%x\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "umask=0x%x\n", arg->umask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "name=%s\n", name);

	path = malloc(strlen(name) + 2);
	if (path == NULL) {
		return -ENOMEM;
	}
	path[0] = '/';
	strcpy(&path[1], name);
	args->filepath = path;

	flags = SPDK_BLOBFS_OPEN_CREATE;
	spdk_fs_open_file_async(task->fvsession->fvdev->fs, path, flags, _do_create_create, task);

	return 0;
}

static int
do_readdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter *iter_p = (spdk_fs_iter *)arg->fh;
	char *buf;
	uint32_t bufsize, bufoff;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_readdir(node_id=%" PRIu64 ", fh=0x%lx, "
		      "offset=0x%lx, size=0x%x, "
		      "readflags=0x%x, lockowner=0x%lx, flags=0x%x)\n",
		      node_id, arg->fh, arg->offset, arg->size,
		      arg->read_flags, arg->lock_owner, arg->flags);

	/* Only support root dir */
	if (node_id != 1) {
		_fuse_reply_err(task, ENOENT);
		return -1;
	}

	buf = calloc(1, arg->size);
	if (buf == NULL) {
		_fuse_reply_err(task, ENOMEM);
		return -1;
	}
	bufsize = arg->size;
	bufoff = 0;

	/* TODO: consider situation that bufsize is not enough and require continuous readdir cmd */
	while (*iter_p != NULL) {
		struct fuse_dirent *dirent;
		size_t entlen;
		size_t entlen_padded;

		dirent = (struct fuse_dirent *)(buf + bufoff);

		file = spdk_fs_iter_get_file(*iter_p);
		*iter_p = spdk_fs_iter_next(*iter_p);
		filename = spdk_file_get_name(file);

		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "Find file %s\n", &filename[1]);

		entlen = FUSE_NAME_OFFSET + strlen(&filename[1]);
		entlen_padded = FUSE_DIRENT_ALIGN(entlen);
		bufoff += entlen_padded;
		if (bufoff > bufsize) {
			SPDK_ERRLOG("bufsize is not enough\n");
			goto reply;
		}

		/* TODO: correct dirent contents */
		dirent->ino = (uint64_t)file;
		dirent->off = 0;
		dirent->namelen = strlen(&filename[1]);
		dirent->type =  DT_REG;
		strncpy(dirent->name, &filename[1], dirent->namelen);
		memset(dirent->name + dirent->namelen, 0, entlen_padded - entlen);
	}

reply:
	_fuse_reply_buf(task, buf, bufoff);
	free(buf);

	return 0;
}

static int
do_opendir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *i_arg = (struct fuse_open_in *) in_arg;
	struct fuse_open_out *o_arg = task->in_iovs[1].iov_base;
	spdk_fs_iter *iter_p;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_opendir(node_id=%" PRIu64 ", flags=0x%x, unused=0x%x)\n",
		      node_id, i_arg->flags, i_arg->unused);

	/* Only support root dir */
	if (node_id != 1) {
		_fuse_reply_err(task, ENOENT);
		return -1;
	}

	iter_p = calloc(1, sizeof(*iter_p));
	*iter_p = spdk_fs_iter_first(task->fvsession->fvdev->fs);
	memset(o_arg, 0, sizeof(*o_arg));
	o_arg->fh = (uint64_t)iter_p;

	task->used_len = sizeof(*o_arg);
	_fuse_reply_ok(task);

	return 0;
}

static int
do_releasedir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_releasedir(node_id=%" PRIu64
		      ", fh=0x%lx, flags=0x%x, releaseflags=0x%x, lockowner=0x%lx)\n",
		      node_id, arg->fh, arg->flags, arg->release_flags, arg->lock_owner);

	/* Only support root dir */
	if (node_id != 1) {
		_fuse_reply_err(task, ENOENT);
		return -1;
	}

	free((spdk_fs_iter *)arg->fh);
	_fuse_reply_ok(task);

	return 0;
}

static void
_do_lookup_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);
	struct fuse_entry_out *earg = task->in_iovs[1].iov_base;
	uint64_t entry_size;

	entry_size = sizeof(struct fuse_entry_out);
	assert(task->in_iovs[1].iov_len >= entry_size);

	memset(earg, 0, entry_size);

	/* Set nodeid to be the memaddr of spdk-file */
	earg->nodeid = (uint64_t)args->fp;
	earg->attr_valid = 0;
	earg->entry_valid = 0;

	earg->attr.mode = S_IFREG | 0644;
	earg->attr.nlink = 1;
	earg->attr.ino = stat->blobid;
	earg->attr.size = stat->size;
	earg->attr.blksize = 4096;
	earg->attr.blocks = (stat->size + 4095) / 4096;

	task->used_len = entry_size;

	_fuse_reply_ok(task);
	return;
}

static void
_do_lookup_open(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct spdk_fuse_op_args *args = fs_task_get_fuse_op_args(task);
	const char *file_path;

	if (fserrno != 0) {
		_fuse_reply_err(task, -fserrno);
		return;
	}

	file_path = spdk_file_get_name(f);
	args->fp = f;

	spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, _do_lookup_stat, task);

	return;
}

static int
do_lookup(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *) in_arg;
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter iter;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_lookup(parent node_id=%" PRIu64 ", name=%s)\n",
		      node_id, name);

	/* Directory is not supported yet */
	if (node_id != 1) {
		_fuse_reply_err(task, ENOSYS);
		return -1;
	}

	iter = spdk_fs_iter_first(task->fvsession->fvdev->fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);

		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "existed file name is %s, requested filename is %s\n",
			      filename, name);

		if (strcmp(&filename[1], name) == 0) {
			spdk_fs_open_file_async(task->fvsession->fvdev->fs, filename, 0, _do_lookup_open, task);
			return 0;
		}
	}

	_fuse_reply_err(task, ENOENT);
	return 0;
}

static void
_do_forget_close(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_forget done for task %p\n", task);

	_send_reply_none(task, 0);
	return;
}

/* TODO: add refcount for node_id; it needs more consideration. */
static int
do_forget(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_forget_in *arg = (struct fuse_forget_in *) in_arg;
	struct spdk_file *file = (struct spdk_file *)node_id;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_forget(node_id=%" PRIu64 ", nlookup=%lu)\n",
		      node_id, arg->nlookup);

	spdk_file_close_async(file, _do_forget_close, task);

	return 0;
}

static int
do_statfs(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_statfs_out *outarg = task->in_iovs[1].iov_base;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_statfs\n");

	outarg->st.bsize = 4096;
	outarg->st.namelen = 255;

	task->used_len = sizeof(*outarg);

	_fuse_reply_ok(task);

	return 0;
}

static int
do_init(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_init_in *arg = (struct fuse_init_in *) in_arg;
	struct fuse_init_out *outarg;
	struct vhost_fuse_info *info = &task->fvsession->info;
	size_t outargsize = sizeof(*outarg);
	(void) node_id;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "INIT: %u.%u\n", arg->major, arg->minor);
	if (arg->major == 7 && arg->minor >= 6) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%08x\n", arg->flags);
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "max_readahead=0x%08x\n",
			      arg->max_readahead);
	}

	outarg = task->in_iovs[1].iov_base;
	if (sizeof(*outarg) != task->in_iovs[1].iov_len) {
		assert(false);
		return -EINVAL;
	}

	if (arg->major < 7) {
		SPDK_ERRLOG("fuse: unsupported protocol version: %u.%u\n",
			    arg->major, arg->minor);
		_fuse_reply_err(task, EPROTO);
		return 1;
	}

	if (arg->major > 7) {
		/* Wait for a second INIT request with a 7.X version */
		_fuse_reply_ok(task);
		return 1;
	}

	info->major = arg->major;
	info->minor = arg->minor;
	info->flags = arg->flags;

	info->max_readahead = arg->max_readahead;
	info->max_write = VHOST_FS_MAX_RWS;
	info->max_background = VHOST_FS_MAX_RWS;
	info->congestion_threshold = info->max_background * 3 / 4;

	info->time_gran = 1;

	outarg->major = FUSE_KERNEL_VERSION;
	outarg->minor = FUSE_KERNEL_MINOR_VERSION;
	/* Always enable big writes, this is superseded
	   by the max_write option */
	outarg->flags |= FUSE_BIG_WRITES;

	outarg->max_readahead = info->max_readahead;
	outarg->max_write = info->max_write;
	outarg->max_background = info->max_background;
	outarg->congestion_threshold = info->congestion_threshold;
	outarg->time_gran = info->time_gran;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   INIT: %u.%u\n", outarg->major, outarg->minor);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   flags=0x%08x\n", outarg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   max_readahead=0x%08x\n",
		      outarg->max_readahead);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   max_write=0x%08x\n", outarg->max_write);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   max_background=%i\n",
		      outarg->max_background);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   congestion_threshold=%i\n",
		      outarg->congestion_threshold);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "   time_gran=%u\n",
		      outarg->time_gran);

	task->used_len = outargsize;
	_fuse_reply_ok(task);

	return 1;
}

static int
do_destroy(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "do_destroy\n");

	_fuse_reply_ok(task);
	return 1;
}

static int do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);

struct spdk_fuse_op vhost_fs_fuse_ops[] = {
	[FUSE_INIT]	   = { do_init,		"INIT"},
	[FUSE_DESTROY]	   = { do_destroy,	"DESTROY"},
	[FUSE_STATFS]	   = { do_statfs,	"STATFS"},

	[FUSE_LOOKUP]	   = { do_lookup,	"LOOKUP"},
	[FUSE_FORGET]	   = { do_forget,	"FORGET"},
	[FUSE_GETATTR]	   = { do_getattr,	"GETATTR"},
	[FUSE_SETATTR]	   = { do_setattr,	"SETATTR"},

	[FUSE_OPENDIR]	   = { do_opendir,	"OPENDIR"},
	[FUSE_READDIR]	   = { do_readdir,	"READDIR"},
	[FUSE_RELEASEDIR]  = { do_releasedir,	"RELEASEDIR"},

	[FUSE_OPEN]	   = { do_open,		"OPEN"},
	[FUSE_READ]	   = { do_read,		"READ"},
	[FUSE_RELEASE]	   = { do_release,	"RELEASE"},
	[FUSE_FLUSH]	   = { do_flush,	"FLUSH"},
	[FUSE_WRITE]	   = { do_write,	"WRITE"},
	[FUSE_CREATE]	   = { do_create,	"CREATE"},

	[FUSE_UNLINK]	   = { do_unlink,	"UNLINK"},
	[FUSE_RENAME]	   = { do_rename,	"RENAME"},
	[FUSE_RENAME2]     = { do_rename2,	"RENAME2"},

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

	if (vhost_fs_fuse_ops[fuse_in->opcode].func != NULL) {
		rc = vhost_fs_fuse_ops[fuse_in->opcode].func(task, fuse_in->nodeid, fuse_arg_in);
	} else {
		rc = do_nothing(task, fuse_in->nodeid, fuse_arg_in);
	}



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
