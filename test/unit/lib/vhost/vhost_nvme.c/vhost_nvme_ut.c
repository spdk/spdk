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
#include "common/lib/test_env.c"

#include "vhost/vhost_nvme.c"
#include "unit/lib/vhost/test_vhost.c"

#include "spdk/bdev_module.h"
#include "spdk/env.h"


DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
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
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0x1);
DEFINE_STUB(spdk_bdev_has_write_cache, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_unmap, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   uint64_t offset, uint64_t nbytes, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_flush, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   uint64_t offset, uint64_t length, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_optimal_io_boundary, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_conf_section_get_intval, int, (struct spdk_conf_section *sp, const char *key), 0);
DEFINE_STUB_V(spdk_bdev_io_get_nvme_status, (const struct spdk_bdev_io *bdev_io, int *sct, int *sc))
DEFINE_STUB(spdk_conf_section_get_nval, char *,
	    (struct spdk_conf_section *sp, const char *key, int idx), NULL);

SPDK_LOG_REGISTER_COMPONENT("vhost", SPDK_LOG_VHOST)

static void
vhost_nvme_controller_construct_test(void)
{
	int rc;

	MOCK_SET_P(spdk_conf_next_section, struct spdk_conf_section *, NULL);

	/* VhostNvme section has non numeric suffix */
	MOCK_SET(spdk_conf_section_match_prefix, bool, true);
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostNvmex");
	rc = spdk_vhost_nvme_controller_construct();
	CU_ASSERT(rc != 0);

	/* Device has no name */
	MOCK_SET_P(spdk_conf_section_get_name, const char *, "VhostNvme0");
	MOCK_SET_P(spdk_conf_section_get_val, char *, NULL);
	rc = spdk_vhost_nvme_controller_construct();
	CU_ASSERT(rc != 0);
}

static void
vhost_nvme_dev_construct_test(void)
{
	int rc;

	/* Failed to construct vhost device */
	MOCK_SET(spdk_vhost_dev_register_fail, bool, true);
	rc = spdk_vhost_nvme_dev_construct("vhost.0", "0x1", 4);
	CU_ASSERT(rc != 0);

	/* Bigger num_io_queues */
	rc = spdk_vhost_nvme_dev_construct("vhost.0", "0x1", MAX_IO_QUEUES + 1);
	CU_ASSERT(rc != 0);
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

	suite = CU_add_suite("vhost_nvme_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "vhost_nvme_controller_construct",
			    vhost_nvme_controller_construct_test) == NULL ||
		CU_add_test(suite, "vhost_nvme_dev_construct", vhost_nvme_dev_construct_test) == NULL
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
