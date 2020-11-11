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
DEFINE_STUB(ftl_reloc, bool, (struct ftl_reloc *reloc), false);
DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));
DEFINE_STUB_V(ftl_reloc_free, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_reloc_halt, (struct ftl_reloc *reloc));
DEFINE_STUB(ftl_reloc_init, struct ftl_reloc *, (struct spdk_ftl_dev *dev), NULL);
DEFINE_STUB(ftl_reloc_is_defrag_active, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB_V(ftl_reloc_resume, (struct ftl_reloc *reloc));
DEFINE_STUB(ftl_restore_device, int,
	    (struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg), 0);
DEFINE_STUB(ftl_restore_md, int,
	    (struct spdk_ftl_dev *dev, ftl_restore_fn cb, void *cb_arg), 0);
DEFINE_STUB_V(ftl_restore_nv_cache,
	      (struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg));

#if defined(FTL_META_DEBUG)
DEFINE_STUB(ftl_band_validate_md, bool, (struct ftl_band *band), true);
#endif
#if defined(DEBUG)
DEFINE_STUB_V(ftl_trace_defrag_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     struct ftl_addr addr, size_t addr_cnt));
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
setup_device(uint32_t num_threads, uint32_t xfer_size)
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
	dev->xfer_size = xfer_size;
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
	free(dev->iov_buf);
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

	dev = setup_device(1, 16);
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

	dev = setup_device(1, 16);
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

	dev = setup_device(1, 16);
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

	dev = setup_device(1, 16);
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

	dev = setup_device(1, 16);
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

	dev = setup_device(g_default_conf.max_io_channels + 1, 16);

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

