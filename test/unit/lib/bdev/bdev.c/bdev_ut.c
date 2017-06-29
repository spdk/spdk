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
#include "bdev.c"

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
spdk_io_device_unregister(void *io_device)
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

static void
open_write_test(void)
{
	struct spdk_bdev bdev[8];
	struct spdk_bdev *array[8];
	struct spdk_bdev_desc *desc[8];
	int rc;

	/*
	 * Create a tree of bdevs to test various open w/ write cases.
	 * Description of this tree is described after creation is
	 * finished.
	 */

	bdev[0].name = "bdev0";
	spdk_bdev_register(&bdev[0]);
	CU_ASSERT(TAILQ_EMPTY(&bdev[0].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[0].vbdevs));

	bdev[1].name = "bdev1";
	spdk_bdev_register(&bdev[1]);
	CU_ASSERT(TAILQ_EMPTY(&bdev[1].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[1].vbdevs));

	bdev[2].name = "bdev2";
	spdk_bdev_register(&bdev[2]);
	CU_ASSERT(TAILQ_EMPTY(&bdev[2].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[2].vbdevs));

	array[0] = &bdev[0];
	array[1] = &bdev[1];
	bdev[3].name = "bdev3";
	spdk_vbdev_register(&bdev[3], array, 2);
	CU_ASSERT(!TAILQ_EMPTY(&bdev[3].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[3].vbdevs));

	array[0] = &bdev[2];
	bdev[4].name = "bdev4";
	spdk_vbdev_register(&bdev[4], array, 1);
	CU_ASSERT(!TAILQ_EMPTY(&bdev[4].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[4].vbdevs));
	bdev[5].name = "bdev5";
	spdk_vbdev_register(&bdev[5], array, 1);
	CU_ASSERT(!TAILQ_EMPTY(&bdev[5].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[5].vbdevs));
	bdev[6].name = "bdev6";
	spdk_vbdev_register(&bdev[6], array, 1);
	CU_ASSERT(!TAILQ_EMPTY(&bdev[6].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[6].vbdevs));

	array[0] = &bdev[3];
	array[1] = &bdev[4];
	bdev[7].name = "bdev7";
	spdk_vbdev_register(&bdev[7], array, 2);
	CU_ASSERT(!TAILQ_EMPTY(&bdev[7].base_bdevs));
	CU_ASSERT(TAILQ_EMPTY(&bdev[7].vbdevs));

	/*
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

	/* Open bdev0 read-only.  This should succeed. */
	rc = spdk_bdev_open(&bdev[0], false, NULL, NULL, &desc[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[0] != NULL);
	spdk_bdev_close(desc[0]);

	/* Open bdev1 read/write.  This should succeed. */
	rc = spdk_bdev_open(&bdev[1], true, NULL, NULL, &desc[1]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[1] != NULL);

	/*
	 * Open bdev3 read/write.  This should fail, since one of its
	 * base bdevs have been explicitly opened read/write.
	 */
	rc = spdk_bdev_open(&bdev[3], true, NULL, NULL, &desc[3]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev3 read-only.  This should succeed. */
	rc = spdk_bdev_open(&bdev[3], false, NULL, NULL, &desc[3]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[3] != NULL);
	spdk_bdev_close(desc[3]);

	/*
	 * Open bdev7 read/write.  This should fail, since one of its
	 * base bdevs have been explicitly opened read/write.  This
	 * test ensures the bdev code traverses through multiple levels
	 * of base bdevs.
	 */
	rc = spdk_bdev_open(&bdev[7], true, NULL, NULL, &desc[7]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev7 read-only.  This should succeed. */
	rc = spdk_bdev_open(&bdev[7], false, NULL, NULL, &desc[7]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[7] != NULL);
	spdk_bdev_close(desc[7]);

	/* Reset tree by closing remaining descriptors. */
	spdk_bdev_close(desc[1]);

	/* Open bdev7 read/write.  This should succeed. */
	rc = spdk_bdev_open(&bdev[7], true, NULL, NULL, &desc[7]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[7] != NULL);

	/*
	 * Open bdev4 read/write.  This should fail, since one of its
	 * virtual bdevs has been explicitly opened read/write.
	 */
	rc = spdk_bdev_open(&bdev[4], true, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev4 read-only.  This should succeed. */
	rc = spdk_bdev_open(&bdev[4], false, NULL, NULL, &desc[4]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[4] != NULL);
	spdk_bdev_close(desc[4]);

	/*
	 * Open bdev2 read/write.  This should fail, since one of its
	 * virtual bdevs has been explicitly opened read/write.  This
	 * test ensures the bdev code traverses through multiple levels
	 * of virtual bdevs.
	 */
	rc = spdk_bdev_open(&bdev[2], true, NULL, NULL, &desc[2]);
	CU_ASSERT(rc == -EPERM);

	/* Open bdev2 read-only.  This should succeed. */
	rc = spdk_bdev_open(&bdev[2], false, NULL, NULL, &desc[2]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(desc[2] != NULL);
	spdk_bdev_close(desc[2]);

	/* Reset tree by closing remaining descriptors. */
	spdk_bdev_close(desc[7]);
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
