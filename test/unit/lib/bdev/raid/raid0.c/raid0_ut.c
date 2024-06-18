/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid0.c"
#include "../common.c"

#define MAX_BASE_DRIVES 32
#define MAX_TEST_IO_RANGE (3 * 3 * 3 * (MAX_BASE_DRIVES + 5))
#define BLOCK_CNT (1024ul * 1024ul * 1024ul * 1024ul)

/* Data structure to capture the output of IO for verification */
struct io_output {
	struct spdk_bdev_desc       *desc;
	struct spdk_io_channel      *ch;
	uint64_t                    offset_blocks;
	uint64_t                    num_blocks;
	spdk_bdev_io_completion_cb  cb;
	void                        *cb_arg;
	enum spdk_bdev_io_type      iotype;
	struct iovec                *iovs;
	int                         iovcnt;
	void                        *md_buf;
};

struct raid_io_ranges {
	uint64_t lba;
	uint64_t nblocks;
};

/* Globals */
struct io_output *g_io_output = NULL;
uint32_t g_io_output_index;
uint32_t g_io_comp_status;
bool g_child_io_status_flag;
TAILQ_HEAD(bdev, spdk_bdev);
uint32_t g_block_len;
uint32_t g_strip_size;
uint32_t g_max_io_size;
uint8_t g_max_base_drives;
struct raid_io_ranges g_io_ranges[MAX_TEST_IO_RANGE];
uint32_t g_io_range_idx;
bool g_enable_dif;

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_queue_io_wait, (struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
					struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn));
DEFINE_STUB(spdk_bdev_flush_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_is_dif_head_of_md, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_notify_blockcnt_change, int, (struct spdk_bdev *bdev, uint64_t size), 0);

bool
spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && bdev->md_interleave;
}

