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

#include "lib/test_env.c"

/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

SPDK_DECLARE_BDEV_MODULE(vbdev_ut);

void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			 int *sc, int *sk, int *asc, int *ascq)
{
}

static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}

static int
stub_destruct(void *ctx)
{
	return 0;
}

static struct spdk_bdev_fn_table fn_table = {
	.destruct = stub_destruct,
};

static void
vbdev_ut_examine(struct spdk_bdev *bdev)
{
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(vbdev_ut));
}

SPDK_BDEV_MODULE_REGISTER(bdev_ut, NULL, NULL, NULL, NULL, NULL)
SPDK_BDEV_MODULE_REGISTER(vbdev_ut, NULL, NULL, NULL, NULL, vbdev_ut_examine)

static struct spdk_bdev *
allocate_bdev(char *name)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(bdev_ut);

	rc = spdk_bdev_register(bdev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&bdev->base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev->vbdevs));

	return bdev;
}

static struct spdk_bdev *
allocate_vbdev(char *name, struct spdk_bdev *base1, struct spdk_bdev *base2)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *array[2];
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(vbdev_ut);

	/* vbdev must have at least one base bdev */
	CU_ASSERT(base1 != NULL);

	array[0] = base1;
	array[1] = base2;

	rc = spdk_vbdev_register(bdev, array, base2 == NULL ? 1 : 2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&bdev->base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev->vbdevs));

	return bdev;
}

static void
free_bdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev, NULL, NULL);
	free(bdev);
}

static void
free_vbdev(struct spdk_bdev *bdev)
{
	CU_ASSERT(!TAILQ_EMPTY(&bdev->base_bdevs));
	spdk_bdev_unregister(bdev, NULL, NULL);
	free(bdev);
}

static void
open_write_test(void)
{
	struct spdk_bdev *bdev[8];
	struct spdk_bdev_desc *desc[8] = {};
	int rc;

	/*
	 * Create a tree of bdevs to test various open w/ write cases.
	 *
	 * bdev0 through bdev2 are physical block devices, such as NVMe
	 * namespaces or Ceph block devices.
	 *
	 * bdev3 is a virtual bdev with multiple base bdevs.  This models
	 * caching or RAID use cases.
	 *
	 * bdev4 through bdev6 are all virtual bdevs with the same base
	 * bdev.  This models partitioning or logical volume use cases.
	 *
	 * bdev7 is a virtual bdev with multiple base bdevs, but these
	 * base bdevs are themselves virtual bdevs.
	 *
	 *                bdev7
	 *                  |
	 *            +----------+
	 *            |          |
	 *          bdev3      bdev4   bdev5   bdev6
	 *            |          |       |       |
	 *        +---+---+      +-------+-------+
	 *        |       |              |
	 *      bdev0   bdev1          bdev2
	 */

	bdev[0] = allocate_bdev("bdev0");
	rc = spdk_bdev_module_claim_bdev(bdev[0], NULL, SPDK_GET_BDEV_MODULE(bdev_ut));
	CU_ASSERT(rc == 0);

	bdev[1] = allocate_bdev("bdev1");
	rc = spdk_bdev_module_claim_bdev(bdev[1], NULL, SPDK_GET_BDEV_MODULE(bdev_ut));
	CU_ASSERT(rc == 0);

	bdev[2] = allocate_bdev("bdev2");
	rc = spdk_bdev_module_claim_bdev(bdev[2], NULL, SPDK_GET_BDEV_MODULE(bdev_ut));
	CU_ASSERT(rc == 0);

	bdev[3] = allocate_vbdev("bdev3", bdev[0], bdev[1]);
	rc = spdk_bdev_module_claim_bdev(bdev[3], NULL, SPDK_GET_BDEV_MODULE(bdev_ut));
	CU_ASSERT(rc == 0);

	bdev[4] = allocate_vbdev("bdev4", bdev[2], NULL);
	rc = spdk_bdev_module_claim_bdev(bdev[4], NULL, SPDK_GET_BDEV_MODULE(bdev_ut));
	CU_ASSERT(rc == 0);

	bdev[5] = allocate_vbdev("bdev5", bdev[2], NULL);
	bdev[6] = allocate_vbdev("bdev6", bdev[2], NULL);

	bdev[7] = allocate_vbdev("bdev7", bdev[3], bdev[4]);

	/* Open bdev0 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[0], false, NULL, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[0] != NULL);
	spdk_bdev_close(desc[0]);

	/*
	 * Open bdev1 read/write.  This should fail since bdev1 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[1], true, NULL, NULL, &desc[1]);
	CU_ASSERT(rc == -EPERM);

	/*
	 * Open bdev3 read/write.  This should fail since bdev3 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[3], true, NULL, NULL, &desc[3]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev3 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[3], false, NULL, NULL, &desc[3]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[3] != NULL);
	spdk_bdev_close(desc[3]);

	/*
	 * Open bdev7 read/write.  This should succeed since it is a leaf
	 * bdev.
	 */
	rc = spdk_bdev_open(bdev[7], true, NULL, NULL, &desc[7]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[7] != NULL);
	spdk_bdev_close(desc[7]);

	/*
	 * Open bdev4 read/write.  This should fail since bdev4 has been claimed
	 * by a vbdev module.
	 */
	rc = spdk_bdev_open(bdev[4], true, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(bdev[4], false, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[4] != NULL);
	spdk_bdev_close(desc[4]);

	free_vbdev(bdev[7]);

	free_vbdev(bdev[3]);
	free_vbdev(bdev[4]);
	free_vbdev(bdev[5]);
	free_vbdev(bdev[6]);

	free_bdev(bdev[0]);
	free_bdev(bdev[1]);
	free_bdev(bdev[2]);

}

static void
bytes_to_blocks_test(void)
{
	struct spdk_bdev bdev;
	uint64_t offset_blocks, num_blocks;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;

	/* All parameters valid */
	offset_blocks = 0;
	num_blocks = 0;
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 1024, &num_blocks) == 0);
	CU_ASSERT(offset_blocks == 1);
	CU_ASSERT(num_blocks == 2);

	/* Offset not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 3, &offset_blocks, 512, &num_blocks) != 0);

	/* Length not a block multiple */
	CU_ASSERT(spdk_bdev_bytes_to_blocks(&bdev, 512, &offset_blocks, 3, &num_blocks) != 0);
}

