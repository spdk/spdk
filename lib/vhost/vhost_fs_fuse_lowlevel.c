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
#include "vhost_fs_fuse_lowlevel.h"

extern struct fuse_lowlevel_ops *fuse_blobfs_ops;

#if 0
static inline struct spdk_fuse_op_args *
fs_task_get_fuse_op_args(struct spdk_vhost_fs_task *task)
{
	return (struct spdk_fuse_op_args *)&task->dummy_args;
}


#endif

static inline struct fuse_in_header *
fs_task_get_fuse_in_header(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;

	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	return fuse_in;
}

struct spdk_filesystem *
spdk_fuse_req_get_fs(fuse_req_t req)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	return task->fvsession->fvdev->fs;
}

struct spdk_io_channel *
spdk_fuse_req_get_io_channel(fuse_req_t req)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	return task->fvsession->io_channel;
}

struct fuse_file_info *
spdk_fuse_req_get_fi(fuse_req_t req)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	return &task->fi;
}

struct spdk_fuse_blobfs_op_args *
spdk_fuse_req_get_dummy_args(fuse_req_t req)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	return (struct spdk_fuse_blobfs_op_args *)&task->dummy_args;
}

int
spdk_fuse_req_get_read_iov(fuse_req_t req, struct iovec **iov)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	if (task->in_iovcnt <= 1) {
		return 0;
	}

	*iov = &task->in_iovs[1];
	return task->in_iovcnt - 1;
}

int
spdk_fuse_req_get_write_iov(fuse_req_t req, struct iovec **iov)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	if (task->out_iovcnt <= 2) {
		return 0;
	}

	*iov = &task->out_iovs[2];
	return task->out_iovcnt - 2;
}

static int
_send_reply_none(struct spdk_vhost_fs_task *task, int error)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse out none: error is %d, unique is 0x%lx\n",
		      error, fs_task_get_fuse_in_header(task)->unique);

	fs_request_finish(task, error);
	return 0;
}

static int
_send_reply(struct spdk_vhost_fs_task *task, int negtive_err)
{
	struct fuse_out_header *out = task->in_iovs[0].iov_base;

	assert(negtive_err > -1000 && negtive_err <= 0);

	if (task->in_iovcnt == 0) {
		return 0;
	}

	task->used_len += sizeof(*out);

	out->unique = fs_task_get_fuse_in_header(task)->unique;
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

#define ST_ATIM_NSEC(stbuf) ((stbuf)->st_atim.tv_nsec)
#define ST_CTIM_NSEC(stbuf) ((stbuf)->st_ctim.tv_nsec)
#define ST_MTIM_NSEC(stbuf) ((stbuf)->st_mtim.tv_nsec)
#define ST_ATIM_NSEC_SET(stbuf, val) (stbuf)->st_atim.tv_nsec = (val)
#define ST_CTIM_NSEC_SET(stbuf, val) (stbuf)->st_ctim.tv_nsec = (val)
#define ST_MTIM_NSEC_SET(stbuf, val) (stbuf)->st_mtim.tv_nsec = (val)

static unsigned long
calc_timeout_sec(double t)
{
	if (t > (double) ULONG_MAX) {
		return ULONG_MAX;
	} else if (t < 0.0) {
		return 0;
	}

	return (unsigned long) t;
}

static unsigned int
calc_timeout_nsec(double t)
{
	double f = t - (double) calc_timeout_sec(t);

	if (f < 0.0) {
		return 0;
	} else if (f >= 0.999999999) {
		return 999999999;
	}

	return (unsigned int)(f * 1.0e9);
}

static void
convert_stat_to_attr(const struct stat *stbuf, struct fuse_attr *attr)
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
	attr->atimensec = stbuf->st_atim.tv_nsec;
	attr->mtimensec = stbuf->st_mtim.tv_nsec;
	attr->ctimensec = stbuf->st_ctim.tv_nsec;
}

