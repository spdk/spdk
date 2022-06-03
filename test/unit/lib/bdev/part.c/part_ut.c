/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "test_domain");
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *domain), 0);

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

static void
_part_cleanup(struct spdk_bdev_part *part)
{
	free(part->internal.bdev.name);
	free(part->internal.bdev.product_name);
}

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
};

static void vbdev_ut_examine(struct spdk_bdev *bdev);

struct spdk_bdev_module vbdev_ut_if = {
	.name = "vbdev_ut",
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
};
static struct spdk_bdev_fn_table part_fn_table = {
	.destruct		= __destruct,
};

static void
part_test(void)
{
	struct spdk_bdev_part_base	*base;
	struct spdk_bdev_part		part1 = {};
	struct spdk_bdev_part		part2 = {};
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
	rc = spdk_bdev_part_construct(&part2, base, "test2", 100, 100, "test");
	SPDK_CU_ASSERT_FATAL(rc == 0);

	spdk_bdev_part_base_hotremove(base, &tailq);

	spdk_bdev_part_base_free(base);
	_part_cleanup(&part1);
	_part_cleanup(&part2);
	spdk_bdev_unregister(&bdev_base, NULL, NULL);

	poll_threads();
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bdev_part", NULL, NULL);

	CU_ADD_TEST(suite, part_test);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
