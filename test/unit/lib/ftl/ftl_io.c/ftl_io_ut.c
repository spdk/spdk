/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/ut_multithread.c"

#include "ftl/ftl_io.c"
#include "ftl/utils/ftl_conf.c"

DEFINE_STUB(spdk_bdev_io_get_append_location, uint64_t, (struct spdk_bdev_io *bdev_io), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_optimal_open_zones, uint32_t, (const struct spdk_bdev *b), 1);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_bdev_is_md_separate, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_is_zoned, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_zone_appendv, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_zone_size, uint64_t, (const struct spdk_bdev *b), 1024);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");
DEFINE_STUB(spdk_bdev_get_write_unit_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB(spdk_bdev_open_ext, int,
	    (const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
	     void *event_ctx, struct spdk_bdev_desc **desc), 0);
DEFINE_STUB(spdk_bdev_read_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_write_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_write_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, void *buf, void *md, uint64_t offset_blocks,
		uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 1024);
DEFINE_STUB(spdk_bdev_get_md_size, uint32_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 4096);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_mempool_create_ctor, struct spdk_mempool *,
	    (const char *name, size_t count, size_t ele_size, size_t cache_size,
	     int socket_id, spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg), NULL);
DEFINE_STUB(spdk_mempool_obj_iter, uint32_t,
	    (struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb, void *obj_cb_arg), 0);
DEFINE_STUB_V(ftl_reloc, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));
DEFINE_STUB_V(ftl_reloc_free, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_reloc_halt, (struct ftl_reloc *reloc));
DEFINE_STUB(ftl_reloc_init, struct ftl_reloc *, (struct spdk_ftl_dev *dev), NULL);
DEFINE_STUB(ftl_reloc_is_defrag_active, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB_V(ftl_reloc_resume, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_l2p_unpin, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count));
DEFINE_STUB(ftl_p2l_ckpt_acquire, struct ftl_p2l_ckpt *, (struct spdk_ftl_dev *dev), NULL);
DEFINE_STUB_V(ftl_p2l_ckpt_release, (struct spdk_ftl_dev *dev, struct ftl_p2l_ckpt *ckpt));
DEFINE_STUB(ftl_l2p_get, ftl_addr, (struct spdk_ftl_dev *dev, uint64_t lba), 0);
DEFINE_STUB_V(ftl_mempool_put, (struct ftl_mempool *mpool, void *element));

#if defined(DEBUG)
DEFINE_STUB_V(ftl_trace_defrag_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     ftl_addr addr, size_t addr_cnt));
DEFINE_STUB_V(ftl_trace_lba_io_init, (struct spdk_ftl_dev *dev, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_limits, (struct spdk_ftl_dev *dev, int limit, size_t num_free));
DEFINE_STUB(ftl_trace_alloc_id, uint64_t, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB_V(ftl_trace_completion, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     enum ftl_trace_completion type));
DEFINE_STUB_V(ftl_trace_wbuf_fill, (struct spdk_ftl_dev *dev, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_wbuf_pop, (struct spdk_ftl_dev *dev, const struct ftl_wbuf_entry *entry));
DEFINE_STUB_V(ftl_trace_write_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
#endif

#if defined(FTL_DUMP_STATS)
DEFINE_STUB_V(ftl_dev_dump_stats, (const struct spdk_ftl_dev *dev));
#endif

struct ftl_io_channel_ctx {
	struct ftl_io_channel *ioch;
};

struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct ftl_io_channel_ctx *ctx = spdk_io_channel_get_ctx(ioch);

	return ctx->ioch;
}

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
{
}

static struct spdk_ftl_dev *
setup_device(uint32_t num_threads, uint32_t xfer_size)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
	struct ftl_io_channel_ctx *ctx;

	allocate_threads(num_threads);
	set_thread(0);

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->core_thread = spdk_get_thread();
	dev->ioch = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE + sizeof(struct ftl_io_channel_ctx));
	SPDK_CU_ASSERT_FATAL(dev->ioch != NULL);

	ctx = spdk_io_channel_get_ctx(dev->ioch);
	ctx->ioch = calloc(1, sizeof(*ctx->ioch));

	ioch = ftl_io_channel_get_ctx(dev->ioch);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	ioch->cq = spdk_ring_create(0, 1024, 0);

	dev->conf = g_default_conf;
	dev->xfer_size = xfer_size;
	dev->base_bdev_desc = (struct spdk_bdev_desc *)0xdeadbeef;
	dev->nv_cache.bdev_desc = (struct spdk_bdev_desc *)0xdead1234;
	spdk_io_device_register(dev, channel_create_cb, channel_destroy_cb, 0, NULL);
	spdk_io_device_register(dev->base_bdev_desc, channel_create_cb, channel_destroy_cb, 0, NULL);
	spdk_io_device_register(dev->nv_cache.bdev_desc, channel_create_cb, channel_destroy_cb, 0, NULL);

	TAILQ_INIT(&dev->ioch_queue);

	return dev;
}