static inline void
_fuse_entry_out_printf(struct fuse_entry_out *earg)
{
	struct fuse_attr *attr = &earg->attr;

	(void)attr;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_entry_out:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx\n",earg->nodeid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    generation = 0x%lx\n",earg->generation);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    entry_valid = 0x%lx\n",earg->entry_valid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    attr_valid = 0x%lx\n",earg->attr_valid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    entry_valid_nsec = 0x%x\n",earg->entry_valid_nsec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    attr_valid_nsec = 0x%x\n",earg->attr_valid_nsec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fuse_attr:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      ino = 0x%lx\n", attr->ino);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      size = 0x%lx\n", attr->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      blocks = 0x%lx\n", attr->blocks);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      atime = 0x%lx\n", attr->atime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mtime = 0x%lx\n", attr->mtime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      ctime = 0x%lx\n", attr->ctime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      atimensec = 0x%x\n", attr->atimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mtimensec = 0x%x\n", attr->mtimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      ctimensec = 0x%x\n", attr->ctimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mode = 0x%x\n", attr->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      nlink = 0x%x\n", attr->nlink);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      uid = 0x%x\n", attr->uid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      gid = 0x%x\n", attr->gid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      rdev = 0x%x\n", attr->rdev);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      blksize = 0x%x\n", attr->blksize);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

int
spdk_fuse_reply_err(fuse_req_t req, int err)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;

	return _send_reply(task, err);
}

void
spdk_fuse_reply_none(fuse_req_t req)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;

	_send_reply_none(task, 0);
}

static void
convert_statfs(const struct statvfs *stbuf,
		struct fuse_kstatfs *kstatfs)
{
	kstatfs->bsize	 = stbuf->f_bsize;
	kstatfs->frsize	 = stbuf->f_frsize;
	kstatfs->blocks	 = stbuf->f_blocks;
	kstatfs->bfree	 = stbuf->f_bfree;
	kstatfs->bavail	 = stbuf->f_bavail;
	kstatfs->files	 = stbuf->f_files;
	kstatfs->ffree	 = stbuf->f_ffree;
	kstatfs->namelen = stbuf->f_namemax;
}

static inline void
fuse_statfs_out_printf(struct fuse_statfs_out *arg)
{
	struct fuse_kstatfs *outarg = &arg->st;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_statfs_out:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    blocks = 0x%lx:\n", outarg->blocks);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    bfree = 0x%lx:\n", outarg->bfree);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    bavail = 0x%lx:\n", outarg->bavail);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    files = 0x%lx:\n", outarg->files);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    ffree = 0x%lx:\n", outarg->ffree);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    bsize = 0x%x:\n", outarg->bsize);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    namelen = 0x%x:\n", outarg->namelen);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    frsize = 0x%x:\n", outarg->frsize);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

int
spdk_fuse_reply_statfs(fuse_req_t req, const struct statvfs *stbuf)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;
	struct fuse_statfs_out *arg = task->in_iovs[1].iov_base;

	memset(arg, 0, sizeof(*arg));
	convert_statfs(stbuf, &arg->st);

	fuse_statfs_out_printf(arg);

	task->used_len = sizeof(*arg);
	return _send_reply(task, 0);
}

int
spdk_fuse_reply_read(fuse_req_t req, size_t count)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;

	task->used_len = count;
	return _send_reply(task, 0);
}

int
spdk_fuse_reply_write(fuse_req_t req, size_t count)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;
	struct fuse_write_out *arg = task->in_iovs[1].iov_base;

	memset(arg, 0, sizeof(*arg));
	arg->size = count;

	return _send_reply(task, 0);
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

static void
fill_entry(struct fuse_entry_out *arg,
		       const struct fuse_entry_param *e)
{
	arg->nodeid = e->ino;
	arg->generation = e->generation;
	arg->entry_valid = calc_timeout_sec(e->entry_timeout);
	arg->entry_valid_nsec = calc_timeout_nsec(e->entry_timeout);
	arg->attr_valid = calc_timeout_sec(e->attr_timeout);
	arg->attr_valid_nsec = calc_timeout_nsec(e->attr_timeout);
	convert_stat(&e->attr, &arg->attr);
}

int
spdk_fuse_reply_entry(fuse_req_t req, const struct fuse_entry_param *e)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;
	struct fuse_entry_out *earg = task->in_iovs[1].iov_base;
	uint64_t entry_size;

	entry_size = sizeof(struct fuse_entry_out);
	assert(task->in_iovs[1].iov_len >= entry_size);

	memset(earg, 0, entry_size);
	fill_entry(earg, e);

	_fuse_entry_out_printf(earg);

	task->used_len = entry_size;
	return _send_reply(task, 0);
}

