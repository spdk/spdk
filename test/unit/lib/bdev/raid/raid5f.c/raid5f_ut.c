/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid5f.c"
#include "../common.c"

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(raid_bdev_module_stop_done, (struct raid_bdev *raid_bdev));

void *
spdk_bdev_io_get_md_buf(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.md_buf;
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);

	if (bdev_io->internal.cb) {
		bdev_io->internal.cb(bdev_io, status == SPDK_BDEV_IO_STATUS_SUCCESS, bdev_io->internal.caller_ctx);
	}
}

bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 3, 4, 5 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint32_t strip_size_kb_values[] = { 1, 4, 128 };
	uint32_t md_len_values[] = { 0, 64 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	uint32_t *strip_size_kb;
	uint32_t *md_len;
	struct raid_params params;
	uint64_t params_count;
	int rc;

	params_count = SPDK_COUNTOF(num_base_bdevs_values) *
		       SPDK_COUNTOF(base_bdev_blockcnt_values) *
		       SPDK_COUNTOF(base_bdev_blocklen_values) *
		       SPDK_COUNTOF(strip_size_kb_values) *
		       SPDK_COUNTOF(md_len_values);
	rc = raid_test_params_alloc(params_count);
	if (rc) {
		return rc;
	}

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				ARRAY_FOR_EACH(strip_size_kb_values, strip_size_kb) {
					ARRAY_FOR_EACH(md_len_values, md_len) {
						params.num_base_bdevs = *num_base_bdevs;
						params.base_bdev_blockcnt = *base_bdev_blockcnt;
						params.base_bdev_blocklen = *base_bdev_blocklen;
						params.strip_size = *strip_size_kb * 1024 / *base_bdev_blocklen;
						params.md_len = *md_len;
						if (params.strip_size == 0 ||
						    params.strip_size > *base_bdev_blockcnt) {
							continue;
						}
						raid_test_params_add(&params);
					}
				}
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	raid_test_params_free();
	return 0;
}

static struct raid5f_info *
create_raid5f(struct raid_params *params)
{
	struct raid_bdev *raid_bdev = raid_test_create_raid_bdev(params, &g_raid5f_module);

	SPDK_CU_ASSERT_FATAL(raid5f_start(raid_bdev) == 0);

	return raid_bdev->module_private;
}

static void
delete_raid5f(struct raid5f_info *r5f_info)
{
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;

	raid5f_stop(raid_bdev);

	raid_test_delete_raid_bdev(raid_bdev);
}

static void
test_raid5f_start(void)
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid5f_info *r5f_info;

		r5f_info = create_raid5f(params);

		SPDK_CU_ASSERT_FATAL(r5f_info != NULL);

		CU_ASSERT_EQUAL(r5f_info->stripe_blocks, params->strip_size * (params->num_base_bdevs - 1));
		CU_ASSERT_EQUAL(r5f_info->total_stripes, params->base_bdev_blockcnt / params->strip_size);
		CU_ASSERT_EQUAL(r5f_info->raid_bdev->bdev.blockcnt,
				(params->base_bdev_blockcnt - params->base_bdev_blockcnt % params->strip_size) *
				(params->num_base_bdevs - 1));
		CU_ASSERT_EQUAL(r5f_info->raid_bdev->bdev.optimal_io_boundary, params->strip_size);
		CU_ASSERT_TRUE(r5f_info->raid_bdev->bdev.split_on_optimal_io_boundary);
		CU_ASSERT_EQUAL(r5f_info->raid_bdev->bdev.write_unit_size, r5f_info->stripe_blocks);

		delete_raid5f(r5f_info);
	}
}

enum test_bdev_error_type {
	TEST_BDEV_ERROR_NONE,
	TEST_BDEV_ERROR_SUBMIT,
	TEST_BDEV_ERROR_COMPLETE,
	TEST_BDEV_ERROR_NOMEM,
};

