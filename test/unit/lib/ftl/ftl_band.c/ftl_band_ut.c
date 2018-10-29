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

#include "stdatomic.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#define FTL_TRACE_ENABLED 0

#include "ftl/ftl_core.c"
#include "ftl/ftl_band.c"
#include "../common/utils.c"

#define TEST_BAND_IDX		68
#define TEST_LBA		0x68676564

#if defined(DEBUG)
DEFINE_STUB(ftl_band_validate_md, bool, (struct ftl_band *band, const uint64_t *lba_map), true);
#endif
DEFINE_STUB_V(ftl_rwb_batch_release, (struct ftl_rwb_batch *batch));
DEFINE_STUB_V(ftl_rwb_push, (struct ftl_rwb_entry *entry));
DEFINE_STUB_V(ftl_rwb_set_limits, (struct ftl_rwb *rwb, const size_t limit[FTL_RWB_TYPE_MAX]));
DEFINE_STUB_V(ftl_rwb_get_limits, (struct ftl_rwb *rwb, size_t limit[FTL_RWB_TYPE_MAX]));
DEFINE_STUB_V(ftl_rwb_batch_revert, (struct ftl_rwb_batch *batch));
DEFINE_STUB(ftl_rwb_acquire, struct ftl_rwb_entry *, (struct ftl_rwb *rwb,
		enum ftl_rwb_entry_type type), NULL);
DEFINE_STUB(ftl_rwb_first_batch, struct ftl_rwb_batch *, (struct ftl_rwb *rwb), NULL);
DEFINE_STUB(ftl_rwb_next_batch, struct ftl_rwb_batch *, (struct ftl_rwb_batch *batch), NULL);
DEFINE_STUB(ftl_rwb_entry_from_offset, struct ftl_rwb_entry *, (struct ftl_rwb *rwb, size_t offset),
	    NULL);
DEFINE_STUB(ftl_rwb_batch_first_entry, struct ftl_rwb_entry *, (struct ftl_rwb_batch *batch), NULL);
DEFINE_STUB(ftl_rwb_num_acquired, size_t, (struct ftl_rwb *rwb, enum ftl_rwb_entry_type type), 0);
DEFINE_STUB(ftl_rwb_num_batches, size_t, (const struct ftl_rwb *rwb), 0);
DEFINE_STUB(ftl_rwb_batch_get_offset, size_t, (const struct ftl_rwb_batch *batch), 0);
DEFINE_STUB(ftl_rwb_entry_cnt, size_t, (const struct ftl_rwb *rwb), 0);
DEFINE_STUB(ftl_rwb_batch_empty, int, (struct ftl_rwb_batch *batch), 0);
DEFINE_STUB(ftl_rwb_pop, struct ftl_rwb_batch *, (struct ftl_rwb *rwb), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_ns, struct spdk_nvme_ns *,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid), NULL);
DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t,
	    (struct spdk_nvme_qpair *qpair,
	     uint32_t max_completions), 0);
DEFINE_STUB(spdk_nvme_ns_cmd_read, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
	     uint64_t lba,
	     uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
	     uint32_t io_flags), 0);
