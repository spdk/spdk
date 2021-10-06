/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of Nvidia Corporation nor the names of its
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

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/dma.h"
#include <infiniband/verbs.h>

#define DMA_TEST_IO_BUFFER_SIZE 4096

static char *g_bdev_name;

static int
parse_arg(int ch, char *arg)
{
	if (ch == 'b') {
		g_bdev_name = optarg;
	} else {
		fprintf(stderr, "Unknown option %c\n", ch);
		return 1;
	}
	return 0;
}

static void
print_usage(void)
{
	printf(" -b <bdev>                bdev name for test\n");
}

static void
dma_test_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

struct dma_test_ctx {
	const char *bdev_name;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_memory_domain *memory_domain;
	void *write_io_buffer;
	void *read_io_buffer;
	struct spdk_bdev_ext_io_opts ext_io_opts;
	struct ibv_mr *mr;
	uint64_t num_blocks;
};

static int
dma_test_translate_memory_cb(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
			     void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	struct dma_test_ctx *ctx = src_domain_ctx;
	struct ibv_qp *dst_domain_qp = (struct ibv_qp *)dst_domain_ctx->rdma.ibv_qp;

	fprintf(stdout, "Translating memory\n");

	ctx->mr = ibv_reg_mr(dst_domain_qp->pd, addr, len, IBV_ACCESS_LOCAL_WRITE |
			     IBV_ACCESS_REMOTE_READ |
			     IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Failed to register memory region, errno %d\n", errno);
		return -1;
	}

	result->iov.iov_base = addr;
	result->iov.iov_len = len;
	result->iov_count = 1;
	result->rdma.lkey = ctx->mr->lkey;
	result->rdma.rkey = ctx->mr->rkey;
	result->dst_domain = dst_domain;

	return 0;
}

static void
dma_test_cleanup(struct dma_test_ctx *ctx)
{
	if (ctx->ch) {
		spdk_put_io_channel(ctx->ch);
		ctx->ch = NULL;
	}
	if (ctx->desc) {
		spdk_bdev_close(ctx->desc);
		ctx->desc = NULL;
	}
	spdk_memory_domain_destroy(ctx->memory_domain);
	ctx->memory_domain = NULL;
	if (ctx->mr) {
		ibv_dereg_mr(ctx->mr);
		ctx->mr = NULL;
	}
	free(ctx->write_io_buffer);
	ctx->write_io_buffer = NULL;
	free(ctx->read_io_buffer);
	ctx->read_io_buffer = NULL;
}

static void
dma_test_read_completed(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct dma_test_ctx *ctx = cb_arg;
	int sct, sc;
	uint32_t cdw0;

	if (success) {
		spdk_bdev_free_io(bdev_io);
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		fprintf(stderr, "bdev read IO failed, cdw0 %x, sct %d, sc %d\n", cdw0, sct, sc);
		spdk_app_stop(-1);
		return;
	}

	if (memcmp(ctx->write_io_buffer, ctx->read_io_buffer, DMA_TEST_IO_BUFFER_SIZE)) {
		fprintf(stderr, "Read buffer doesn't match written data!\n");
		spdk_app_stop(-1);
		return;
	}

	fprintf(stdout, "DMA test completed successfully\n");

	dma_test_cleanup(ctx);

	spdk_app_stop(0);
}

static void
dma_test_write_completed(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct dma_test_ctx *ctx = cb_arg;
	struct iovec iov;
	int sct, sc, rc;
	uint32_t cdw0;

	if (success) {
		spdk_bdev_free_io(bdev_io);
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		fprintf(stderr, "bdev write IO failed, cdw0 %x, sct %d, sc %d\n", cdw0, sct, sc);
		spdk_app_stop(-1);
		return;
	}

	fprintf(stdout, "Write IO completed, submitting read IO\n");

	ibv_dereg_mr(ctx->mr);

	iov.iov_base = ctx->read_io_buffer;
	iov.iov_len = DMA_TEST_IO_BUFFER_SIZE;

	rc = spdk_bdev_readv_blocks_ext(ctx->desc, ctx->ch, &iov, 1, 0, ctx->num_blocks,
					dma_test_read_completed, ctx, &ctx->ext_io_opts);
	if (rc) {
		fprintf(stderr, "Falied to submit read operation");
		spdk_app_stop(-1);
	}
}

