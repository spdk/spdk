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
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blobfs.h"
#include "spdk/io_channel.h"
#include "spdk/bdev.h"
#include "spdk/blob_bdev.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

static bool g_spdk_initialized = false;
static struct spdk_app_opts g_opts;
static const char *g_config_file;
static const char *g_bdev;
static struct spdk_filesystem *g_fs;
static pthread_t g_spdk_tid;
static volatile bool g_spdk_ready = false;
static struct spdk_bs_dev *g_bs_dev;

static void
fs_unload_cb(void *ctx, int fserrno)
{
	assert(fserrno == 0);

	spdk_app_stop(0);
}

static void
fio_blobfs_shutdown(void)
{
	if (g_fs != NULL) {
		spdk_fs_unload(g_fs, fs_unload_cb, NULL);
	} else {
		fs_unload_cb(NULL, 0);
	}
}

static void
fs_load_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	if (fserrno == 0) {
		g_fs = fs;
	}
	g_spdk_ready = true;
}

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

static void
blobfs_fio_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;

	pthread_setname_np(pthread_self(), "spdk");
	bdev = spdk_bdev_get_by_name(g_bdev);

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev);
		exit(1);
	}

	if (!spdk_bdev_claim(bdev, NULL, NULL)) {
		SPDK_ERRLOG("could not claim bdev %s\n", g_bdev);
		exit(1);
	}

	g_bs_dev = spdk_bdev_create_bs_dev(bdev);
	printf("using bdev %s\n", g_bdev);
	spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);
}

static void *
initialize_spdk(void *arg)
{
	spdk_app_init(&g_opts);
	spdk_app_start(blobfs_fio_run, NULL, NULL);
	spdk_app_fini();
	pthread_exit(NULL);
}

static void
_spdk_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	/* Not supported. */
	assert(false);
}

static int spdk_fio_setup(struct thread_data *td)
{
	struct spdk_io_channel *channel;
	struct fio_file *f;
	unsigned int i;

	if (g_config_file == NULL) {
		fprintf(stderr, "no conf file specified\n");
		return -1;
	}

	if (g_bdev == NULL) {
		fprintf(stderr, "no bdev specified\n");
		return -1;
	}

	if (!g_spdk_initialized) {
		spdk_app_opts_init(&g_opts);
		g_opts.config_file = g_config_file;
		g_opts.reactor_mask = "0x1";
		g_opts.dpdk_mem_size = 8192;
		g_opts.shutdown_cb = fio_blobfs_shutdown;

		pthread_create(&g_spdk_tid, NULL, &initialize_spdk, NULL);
		while (!g_spdk_ready)
			;

		g_spdk_initialized = true;
	}

	spdk_allocate_thread(_spdk_send_msg, NULL);

	channel = spdk_fs_alloc_io_channel_sync(g_fs);
	td->io_ops_data = channel;

	for_each_file(td, f, i) {
		struct spdk_file *file;
		struct spdk_file_stat stat;
		int rc;

		if (td_write(td) && td_random(td)) {
			fprintf(stderr, "blobfs does not support random writes currently\n");
			return -1;
		}

		rc = spdk_fs_file_stat(g_fs, channel, f->file_name, &stat);
		if (rc != 0) {
			spdk_fs_create_file(g_fs, channel, f->file_name);
			stat.size = 0;
		}
		if (td_write(td)) {
			stat.size = 0;
		} else if (stat.size < td->o.size) {
			stat.size = td->o.size;
		}

		rc = spdk_fs_open_file(g_fs, channel, f->file_name, 0, &file);
		if (rc) {
			fprintf(stderr, "could not open %s\n", f->file_name);
			return -1;
		}

		spdk_file_truncate(file, channel, stat.size);

		spdk_file_close(file, channel);

		f->real_file_size = stat.size;
		f->filetype = FIO_TYPE_FILE;
		fio_file_set_size_known(f);
	}

	return 0;
}

static int spdk_fio_open(struct thread_data *td, struct fio_file *f)
{
	struct spdk_file *file;
	struct spdk_io_channel *channel = td->io_ops_data;
	int rc;

	rc = spdk_fs_open_file(g_fs, channel, f->file_name, 0, &file);

	if (rc != 0) {
		return rc;
	}

	f->engine_data = file;
	return 0;
}

static int spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	struct spdk_file *file = f->engine_data;
	struct spdk_io_channel *channel = td->io_ops_data;

	spdk_file_close(file, channel);
	f->engine_data = NULL;

	return 0;
}

static int spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_io_channel *channel = td->io_ops_data;
	struct spdk_file *file = io_u->file->engine_data;
	int rc;

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = spdk_file_read(file, channel, io_u->buf, io_u->offset, io_u->xfer_buflen);
		break;
	case DDIR_WRITE:
		if (io_u->offset == 0) {
			spdk_file_close(file, channel);
			spdk_fs_delete_file(g_fs, channel, io_u->file->file_name);
			spdk_fs_open_file(g_fs, channel, io_u->file->file_name, SPDK_BLOBFS_OPEN_CREATE, &file);
			io_u->file->engine_data = file;
		}
		rc = spdk_file_write(file, channel, io_u->buf, io_u->offset, io_u->xfer_buflen);
		break;
	default:
		rc = -1;
		break;
	}

	if (rc < 0) {
		assert(0);
	}

	return FIO_Q_COMPLETED;
}

static int spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static void spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_io_channel *channel = td->io_ops_data;

	spdk_put_io_channel(channel);
	spdk_app_start_shutdown();
	pthread_join(g_spdk_tid, NULL);
}

static int
str_conf_cb(void *data, const char *input)
{
	g_config_file = strdup(input);
	return 0;
}

static int
str_bdev_cb(void *data, const char *input)
{
	g_bdev = strdup(input);
	return 0;
}

static struct fio_option options[] = {
	{
		.name		= "spdk_conf",
		.lname		= "spdk configuration file",
		.type		= FIO_OPT_STR_STORE,
		.cb		= str_conf_cb,
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_bdev",
		.lname		= "spdk block device",
		.type		= FIO_OPT_STR_STORE,
		.cb		= str_bdev_cb,
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

/* FIO imports this structure using dlsym */
struct ioengine_ops ioengine = {
	.name			= "spdk_blobfs",
	.version		= FIO_IOOPS_VERSION,
	.queue			= spdk_fio_queue,
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	.setup			= spdk_fio_setup,
	.options		= options,
	.option_struct_size	= 1,
	.flags			= FIO_SYNCIO | FIO_NODISKUTIL,
};