DEFINE_STUB(spdk_nvme_ocssd_ns_cmd_vector_reset, int,
	    (struct spdk_nvme_ns *ns,
	     struct spdk_nvme_qpair *qpair,
	     uint64_t *lba_list, uint32_t num_lbas,
	     struct spdk_ocssd_chunk_information_entry *chunk_info,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvme_ns_cmd_write_with_md, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	     void *buffer, void *metadata, uint64_t lba,
	     uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
	     uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB_V(ftl_reloc, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_lbks, int prio));
DEFINE_STUB(ftl_anm_register_device, int, (struct spdk_ftl_dev *dev, ftl_anm_fn fn), 0);
DEFINE_STUB_V(ftl_anm_event_complete, (struct ftl_anm_event *event));
DEFINE_STUB_V(ftl_anm_unregister_device, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_io_init_internal, struct ftl_io *, (const struct ftl_io_init_opts *opts), NULL);
DEFINE_STUB(ftl_io_inc_req, size_t, (struct ftl_io *io), 0);
DEFINE_STUB(ftl_io_dec_req, size_t, (struct ftl_io *io), 0);
DEFINE_STUB(ftl_io_current_lba, uint64_t, (struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_update_iovec, (struct ftl_io *io, size_t lbk_cnt));
DEFINE_STUB(ftl_iovec_num_lbks, size_t, (struct iovec *iov, size_t iov_cnt), 0);
DEFINE_STUB(ftl_io_iovec_addr, void *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_iovec_len_left, size_t, (struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_user_init, (struct spdk_ftl_dev *dev, struct ftl_io *io, uint64_t lba,
				 size_t lbk_cnt, struct iovec *iov, size_t iov_cnt,
				 const spdk_ftl_fn cb_fn, void *cb_arg, int type));
DEFINE_STUB(ftl_io_get_md, void *, (const struct ftl_io *io), NULL);
DEFINE_STUB_V(ftl_io_complete, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_process_error, (struct ftl_io *io, const struct spdk_nvme_cpl *status));
DEFINE_STUB(ftl_io_erase_init, struct ftl_io *, (struct ftl_band *band, size_t lbk_cnt,
		spdk_ftl_fn cb),
	    NULL);
DEFINE_STUB(ftl_io_iovec, struct iovec *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_rwb_init, struct ftl_io *, (struct spdk_ftl_dev *dev, struct ftl_band *band,
		struct ftl_rwb_batch *batch, spdk_ftl_fn cb), NULL);
DEFINE_STUB(ftl_io_alloc, struct ftl_io *, (struct spdk_io_channel *ch), NULL);
DEFINE_STUB_V(ftl_io_free, (struct ftl_io *io));
DEFINE_STUB(ftl_trace_init,  struct ftl_trace *, (void), NULL);
DEFINE_STUB(ftl_trace_alloc_group, ftl_trace_group_t, (struct ftl_trace *trace),
	    FTL_TRACE_INVALID_ID);
DEFINE_STUB_V(ftl_trace_free, (struct ftl_trace *trace));
DEFINE_STUB_V(ftl_trace_defrag_band, (struct ftl_trace *trace, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_write_band, (struct ftl_trace *trace, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_lba_io_init, (struct ftl_trace *trace, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_rwb_fill, (struct ftl_trace *trace, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_rwb_pop, (struct ftl_trace *trace, const struct ftl_rwb_entry *entry));
DEFINE_STUB_V(ftl_trace_submission, (struct ftl_trace *trace, const struct ftl_io *io,
				     struct ftl_ppa ppa, size_t ppa_cnt));
DEFINE_STUB_V(ftl_trace_completion, (struct ftl_trace *trace,
				     const struct ftl_io *io,
				     enum ftl_trace_completion type));
DEFINE_STUB_V(ftl_trace_limits, (struct ftl_trace *trace, const size_t *limits, size_t num_free));

static struct spdk_ocssd_geometry_data g_geo = {
	.num_grp	= 4,
	.num_pu		= 3,
	.num_chk	= 1500,
	.clba		= 100,
	.ws_opt		= 16,
	.ws_min		= 4,
};

static struct spdk_ftl_punit_range g_range = {
	.begin		= 2,
	.end		= 9,
};

static struct spdk_ftl_dev		*g_dev;
static struct ftl_band	*g_band;

static void
setup_band(void)
{
	g_dev = test_init_ftl_dev(&g_geo, &g_range);
	g_band = test_init_ftl_band(g_dev, TEST_BAND_IDX);
	int rc = ftl_band_alloc_md(g_band);
	CU_ASSERT_EQUAL_FATAL(rc, 0);
}

static void
cleanup_band(void)
{
	test_free_ftl_band(g_band);
	test_free_ftl_dev(g_dev);
}

static struct ftl_ppa
ppa_from_punit(uint64_t punit)
{
	struct ftl_ppa ppa = {};
	ppa.grp = punit % g_geo.num_grp;
	ppa.pu = punit / g_geo.num_grp;
	return ppa;
}

static uint64_t
offset_from_ppa(struct ftl_ppa ppa, struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	unsigned int punit;

	punit = ftl_ppa_flatten_punit(dev, ppa);
	assert(ppa.chk == band->id);

	return punit * ftl_dev_lbks_in_chunk(dev) + ppa.lbk;
}

static void
test_band_lbkoff_from_ppa_base(void)
{
	struct ftl_ppa ppa;
	uint64_t offset, i, flat_lun = 0;

	for (i = g_range.begin; i < g_range.end; ++i) {
		ppa = ppa_from_punit(i);
		ppa.chk = TEST_BAND_IDX;

		offset = ftl_band_lbkoff_from_ppa(g_band, ppa);
		CU_ASSERT_EQUAL(offset, flat_lun * ftl_dev_lbks_in_chunk(g_dev));
		flat_lun++;
	}
}

static void
test_band_lbkoff_from_ppa_lbk(void)
{
	struct ftl_ppa ppa;
	uint64_t offset, expect, i, j;

	for (i = g_range.begin; i < g_range.end; ++i) {
		for (j = 0; j < g_geo.clba; ++j) {
			ppa = ppa_from_punit(i);
			ppa.chk = TEST_BAND_IDX;
			ppa.lbk = j;

			offset = ftl_band_lbkoff_from_ppa(g_band, ppa);

			expect = offset_from_ppa(ppa, g_band);
			CU_ASSERT_EQUAL(offset, expect);
		}
	}
}

static void
test_band_ppa_from_lbkoff(void)
{
	struct ftl_ppa ppa, expect;
	uint64_t offset, i, j;

	for (i = g_range.begin; i < g_range.end; ++i) {
		for (j = 0; j < g_geo.clba; ++j) {
			expect = ppa_from_punit(i);
			expect.chk = TEST_BAND_IDX;
			expect.lbk = j;

			offset = ftl_band_lbkoff_from_ppa(g_band, expect);
			ppa = ftl_band_ppa_from_lbkoff(g_band, offset);

			CU_ASSERT_EQUAL(ppa.ppa, expect.ppa);
		}
	}
}

static void
test_band_set_addr(void)
{
	struct ftl_md *md = &g_band->md;
	struct ftl_ppa ppa;
	uint64_t offset = 0;

	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;

	CU_ASSERT_EQUAL(md->num_vld, 0);

	offset = offset_from_ppa(ppa, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 1);
	CU_ASSERT_EQUAL(md->lba_map[offset], TEST_LBA);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset));

	ppa.pu++;
	offset = offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 2);
	CU_ASSERT_EQUAL(md->lba_map[offset], TEST_LBA + 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset));
	ppa.pu--;
	offset = offset_from_ppa(ppa, g_band);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset));
}

static void
test_invalidate_addr(void)
{
	struct ftl_md *md = &g_band->md;
	struct ftl_ppa ppa;
	uint64_t offset[2];

	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	offset[0] = offset_from_ppa(ppa, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset[0]));
	ftl_invalidate_addr(g_band->dev, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 0);
	CU_ASSERT_FALSE(spdk_bit_array_get(md->vld_map, offset[0]));

	offset[0] = offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	ppa.pu++;
	offset[1] = offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 2);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset[0]));
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset[1]));
	ftl_invalidate_addr(g_band->dev, ppa);
	CU_ASSERT_EQUAL(md->num_vld, 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(md->vld_map, offset[0]));
	CU_ASSERT_FALSE(spdk_bit_array_get(md->vld_map, offset[1]));
}

