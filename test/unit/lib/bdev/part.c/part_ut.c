/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"
#include "bdev/part.c"

#include "common/lib/bdev/common_stubs.h"

struct ut_expected_io {
};

struct bdev_ut_channel {
	TAILQ_HEAD(, spdk_bdev_io) outstanding_io;
	uint32_t    outstanding_io_count;
	TAILQ_HEAD(, ut_expected_io) expected_io;
};

static uint32_t g_part_ut_io_device;
static struct bdev_ut_channel *g_bdev_ut_channel;
static int g_accel_io_device;

DEFINE_RETURN_MOCK(spdk_memory_domain_pull_data, int);
int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_memory_domain_pull_data);

	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_memory_domain_push_data, int);
int
spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_memory_domain_push_data);

	cpl_cb(cpl_cb_arg, 0);
	return 0;
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_accel_io_device);
}

static int
ut_accel_ch_create_cb(void *io_device, void *ctx)
{
	return 0;
}

static void
ut_accel_ch_destroy_cb(void *io_device, void *ctx)
{
}

static int
ut_part_setup(void)
{
	spdk_io_device_register(&g_accel_io_device, ut_accel_ch_create_cb,
				ut_accel_ch_destroy_cb, 0, NULL);
	return 0;
}

static int
ut_part_teardown(void)
{
	spdk_io_device_unregister(&g_accel_io_device, NULL);

	return 0;
}

static void
_part_cleanup(struct spdk_bdev_part *part)
{
	spdk_io_device_unregister(part, NULL);
	free(part->internal.bdev.name);
	free(part->internal.bdev.product_name);
}

static struct spdk_io_channel *
part_ut_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_part_ut_io_device);
}

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

static int
bdev_ut_create_ch(void *io_device, void *ctx_buf)
{
	struct bdev_ut_channel *ch = ctx_buf;

	CU_ASSERT(g_bdev_ut_channel == NULL);
	g_bdev_ut_channel = ch;
	g_part_ut_io_device++;

	TAILQ_INIT(&ch->outstanding_io);
	ch->outstanding_io_count = 0;
	TAILQ_INIT(&ch->expected_io);
	return 0;
}

static void
bdev_ut_destroy_ch(void *io_device, void *ctx_buf)
{
	CU_ASSERT(g_bdev_ut_channel != NULL);
	g_bdev_ut_channel = NULL;
	g_part_ut_io_device--;
}

struct spdk_bdev_module bdev_ut_if;

static int
bdev_ut_module_init(void)
{
	spdk_io_device_register(&g_part_ut_io_device, bdev_ut_create_ch, bdev_ut_destroy_ch,
				sizeof(struct bdev_ut_channel), NULL);
	spdk_bdev_module_init_done(&bdev_ut_if);
	return 0;
}

static void
bdev_ut_module_fini(void)
{
	spdk_io_device_unregister(&g_part_ut_io_device, NULL);
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = bdev_ut_module_init,
	.module_fini = bdev_ut_module_fini,
	.async_init = true,
};

static void vbdev_ut_examine(struct spdk_bdev *bdev);

static int
vbdev_ut_module_init(void)
{
	return 0;
}

static void
vbdev_ut_module_fini(void)
{
}

struct spdk_bdev_module vbdev_ut_if = {
	.name = "vbdev_ut",
	.module_init = vbdev_ut_module_init,
	.module_fini = vbdev_ut_module_fini,
	.examine_config = vbdev_ut_examine,
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(vbdev_ut, &vbdev_ut_if)

static void
vbdev_ut_examine(struct spdk_bdev *bdev)
{
	spdk_bdev_module_examine_done(&vbdev_ut_if);
}

static int
__destruct(void *ctx)
{
	return 0;
}

static bool
__io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	return true;
}

#define PART_UT_READ_PATTERN 0xA5

static void *g_base_read_buf;
static enum spdk_bdev_io_status g_base_read_status;
static struct spdk_bdev_io *g_part_read_io;
static bool g_part_read_done;
static bool g_part_read_success;

