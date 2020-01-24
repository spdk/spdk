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
#include "common/lib/ut_multithread.c"

#include "ftl/ftl_io.c"
#include "ftl/ftl_init.c"

DEFINE_STUB(ftl_trace_alloc_id, uint64_t, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB(spdk_bdev_io_get_append_location, uint64_t, (struct spdk_bdev_io *bdev_io), 0);
DEFINE_STUB_V(ftl_band_acquire_lba_map, (struct ftl_band *band));
DEFINE_STUB_V(ftl_band_release_lba_map, (struct ftl_band *band));
DEFINE_STUB(ftl_io_channel_poll, int, (void *ioch), 0);

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *bdev_desc)
{
	return spdk_get_io_channel(bdev_desc);
}

static int
channel_create_cb(void *io_device, void *ctx)
{
	return 0;
}

static void
channel_destroy_cb(void *io_device, void *ctx)
{}

static struct spdk_ftl_dev *
setup_device(uint32_t num_threads)
{
	struct spdk_ftl_dev *dev;
	struct _ftl_io_channel *_ioch;
	struct ftl_io_channel *ioch;
	int rc;

	allocate_threads(num_threads);
	set_thread(0);

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->core_thread = spdk_get_thread();
	dev->ioch = calloc(1, sizeof(*_ioch) + sizeof(struct spdk_io_channel));
	SPDK_CU_ASSERT_FATAL(dev->ioch != NULL);

	_ioch = (struct _ftl_io_channel *)(dev->ioch + 1);
	ioch = _ioch->ioch = calloc(1, sizeof(*ioch));
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	ioch->elem_size = sizeof(struct ftl_md_io);
	ioch->io_pool = spdk_mempool_create("io-pool", 4096, ioch->elem_size, 0, 0);

	SPDK_CU_ASSERT_FATAL(ioch->io_pool != NULL);

	dev->conf = g_default_conf;
	dev->base_bdev_desc = (struct spdk_bdev_desc *)0xdeadbeef;
	spdk_io_device_register(dev->base_bdev_desc, channel_create_cb, channel_destroy_cb, 0, NULL);

	rc = ftl_dev_init_io_channel(dev);
	CU_ASSERT_EQUAL(rc, 0);

	return dev;
}

static void
free_device(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;

	ioch = ftl_io_channel_get_ctx(dev->ioch);
	spdk_mempool_free(ioch->io_pool);
	free(ioch);

	spdk_io_device_unregister(dev, NULL);
	spdk_io_device_unregister(dev->base_bdev_desc, NULL);
	free_threads();

	free(dev->ioch_array);
	free(dev->ioch);
	free(dev);
}

static void
setup_io(struct ftl_io *io, struct spdk_ftl_dev *dev, ftl_io_fn cb, void *ctx)
{
	io->dev = dev;
	io->cb_fn = cb;
	io->cb_ctx = ctx;
}

static struct ftl_io *
alloc_io(struct spdk_ftl_dev *dev, ftl_io_fn cb, void *ctx)
{
	struct ftl_io *io;

	io = ftl_io_alloc(dev->ioch);
	SPDK_CU_ASSERT_FATAL(io != NULL);
	setup_io(io, dev, cb, ctx);

	return io;
}

static void
io_complete_cb(struct ftl_io *io, void *ctx, int status)
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

	dev = setup_device(1);
	ioch = ftl_io_channel_get_ctx(dev->ioch);
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
test_alloc_free(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
	struct ftl_io *parent, *child;
	int parent_status = -1;
	size_t pool_size;

	dev = setup_device(1);
	ioch = ftl_io_channel_get_ctx(dev->ioch);
	pool_size = spdk_mempool_count(ioch->io_pool);

	parent = alloc_io(dev, io_complete_cb, &parent_status);
	SPDK_CU_ASSERT_FATAL(parent != NULL);
	child = ftl_io_alloc_child(parent);
	SPDK_CU_ASSERT_FATAL(child != NULL);

	ftl_io_free(child);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size - 1);

	child = ftl_io_alloc_child(parent);
	SPDK_CU_ASSERT_FATAL(child != NULL);
	ftl_io_complete(child);
	CU_ASSERT_EQUAL(parent_status, -1);
	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(parent_status, 0);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	parent_status = -1;
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	SPDK_CU_ASSERT_FATAL(parent != NULL);
	child = ftl_io_alloc_child(parent);
	SPDK_CU_ASSERT_FATAL(child != NULL);

	ftl_io_free(child);
	CU_ASSERT_EQUAL(parent_status, -1);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size - 1);
	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(parent_status, 0);
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

	dev = setup_device(1);
	ioch = ftl_io_channel_get_ctx(dev->ioch);
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