static void
test_next_xfer_ppa(void)
{
	struct ftl_ppa ppa, result, expect;

	/* Verify simple one lbk incremention */
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = 0;
	expect = ppa;
	expect.lbk = 1;

	result = ftl_band_next_xfer_ppa(g_band, ppa, 1);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping between chunks */
	expect = ppa_from_punit(g_range.begin + 1);
	expect.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping works with unaligned offsets */
	expect = ppa_from_punit(g_range.begin + 1);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = 3;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping from last chunk to the first one */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size;
	ppa = ppa_from_punit(g_range.end);
	ppa.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping from last chunk to the first one with unaligned offset */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size + 2;
	ppa = ppa_from_punit(g_range.end);
	ppa.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size + 2);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify large offset spanning across the whole band multiple times */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size * 5 + 4;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_ppa(g_band, ppa, 3 * g_dev->xfer_size *
					ftl_dev_num_punits(g_dev) + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Remove one chunk and verify it's skipped properly */
	g_band->chunk_buf[1].state = FTL_CHUNK_STATE_BAD;
	CIRCLEQ_REMOVE(&g_band->chunks, &g_band->chunk_buf[1], circleq);
	g_band->num_chunks--;
	expect = ppa_from_punit(g_range.begin + 2);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size * 5 + 4;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_ppa(g_band, ppa, 3 * g_dev->xfer_size *
					(ftl_dev_num_punits(g_dev) - 1) + g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite_with_setup_and_teardown("ftl_band_suite", NULL, NULL,
			setup_band, cleanup_band);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_band_lbkoff_from_ppa_base",
			    test_band_lbkoff_from_ppa_base) == NULL
		|| CU_add_test(suite, "test_band_lbkoff_from_ppa_lbk",
			       test_band_lbkoff_from_ppa_lbk) == NULL
		|| CU_add_test(suite, "test_band_ppa_from_lbkoff",
			       test_band_ppa_from_lbkoff) == NULL
		|| CU_add_test(suite, "test_band_set_addr",
			       test_band_set_addr) == NULL
		|| CU_add_test(suite, "test_invalidate_addr",
			       test_invalidate_addr) == NULL
		|| CU_add_test(suite, "test_next_xfer_ppa",
			       test_next_xfer_ppa) == NULL
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