static bool
dma_test_check_bdev_supports_rdma_memory_domain(struct dma_test_ctx *ctx)
{
	struct spdk_memory_domain **bdev_domains;
	int bdev_domains_count, bdev_domains_count_tmp, i;
	bool rdma_domain_supported = false;

	bdev_domains_count = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(ctx->desc), NULL, 0);

	if (bdev_domains_count < 0) {
		fprintf(stderr, "Failed to get bdev memory domains count, rc %d\n", bdev_domains_count);
		return false;
	} else if (bdev_domains_count == 0) {
		fprintf(stderr, "bdev %s doesn't support any memory domains\n", ctx->bdev_name);
		return false;
	}

	fprintf(stdout, "bdev %s reports %d memory domains\n", ctx->bdev_name, bdev_domains_count);

	bdev_domains = calloc((size_t)bdev_domains_count, sizeof(*bdev_domains));
	if (!bdev_domains) {
		fprintf(stderr, "Failed to allocate memory domains\n");
		return false;
	}

	bdev_domains_count_tmp = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(ctx->desc),
				 bdev_domains, bdev_domains_count);
	if (bdev_domains_count_tmp != bdev_domains_count) {
		fprintf(stderr, "Unexpected bdev domains return value %d\n", bdev_domains_count_tmp);
		return false;
	}

	for (i = 0; i < bdev_domains_count; i++) {
		if (spdk_memory_domain_get_dma_device_type(bdev_domains[i]) == SPDK_DMA_DEVICE_TYPE_RDMA) {
			/* Bdev supports memory domain of RDMA type, we can try to submit IO request to it using
			 * bdev ext API */
			rdma_domain_supported = true;
			break;
		}
	}

	fprintf(stdout, "bdev %s %s RDMA memory domain\n", ctx->bdev_name,
		rdma_domain_supported ? "supports" : "doesn't support");
	free(bdev_domains);

	return rdma_domain_supported;
}

static void
dma_test_run(void *arg)
{
	struct dma_test_ctx *ctx = arg;

	struct iovec iov;
	int rc;

	/* Test scenario:
	 * 1. Open bdev, check that it supports RDMA memory domain
	 * 2. Allocate IO buffer using regular malloc. In that case SPDK NVME_RDMA driver won't create a
	 * memory region for this IO and won't be able to find memory keys
	 * 3. Create dma memory domain which translation callback creates a memory region and
	 * returns memory keys to NVME RDMA driver
	 * 4. Do the same for read operation, compare buffers when done */

	/* Prepare bdev */
	rc = spdk_bdev_open_ext(ctx->bdev_name, true, dma_test_bdev_event_cb, NULL, &ctx->desc);
	if (rc) {
		fprintf(stderr, "Failed to open bdev %s\n", ctx->bdev_name);
		spdk_app_stop(-1);
		return;
	}

	ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
	if (!ctx->ch) {
		fprintf(stderr, "Failed to get io chanel for bdev %s\n", ctx->bdev_name);
		spdk_bdev_close(ctx->desc);
		spdk_app_stop(-1);
		return;
	}

	if (!dma_test_check_bdev_supports_rdma_memory_domain(ctx)) {
		spdk_bdev_close(ctx->desc);
		spdk_app_stop(-1);
		return;
	}

	ctx->num_blocks = DMA_TEST_IO_BUFFER_SIZE / spdk_bdev_get_block_size(spdk_bdev_desc_get_bdev(
				  ctx->desc));

	/* Create a memory domain to represent the source memory domain.
	 * Since we don't actually have a remote memory domain in this test, this will describe memory
	 * on the local system and the translation to the destination memory domain will be trivial.
	 * But this at least allows us to demonstrate the flow and test the functionality. */
	rc = spdk_memory_domain_create(&ctx->memory_domain, SPDK_DMA_DEVICE_TYPE_RDMA, NULL, "test_dma");
	if (rc) {
		fprintf(stderr, "Can't create memory domain, rc %d\n", rc);
		spdk_app_stop(-1);
		return;
	}

	spdk_memory_domain_set_translation(ctx->memory_domain, dma_test_translate_memory_cb);

	ctx->write_io_buffer = malloc(DMA_TEST_IO_BUFFER_SIZE);
	if (!ctx->write_io_buffer) {
		fprintf(stderr, "IO buffer allocation failed");
		spdk_app_stop(-1);;
		return;
	}
	memset(ctx->write_io_buffer, 0xd, DMA_TEST_IO_BUFFER_SIZE);

	ctx->read_io_buffer = malloc(DMA_TEST_IO_BUFFER_SIZE);
	if (!ctx->read_io_buffer) {
		fprintf(stderr, "IO buffer allocation failed");
		spdk_app_stop(-1);;
		return;
	}

	ctx->ext_io_opts.memory_domain = ctx->memory_domain;
	ctx->ext_io_opts.memory_domain_ctx = ctx;
	iov.iov_base = ctx->write_io_buffer;
	iov.iov_len = DMA_TEST_IO_BUFFER_SIZE;

	fprintf(stdout, "Submitting write IO\n");

	rc = spdk_bdev_writev_blocks_ext(ctx->desc, ctx->ch, &iov, 1, 0, ctx->num_blocks,
					 dma_test_write_completed, ctx, &ctx->ext_io_opts);
	if (rc) {
		fprintf(stderr, "Falied to submit write operation");
		spdk_app_stop(-1);
	}
}

int
main(int argc, char **argv)
{
	struct dma_test_ctx ctx = {};
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "test_dma";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL, parse_arg, print_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (!g_bdev_name) {
		fprintf(stderr, "bdev name for test is not set\n");
		exit(1);
	}
	ctx.bdev_name = g_bdev_name;

	rc = spdk_app_start(&opts, dma_test_run, &ctx);

	dma_test_cleanup(&ctx);

	spdk_app_fini();

	return rc;
}
