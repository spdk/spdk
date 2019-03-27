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

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_io.c"

DEFINE_STUB(ftl_trace_alloc_id, uint64_t, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB_V(ftl_band_acquire_md, (struct ftl_band *band));
DEFINE_STUB_V(ftl_band_release_md, (struct ftl_band *band));

static struct spdk_ftl_dev *
setup_device(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	dev->ioch = calloc(1, sizeof(*ioch) + sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(dev->ioch != NULL);

	ioch = spdk_io_channel_get_ctx(dev->ioch);

	ioch->elem_size = sizeof(struct ftl_md_io);
	ioch->io_pool = spdk_mempool_create("io-pool", 4096, ioch->elem_size, 0, 0);

	SPDK_CU_ASSERT_FATAL(ioch->io_pool != NULL);

	return dev;
}

static void
free_device(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;

	ioch = spdk_io_channel_get_ctx(dev->ioch);
	spdk_mempool_free(ioch->io_pool);

	free(dev->ioch);
	free(dev);
}

static void
setup_io(struct ftl_io *io, struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *ctx)
{
	io->dev = dev;
	io->cb.fn = cb;
	io->cb.ctx = ctx;
}

static struct ftl_io *
alloc_io(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *ctx)
{
	struct ftl_io *io;

	io = ftl_io_alloc(dev->ioch);
	SPDK_CU_ASSERT_FATAL(io != NULL);
	setup_io(io, dev, cb, ctx);

	return io;
}

static void
io_complete_cb(void *ctx, int status)
{
	*(int *)ctx = status;
}

static void
test_completion(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
	struct ftl_io *io;
	int req, status = 0;
	size_t pool_size;

	dev = setup_device();
	ioch = spdk_io_channel_get_ctx(dev->ioch);
	pool_size = spdk_mempool_count(ioch->io_pool);

	io = alloc_io(dev, io_complete_cb, &status);
	io->status = -EIO;

#define NUM_REQUESTS 16
	for (req = 0; req < NUM_REQUESTS; ++req) {
		ftl_io_inc_req(io);
		CU_ASSERT_FALSE(ftl_io_done(io));
	}

	CU_ASSERT_EQUAL(io->req_cnt, NUM_REQUESTS);

	for (req = 0; req < (NUM_REQUESTS - 1); ++req) {
		ftl_io_dec_req(io);
		CU_ASSERT_FALSE(ftl_io_done(io));
	}

	CU_ASSERT_EQUAL(io->req_cnt, 1);

	ftl_io_dec_req(io);
	CU_ASSERT_TRUE(ftl_io_done(io));

	ftl_io_complete(io);
	CU_ASSERT_EQUAL(status, -EIO);

	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	free_device(dev);
}

static void
test_child_requests(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
#define MAX_CHILDREN 16
	struct ftl_io *parent, *child[MAX_CHILDREN];
	int status[MAX_CHILDREN + 1], i;
	size_t pool_size;

	dev = setup_device();
	ioch = spdk_io_channel_get_ctx(dev->ioch);
	pool_size = spdk_mempool_count(ioch->io_pool);

	/* Verify correct behaviour when children finish first */
	parent = alloc_io(dev, io_complete_cb, &status[0]);
	parent->status = 0;

	ftl_io_inc_req(parent);
	status[0] = -1;

	for (i = 0; i < MAX_CHILDREN; ++i) {
		status[i + 1] = -1;

		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		SPDK_CU_ASSERT_FATAL(child[i]->flags & FTL_IO_KEEP_ALIVE);
		setup_io(child[i], dev, io_complete_cb, &status[i + 1]);
		child[i]->status = 0;

		ftl_io_inc_req(child[i]);
	}

	CU_ASSERT_FALSE(ftl_io_done(parent));
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size - MAX_CHILDREN - 1);

	for (i = 0; i < MAX_CHILDREN; ++i) {
		CU_ASSERT_FALSE(ftl_io_done(child[i]));
		ftl_io_dec_req(child[i]);
		CU_ASSERT_TRUE(ftl_io_done(child[i]));
		CU_ASSERT_FALSE(ftl_io_done(parent));

		ftl_io_complete(child[i]);
		CU_ASSERT_TRUE(ftl_io_done(child[i]));
		CU_ASSERT_FALSE(ftl_io_done(parent));
		CU_ASSERT_EQUAL(status[i + 1], 0);
	}

	CU_ASSERT_EQUAL(status[0], -1);

	ftl_io_dec_req(parent);
	CU_ASSERT_EQUAL(parent->req_cnt, 0);
	CU_ASSERT_TRUE(ftl_io_done(parent));

	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(status[0], 0);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);


	/* Verify correct behaviour when parent finishes first */
	parent = alloc_io(dev, io_complete_cb, &status[0]);
	parent->status = 0;

	ftl_io_inc_req(parent);
	status[0] = -1;

	for (i = 0; i < MAX_CHILDREN; ++i) {
		status[i + 1] = -1;

		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		SPDK_CU_ASSERT_FATAL(child[i]->flags & FTL_IO_KEEP_ALIVE);
		setup_io(child[i], dev, io_complete_cb, &status[i + 1]);
		child[i]->status = 0;

		ftl_io_inc_req(child[i]);
	}

	CU_ASSERT_FALSE(ftl_io_done(parent));
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size - MAX_CHILDREN - 1);

	ftl_io_dec_req(parent);
	CU_ASSERT_TRUE(ftl_io_done(parent));
	CU_ASSERT_EQUAL(parent->req_cnt, 0);

	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(status[0], -1);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size - MAX_CHILDREN - 1);

	for (i = 0; i < MAX_CHILDREN; ++i) {
		CU_ASSERT_FALSE(ftl_io_done(child[i]));
		ftl_io_dec_req(child[i]);
		CU_ASSERT_TRUE(ftl_io_done(child[i]));

		ftl_io_complete(child[i]);
		CU_ASSERT_EQUAL(status[i + 1], 0);
	}

	CU_ASSERT_EQUAL(status[0], 0);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	free_device(dev);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ftl_io_suite", NULL, NULL);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_completion",
			    test_completion) == NULL
		|| CU_add_test(suite, "test_child_requests",
			       test_child_requests) == NULL

	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
