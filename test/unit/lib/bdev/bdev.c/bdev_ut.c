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

#include "bdev.c"

SPDK_DECLARE_BDEV_MODULE(vbdev_ut);

void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return NULL;
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size)
{
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
}

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	return NULL;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
}

void
spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
		      spdk_channel_for_each_cpl cpl)
{
}

struct spdk_thread *
spdk_io_channel_get_thread(struct spdk_io_channel *ch)
{
	return NULL;
}

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

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(bdev_ut);

	spdk_bdev_register(bdev);
	CU_ASSERT(TAILQ_EMPTY(&bdev->base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev->vbdevs));

	return bdev;
}

static struct spdk_bdev *
allocate_vbdev(char *name, struct spdk_bdev *base1, struct spdk_bdev *base2)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev *array[2];

	bdev = calloc(1, sizeof(*bdev));
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	bdev->name = name;
	bdev->fn_table = &fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(vbdev_ut);

	/* vbdev must have at least one base bdev */
	CU_ASSERT(base1 != NULL);

	array[0] = base1;
	array[1] = base2;

	spdk_vbdev_register(bdev, array, base2 == NULL ? 1 : 2);
	CU_ASSERT(!TAILQ_EMPTY(&bdev->base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev->vbdevs));

	return bdev;
}

static void
free_bdev(struct spdk_bdev *bdev)
{
	spdk_bdev_unregister(bdev);
	free(bdev);
}

static void
free_vbdev(struct spdk_bdev *bdev)
{
	spdk_vbdev_unregister(bdev);
	free(bdev);
}

static void
open_write_test(void)
{
	struct spdk_bdev *bdev[8];
	struct spdk_bdev_desc *desc[8];
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
io_valid_test(void)
{
	struct spdk_bdev bdev;

	memset(&bdev, 0, sizeof(bdev));

	bdev.blocklen = 512;
	bdev.blockcnt = 100;

	/* All parameters valid */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 512, 1024) == true);

	/* Offset not a block multiple */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 3, 512) == false);

	/* Length not a block multiple */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 512, 3) == false);

	/* Last valid block */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 50688, 512) == true);

	/* Offset past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 51200, 512) == false);

	/* Offset + length past end of bdev */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 50688, 1024) == false);

	/* Offset near end of uint64_t range (2^64 - 512) */
	CU_ASSERT(spdk_bdev_io_valid(&bdev, 18446744073709551104ULL, 512) == false);
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
		CU_add_test(suite, "io_valid", io_valid_test) == NULL ||
		CU_add_test(suite, "open_write", open_write_test) == NULL
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
