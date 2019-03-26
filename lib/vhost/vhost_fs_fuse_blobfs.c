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
#include "spdk/log.h"
#include "spdk_internal/log.h"

#include "vhost_fs_fuse_lowlevel.h"


static void fuse_blobfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *basename);

#if 0
static bool
spdk_fs_is_file_exist(struct spdk_filesystem *fs, const char *basepath, char *fullname)
{
	int filenum;
	char **files_in_dir;
	bool exist = false;

	filenum = spdk_fs_dir_file_num(fs, basepath);
	files_in_dir = spdk_fs_readdir(fs, basepath, filenum);

	for (int i = 0; i < files_in_dir; i++) {
		if (strcmp(fullname, files_in_dir[i]) == 0) {
			exist = true;
			break;
		}
	}

	return exist;
}
#endif

static void
_do_rename_rename(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);

	if (fserrno) {
		SPDK_ERRLOG("_do_rename_rename: failed %d\n", fserrno);
	}

	free(args->op.rename.ori_name);
	free(args->op.rename.new_name);

	spdk_fuse_reply_err(req, fserrno);
	return;
}

static void
fuse_blobfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name_ori,
		      fuse_ino_t newparent, const char *newname,
		      unsigned int flags)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	char *filepath_new, *filepath_ori;
	struct spdk_directory *dir;

	filepath_ori = calloc(1, SPDK_FILE_NAME_MAX);
	assert(filepath_ori != NULL);
	filepath_new = calloc(1, SPDK_FILE_NAME_MAX);
	assert(filepath_new != NULL);

	/* Parent may be the root path which is different than common dir */
	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath_ori, "%s", name_ori);
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath_ori);
		sprintf(filepath_ori, "%s/%s", filepath_ori, name_ori);
	}
	if (spdk_fs_path_is_valid(fs, filepath_ori) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		free(filepath_ori);
		free(filepath_new);
		return;
	}

	if (newparent == FUSE_ROOT_ID) {
		sprintf(filepath_new, "%s", newname);
	} else {
		dir = (struct spdk_directory *)newparent;

		spdk_fs_get_dir_path(dir, filepath_new);
		sprintf(filepath_new, "%s/%s", filepath_new, newname);
	}
	if (spdk_fs_path_is_valid(fs, filepath_new)) {
		spdk_fuse_reply_err(req, -EEXIST);

		free(filepath_ori);
		free(filepath_new);
		return;
	}

	args->op.rename.ori_name = filepath_ori;
	args->op.rename.new_name = filepath_new;

	if (spdk_fs_path_is_dir(fs, filepath_ori)) {
		spdk_fs_rename_dir_async(fs, filepath_ori, filepath_new, _do_rename_rename, req);
	} else {
		spdk_fs_rename_file_async(fs, filepath_ori, filepath_new, _do_rename_rename, req);

	}
}

static void
_do_read_read(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);

	if (fserrno && args->op.read.size != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_BLOBFS, "_do_read_read: failed %d\n", fserrno);

		spdk_fuse_reply_err(req, fserrno);
		return;
	}

	spdk_fuse_reply_read(req, args->op.read.size);
	return;
}

static void
fuse_blobfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
		    off_t offset, struct fuse_file_info *fi)
{
	struct spdk_io_channel *io_channel = spdk_fuse_req_get_io_channel(req);
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct spdk_file *file;
	struct iovec *data_iovs;
	int iov_num;
	size_t valid_len;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, -EISDIR);

		return;
	}

	iov_num = spdk_fuse_req_get_read_iov(req, &data_iovs);
	if (iov_num == 0) {
		spdk_fuse_reply_err(req, -EINVAL);

		return;
	}

	file = (struct spdk_file *)ino;

	valid_len = spdk_file_get_length(file);
	if (offset >= (off_t)valid_len) {
		size = 0;
	} else {
		size = spdk_min(valid_len - offset, size);
	}

	args->op.read.size = size;
	spdk_file_readv_async(file, io_channel, data_iovs, iov_num, offset, size,
			      _do_read_read, req);
}

static void
_do_write_write(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);

	if (fserrno) {
		if (fserrno == -EBUSY) {
			spdk_fuse_reply_err(req, -EBUSY);
			return;
		}

		SPDK_ERRLOG("_do_write_write: failed %d\n", fserrno);
		spdk_fuse_reply_err(req, fserrno);
		return;
	}

	spdk_fuse_reply_write(req, args->op.write.size);
	return;
}