bool
spdk_bdev_is_md_separate(const struct spdk_bdev *bdev)
{
	return (bdev->md_len != 0) && !bdev->md_interleave;
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

static int
set_test_opts(void)
{
	g_max_base_drives = MAX_BASE_DRIVES;
	g_block_len = 4096;
	g_strip_size = 64;
	g_max_io_size = 1024;
	g_enable_dif = false;

	return 0;
}

static int
set_test_opts_dif(void)
{
	set_test_opts();
	g_enable_dif = true;

	return 0;
}

/* Set globals before every test run */
static void
set_globals(void)
{
	uint32_t max_splits;

	if (g_max_io_size < g_strip_size) {
		max_splits = 2;
	} else {
		max_splits = (g_max_io_size / g_strip_size) + 1;
	}
	if (max_splits < g_max_base_drives) {
		max_splits = g_max_base_drives;
	}

	g_io_output = calloc(max_splits, sizeof(struct io_output));
	SPDK_CU_ASSERT_FATAL(g_io_output != NULL);
	g_io_output_index = 0;
	g_io_comp_status = 0;
	g_child_io_status_flag = true;
}

/* Reset globals */
static void
reset_globals(void)
{
	if (g_io_output) {
		free(g_io_output);
		g_io_output = NULL;
	}
}

static void
generate_dif(struct iovec *iovs, int iovcnt, void *md_buf,
	     uint64_t offset_blocks, uint32_t num_blocks, struct spdk_bdev *bdev)
{
	struct spdk_dif_ctx dif_ctx;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	spdk_dif_type_t dif_type;
	bool md_interleaved;
	struct iovec md_iov;

	dif_type = spdk_bdev_get_dif_type(bdev);
	md_interleaved = spdk_bdev_is_md_interleaved(bdev);

	if (dif_type == SPDK_DIF_DISABLE) {
		return;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       md_interleaved,
			       spdk_bdev_is_dif_head_of_md(bdev),
			       dif_type,
			       bdev->dif_check_flags,
			       offset_blocks,
			       0xFFFF, 0x123, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	if (!md_interleaved) {
		md_iov.iov_base = md_buf;
		md_iov.iov_len	= spdk_bdev_get_md_size(bdev) * num_blocks;

		rc = spdk_dix_generate(iovs, iovcnt, &md_iov, num_blocks, &dif_ctx);
		SPDK_CU_ASSERT_FATAL(rc == 0);
	}
}

static void
verify_dif(struct iovec *iovs, int iovcnt, void *md_buf,
	   uint64_t offset_blocks, uint32_t num_blocks, struct spdk_bdev *bdev)
{
	struct spdk_dif_ctx dif_ctx;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct spdk_dif_error errblk;
	spdk_dif_type_t dif_type;
	bool md_interleaved;
	struct iovec md_iov;

	dif_type = spdk_bdev_get_dif_type(bdev);
	md_interleaved = spdk_bdev_is_md_interleaved(bdev);

	if (dif_type == SPDK_DIF_DISABLE) {
		return;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       md_interleaved,
			       spdk_bdev_is_dif_head_of_md(bdev),
			       dif_type,
			       bdev->dif_check_flags,
			       offset_blocks,
			       0xFFFF, 0x123, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	if (!md_interleaved) {
		md_iov.iov_base = md_buf;
		md_iov.iov_len	= spdk_bdev_get_md_size(bdev) * num_blocks;

		rc = spdk_dix_verify(iovs, iovcnt,
				     &md_iov, num_blocks, &dif_ctx, &errblk);
		SPDK_CU_ASSERT_FATAL(rc == 0);
	}
}

static void
remap_dif(void *md_buf, uint64_t num_blocks, struct spdk_bdev *bdev, uint32_t remapped_offset)
{
	struct spdk_dif_ctx dif_ctx;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct spdk_dif_error errblk;
	spdk_dif_type_t dif_type;
	bool md_interleaved;
	struct iovec md_iov;

	dif_type = spdk_bdev_get_dif_type(bdev);
	md_interleaved = spdk_bdev_is_md_interleaved(bdev);

	if (dif_type == SPDK_DIF_DISABLE) {
		return;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       md_interleaved,
			       spdk_bdev_is_dif_head_of_md(bdev),
			       dif_type,
			       bdev->dif_check_flags,
			       0,
			       0xFFFF, 0x123, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	if (!md_interleaved) {
		md_iov.iov_base = md_buf;
		md_iov.iov_len	= spdk_bdev_get_md_size(bdev) * num_blocks;

		spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);

		rc = spdk_dix_remap_ref_tag(&md_iov, num_blocks, &dif_ctx, &errblk, false);
		SPDK_CU_ASSERT_FATAL(rc == 0);
	}
}

/* Store the IO completion status in global variable to verify by various tests */
void
raid_test_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	g_io_comp_status = ((status == SPDK_BDEV_IO_STATUS_SUCCESS) ? true : false);
}

int
raid_bdev_remap_dix_reftag(void *md_buf, uint64_t num_blocks,
			   struct spdk_bdev *bdev, uint32_t remapped_offset)
{
	remap_dif(md_buf, num_blocks, bdev, remapped_offset);

	return 0;
}

int
raid_bdev_verify_dix_reftag(struct iovec *iovs, int iovcnt, void *md_buf,
			    uint64_t num_blocks, struct spdk_bdev *bdev, uint32_t offset_blocks)
{
	verify_dif(iovs, iovcnt, md_buf, offset_blocks, num_blocks, bdev);

	return 0;
}

static void
set_io_output(struct io_output *output,
	      struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	      uint64_t offset_blocks, uint64_t num_blocks,
	      spdk_bdev_io_completion_cb cb, void *cb_arg,
	      enum spdk_bdev_io_type iotype, struct iovec *iovs,
	      int iovcnt, void *md)
{
	output->desc = desc;
	output->ch = ch;
	output->offset_blocks = offset_blocks;
	output->num_blocks = num_blocks;
	output->cb = cb;
	output->cb_arg = cb_arg;
	output->iotype = iotype;
	output->iovs = iovs;
	output->iovcnt = iovcnt;
	output->md_buf = md;
}

static struct spdk_bdev_io *
get_child_io(struct io_output *output)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);

	bdev_io->bdev = spdk_bdev_desc_get_bdev(output->desc);
	bdev_io->type = output->iotype;
	bdev_io->u.bdev.offset_blocks = output->offset_blocks;
	bdev_io->u.bdev.num_blocks = output->num_blocks;
	bdev_io->u.bdev.iovs = output->iovs;
	bdev_io->u.bdev.iovcnt = output->iovcnt;
	bdev_io->u.bdev.md_buf = output->md_buf;

	return bdev_io;
}

