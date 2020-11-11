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

#include "ftl/ftl_core.c"
#include "ftl/ftl_band.c"
#include "ftl/ftl_init.c"
#include "../common/utils.c"

struct base_bdev_geometry g_geo = {
	.write_unit_size    = 16,
	.optimal_open_zones = 12,
	.zone_size	    = 128,
	.blockcnt	    = 20 * 128 * 12,
};

#if defined(DEBUG)
DEFINE_STUB(ftl_band_validate_md, bool, (struct ftl_band *band), true);
DEFINE_STUB_V(ftl_trace_limits, (struct spdk_ftl_dev *dev, int limit, size_t num_free));

DEFINE_STUB_V(ftl_trace_completion, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     enum ftl_trace_completion completion));
DEFINE_STUB_V(ftl_trace_defrag_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_wbuf_fill, (struct spdk_ftl_dev *dev, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_wbuf_pop, (struct spdk_ftl_dev *dev, const struct ftl_wbuf_entry *entry));
DEFINE_STUB_V(ftl_trace_write_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     struct ftl_addr addr, size_t addr_cnt));
#endif
#if defined(FTL_META_DEBUG)
DEFINE_STUB_V(ftl_dev_dump_bands, (struct spdk_ftl_dev *dev));
#endif
#if defined(FTL_DUMP_STATS)
DEFINE_STUB_V(ftl_dev_dump_stats, (const struct spdk_ftl_dev *dev));
#endif
DEFINE_STUB_V(ftl_io_call_foreach_child,
	      (struct ftl_io *io, int (*callback)(struct ftl_io *)));
DEFINE_STUB(ftl_io_current_lba, uint64_t, (const struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_dec_req, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_free, (struct ftl_io *io));
DEFINE_STUB(ftl_io_get_lba, uint64_t,
	    (const struct ftl_io *io, size_t offset), 0);
DEFINE_STUB_V(ftl_io_inc_req, (struct ftl_io *io));
DEFINE_STUB(ftl_io_iovec_addr, void *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_iovec_len_left, size_t, (struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_fail, (struct ftl_io *io, int status));
DEFINE_STUB(ftl_io_init_internal, struct ftl_io *,
	    (const struct ftl_io_init_opts *opts), NULL);
DEFINE_STUB_V(ftl_io_reset, (struct ftl_io *io));
DEFINE_STUB(ftl_iovec_num_blocks, size_t,
	    (struct iovec *iov, size_t iov_cnt), 0);
DEFINE_STUB_V(ftl_io_process_error, (struct ftl_io *io, const struct spdk_nvme_cpl *status));
DEFINE_STUB_V(ftl_io_shrink_iovec, (struct ftl_io *io, size_t num_blocks));
DEFINE_STUB(ftl_io_wbuf_init, struct ftl_io *,
	    (struct spdk_ftl_dev *dev, struct ftl_addr addr,
	     struct ftl_band *band, struct ftl_batch *batch, ftl_io_fn cb), NULL);
DEFINE_STUB(ftl_io_user_init, struct ftl_io *,
	    (struct spdk_io_channel *ioch, uint64_t lba, size_t num_blocks,
	     struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
	     void *cb_arg, int type), NULL);
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

DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_md_size, uint32_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_media_events, size_t,
	    (struct spdk_bdev_desc *bdev_desc, struct spdk_bdev_media_event *events,
	     size_t max_events), 0);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_write_unit_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_zone_info, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_io_get_append_location, uint64_t, (struct spdk_bdev_io *bdev_io), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB(spdk_bdev_is_md_separate, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_is_zoned, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
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
DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_zone_appendv, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_zone_management, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		uint64_t zone_id, enum spdk_bdev_zone_action action,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_mempool_create_ctor, struct spdk_mempool *,
	    (const char *name, size_t count, size_t ele_size, size_t cache_size,
	     int socket_id, spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg), NULL);
DEFINE_STUB(spdk_mempool_obj_iter, uint32_t,
	    (struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb, void *obj_cb_arg), 0);

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB_V(pmem_persist, (const void *addr, size_t len));
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

struct ftl_io *
ftl_io_erase_init(struct ftl_band *band, size_t num_blocks, ftl_io_fn cb)
{
	struct ftl_io *io;

	io = calloc(1, sizeof(struct ftl_io));
	SPDK_CU_ASSERT_FATAL(io != NULL);

	io->dev = band->dev;
	io->band = band;
	io->cb_fn = cb;
	io->num_blocks = 1;

	return io;
}

void
ftl_io_advance(struct ftl_io *io, size_t num_blocks)
{
	io->pos += num_blocks;
}

void
ftl_io_complete(struct ftl_io *io)
{
	io->cb_fn(io, NULL, 0);
	free(io);
}

static void
setup_wptr_test(struct spdk_ftl_dev **dev, const struct base_bdev_geometry *geo)
{
	struct spdk_ftl_dev *t_dev;
	struct _ftl_io_channel *_ioch;
	size_t i;

	t_dev = test_init_ftl_dev(geo);
	for (i = 0; i < ftl_get_num_bands(t_dev); ++i) {
		test_init_ftl_band(t_dev, i, geo->zone_size);
		t_dev->bands[i].state = FTL_BAND_STATE_CLOSED;
		ftl_band_set_state(&t_dev->bands[i], FTL_BAND_STATE_FREE);
	}

	_ioch = (struct _ftl_io_channel *)(t_dev->ioch + 1);
	_ioch->ioch = calloc(1, sizeof(*_ioch->ioch));
	SPDK_CU_ASSERT_FATAL(_ioch->ioch != NULL);

	*dev = t_dev;
}

static void
cleanup_wptr_test(struct spdk_ftl_dev *dev)
{
	struct _ftl_io_channel *_ioch;
	size_t i;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		dev->bands[i].lba_map.segments = NULL;
		test_free_ftl_band(&dev->bands[i]);
	}

	_ioch = (struct _ftl_io_channel *)(dev->ioch + 1);
	free(_ioch->ioch);

	test_free_ftl_dev(dev);
}

