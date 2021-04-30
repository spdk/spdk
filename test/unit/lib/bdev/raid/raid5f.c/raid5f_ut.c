/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid5f.c"

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_queue_io_wait, (struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
					struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn));

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

struct raid5f_params {
	uint8_t num_base_bdevs;
	uint64_t base_bdev_blockcnt;
	uint32_t base_bdev_blocklen;
	uint32_t strip_size;
};

static struct raid5f_params *g_params;
static size_t g_params_count;

#define ARRAY_FOR_EACH(a, e) \
	for (e = a; e < a + SPDK_COUNTOF(a); e++)

#define RAID5F_PARAMS_FOR_EACH(p) \
	for (p = g_params; p < g_params + g_params_count; p++)

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 3, 4, 5 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint32_t strip_size_kb_values[] = { 1, 4, 128 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	uint32_t *strip_size_kb;
	struct raid5f_params *params;

	g_params_count = SPDK_COUNTOF(num_base_bdevs_values) *
			 SPDK_COUNTOF(base_bdev_blockcnt_values) *
			 SPDK_COUNTOF(base_bdev_blocklen_values) *
			 SPDK_COUNTOF(strip_size_kb_values);
	g_params = calloc(g_params_count, sizeof(*g_params));
	if (!g_params) {
		return -ENOMEM;
	}

	params = g_params;

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				ARRAY_FOR_EACH(strip_size_kb_values, strip_size_kb) {
					params->num_base_bdevs = *num_base_bdevs;
					params->base_bdev_blockcnt = *base_bdev_blockcnt;
					params->base_bdev_blocklen = *base_bdev_blocklen;
					params->strip_size = *strip_size_kb * 1024 / *base_bdev_blocklen;
					if (params->strip_size == 0 ||
					    params->strip_size > *base_bdev_blockcnt) {
						g_params_count--;
						continue;
					}
					params++;
				}
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	free(g_params);
	return 0;
}

static struct raid_bdev *
create_raid_bdev(struct raid5f_params *params)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);

	raid_bdev->module = &g_raid5f_module;
	raid_bdev->num_base_bdevs = params->num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	SPDK_CU_ASSERT_FATAL(raid_bdev->base_bdev_info != NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->bdev = calloc(1, sizeof(*base_info->bdev));
		SPDK_CU_ASSERT_FATAL(base_info->bdev != NULL);

		base_info->bdev->blockcnt = params->base_bdev_blockcnt;
		base_info->bdev->blocklen = params->base_bdev_blocklen;
	}

	raid_bdev->strip_size = params->strip_size;
	raid_bdev->strip_size_kb = params->strip_size * params->base_bdev_blocklen / 1024;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->blocklen_shift = spdk_u32log2(params->base_bdev_blocklen);
	raid_bdev->bdev.blocklen = params->base_bdev_blocklen;

	return raid_bdev;
}

static void
delete_raid_bdev(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		free(base_info->bdev);
	}
	free(raid_bdev->base_bdev_info);
	free(raid_bdev);
}

static struct raid5f_info *
create_raid5f(struct raid5f_params *params)
{
	struct raid_bdev *raid_bdev = create_raid_bdev(params);

	SPDK_CU_ASSERT_FATAL(raid5f_start(raid_bdev) == 0);

	return raid_bdev->module_private;
}

static void
delete_raid5f(struct raid5f_info *r5f_info)
{
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;

	raid5f_stop(raid_bdev);

	delete_raid_bdev(raid_bdev);
}

