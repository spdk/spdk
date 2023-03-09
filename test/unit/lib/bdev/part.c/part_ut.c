/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"
#include "bdev/part.c"

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

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "test_domain");
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *domain), 0);
DEFINE_STUB_V(spdk_accel_sequence_finish,
	      (struct spdk_accel_sequence *seq, spdk_accel_completion_cb cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_accel_sequence_abort, (struct spdk_accel_sequence *seq));
DEFINE_STUB_V(spdk_accel_sequence_reverse, (struct spdk_accel_sequence *seq));
DEFINE_STUB(spdk_accel_append_copy, int,
	    (struct spdk_accel_sequence **seq, struct spdk_io_channel *ch, struct iovec *dst_iovs,
	     uint32_t dst_iovcnt, struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
	     struct iovec *src_iovs, uint32_t src_iovcnt, struct spdk_memory_domain *src_domain,
	     void *src_domain_ctx, int flags, spdk_accel_step_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_accel_get_memory_domain, struct spdk_memory_domain *, (void), NULL);

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

static struct spdk_bdev_fn_table base_fn_table = {
	.destruct		= __destruct,
	.get_io_channel = part_ut_get_io_channel,
};
static struct spdk_bdev_fn_table part_fn_table = {
	.destruct		= __destruct,
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
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_part_base_construct_ext("base", NULL, &vbdev_ut_if,
					       &part_fn_table, &tailq, NULL,
					       NULL, 0, NULL, NULL, &base);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(base != NULL);

	rc = spdk_bdev_part_construct(&part1, base, "test1", 0, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(base->ref == 1);
	SPDK_CU_ASSERT_FATAL(base->claimed == true);
	rc = spdk_bdev_part_construct(&part2, base, "test2", 100, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);
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

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bdev_part", ut_part_setup, ut_part_teardown);

	CU_ADD_TEST(suite, part_test);
	CU_ADD_TEST(suite, part_free_test);
	CU_ADD_TEST(suite, part_get_io_channel_test);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();
	free_cores();

	return num_failures;
}