static void
test_wptr(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_wptr *wptr;
	struct ftl_band *band;
	struct ftl_io io = { 0 };
	size_t xfer_size;
	size_t zone, block, offset, i;
	int rc;

	setup_wptr_test(&dev, &g_geo);

	xfer_size = dev->xfer_size;
	ftl_add_wptr(dev);
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		wptr = LIST_FIRST(&dev->wptr_list);
		band = wptr->band;
		ftl_band_set_state(band, FTL_BAND_STATE_OPENING);
		ftl_band_set_state(band, FTL_BAND_STATE_OPEN);
		io.band = band;
		io.dev = dev;

		for (block = 0, offset = 0; block < ftl_get_num_blocks_in_zone(dev) / xfer_size; ++block) {
			for (zone = 0; zone < band->num_zones; ++zone) {
				CU_ASSERT_EQUAL(wptr->offset, offset);
				ftl_wptr_advance(wptr, xfer_size);
				offset += xfer_size;
			}
		}

		CU_ASSERT_EQUAL(band->state, FTL_BAND_STATE_FULL);

		ftl_band_set_state(band, FTL_BAND_STATE_CLOSING);

		/* Call the metadata completion cb to force band state change */
		/* and removal of the actual wptr */
		ftl_md_write_cb(&io, NULL, 0);
		CU_ASSERT_EQUAL(band->state, FTL_BAND_STATE_CLOSED);
		CU_ASSERT_TRUE(LIST_EMPTY(&dev->wptr_list));

		rc = ftl_add_wptr(dev);

		/* There are no free bands during the last iteration, so */
		/* there'll be no new wptr allocation */
		if (i == (ftl_get_num_bands(dev) - 1)) {
			CU_ASSERT_EQUAL(rc, -1);
		} else {
			CU_ASSERT_EQUAL(rc, 0);
		}
	}

	cleanup_wptr_test(dev);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_wptr_suite", NULL, NULL);


	CU_ADD_TEST(suite, test_wptr);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