static void
fuse_blobfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
	       size_t size, off_t off, struct fuse_file_info *fi)
{
	struct spdk_io_channel *io_channel = spdk_fuse_req_get_io_channel(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_file *file;
	struct iovec *data_iovs;
	int iov_num;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, -EISDIR);

		return;
	}

	iov_num = spdk_fuse_req_get_write_iov(req, &data_iovs);
	if (iov_num == 0) {
		spdk_fuse_reply_err(req, -EINVAL);

		return;
	}

	file = (struct spdk_file *)ino;
	args->op.write.size = size;
	spdk_file_writev_async(file, io_channel, data_iovs, iov_num,
			off, size, _do_write_write, req);
}

static void
fuse_blobfs_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
			 off_t offset, off_t length, struct fuse_file_info *fi)
{
	spdk_fuse_reply_err(req, -ENOSYS);
}

static void
_do_unlink_delete(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);

	spdk_fuse_reply_err(req, fserrno);

	free(args->op.unlink.filepath);
}

static void
fuse_blobfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	char *filepath;
	struct spdk_directory *dir;

	filepath = calloc(1, SPDK_FILE_NAME_MAX);
	assert(filepath != NULL);

	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath, "%s", name);
	} else if (spdk_fs_is_dir_node(fs, (void *)parent) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		return;
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath);
		sprintf(filepath, "%s/%s", filepath, name);
	}

	if (spdk_fs_path_is_file(fs, filepath) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		free(filepath);
		return;
	}

	args->op.unlink.filepath = filepath;
	spdk_fs_delete_file_async(fs, filepath, _do_unlink_delete, req);

	return;
}

static void
_do_create_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct fuse_file_info *fi = spdk_fuse_req_get_fi(req);
	struct fuse_entry_param e = {};
	struct stat *stbuf = &e.attr;

	if (fserrno != 0) {
		spdk_fuse_reply_err(req, fserrno);

		free(args->op.create.filepath);
		return;
	}

	assert(stat->type == SPDK_BLOBFS_FILE);

	e.ino = (fuse_ino_t)args->op.create.dir_or_file;

	stbuf->st_mode = S_IFREG | 0644;
	stbuf->st_nlink = 1;
	stbuf->st_size = stat->size;
	stbuf->st_blksize = 4096;
	stbuf->st_blocks = (stat->size + 4095) / 4096;

	spdk_fuse_reply_create(req, &e, fi);
	free(args->op.create.filepath);
}

static void
_do_create_open(void *ctx, struct spdk_file *f, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	const char *filepath;

	if (fserrno != 0) {
		spdk_fuse_reply_err(req, fserrno);

		free(args->op.create.filepath);
		return;
	}

	filepath = spdk_file_get_name(f);
	assert(strcmp(filepath, args->op.create.filepath) == 0);

	args->op.create.dir_or_file = f;
	spdk_fs_file_stat_async(fs, filepath, _do_create_stat, req);

	return;
}

static void
fuse_blobfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
		mode_t mode, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	char *filepath;
	struct spdk_directory *dir;
	uint32_t flags;

	filepath = calloc(1, SPDK_FILE_NAME_MAX);
	assert(filepath != NULL);

	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath, "%s", name);
	} else if (spdk_fs_is_dir_node(fs, (void *)parent) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		return;
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath);
		sprintf(filepath, "%s/%s", filepath, name);
	}

	if (spdk_fs_path_is_valid(fs, filepath)) {
		spdk_fuse_reply_err(req, -EEXIST);

		free(filepath);
		return;
	}

	args->op.create.filepath = filepath;

	flags = SPDK_BLOBFS_OPEN_CREATE;
	spdk_fs_open_file_async(fs, filepath, flags, _do_create_open, req);

	return;
}

static void
_do_getattr_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	fuse_req_t req = ctx;
	struct stat stbuf = {};

	if (fserrno) {
		spdk_fuse_reply_err(req, fserrno);
		return;
	}

	stbuf.st_mode = S_IFREG | 0644;
	stbuf.st_nlink = 1;
	stbuf.st_size = stat->size;

	spdk_fuse_reply_attr(req, &stbuf, 0);
}

