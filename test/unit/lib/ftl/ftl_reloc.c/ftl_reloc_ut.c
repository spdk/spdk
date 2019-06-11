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

#include "ftl/ftl_reloc.c"
#include "../common/utils.c"

#define MAX_ACTIVE_RELOCS 5
#define MAX_RELOC_QDEPTH  31

static struct spdk_ocssd_geometry_data g_geo = {
	.num_grp	= 4,
	.num_pu		= 3,
	.num_chk	= 500,
	.clba		= 100,
	.ws_opt		= 16,
	.ws_min		= 4,
};

static struct spdk_ftl_punit_range g_range = {
	.begin		= 2,
	.end		= 9,
};

DEFINE_STUB(ftl_dev_tail_md_disk_size, size_t, (const struct spdk_ftl_dev *dev), 1);
DEFINE_STUB_V(ftl_band_set_state, (struct ftl_band *band, enum ftl_band_state state));
DEFINE_STUB_V(ftl_trace_lba_io_init, (struct spdk_ftl_dev *dev, const struct ftl_io *io));
DEFINE_STUB_V(ftl_free_io, (struct ftl_io *io));

int
ftl_band_alloc_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	ftl_band_acquire_lba_map(band);
	band->lba_map.map = spdk_mempool_get(dev->lba_pool);

	return 0;
}

void
ftl_band_release_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	band->lba_map.ref_cnt--;
	spdk_mempool_put(dev->lba_pool, band->lba_map.map);
	band->lba_map.map = NULL;
}

void
ftl_band_acquire_lba_map(struct ftl_band *band)
{
	band->lba_map.ref_cnt++;
}

size_t
ftl_lba_map_num_lbks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_num_band_lbks(dev) * sizeof(uint64_t), FTL_BLOCK_SIZE);
}

int
ftl_band_read_lba_map(struct ftl_band *band, size_t offset,
		      size_t lbk_cnt, ftl_io_fn fn, void *ctx)
{
	fn(ctx, ctx, 0);
	return 0;
}

uint64_t
ftl_band_lbkoff_from_ppa(struct ftl_band *band, struct ftl_ppa ppa)
{
	return test_offset_from_ppa(ppa, band);
}

struct ftl_ppa
ftl_band_ppa_from_lbkoff(struct ftl_band *band, uint64_t lbkoff)
{
	struct ftl_ppa ppa = { .ppa = 0 };
	struct spdk_ftl_dev *dev = band->dev;
	uint64_t punit;

	punit = lbkoff / ftl_dev_lbks_in_chunk(dev) + dev->range.begin;

	ppa.lbk = lbkoff % ftl_dev_lbks_in_chunk(dev);
	ppa.chk = band->id;
	ppa.pu = punit / dev->geo.num_grp;
	ppa.grp = punit % dev->geo.num_grp;

	return ppa;
}

void
ftl_io_read(struct ftl_io *io)
{
	io->cb_fn(io, io->cb_ctx, 0);
	free(io);
}

void
ftl_io_write(struct ftl_io *io)
{
	io->cb_fn(io, io->cb_ctx, 0);
	free(io->lba.vector);
	free(io);
}

struct ftl_io *
ftl_io_init_internal(const struct ftl_io_init_opts *opts)
{
	struct ftl_io *io = opts->io;

	if (!io) {
		io = calloc(1, opts->size);
	}

	SPDK_CU_ASSERT_FATAL(io != NULL);

	io->dev = opts->dev;
	io->band = opts->band;
	io->flags = opts->flags;
	io->cb_fn = opts->cb_fn;
	io->cb_ctx = io;
	io->lbk_cnt = opts->lbk_cnt;
	io->iov[0].iov_base = opts->data;

	if (opts->flags & FTL_IO_VECTOR_LBA) {
		io->lba.vector = calloc(io->lbk_cnt, sizeof(uint64_t));
		SPDK_CU_ASSERT_FATAL(io->lba.vector != NULL);
	}

	return io;
}

struct ftl_io *
ftl_io_alloc(struct spdk_io_channel *ch)
{
	size_t io_size = sizeof(struct ftl_md_io);

	return malloc(io_size);
}

void
ftl_io_reinit(struct ftl_io *io, ftl_io_fn fn, void *ctx, int flags, int type)
{
	io->cb_fn = fn;
	io->cb_ctx = ctx;
	io->type = type;
}

