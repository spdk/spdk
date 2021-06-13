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

#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/blobfs.h"

#include "blobfs_fuse.h"

#define FUSE_USE_VERSION 30
#include "fuse3/fuse.h"
#include "fuse3/fuse_lowlevel.h"

struct spdk_blobfs_fuse {
	char *bdev_name;
	char *mountpoint;
	struct spdk_fs_thread_ctx *channel;
	struct spdk_filesystem *fs;

	struct fuse *fuse_handle;
	pthread_t	fuse_tid;

	blobfs_fuse_unmount_cb cb_fn;
	void *cb_arg;
};

/* Each thread serves one blobfs */
static __thread struct spdk_blobfs_fuse *thd_bfuse;

static void
blobfs_fuse_free(struct spdk_blobfs_fuse *bfuse)
{
	if (bfuse == NULL) {
		return;
	}

	free(bfuse->bdev_name);
	free(bfuse->mountpoint);
	free(bfuse);
}

static void
__call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

void
blobfs_fuse_send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

static int
fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	struct spdk_file_stat stat;
	int rc;

	if (!strcmp(path, "/")) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return 0;
	}

	rc = spdk_fs_file_stat(thd_bfuse->fs, thd_bfuse->channel, path, &stat);
	if (rc == 0) {
		stbuf->st_mode = S_IFREG | 0644;
		stbuf->st_nlink = 1;
		stbuf->st_size = stat.size;
	}

	return rc;
}

static int
fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	     off_t offset, struct fuse_file_info *fi,
	     enum fuse_readdir_flags flags)
{
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter iter;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	iter = spdk_fs_iter_first(thd_bfuse->fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);
		filler(buf, &filename[1], NULL, 0, 0);
	}

	return 0;
}

static int
fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	return spdk_fs_create_file(thd_bfuse->fs, thd_bfuse->channel, path);
}

static int
fuse_unlink(const char *path)
{
	return spdk_fs_delete_file(thd_bfuse->fs, thd_bfuse->channel, path);
}

static int
fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	struct spdk_file *file;
	int rc;

	rc = spdk_fs_open_file(thd_bfuse->fs, thd_bfuse->channel, path, 0, &file);
	if (rc != 0) {
		return -rc;
	}

	rc = spdk_file_truncate(file, thd_bfuse->channel, size);
	if (rc != 0) {
		return -rc;
	}

	spdk_file_close(file, thd_bfuse->channel);

	return 0;
}

static int
fuse_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	return 0;
}

static int
fuse_open(const char *path, struct fuse_file_info *info)
{
	struct spdk_file *file;
	int rc;

	rc = spdk_fs_open_file(thd_bfuse->fs, thd_bfuse->channel, path, 0, &file);
	if (rc != 0) {
		return -rc;
	}

	info->fh = (uintptr_t)file;
	return 0;
}

static int
fuse_release(const char *path, struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;

	return spdk_file_close(file, thd_bfuse->channel);
}

static int
fuse_read(const char *path, char *buf, size_t len, off_t offset, struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;

	return spdk_file_read(file, thd_bfuse->channel, buf, offset, len);
}

static int
fuse_write(const char *path, const char *buf, size_t len, off_t offset,
	   struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;
	int rc;

	rc = spdk_file_write(file, thd_bfuse->channel, (void *)buf, offset, len);
	if (rc == 0) {
		return len;
	} else {
		return rc;
	}
}

static int
fuse_flush(const char *path, struct fuse_file_info *info)
{
	return 0;
}

static int
fuse_fsync(const char *path, int datasync, struct fuse_file_info *info)
{
	return 0;
}

static int
fuse_rename(const char *old_path, const char *new_path, unsigned int flags)
{
	return spdk_fs_rename_file(thd_bfuse->fs, thd_bfuse->channel, old_path, new_path);
}