static void
num_blocks_test(void)
{
	struct spdk_bdev bdev;
	struct spdk_bdev_desc *desc;

	memset(&bdev, 0, sizeof(bdev));
	bdev.name = "num_blocks";
	bdev.fn_table = &fn_table;
	bdev.module = SPDK_GET_BDEV_MODULE(bdev_ut);
	spdk_bdev_register(&bdev);
	spdk_bdev_notify_blockcnt_change(&bdev, 50);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 70) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 30) == 0);

	/* In case bdev opened */
	spdk_bdev_open(&bdev, false, NULL, NULL, &desc);

	/* Growing block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 80) == 0);
	/* Shrinking block number */
	CU_ASSERT(spdk_bdev_notify_blockcnt_change(&bdev, 20) != 0);

	spdk_bdev_close(desc);
	spdk_bdev_unregister(&bdev, NULL, NULL);
}

static void
io_valid_test(void)
{
	struct spdk_bdev bdev;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;
	spdk_bdev_notify_blockcnt_change(&bdev, 100);

	/* All parameters valid */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 1, 2) == true);

	/* Last valid block */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 1) == true);

	/* Offset past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 100, 1) == false);

	/* Offset + length past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 99, 2) == false);

	/* Offset near end of uint64_t range (2^64 - 1) */
	CU_ASSERT(spdk_bdev_io_valid_blocks(&bdev, 18446744073709551615ULL, 1) == false);
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
	struct spdk_bdev_part		part1, part2;
	struct spdk_bdev		bdev_base = {};
	SPDK_BDEV_PART_TAILQ		tailq = TAILQ_HEAD_INITIALIZER(tailq);
	int rc;

	base = calloc(1, sizeof(*base));
	SPDK_CU_ASSERT_FATAL(base != NULL);

	bdev_base.name = "base";
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = SPDK_GET_BDEV_MODULE(bdev_ut);
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);
	spdk_bdev_part_base_construct(base, &bdev_base, NULL, SPDK_GET_BDEV_MODULE(vbdev_ut),
				      &part_fn_table, &tailq, __base_free, 0, NULL, NULL);

	spdk_bdev_part_construct(&part1, base, "test1", 0, 100, "test");
	spdk_bdev_part_construct(&part2, base, "test2", 100, 100, "test");

	spdk_bdev_part_base_hotremove(&bdev_base, &tailq);

	/*
	 * The base device was removed - ensure that the partition vbdevs were
	 *  removed from the base's vbdev list.
	 */
	CU_ASSERT(TAILQ_EMPTY(&bdev_base.vbdevs));

	spdk_bdev_part_base_free(base);
	spdk_bdev_unregister(&bdev_base, NULL, NULL);
}

static void
alias_add_del_test(void)
{
	struct spdk_bdev *bdev[2];
	int rc;

	/* Creating and registering bdevs */
	bdev[0] = allocate_bdev("bdev0");
	SPDK_CU_ASSERT_FATAL(bdev[0] != 0);

	bdev[1] = allocate_bdev("bdev1");
	SPDK_CU_ASSERT_FATAL(bdev[1] != 0);

	/*
	 * Trying adding an alias identical to name.
	 * Alias is identical to name, so it can not be added to aliases list
	 */
	rc = spdk_bdev_alias_add(bdev[0], bdev[0]->name);
	CU_ASSERT(rc == -EEXIST);

	/*
	 * Trying to add empty alias,
	 * this one should fail
	 */
	rc = spdk_bdev_alias_add(bdev[0], NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Trying adding same alias to two different registered bdevs */

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias was added to another bdev, so this one should fail */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 0");
	CU_ASSERT(rc == -EEXIST);

	/* Alias is used first time, so this one should pass */
	rc = spdk_bdev_alias_add(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying removing an alias from registered bdevs */

	/* Alias is not on a bdev aliases list, so this one should fail */
	rc = spdk_bdev_alias_del(bdev[0], "not existing");
	CU_ASSERT(rc == -ENOENT);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[0], "proper alias 0");
	CU_ASSERT(rc == 0);

	/* Alias is present on a bdev aliases list, so this one should pass */
	rc = spdk_bdev_alias_del(bdev[1], "proper alias 1");
	CU_ASSERT(rc == 0);

	/* Trying to remove name instead of alias, so this one should fail, name cannot be changed or removed */
	rc = spdk_bdev_alias_del(bdev[0], bdev[0]->name);
	CU_ASSERT(rc != 0);

	/* Unregister and free bdevs */
	spdk_bdev_unregister(bdev[0], NULL, NULL);
	spdk_bdev_unregister(bdev[1], NULL, NULL);

	free(bdev[0]);
	free(bdev[1]);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "bytes_to_blocks_test", bytes_to_blocks_test) == NULL ||
		CU_add_test(suite, "num_blocks_test", num_blocks_test) == NULL ||
		CU_add_test(suite, "io_valid", io_valid_test) == NULL ||
		CU_add_test(suite, "open_write", open_write_test) == NULL ||
		CU_add_test(suite, "part", part_test) == NULL ||
		CU_add_test(suite, "alias_add_del", alias_add_del_test) == NULL
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