static void
single_reloc_move(struct ftl_band_reloc *breloc)
{
	/* Process read */
	ftl_process_reloc(breloc);
	/* Process lba map read */
	ftl_process_reloc(breloc);
	/* Process write */
	ftl_process_reloc(breloc);
}

static void
setup_reloc(struct spdk_ftl_dev **_dev, struct ftl_reloc **_reloc,
	    const struct spdk_ocssd_geometry_data *geo, const struct spdk_ftl_punit_range *range)
{
	size_t i;
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;

	dev = test_init_ftl_dev(geo, range);
	dev->conf.max_active_relocs = MAX_ACTIVE_RELOCS;
	dev->conf.max_reloc_qdepth = MAX_RELOC_QDEPTH;

	SPDK_CU_ASSERT_FATAL(ftl_dev_num_bands(dev) > 0);

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		test_init_ftl_band(dev, i);
	}

	reloc = ftl_reloc_init(dev);
	dev->reloc = reloc;
	CU_ASSERT_PTR_NOT_NULL_FATAL(reloc);
	ftl_reloc_resume(reloc);

	*_dev = dev;
	*_reloc = reloc;
}

static void
cleanup_reloc(struct spdk_ftl_dev *dev, struct ftl_reloc *reloc)
{
	size_t i;

	for (i = 0; i < ftl_dev_num_bands(reloc->dev); ++i) {
		SPDK_CU_ASSERT_FATAL(reloc->brelocs[i].active == false);
	}

	ftl_reloc_free(reloc);

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		test_free_ftl_band(&dev->bands[i]);
	}
	test_free_ftl_dev(dev);
}

static void
set_band_valid_map(struct ftl_band *band, size_t offset, size_t num_lbks)
{
	struct ftl_lba_map *lba_map = &band->lba_map;
	size_t i;

	SPDK_CU_ASSERT_FATAL(lba_map != NULL);
	for (i = offset; i < offset + num_lbks; ++i) {
		spdk_bit_array_set(lba_map->vld, i);
		lba_map->num_vld++;
	}
}