static void
test_child_status(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
	struct ftl_io *parent, *child[2];
	int parent_status, child_status[2];
	size_t pool_size, i;

	dev = setup_device(1);
	ioch = ftl_io_channel_get_ctx(dev->ioch);
	pool_size = spdk_mempool_count(ioch->io_pool);

	/* Verify the first error is returned by the parent */
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	parent->status = 0;

	for (i = 0; i < 2; ++i) {
		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		setup_io(child[i], dev, io_complete_cb, &child_status[i]);
	}

	child[0]->status = -3;
	child[1]->status = -4;

	ftl_io_complete(child[1]);
	ftl_io_complete(child[0]);
	ftl_io_complete(parent);

	CU_ASSERT_EQUAL(child_status[0], -3);
	CU_ASSERT_EQUAL(child_status[1], -4);
	CU_ASSERT_EQUAL(parent_status, -4);

	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	/* Verify parent's status is kept if children finish successfully */
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	parent->status = -1;

	for (i = 0; i < 2; ++i) {
		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		setup_io(child[i], dev, io_complete_cb, &child_status[i]);
	}

	child[0]->status = 0;
	child[1]->status = 0;

	ftl_io_complete(parent);
	ftl_io_complete(child[1]);
	ftl_io_complete(child[0]);

	CU_ASSERT_EQUAL(child_status[0], 0);
	CU_ASSERT_EQUAL(child_status[1], 0);
	CU_ASSERT_EQUAL(parent_status, -1);

	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	/* Verify parent's status is kept if children fail too */
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	parent->status = -1;

	for (i = 0; i < 2; ++i) {
		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		setup_io(child[i], dev, io_complete_cb, &child_status[i]);
	}

	child[0]->status = -3;
	child[1]->status = -4;

	ftl_io_complete(parent);
	ftl_io_complete(child[1]);
	ftl_io_complete(child[0]);

	CU_ASSERT_EQUAL(child_status[0], -3);
	CU_ASSERT_EQUAL(child_status[1], -4);
	CU_ASSERT_EQUAL(parent_status, -1);

	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	free_device(dev);
}

static void
test_multi_generation(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
#define MAX_GRAND_CHILDREN	32
	struct ftl_io *parent, *child[MAX_CHILDREN], *gchild[MAX_CHILDREN * MAX_GRAND_CHILDREN];
	int parent_status, child_status[MAX_CHILDREN], gchild_status[MAX_CHILDREN * MAX_GRAND_CHILDREN];
	size_t pool_size;
	int i, j;

	dev = setup_device(1);
	ioch = ftl_io_channel_get_ctx(dev->ioch);
	pool_size = spdk_mempool_count(ioch->io_pool);

	/* Verify correct behaviour when children finish first */
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	parent->status = 0;

	ftl_io_inc_req(parent);
	parent_status = -1;

	for (i = 0; i < MAX_CHILDREN; ++i) {
		child_status[i] = -1;

		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		setup_io(child[i], dev, io_complete_cb, &child_status[i]);
		child[i]->status = 0;


		for (j = 0; j < MAX_GRAND_CHILDREN; ++j) {
			struct ftl_io *io = ftl_io_alloc_child(child[i]);
			SPDK_CU_ASSERT_FATAL(io != NULL);

			gchild[i * MAX_GRAND_CHILDREN + j] = io;
			gchild_status[i * MAX_GRAND_CHILDREN + j] = -1;
			setup_io(io, dev, io_complete_cb, &gchild_status[i * MAX_GRAND_CHILDREN + j]);
			io->status = 0;

			ftl_io_inc_req(io);
		}

		ftl_io_inc_req(child[i]);
	}

	for (i = 0; i < MAX_CHILDREN; ++i) {
		CU_ASSERT_FALSE(ftl_io_done(child[i]));
		ftl_io_dec_req(child[i]);
		CU_ASSERT_TRUE(ftl_io_done(child[i]));

		ftl_io_complete(child[i]);
		CU_ASSERT_FALSE(ftl_io_done(parent));
		CU_ASSERT_EQUAL(child_status[i], -1);

		for (j = 0; j < MAX_GRAND_CHILDREN; ++j) {
			struct ftl_io *io = gchild[i * MAX_GRAND_CHILDREN + j];

			CU_ASSERT_FALSE(ftl_io_done(io));
			ftl_io_dec_req(io);
			CU_ASSERT_TRUE(ftl_io_done(io));
			ftl_io_complete(io);
			CU_ASSERT_EQUAL(gchild_status[i * MAX_GRAND_CHILDREN + j], 0);
		}

		CU_ASSERT_EQUAL(child_status[i], 0);
	}

	ftl_io_dec_req(parent);
	CU_ASSERT_TRUE(ftl_io_done(parent));
	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(parent_status, 0);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	/* Verify correct behaviour when parents finish first */
	parent = alloc_io(dev, io_complete_cb, &parent_status);
	parent->status = 0;
	parent_status = -1;

	for (i = 0; i < MAX_CHILDREN; ++i) {
		child_status[i] = -1;

		child[i] = ftl_io_alloc_child(parent);
		SPDK_CU_ASSERT_FATAL(child[i] != NULL);
		setup_io(child[i], dev, io_complete_cb, &child_status[i]);
		child[i]->status = 0;

		for (j = 0; j < MAX_GRAND_CHILDREN; ++j) {
			struct ftl_io *io = ftl_io_alloc_child(child[i]);
			SPDK_CU_ASSERT_FATAL(io != NULL);

			gchild[i * MAX_GRAND_CHILDREN + j] = io;
			gchild_status[i * MAX_GRAND_CHILDREN + j] = -1;
			setup_io(io, dev, io_complete_cb, &gchild_status[i * MAX_GRAND_CHILDREN + j]);
			io->status = 0;

			ftl_io_inc_req(io);
		}

		CU_ASSERT_TRUE(ftl_io_done(child[i]));
		ftl_io_complete(child[i]);
		CU_ASSERT_EQUAL(child_status[i], -1);
	}

	CU_ASSERT_TRUE(ftl_io_done(parent));
	ftl_io_complete(parent);
	CU_ASSERT_EQUAL(parent_status, -1);

	for (i = 0; i < MAX_CHILDREN; ++i) {
		for (j = 0; j < MAX_GRAND_CHILDREN; ++j) {
			struct ftl_io *io = gchild[i * MAX_GRAND_CHILDREN + j];

			CU_ASSERT_FALSE(ftl_io_done(io));
			ftl_io_dec_req(io);
			CU_ASSERT_TRUE(ftl_io_done(io));
			ftl_io_complete(io);
			CU_ASSERT_EQUAL(gchild_status[i * MAX_GRAND_CHILDREN + j], 0);
		}

		CU_ASSERT_EQUAL(child_status[i], 0);
	}

	CU_ASSERT_EQUAL(parent_status, 0);
	CU_ASSERT_EQUAL(spdk_mempool_count(ioch->io_pool), pool_size);

	free_device(dev);
}