static void
test_raid5f_start(void)
{
	struct raid5f_params *params;

	RAID5F_PARAMS_FOR_EACH(params) {
		struct raid5f_info *r5f_info;

		r5f_info = create_raid5f(params);

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

struct raid_io_info {
	struct raid5f_info *r5f_info;
	struct raid_bdev_io_channel *raid_ch;
	enum spdk_bdev_io_type io_type;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	void *src_buf;
	void *dest_buf;
	size_t buf_size;
	enum spdk_bdev_io_status status;
	bool failed;
	int remaining;
	TAILQ_HEAD(, spdk_bdev_io) bdev_io_queue;
};

struct test_raid_bdev_io {
	char bdev_io_buf[sizeof(struct spdk_bdev_io) + sizeof(struct raid_bdev_io)];
	struct raid_io_info *io_info;
	void *buf;
};

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
		bdev_io->iov.iov_base = dest_buf;
	} else {
		test_raid_bdev_io->buf = dest_buf;
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

static void
submit_io(struct raid_io_info *io_info, struct spdk_bdev_desc *desc,
	  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(*bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->internal.cb = cb;
	bdev_io->internal.caller_ctx = cb_arg;

	TAILQ_INSERT_TAIL(&io_info->bdev_io_queue, bdev_io, internal.link);
}

static void
process_io_completions(struct raid_io_info *io_info)
{
	struct spdk_bdev_io *bdev_io;

	while ((bdev_io = TAILQ_FIRST(&io_info->bdev_io_queue))) {
		TAILQ_REMOVE(&io_info->bdev_io_queue, bdev_io, internal.link);

		bdev_io->internal.cb(bdev_io, true, bdev_io->internal.caller_ctx);
	}
}

int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
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
	void *dest_buf;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_write_complete_bdev_io);
	SPDK_CU_ASSERT_FATAL(iovcnt == 1);

	stripe_req = raid5f_chunk_stripe_req(chunk);
	test_raid_bdev_io = (struct test_raid_bdev_io *)spdk_bdev_io_from_ctx(stripe_req->raid_io);
	io_info = test_raid_bdev_io->io_info;
	raid_bdev = io_info->r5f_info->raid_bdev;

	SPDK_CU_ASSERT_FATAL(chunk != stripe_req->parity_chunk);

	stripe_idx_off = offset_blocks / raid_bdev->strip_size -
			 io_info->offset_blocks / io_info->r5f_info->stripe_blocks;

	data_chunk_idx = chunk < stripe_req->parity_chunk ? chunk->index : chunk->index - 1;
	dest_buf = test_raid_bdev_io->buf +
		   (stripe_idx_off * io_info->r5f_info->stripe_blocks +
		    data_chunk_idx * raid_bdev->strip_size) *
		   raid_bdev->bdev.blocklen;

	memcpy(dest_buf, iov->iov_base, iov->iov_len);

	submit_io(test_raid_bdev_io->io_info, desc, cb, cb_arg);

	return 0;
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;
	struct test_raid_bdev_io *test_raid_bdev_io;

	SPDK_CU_ASSERT_FATAL(cb == raid5f_chunk_read_complete);
	SPDK_CU_ASSERT_FATAL(iovcnt == 1);

	test_raid_bdev_io = (struct test_raid_bdev_io *)spdk_bdev_io_from_ctx(raid_io);

	memcpy(iov->iov_base, test_raid_bdev_io->buf, iov->iov_len);

	submit_io(test_raid_bdev_io->io_info, desc, cb, cb_arg);

	return 0;
}

static void
test_raid5f_write_request(struct raid_io_info *io_info)
{
	struct raid_bdev_io *raid_io;

	SPDK_CU_ASSERT_FATAL(io_info->num_blocks / io_info->r5f_info->stripe_blocks == 1);

	raid_io = get_raid_io(io_info, 0, io_info->num_blocks);

	raid5f_submit_rw_request(raid_io);

	process_io_completions(io_info);
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
}

static void
init_io_info(struct raid_io_info *io_info, struct raid5f_info *r5f_info,
	     struct raid_bdev_io_channel *raid_ch, enum spdk_bdev_io_type io_type,
	     uint64_t offset_blocks, uint64_t num_blocks)
{
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t blocklen = raid_bdev->bdev.blocklen;
	void *src_buf, *dest_buf;
	size_t buf_size = num_blocks * blocklen;
	uint64_t block;

	memset(io_info, 0, sizeof(*io_info));

	src_buf = spdk_dma_malloc(buf_size, 4096, NULL);
	SPDK_CU_ASSERT_FATAL(src_buf != NULL);

	dest_buf = spdk_dma_malloc(buf_size, 4096, NULL);
	SPDK_CU_ASSERT_FATAL(dest_buf != NULL);

	memset(src_buf, 0xff, buf_size);
	for (block = 0; block < num_blocks; block++) {
		*((uint64_t *)(src_buf + block * blocklen)) = block;
	}

	io_info->r5f_info = r5f_info;
	io_info->raid_ch = raid_ch;
	io_info->io_type = io_type;
	io_info->offset_blocks = offset_blocks;
	io_info->num_blocks = num_blocks;
	io_info->src_buf = src_buf;
	io_info->dest_buf = dest_buf;
	io_info->buf_size = buf_size;
	io_info->status = SPDK_BDEV_IO_STATUS_PENDING;

	TAILQ_INIT(&io_info->bdev_io_queue);
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
		test_raid5f_write_request(&io_info);
		break;
	default:
		CU_FAIL_FATAL("unsupported io_type");
	}

	CU_ASSERT(io_info.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(memcmp(io_info.src_buf, io_info.dest_buf, io_info.buf_size) == 0);

	deinit_io_info(&io_info);
}

static void
run_for_each_raid5f_config(void (*test_fn)(struct raid_bdev *raid_bdev,
			   struct raid_bdev_io_channel *raid_ch))
{
	struct raid5f_params *params;

	RAID5F_PARAMS_FOR_EACH(params) {
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
	struct raid5f_io_channel *r5ch = spdk_io_channel_get_ctx(raid_ch->module_channel);
	size_t strip_bytes = raid_bdev->strip_size * raid_bdev->bdev.blocklen;
	struct raid_bdev_io raid_io = { .raid_bdev = raid_bdev };
	struct stripe_request *stripe_req;
	struct chunk *chunk;
	struct iovec iovs[] = {
		{ .iov_base = (void *)0x0ff0000, .iov_len = strip_bytes },
		{ .iov_base = (void *)0x1ff0000, .iov_len = strip_bytes / 2 },
		{ .iov_base = (void *)0x2ff0000, .iov_len = strip_bytes * 2 },
		{ .iov_base = (void *)0x3ff0000, .iov_len = strip_bytes * raid_bdev->num_base_bdevs },
	};
	size_t iovcnt = sizeof(iovs) / sizeof(iovs[0]);
	int ret;

	stripe_req = raid5f_stripe_request_alloc(r5ch);
	SPDK_CU_ASSERT_FATAL(stripe_req != NULL);

	stripe_req->parity_chunk = &stripe_req->chunks[raid5f_stripe_data_chunks_num(raid_bdev)];
	stripe_req->raid_io = &raid_io;

	ret = raid5f_stripe_request_map_iovecs(stripe_req, iovs, iovcnt);
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

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