struct raid_io_info {
	struct raid5f_info *r5f_info;
	struct raid_bdev_io_channel *raid_ch;
	enum spdk_bdev_io_type io_type;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	void *src_buf;
	void *dest_buf;
	void *src_md_buf;
	void *dest_md_buf;
	size_t buf_size;
	void *parity_buf;
	void *reference_parity;
	size_t parity_buf_size;
	void *parity_md_buf;
	void *reference_md_parity;
	size_t parity_md_buf_size;
	enum spdk_bdev_io_status status;
	bool failed;
	int remaining;
	TAILQ_HEAD(, spdk_bdev_io) bdev_io_queue;
	TAILQ_HEAD(, spdk_bdev_io_wait_entry) bdev_io_wait_queue;
	struct {
		enum test_bdev_error_type type;
		struct spdk_bdev *bdev;
		void (*on_enomem_cb)(struct raid_io_info *io_info, void *ctx);
		void *on_enomem_cb_ctx;
	} error;
};

struct test_raid_bdev_io {
	char bdev_io_buf[sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io)];
	struct raid_io_info *io_info;
	void *buf;
	void *buf_md;
};

void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	struct raid_io_info *io_info;

	io_info = ((struct test_raid_bdev_io *)spdk_bdev_io_from_ctx(raid_io))->io_info;

	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	TAILQ_INSERT_TAIL(&io_info->bdev_io_wait_queue, &raid_io->waitq_entry, link);
}

static void
raid_bdev_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_io_info *io_info = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		io_info->failed = true;
	}

	if (--io_info->remaining == 0) {
		if (io_info->failed) {
			io_info->status = SPDK_BDEV_IO_STATUS_FAILED;
		} else {
			io_info->status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}
	}
}

static struct raid_bdev_io *
get_raid_io(struct raid_io_info *io_info, uint64_t offset_blocks_split, uint64_t num_blocks)
{
	struct spdk_bdev_io *bdev_io;
	struct raid_bdev_io *raid_io;
	struct raid_bdev *raid_bdev = io_info->r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	struct test_raid_bdev_io *test_raid_bdev_io;
	void *src_buf = io_info->src_buf + offset_blocks_split * blocklen;
	void *dest_buf = io_info->dest_buf + offset_blocks_split * blocklen;

	void *src_md_buf = io_info->src_md_buf + offset_blocks_split * raid_bdev->bdev.md_len;
	void *dest_md_buf = io_info->dest_md_buf + offset_blocks_split * raid_bdev->bdev.md_len;

	test_raid_bdev_io = calloc(1, sizeof(*test_raid_bdev_io));
	SPDK_CU_ASSERT_FATAL(test_raid_bdev_io != NULL);

	SPDK_CU_ASSERT_FATAL(test_raid_bdev_io->bdev_io_buf == (char *)test_raid_bdev_io);
	bdev_io = (struct spdk_bdev_io *)test_raid_bdev_io->bdev_io_buf;
	bdev_io->bdev = &raid_bdev->bdev;
	bdev_io->type = io_info->io_type;
	bdev_io->u.bdev.offset_blocks = io_info->offset_blocks + offset_blocks_split;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->internal.cb = raid_bdev_io_completion_cb;
	bdev_io->internal.caller_ctx = io_info;

	raid_io = (void *)bdev_io->driver_ctx;
	raid_io->raid_bdev = raid_bdev;
	raid_io->raid_ch = io_info->raid_ch;
	raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	test_raid_bdev_io->io_info = io_info;

	if (io_info->io_type == SPDK_BDEV_IO_TYPE_READ) {
		test_raid_bdev_io->buf = src_buf;
		test_raid_bdev_io->buf_md = src_md_buf;
		bdev_io->u.bdev.md_buf = dest_md_buf;
		bdev_io->iov.iov_base = dest_buf;
	} else {
		test_raid_bdev_io->buf = dest_buf;
		test_raid_bdev_io->buf_md = dest_md_buf;
		bdev_io->u.bdev.md_buf = src_md_buf;
		bdev_io->iov.iov_base = src_buf;
	}

	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->iov.iov_len = num_blocks * blocklen;

	io_info->remaining++;

	return raid_io;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	free(bdev_io);
}