static void
test_io_channel_create(void)
{
	struct spdk_ftl_dev *dev;
	struct spdk_io_channel *ioch, **ioch_array;
	struct ftl_io_channel *ftl_ioch;
	uint32_t ioch_idx;

	dev = setup_device(g_default_conf.max_io_channels + 1);

	ioch = spdk_get_io_channel(dev);
	CU_ASSERT(ioch != NULL);
	CU_ASSERT_EQUAL(dev->num_io_channels, 1);
	spdk_put_io_channel(ioch);
	poll_threads();
	CU_ASSERT_EQUAL(dev->num_io_channels, 0);

	ioch_array = calloc(dev->conf.max_io_channels, sizeof(*ioch_array));
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	for (ioch_idx = 0; ioch_idx < dev->conf.max_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ioch = ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(ioch != NULL);
		poll_threads();

		ftl_ioch = ftl_io_channel_get_ctx(ioch);
		CU_ASSERT_EQUAL(ftl_ioch->index, ioch_idx);
	}

	CU_ASSERT_EQUAL(dev->num_io_channels, dev->conf.max_io_channels);
	set_thread(dev->conf.max_io_channels);
	ioch = spdk_get_io_channel(dev);
	CU_ASSERT_EQUAL(dev->num_io_channels, dev->conf.max_io_channels);
	CU_ASSERT_EQUAL(ioch, NULL);

	for (ioch_idx = 0; ioch_idx < dev->conf.max_io_channels; ioch_idx += 2) {
		set_thread(ioch_idx);
		spdk_put_io_channel(ioch_array[ioch_idx]);
		ioch_array[ioch_idx] = NULL;
		poll_threads();
	}

	poll_threads();
	CU_ASSERT_EQUAL(dev->num_io_channels, dev->conf.max_io_channels / 2);

	for (ioch_idx = 0; ioch_idx < dev->conf.max_io_channels; ioch_idx++) {
		set_thread(ioch_idx);

		if (ioch_array[ioch_idx] == NULL) {
			ioch = ioch_array[ioch_idx] = spdk_get_io_channel(dev);
			SPDK_CU_ASSERT_FATAL(ioch != NULL);
			poll_threads();

			ftl_ioch = ftl_io_channel_get_ctx(ioch);
			CU_ASSERT_EQUAL(ftl_ioch->index, ioch_idx);
		}
	}

	for (ioch_idx = 0; ioch_idx < dev->conf.max_io_channels; ioch_idx++) {
		set_thread(ioch_idx);
		spdk_put_io_channel(ioch_array[ioch_idx]);
	}

	poll_threads();
	CU_ASSERT_EQUAL(dev->num_io_channels, 0);

	free(ioch_array);
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
		|| CU_add_test(suite, "test_alloc_free",
			       test_alloc_free) == NULL
		|| CU_add_test(suite, "test_child_requests",
			       test_child_requests) == NULL
		|| CU_add_test(suite, "test_child_status",
			       test_child_status) == NULL
		|| CU_add_test(suite, "test_multi_generation",
			       test_multi_generation) == NULL
		|| CU_add_test(suite, "test_io_channel_create",
			       test_io_channel_create) == NULL

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
