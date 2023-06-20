/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/env.h"
#include "spdk/xor.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid5f.c"
#include "../common.c"

static void *g_accel_p = (void *)0xdeadbeaf;
static bool g_test_degraded;

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(raid_bdev_module_stop_done, (struct raid_bdev *raid_bdev));
DEFINE_STUB(accel_channel_create, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_channel_destroy, (void *io_device, void *ctx_buf));
DEFINE_STUB_V(raid_bdev_process_request_complete, (struct raid_bdev_process_request *process_req,
		int status));
DEFINE_STUB_V(raid_bdev_io_init, (struct raid_bdev_io *raid_io,
				  struct raid_bdev_io_channel *raid_ch,
				  enum spdk_bdev_io_type type, uint64_t offset_blocks,
				  uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
				  struct spdk_memory_domain *memory_domain, void *memory_domain_ctx));

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(g_accel_p);
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

struct xor_ctx {
	spdk_accel_completion_cb cb_fn;
	void *cb_arg;
};

static void
finish_xor(void *_ctx)
{
	struct xor_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, 0);

	free(ctx);
}

int
spdk_accel_submit_xor(struct spdk_io_channel *ch, void *dst, void **sources, uint32_t nsrcs,
		      uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct xor_ctx *ctx;

	ctx = malloc(sizeof(*ctx));
	SPDK_CU_ASSERT_FATAL(ctx != NULL);
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	SPDK_CU_ASSERT_FATAL(spdk_xor_gen(dst, sources, nsrcs, nbytes) == 0);

	spdk_thread_send_msg(spdk_get_thread(), finish_xor, ctx);

	return 0;
}

static void
init_accel(void)
{
	spdk_io_device_register(g_accel_p, accel_channel_create, accel_channel_destroy,
				sizeof(int), "accel_p");
}

static void
fini_accel(void)
{
	spdk_io_device_unregister(g_accel_p, NULL);
}

static int
test_suite_init(void)
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

	init_accel();

	return 0;
}

static int
test_suite_cleanup(void)
{
	fini_accel();
	raid_test_params_free();
	return 0;
}

static void
test_setup(void)
{
	g_test_degraded = false;
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
	uint64_t stripe_index;
	uint64_t offset_blocks;
	uint64_t stripe_offset_blocks;
	uint64_t num_blocks;
	void *src_buf;
	void *dest_buf;
	void *src_md_buf;
	void *dest_md_buf;
	size_t buf_size;
	size_t buf_md_size;
	void *parity_buf;
	void *reference_parity;
	size_t parity_buf_size;
	void *parity_md_buf;
	void *reference_md_parity;
	size_t parity_md_buf_size;
	void *degraded_buf;
	void *degraded_md_buf;
	enum spdk_bdev_io_status status;
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
	struct raid_bdev_io raid_io;
	struct raid_io_info *io_info;
	void *buf;
	void *buf_md;
};

void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	struct test_raid_bdev_io *test_raid_bdev_io = SPDK_CONTAINEROF(raid_io, struct test_raid_bdev_io,
			raid_io);
	struct raid_io_info *io_info = test_raid_bdev_io->io_info;

	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	TAILQ_INSERT_TAIL(&io_info->bdev_io_wait_queue, &raid_io->waitq_entry, link);
}

static void
raid_test_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct test_raid_bdev_io *test_raid_bdev_io = SPDK_CONTAINEROF(raid_io, struct test_raid_bdev_io,
			raid_io);

	test_raid_bdev_io->io_info->status = status;

	free(raid_io->iovs);
	free(test_raid_bdev_io);
}