static struct fuse_operations spdk_fuse_oper = {
	.getattr	= fuse_getattr,
	.readdir	= fuse_readdir,
	.mknod		= fuse_mknod,
	.unlink		= fuse_unlink,
	.truncate	= fuse_truncate,
	.utimens	= fuse_utimens,
	.open		= fuse_open,
	.release	= fuse_release,
	.read		= fuse_read,
	.write		= fuse_write,
	.flush		= fuse_flush,
	.fsync		= fuse_fsync,
	.rename		= fuse_rename,
};

static void *
fuse_loop_new_thread(void *arg)
{
	struct spdk_blobfs_fuse *bfuse = arg;

	spdk_unaffinitize_thread();

	thd_bfuse = bfuse;
	SPDK_NOTICELOG("Start to loop blobfs on bdev %s mounted at %s\n", bfuse->bdev_name,
		       bfuse->mountpoint);

	bfuse->channel = spdk_fs_alloc_thread_ctx(bfuse->fs);

	fuse_loop(bfuse->fuse_handle);
	fuse_unmount(bfuse->fuse_handle);
	fuse_destroy(bfuse->fuse_handle);
	SPDK_NOTICELOG("Blobfs on bdev %s unmounted from %s\n", bfuse->bdev_name, bfuse->mountpoint);

	spdk_fs_free_thread_ctx(bfuse->channel);

	bfuse->cb_fn(bfuse->cb_arg);

	blobfs_fuse_free(bfuse);

	pthread_exit(NULL);
}

int
blobfs_fuse_start(const char *bdev_name, const char *mountpoint, struct spdk_filesystem *fs,
		  blobfs_fuse_unmount_cb cb_fn, void *cb_arg, struct spdk_blobfs_fuse **_bfuse)
{
	/* Set argv[1] as bdev_name in order to show bdev_name as the mounting source */
	char *argv[1] = {(char *)bdev_name};
	struct fuse_args args = FUSE_ARGS_INIT(1, argv);
	struct fuse_cmdline_opts opts = {};
	struct fuse *fuse_handle;
	struct spdk_blobfs_fuse *bfuse;
	pthread_t tid;
	int rc;

	bfuse = (struct spdk_blobfs_fuse *)calloc(1, sizeof(*bfuse));
	if (bfuse == NULL) {
		return -ENOMEM;
	}

	bfuse->bdev_name = strdup(bdev_name);
	bfuse->mountpoint = strdup(mountpoint);
	if (!bfuse->bdev_name || !bfuse->mountpoint) {
		rc = -ENOMEM;
		goto err;
	}
	bfuse->fs = fs;
	bfuse->cb_fn = cb_fn;
	bfuse->cb_arg = cb_arg;

	rc = fuse_parse_cmdline(&args, &opts);
	assert(rc == 0);

	fuse_handle = fuse_new(&args, &spdk_fuse_oper, sizeof(spdk_fuse_oper), NULL);
	fuse_opt_free_args(&args);
	if (fuse_handle == NULL) {
		SPDK_ERRLOG("could not create fuse handle!\n");
		rc = -1;
		goto err;
	}
	bfuse->fuse_handle = fuse_handle;

	rc = fuse_mount(bfuse->fuse_handle, bfuse->mountpoint);
	if (rc != 0) {
		SPDK_ERRLOG("could not mount fuse handle\n");
		rc = -1;
		goto err;
	}

	rc = pthread_create(&tid, NULL, fuse_loop_new_thread, bfuse);
	if (rc != 0) {
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		rc = -rc;
		goto err;
	}
	bfuse->fuse_tid = tid;

	rc = pthread_detach(tid);
	if (rc != 0) {
		SPDK_ERRLOG("could not detach thread for fuse loop thread: %s\n", spdk_strerror(rc));
		rc = -rc;
		goto err;
	}

	*_bfuse = bfuse;
	return 0;

err:
	blobfs_fuse_free(bfuse);

	return rc;
}

void
blobfs_fuse_stop(struct spdk_blobfs_fuse *bfuse)
{
	if (bfuse) {
		fuse_session_exit(fuse_get_session(bfuse->fuse_handle));
		pthread_kill(bfuse->fuse_tid, SIGINT);
	}
}