static inline void
_fuse_open_out_printf(struct fuse_open_out *arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_open_out:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx:\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    open_flags = 0x%x:\n", arg->open_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

static void
fill_open(struct fuse_open_out *arg,
		      const struct fuse_file_info *f)
{
	arg->fh = f->fh;
	if (f->direct_io)
		arg->open_flags |= FOPEN_DIRECT_IO;
	if (f->keep_cache)
		arg->open_flags |= FOPEN_KEEP_CACHE;
	if (f->nonseekable)
		arg->open_flags |= FOPEN_NONSEEKABLE;
}

int
spdk_fuse_reply_open(fuse_req_t req, const struct fuse_file_info *f)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;
	struct fuse_open_out *arg = task->in_iovs[1].iov_base;

	memset(arg, 0, sizeof(*arg));
	fill_open(arg, f);

	_fuse_open_out_printf(arg);
	task->used_len = sizeof(*arg);

	return _send_reply(task, 0);;
}

int
spdk_fuse_reply_attr(fuse_req_t req, const struct stat *attr,
		    double attr_timeout)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;
	struct fuse_attr_out *outarg;

	outarg = task->in_iovs[1].iov_base;
	memset(outarg, 0, sizeof(*outarg));

	outarg->attr_valid = calc_timeout_sec(attr_timeout);
	outarg->attr_valid_nsec = calc_timeout_nsec(attr_timeout);
	convert_stat_to_attr(attr, &outarg->attr);

	task->used_len = sizeof(*outarg);
	return _send_reply(task, 0);

}

int
spdk_fuse_reply_buf(fuse_req_t req, const char *buf, size_t size)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;
	int i;
	uint32_t bufoff = 0;
	uint32_t bufrem = size;
	uint32_t iov_len;

	for (i = 1; i < task->in_iovcnt || bufrem > 0; i++) {
		iov_len = task->in_iovs[i].iov_len;
		if (bufrem < task->in_iovs[i].iov_len) {
			iov_len = bufrem;
		}
		memcpy(task->in_iovs[i].iov_base, buf + bufoff, iov_len);
		bufoff += iov_len;
		bufrem -= iov_len;
	}

	if (bufrem != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "Failed to send whole buf by in_iovs! Remain 0x%x bytes",
			      bufrem);
	}

	task->used_len = bufoff;

	return _send_reply(task, 0);
}

int
spdk_fuse_reply_create(fuse_req_t req, const struct fuse_entry_param *e,
		      const struct fuse_file_info *fi)
{
	struct spdk_vhost_fs_task *task = (struct spdk_vhost_fs_task *)req;;
	struct fuse_entry_out *earg;
	struct fuse_open_out *oarg;
	size_t entry_size;

	earg = (struct fuse_entry_out *)task->in_iovs[1].iov_base;
	oarg = (struct fuse_open_out *)(task->in_iovs[1].iov_base + sizeof(struct fuse_entry_out));

	memset(earg, 0, sizeof(*earg));
	memset(oarg, 0, sizeof(*oarg));

	fill_entry(earg, e);
	fill_open(oarg, fi);

	entry_size = sizeof(struct fuse_entry_out) + sizeof(struct fuse_open_out);
	assert(task->in_iovs[1].iov_len >= entry_size);
	task->used_len = entry_size;

	return _send_reply(task, 0);
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
size_t
spdk_fuse_add_direntry(char *buf, size_t bufsize,
			 const char *name, const struct stat *stbuf, off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;
	struct fuse_dirent *dirent;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);

	if ((buf == NULL) || (entlen_padded > bufsize))
	  return entlen_padded;

	dirent = (struct fuse_dirent*) buf;
	dirent->ino = stbuf->st_ino;
	dirent->off = off;
	dirent->namelen = namelen;
	dirent->type = (stbuf->st_mode & 0170000) >> 12;
	strncpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}

/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
size_t
spdk_fuse_add_direntry_plus(char *buf, size_t bufsize,
			      const char *name,
			      const struct fuse_entry_param *e, off_t off)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;

	namelen = strlen(name);
	entlen = FUSE_NAME_OFFSET_DIRENTPLUS + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);
	if ((buf == NULL) || (entlen_padded > bufsize))
	  return entlen_padded;

	struct fuse_direntplus *dp = (struct fuse_direntplus *) buf;
	memset(&dp->entry_out, 0, sizeof(dp->entry_out));
	fill_entry(&dp->entry_out, e);

	struct fuse_dirent *dirent = &dp->dirent;
	dirent->ino = e->attr.st_ino;
	dirent->off = off;
	dirent->namelen = namelen;
	dirent->type = (e->attr.st_mode & 0170000) >> 12;
	strncpy(dirent->name, name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);

	return entlen_padded;
}