static void
part_ut_base_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			     bool success)
{
	CU_ASSERT(success == true);

	if (bdev_io->internal.f.has_buf) {
		/* The bdev layer allocated the data buffer on this (base) bdev_io */
		g_base_read_buf = bdev_io->u.bdev.iovs[0].iov_base;
		CU_ASSERT(bdev_io->internal.buf.ptr == g_base_read_buf);
	}

	memset(bdev_io->u.bdev.iovs[0].iov_base, PART_UT_READ_PATTERN,
	       bdev_io->u.bdev.iovs[0].iov_len);
	spdk_bdev_io_complete(bdev_io, g_base_read_status);
}

static void
part_ut_base_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	SPDK_CU_ASSERT_FATAL(bdev_io->type == SPDK_BDEV_IO_TYPE_READ);

	/* Like a real base bdev module, allocate a data buffer if the caller didn't provide one */
	spdk_bdev_io_get_buf(bdev_io, part_ut_base_read_get_buf_cb,
			     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

static void
part_ut_part_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_part_channel *part_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_bdev_part_submit_request(part_ch, bdev_io);
	CU_ASSERT(rc == 0);
}

static struct spdk_bdev_fn_table base_fn_table = {
	.destruct		= __destruct,
	.submit_request	= part_ut_base_submit_request,
	.get_io_channel = part_ut_get_io_channel,
	.io_type_supported	= __io_type_supported,
};
static struct spdk_bdev_fn_table part_fn_table = {
	.destruct		= __destruct,
	.submit_request	= part_ut_part_submit_request,
	.io_type_supported	= __io_type_supported,
};

static void
bdev_init_cb(void *arg, int rc)
{
	CU_ASSERT(rc == 0);
}

static void
bdev_fini_cb(void *arg)
{
}

static void
ut_init_bdev(void)
{
	int rc;

	rc = spdk_iobuf_initialize();
	CU_ASSERT(rc == 0);

	spdk_bdev_initialize(bdev_init_cb, NULL);
	poll_threads();
}

static void
ut_fini_bdev(void)
{
	spdk_bdev_finish(bdev_fini_cb, NULL);
	spdk_iobuf_finish(bdev_fini_cb, NULL);
	poll_threads();
}

static void
bdev_ut_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static void
part_test(void)
{
	struct spdk_bdev_part_base	*base;
	struct spdk_bdev_part		part1 = {};
	struct spdk_bdev_part		part2 = {};
	struct spdk_bdev_part		part3 = {};
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	int rc;

	bdev_base.name = "base";
	bdev_base.blocklen = 512;
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	bdev_base.dif_storage_tag_size = 32;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &tailq, NULL,
					       NULL, 0, NULL, NULL, &base);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(base != NULL);

	rc = spdk_bdev_part_construct(&part1, base, "test1", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(spdk_bdev_get_dif_storage_tag_size(&part1.internal.bdev) == 32);
	SPDK_CU_ASSERT_FATAL(base->ref == 1);
	SPDK_CU_ASSERT_FATAL(base->claimed == true);
	rc = spdk_bdev_part_construct(&part2, base, "test2", 100, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(spdk_bdev_get_dif_storage_tag_size(&part2.internal.bdev) == 32);
	SPDK_CU_ASSERT_FATAL(base->ref == 2);
	SPDK_CU_ASSERT_FATAL(base->claimed == true);
	rc = spdk_bdev_part_construct(&part3, base, "test1", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc != 0);
	SPDK_CU_ASSERT_FATAL(base->ref == 2);
	SPDK_CU_ASSERT_FATAL(base->claimed == true);

	spdk_bdev_part_base_hotremove(base, &tailq);

	spdk_bdev_part_base_free(base);
	_part_cleanup(&part1);
	_part_cleanup(&part2);
	spdk_bdev_unregister(&bdev_base, NULL, NULL);

	poll_threads();
}

static void
part_free_test(void)
{
	struct spdk_bdev_part_base	*base = NULL;
	struct spdk_bdev_part		*part;
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	int rc;

	bdev_base.name = "base";
	bdev_base.blocklen = 512;
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	poll_threads();

	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &tailq, NULL,
					       NULL, 0, NULL, NULL, &base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&tailq));
	SPDK_CU_ASSERT_FATAL(base != NULL);

	part = calloc(1, sizeof(*part));
	SPDK_CU_ASSERT_FATAL(part != NULL);
	rc = spdk_bdev_part_construct(part, base, "test", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	poll_threads();
	CU_ASSERT(!TAILQ_EMPTY(&tailq));

	spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
	poll_threads();

	rc = spdk_bdev_part_free(part);
	CU_ASSERT(rc == 1);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&tailq));

	spdk_bdev_unregister(&bdev_base, NULL, NULL);
	poll_threads();
}