static void
test_acquire_entry(void)
{
	struct spdk_ftl_dev *dev;
	struct spdk_io_channel *ioch, **ioch_array;
	struct ftl_io_channel *ftl_ioch;
	struct ftl_wbuf_entry *entry, **entries;
	uint32_t num_entries, num_io_channels = 2;
	uint32_t ioch_idx, entry_idx, tmp_idx;

	dev = setup_device(num_io_channels, 16);

	num_entries = dev->conf.write_buffer_size / FTL_BLOCK_SIZE;
	entries = calloc(num_entries * num_io_channels, sizeof(*entries));
	SPDK_CU_ASSERT_FATAL(entries != NULL);
	ioch_array = calloc(num_io_channels, sizeof(*ioch_array));
	SPDK_CU_ASSERT_FATAL(ioch_array != NULL);

	/* Acquire whole buffer of internal entries */
	entry_idx = 0;
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(ioch_array[ioch_idx] != NULL);
		ftl_ioch = ftl_io_channel_get_ctx(ioch_array[ioch_idx]);
		poll_threads();

		for (tmp_idx = 0; tmp_idx < num_entries; ++tmp_idx) {
			entries[entry_idx++] = ftl_acquire_wbuf_entry(ftl_ioch, FTL_IO_INTERNAL);
			CU_ASSERT(entries[entry_idx - 1] != NULL);
		}

		entry = ftl_acquire_wbuf_entry(ftl_ioch, FTL_IO_INTERNAL);
		CU_ASSERT(entry == NULL);
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);

		for (tmp_idx = 0; tmp_idx < num_entries; ++tmp_idx) {
			ftl_release_wbuf_entry(entries[ioch_idx * num_entries + tmp_idx]);
			entries[ioch_idx * num_entries + tmp_idx] = NULL;
		}

		spdk_put_io_channel(ioch_array[ioch_idx]);
	}
	poll_threads();

	/* Do the same for user entries */
	entry_idx = 0;
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(ioch_array[ioch_idx] != NULL);
		ftl_ioch = ftl_io_channel_get_ctx(ioch_array[ioch_idx]);
		poll_threads();

		for (tmp_idx = 0; tmp_idx < num_entries; ++tmp_idx) {
			entries[entry_idx++] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
			CU_ASSERT(entries[entry_idx - 1] != NULL);
		}

		entry = ftl_acquire_wbuf_entry(ftl_ioch, 0);
		CU_ASSERT(entry == NULL);
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);

		for (tmp_idx = 0; tmp_idx < num_entries; ++tmp_idx) {
			ftl_release_wbuf_entry(entries[ioch_idx * num_entries + tmp_idx]);
			entries[ioch_idx * num_entries + tmp_idx] = NULL;
		}

		spdk_put_io_channel(ioch_array[ioch_idx]);
	}
	poll_threads();

	/* Verify limits */
	entry_idx = 0;
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(ioch_array[ioch_idx] != NULL);
		ftl_ioch = ftl_io_channel_get_ctx(ioch_array[ioch_idx]);
		poll_threads();

		ftl_ioch->qdepth_limit = num_entries / 2;
		for (tmp_idx = 0; tmp_idx < num_entries / 2; ++tmp_idx) {
			entries[entry_idx++] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
			CU_ASSERT(entries[entry_idx - 1] != NULL);
		}

		entry = ftl_acquire_wbuf_entry(ftl_ioch, 0);
		CU_ASSERT(entry == NULL);

		for (; tmp_idx < num_entries; ++tmp_idx) {
			entries[entry_idx++] = ftl_acquire_wbuf_entry(ftl_ioch, FTL_IO_INTERNAL);
			CU_ASSERT(entries[entry_idx - 1] != NULL);
		}
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);

		for (tmp_idx = 0; tmp_idx < num_entries; ++tmp_idx) {
			ftl_release_wbuf_entry(entries[ioch_idx * num_entries + tmp_idx]);
			entries[ioch_idx * num_entries + tmp_idx] = NULL;
		}

		spdk_put_io_channel(ioch_array[ioch_idx]);
	}
	poll_threads();

	/* Verify acquire/release */
	set_thread(0);
	ioch = spdk_get_io_channel(dev);
	SPDK_CU_ASSERT_FATAL(ioch != NULL);
	ftl_ioch = ftl_io_channel_get_ctx(ioch);
	poll_threads();

	for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
		entries[entry_idx] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
		CU_ASSERT(entries[entry_idx] != NULL);
	}

	entry = ftl_acquire_wbuf_entry(ftl_ioch, 0);
	CU_ASSERT(entry == NULL);

	for (entry_idx = 0; entry_idx < num_entries / 2; ++entry_idx) {
		ftl_release_wbuf_entry(entries[entry_idx]);
		entries[entry_idx] = NULL;
	}

	for (; entry_idx < num_entries; ++entry_idx) {
		entries[entry_idx - num_entries / 2] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
		CU_ASSERT(entries[entry_idx - num_entries / 2] != NULL);
	}

	for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
		ftl_release_wbuf_entry(entries[entry_idx]);
		entries[entry_idx] = NULL;
	}

	spdk_put_io_channel(ioch);
	poll_threads();

	free(ioch_array);
	free(entries);
	free_device(dev);
}