static int
submit_io(struct raid_io_info *io_info, struct spdk_bdev_desc *desc,
	  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = desc->bdev;
	struct spdk_bdev_io *bdev_io;

	if (bdev == io_info->error.bdev) {
		if (io_info->error.type == TEST_BDEV_ERROR_SUBMIT) {
			return -EINVAL;
		} else if (io_info->error.type == TEST_BDEV_ERROR_NOMEM) {
			return -ENOMEM;
		}
	}

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->bdev = bdev;
	bdev_io->internal.cb = cb;
	bdev_io->internal.caller_ctx = cb_arg;

	TAILQ_INSERT_TAIL(&io_info->bdev_io_queue, bdev_io, internal.link);

	return 0;
}

static void
process_io_completions(struct raid_io_info *io_info)
{
	struct spdk_bdev_io *bdev_io;
	bool success;

	while ((bdev_io = TAILQ_FIRST(&io_info->bdev_io_queue))) {
		TAILQ_REMOVE(&io_info->bdev_io_queue, bdev_io, internal.link);

		if (io_info->error.type == TEST_BDEV_ERROR_COMPLETE &&
		    io_info->error.bdev == bdev_io->bdev) {
			success = false;
		} else {
			success = true;
		}

		bdev_io->internal.cb(bdev_io, success, bdev_io->internal.caller_ctx);
	}

	if (io_info->error.type == TEST_BDEV_ERROR_NOMEM) {
		struct spdk_bdev_io_wait_entry *waitq_entry, *tmp;
		struct spdk_bdev *enomem_bdev = io_info->error.bdev;

		io_info->error.type = TEST_BDEV_ERROR_NONE;

		if (io_info->error.on_enomem_cb != NULL) {
			io_info->error.on_enomem_cb(io_info, io_info->error.on_enomem_cb_ctx);
		}

		TAILQ_FOREACH_SAFE(waitq_entry, &io_info->bdev_io_wait_queue, link, tmp) {
			TAILQ_REMOVE(&io_info->bdev_io_wait_queue, waitq_entry, link);
			CU_ASSERT(waitq_entry->bdev == enomem_bdev);
			waitq_entry->cb_fn(waitq_entry->cb_arg);
		}

		process_io_completions(io_info);
	} else {
		CU_ASSERT(TAILQ_EMPTY(&io_info->bdev_io_wait_queue));
	}
}

#define DATA_OFFSET_TO_MD_OFFSET(raid_bdev, data_offset) ((data_offset >> raid_bdev->blocklen_shift) * raid_bdev->bdev.md_len)

int
spdk_bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, void *md_buf,
				uint64_t offset_blocks, uint64_t num_blocks,
				spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct chunk *chunk = cb_arg;
	struct stripe_request *stripe_req;
	struct test_raid_bdev_io *test_raid_bdev_io;
	struct raid_io_info *io_info;
	struct raid_bdev *raid_bdev;
	uint64_t stripe_idx_off;
	uint8_t data_chunk_idx;
	uint64_t data_offset;
	void *dest_buf, *dest_md_buf;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_write_complete_bdev_io);
	SPDK_CU_ASSERT_FATAL(iovcnt == 1);

	stripe_req = raid5f_chunk_stripe_req(chunk);
	test_raid_bdev_io = (struct test_raid_bdev_io *)spdk_bdev_io_from_ctx(stripe_req->raid_io);
	io_info = test_raid_bdev_io->io_info;

	raid_bdev = io_info->r5f_info->raid_bdev;

	stripe_idx_off = offset_blocks / raid_bdev->strip_size -
			 io_info->offset_blocks / io_info->r5f_info->stripe_blocks;

	if (chunk == stripe_req->parity_chunk) {
		if (io_info->parity_buf == NULL) {
			goto submit;
		}
		data_offset = stripe_idx_off * raid_bdev->strip_size_kb * 1024;
		dest_buf = io_info->parity_buf + data_offset;

		if (md_buf != NULL) {
			data_offset = DATA_OFFSET_TO_MD_OFFSET(raid_bdev, data_offset);
			dest_md_buf = io_info->parity_md_buf + data_offset;
		}
	} else {
		data_chunk_idx = chunk < stripe_req->parity_chunk ? chunk->index : chunk->index - 1;
		data_offset = (stripe_idx_off * io_info->r5f_info->stripe_blocks +
			       data_chunk_idx * raid_bdev->strip_size) *
			      raid_bdev->bdev.blocklen;
		dest_buf = test_raid_bdev_io->buf + data_offset;

		if (md_buf != NULL) {
			data_offset = DATA_OFFSET_TO_MD_OFFSET(raid_bdev, data_offset);
			dest_md_buf = test_raid_bdev_io->buf_md + data_offset;
		}
	}

	memcpy(dest_buf, iov->iov_base, iov->iov_len);
	if (md_buf != NULL) {
		memcpy(dest_md_buf, md_buf, DATA_OFFSET_TO_MD_OFFSET(raid_bdev, iov->iov_len));
	}

