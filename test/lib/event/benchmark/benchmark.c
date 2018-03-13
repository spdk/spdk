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

#include "benchmark.h"

#include "rte_config.h"
#include "rte_mempool.h"
#include "rte_malloc.h"

static bool g_done = false;
static uint64_t g_time_in_sec = 1;
static bool g_use_callback_api = false;
static bool g_calculate_iova = false;
static bool g_use_structure_link_api = false;
static uint64_t g_io_done = 0;
static struct rte_mempool *g_iov_pool;

static bool
benchmark_cb(void *arg, struct benchmark_iov *biov)
{
	struct benchmark_ctx	*ctx = arg;
	struct iovec		*iov = &ctx->iov[ctx->current_iov++];

	biov->buf = iov->iov_base;
	if (biov->calculate_iova) {
		biov->iova = spdk_vtophys(biov->buf);
	}
	biov->len = iov->iov_len;

	return (ctx->current_iov == IOV_COUNT);
}

static void
callback_poller(void *arg)
{
	struct benchmark_ctx *ctx = arg;

	ctx->current_iov = 0;
	submit_callback(ctx, benchmark_cb);
	ctx->io_done++;

	if (g_done) {
		spdk_poller_unregister(&ctx->poller);
		__sync_fetch_and_add(&g_io_done, ctx->io_done);
		free(ctx);
	}
}

static void
callback_iova_poller(void *arg)
{
	struct benchmark_ctx *ctx = arg;

	ctx->current_iov = 0;
	submit_callback_iova(ctx, benchmark_cb);
	ctx->io_done++;

	if (g_done) {
		spdk_poller_unregister(&ctx->poller);
		__sync_fetch_and_add(&g_io_done, ctx->io_done);
		free(ctx);
	}
}

static void
structure_poller(void *arg)
{
	struct benchmark_ctx *ctx = arg;
	struct benchmark_iov *iov;
	int i;

	rte_mempool_get(g_iov_pool, (void **)&iov);
	if (!iov) {
		SPDK_ERRLOG("could not allocate iov\n");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < IOV_COUNT; i++) {
		iov[i].buf = ctx->iov[i].iov_base;
		iov[i].len = ctx->iov[i].iov_len;
		iov[i].iova = spdk_vtophys(iov[i].buf);
	}

	submit_structure(iov, IOV_COUNT);
	ctx->io_done++;
	rte_mempool_put(g_iov_pool, iov);

	if (g_done) {
		spdk_poller_unregister(&ctx->poller);
		__sync_fetch_and_add(&g_io_done, ctx->io_done);
		free(ctx);
	}
}

static void
structure_link_poller(void *arg)
{
	struct benchmark_ctx *ctx = arg;
	struct benchmark_iov *iov;
	int i;

	rte_mempool_get(g_iov_pool, (void **)&iov);
	if (!iov) {
		SPDK_ERRLOG("could not allocate iov\n");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < IOV_COUNT; i++) {
		iov[i].buf = ctx->iov[i].iov_base;
		iov[i].len = ctx->iov[i].iov_len;
		iov[i].iova = spdk_vtophys(iov[i].buf);
		iov[i].next = &iov[i+1];
	}

	iov[IOV_COUNT - 1].next = NULL;
	submit_structure_link(iov);
	ctx->io_done++;
	rte_mempool_put(g_iov_pool, iov);

	if (g_done) {
		spdk_poller_unregister(&ctx->poller);
		__sync_fetch_and_add(&g_io_done, ctx->io_done);
		free(ctx);
	}
}

static void
start_poller(void *arg)
{
	struct benchmark_ctx *ctx;
	int i;

	ctx = calloc(1, sizeof(*ctx) + 4096);
	if (!ctx) {
		SPDK_ERRLOG("could not allocate ctx\n");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < IOV_COUNT; i++) {
		ctx->iov[i].iov_base = rte_malloc(NULL, 4096, 4096);
		ctx->iov[i].iov_len = 4096;
	}

	if (g_use_callback_api) {
		ctx->poller = spdk_poller_register(callback_poller, ctx, 0);
	} else if (g_calculate_iova) {
		ctx->poller = spdk_poller_register(callback_iova_poller, ctx, 0);
	} else if (g_use_structure_link_api) {
		ctx->poller = spdk_poller_register(structure_link_poller, ctx, 0);
	} else {
		ctx->poller = spdk_poller_register(structure_poller, ctx, 0);
	}
}

static void
start_poller_done(void *arg)
{
}

static void
stop_test2(void *arg)
{
	spdk_app_stop(0);
}

static void
stop_test(void *arg)
{
	g_done = true;
	spdk_poller_register(stop_test2, NULL, 1000);
}

static void
test_start(void *arg1, void *arg2)
{
	g_iov_pool = rte_mempool_create("iov", 2048, IOV_COUNT * sizeof(struct benchmark_iov),
					64, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
	spdk_for_each_thread(start_poller, NULL, start_poller_done);
	spdk_poller_register(stop_test, NULL, g_time_in_sec * 1000 * 1000);
}

static void
benchmark_usage(void)
{
	printf("\t[-A] (use callback api - no iova)\n");
	printf("\t[-I] (use callback api - with iova)\n");
	printf("\t[-L] (use structure api - link structures)\n");
	printf("\t[-t time in seconds]\n");
}

static void
benchmark_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'A':
		g_use_callback_api = true;
		break;
	case 'I':
		g_calculate_iova = true;
		break;
	case 'L':
		g_use_structure_link_api = true;
		break;
	case 'T':
		g_time_in_sec = atoi(arg);
		break;
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int rc = 0;

	spdk_app_opts_init(&opts);
	opts.name = "benchmark";

	g_time_in_sec = 0;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "AILT:", benchmark_parse_arg, benchmark_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, test_start, NULL, NULL);

	spdk_app_fini();
	printf("io_done = %ju\n", g_io_done);

	return rc;
}