static void
child_io_complete(struct spdk_bdev_io *bdev_io, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (g_child_io_status_flag && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		verify_dif(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.md_buf,
			   bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks, bdev_io->bdev);
	}

	cb(bdev_io, g_child_io_status_flag, cb_arg);
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	if (g_max_io_size < g_strip_size) {
		SPDK_CU_ASSERT_FATAL(g_io_output_index < 2);
	} else {
		SPDK_CU_ASSERT_FATAL(g_io_output_index < (g_max_io_size / g_strip_size) + 1);
	}
	set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
		      SPDK_BDEV_IO_TYPE_WRITE, iov, iovcnt, opts->metadata);
	g_io_output_index++;

	child_io = get_child_io(output);
	child_io_complete(child_io, cb, cb_arg);

	return 0;
}

int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
		      SPDK_BDEV_IO_TYPE_UNMAP, NULL, 0, NULL);
	g_io_output_index++;

	child_io = get_child_io(output);
	child_io_complete(child_io, cb, cb_arg);

	return 0;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	if (bdev_io) {
		free(bdev_io);
	}
}

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	struct io_output *output = &g_io_output[g_io_output_index];
	struct spdk_bdev_io *child_io;

	SPDK_CU_ASSERT_FATAL(g_io_output_index <= (g_max_io_size / g_strip_size) + 1);
	set_io_output(output, desc, ch, offset_blocks, num_blocks, cb, cb_arg,
		      SPDK_BDEV_IO_TYPE_READ, iov, iovcnt, opts->metadata);
	generate_dif(iov, iovcnt, opts->metadata, offset_blocks, num_blocks,
		     spdk_bdev_desc_get_bdev(desc));
	g_io_output_index++;

	child_io = get_child_io(output);
	child_io_complete(child_io, cb, cb_arg);

	return 0;
}

static void
raid_io_cleanup(struct raid_bdev_io *raid_io)
{
	if (raid_io->iovs) {
		int i;

		for (i = 0; i < raid_io->iovcnt; i++) {
			free(raid_io->iovs[i].iov_base);
		}
		free(raid_io->iovs);
	}

	free(raid_io->md_buf);
	free(raid_io);
}

static void
raid_io_initialize(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		   struct raid_bdev *raid_bdev, uint64_t lba, uint64_t blocks, int16_t iotype)
{
	struct iovec *iovs = NULL;
	int iovcnt = 0;
	void *md_buf = NULL;

	if (iotype != SPDK_BDEV_IO_TYPE_UNMAP && iotype != SPDK_BDEV_IO_TYPE_FLUSH) {
		iovcnt = 1;
		iovs = calloc(iovcnt, sizeof(struct iovec));
		SPDK_CU_ASSERT_FATAL(iovs != NULL);
		iovs->iov_len = blocks * g_block_len;
		iovs->iov_base = calloc(1, iovs->iov_len);
		SPDK_CU_ASSERT_FATAL(iovs->iov_base != NULL);

		if (spdk_bdev_is_md_separate(&raid_bdev->bdev)) {
			md_buf = calloc(1, blocks * spdk_bdev_get_md_size(&raid_bdev->bdev));
			SPDK_CU_ASSERT_FATAL(md_buf != NULL);
		}
	}

	raid_test_bdev_io_init(raid_io, raid_bdev, raid_ch, iotype, lba, blocks, iovs, iovcnt, md_buf);
}

