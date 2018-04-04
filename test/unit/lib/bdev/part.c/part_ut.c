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

#include "spdk_cunit.h"

#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"

/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"
#include "bdev/part.c"

/* Return NULL to test hardcoded defaults. */
struct spdk_conf_section *
spdk_conf_find_section(struct spdk_conf *cp, const char *name)
{
	return NULL;
}

/* Return NULL to test hardcoded defaults. */
char *
spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key, int idx1, int idx2)
{
	return NULL;
}

static void
_part_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
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
	.examine = vbdev_ut_examine,
};

SPDK_BDEV_MODULE_REGISTER(&bdev_ut_if)
SPDK_BDEV_MODULE_REGISTER(&vbdev_ut_if)

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
__base_free(struct spdk_bdev_part_base *base)
{
	free(base);
}

static void
part_test(void)
{
	struct spdk_bdev_part_base	*base;
	struct spdk_bdev_part		part1 = {};
	struct spdk_bdev_part		part2 = {};
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	int rc;

	base = calloc(1, sizeof(*base));
	SPDK_CU_ASSERT_FATAL(base != NULL);

	bdev_base.name = "base";
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	spdk_bdev_part_base_construct(base, &bdev_base, NULL, &vbdev_ut_if,
				      &part_fn_table, &tailq, __base_free, 0, NULL, NULL);

	spdk_bdev_part_construct(&part1, base, "test1", 0, 100, "test");
	spdk_bdev_part_construct(&part2, base, "test2", 100, 100, "test");

	spdk_bdev_part_base_hotremove(&bdev_base, &tailq);

	/*
	 * The base device was removed - ensure that the partition vbdevs were
	 *  removed from the base's vbdev list.
	 */
	CU_ASSERT(bdev_base.vbdevs_cnt == 0);

	spdk_bdev_part_base_free(base);
	spdk_bdev_unregister(&bdev_base, NULL, NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev_part", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "part", part_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	spdk_allocate_thread(_part_send_msg, NULL, NULL, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	return num_failures;
}