static void
fuse_blobfs_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_file *file;;
	const char *filepath;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		struct stat stbuf = {};

		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 2;
		spdk_fuse_reply_attr(req, &stbuf, 0);

		return;
	}

	file = (struct spdk_file *)ino;
	filepath = spdk_file_get_name(file);

	spdk_fs_file_stat_async(fs, filepath, _do_getattr_stat, req);
	return;
}

static void
fuse_blobfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
		       int valid, struct fuse_file_info *fi)
{
	/* TODO: current setattr does nothing */
	fuse_blobfs_getattr(req, ino, fi);

	return;
}

static void
fuse_blobfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_directory *dir;
	char dirpath[SPDK_FILE_NAME_MAX];
	char *buf, *p;
	size_t size_rem, offset_rem;
	int file_num;
	char **filename_list;
	int i;
	uint64_t *idx_in_dir = (uint64_t *)fi->fh;
	int idx = *idx_in_dir;

	struct stat dir_st = {
			.st_mode = S_IFDIR | 0755,
		};
	struct stat file_st = {
			.st_mode = S_IFREG | 0644,
		};
	struct stat *st = &file_st;


	if (ino == FUSE_ROOT_ID) {
		sprintf(dirpath, "%s", "");
	} else if (spdk_fs_is_dir_node(fs, (void *)ino) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		return;
	} else {
		dir = (struct spdk_directory *)ino;

		spdk_fs_get_dir_path(dir, dirpath);
	}

	buf = calloc(1, size);
	assert(buf);

	file_num = spdk_fs_dir_file_num(fs, dirpath);
	filename_list = spdk_fs_readdir(fs, dirpath, file_num);

	/* skip previous filenames with offset*/
	p = buf;
	offset_rem = offset;
	for (i = 0; i < file_num; i++) {
		char filepath[SPDK_FILE_NAME_MAX];
		char *filename = filename_list[i];
		size_t entlen_padded;

		if (ino == FUSE_ROOT_ID) {
			sprintf(filepath, "%s", filename);
		} else {
			sprintf(filepath, "%s/%s", dirpath, filename);
		}
		if (spdk_fs_path_is_dir(fs, filepath)) {

			dir = spdk_fs_get_dir_id(fs, filepath);
			st = &dir_st;
			st->st_ino = (__ino_t)dir;
		} else {
			st = &file_st;

			st->st_ino = (__ino_t)0x12345;
		}

		entlen_padded = spdk_fuse_add_direntry(p, size, filename, st, 0);
		if (offset_rem <= entlen_padded) {
			break;
		}

		offset_rem -= entlen_padded;
	}

	/* start to fill buf */
	memset(buf, 0, size);
	p = buf;
	size_rem = size;
	offset_rem = offset;
	for (; i < file_num && idx < file_num; i++) {
		char filepath[SPDK_FILE_NAME_MAX];
		char *filename = filename_list[i];
		size_t entlen_padded;

		if (ino == FUSE_ROOT_ID) {
			sprintf(filepath, "%s", filename);
		} else {
			sprintf(filepath, "%s/%s", dirpath, filename);
		}
		if (spdk_fs_path_is_dir(fs, filepath)) {

			dir = spdk_fs_get_dir_id(fs, filepath);
			st = &dir_st;
			st->st_ino = (__ino_t)dir;
		} else {
			st = &file_st;
		}

		entlen_padded = spdk_fuse_add_direntry(p, size_rem, filename, st, offset);
		if (size_rem < entlen_padded) {
			break;
		}

		p += entlen_padded;
		size_rem -= entlen_padded;
		offset += entlen_padded;
	}

	if(idx < i) {
		idx = i;
		*idx_in_dir = idx;
	}

	spdk_fuse_reply_buf(req, buf, size - size_rem);

	free(buf);
	return;
}

static void
fuse_blobfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
		     mode_t mode)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_directory *dir;
	char filepath[SPDK_FILE_NAME_MAX];

	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath, "%s", name);
	} else if (spdk_fs_is_dir_node(fs, (void *)parent) == false) {
		spdk_fuse_reply_err(req, -ENOTDIR);

		return;
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath);
		sprintf(filepath, "%s/%s", filepath, name);
	}


	spdk_fs_mkdir(fs, filepath, mode);

	fuse_blobfs_lookup(req, parent, name);

	return;
}

static void
_do_rmdir_delete_dir(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;

	spdk_fuse_reply_err(req, fserrno);
}