submit:
	return submit_io(io_info, desc, cb, cb_arg);
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks, num_blocks, cb,
					       cb_arg);
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			    uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	CU_ASSERT_PTR_NULL(opts->memory_domain);
	CU_ASSERT_PTR_NULL(opts->memory_domain_ctx);

	return spdk_bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, opts->metadata, offset_blocks,
					       num_blocks, cb, cb_arg);
}

int
spdk_bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, void *md_buf,
			       uint64_t offset_blocks, uint64_t num_blocks,
			       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;
	struct test_raid_bdev_io *test_raid_bdev_io;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_read_complete);
	SPDK_CU_ASSERT_FATAL(iovcnt == 1);

	test_raid_bdev_io = (struct test_raid_bdev_io *)spdk_bdev_io_from_ctx(raid_io);

	memcpy(iov->iov_base, test_raid_bdev_io->buf, iov->iov_len);
	if (md_buf != NULL) {
		memcpy(md_buf, test_raid_bdev_io->buf_md, DATA_OFFSET_TO_MD_OFFSET(raid_io->raid_bdev,
				iov->iov_len));
	}

	return submit_io(test_raid_bdev_io->io_info, desc, cb, cb_arg);
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, NULL, offset_blocks, num_blocks, cb,
					      cb_arg);
}

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			   uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			   struct spdk_bdev_ext_io_opts *opts)
{
	CU_ASSERT_PTR_NULL(opts->memory_domain);
	CU_ASSERT_PTR_NULL(opts->memory_domain_ctx);

	return spdk_bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, opts->metadata, offset_blocks,
					      num_blocks, cb, cb_arg);
}

static void
xor_block(uint8_t *a, uint8_t *b, size_t size)
{
	while (size-- > 0) {
		a[size] ^= b[size];
	}
}

static void
test_raid5f_write_request(struct raid_io_info *io_info)
{
	struct raid_bdev_io *raid_io;

	SPDK_CU_ASSERT_FATAL(io_info->num_blocks / io_info->r5f_info->stripe_blocks == 1);

	raid_io = get_raid_io(io_info, 0, io_info->num_blocks);

	raid5f_submit_rw_request(raid_io);

	process_io_completions(io_info);

	if (io_info->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		if (io_info->parity_buf) {
			CU_ASSERT(memcmp(io_info->parity_buf, io_info->reference_parity,
					 io_info->parity_buf_size) == 0);
		}
		if (io_info->parity_md_buf) {
			CU_ASSERT(memcmp(io_info->parity_md_buf, io_info->reference_md_parity,
					 io_info->parity_md_buf_size) == 0);
		}
	}
}

static void
test_raid5f_read_request(struct raid_io_info *io_info)
{
	uint32_t strip_size = io_info->r5f_info->raid_bdev->strip_size;
	uint64_t num_blocks = io_info->num_blocks;
	uint64_t offset_blocks_split = 0;

	while (num_blocks) {
		uint64_t chunk_offset = offset_blocks_split % strip_size;
		uint64_t num_blocks_split = spdk_min(num_blocks, strip_size - chunk_offset);
		struct raid_bdev_io *raid_io;

		raid_io = get_raid_io(io_info, offset_blocks_split, num_blocks_split);

		raid5f_submit_rw_request(raid_io);

		num_blocks -= num_blocks_split;
		offset_blocks_split += num_blocks_split;
	}

	process_io_completions(io_info);
}

