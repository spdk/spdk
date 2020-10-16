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

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/util.h"

#define MAX_DEVS 64

#define MAX_IOVS 128

#define DATA_PATTERN 0x5A

#define BASE_LBA_START 0x100000

struct dev {
	struct spdk_nvme_ctrlr			*ctrlr;
	char					name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int io_complete_flag = 0;

struct sgl_element {
	void *base;
	size_t offset;
	size_t len;
};

struct io_request {
	uint32_t current_iov_index;
	uint32_t current_iov_bytes_left;
	struct sgl_element iovs[MAX_IOVS];
	uint32_t nseg;
	uint32_t misalign;
};

static void nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	uint32_t i;
	uint32_t offset = 0;
	struct sgl_element *iov;
	struct io_request *req = (struct io_request *)cb_arg;

	for (i = 0; i < req->nseg; i++) {
		iov = &req->iovs[i];
		offset += iov->len;
		if (offset > sgl_offset) {
			break;
		}
	}
	req->current_iov_index = i;
	req->current_iov_bytes_left = offset - sgl_offset;
	return;
}

static int nvme_request_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	struct sgl_element *iov;

	if (req->current_iov_index >= req->nseg) {
		*length = 0;
		*address = NULL;
		return 0;
	}

	iov = &req->iovs[req->current_iov_index];

	if (req->current_iov_bytes_left) {
		*address = iov->base + iov->offset + iov->len - req->current_iov_bytes_left;
		*length = req->current_iov_bytes_left;
		req->current_iov_bytes_left = 0;
	} else {
		*address = iov->base + iov->offset;
		*length = iov->len;
	}

	req->current_iov_index++;

	return 0;
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		io_complete_flag = 2;
	} else {
		io_complete_flag = 1;
	}
}