static void
fuse_blobfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_directory *dir;
	char filepath[SPDK_FILE_NAME_MAX];

	if (parent != FUSE_ROOT_ID && spdk_fs_is_dir_node(fs, (void *)parent) == false) {
		spdk_fuse_reply_err(req, -ENOENT);
		return;
	}

	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath, "%s", name);
	} else if (spdk_fs_is_dir_node(fs, (void *)parent) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		return;
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath);
		sprintf(filepath, "%s/%s", filepath, name);
	}

	spdk_fs_delete_dir_async(fs, filepath, _do_rmdir_delete_dir, req);
}

static void
fuse_blobfs_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
			struct fuse_file_info *fi)
{
	spdk_fuse_reply_err(req, -ENOSYS);
}

static void
fuse_blobfs_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
			   off_t offset, struct fuse_file_info *fi)
{
	spdk_fuse_reply_err(req, -ENOSYS);
}

static void
_do_open_open(void *ctx, struct spdk_file *f, int fserrno)
{
	fuse_req_t req = ctx;
	struct fuse_file_info *fi = spdk_fuse_req_get_fi(req);

	if (fserrno != 0) {
		spdk_fuse_reply_err(req, fserrno);
		return;
	}

	spdk_fuse_reply_open(req, fi);
}

static void
fuse_blobfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	const char *filepath;
	struct spdk_file *file;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, -EISDIR);

		return;
	}

	file = (struct spdk_file *)ino;
	filepath = spdk_file_get_name(file);

	spdk_fs_open_file_async(fs, filepath, 0, _do_open_open, req);

	return;
}

static void
_do_release_close(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;

	if (fserrno) {
		SPDK_ERRLOG("do_release_close: failed %d\n", fserrno);
	}

	spdk_fuse_reply_err(req, fserrno);
	return;
}

static void
fuse_blobfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_file *file;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, -EISDIR);

		return;
	}

	file = (struct spdk_file *)ino;
	spdk_file_close_async(file, _do_release_close, req);

	return;
}

static void
_do_flush_sync(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;

	if (fserrno) {
		SPDK_ERRLOG("_do_flush_sync: failed %d\n", fserrno);
	}

	spdk_fuse_reply_err(req, fserrno);
	return;
}

static void
fuse_blobfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct spdk_io_channel *io_channel = spdk_fuse_req_get_io_channel(req);
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_file *file;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, -EISDIR);

		return;
	}

	file = (struct spdk_file *)ino;
	spdk_file_sync_async(file, io_channel, _do_flush_sync, req);

	return;
}

static void
fuse_blobfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
		     struct fuse_file_info *fi)
{
	fuse_blobfs_flush(req, ino, fi);
}

static void
fuse_blobfs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
	spdk_fuse_reply_err(req, 0);
}

static void
fuse_blobfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	uint64_t *idx_in_dir;

	idx_in_dir = calloc(1, sizeof(uint64_t));
	assert(idx_in_dir);

	fi->fh = (uint64_t)idx_in_dir;

	if (ino == FUSE_ROOT_ID ||  spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_open(req, fi);
	} else {
		spdk_fuse_reply_err(req, -EIO);
	}

	return;
}

static void
fuse_blobfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);

	if (ino == FUSE_ROOT_ID ||  spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_err(req, 0);

		free((uint64_t *)fi->fh);
	} else {
		spdk_fuse_reply_err(req, -EIO);
	}
}

static void
_do_lookup_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct fuse_entry_param e = {};
	struct stat *stbuf = &e.attr;

	/* Set nodeid to be the memaddr of spdk-file */
	e.ino = (fuse_ino_t)args->op.lookup.dir_or_file;

	if (stat->type == SPDK_BLOBFS_FILE) {
		stbuf->st_mode = S_IFREG | 0644;
		stbuf->st_nlink = 1;
		stbuf->st_size = stat->size;
		stbuf->st_blksize = 4096;
		stbuf->st_blocks = (stat->size + 4095) / 4096;
	} else if (stat->type == SPDK_BLOBFS_DIRECTORY) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		assert(0);
	}

	spdk_fuse_reply_entry(req, &e);

	free(args->op.lookup.filepath);
	return;
}

static void
_do_lookup_open(void *ctx, struct spdk_file *f, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	const char *filepath;

	if (fserrno != 0) {
		spdk_fuse_reply_err(req, fserrno);

		free(args->op.lookup.filepath);
		return;
	}

	filepath = spdk_file_get_name(f);
	assert(strcmp(filepath, args->op.lookup.filepath) == 0);

	args->op.lookup.dir_or_file = f;
	spdk_fs_file_stat_async(fs, filepath, _do_lookup_stat, req);

	return;
}