static void
deinit_io_info(struct raid_io_info *io_info)
{
	free(io_info->src_buf);
	free(io_info->dest_buf);
	free(io_info->src_md_buf);
	free(io_info->dest_md_buf);
	free(io_info->parity_buf);
	free(io_info->reference_parity);
	free(io_info->parity_md_buf);
	free(io_info->reference_md_parity);
}

static void
init_io_info(struct raid_io_info *io_info, struct raid5f_info *r5f_info,
	     struct raid_bdev_io_channel *raid_ch, enum spdk_bdev_io_type io_type,
	     uint64_t offset_blocks, uint64_t num_blocks)
{
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	void *src_buf, *dest_buf;
	void *src_md_buf, *dest_md_buf;
	size_t buf_size = num_blocks * blocklen;
	size_t buf_md_size = num_blocks * raid_bdev->bdev.md_len;
	uint64_t block;
	uint64_t i;

	memset(io_info, 0, sizeof(*io_info));

	if (buf_size) {
		src_buf = spdk_dma_malloc(buf_size, 4096, NULL);
		SPDK_CU_ASSERT_FATAL(src_buf != NULL);

		dest_buf = spdk_dma_malloc(buf_size, 4096, NULL);
		SPDK_CU_ASSERT_FATAL(dest_buf != NULL);

		memset(src_buf, 0xff, buf_size);
		for (block = 0; block < num_blocks; block++) {
			*((uint64_t *)(src_buf + block * blocklen)) = block;
		}
	} else {
		src_buf = NULL;
		dest_buf = NULL;
	}

	if (buf_md_size) {
		src_md_buf = spdk_dma_malloc(buf_md_size, 4096, NULL);
		SPDK_CU_ASSERT_FATAL(src_md_buf != NULL);

		dest_md_buf = spdk_dma_malloc(buf_md_size, 4096, NULL);
		SPDK_CU_ASSERT_FATAL(dest_md_buf != NULL);

		memset(src_md_buf, 0xff, buf_md_size);
		for (i = 0; i < buf_md_size; i++) {
			*((uint8_t *)(src_md_buf + i)) = (uint8_t)i;
		}
	} else {
		src_md_buf = NULL;
		dest_md_buf = NULL;
	}

	io_info->r5f_info = r5f_info;
	io_info->raid_ch = raid_ch;
	io_info->io_type = io_type;
	io_info->offset_blocks = offset_blocks;
	io_info->num_blocks = num_blocks;
	io_info->src_buf = src_buf;
	io_info->dest_buf = dest_buf;
	io_info->src_md_buf = src_md_buf;
	io_info->dest_md_buf = dest_md_buf;
	io_info->buf_size = buf_size;
	io_info->status = SPDK_BDEV_IO_STATUS_PENDING;

	TAILQ_INIT(&io_info->bdev_io_queue);
	TAILQ_INIT(&io_info->bdev_io_wait_queue);
}

static void
io_info_setup_parity(struct raid_io_info *io_info)
{
	struct raid5f_info *r5f_info = io_info->r5f_info;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	uint64_t num_stripes = io_info->num_blocks / r5f_info->stripe_blocks;
	size_t strip_len = raid_bdev->strip_size * blocklen;
	size_t strip_md_len = raid_bdev->strip_size * raid_bdev->bdev.md_len;
	void *src = io_info->src_buf;
	void *dest;
	unsigned i, j;

	io_info->parity_buf_size = num_stripes * strip_len;
	io_info->parity_buf = calloc(1, io_info->parity_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->parity_buf != NULL);

	io_info->reference_parity = calloc(1, io_info->parity_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->reference_parity != NULL);

	dest = io_info->reference_parity;
	for (i = 0; i < num_stripes; i++) {
		for (j = 0; j < raid5f_stripe_data_chunks_num(raid_bdev); j++) {
			xor_block(dest, src, strip_len);
			src += strip_len;
		}
		dest += strip_len;
	}

	io_info->parity_md_buf_size = num_stripes * strip_md_len;
	io_info->parity_md_buf = calloc(1, io_info->parity_md_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->parity_md_buf != NULL);

	io_info->reference_md_parity = calloc(1, io_info->parity_md_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->reference_md_parity != NULL);

	src = io_info->src_md_buf;
	dest = io_info->reference_md_parity;
	for (i = 0; i < num_stripes; i++) {
		for (j = 0; j < raid5f_stripe_data_chunks_num(raid_bdev); j++) {
			xor_block(dest, src, strip_md_len);
			src += strip_md_len;
		}
		dest += strip_md_len;
	}
}