static void
verify_io(struct raid_bdev_io *raid_io, uint32_t io_status)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t num_base_drives = raid_bdev->num_base_bdevs;
	uint32_t strip_shift = spdk_u32log2(g_strip_size);
	uint64_t start_strip = raid_io->offset_blocks >> strip_shift;
	uint64_t end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
			     strip_shift;
	uint32_t splits_reqd = (end_strip - start_strip + 1);
	uint32_t strip;
	uint64_t pd_strip;
	uint8_t pd_idx;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint64_t pd_blocks;
	uint32_t index = 0;
	struct io_output *output;

	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);
	SPDK_CU_ASSERT_FATAL(num_base_drives != 0);

	CU_ASSERT(splits_reqd == g_io_output_index);
	for (strip = start_strip; strip <= end_strip; strip++, index++) {
		pd_strip = strip / num_base_drives;
		pd_idx = strip % num_base_drives;
		if (strip == start_strip) {
			offset_in_strip = raid_io->offset_blocks & (g_strip_size - 1);
			pd_lba = (pd_strip << strip_shift) + offset_in_strip;
			if (strip == end_strip) {
				pd_blocks = raid_io->num_blocks;
			} else {
				pd_blocks = g_strip_size - offset_in_strip;
			}
		} else if (strip == end_strip) {
			pd_lba = pd_strip << strip_shift;
			pd_blocks = ((raid_io->offset_blocks + raid_io->num_blocks - 1) &
				     (g_strip_size - 1)) + 1;
		} else {
			pd_lba = pd_strip << raid_bdev->strip_size_shift;
			pd_blocks = raid_bdev->strip_size;
		}
		output = &g_io_output[index];
		CU_ASSERT(pd_lba == output->offset_blocks);
		CU_ASSERT(pd_blocks == output->num_blocks);
		CU_ASSERT(raid_bdev_channel_get_base_channel(raid_io->raid_ch, pd_idx) == output->ch);
		CU_ASSERT(raid_bdev->base_bdev_info[pd_idx].desc == output->desc);
		CU_ASSERT(raid_io->type == output->iotype);
		if (raid_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			verify_dif(output->iovs, output->iovcnt, output->md_buf,
				   output->offset_blocks, output->num_blocks,
				   spdk_bdev_desc_get_bdev(raid_bdev->base_bdev_info[pd_idx].desc));
		}
	}
	CU_ASSERT(g_io_comp_status == io_status);
}