static void
test_submit_batch(void)
{
	struct spdk_ftl_dev *dev;
	struct spdk_io_channel **_ioch_array;
	struct ftl_io_channel **ioch_array;
	struct ftl_wbuf_entry *entry;
	struct ftl_batch *batch, *batch2;
	uint32_t num_io_channels = 16;
	uint32_t ioch_idx, tmp_idx, entry_idx;
	uint64_t ioch_bitmap;
	size_t num_entries;

	dev = setup_device(num_io_channels, num_io_channels);

	_ioch_array = calloc(num_io_channels, sizeof(*_ioch_array));
	SPDK_CU_ASSERT_FATAL(_ioch_array != NULL);
	ioch_array = calloc(num_io_channels, sizeof(*ioch_array));
	SPDK_CU_ASSERT_FATAL(ioch_array != NULL);

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		_ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(_ioch_array[ioch_idx] != NULL);
		ioch_array[ioch_idx] = ftl_io_channel_get_ctx(_ioch_array[ioch_idx]);
		poll_threads();
	}

	/* Make sure the IO channels are not starved and entries are popped in RR fashion */
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);

		for (entry_idx = 0; entry_idx < dev->xfer_size; ++entry_idx) {
			entry = ftl_acquire_wbuf_entry(ioch_array[ioch_idx], 0);
			SPDK_CU_ASSERT_FATAL(entry != NULL);

			num_entries = spdk_ring_enqueue(ioch_array[ioch_idx]->submit_queue,
							(void **)&entry, 1, NULL);
			CU_ASSERT(num_entries == 1);
		}
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		for (tmp_idx = 0; tmp_idx < ioch_idx; ++tmp_idx) {
			set_thread(tmp_idx);

			while (spdk_ring_count(ioch_array[tmp_idx]->submit_queue) < dev->xfer_size) {
				entry = ftl_acquire_wbuf_entry(ioch_array[tmp_idx], 0);
				SPDK_CU_ASSERT_FATAL(entry != NULL);

				num_entries = spdk_ring_enqueue(ioch_array[tmp_idx]->submit_queue,
								(void **)&entry, 1, NULL);
				CU_ASSERT(num_entries == 1);
			}
		}

		set_thread(ioch_idx);

		batch = ftl_get_next_batch(dev);
		SPDK_CU_ASSERT_FATAL(batch != NULL);

		TAILQ_FOREACH(entry, &batch->entries, tailq) {
			CU_ASSERT(entry->ioch == ioch_array[ioch_idx]);
		}

		ftl_release_batch(dev, batch);

		CU_ASSERT(spdk_ring_count(ioch_array[ioch_idx]->free_queue) ==
			  ioch_array[ioch_idx]->num_entries);
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels - 1; ++ioch_idx) {
		batch = ftl_get_next_batch(dev);
		SPDK_CU_ASSERT_FATAL(batch != NULL);
		ftl_release_batch(dev, batch);
	}

	/* Make sure the batch can be built from entries from any IO channel */
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		entry = ftl_acquire_wbuf_entry(ioch_array[ioch_idx], 0);
		SPDK_CU_ASSERT_FATAL(entry != NULL);

		num_entries = spdk_ring_enqueue(ioch_array[ioch_idx]->submit_queue,
						(void **)&entry, 1, NULL);
		CU_ASSERT(num_entries == 1);
	}

	batch = ftl_get_next_batch(dev);
	SPDK_CU_ASSERT_FATAL(batch != NULL);

	ioch_bitmap = 0;
	TAILQ_FOREACH(entry, &batch->entries, tailq) {
		ioch_bitmap |= 1 << entry->ioch->index;
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		CU_ASSERT((ioch_bitmap & (1 << ioch_array[ioch_idx]->index)) != 0);
	}
	ftl_release_batch(dev, batch);

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		CU_ASSERT(spdk_ring_count(ioch_array[ioch_idx]->free_queue) ==
			  ioch_array[ioch_idx]->num_entries);
	}

	/* Make sure pending batches are prioritized */
	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);

		while (spdk_ring_count(ioch_array[ioch_idx]->submit_queue) < dev->xfer_size) {
			entry = ftl_acquire_wbuf_entry(ioch_array[ioch_idx], 0);
			SPDK_CU_ASSERT_FATAL(entry != NULL);
			num_entries = spdk_ring_enqueue(ioch_array[ioch_idx]->submit_queue,
							(void **)&entry, 1, NULL);
			CU_ASSERT(num_entries == 1);
		}
	}

	batch = ftl_get_next_batch(dev);
	SPDK_CU_ASSERT_FATAL(batch != NULL);

	TAILQ_INSERT_TAIL(&dev->pending_batches, batch, tailq);
	batch2 = ftl_get_next_batch(dev);
	SPDK_CU_ASSERT_FATAL(batch2 != NULL);

	CU_ASSERT(TAILQ_EMPTY(&dev->pending_batches));
	CU_ASSERT(batch == batch2);

	batch = ftl_get_next_batch(dev);
	SPDK_CU_ASSERT_FATAL(batch != NULL);

	ftl_release_batch(dev, batch);
	ftl_release_batch(dev, batch2);

	for (ioch_idx = 2; ioch_idx < num_io_channels; ++ioch_idx) {
		batch = ftl_get_next_batch(dev);
		SPDK_CU_ASSERT_FATAL(batch != NULL);
		ftl_release_batch(dev, batch);
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		spdk_put_io_channel(_ioch_array[ioch_idx]);
	}
	poll_threads();

	free(_ioch_array);
	free(ioch_array);
	free_device(dev);
}