#if 1 /* rename start */
static int
do_rename(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename_in *arg = (struct fuse_rename_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;

	fuse_blobfs_ops->rename((fuse_req_t)task, node_id, ori_name, arg->newdir, new_name, 0);

	return 0;
}

static void
info_rename(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename_in *arg = (struct fuse_rename_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;

	(void)new_name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_rename_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    newdir = 0x%lx\n", arg->newdir);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    ori_name = %s\n", ori_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    new_name = %s\n", new_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

static int
do_rename2(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename2_in *arg = (struct fuse_rename2_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;

	fuse_blobfs_ops->rename((fuse_req_t)task, node_id, ori_name, arg->newdir, new_name, arg->flags);

	return 0;
}

static void
info_rename2(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_rename2_in *arg = (struct fuse_rename2_in *) in_arg;
	char *ori_name = (char *)arg + sizeof(*arg);
	char *new_name = ori_name + strlen(ori_name) + 1;

	(void)new_name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_rename2_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    newdir = 0x%lx\n", arg->newdir);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    ori_name = %s\n", ori_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    new_name = %s\n", new_name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* rename end */

#if 1 /* read start */
static int
do_read(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;
	fi->lock_owner = arg->lock_owner;
	fi->flags = arg->flags;

	fuse_blobfs_ops->read((fuse_req_t)task, node_id, arg->size, arg->offset, fi);
	return 0;
}

static void
info_read(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_read_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    offset=0x%lx\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    size=0x%x\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    read_flags=0x%x\n", arg->read_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags=0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* read end */

#if 1 /* write start */
static int
do_write(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_write_in *arg = (struct fuse_write_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;
	char *data = NULL;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;
	fi->writepage = (arg->write_flags & 1) != 0;
	fi->lock_owner = arg->lock_owner;
	fi->flags = arg->flags;

	/* data buffer is in other iov, not follow with arg */
	//data = (char *)(arg) + sizeof(*(arg));

	fuse_blobfs_ops->write((fuse_req_t)task, node_id, data, arg->size,
			 arg->offset, fi);

	return 0;
}

static void
info_write(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_write_in *arg = (struct fuse_write_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_write_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    offset=0x%lx\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    size=0x%x\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    write_flags=0x%x\n", arg->write_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags=0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* write end */

#if 1 /* unlink start */
static int
do_unlink(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *)in_arg;

	fuse_blobfs_ops->unlink((fuse_req_t)task, node_id, name);
	return 0;
}

static void
info_unlink(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *)in_arg;

	(void)name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "parent nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "unlink name = %s\n", name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* unlink end */

#if 1 /* create start */
static int
do_create(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_create_in *arg = (struct fuse_create_in *) in_arg;
	char *name = (char *)in_arg + sizeof(struct fuse_create_in);
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->flags = arg->flags;

	fuse_blobfs_ops->create((fuse_req_t)task, node_id, name, arg->mode, fi);
	return 0;
}

static void
info_create(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_create_in *arg = (struct fuse_create_in *) in_arg;
	char *name = (char *)in_arg + sizeof(struct fuse_create_in);

	(void)arg;
	(void)name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_create_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    mode = 0x%x\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    umask = 0x%x\n", arg->umask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    name=%s\n", name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* create end */

#if 1 /* flush start */
static int
do_flush(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_flush_in *arg = (struct fuse_flush_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;
	fi->flush = 1;
	fi->lock_owner = arg->lock_owner;

	fuse_blobfs_ops->flush((fuse_req_t)task, node_id, fi);

	return 0;
}

static void
info_flush(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_flush_in *arg = (struct fuse_flush_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_flush_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner = 0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* flush end */

#if 1 /* fsync start */
static int
do_fsync(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fsync_in *arg = (struct fuse_fsync_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;

	if (fi->fh == (uint64_t)-1) {
		fi = NULL;
	}

	fuse_blobfs_ops->fsync((fuse_req_t)task, node_id, arg->fsync_flags & 1, fi);

	return 0;
}

static void
info_fsync(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fsync_in *arg = (struct fuse_fsync_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_fsync_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fsync_flags = 0x%x\n", arg->fsync_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* fsync end */

#if 1 /* fallocate start */
static int
do_fallocate(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fallocate_in *arg = (struct fuse_fallocate_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;

	fuse_blobfs_ops->fallocate((fuse_req_t)task, node_id, arg->mode, arg->offset, arg->length, fi);

	return 0;
}

static void
info_fallocate(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fallocate_in *arg = (struct fuse_fallocate_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_fallocate_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    offset = 0x%lx\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    length = 0x%lx\n", arg->length);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    mode = 0x%x\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /*  end */

#if 1 /* access start */
static int
do_access(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_access_in *arg = (struct fuse_access_in *) in_arg;

	fuse_blobfs_ops->access((fuse_req_t)task, node_id, arg->mask);

	return 0;
}

static void
info_access(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_access_in *arg = (struct fuse_access_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_access_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    mask = 0x%x\n", arg->mask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /*  end */

#if 1 /* readdir start */
static int
do_readdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;

	fuse_blobfs_ops->readdir((fuse_req_t)task, node_id, arg->size, arg->offset, fi);

	return 0;
}

static void
info_readdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_read_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx:\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    offset = 0x%lx:\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    size = 0x%x:\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    read_flags = 0x%x:\n", arg->read_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner = 0x%lx:\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x:\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

static int
do_readdirplus(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;

	fuse_blobfs_ops->readdirplus((fuse_req_t)task, node_id, arg->size, arg->offset, fi);

	return 0;
}

static void
info_readdirplus(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_read_in(plus):\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx:\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    offset = 0x%lx:\n", arg->offset);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    size = 0x%x:\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    read_flags = 0x%x:\n", arg->read_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner = 0x%lx:\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x:\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* readdir end */

#if 1 /* mkdir start */
static int
do_mkdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_mkdir_in *arg = (struct fuse_mkdir_in *) in_arg;
	const char *name = (char *)arg + sizeof(*arg);

	fuse_blobfs_ops->mkdir((fuse_req_t)task, node_id, name, arg->mode);

	return 0;
}

static void
info_mkdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_mkdir_in *arg = (struct fuse_mkdir_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_mkdir_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    mode = 0x%x:\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    umask = 0x%x:\n", arg->umask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* dir end */

#if 1 /* rmdir start */
static int
do_rmdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	const char *name = in_arg;

	fuse_blobfs_ops->rmdir((fuse_req_t)task, node_id, name);

	return 0;
}

static void
info_rmdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	const char *name = in_arg;

	(void)name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "rmdir:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    name = %s:\n", name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* rmdir end */

#if 1 /* fsyncdir start */
static int
do_fsyncdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fsync_in *arg = (struct fuse_fsync_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->fh = arg->fh;

	fuse_blobfs_ops->fsyncdir((fuse_req_t)task, node_id, arg->fsync_flags & 1, fi);

	return 0;
}

static void
info_fsyncdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_fsync_in *arg = (struct fuse_fsync_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_fsync_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx:\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fsync_flags = 0x%x:\n", arg->fsync_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* fsyncdir end */

#if 1 /* getattr start */
static int
do_getattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_getattr_in *arg = (struct fuse_getattr_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	if (task->fvsession->info.minor < 9) {
		SPDK_ERRLOG("Client Fuse Version is not compatible\n");
		_fuse_reply_err(task, EPROTONOSUPPORT);

		return 0;
	}

	if (arg->getattr_flags & FUSE_GETATTR_FH) {
		memset(fi, 0, sizeof(*fi));
		fi->fh = arg->fh;
	}

	fuse_blobfs_ops->getattr((fuse_req_t)task, node_id, fi);
	return 0;
}

static void
info_getattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_getattr_in *arg = (struct fuse_getattr_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_getattr_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    getattr_flags=0x%x\n", arg->getattr_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    dummy=0x%x\n", arg->dummy);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* getattr end */

#if 1 /* setattr start */
static void
convert_attr(const struct fuse_setattr_in *attr, struct stat *stbuf)
{
	stbuf->st_mode	       = attr->mode;
	stbuf->st_uid	       = attr->uid;
	stbuf->st_gid	       = attr->gid;
	stbuf->st_size	       = attr->size;
	stbuf->st_atime	       = attr->atime;
	stbuf->st_mtime	       = attr->mtime;
	stbuf->st_ctime        = attr->ctime;
	ST_ATIM_NSEC_SET(stbuf, attr->atimensec);
	ST_MTIM_NSEC_SET(stbuf, attr->mtimensec);
	ST_CTIM_NSEC_SET(stbuf, attr->ctimensec);
}

static int
do_setattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_setattr_in *arg = (struct fuse_setattr_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;
	struct stat stbuf;

	memset(&stbuf, 0, sizeof(stbuf));
	convert_attr(arg, &stbuf);

	if (arg->valid & FATTR_FH) {
		arg->valid &= ~FATTR_FH;
		memset(fi, 0, sizeof(*fi));
		fi->fh = arg->fh;
	}
	arg->valid &=
		FUSE_SET_ATTR_MODE	|
		FUSE_SET_ATTR_UID	|
		FUSE_SET_ATTR_GID	|
		FUSE_SET_ATTR_SIZE	|
		FUSE_SET_ATTR_ATIME	|
		FUSE_SET_ATTR_MTIME	|
		FUSE_SET_ATTR_ATIME_NOW	|
		FUSE_SET_ATTR_MTIME_NOW |
		FUSE_SET_ATTR_CTIME;

	fuse_blobfs_ops->setattr((fuse_req_t)task, node_id, &stbuf, arg->valid, fi);
	return 0;
}

static void
info_setattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_setattr_in *arg = (struct fuse_setattr_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_setattr_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid is %ld\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    valid=0x%x\n", arg->valid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh=0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    size=0x%lx\n", arg->size);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner=0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      atime = 0x%lx\n", arg->atime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mtime = 0x%lx\n", arg->mtime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      ctime = 0x%lx\n", arg->ctime);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      atimensec = 0x%x\n", arg->atimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mtimensec = 0x%x\n", arg->mtimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      ctimensec = 0x%x\n", arg->ctimensec);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      mode = 0x%x\n", arg->mode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      uid = 0x%x\n", arg->uid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "      gid = 0x%x\n", arg->gid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* setattr end */


#if 1 /* open start */
static int
do_open(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *i_arg = (struct fuse_open_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->flags = i_arg->flags;

	fuse_blobfs_ops->open((fuse_req_t)task, node_id, fi);

	return 0;
}

static void
info_open(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *arg = (struct fuse_open_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_open_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* open end */

#if 1 /* opendir start */
static int
do_opendir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *i_arg = (struct fuse_open_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->flags = i_arg->flags;

	fuse_blobfs_ops->opendir((fuse_req_t)task, node_id, fi);

	return 0;
}

static void
info_opendir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *arg = (struct fuse_open_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_open_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x:\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* opendir end */


#if 1 /* release start */
static int
do_release(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->flags = arg->flags;
	fi->fh = arg->fh;

	fi->flush = (arg->release_flags & FUSE_RELEASE_FLUSH) ? 1 : 0;
	fi->lock_owner = arg->lock_owner;

	if (arg->release_flags & FUSE_RELEASE_FLOCK_UNLOCK) {
		fi->flock_release = 1;
		fi->lock_owner = arg->lock_owner;
	}

	fuse_blobfs_ops->release((fuse_req_t)task, node_id, fi);

	return 0;
}

static void
info_release(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_release_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    release_flags = 0x%x\n", arg->release_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner = 0x%lx\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* release end */

#if 1 /* releasedir start */
static int
do_releasedir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;
	struct fuse_file_info *fi = &task->fi;

	memset(fi, 0, sizeof(*fi));
	fi->flags = arg->flags;
	fi->fh = arg->fh;

	fuse_blobfs_ops->releasedir((fuse_req_t)task, node_id, fi);

	return 0;
}

static void
info_releasedir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_release_in:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    fh = 0x%lx:\n", arg->fh);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    flags = 0x%x:\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    release_flags = 0x%x:\n", arg->release_flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    lock_owner = 0x%lx:\n", arg->lock_owner);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* releasedir end */

#if 1 /* lookup start */
static int
do_lookup(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *) in_arg;

	fuse_blobfs_ops->lookup((fuse_req_t)task, node_id, name);

	return 0;
}

static void
info_lookup(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *) in_arg;

	(void)name;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "parent nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "name = %s\n",name);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* lookup end */

#if 1 /* forget start */
static int
do_forget(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_forget_in *arg = (struct fuse_forget_in *) in_arg;

	fuse_blobfs_ops->forget((fuse_req_t)task, node_id, arg->nlookup);

	return 0;
}

static void
info_forget(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_forget_in *arg = (struct fuse_forget_in *) in_arg;

	(void)arg;
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "nodeid = 0x%lx:\n", node_id);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "nlookup = 0x%lx:\n", arg->nlookup);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}
#endif /* forget end */

#if 1 /* statfs start */
static int
do_statfs(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	fuse_blobfs_ops->statfs((fuse_req_t) task, node_id);

	return 0;
}

static void
info_statfs(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
}
#endif /* statfs end */

static inline void
_vhost_fuse_info_config(struct vhost_fuse_info *info, struct fuse_init_in *arg)
{
	info->major = arg->major;
	info->minor = arg->minor;
	info->flags = arg->flags;

	info->max_readahead = arg->max_readahead;
	info->max_write = VHOST_FS_MAX_RWS;
	info->max_background = VHOST_FS_MAX_RWS;
	info->congestion_threshold = info->max_background * 3 / 4;

	info->time_gran = 1;
}

static inline void
_fuse_init_out_config(struct fuse_init_out *outarg, struct vhost_fuse_info *info)
{
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
}

static inline void
_fuse_init_out_printf(struct fuse_init_out *outarg)
{
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
}

static int
do_init(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_init_in *arg = (struct fuse_init_in *) in_arg;
	struct fuse_init_out *outarg;
	struct vhost_fuse_info *info = &task->fvsession->info;
	size_t outargsize = sizeof(*outarg);

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

	_vhost_fuse_info_config(info, arg);
	_fuse_init_out_config(outarg, info);
	_fuse_init_out_printf(outarg);

	task->used_len = outargsize;
	_fuse_reply_ok(task);
	return 1;
}

static void
info_init(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_init_in *arg = (struct fuse_init_in *) in_arg;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "Major.Minor: %u.%u\n", arg->major, arg->minor);
	if (arg->major != 7 || arg->minor < 6) {
		SPDK_ERRLOG("Higher version of FUSE is required\n");
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "flags=0x%08x\n", arg->flags);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "max_readahead=0x%08x\n",
		      arg->max_readahead);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

static int
do_destroy(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	_fuse_reply_ok(task);
	return 1;
}

static void
info_destroy(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
}

static int
do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	_fuse_reply_err(task, ENOSYS);
	return -1;
}

static void
info_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "Undetermined yet\n");
}

struct spdk_fuse_lowlevel_op {
	/* Return 0, successfully submitted; or -errno if failed; 1 if completed without cb */
	int (*func)(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);

	/* Name of the Fuse request */
	const char *op_name;

	/* Print elements inside the fuse request */
	void (*info_request)(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);
} vhost_fs_fuse_ops[] = {
	[FUSE_INIT]	   = { do_init,		"INIT",		info_init},
	[FUSE_DESTROY]	   = { do_destroy,	"DESTROY",	info_destroy},
	[FUSE_STATFS]	   = { do_statfs,	"STATFS",	info_statfs},

	[FUSE_LOOKUP]	   = { do_lookup,	"LOOKUP",	info_lookup},
	[FUSE_FORGET]	   = { do_forget,	"FORGET",	info_forget},
	[FUSE_GETATTR]	   = { do_getattr,	"GETATTR",	info_getattr},
	[FUSE_SETATTR]	   = { do_setattr,	"SETATTR",	info_setattr},

	[FUSE_OPENDIR]	   = { do_opendir,	"OPENDIR",	info_opendir},
	[FUSE_READDIR]	   = { do_readdir,	"READDIR",	info_readdir},
	[FUSE_RELEASEDIR]  = { do_releasedir,	"RELEASEDIR",	info_releasedir},
	[FUSE_MKDIR]	   = { do_mkdir,	"MKDIR",	info_mkdir},
	[FUSE_RMDIR]	   = { do_rmdir,	"RMDIR",	info_rmdir},
	[FUSE_FSYNCDIR]	   = { do_fsyncdir,	"FSYNCDIR",	info_fsyncdir},
	[FUSE_READDIRPLUS] = { do_readdirplus,	"READDIRPLUS",	info_readdirplus},

	[FUSE_OPEN]	   = { do_open,		"OPEN",	info_open},
	[FUSE_READ]	   = { do_read,		"READ",	info_read},
	[FUSE_RELEASE]	   = { do_release,	"RELEASE",	info_release},
	[FUSE_FLUSH]	   = { do_flush,	"FLUSH",	info_flush},
	[FUSE_WRITE]	   = { do_write,	"WRITE",	info_write},
	[FUSE_CREATE]	   = { do_create,	"CREATE",	info_create},
	[FUSE_FSYNC]	   = { do_fsync,	"FSYNC",	info_fsync},
	[FUSE_FALLOCATE]   = { do_fallocate,	"FALLOCATE",	info_fallocate},
	[FUSE_ACCESS]	   = { do_access,	"ACCESS",	info_access},

	[FUSE_UNLINK]	   = { do_unlink,	"UNLINK",	info_unlink},
	[FUSE_RENAME]	   = { do_rename,	"RENAME",	info_rename},
	[FUSE_RENAME2]     = { do_rename2,	"RENAME2",	info_rename2},

	[FUSE_READLINK]	   = { do_nothing,	"READLINK",	info_nothing},
	[FUSE_SYMLINK]	   = { do_nothing,	"SYMLINK",	info_nothing},
	[FUSE_MKNOD]	   = { do_nothing,	"MKNOD",	info_nothing},
	[FUSE_LINK]	   = { do_nothing,	"LINK",	info_nothing},
	[FUSE_SETXATTR]	   = { do_nothing,	"SETXATTR",	info_nothing},
	[FUSE_GETXATTR]	   = { do_nothing,	"GETXATTR",	info_nothing},
	[FUSE_LISTXATTR]   = { do_nothing,	"LISTXATTR",	info_nothing},
	[FUSE_REMOVEXATTR] = { do_nothing,	"REMOVEXATTR",	info_nothing},
	[FUSE_GETLK]	   = { do_nothing,	"GETLK",	info_nothing},
	[FUSE_SETLK]	   = { do_nothing,	"SETLK",	info_nothing},
	[FUSE_SETLKW]	   = { do_nothing,	"SETLKW",	info_nothing},
	[FUSE_INTERRUPT]   = { do_nothing,	"INTERRUPT",	info_nothing},
	[FUSE_BMAP]	   = { do_nothing,	"BMAP",	info_nothing},
	[FUSE_IOCTL]	   = { do_nothing,	"IOCTL",	info_nothing},
	[FUSE_POLL]	   = { do_nothing,	"POLL",	info_nothing},
	[FUSE_NOTIFY_REPLY] = { (void *) 1,	"NOTIFY_REPLY",	info_nothing},
	[FUSE_BATCH_FORGET] = { do_nothing,	"BATCH_FORGET",	info_nothing},
	[CUSE_INIT]	   = { do_nothing,	"CUSE_INIT",	info_nothing},
};

static int
do_undefined(struct spdk_vhost_fs_task *task, struct fuse_in_header *fuse_in)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "FUSE request type '%"PRIu32"'(%s).\n", fuse_in->opcode,
		      "Undefined");

	_fuse_reply_err(task, ENOSYS);

	return -1;
}

static inline void
_fuse_in_header_printf(struct fuse_in_header *fuse_in)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "fuse_in_header:\n");
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    len = 0x%x:\n", fuse_in->len);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    opcode = 0x%x:\n", fuse_in->opcode);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    unique = 0x%lx:\n", fuse_in->unique);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    nodeid = 0x%lx:\n", fuse_in->nodeid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    uid = 0x%x:\n", fuse_in->uid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    gid = 0x%x:\n", fuse_in->gid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    pid = 0x%x:\n", fuse_in->pid);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "    padding = 0x%x:\n", fuse_in->padding);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "\n");
}

int
spdk_vhost_fs_fuse_operate(struct spdk_vhost_fs_task *task)
{
	struct fuse_in_header *fuse_in;
	void *fuse_arg_in;
	int rc;

	fuse_in = fs_task_get_fuse_in_header(task);
	_fuse_in_header_printf(fuse_in);

	/* In general, argument for FUSE operation is the second readable iov.
	 * But for some brief cmds, like Forget, its argument is also in the end of
	 * first readable iov.
	 */
	fuse_arg_in = task->out_iovs[1].iov_base;
	if (task->out_iovs[0].iov_len > sizeof(struct fuse_in_header)) {
		fuse_arg_in = task->out_iovs[0].iov_base + sizeof(struct fuse_in_header);
	}

	/* Reply undefined operation */
	if (vhost_fs_fuse_ops[fuse_in->opcode].func == NULL) {
		return do_undefined(task, fuse_in);
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_OPS, "FUSE request type '%"PRIu32"'(%s).\n", fuse_in->opcode,
		      vhost_fs_fuse_ops[fuse_in->opcode].op_name);
	vhost_fs_fuse_ops[fuse_in->opcode].info_request(task, fuse_in->nodeid, fuse_arg_in);

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