static struct raid_bdev_io *
get_raid_io(struct raid_io_info *io_info)
{
	struct raid_bdev_io *raid_io;
	struct raid_bdev *raid_bdev = io_info->r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	struct test_raid_bdev_io *test_raid_bdev_io;
	struct iovec *iovs;
	int iovcnt;
	void *md_buf;
	size_t iov_len, remaining;
	struct iovec *iov;
	void *buf;
	int i;

	test_raid_bdev_io = calloc(1, sizeof(*test_raid_bdev_io));
	SPDK_CU_ASSERT_FATAL(test_raid_bdev_io != NULL);

	test_raid_bdev_io->io_info = io_info;

	if (io_info->io_type == SPDK_BDEV_IO_TYPE_READ) {
		test_raid_bdev_io->buf = io_info->src_buf;
		test_raid_bdev_io->buf_md = io_info->src_md_buf;
		buf = io_info->dest_buf;
		md_buf = io_info->dest_md_buf;
	} else {
		test_raid_bdev_io->buf = io_info->dest_buf;
		test_raid_bdev_io->buf_md = io_info->dest_md_buf;
		buf = io_info->src_buf;
		md_buf = io_info->src_md_buf;
	}

	iovcnt = 7;
	iovs = calloc(iovcnt, sizeof(*iovs));
	SPDK_CU_ASSERT_FATAL(iovs != NULL);

	remaining = io_info->num_blocks * blocklen;
	iov_len = remaining / iovcnt;

	for (i = 0; i < iovcnt; i++) {
		iov = &iovs[i];
		iov->iov_base = buf;
		iov->iov_len = iov_len;
		buf += iov_len;
		remaining -= iov_len;
	}
	iov->iov_len += remaining;

	raid_io = &test_raid_bdev_io->raid_io;

	raid_test_bdev_io_init(raid_io, raid_bdev, io_info->raid_ch, io_info->io_type,
			       io_info->offset_blocks, io_info->num_blocks, iovs, iovcnt, md_buf);

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
	uint8_t data_chunk_idx;
	uint64_t data_offset;
	struct iovec dest;
	void *dest_md_buf;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_complete_bdev_io);

	stripe_req = raid5f_chunk_stripe_req(chunk);
	test_raid_bdev_io = SPDK_CONTAINEROF(stripe_req->raid_io, struct test_raid_bdev_io, raid_io);
	io_info = test_raid_bdev_io->io_info;
	raid_bdev = io_info->r5f_info->raid_bdev;

	if (chunk == stripe_req->parity_chunk) {
		if (io_info->parity_buf == NULL) {
			goto submit;
		}
		dest.iov_base = io_info->parity_buf;
		if (md_buf != NULL) {
			dest_md_buf = io_info->parity_md_buf;
		}
	} else {
		data_chunk_idx = chunk < stripe_req->parity_chunk ? chunk->index : chunk->index - 1;
		data_offset = data_chunk_idx * raid_bdev->strip_size * raid_bdev->bdev.blocklen;
		dest.iov_base = test_raid_bdev_io->buf + data_offset;
		if (md_buf != NULL) {
			data_offset = DATA_OFFSET_TO_MD_OFFSET(raid_bdev, data_offset);
			dest_md_buf = test_raid_bdev_io->buf_md + data_offset;
		}
	}
	dest.iov_len = num_blocks * raid_bdev->bdev.blocklen;

	spdk_iovcpy(iov, iovcnt, &dest, 1);
	if (md_buf != NULL) {
		memcpy(dest_md_buf, md_buf, num_blocks * raid_bdev->bdev.md_len);
	}

submit:
	return submit_io(io_info, desc, cb, cb_arg);
}