static void
part_get_io_channel_test(void)
{
	struct spdk_bdev_part_base	*base = NULL;
	struct spdk_bdev_desc   *desc = NULL;
	struct spdk_io_channel  *io_ch;
	struct spdk_bdev_part		*part;
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	int rc;

	ut_init_bdev();
	bdev_base.name = "base";
	bdev_base.blocklen = 512;
	bdev_base.blockcnt = 1024;
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);

	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &tailq, NULL,
					       NULL, 100, NULL, NULL, &base);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&tailq));
	SPDK_CU_ASSERT_FATAL(base != NULL);

	part = calloc(1, sizeof(*part));
	SPDK_CU_ASSERT_FATAL(part != NULL);
	rc = spdk_bdev_part_construct(part, base, "test", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&tailq));

	rc = spdk_bdev_open_ext("test", true, bdev_ut_event_cb, NULL, &desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(&part->internal.bdev == spdk_bdev_desc_get_bdev(desc));

	io_ch = spdk_bdev_get_io_channel(desc);
	CU_ASSERT(io_ch != NULL);
	CU_ASSERT(g_part_ut_io_device == 1);

	spdk_put_io_channel(io_ch);
	spdk_bdev_close(desc);
	spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
	poll_threads();
	CU_ASSERT(g_part_ut_io_device == 0);

	rc = spdk_bdev_part_free(part);
	CU_ASSERT(rc == 1);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&tailq));

	spdk_bdev_unregister(&bdev_base, NULL, NULL);
	ut_fini_bdev();
}

static void
part_construct_ext(void)
{
	struct spdk_bdev_part_base	*base;
	struct spdk_bdev_part		part1 = {};
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	const char			*uuid = "7ed764b7-a841-41b1-ba93-6548d9335a44";
	struct spdk_bdev_part_construct_opts opts;
	int rc;

	bdev_base.name = "base";
	bdev_base.blocklen = 512;
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &tailq, NULL,
					       NULL, 0, NULL, NULL, &base);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(base != NULL);

	/* Verify opts.uuid is used as bdev UUID */
	spdk_bdev_part_construct_opts_init(&opts, sizeof(opts));
	spdk_uuid_parse(&opts.uuid, uuid);
	rc = spdk_bdev_part_construct_ext(&part1, base, "test1", 0, 100, "test", &opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(base->ref == 1);
	SPDK_CU_ASSERT_FATAL(base->claimed == true);
	CU_ASSERT(spdk_bdev_get_by_name(uuid) != NULL);
	CU_ASSERT(spdk_bdev_get_by_name("test1") != NULL);

	/* Clean up */
	spdk_bdev_part_base_hotremove(base, &tailq);
	spdk_bdev_part_base_free(base);
	_part_cleanup(&part1);
	spdk_bdev_unregister(&bdev_base, NULL, NULL);

	poll_threads();
}

struct part_io_test_ctx {
	struct spdk_bdev		bdev_base;
	struct spdk_bdev_part_base	*base;
	struct spdk_bdev_part		*part;
	struct spdk_bdev_desc		*desc;
	struct spdk_io_channel		*io_ch;
	SPDK_BDEV_PART_TAILQ		tailq;
};

static void
part_ut_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_part_read_done = true;
	g_part_read_success = success;
	/* Hold on to the bdev_io like a real upper layer that keeps consuming the data
	 * after the completion callback (e.g. a target queueing it for transmission)
	 * and calls spdk_bdev_free_io() only when done. */
	g_part_read_io = bdev_io;
}