static void
verify_io_without_payload(struct raid_bdev_io *raid_io, uint32_t io_status)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t num_base_drives = raid_bdev->num_base_bdevs;
	uint32_t strip_shift = spdk_u32log2(g_strip_size);
	uint64_t start_offset_in_strip = raid_io->offset_blocks % g_strip_size;
	uint64_t end_offset_in_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) %
				       g_strip_size;
	uint64_t start_strip = raid_io->offset_blocks >> strip_shift;
	uint64_t end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
			     strip_shift;
	uint8_t n_disks_involved;
	uint64_t start_strip_disk_idx;
	uint64_t end_strip_disk_idx;
	uint64_t nblocks_in_start_disk;
	uint64_t offset_in_start_disk;
	uint8_t disk_idx;
	uint64_t base_io_idx;
	uint64_t sum_nblocks = 0;
	struct io_output *output;

	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);
	SPDK_CU_ASSERT_FATAL(num_base_drives != 0);
	SPDK_CU_ASSERT_FATAL(raid_io->type != SPDK_BDEV_IO_TYPE_READ);
	SPDK_CU_ASSERT_FATAL(raid_io->type != SPDK_BDEV_IO_TYPE_WRITE);

	n_disks_involved = spdk_min(end_strip - start_strip + 1, num_base_drives);
	CU_ASSERT(n_disks_involved == g_io_output_index);

	start_strip_disk_idx = start_strip % num_base_drives;
	end_strip_disk_idx = end_strip % num_base_drives;

	offset_in_start_disk = g_io_output[0].offset_blocks;
	nblocks_in_start_disk = g_io_output[0].num_blocks;

	for (base_io_idx = 0, disk_idx = start_strip_disk_idx; base_io_idx < n_disks_involved;
	     base_io_idx++, disk_idx++) {
		uint64_t start_offset_in_disk;
		uint64_t end_offset_in_disk;

		output = &g_io_output[base_io_idx];

		/* round disk_idx */
		if (disk_idx >= num_base_drives) {
			disk_idx %= num_base_drives;
		}

		/* start_offset_in_disk aligned in strip check:
		 * The first base io has a same start_offset_in_strip with the whole raid io.
		 * Other base io should have aligned start_offset_in_strip which is 0.
		 */
		start_offset_in_disk = output->offset_blocks;
		if (base_io_idx == 0) {
			CU_ASSERT(start_offset_in_disk % g_strip_size == start_offset_in_strip);
		} else {
			CU_ASSERT(start_offset_in_disk % g_strip_size == 0);
		}

		/* end_offset_in_disk aligned in strip check:
		 * Base io on disk at which end_strip is located, has a same end_offset_in_strip
		 * with the whole raid io.
		 * Other base io should have aligned end_offset_in_strip.
		 */
		end_offset_in_disk = output->offset_blocks + output->num_blocks - 1;
		if (disk_idx == end_strip_disk_idx) {
			CU_ASSERT(end_offset_in_disk % g_strip_size == end_offset_in_strip);
		} else {
			CU_ASSERT(end_offset_in_disk % g_strip_size == g_strip_size - 1);
		}

		/* start_offset_in_disk compared with start_disk.
		 * 1. For disk_idx which is larger than start_strip_disk_idx: Its start_offset_in_disk
		 *    mustn't be larger than the start offset of start_offset_in_disk; And the gap
		 *    must be less than strip size.
		 * 2. For disk_idx which is less than start_strip_disk_idx, Its start_offset_in_disk
		 *    must be larger than the start offset of start_offset_in_disk; And the gap mustn't
		 *    be less than strip size.
		 */
		if (disk_idx > start_strip_disk_idx) {
			CU_ASSERT(start_offset_in_disk <= offset_in_start_disk);
			CU_ASSERT(offset_in_start_disk - start_offset_in_disk < g_strip_size);
		} else if (disk_idx < start_strip_disk_idx) {
			CU_ASSERT(start_offset_in_disk > offset_in_start_disk);
			CU_ASSERT(output->offset_blocks - offset_in_start_disk <= g_strip_size);
		}

		/* nblocks compared with start_disk:
		 * The gap between them must be within a strip size.
		 */
		if (output->num_blocks <= nblocks_in_start_disk) {
			CU_ASSERT(nblocks_in_start_disk - output->num_blocks <= g_strip_size);
		} else {
			CU_ASSERT(output->num_blocks - nblocks_in_start_disk < g_strip_size);
		}

		sum_nblocks += output->num_blocks;

		CU_ASSERT(raid_bdev_channel_get_base_channel(raid_io->raid_ch, disk_idx) == output->ch);
		CU_ASSERT(raid_bdev->base_bdev_info[disk_idx].desc == output->desc);
		CU_ASSERT(raid_io->type == output->iotype);
	}

	/* Sum of each nblocks should be same with raid bdev_io */
	CU_ASSERT(raid_io->num_blocks == sum_nblocks);

	CU_ASSERT(g_io_comp_status == io_status);
}

static struct raid_bdev *
create_raid0(void)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;
	struct raid_params params = {
		.num_base_bdevs = g_max_base_drives,
		.base_bdev_blockcnt = BLOCK_CNT,
		.base_bdev_blocklen = g_block_len,
		.strip_size = g_strip_size,
		.md_type = g_enable_dif ? RAID_PARAMS_MD_SEPARATE : RAID_PARAMS_MD_NONE,
	};

	raid_bdev = raid_test_create_raid_bdev(&params, &g_raid0_module);

	SPDK_CU_ASSERT_FATAL(raid0_start(raid_bdev) == 0);

	if (g_enable_dif) {
		raid_bdev->bdev.dif_type = SPDK_DIF_TYPE1;
		raid_bdev->bdev.dif_check_flags =
			SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK |
			SPDK_DIF_FLAGS_APPTAG_CHECK;

		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			bdev->dif_type = raid_bdev->bdev.dif_type;
			bdev->dif_check_flags = raid_bdev->bdev.dif_check_flags;
		}
	}

	return raid_bdev;
}