static void
test_raid5f_submit_rw_request(struct raid5f_info *r5f_info, struct raid_bdev_io_channel *raid_ch,
			      enum spdk_bdev_io_type io_type, uint64_t stripe_index, uint64_t stripe_offset_blocks,
			      uint64_t num_blocks)
{
	uint64_t offset_blocks = stripe_index * r5f_info->stripe_blocks + stripe_offset_blocks;
	struct raid_io_info io_info;

	init_io_info(&io_info, r5f_info, raid_ch, io_type, offset_blocks, num_blocks);

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		test_raid5f_read_request(&io_info);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_info_setup_parity(&io_info);
		test_raid5f_write_request(&io_info);
		break;
	default:
		CU_FAIL_FATAL("unsupported io_type");
	}

	assert(io_info.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	assert(memcmp(io_info.src_buf, io_info.dest_buf, io_info.buf_size) == 0);

	CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(memcmp(io_info.src_buf, io_info.dest_buf, io_info.buf_size) == 0);

	deinit_io_info(&io_info);
}

static void
run_for_each_raid5f_config(void (*test_fn)(struct raid_bdev *raid_bdev,
			   struct raid_bdev_io_channel *raid_ch))
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid5f_info *r5f_info;
		struct raid_bdev_io_channel raid_ch = { 0 };

		r5f_info = create_raid5f(params);

		raid_ch.num_channels = params->num_base_bdevs;
		raid_ch.base_channel = calloc(params->num_base_bdevs, sizeof(struct spdk_io_channel *));
		SPDK_CU_ASSERT_FATAL(raid_ch.base_channel != NULL);

		raid_ch.module_channel = raid5f_get_io_channel(r5f_info->raid_bdev);
		SPDK_CU_ASSERT_FATAL(raid_ch.module_channel);

		test_fn(r5f_info->raid_bdev, &raid_ch);

		spdk_put_io_channel(raid_ch.module_channel);
		poll_threads();

		free(raid_ch.base_channel);

		delete_raid5f(r5f_info);
	}
}

#define RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, i) \
	for (i = 0; i < spdk_min(raid_bdev->num_base_bdevs, ((struct raid5f_info *)raid_bdev->module_private)->total_stripes); i++)

struct test_request_conf {
	uint64_t stripe_offset_blocks;
	uint64_t num_blocks;
};

static void
__test_raid5f_submit_read_request(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	uint32_t strip_size = raid_bdev->strip_size;
	unsigned int i;

	struct test_request_conf test_requests[] = {
		{ 0, 1 },
		{ 0, strip_size },
		{ 0, strip_size + 1 },
		{ 0, r5f_info->stripe_blocks },
		{ 1, 1 },
		{ 1, strip_size },
		{ 1, strip_size + 1 },
		{ strip_size, 1 },
		{ strip_size, strip_size },
		{ strip_size, strip_size + 1 },
		{ strip_size - 1, 1 },
		{ strip_size - 1, strip_size },
		{ strip_size - 1, strip_size + 1 },
		{ strip_size - 1, 2 },
	};
	for (i = 0; i < SPDK_COUNTOF(test_requests); i++) {
		struct test_request_conf *t = &test_requests[i];
		uint64_t stripe_index;

		RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, stripe_index) {
			test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_READ,
						      stripe_index, t->stripe_offset_blocks, t->num_blocks);
		}
	}
}
static void
test_raid5f_submit_read_request(void)
{
	run_for_each_raid5f_config(__test_raid5f_submit_read_request);
}

