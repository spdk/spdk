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

#include "bdev/crypto/vbdev_crypto.c"

DEFINE_STUB(spdk_conf_find_section, struct spdk_conf_section *,
(struct spdk_conf *cp, const char *name), NULL);
DEFINE_STUB(spdk_conf_section_get_nval, char *,
(struct spdk_conf_section *sp, const char *key, int idx), NULL);
DEFINE_STUB(spdk_conf_section_get_nmval, char *,
(struct spdk_conf_section *sp, const char *key, int idx1, int idx2), NULL);

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));

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
DEFINE_STUB(rte_cryptodev_start, int, (uint8_t dev_id), 0);
DEFINE_STUB_V(rte_cryptodev_stop, (uint8_t dev_id));
DEFINE_STUB(rte_cryptodev_sym_session_create, struct rte_cryptodev_sym_session *,
	    (struct rte_mempool *mempool), 0);
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

static void
test_crypto_operation(void)
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

	suite = CU_add_suite("crypto", NULL, NULL);
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