static void
delete_raid0(struct raid_bdev *raid_bdev)
{
	raid_test_delete_raid_bdev(raid_bdev);
}

static void
test_write_io(void)
{
	struct raid_bdev *raid_bdev;
	uint8_t i;
	uint64_t io_len;
	uint64_t lba = 0;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;

	set_globals();

	raid_bdev = create_raid0();
	raid_ch = raid_test_create_io_channel(raid_bdev);

	/* test 2 IO sizes based on global strip size set earlier */
	for (i = 0; i < 2; i++) {
		raid_io = calloc(1, sizeof(*raid_io));
		SPDK_CU_ASSERT_FATAL(raid_io != NULL);
		io_len = (g_strip_size / 2) << i;
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		generate_dif(raid_io->iovs, raid_io->iovcnt, raid_io->md_buf,
			     raid_io->offset_blocks, raid_io->num_blocks, &raid_bdev->bdev);
		raid0_submit_rw_request(raid_io);
		verify_io(raid_io, g_child_io_status_flag);
		raid_io_cleanup(raid_io);
	}

	raid_test_destroy_io_channel(raid_ch);
	delete_raid0(raid_bdev);

	reset_globals();
}

static void
test_read_io(void)
{
	struct raid_bdev *raid_bdev;
	uint8_t i;
	uint64_t io_len;
	uint64_t lba = 0;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;

	set_globals();

	raid_bdev = create_raid0();
	raid_ch = raid_test_create_io_channel(raid_bdev);

	/* test 2 IO sizes based on global strip size set earlier */
	lba = 0;
	for (i = 0; i < 2; i++) {
		raid_io = calloc(1, sizeof(*raid_io));
		SPDK_CU_ASSERT_FATAL(raid_io != NULL);
		io_len = (g_strip_size / 2) << i;
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, io_len, SPDK_BDEV_IO_TYPE_READ);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		raid0_submit_rw_request(raid_io);
		verify_io(raid_io, g_child_io_status_flag);
		raid_io_cleanup(raid_io);
	}

	raid_test_destroy_io_channel(raid_ch);
	delete_raid0(raid_bdev);

	reset_globals();
}

static void
raid_bdev_io_generate_by_strips(uint64_t n_strips)
{
	uint64_t lba;
	uint64_t nblocks;
	uint64_t start_offset;
	uint64_t end_offset;
	uint64_t offsets_in_strip[3];
	uint64_t start_bdev_idx;
	uint64_t start_bdev_offset;
	uint64_t start_bdev_idxs[3];
	int i, j, l;

	/* 3 different situations of offset in strip */
	offsets_in_strip[0] = 0;
	offsets_in_strip[1] = g_strip_size >> 1;
	offsets_in_strip[2] = g_strip_size - 1;

	/* 3 different situations of start_bdev_idx */
	start_bdev_idxs[0] = 0;
	start_bdev_idxs[1] = g_max_base_drives >> 1;
	start_bdev_idxs[2] = g_max_base_drives - 1;

	/* consider different offset in strip */
	for (i = 0; i < 3; i++) {
		start_offset = offsets_in_strip[i];
		for (j = 0; j < 3; j++) {
			end_offset = offsets_in_strip[j];
			if (n_strips == 1 && start_offset > end_offset) {
				continue;
			}

			/* consider at which base_bdev lba is started. */
			for (l = 0; l < 3; l++) {
				start_bdev_idx = start_bdev_idxs[l];
				start_bdev_offset = start_bdev_idx * g_strip_size;
				lba = start_bdev_offset + start_offset;
				nblocks = (n_strips - 1) * g_strip_size + end_offset - start_offset + 1;

				g_io_ranges[g_io_range_idx].lba = lba;
				g_io_ranges[g_io_range_idx].nblocks = nblocks;

				SPDK_CU_ASSERT_FATAL(g_io_range_idx < MAX_TEST_IO_RANGE);
				g_io_range_idx++;
			}
		}
	}
}

