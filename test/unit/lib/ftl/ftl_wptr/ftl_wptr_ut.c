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
#include "../common/utils.c"

struct base_bdev_geometry g_geo = {
	.write_unit_size    = 16,
	.optimal_open_zones = 12,
	.zone_size	    = 128,
	.blockcnt	    = 20 * 128 * 12,
};

#if defined(DEBUG)
DEFINE_STUB(ftl_band_validate_md, bool, (struct ftl_band *band), true);
#endif
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB_V(ftl_io_dec_req, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_inc_req, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_fail, (struct ftl_io *io, int status));
DEFINE_STUB_V(ftl_trace_completion, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     enum ftl_trace_completion completion));
DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));
DEFINE_STUB_V(ftl_trace_write_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     struct ftl_addr addr, size_t addr_cnt));
DEFINE_STUB_V(ftl_rwb_get_limits, (struct ftl_rwb *rwb, size_t limit[FTL_RWB_TYPE_MAX]));
DEFINE_STUB_V(ftl_io_process_error, (struct ftl_io *io, const struct spdk_nvme_cpl *status));
DEFINE_STUB_V(ftl_trace_limits, (struct spdk_ftl_dev *dev, const size_t *limits, size_t num_free));
DEFINE_STUB(ftl_rwb_entry_cnt, size_t, (const struct ftl_rwb *rwb), 0);
DEFINE_STUB_V(ftl_rwb_set_limits, (struct ftl_rwb *rwb, const size_t limit[FTL_RWB_TYPE_MAX]));
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_zone_management, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		uint64_t zone_id, enum spdk_bdev_zone_action action,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_io_get_append_location, uint64_t, (struct spdk_bdev_io *bdev_io), 0);

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
	size_t i;
	struct spdk_ftl_dev *t_dev;

	t_dev = test_init_ftl_dev(geo);
	for (i = 0; i < ftl_get_num_bands(t_dev); ++i) {
		test_init_ftl_band(t_dev, i, geo->zone_size);
		t_dev->bands[i].state = FTL_BAND_STATE_CLOSED;
		ftl_band_set_state(&t_dev->bands[i], FTL_BAND_STATE_FREE);
	}

	*dev = t_dev;
}

static void
cleanup_wptr_test(struct spdk_ftl_dev *dev)
{
	size_t i;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		dev->bands[i].lba_map.segments = NULL;
		test_free_ftl_band(&dev->bands[i]);
	}

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

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ftl_wptr_suite", NULL, NULL);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_wptr",
			    test_wptr) == NULL
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