static int
spdk_bdev_readv_blocks_degraded(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, void *md_buf,
				uint64_t offset_blocks, uint64_t num_blocks,
				spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct chunk *chunk = cb_arg;
	struct stripe_request *stripe_req;
	struct test_raid_bdev_io *test_raid_bdev_io;
	struct raid_io_info *io_info;
	struct raid_bdev *raid_bdev;
	uint8_t data_chunk_idx;
	void *buf, *buf_md;
	struct iovec src;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_complete_bdev_io);

	stripe_req = raid5f_chunk_stripe_req(chunk);
	test_raid_bdev_io = SPDK_CONTAINEROF(stripe_req->raid_io, struct test_raid_bdev_io, raid_io);
	io_info = test_raid_bdev_io->io_info;
	raid_bdev = io_info->r5f_info->raid_bdev;

	if (chunk == stripe_req->parity_chunk) {
		buf = io_info->reference_parity;
		buf_md = io_info->reference_md_parity;
	} else {
		data_chunk_idx = chunk < stripe_req->parity_chunk ? chunk->index : chunk->index - 1;
		buf = io_info->degraded_buf +
		      data_chunk_idx * raid_bdev->strip_size * raid_bdev->bdev.blocklen;
		buf_md = io_info->degraded_md_buf +
			 data_chunk_idx * raid_bdev->strip_size * raid_bdev->bdev.md_len;
	}

	buf += (offset_blocks % raid_bdev->strip_size) * raid_bdev->bdev.blocklen;
	buf_md += (offset_blocks % raid_bdev->strip_size) * raid_bdev->bdev.md_len;

	src.iov_base = buf;
	src.iov_len = num_blocks * raid_bdev->bdev.blocklen;

	spdk_iovcpy(&src, 1, iov, iovcnt);
	if (md_buf != NULL) {
		memcpy(md_buf, buf_md, num_blocks * raid_bdev->bdev.md_len);
	}

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
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct test_raid_bdev_io *test_raid_bdev_io = SPDK_CONTAINEROF(raid_io, struct test_raid_bdev_io,
			raid_io);
	struct iovec src;

	if (cb == raid5f_chunk_complete_bdev_io) {
		return spdk_bdev_readv_blocks_degraded(desc, ch, iov, iovcnt, md_buf, offset_blocks,
						       num_blocks, cb, cb_arg);
	}

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_read_complete);

	src.iov_base = test_raid_bdev_io->buf;
	src.iov_len = num_blocks * raid_bdev->bdev.blocklen;

	spdk_iovcpy(&src, 1, iov, iovcnt);
	if (md_buf != NULL) {
		memcpy(md_buf, test_raid_bdev_io->buf_md, num_blocks * raid_bdev->bdev.md_len);
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

	raid_io = get_raid_io(io_info);

	raid5f_submit_rw_request(raid_io);

	poll_threads();

	process_io_completions(io_info);

	if (g_test_degraded) {
		struct raid_bdev *raid_bdev = io_info->r5f_info->raid_bdev;
		uint8_t p_idx;
		uint8_t i;
		off_t offset;
		uint32_t strip_len;

		for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
			if (!raid_bdev_channel_get_base_channel(io_info->raid_ch, i)) {
				break;
			}
		}

		SPDK_CU_ASSERT_FATAL(i != raid_bdev->num_base_bdevs);

		p_idx = raid5f_stripe_parity_chunk_index(raid_bdev, io_info->stripe_index);

		if (i == p_idx) {
			return;
		}

		if (i >= p_idx) {
			i--;
		}

		strip_len = raid_bdev->strip_size_kb * 1024;
		offset = i * strip_len;

		memcpy(io_info->dest_buf + offset, io_info->src_buf + offset, strip_len);
		if (io_info->dest_md_buf) {
			strip_len = raid_bdev->strip_size * raid_bdev->bdev.md_len;
			offset = i * strip_len;
			memcpy(io_info->dest_md_buf + offset, io_info->src_md_buf + offset, strip_len);
		}
	}

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
	struct raid_bdev_io *raid_io;

	SPDK_CU_ASSERT_FATAL(io_info->num_blocks <= io_info->r5f_info->raid_bdev->strip_size);

	raid_io = get_raid_io(io_info);

	raid5f_submit_rw_request(raid_io);

	process_io_completions(io_info);

	if (g_test_degraded) {
		/* for the reconstruct read xor callback */
		poll_threads();
	}
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
	free(io_info->degraded_buf);
	free(io_info->degraded_md_buf);
}

static void
init_io_info(struct raid_io_info *io_info, struct raid5f_info *r5f_info,
	     struct raid_bdev_io_channel *raid_ch, enum spdk_bdev_io_type io_type,
	     uint64_t stripe_index, uint64_t stripe_offset_blocks, uint64_t num_blocks)
{
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	void *src_buf, *dest_buf;
	void *src_md_buf, *dest_md_buf;
	size_t buf_size = num_blocks * blocklen;
	size_t buf_md_size = num_blocks * raid_bdev->bdev.md_len;
	uint64_t block;
	uint64_t i;

	SPDK_CU_ASSERT_FATAL(stripe_offset_blocks < r5f_info->stripe_blocks);

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
	io_info->stripe_index = stripe_index;
	io_info->offset_blocks = stripe_index * r5f_info->stripe_blocks + stripe_offset_blocks;
	io_info->stripe_offset_blocks = stripe_offset_blocks;
	io_info->num_blocks = num_blocks;
	io_info->src_buf = src_buf;
	io_info->dest_buf = dest_buf;
	io_info->src_md_buf = src_md_buf;
	io_info->dest_md_buf = dest_md_buf;
	io_info->buf_size = buf_size;
	io_info->buf_md_size = buf_md_size;
	io_info->status = SPDK_BDEV_IO_STATUS_PENDING;

	TAILQ_INIT(&io_info->bdev_io_queue);
	TAILQ_INIT(&io_info->bdev_io_wait_queue);
}

