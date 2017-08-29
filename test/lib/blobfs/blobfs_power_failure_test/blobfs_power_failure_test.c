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
#include "spdk/blob_bdev.h"
#include "spdk/log.h"

struct spdk_bs_dev *g_bs_dev;
struct spdk_filesystem *g_fs;
struct spdk_file *g_file;
struct spdk_io_channel *g_channel;
int g_fserrno;
const char *g_bdev_name;
const char *test_case;
int g_result;

static void
__call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

static void
stop_cb(void *ctx, int fserrno)
{
	spdk_app_stop(0);
}

static void
shutdown_cb(void *arg1, void *arg2)
{
	struct spdk_filesystem *fs = arg1;

	spdk_fs_free_io_channel(g_channel);
	spdk_fs_unload(fs, stop_cb, NULL);
}

static void
__send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

static void
start_test_fn(void *arg1, void *arg2)
{
	struct spdk_event *event;

	if (!strcmp(test_case, "power_failure_simulation")) {
		spdk_fs_open_file(g_fs, g_channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
		assert(g_file != NULL);
		spdk_fs_delete_file(g_fs, g_channel, "testfile");
		exit(0);
	}

	if (!strcmp(test_case, "power_recover_check")) {
		spdk_fs_open_file(g_fs, g_channel, "testfile", 0, &g_file);
		if (g_file != NULL) {
			g_result = 1;
		} else {
			printf("file testfile has been deleted\n");
		}
	}

	event = spdk_event_allocate(0, shutdown_cb, g_fs, NULL);
	spdk_event_call(event);

}

static void
fs_load_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct spdk_event *event;

	g_fs = fs;
	g_channel = spdk_fs_alloc_io_channel_sync(g_fs);
	event = spdk_event_allocate(1, start_test_fn, NULL, NULL);
	spdk_event_call(event);
}

static void
spdk_delete_file_test_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(g_bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name);
		exit(1);
	}

	g_bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	printf("using bdev %s\n", g_bdev_name);
	spdk_fs_load(g_bs_dev, __send_request, fs_load_cb, NULL);
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};

	if (argc < 4) {
		SPDK_ERRLOG("usage: %s <conffile> <bdevname> <test_case>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts);
	opts.name = "spdk_mkfs";
	opts.config_file = argv[1];
	opts.reactor_mask = "0x3";
	opts.mem_size = 1024;
	opts.shutdown_cb = NULL;
	test_case = argv[3];
	spdk_fs_set_cache_size(512);

	g_bdev_name = argv[2];
	spdk_app_start(&opts, spdk_delete_file_test_run, NULL, NULL);
	spdk_app_fini();

	return g_result;
}