static void
ut_part_io_test_setup(struct part_io_test_ctx *ctx)
{
	int rc;

	ut_init_bdev();

	TAILQ_INIT(&ctx->tailq);
	ctx->bdev_base.name = "base";
	ctx->bdev_base.blocklen = 512;
	ctx->bdev_base.blockcnt = 1024;
	ctx->bdev_base.fn_table = &base_fn_table;
	ctx->bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&ctx->bdev_base);
	CU_ASSERT(rc == 0);

	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &ctx->tailq, NULL,
					       NULL, sizeof(struct spdk_bdev_part_channel),
					       NULL, NULL, &ctx->base);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(ctx->base != NULL);

	ctx->part = calloc(1, sizeof(*ctx->part));
	SPDK_CU_ASSERT_FATAL(ctx->part != NULL);
	rc = spdk_bdev_part_construct(ctx->part, ctx->base, "test", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_bdev_open_ext("test", true, bdev_ut_event_cb, NULL, &ctx->desc);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(ctx->desc != NULL);

	ctx->io_ch = spdk_bdev_get_io_channel(ctx->desc);
	SPDK_CU_ASSERT_FATAL(ctx->io_ch != NULL);

	g_base_read_buf = NULL;
	g_base_read_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_part_read_io = NULL;
	g_part_read_done = false;
	g_part_read_success = false;
}

static void
ut_part_io_test_teardown(struct part_io_test_ctx *ctx)
{
	int rc;

	spdk_put_io_channel(ctx->io_ch);
	spdk_bdev_close(ctx->desc);
	spdk_bdev_unregister(&ctx->part->internal.bdev, NULL, NULL);
	poll_threads();

	rc = spdk_bdev_part_free(ctx->part);
	CU_ASSERT(rc == 1);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&ctx->tailq));

	spdk_bdev_unregister(&ctx->bdev_base, NULL, NULL);
	ut_fini_bdev();
}