static void build_io_request_0(struct io_request *req)
{
	req->nseg = 1;

	req->iovs[0].base = spdk_zmalloc(0x800, 4, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x800;
}

static void build_io_request_1(struct io_request *req)
{
	req->nseg = 1;

	/* 512B for 1st sge */
	req->iovs[0].base = spdk_zmalloc(0x200, 0x200, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x200;
}

static void build_io_request_2(struct io_request *req)
{
	req->nseg = 1;

	/* 256KB for 1st sge */
	req->iovs[0].base = spdk_zmalloc(0x40000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x40000;
}

static void build_io_request_3(struct io_request *req)
{
	req->nseg = 3;

	/* 2KB for 1st sge, make sure the iov address start at 0x800 boundary,
	 *  and end with 0x1000 boundary */
	req->iovs[0].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].offset = 0x800;
	req->iovs[0].len = 0x800;

	/* 4KB for 2th sge */
	req->iovs[1].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[1].len = 0x1000;

	/* 12KB for 3th sge */
	req->iovs[2].base = spdk_zmalloc(0x3000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[2].len = 0x3000;
}

static void build_io_request_4(struct io_request *req)
{
	uint32_t i;

	req->nseg = 32;

	/* 4KB for 1st sge */
	req->iovs[0].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x1000;

	/* 8KB for the rest 31 sge */
	for (i = 1; i < req->nseg; i++) {
		req->iovs[i].base = spdk_zmalloc(0x2000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		req->iovs[i].len = 0x2000;
	}
}

static void build_io_request_5(struct io_request *req)
{
	req->nseg = 1;

	/* 8KB for 1st sge */
	req->iovs[0].base = spdk_zmalloc(0x2000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x2000;
}

static void build_io_request_6(struct io_request *req)
{
	req->nseg = 2;

	/* 4KB for 1st sge */
	req->iovs[0].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].len = 0x1000;

	/* 4KB for 2st sge */
	req->iovs[1].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[1].len = 0x1000;
}

static void build_io_request_7(struct io_request *req)
{
	uint8_t *base;

	req->nseg = 1;

	/*
	 * Create a 64KB sge, but ensure it is *not* aligned on a 4KB
	 *  boundary.  This is valid for single element buffers with PRP.
	 */
	base = spdk_zmalloc(0x11000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->misalign = 64;
	req->iovs[0].base = base + req->misalign;
	req->iovs[0].len = 0x10000;
}

static void build_io_request_8(struct io_request *req)
{
	req->nseg = 2;

	/*
	 * 1KB for 1st sge, make sure the iov address does not start and end
	 * at 0x1000 boundary
	 */
	req->iovs[0].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[0].offset = 0x400;
	req->iovs[0].len = 0x400;

	/*
	 * 1KB for 1st sge, make sure the iov address does not start and end
	 * at 0x1000 boundary
	 */
	req->iovs[1].base = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	req->iovs[1].offset = 0x400;
	req->iovs[1].len = 0x400;
}

static void build_io_request_9(struct io_request *req)
{
	/*
	 * Check if mixed PRP complaint and not complaint requests are handled
	 * properly by splitting them into subrequests.
	 * Construct buffers with following theme:
	 */
	const size_t req_len[] = {  2048, 4096, 2048,  4096,  2048,  1024 };
	const size_t req_off[] = { 0x800,  0x0,  0x0, 0x100, 0x800, 0x800 };
	struct sgl_element *iovs = req->iovs;
	uint32_t i;
	req->nseg = SPDK_COUNTOF(req_len);
	assert(SPDK_COUNTOF(req_len) == SPDK_COUNTOF(req_off));

	for (i = 0; i < req->nseg; i++) {
		iovs[i].base = spdk_zmalloc(req_off[i] + req_len[i], 0x4000, NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		iovs[i].offset = req_off[i];
		iovs[i].len = req_len[i];
	}
}

static void build_io_request_10(struct io_request *req)
{
	/*
	 * Test the case where we have a valid PRP list, but the first and last
	 * elements are not exact multiples of the logical block size.
	 */
	const size_t req_len[] = {  4004, 4096,  92 };
	const size_t req_off[] = {  0x5c,  0x0, 0x0 };
	struct sgl_element *iovs = req->iovs;
	uint32_t i;
	req->nseg = SPDK_COUNTOF(req_len);
	assert(SPDK_COUNTOF(req_len) == SPDK_COUNTOF(req_off));

	for (i = 0; i < req->nseg; i++) {
		iovs[i].base = spdk_zmalloc(req_off[i] + req_len[i], 0x4000, NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		iovs[i].offset = req_off[i];
		iovs[i].len = req_len[i];
	}
}

static void build_io_request_11(struct io_request *req)
{
	/* This test case focuses on the last element not starting on a page boundary. */
	const size_t req_len[] = { 512, 512 };
	const size_t req_off[] = { 0xe00, 0x800 };
	struct sgl_element *iovs = req->iovs;
	uint32_t i;
	req->nseg = SPDK_COUNTOF(req_len);
	assert(SPDK_COUNTOF(req_len) == SPDK_COUNTOF(req_off));

	for (i = 0; i < req->nseg; i++) {
		iovs[i].base = spdk_zmalloc(req_off[i] + req_len[i], 0x4000, NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		iovs[i].offset = req_off[i];
		iovs[i].len = req_len[i];
	}
}

typedef void (*nvme_build_io_req_fn_t)(struct io_request *req);

static void
free_req(struct io_request *req)
{
	uint32_t i;

	if (req == NULL) {
		return;
	}

	for (i = 0; i < req->nseg; i++) {
		spdk_free(req->iovs[i].base - req->misalign);
	}

	spdk_free(req);
}

static int
writev_readv_tests(struct dev *dev, nvme_build_io_req_fn_t build_io_fn, const char *test_name)
{
	int rc = 0;
	uint32_t len, lba_count;
	uint32_t i, j, nseg, remainder;
	char *buf;

	struct io_request *req;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	const struct spdk_nvme_ns_data *nsdata;

	ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, 1);
	if (!ns) {
		fprintf(stderr, "Null namespace\n");
		return 0;
	}
	nsdata = spdk_nvme_ns_get_data(ns);
	if (!nsdata || !spdk_nvme_ns_get_sector_size(ns)) {
		fprintf(stderr, "Empty nsdata or wrong sector size\n");
		return 0;
	}

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		return 0;
	}

	req = spdk_zmalloc(sizeof(*req), 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!req) {
		fprintf(stderr, "Allocate request failed\n");
		return 0;
	}

	/* IO parameters setting */
	build_io_fn(req);

	len = 0;
	for (i = 0; i < req->nseg; i++) {
		struct sgl_element *sge = &req->iovs[i];

		len += sge->len;
	}

	lba_count = len / spdk_nvme_ns_get_sector_size(ns);
	remainder = len % spdk_nvme_ns_get_sector_size(ns);
	if (!lba_count || remainder || (BASE_LBA_START + lba_count > (uint32_t)nsdata->nsze)) {
		fprintf(stderr, "%s: %s Invalid IO length parameter\n", dev->name, test_name);
		free_req(req);
		return 0;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, NULL, 0);
	if (!qpair) {
		free_req(req);
		return -1;
	}

	nseg = req->nseg;
	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].base + req->iovs[i].offset, DATA_PATTERN, req->iovs[i].len);
	}

	rc = spdk_nvme_ns_cmd_writev(ns, qpair, BASE_LBA_START, lba_count,
				     io_complete, req, 0,
				     nvme_request_reset_sgl,
				     nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "%s: %s writev failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	io_complete_flag = 0;

	while (!io_complete_flag) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s writev failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	/* reset completion flag */
	io_complete_flag = 0;

	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].base + req->iovs[i].offset, 0, req->iovs[i].len);
	}

	rc = spdk_nvme_ns_cmd_readv(ns, qpair, BASE_LBA_START, lba_count,
				    io_complete, req, 0,
				    nvme_request_reset_sgl,
				    nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "%s: %s readv failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	while (!io_complete_flag) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s readv failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	for (i = 0; i < nseg; i++) {
		buf = (char *)req->iovs[i].base + req->iovs[i].offset;
		for (j = 0; j < req->iovs[i].len; j++) {
			if (buf[j] != DATA_PATTERN) {
				fprintf(stderr, "%s: %s write/read success, but memcmp Failed\n", dev->name, test_name);
				spdk_nvme_ctrlr_free_io_qpair(qpair);
				free_req(req);
				return -1;
			}
		}
	}

	fprintf(stdout, "%s: %s test passed\n", dev->name, test_name);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
	free_req(req);
	return rc;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

int main(int argc, char **argv)
{
	struct dev		*iter;
	int			rc;
	struct spdk_env_opts	opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	opts.name = "nvme_sgl";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("NVMe Readv/Writev Request test\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "nvme_probe() failed\n");
		exit(1);
	}

	rc = 0;
	foreach_dev(iter) {
#define TEST(x) writev_readv_tests(iter, x, #x)
		if (TEST(build_io_request_0)
		    || TEST(build_io_request_1)
		    || TEST(build_io_request_2)
		    || TEST(build_io_request_3)
		    || TEST(build_io_request_4)
		    || TEST(build_io_request_5)
		    || TEST(build_io_request_6)
		    || TEST(build_io_request_7)
		    || TEST(build_io_request_8)
		    || TEST(build_io_request_9)
		    || TEST(build_io_request_10)
		    || TEST(build_io_request_11)) {
#undef TEST
			rc = 1;
			printf("%s: failed sgl tests\n", iter->name);
		}
	}

	printf("Cleaning up...\n");

	foreach_dev(iter) {
		spdk_nvme_detach_async(iter->ctrlr, &detach_ctx);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

	return rc;
}