static void
__test_raid5f_stripe_request_map_iovecs(struct raid_bdev *raid_bdev,
					struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	struct raid5f_io_channel *r5ch = spdk_io_channel_get_ctx(raid_ch->module_channel);
	size_t strip_bytes = raid_bdev->strip_size * raid_bdev->bdev.blocklen;
	struct raid_io_info io_info;
	struct raid_bdev_io *raid_io;
	struct spdk_bdev_io *bdev_io;
	struct stripe_request *stripe_req;
	struct chunk *chunk;
	struct iovec iovs[] = {
		{ .iov_base = (void *)0x0ff0000, .iov_len = strip_bytes },
		{ .iov_base = (void *)0x1ff0000, .iov_len = strip_bytes / 2 },
		{ .iov_base = (void *)0x2ff0000, .iov_len = strip_bytes * 2 },
		{ .iov_base = (void *)0x3ff0000, .iov_len = strip_bytes * raid_bdev->num_base_bdevs },
	};
	size_t iovcnt = SPDK_COUNTOF(iovs);
	int ret;

	init_io_info(&io_info, r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE, 0, 0);

	raid_io = get_raid_io(&io_info, 0, 0);
	bdev_io = spdk_bdev_io_from_ctx(raid_io);
	bdev_io->u.bdev.iovs = iovs;
	bdev_io->u.bdev.iovcnt = iovcnt;

	stripe_req = raid5f_stripe_request_alloc(r5ch);
	SPDK_CU_ASSERT_FATAL(stripe_req != NULL);

	stripe_req->parity_chunk = &stripe_req->chunks[raid5f_stripe_data_chunks_num(raid_bdev)];
	stripe_req->raid_io = raid_io;

	ret = raid5f_stripe_request_map_iovecs(stripe_req);
	CU_ASSERT(ret == 0);

	chunk = &stripe_req->chunks[0];
	CU_ASSERT_EQUAL(chunk->iovcnt, 1);
	CU_ASSERT_EQUAL(chunk->iovs[0].iov_base, iovs[0].iov_base);
	CU_ASSERT_EQUAL(chunk->iovs[0].iov_len, iovs[0].iov_len);

	chunk = &stripe_req->chunks[1];
	CU_ASSERT_EQUAL(chunk->iovcnt, 2);
	CU_ASSERT_EQUAL(chunk->iovs[0].iov_base, iovs[1].iov_base);
	CU_ASSERT_EQUAL(chunk->iovs[0].iov_len, iovs[1].iov_len);
	CU_ASSERT_EQUAL(chunk->iovs[1].iov_base, iovs[2].iov_base);
	CU_ASSERT_EQUAL(chunk->iovs[1].iov_len, iovs[2].iov_len / 4);

	if (raid_bdev->num_base_bdevs > 3) {
		chunk = &stripe_req->chunks[2];
		CU_ASSERT_EQUAL(chunk->iovcnt, 1);
		CU_ASSERT_EQUAL(chunk->iovs[0].iov_base, iovs[2].iov_base + strip_bytes / 2);
		CU_ASSERT_EQUAL(chunk->iovs[0].iov_len, iovs[2].iov_len / 2);
	}
	if (raid_bdev->num_base_bdevs > 4) {
		chunk = &stripe_req->chunks[3];
		CU_ASSERT_EQUAL(chunk->iovcnt, 2);
		CU_ASSERT_EQUAL(chunk->iovs[0].iov_base, iovs[2].iov_base + (strip_bytes / 2) * 3);
		CU_ASSERT_EQUAL(chunk->iovs[0].iov_len, iovs[2].iov_len / 4);
		CU_ASSERT_EQUAL(chunk->iovs[1].iov_base, iovs[3].iov_base);
		CU_ASSERT_EQUAL(chunk->iovs[1].iov_len, strip_bytes / 2);
	}

	raid5f_stripe_request_free(stripe_req);
	spdk_bdev_free_io(bdev_io);
	deinit_io_info(&io_info);
}
static void
test_raid5f_stripe_request_map_iovecs(void)
{
	run_for_each_raid5f_config(__test_raid5f_stripe_request_map_iovecs);
}