static void
io_info_setup_parity(struct raid_io_info *io_info, void *src, void *src_md)
{
	struct raid5f_info *r5f_info = io_info->r5f_info;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	size_t strip_len = raid_bdev->strip_size * blocklen;
	unsigned i;

	io_info->parity_buf_size = strip_len;
	io_info->parity_buf = calloc(1, io_info->parity_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->parity_buf != NULL);

	io_info->reference_parity = calloc(1, io_info->parity_buf_size);
	SPDK_CU_ASSERT_FATAL(io_info->reference_parity != NULL);

	for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
		xor_block(io_info->reference_parity, src, strip_len);
		src += strip_len;
	}

	if (src_md) {
		size_t strip_md_len = raid_bdev->strip_size * raid_bdev->bdev.md_len;

		io_info->parity_md_buf_size = strip_md_len;
		io_info->parity_md_buf = calloc(1, io_info->parity_md_buf_size);
		SPDK_CU_ASSERT_FATAL(io_info->parity_md_buf != NULL);

		io_info->reference_md_parity = calloc(1, io_info->parity_md_buf_size);
		SPDK_CU_ASSERT_FATAL(io_info->reference_md_parity != NULL);

		for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
			xor_block(io_info->reference_md_parity, src_md, strip_md_len);
			src_md += strip_md_len;
		}
	}
}

static void
io_info_setup_degraded(struct raid_io_info *io_info)
{
	struct raid5f_info *r5f_info = io_info->r5f_info;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	uint32_t md_len = raid_bdev->bdev.md_len;
	size_t stripe_len = r5f_info->stripe_blocks * blocklen;
	size_t stripe_md_len = r5f_info->stripe_blocks * md_len;

	io_info->degraded_buf = malloc(stripe_len);
	SPDK_CU_ASSERT_FATAL(io_info->degraded_buf != NULL);

	memset(io_info->degraded_buf, 0xab, stripe_len);

	memcpy(io_info->degraded_buf + io_info->stripe_offset_blocks * blocklen,
	       io_info->src_buf, io_info->num_blocks * blocklen);

	if (stripe_md_len != 0) {
		io_info->degraded_md_buf = malloc(stripe_md_len);
		SPDK_CU_ASSERT_FATAL(io_info->degraded_md_buf != NULL);

		memset(io_info->degraded_md_buf, 0xab, stripe_md_len);

		memcpy(io_info->degraded_md_buf + io_info->stripe_offset_blocks * md_len,
		       io_info->src_md_buf, io_info->num_blocks * md_len);
	}

	io_info_setup_parity(io_info, io_info->degraded_buf, io_info->degraded_md_buf);

	memset(io_info->degraded_buf + io_info->stripe_offset_blocks * blocklen,
	       0xcd, io_info->num_blocks * blocklen);

	if (stripe_md_len != 0) {
		memset(io_info->degraded_md_buf + io_info->stripe_offset_blocks * md_len,
		       0xcd, io_info->num_blocks * md_len);
	}
}

static void
test_raid5f_submit_rw_request(struct raid5f_info *r5f_info, struct raid_bdev_io_channel *raid_ch,
			      enum spdk_bdev_io_type io_type, uint64_t stripe_index, uint64_t stripe_offset_blocks,
			      uint64_t num_blocks)
{
	struct raid_io_info io_info;

	init_io_info(&io_info, r5f_info, raid_ch, io_type, stripe_index, stripe_offset_blocks, num_blocks);

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (g_test_degraded) {
			io_info_setup_degraded(&io_info);
		}
		test_raid5f_read_request(&io_info);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_info_setup_parity(&io_info, io_info.src_buf, io_info.src_md_buf);
		test_raid5f_write_request(&io_info);
		break;
	default:
		CU_FAIL_FATAL("unsupported io_type");
	}

	CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(memcmp(io_info.src_buf, io_info.dest_buf, io_info.buf_size) == 0);
	if (io_info.buf_md_size) {
		CU_ASSERT(memcmp(io_info.src_md_buf, io_info.dest_md_buf, io_info.buf_md_size) == 0);
	}

	deinit_io_info(&io_info);
}