static void
part_read_bdev_buf_ownership_test(void)
{
	struct part_io_test_ctx ctx = {};
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	uint8_t *buf;
	void *probe_buf;
	int rc;

	ut_part_io_test_setup(&ctx);

	/* iov-less read: the base bdev allocates the data buffer on the child bdev_io */
	rc = spdk_bdev_read_blocks(ctx.desc, ctx.io_ch, NULL, 0, 1, part_ut_io_done, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_part_read_done == true);
	CU_ASSERT(g_part_read_success == true);
	SPDK_CU_ASSERT_FATAL(g_part_read_io != NULL);
	SPDK_CU_ASSERT_FATAL(g_base_read_buf != NULL);

	/* The base (child) bdev_io was already freed. Ownership of its buffer must have
	 * been transferred to the part bdev_io, keeping the data valid for the upper layer. */
	CU_ASSERT(g_part_read_io->internal.f.has_buf == true);
	CU_ASSERT(g_part_read_io->internal.buf.ptr == g_base_read_buf);
	CU_ASSERT(g_part_read_io->u.bdev.iovs[0].iov_base == g_base_read_buf);
	buf = g_base_read_buf;
	CU_ASSERT(buf[0] == PART_UT_READ_PATTERN);
	CU_ASSERT(buf[511] == PART_UT_READ_PATTERN);

	/* The buffer must not be back in the iobuf pool while the upper layer still holds
	 * the part bdev_io - a new allocation must not hand out the same buffer.
	 * The iobuf per-channel cache is LIFO, so without the ownership transfer this
	 * would deterministically return g_base_read_buf. */
	mgmt_ch = g_part_read_io->internal.ch->shared_resource->mgmt_ch;
	probe_buf = spdk_iobuf_get(&mgmt_ch->iobuf, 512, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(probe_buf != NULL);
	CU_ASSERT(probe_buf != g_base_read_buf);
	spdk_iobuf_put(&mgmt_ch->iobuf, probe_buf, 512);

	/* Freeing the part bdev_io returns the buffer to the pool (exactly once) */
	spdk_bdev_free_io(g_part_read_io);
	probe_buf = spdk_iobuf_get(&mgmt_ch->iobuf, 512, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(probe_buf != NULL);
	CU_ASSERT(probe_buf == g_base_read_buf);
	spdk_iobuf_put(&mgmt_ch->iobuf, probe_buf, 512);

	ut_part_io_test_teardown(&ctx);
}

static void
part_read_bdev_buf_ownership_failed_io_test(void)
{
	struct part_io_test_ctx ctx = {};
	struct spdk_bdev_mgmt_channel *mgmt_ch;
	void *probe_buf, *probe_buf2;
	int rc;

	ut_part_io_test_setup(&ctx);
	g_base_read_status = SPDK_BDEV_IO_STATUS_FAILED;

	/* On a failed read the upper layer only propagates the error and must not
	 * consume the data, so no ownership transfer happens: freeing the child
	 * bdev_io returns the buffer to the pool right away, and freeing the part
	 * bdev_io must not release it a second time. */
	rc = spdk_bdev_read_blocks(ctx.desc, ctx.io_ch, NULL, 0, 1, part_ut_io_done, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_part_read_done == true);
	CU_ASSERT(g_part_read_success == false);
	SPDK_CU_ASSERT_FATAL(g_part_read_io != NULL);
	SPDK_CU_ASSERT_FATAL(g_base_read_buf != NULL);
	CU_ASSERT(g_part_read_io->internal.f.has_buf == false);

	/* The child bdev_io was freed with the buffer still owned by it, so the
	 * buffer is already back in the pool (the per-channel cache is LIFO). */
	mgmt_ch = g_part_read_io->internal.ch->shared_resource->mgmt_ch;
	probe_buf = spdk_iobuf_get(&mgmt_ch->iobuf, 512, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(probe_buf != NULL);
	CU_ASSERT(probe_buf == g_base_read_buf);
	spdk_iobuf_put(&mgmt_ch->iobuf, probe_buf, 512);

	spdk_bdev_free_io(g_part_read_io);

	/* Freeing the part bdev_io must not return the buffer a second time -
	 * a double free would make two consecutive allocations hand out the
	 * same buffer. */
	probe_buf = spdk_iobuf_get(&mgmt_ch->iobuf, 512, NULL, NULL);
	probe_buf2 = spdk_iobuf_get(&mgmt_ch->iobuf, 512, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(probe_buf != NULL);
	SPDK_CU_ASSERT_FATAL(probe_buf2 != NULL);
	CU_ASSERT(probe_buf == g_base_read_buf);
	CU_ASSERT(probe_buf2 != g_base_read_buf);
	spdk_iobuf_put(&mgmt_ch->iobuf, probe_buf2, 512);
	spdk_iobuf_put(&mgmt_ch->iobuf, probe_buf, 512);

	ut_part_io_test_teardown(&ctx);
}

static void
part_read_user_buf_test(void)
{
	struct part_io_test_ctx ctx = {};
	uint8_t user_buf[512] = {};
	int rc;

	ut_part_io_test_setup(&ctx);

	/* Read with a caller-provided buffer: nothing is allocated on the child bdev_io,
	 * so no ownership is transferred to the part bdev_io. */
	rc = spdk_bdev_read_blocks(ctx.desc, ctx.io_ch, user_buf, 0, 1, part_ut_io_done, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();

	CU_ASSERT(g_part_read_done == true);
	CU_ASSERT(g_part_read_success == true);
	SPDK_CU_ASSERT_FATAL(g_part_read_io != NULL);
	CU_ASSERT(g_base_read_buf == NULL);
	CU_ASSERT(g_part_read_io->internal.f.has_buf == false);
	CU_ASSERT(g_part_read_io->u.bdev.iovs[0].iov_base == user_buf);
	CU_ASSERT(user_buf[0] == PART_UT_READ_PATTERN);
	CU_ASSERT(user_buf[511] == PART_UT_READ_PATTERN);

	spdk_bdev_free_io(g_part_read_io);

	ut_part_io_test_teardown(&ctx);
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("bdev_part", ut_part_setup, ut_part_teardown);

	CU_ADD_TEST(suite, part_test);
	CU_ADD_TEST(suite, part_free_test);
	CU_ADD_TEST(suite, part_get_io_channel_test);
	CU_ADD_TEST(suite, part_construct_ext);
	CU_ADD_TEST(suite, part_read_bdev_buf_ownership_test);
	CU_ADD_TEST(suite, part_read_bdev_buf_ownership_failed_io_test);
	CU_ADD_TEST(suite, part_read_user_buf_test);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();
	free_cores();

	return num_failures;
}
