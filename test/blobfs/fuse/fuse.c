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

#define FUSE_USE_VERSION 30
#include "fuse3/fuse.h"
#include "fuse3/fuse_lowlevel.h"

#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"

struct fuse *g_fuse;
char *g_bdev_name;
char *g_mountpoint;
pthread_t g_fuse_thread;

struct spdk_bs_dev *g_bs_dev;
struct spdk_filesystem *g_fs;
struct spdk_fs_thread_ctx *g_channel;
struct spdk_file *g_file;
int g_fserrno;
int g_fuse_argc = 0;
char **g_fuse_argv = NULL;

static void
__call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

static void
__send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

static int
spdk_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	struct spdk_file_stat stat;
	int rc;

	if (!strcmp(path, "/")) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return 0;
	}

	rc = spdk_fs_file_stat(g_fs, g_channel, path, &stat);
	if (rc == 0) {
		stbuf->st_mode = S_IFREG | 0644;
		stbuf->st_nlink = 1;
		stbuf->st_size = stat.size;
	}

	return rc;
}

static int
spdk_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		  off_t offset, struct fuse_file_info *fi,
		  enum fuse_readdir_flags flags)
{
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter iter;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	iter = spdk_fs_iter_first(g_fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);
		filler(buf, &filename[1], NULL, 0, 0);
	}

	return 0;
}

static int
spdk_fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	return spdk_fs_create_file(g_fs, g_channel, path);
}

static int
spdk_fuse_unlink(const char *path)
{
	return spdk_fs_delete_file(g_fs, g_channel, path);
}

static int
spdk_fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	struct spdk_file *file;
	int rc;

	rc = spdk_fs_open_file(g_fs, g_channel, path, 0, &file);
	if (rc != 0) {
		return -rc;
	}

	rc = spdk_file_truncate(file, g_channel, size);
	if (rc != 0) {
		return -rc;
	}

	spdk_file_close(file, g_channel);

	return 0;
}

static int
spdk_fuse_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	return 0;
}

static int
spdk_fuse_open(const char *path, struct fuse_file_info *info)
{
	struct spdk_file *file;
	int rc;

	rc = spdk_fs_open_file(g_fs, g_channel, path, 0, &file);
	if (rc != 0) {
		return -rc;
	}

	info->fh = (uintptr_t)file;
	return 0;
}

static int
spdk_fuse_release(const char *path, struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;

	return spdk_file_close(file, g_channel);
}

static int
spdk_fuse_read(const char *path, char *buf, size_t len, off_t offset, struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;

	return spdk_file_read(file, g_channel, buf, offset, len);
}

static int
spdk_fuse_write(const char *path, const char *buf, size_t len, off_t offset,
		struct fuse_file_info *info)
{
	struct spdk_file *file = (struct spdk_file *)info->fh;
	int rc;

	rc = spdk_file_write(file, g_channel, (void *)buf, offset, len);
	if (rc == 0) {
		return len;
	} else {
		return rc;
	}
}

static int
spdk_fuse_flush(const char *path, struct fuse_file_info *info)
{
	return 0;
}

static int
spdk_fuse_fsync(const char *path, int datasync, struct fuse_file_info *info)
{
	return 0;
}

static int
spdk_fuse_rename(const char *old_path, const char *new_path, unsigned int flags)
{
	return spdk_fs_rename_file(g_fs, g_channel, old_path, new_path);
}

static struct fuse_operations spdk_fuse_oper = {
	.getattr	= spdk_fuse_getattr,
	.readdir	= spdk_fuse_readdir,
	.mknod		= spdk_fuse_mknod,
	.unlink		= spdk_fuse_unlink,
	.truncate	= spdk_fuse_truncate,
	.utimens	= spdk_fuse_utimens,
	.open		= spdk_fuse_open,
	.release	= spdk_fuse_release,
	.read		= spdk_fuse_read,
	.write		= spdk_fuse_write,
	.flush		= spdk_fuse_flush,
	.fsync		= spdk_fuse_fsync,
	.rename		= spdk_fuse_rename,
};

static void
construct_targets(void)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(g_bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name);
		exit(1);
	}

	g_bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);

	printf("Mounting BlobFS on bdev %s\n", spdk_bdev_get_name(bdev));
}

static void
start_fuse_fn(void *arg1, void *arg2)
{
	struct fuse_args args = FUSE_ARGS_INIT(g_fuse_argc, g_fuse_argv);
	int rc;
	struct fuse_cmdline_opts opts = {};

	g_fuse_thread = pthread_self();
	rc = fuse_parse_cmdline(&args, &opts);
	if (rc != 0) {
		spdk_app_stop(-1);
		fuse_opt_free_args(&args);
		return;
	}
	g_fuse = fuse_new(&args, &spdk_fuse_oper, sizeof(spdk_fuse_oper), NULL);
	fuse_opt_free_args(&args);

	rc = fuse_mount(g_fuse, g_mountpoint);
	if (rc != 0) {
		spdk_app_stop(-1);
		return;
	}

	fuse_daemonize(true /* true = run in foreground */);

	fuse_loop(g_fuse);

	fuse_unmount(g_fuse);
	fuse_destroy(g_fuse);
}

static void
init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct spdk_event *event;

	g_fs = fs;
	g_channel = spdk_fs_alloc_thread_ctx(g_fs);
	event = spdk_event_allocate(1, start_fuse_fn, NULL, NULL);
	spdk_event_call(event);
}

static void
spdk_fuse_run(void *arg1)
{
	construct_targets();
	spdk_fs_load(g_bs_dev, __send_request, init_cb, NULL);
}

static void
shutdown_cb(void *ctx, int fserrno)
{
	fuse_session_exit(fuse_get_session(g_fuse));
	pthread_kill(g_fuse_thread, SIGINT);
	spdk_fs_free_thread_ctx(g_channel);
	spdk_app_stop(0);
}

static void
spdk_fuse_shutdown(void)
{
	spdk_fs_unload(g_fs, shutdown_cb, NULL);
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <conffile> <bdev name> <mountpoint>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts);
	opts.name = "spdk_fuse";
	opts.config_file = argv[1];
	opts.reactor_mask = "0x3";
	opts.shutdown_cb = spdk_fuse_shutdown;

	g_bdev_name = argv[2];
	g_mountpoint = argv[3];
	g_fuse_argc = argc - 2;
	g_fuse_argv = &argv[2];

	rc = spdk_app_start(&opts, spdk_fuse_run, NULL);
	spdk_app_fini();

	return rc;
}