static void
free_device(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;

	ioch = ftl_io_channel_get_ctx(dev->ioch);
	spdk_ring_free(ioch->cq);
	free(ioch);

	spdk_io_device_unregister(dev, NULL);
	spdk_io_device_unregister(dev->base_bdev_desc, NULL);
	spdk_io_device_unregister(dev->nv_cache.bdev_desc, NULL);

	while (!TAILQ_EMPTY(&dev->ioch_queue)) {
		TAILQ_REMOVE(&dev->ioch_queue, TAILQ_FIRST(&dev->ioch_queue), entry);
	}

	free_threads();

	free(dev->ioch);
	free(dev->sb);
	free(dev);
}

static void
setup_io(struct ftl_io *io, struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *ctx)
{
	io->dev = dev;
	io->user_fn = cb;
	io->cb_ctx = ctx;
	io->flags = 0;
	io->ioch = dev->ioch;
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
	struct ftl_io io = { 0 }, *io_ring;
	int req, status = 0;

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
	ioch = ftl_io_channel_get_ctx(dev->ioch);

	/* Setup IO and 'send' NUM_REQUESTS subrequests */
	setup_io(&io, dev, io_complete_cb, &status);
	io.status = -EIO;

#define NUM_REQUESTS 16
	for (req = 0; req < NUM_REQUESTS; ++req) {
		ftl_io_inc_req(&io);
		CU_ASSERT_FALSE(ftl_io_done(&io));
	}

	CU_ASSERT_EQUAL(io.req_cnt, NUM_REQUESTS);

	/* Complete all but one subrequest, make sure io still not marked as done */
	for (req = 0; req < (NUM_REQUESTS - 1); ++req) {
		ftl_io_dec_req(&io);
		CU_ASSERT_FALSE(ftl_io_done(&io));
	}

	CU_ASSERT_EQUAL(io.req_cnt, 1);

	/* Complete last subrequest, make sure it appears on the completion queue */
	ftl_io_dec_req(&io);
	CU_ASSERT_TRUE(ftl_io_done(&io));

	ftl_io_complete(&io);

	CU_ASSERT_EQUAL(spdk_ring_count(ioch->cq), 1);

	/* Dequeue and check if the completion callback changes the status, this is usually done via poller */
	spdk_ring_dequeue(ioch->cq, (void **)&io_ring, 1);
	io_ring->user_fn(io_ring->cb_ctx, io_ring->status);

	CU_ASSERT_EQUAL(status, -EIO);

	free_device(dev);
}

static void
test_multiple_ios(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;
	struct ftl_io io[2] = { 0 }, *io_ring[2];
	int status = -1;

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
	ioch = ftl_io_channel_get_ctx(dev->ioch);

	/* Send t2o IOs and check if both are in the completion queue */
	setup_io(&io[0], dev, io_complete_cb, &status);
	CU_ASSERT_EQUAL(spdk_ring_count(ioch->cq), 0);

	ftl_io_complete(io);
	CU_ASSERT_EQUAL(spdk_ring_count(ioch->cq), 1);

	setup_io(&io[1], dev, io_complete_cb, &status);

	ftl_io_complete(&io[1]);
	CU_ASSERT_EQUAL(spdk_ring_count(ioch->cq), 2);

	/* Dequeue and check if the completion callback changes the status, this is usually done via poller */
	spdk_ring_dequeue(ioch->cq, (void **)io_ring, 2);
	status = -1;
	io_ring[0]->user_fn(io_ring[0]->cb_ctx, io_ring[0]->status);
	CU_ASSERT_EQUAL(status, 0);
	status = -1;
	io_ring[1]->user_fn(io_ring[1]->cb_ctx, io_ring[1]->status);
	CU_ASSERT_EQUAL(status, 0);

	free_device(dev);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_io_suite", NULL, NULL);


	CU_ADD_TEST(suite, test_completion);
	CU_ADD_TEST(suite, test_multiple_ios);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
