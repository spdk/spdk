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
#include "ftl/ftl_core.c"
#include "ftl/ftl_band.c"

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
DEFINE_STUB(spdk_bdev_zone_management, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, uint64_t zone_id, enum spdk_bdev_zone_action action,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");
DEFINE_STUB(spdk_bdev_get_write_unit_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);
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
DEFINE_STUB(spdk_bdev_get_media_events, size_t,
	    (struct spdk_bdev_desc *bdev_desc, struct spdk_bdev_media_event *events,
	     size_t max_events), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_zone_info, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
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

#if defined(FTL_META_DEBUG)
DEFINE_STUB_V(ftl_band_validate_md, (struct ftl_band *band, ftl_band_validate_md_cb cb));
#endif
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
#if defined(FTL_META_DEBUG)
DEFINE_STUB_V(ftl_dev_dump_bands, (struct spdk_ftl_dev *dev));
#endif
#if defined(FTL_DUMP_STATS)
DEFINE_STUB_V(ftl_dev_dump_stats, (const struct spdk_ftl_dev *dev));
#endif

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB(pmem_map_file, void *,
	    (const char *path, size_t len, int flags, mode_t mode,
	     size_t *mapped_lenp, int *is_pmemp), NULL);
DEFINE_STUB(pmem_unmap, int, (void *addr, size_t len), 0);
DEFINE_STUB(pmem_memset_persist, void *, (void *pmemdest, int c, size_t len), NULL);
#endif

struct ftl_io_channel_cntx {
	struct ftl_io_channel *ioch;
};

struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct ftl_io_channel_cntx *cntx = spdk_io_channel_get_ctx(ioch);
	return cntx->ioch;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *bdev_desc)
{
	return spdk_get_io_channel(bdev_desc);
}

static int
channel_create_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	dev->num_io_channels++;
	return 0;
}

static void
channel_destroy_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	dev->num_io_channels--;
}

static struct spdk_ftl_dev *
setup_device(uint32_t num_threads, uint32_t xfer_size)
{
	struct spdk_ftl_dev *dev;
	struct ftl_io_channel *ioch;

	allocate_threads(num_threads);
	set_thread(0);

	dev = calloc(1, sizeof(*dev));
	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->core_thread = spdk_get_thread();
	dev->ioch = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE + sizeof(struct ftl_io_channel_cntx));
	SPDK_CU_ASSERT_FATAL(dev->ioch != NULL);

	struct ftl_io_channel_cntx *cntx = spdk_io_channel_get_ctx(dev->ioch);
	cntx->ioch = calloc(1, sizeof(*cntx->ioch));

	ioch = ftl_io_channel_get_ctx(dev->ioch);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	ioch->io_pool_elem_size = sizeof(struct ftl_io);
	ioch->io_pool = spdk_mempool_create("io-pool", 4096, ioch->io_pool_elem_size, 0, 0);

	SPDK_CU_ASSERT_FATAL(ioch->io_pool != NULL);

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
	spdk_mempool_free(ioch->io_pool);
	free(ioch);

	spdk_io_device_unregister(dev, NULL);
	spdk_io_device_unregister(dev->base_bdev_desc, NULL);
	spdk_io_device_unregister(dev->nv_cache.bdev_desc, NULL);

	while (dev->ioch_queue.tqh_first != NULL) {
		TAILQ_REMOVE(&dev->ioch_queue, dev->ioch_queue.tqh_first, entry);
	}

	free_threads();

	free(dev->ioch);
	free(dev->sb);
	free(dev);
}

static void
setup_io(struct ftl_io *io, struct spdk_ftl_dev *dev, ftl_io_fn cb, void *ctx)
{
	io->dev = dev;
	io->cb_fn = cb;
	io->cb_ctx = ctx;
	io->flags = FTL_IO_INTERNAL;
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

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
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

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
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

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
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

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
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
	CU_ASSERT_EQUAL(parent_status, -3);

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
	CU_ASSERT_EQUAL(parent_status, -3);

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

	dev = setup_device(1, FTL_NUM_LBA_IN_BLOCK);
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


int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_io_suite", NULL, NULL);


	CU_ADD_TEST(suite, test_completion);
	CU_ADD_TEST(suite, test_alloc_free);
	CU_ADD_TEST(suite, test_child_requests);
	CU_ADD_TEST(suite, test_child_status);
	CU_ADD_TEST(suite, test_multi_generation);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