static void
test_reloc_iter_full(void)
{
	size_t num_lbks, num_iters, reminder, i;
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
	struct ftl_ppa ppa;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	dev->geo.clba = 100;
	breloc = &reloc->brelocs[0];
	band = breloc->band;

	set_band_valid_map(band, 0, ftl_num_band_lbks(dev));

	ftl_reloc_add(reloc, band, 0, ftl_num_band_lbks(dev), 0);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));

	num_iters = ftl_dev_num_punits(dev) *
		    (ftl_dev_lbks_in_chunk(dev) / reloc->xfer_size);

	for (i = 0; i < num_iters; i++) {
		num_lbks = ftl_reloc_next_lbks(breloc, &ppa);
		CU_ASSERT_EQUAL(num_lbks, reloc->xfer_size);
	}

	num_iters = ftl_dev_num_punits(dev);

	/* ftl_reloc_next_lbks is searching for maximum xfer_size */
	/* contiguous valid logic blocks in chunk, so we can end up */
	/* with some reminder if number of logical blocks in chunk */
	/* is not divisible by xfer_size */
	reminder = ftl_dev_lbks_in_chunk(dev) % reloc->xfer_size;
	for (i = 0; i < num_iters; i++) {
		num_lbks = ftl_reloc_next_lbks(breloc, &ppa);
		CU_ASSERT_EQUAL(reminder, num_lbks);
	}

	/* num_lbks should remain intact since all the blocks are valid */
	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_iter_empty(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
	struct ftl_ppa ppa;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;

	ftl_reloc_add(reloc, band, 0, ftl_num_band_lbks(dev), 0);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));
	CU_ASSERT_EQUAL(0, ftl_reloc_next_lbks(breloc, &ppa));
	CU_ASSERT_EQUAL(breloc->num_lbks, 0);

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_full_band(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
	size_t num_moves, num_iters, num_lbk, i;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;
	num_moves = MAX_RELOC_QDEPTH * reloc->xfer_size;
	num_iters = ftl_num_band_lbks(dev) / num_moves;

	set_band_valid_map(band, 0, ftl_num_band_lbks(dev));

	ftl_reloc_add(reloc, band, 0, ftl_num_band_lbks(dev), 0);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));

	ftl_reloc_add_active_queue(breloc);

	for (i = 1; i <= num_iters; ++i) {
		single_reloc_move(breloc);
		num_lbk = ftl_num_band_lbks(dev) - (i * num_moves);
		CU_ASSERT_EQUAL(breloc->num_lbks, num_lbk);

	}

	/*  Process reminder lbks */
	single_reloc_move(breloc);
	/*  Drain move queue */
	ftl_reloc_process_moves(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, 0);
	CU_ASSERT_TRUE(ftl_reloc_done(breloc));
	ftl_reloc_release(breloc);

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_scatter_band(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
	size_t num_iters, i;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;
	num_iters = ftl_num_band_lbks(dev) / MAX_RELOC_QDEPTH;

	for (i = 0; i < ftl_num_band_lbks(dev); ++i) {
		if (i % 2) {
			set_band_valid_map(band, i, 1);
		}
	}

	ftl_reloc_add(reloc, band, 0, ftl_num_band_lbks(dev), 0);
	ftl_reloc_add_active_queue(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));

	for (i = 0; i < num_iters ; ++i) {
		single_reloc_move(breloc);
	}

	CU_ASSERT_EQUAL(breloc->num_lbks, 0);
	CU_ASSERT_TRUE(ftl_reloc_done(breloc));
	ftl_reloc_release(breloc);

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_chunk(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
	size_t num_io, num_iters, num_lbk, i;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;
	/* High priority band have allocated lba map */
	band->high_prio = 1;
	ftl_band_alloc_lba_map(band);
	num_io = MAX_RELOC_QDEPTH * reloc->xfer_size;
	num_iters = ftl_dev_lbks_in_chunk(dev) / num_io;

	set_band_valid_map(band, 0, ftl_num_band_lbks(dev));

	ftl_reloc_add(reloc, band, ftl_dev_lbks_in_chunk(dev) * 3,
		      ftl_dev_lbks_in_chunk(dev), 1);
	ftl_reloc_add_active_queue(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_dev_lbks_in_chunk(dev));

	for (i = 1; i <= num_iters ; ++i) {
		single_reloc_move(breloc);
		num_lbk = ftl_dev_lbks_in_chunk(dev) - (i * num_io);

		CU_ASSERT_EQUAL(breloc->num_lbks, num_lbk);
	}

	/* In case num_lbks_in_chunk % num_io != 0 one extra iteration is needed  */
	single_reloc_move(breloc);
	/*  Drain move queue */
	ftl_reloc_process_moves(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, 0);
	CU_ASSERT_TRUE(ftl_reloc_done(breloc));
	ftl_reloc_release(breloc);

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_single_lbk(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;
#define TEST_RELOC_OFFSET 6

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;

	set_band_valid_map(band, TEST_RELOC_OFFSET, 1);

	ftl_reloc_add(reloc, band, TEST_RELOC_OFFSET, 1, 0);
	ftl_reloc_add_active_queue(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, 1);

	single_reloc_move(breloc);
	/*  Drain move queue */
	ftl_reloc_process_moves(breloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, 0);
	CU_ASSERT_TRUE(ftl_reloc_done(breloc));
	ftl_reloc_release(breloc);

	cleanup_reloc(dev, reloc);
}

static void
test_reloc_empty_band(void)
{
	struct spdk_ftl_dev *dev;
	struct ftl_reloc *reloc;
	struct ftl_band_reloc *breloc;
	struct ftl_band *band;

	setup_reloc(&dev, &reloc, &g_geo, &g_range);

	breloc = &reloc->brelocs[0];
	band = breloc->band;

	ftl_reloc_add(reloc, band, 0, ftl_num_band_lbks(dev), 0);

	CU_ASSERT_EQUAL(breloc->num_lbks, ftl_num_band_lbks(dev));

	ftl_reloc(reloc);

	CU_ASSERT_EQUAL(breloc->num_lbks, 0);
	SPDK_CU_ASSERT_FATAL(breloc->moves != NULL);

	cleanup_reloc(dev, reloc);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ftl_band_suite", NULL, NULL);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_reloc_iter_full",
			    test_reloc_iter_full) == NULL
		|| CU_add_test(suite, "test_reloc_iter_empty",
			       test_reloc_iter_empty) == NULL
		|| CU_add_test(suite, "test_reloc_empty_band",
			       test_reloc_empty_band) == NULL
		|| CU_add_test(suite, "test_reloc_full_band",
			       test_reloc_full_band) == NULL
		|| CU_add_test(suite, "test_reloc_scatter_band",
			       test_reloc_scatter_band) == NULL
		|| CU_add_test(suite, "test_reloc_chunk",
			       test_reloc_chunk) == NULL
		|| CU_add_test(suite, "test_reloc_single_lbk",
			       test_reloc_single_lbk) == NULL
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
