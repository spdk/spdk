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
#include "spdk_internal/mock.h"
#include "unit/lib/json_mock.c"

#include <rte_compressdev.h>

/* Those functions are defined as static inline in DPDK, so we can't
 * mock them straight away. We use defines to redirect them into
 * our custom functions.
 */

void __rte_experimental mock_rte_compressdev_info_get(uint8_t dev_id,
		struct rte_compressdev_info *dev_info);
#define rte_compressdev_info_get mock_rte_compressdev_info_get
void __rte_experimental
mock_rte_compressdev_info_get(uint8_t dev_id, struct rte_compressdev_info *dev_info)
{
	/* TODO return global struct */
}

int __rte_experimental mock_rte_compressdev_configure(uint8_t dev_id,
		struct rte_compressdev_config *config);
#define rte_compressdev_configure mock_rte_compressdev_configure
int __rte_experimental
mock_rte_compressdev_configure(uint8_t dev_id, struct rte_compressdev_config *config)
{
	return 0;
}

int __rte_experimental mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
		uint32_t max_inflight_ops, int socket_id);
#define rte_compressdev_queue_pair_setup mock_rte_compressdev_queue_pair_setup
int __rte_experimental
mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
				      uint32_t max_inflight_ops, int socket_id)
{
	return 0;
}

int __rte_experimental mock_rte_compressdev_start(uint8_t dev_id);
#define rte_compressdev_start mock_rte_compressdev_start
int __rte_experimental
mock_rte_compressdev_start(uint8_t dev_id)
{
	return 0;
}

int __rte_experimental mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform);
#define rte_compressdev_private_xform_create mock_rte_compressdev_private_xform_create
int __rte_experimental
mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform)
{
	return 0;
}

uint8_t __rte_experimental mock_rte_compressdev_count(void);
#define rte_compressdev_count mock_rte_compressdev_count
uint8_t __rte_experimental
mock_rte_compressdev_count(void)
{
	return 0;
}

struct rte_mempool *__rte_experimental mock_rte_comp_op_pool_create(const char *name,
		unsigned int nb_elts, unsigned int cache_size, uint16_t user_size,
		int socket_id);
#define rte_comp_op_pool_create mock_rte_comp_op_pool_create
struct rte_mempool *__rte_experimental
mock_rte_comp_op_pool_create(const char *name, unsigned int nb_elts,
			     unsigned int cache_size, uint16_t user_size, int socket_id)
{
	return NULL;
}

#include "bdev/compress/vbdev_compress.c"

/* SPDK stubs */
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write,
				  spdk_bdev_remove_cb_t remove_cb,
				  void *remove_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_env_get_socket_id, uint32_t, (uint32_t core), 0);
DEFINE_STUB_V(spdk_reduce_vol_readv, (struct spdk_reduce_vol *vol,
				      struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
				      spdk_reduce_vol_op_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_writev, (struct spdk_reduce_vol *vol,
				       struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
				       spdk_reduce_vol_op_complete cb_fn, void *cb_arg));
DEFINE_STUB(spdk_bdev_io_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_io *bdev_io),
	    0);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB_V(spdk_reduce_vol_unload, (struct spdk_reduce_vol *vol,
				       spdk_reduce_vol_op_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_load, (struct spdk_reduce_backing_dev *backing_dev,
				     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg));

/* DPDK stubs */
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_eal_get_configuration, struct rte_config *, (void), NULL);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB_V(rte_mempool_free, (struct rte_mempool *mp));

struct spdk_bdev_io *g_bdev_io;
struct spdk_io_channel *g_io_ch;

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(g_io_ch, g_bdev_io, true);
}

/* Mock these functions to call the callback and then return the value we require */
int ut_spdk_bdev_readv_blocks = 0;
bool ut_spdk_bdev_readv_blocks_mocked = false;
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_readv_blocks, cb_arg);
	return ut_spdk_bdev_readv_blocks;
}

int ut_spdk_bdev_writev_blocks = 0;
bool ut_spdk_bdev_writev_blocks_mocked = false;
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_writev_blocks, cb_arg);
	return ut_spdk_bdev_writev_blocks;
}

int ut_spdk_bdev_unmap_blocks = 0;
bool ut_spdk_bdev_unmap_blocks_mocked = false;
int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_unmap_blocks, cb_arg);
	return ut_spdk_bdev_unmap_blocks;
}

int ut_spdk_bdev_flush_blocks = 0;
bool ut_spdk_bdev_flush_blocks_mocked = false;
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		       void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_flush_blocks, cb_arg);
	return ut_spdk_bdev_flush_blocks;
}

int ut_spdk_bdev_reset = 0;
bool ut_spdk_bdev_reset_mocked = false;
int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_reset, cb_arg);
	return ut_spdk_bdev_reset;
}

bool g_completion_called = false;
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
	g_completion_called = true;
}


/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{

	return 0;
}

static void
test_error_paths(void)
{

}

static void
test_simple_write(void)
{
}

static void
test_simple_read(void)
{
}

static void
test_large_rw(void)
{
}

static void
test_passthru(void)
{

}

static void
test_reset(void)
{
	/* TODO: There are a few different ways to do this given that
	 * the code uses spdk_for_each_channel() to implement reset
	 * handling. SUbmitting w/o UT for this function for now and
	 * will follow up with something shortly.
	 */
}

static void
test_initdrivers(void)
{
}

static void
test_supported_io(void)
{

}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("compress", test_setup, test_cleanup);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "test_error_paths",
			test_error_paths) == NULL ||
	    CU_add_test(suite, "test_simple_write",
			test_simple_write) == NULL ||
	    CU_add_test(suite, "test_simple_read",
			test_simple_read) == NULL ||
	    CU_add_test(suite, "test_large_rw",
			test_large_rw) == NULL ||
	    CU_add_test(suite, "test_passthru",
			test_passthru) == NULL ||
	    CU_add_test(suite, "test_initdrivers",
			test_initdrivers) == NULL ||
	    CU_add_test(suite, "test_supported_io",
			test_supported_io) == NULL ||
	    CU_add_test(suite, "test_reset",
			test_reset) == NULL
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