static void
fuse_blobfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *basename)
{
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	char *filepath;
	struct spdk_directory *dir;

	filepath = calloc(1, SPDK_FILE_NAME_MAX);
	assert(filepath != NULL);

	/* Parent may be the root path which is different than common dir */
	if (parent == FUSE_ROOT_ID) {
		sprintf(filepath, "%s", basename);
	} else {
		dir = (struct spdk_directory *)parent;

		spdk_fs_get_dir_path(dir, filepath);
		sprintf(filepath, "%s/%s", filepath, basename);
	}

	if (spdk_fs_path_is_valid(fs, filepath) == false) {
		spdk_fuse_reply_err(req, -ENOENT);

		free(filepath);
		return;
	}

	/* object in lookup can be dir or file which are different */
	args->op.lookup.filepath = filepath;
	if (spdk_fs_path_is_file(fs, filepath)) {
		spdk_fs_open_file_async(fs, filepath, 0, _do_lookup_open, req);
	} else {
		dir = spdk_fs_get_dir_id(fs, filepath);
		args->op.lookup.dir_or_file = dir;

		spdk_fs_file_stat_async(fs, filepath, _do_lookup_stat, req);
	}

	return;
}

static void
_do_forget_close(void *ctx, int fserrno)
{
	fuse_req_t req = ctx;
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);

	if (fserrno && args->op.forget.fserrno == 0) {
		args->op.forget.fserrno = fserrno;
	}

	args->op.forget.nlookup--;
	if (args->op.forget.nlookup != 0) {
		return;
	}

	if (args->op.forget.fserrno) {
		spdk_fuse_reply_err(req, args->op.forget.fserrno);
	} else {
		spdk_fuse_reply_none(req);
	}

	return;
}

static void
fuse_blobfs_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct spdk_fuse_blobfs_op_args *args = spdk_fuse_req_get_dummy_args(req);
	struct spdk_filesystem *fs = spdk_fuse_req_get_fs(req);
	struct spdk_file *file;
	uint64_t i;

	if (ino == FUSE_ROOT_ID || spdk_fs_is_dir_node(fs, (void *)ino)) {
		spdk_fuse_reply_none(req);
		return;
	}


	file = (struct spdk_file *)ino;
	args->op.forget.nlookup = nlookup;
	args->op.forget.fserrno = 0;

	for (i = 0; i < nlookup; i++) {
		spdk_file_close_async(file, _do_forget_close, req);
	}

	return;
}

static void
fuse_blobfs_statfs(fuse_req_t req, fuse_ino_t ino)
{
	struct statvfs stbuf = {};

	stbuf.f_namemax = SPDK_FILE_NAME_MAX;
	stbuf.f_bsize = 4096;

	spdk_fuse_reply_statfs(req, &stbuf);

	return;
}

struct fuse_lowlevel_ops fuse_blobfs_ops_t = {
		.statfs = fuse_blobfs_statfs,
		.lookup = fuse_blobfs_lookup,
		.forget = fuse_blobfs_forget,
		.getattr = fuse_blobfs_getattr,
		.setattr = fuse_blobfs_setattr,

		.open = fuse_blobfs_open,
		.release = fuse_blobfs_release,
		.flush = fuse_blobfs_flush,
		.create = fuse_blobfs_create,
		.unlink = fuse_blobfs_unlink,
		.read = fuse_blobfs_read,
		.write = fuse_blobfs_write,
		.fallocate = fuse_blobfs_fallocate,
		.fsync = fuse_blobfs_fsync,

		.opendir = fuse_blobfs_opendir,
		.releasedir = fuse_blobfs_releasedir,
		.readdir = fuse_blobfs_readdir,
		.readdirplus = fuse_blobfs_readdirplus,
		.mkdir = fuse_blobfs_mkdir,
		.rmdir = fuse_blobfs_rmdir,
		.fsyncdir = fuse_blobfs_fsyncdir,

		.rename = fuse_blobfs_rename,
		.access = fuse_blobfs_access,
};


struct fuse_lowlevel_ops *fuse_blobfs_ops = &fuse_blobfs_ops_t;

SPDK_LOG_REGISTER_COMPONENT("vhost_fs_blobfs", SPDK_LOG_VHOST_FS_BLOBFS)
