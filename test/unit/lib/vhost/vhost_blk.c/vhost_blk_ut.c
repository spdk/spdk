/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "spdk/stdinc.h"

#include "CUnit/Basic.h"
#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "lib/test_env.c"

#include "vhost/vhost_blk.c"
#include "unit/lib/vhost/test_vhost.c"

#include "spdk_internal/bdev.h"
#include "spdk/env.h"

DEFINE_STUB(spdk_bdev_free_io, int, (struct spdk_bdev_io *bdev_io), 0);
DEFINE_STUB(spdk_bdev_readv, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb,
				   void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t len, spdk_bdev_io_completion_cb cb,
				    void *cb_arg), 0);
DEFINE_STUB_P(spdk_bdev_get_product_name, const char, (const struct spdk_bdev *bdev), {0});
DEFINE_STUB_P(spdk_bdev_get_name, const char, (const struct spdk_bdev *bdev), {0});
DEFINE_STUB_P(spdk_conf_section_get_val, char, (struct spdk_conf_section *sp, const char *key), {0});
DEFINE_STUB_P(spdk_bdev_get_by_name, struct spdk_bdev, (const char *bdev_name), {0});
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write,
				  spdk_bdev_remove_cb_t remove_cb, void *remove_ctx, struct spdk_bdev_desc **desc), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(rte_vhost_driver_enable_features, int, (const char *path, uint64_t features), 0);
DEFINE_STUB_P(spdk_bdev_get_io_channel, struct spdk_io_channel, (struct spdk_bdev_desc *desc), {0});

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return 512;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return 0x1;
}

bool
spdk_bdev_has_write_cache(const struct spdk_bdev *bdev)
{
	return false;
}

static void
vhost_blk_controller_construct_test(void)
{
	int rc;

	MOCK_SET_P(spdk_conf_next_section, struct spdk_conf_section *, NULL);

	/* VhostBlk section has non numeric suffix */
	MOCK_SET(spdk_conf_section_match_prefix, bool, true);
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostBlkx");
	rc = spdk_vhost_blk_controller_construct();
	CU_ASSERT(rc != 0);

	/* Device has no name */
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostBlk0");
	MOCK_SET_P(spdk_conf_section_get_val, char *, NULL);
	rc = spdk_vhost_blk_controller_construct();
	CU_ASSERT(rc != 0);
}

static struct spdk_vhost_blk_dev *
alloc_bvdev(void)
{
	struct spdk_vhost_blk_dev *bvdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_blk_dev),
					   SPDK_CACHE_LINE_SIZE, NULL);

	SPDK_CU_ASSERT_FATAL(bvdev != NULL);
	bvdev->vdev.backend = &vhost_blk_device_backend;
	return bvdev;
}

static void
vhost_blk_construct_test(void)
{
	int rc;
	struct spdk_bdev *ut_p_spdk_bdev = MOCK_PASS_THRU_P;

	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, false);
	MOCK_SET(spdk_vhost_dev_register_fail, bool, false);

	/* Create device with invalid name */
	MOCK_SET_P(spdk_bdev_get_by_name, struct spdk_bdev *, NULL);
	rc = spdk_vhost_blk_construct("vhost.0", "0x1", NULL, true);
	CU_ASSERT(rc != 0);

	/* Device could not be opened */
	MOCK_SET_P(spdk_bdev_get_by_name, struct spdk_bdev *, ut_p_spdk_bdev);
	MOCK_SET(spdk_bdev_open, int, -ENOMEM);
	rc = spdk_vhost_blk_construct("vhost.0", "0x1", "Malloc0", true);
	CU_ASSERT(rc != 0);

	/* Failed to construct controller */
	MOCK_SET(spdk_bdev_open, int, 0);
	MOCK_SET(spdk_vhost_dev_register_fail, bool, true);
	rc = spdk_vhost_blk_construct("vhost.0", "0x1", "Malloc0", true);
	CU_ASSERT(rc != 0);

	/* Failed to set readonly as a feature */
	MOCK_SET(rte_vhost_driver_enable_features, int, -1);
	rc = spdk_vhost_blk_construct("vhost.0", "0x1", "Malloc0", true);
	CU_ASSERT(rc != 0);

	/* Failed to set readonly as a feature and failed to remove controller */
	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, true);
	rc = spdk_vhost_blk_construct("vhost.0", "0x1", "Malloc0", true);
	CU_ASSERT(rc != 0);
}

static void
vhost_blk_destroy_test(void)
{
	int rc;
	struct spdk_vhost_blk_dev *bvdev = NULL;

	bvdev = alloc_bvdev();

	/* Device has an incorrect type */
	bvdev->vdev.backend = NULL;;
	rc = spdk_vhost_blk_destroy(&bvdev->vdev);
	CU_ASSERT(rc == -EINVAL);

	/* Failed to remove device */
	bvdev->vdev.backend = &vhost_blk_device_backend;
	MOCK_SET(spdk_vhost_dev_unregister_fail, bool, true);
	rc = spdk_vhost_blk_destroy(&bvdev->vdev);
	CU_ASSERT(rc == -1);

	if (rc != 0) {
		free(bvdev);
	}
}

static int
test_setup(void)
{
	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("vhost_scsi_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "vhost_blk_controller_construct", vhost_blk_controller_construct_test) == NULL ||
		CU_add_test(suite, "vhost_blk_construct_test", vhost_blk_construct_test) == NULL ||
		CU_add_test(suite, "vhost_blk_destroy", vhost_blk_destroy_test) == NULL
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