static void
raid_bdev_io_generate(void)
{
	uint64_t n_strips;
	uint64_t n_strips_span = g_max_base_drives;
	uint64_t n_strips_times[5] = {g_max_base_drives + 1, g_max_base_drives * 2 - 1,
				      g_max_base_drives * 2, g_max_base_drives * 3,
				      g_max_base_drives * 4
				     };
	uint32_t i;

	g_io_range_idx = 0;

	/* consider different number of strips from 1 to strips spanned base bdevs,
	 * and even to times of strips spanned base bdevs
	 */
	for (n_strips = 1; n_strips < n_strips_span; n_strips++) {
		raid_bdev_io_generate_by_strips(n_strips);
	}

	for (i = 0; i < SPDK_COUNTOF(n_strips_times); i++) {
		n_strips = n_strips_times[i];
		raid_bdev_io_generate_by_strips(n_strips);
	}
}

static void
test_unmap_io(void)
{
	struct raid_bdev *raid_bdev;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;

	set_globals();

	raid_bdev = create_raid0();
	raid_ch = raid_test_create_io_channel(raid_bdev);

	raid_bdev_io_generate();
	for (count = 0; count < g_io_range_idx; count++) {
		raid_io = calloc(1, sizeof(*raid_io));
		SPDK_CU_ASSERT_FATAL(raid_io != NULL);
		io_len = g_io_ranges[count].nblocks;
		lba = g_io_ranges[count].lba;
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, io_len, SPDK_BDEV_IO_TYPE_UNMAP);
		memset(g_io_output, 0, g_max_base_drives * sizeof(struct io_output));
		g_io_output_index = 0;
		raid0_submit_null_payload_request(raid_io);
		verify_io_without_payload(raid_io, g_child_io_status_flag);
		raid_io_cleanup(raid_io);
	}

	raid_test_destroy_io_channel(raid_ch);
	delete_raid0(raid_bdev);

	reset_globals();
}

/* Test IO failures */
static void
test_io_failure(void)
{
	struct raid_bdev *raid_bdev;
	uint32_t count;
	uint64_t io_len;
	uint64_t lba;
	struct raid_bdev_io *raid_io;
	struct raid_bdev_io_channel *raid_ch;

	set_globals();

	raid_bdev = create_raid0();
	raid_ch = raid_test_create_io_channel(raid_bdev);

	lba = 0;
	g_child_io_status_flag = false;
	for (count = 0; count < 1; count++) {
		raid_io = calloc(1, sizeof(*raid_io));
		SPDK_CU_ASSERT_FATAL(raid_io != NULL);
		io_len = (g_strip_size / 2) << count;
		raid_io_initialize(raid_io, raid_ch, raid_bdev, lba, io_len, SPDK_BDEV_IO_TYPE_WRITE);
		lba += g_strip_size;
		memset(g_io_output, 0, ((g_max_io_size / g_strip_size) + 1) * sizeof(struct io_output));
		g_io_output_index = 0;
		generate_dif(raid_io->iovs, raid_io->iovcnt, raid_io->md_buf,
			     raid_io->offset_blocks, raid_io->num_blocks, &raid_bdev->bdev);
		raid0_submit_rw_request(raid_io);
		verify_io(raid_io, g_child_io_status_flag);
		raid_io_cleanup(raid_io);
	}

	raid_test_destroy_io_channel(raid_ch);
	delete_raid0(raid_bdev);

	reset_globals();
}

int
main(int argc, char **argv)
{
	unsigned int    num_failures;

	CU_TestInfo tests[] = {
		{ "test_write_io", test_write_io },
		{ "test_read_io", test_read_io },
		{ "test_unmap_io", test_unmap_io },
		{ "test_io_failure", test_io_failure },
		CU_TEST_INFO_NULL,
	};
	CU_SuiteInfo suites[] = {
		{ "raid0", set_test_opts, NULL, NULL, NULL, tests },
		{ "raid0_dif", set_test_opts_dif, NULL, NULL, NULL, tests },
		CU_SUITE_INFO_NULL,
	};

	CU_initialize_registry();
	CU_register_suites(suites);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