static void
run_for_each_raid5f_config(void (*test_fn)(struct raid_bdev *raid_bdev,
			   struct raid_bdev_io_channel *raid_ch))
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid5f_info *r5f_info;
		struct raid_bdev_io_channel *raid_ch;

		r5f_info = create_raid5f(params);
		raid_ch = raid_test_create_io_channel(r5f_info->raid_bdev);

		if (g_test_degraded) {
			raid_ch->_base_channels[0] = NULL;
		}

		test_fn(r5f_info->raid_bdev, raid_ch);

		raid_test_destroy_io_channel(raid_ch);
		delete_raid5f(r5f_info);
	}
}

#define RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, i) \
	for (i = 0; i < spdk_min(raid_bdev->num_base_bdevs, ((struct raid5f_info *)raid_bdev->module_private)->total_stripes); i++)

static void
__test_raid5f_submit_read_request(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	uint32_t strip_size = raid_bdev->strip_size;
	uint64_t stripe_index;
	unsigned int i;

	for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
		uint64_t stripe_offset = i * strip_size;

		RAID5F_TEST_FOR_EACH_STRIPE(raid_bdev, stripe_index) {
			test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_READ,
						      stripe_index, stripe_offset, 1);

			test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_READ,
						      stripe_index, stripe_offset, strip_size);

			test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_READ,
						      stripe_index, stripe_offset + strip_size - 1, 1);
			if (strip_size <= 2) {
				continue;
			}
			test_raid5f_submit_rw_request(r5f_info, raid_ch, SPDK_BDEV_IO_TYPE_READ,
						      stripe_index, stripe_offset + 1, strip_size - 2);
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
	struct raid5f_io_channel *r5ch = raid_bdev_channel_get_module_ctx(raid_ch);
	size_t strip_bytes = raid_bdev->strip_size * raid_bdev->bdev.blocklen;
	struct raid_bdev_io raid_io = {};
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

	raid_io.raid_bdev = raid_bdev;
	raid_io.iovs = iovs;
	raid_io.iovcnt = iovcnt;

	stripe_req = raid5f_stripe_request_alloc(r5ch, STRIPE_REQ_WRITE);
	SPDK_CU_ASSERT_FATAL(stripe_req != NULL);

	stripe_req->parity_chunk = &stripe_req->chunks[raid5f_stripe_data_chunks_num(raid_bdev)];
	stripe_req->raid_io = &raid_io;

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
					     stripe_index, 0, r5f_info->stripe_blocks);

				io_info.error.type = error_type;
				io_info.error.bdev = base_bdev_info->desc->bdev;

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
					     stripe_index, 0, r5f_info->stripe_blocks);

				io_info.error.type = TEST_BDEV_ERROR_NOMEM;
				io_info.error.bdev = base_bdev_info->desc->bdev;
				io_info.error.on_enomem_cb = chunk_write_error_with_enomem_cb;
				io_info.error.on_enomem_cb_ctx = &on_enomem_cb_ctx;
				on_enomem_cb_ctx.error_type = error_type;
				on_enomem_cb_ctx.bdev = base_bdev_info_last->desc->bdev;

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

static void
test_raid5f_submit_full_stripe_write_request_degraded(void)
{
	g_test_degraded = true;
	run_for_each_raid5f_config(__test_raid5f_submit_full_stripe_write_request);
}

static void
test_raid5f_submit_read_request_degraded(void)
{
	g_test_degraded = true;
	run_for_each_raid5f_config(__test_raid5f_submit_read_request);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite_with_setup_and_teardown("raid5f", test_suite_init, test_suite_cleanup,
			test_setup, NULL);
	CU_ADD_TEST(suite, test_raid5f_start);
	CU_ADD_TEST(suite, test_raid5f_submit_read_request);
	CU_ADD_TEST(suite, test_raid5f_stripe_request_map_iovecs);
	CU_ADD_TEST(suite, test_raid5f_submit_full_stripe_write_request);
	CU_ADD_TEST(suite, test_raid5f_chunk_write_error);
	CU_ADD_TEST(suite, test_raid5f_chunk_write_error_with_enomem);
	CU_ADD_TEST(suite, test_raid5f_submit_full_stripe_write_request_degraded);
	CU_ADD_TEST(suite, test_raid5f_submit_read_request_degraded);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