static void
test_entry_address(void)
{
	struct spdk_ftl_dev *dev;
	struct spdk_io_channel **ioch_array;
	struct ftl_io_channel *ftl_ioch;
	struct ftl_wbuf_entry **entry_array;
	struct ftl_addr addr;
	uint32_t num_entries, num_io_channels = 7;
	uint32_t ioch_idx, entry_idx;

	dev = setup_device(num_io_channels, num_io_channels);
	ioch_array = calloc(num_io_channels, sizeof(*ioch_array));
	SPDK_CU_ASSERT_FATAL(ioch_array != NULL);

	num_entries = dev->conf.write_buffer_size / FTL_BLOCK_SIZE;
	entry_array = calloc(num_entries, sizeof(*entry_array));
	SPDK_CU_ASSERT_FATAL(entry_array != NULL);

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ioch_array[ioch_idx] = spdk_get_io_channel(dev);
		SPDK_CU_ASSERT_FATAL(ioch_array[ioch_idx] != NULL);
		poll_threads();
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ++ioch_idx) {
		set_thread(ioch_idx);
		ftl_ioch = ftl_io_channel_get_ctx(ioch_array[ioch_idx]);

		for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
			entry_array[entry_idx] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
			SPDK_CU_ASSERT_FATAL(entry_array[entry_idx] != NULL);

			addr = ftl_get_addr_from_entry(entry_array[entry_idx]);
			CU_ASSERT(addr.cached == 1);
			CU_ASSERT((addr.cache_offset >> dev->ioch_shift) == entry_idx);
			CU_ASSERT((addr.cache_offset & ((1 << dev->ioch_shift) - 1)) == ioch_idx);
			CU_ASSERT(entry_array[entry_idx] == ftl_get_entry_from_addr(dev, addr));
		}

		for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
			ftl_release_wbuf_entry(entry_array[entry_idx]);
		}
	}

	for (ioch_idx = 0; ioch_idx < num_io_channels; ioch_idx += 2) {
		set_thread(ioch_idx);
		spdk_put_io_channel(ioch_array[ioch_idx]);
		ioch_array[ioch_idx] = NULL;
	}
	poll_threads();

	for (ioch_idx = 1; ioch_idx < num_io_channels; ioch_idx += 2) {
		set_thread(ioch_idx);
		ftl_ioch = ftl_io_channel_get_ctx(ioch_array[ioch_idx]);

		for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
			entry_array[entry_idx] = ftl_acquire_wbuf_entry(ftl_ioch, 0);
			SPDK_CU_ASSERT_FATAL(entry_array[entry_idx] != NULL);

			addr = ftl_get_addr_from_entry(entry_array[entry_idx]);
			CU_ASSERT(addr.cached == 1);
			CU_ASSERT(entry_array[entry_idx] == ftl_get_entry_from_addr(dev, addr));
		}

		for (entry_idx = 0; entry_idx < num_entries; ++entry_idx) {
			ftl_release_wbuf_entry(entry_array[entry_idx]);
		}
	}

	for (ioch_idx = 1; ioch_idx < num_io_channels; ioch_idx += 2) {
		set_thread(ioch_idx);
		spdk_put_io_channel(ioch_array[ioch_idx]);
	}
	poll_threads();

	free(entry_array);
	free(ioch_array);
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
	CU_ADD_TEST(suite, test_io_channel_create);
	CU_ADD_TEST(suite, test_acquire_entry);
	CU_ADD_TEST(suite, test_submit_batch);
	CU_ADD_TEST(suite, test_entry_address);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