static void
__test_raid5f_submit_full_stripe_write_request(struct raid_bdev *raid_bdev,
		struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	uint64_t stripe_index;

	RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, stripe_index) {
		test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE,
					      stripe_index, 0, r5f_info->stripe_blocks);
	}
}
static void
test_raid5f_submit_full_stripe_write_request(void)
{
	run_for_each_raid5f_config(__test_raid5f_submit_full_stripe_write_request);
}

static void
__test_raid5f_chunk_write_error(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	struct raid_base_bdev_info *base_bdev_info;
	uint64_t stripe_index;
	struct raid_io_info io_info;
	enum test_bdev_error_type error_type;

	for (error_type = TEST_BDEV_ERROR_SUBMIT; error_type <= TEST_BDEV_ERROR_NOMEM; error_type++) {
		RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, stripe_index) {
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_bdev_info) {
				init_io_info(&io_info, r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE,
					     stripe_index * r5f_info->stripe_blocks, r5f_info->stripe_blocks);

				io_info.error.type = error_type;
				io_info.error.bdev = base_bdev_info->bdev;

				test_raid5f_write_request(&io_info);

				if (error_type == TEST_BDEV_ERROR_NOMEM) {
					CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_SUCCESS);
				} else {
					CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_FAILED);
				}

				deinit_io_info(&io_info);
			}
		}
	}
}
static void
test_raid5f_chunk_write_error(void)
{
	run_for_each_raid5f_config(__test_raid5f_chunk_write_error);
}

struct chunk_write_error_with_enomem_ctx {
	enum test_bdev_error_type error_type;
	struct spdk_bdev *bdev;
};

static void
chunk_write_error_with_enomem_cb(struct raid_io_info *io_info, void *_ctx)
{
	struct chunk_write_error_with_enomem_ctx *ctx = _ctx;

	io_info->error.type = ctx->error_type;
	io_info->error.bdev = ctx->bdev;
}

static void
__test_raid5f_chunk_write_error_with_enomem(struct raid_bdev *raid_bdev,
		struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	struct raid_base_bdev_info *base_bdev_info;
	uint64_t stripe_index;
	struct raid_io_info io_info;
	enum test_bdev_error_type error_type;
	struct chunk_write_error_with_enomem_ctx on_enomem_cb_ctx;

	for (error_type = TEST_BDEV_ERROR_SUBMIT; error_type <= TEST_BDEV_ERROR_COMPLETE; error_type++) {
		RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, stripe_index) {
			struct raid_base_bdev_info *base_bdev_info_last =
					&raid_bdev->base_bdev_info[raid_bdev->num_base_bdevs - 1];

			RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_bdev_info) {
				if (base_bdev_info == base_bdev_info_last) {
					continue;
				}

				init_io_info(&io_info, r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE,
					     stripe_index * r5f_info->stripe_blocks, r5f_info->stripe_blocks);

				io_info.error.type = TEST_BDEV_ERROR_NOMEM;
				io_info.error.bdev = base_bdev_info->bdev;
				io_info.error.on_enomem_cb = chunk_write_error_with_enomem_cb;
				io_info.error.on_enomem_cb_ctx = &on_enomem_cb_ctx;
				on_enomem_cb_ctx.error_type = error_type;
				on_enomem_cb_ctx.bdev = base_bdev_info_last->bdev;

				test_raid5f_write_request(&io_info);

				CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_FAILED);

				deinit_io_info(&io_info);
			}
		}
	}
}
static void
test_raid5f_chunk_write_error_with_enomem(void)
{
	run_for_each_raid5f_config(__test_raid5f_chunk_write_error_with_enomem);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid5f", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid5f_start);
	CU_ADD_TEST(suite, test_raid5f_submit_read_request);
	CU_ADD_TEST(suite, test_raid5f_stripe_request_map_iovecs);
	CU_ADD_TEST(suite, test_raid5f_submit_full_stripe_write_request);
	CU_ADD_TEST(suite, test_raid5f_chunk_write_error);
	CU_ADD_TEST(suite, test_raid5f_chunk_write_error_with_enomem);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
