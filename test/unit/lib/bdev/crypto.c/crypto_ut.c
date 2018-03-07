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

/* these rte_ headers are our local copies of the DPDK headers hacked to mock some functions
 * included in them that can't be done with our mock library.
 */
#include "rte_crypto.h"
#include "rte_cryptodev.h"
#include "bdev/crypto/vbdev_crypto.c"

/* SPDK stubs */
DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *,
(struct spdk_conf *cp, const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nval, char *,
(struct spdk_conf_section *sp, const char *key, int idx), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
(struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_io_complete, (struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_writev_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_mempool_get_bulk, int, (struct spdk_mempool *mp, void **ele_arr, size_t count), 0);
DEFINE_STUB_V(spdk_mempool_put_bulk, (struct spdk_mempool *mp, void *const *ele_arr, size_t count));
DEFINE_STUB(spdk_bdev_readv_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_unmap_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_flush_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_reset, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg));
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
					  void *remove_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_vbdev_register, int, (struct spdk_bdev *vbdev, struct spdk_bdev **base_bdevs, int base_bdev_count), 0);

/* DPDK stubs */
DEFINE_STUB(rte_cryptodev_count, uint8_t, (void), 0);
DEFINE_STUB(rte_eal_get_configuration, struct rte_config*, (void), 0);
DEFINE_STUB_V(rte_mempool_free, (struct rte_mempool* mp));
DEFINE_STUB(rte_cryptodev_get_private_session_size, unsigned int, (uint8_t dev_id), 0);
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_crypto_op_pool_create, struct rte_mempool*,
	    (const char *name, enum rte_crypto_op_type type, unsigned nb_elts,
	     unsigned cache_size, uint16_t priv_size, int socket_id), 0);
DEFINE_STUB_V(rte_cryptodev_info_get, (uint8_t dev_id, struct rte_cryptodev_info *dev_info));
DEFINE_STUB(rte_cryptodev_device_count_by_driver, uint8_t, (uint8_t driver_id), 0);
DEFINE_STUB(rte_cryptodev_socket_id, int, (uint8_t dev_id), 0);
DEFINE_STUB(rte_cryptodev_configure, int, (uint8_t dev_id, struct rte_cryptodev_config *config), 0);
DEFINE_STUB(rte_cryptodev_queue_pair_setup, int, (uint8_t dev_id, uint16_t queue_pair_id,
		const struct rte_cryptodev_qp_conf *qp_conf,
		int socket_id, struct rte_mempool *session_pool), 0);
DEFINE_STUB(rte_cryptodev_start, int, (uint8_t dev_id), 0)
DEFINE_STUB_V(rte_cryptodev_stop, (uint8_t dev_id));

DEFINE_STUB(rte_cryptodev_sym_session_create, struct rte_cryptodev_sym_session*,
	      (struct rte_mempool *mempool), (struct rte_cryptodev_sym_session*)1);
DEFINE_STUB(rte_cryptodev_sym_session_clear, int, (uint8_t dev_id, struct rte_cryptodev_sym_session *sess), 0);
DEFINE_STUB(rte_cryptodev_sym_session_free, int, (struct rte_cryptodev_sym_session *sess), 0);
DEFINE_STUB(rte_cryptodev_sym_session_init, int, (uint8_t dev_id, struct rte_cryptodev_sym_session *sess,
		struct rte_crypto_sym_xform *xforms, struct rte_mempool *mempool), 0);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
void __attribute__ ((noreturn)) __rte_panic(const char *funcname, const char *format, ...) {
	abort();
}
struct rte_mempool_ops_table rte_mempool_ops_table;
struct rte_cryptodev *rte_cryptodevs;
__thread unsigned per_lcore__lcore_id = 0;

/* global vars and setup/cleanup functions used for all test functions */
enum rte_crypto_cipher_operation crypto_op;
struct spdk_bdev_io *bdev_io;
struct iovec iovs;
struct crypto_bdev_io *io_ctx;
struct crypto_io_channel *crypto_ch;
struct vbdev_pmd pmd;
struct vbdev_crypto crypto_node;
struct rte_crypto_op *op;
struct rte_mbuf mbuf = {};
struct rte_mbuf en_mbuf = {};
struct rte_crypto_op dequeued_op = {};

/* these globals are externs in our local rte_ header files so we can control
 * specific functions for mocking.
 */
uint16_t dequeue_mock;
uint16_t enqueue_mock;


static int
test_setup(void)
{
	int i;

	/* Prepare essential variables for test routines */
	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct crypto_bdev_io));
	bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec) * 128);
	crypto_ch = calloc(1, sizeof(struct crypto_io_channel));
	io_ctx = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	memset(&pmd, 0, sizeof(struct vbdev_pmd));
	memset(&crypto_node, 0, sizeof(struct vbdev_crypto));
	io_ctx->crypto_ch = crypto_ch;
	io_ctx->crypto_node = &crypto_node;
	crypto_ch->pmd = &pmd;
	op = calloc(1, sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op));

	for (i = 0; i < NUM_MBUFS; i++ ) {
		crypto_ch->crypto_ops[i] = op;
		crypto_ch->mbufs[i] = &mbuf;
		crypto_ch->en_mbufs[i] = &en_mbuf;
		crypto_ch->dequeued_ops[i] = &dequeued_op;
	}
	return 0;
}

static int
test_cleanup(void)
{
	free(io_ctx->cry_iov.iov_base);
	free(op);
	free(bdev_io->u.bdev.iovs);
	free(bdev_io);
	free(crypto_ch);
	return 0;
}

static void
test_crypto_operation(void)
{
	int rc;

	/* Single element block size read, no chaining */
	pmd.cdev_info.feature_flags = ~RTE_CRYPTODEV_FF_MBUF_SCATTER_GATHER;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.num_blocks = 1;
	bdev_io->u.bdev.iovs[0].iov_len = 512;
	crypto_node.crypto_bdev.blocklen = 512;

	enqueue_mock = dequeue_mock = 1;
	rc = _crypto_operation(bdev_io, RTE_CRYPTO_CIPHER_OP_ENCRYPT);
	CU_ASSERT(rc == 0);

}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("crypto", test_setup, test_cleanup);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "ut_crypto_operation",
			    test_crypto_operation) == NULL
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
