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
#include "spdk/event.h"
#include "spdk/slab.h"
#include "spdk/log.h"
#include "spdk/string.h"

const char *g_bdev_name;

//static void
//stop_cb(void *ctx, int fserrno)
//{
//	spdk_app_stop(0);
//}
//
//static void
//shutdown_cb(void *arg1, void *arg2)
//{
//	struct spdk_filesystem *fs = arg1;
//
//	printf("done.\n");
//	spdk_fs_unload(fs, stop_cb, NULL);
//}
//
//static void
//init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
//{
//	struct spdk_event *event;
//
//	event = spdk_event_allocate(0, shutdown_cb, fs, NULL);
//	spdk_event_call(event);
//}
static void
_slab_percore_dryrun_cpl(void *ctx)
{
	fprintf(stderr, "spdk slab dryrun operations are done\n");
}

struct slab_dryrun_req {
	struct spdk_slot_item *item;
	char *buf;
	uint32_t len;
};

static void
__slab_read_cb(void *cb_arg, int err)
{
	struct slab_dryrun_req *req = cb_arg;
	int rc;
	assert(err == 0);

	rc = spdk_slab_put_item(req->item);
	assert(rc == 0);

	fprintf(stderr, "slab content is %s\n", req->buf);
	spdk_dma_free(req->buf);
	free(req);
}

static void
__slab_write_cb(void *cb_arg, int err)
{
	struct slab_dryrun_req *req = cb_arg;
	int rc;
	assert(err == 0);

	fprintf(stderr, "slab is written already\n");
	memset(req->buf, 0, req->len);
	rc = spdk_slab_item_obtain(req->item, req->buf, req->len,
				   __slab_read_cb, req);
	assert(rc == 0);

}

static void
_slab_percore_dryrun(void *ctx)
{
	struct spdk_slot_item *item;
	int size;
	int rc;
	char *buf;
	uint32_t len;
	struct slab_dryrun_req *req;

	size = 1024;
	rc = spdk_slab_get_item(size, &item);
	assert(rc == 0);

	len = 1024;
	buf = spdk_dma_malloc(len, 4096, NULL);
	assert(buf);

	strcpy(buf, spdk_cpuset_fmt(spdk_thread_get_cpumask(spdk_get_thread())));

	req = calloc(1, len);
	req->buf = buf;
	req->len = len;
	req->item = item;

	rc = spdk_slab_item_store(item, buf, len,
				  __slab_write_cb, req);
	assert(rc == 0);
}

static void
slab_mgr_create_cb(void *cb_arg, int slab_errno)
{
	fprintf(stderr, "spdk slab is created on bdev %s...\n", g_bdev_name);


	fprintf(stderr, "slab mgr created errno is %d\n", slab_errno);
	assert(slab_errno == 0);

	spdk_for_each_thread(_slab_percore_dryrun, NULL, _slab_percore_dryrun_cpl);
}

static void
spdk_slab_run(void *arg1)
{
	struct spdk_cpuset *core_mask;
	struct spdk_slab_opts opt = {};
	int rc;

	fprintf(stderr, "Create spdk slab on bdev %s...\n", g_bdev_name);
	core_mask = spdk_app_get_core_mask();
	rc = spdk_slab_mgr_create(g_bdev_name, core_mask, &opt, slab_mgr_create_cb, NULL);
	assert(rc == 0);
}

static void
slab_usage(void)
{
	printf(" //waiting to add! -C <size>                 cluster size\n");
}

static int
slab_parse_arg(int ch, char *arg)
{
//	bool has_prefix;

	switch (ch) {
	case 'C':
//		spdk_parse_capacity(arg, &g_cluster_size, &has_prefix);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	if (argc < 3) {
		SPDK_ERRLOG("usage: %s <conffile> <bdevname>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts);
	opts.name = "spdk_slab_test";
	opts.config_file = argv[1];
	opts.reactor_mask = "0xf";
	opts.shutdown_cb = NULL;

	g_bdev_name = argv[2];
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:", NULL,
				      slab_parse_arg, slab_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, spdk_slab_run, NULL);
	spdk_app_fini();

	return rc;
}
