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
#include "spdk_internal/mock.h"
#include "spdk_internal/idxd.h"
#include "common/lib/test_env.c"

#include "idxd/idxd.h"

#include "idxd/idxd_kernel.c"

DEFINE_STUB_V(idxd_impl_register, (struct spdk_idxd_impl *impl));

static int
test_kernel_idxd_set_config(void)
{

	struct spdk_kernel_idxd_device kernel_idxd = {};
	int rc;

	kernel_idxd.wq_ctx = calloc(g_kernel_dev_cfg.num_groups, sizeof(*kernel_idxd.wq_ctx));
	SPDK_CU_ASSERT_FATAL(kernel_idxd.wq_ctx != NULL);
	kernel_idxd.wq_active_num = 1;
	rc = kernel_idxd_wq_config(&kernel_idxd);
	CU_ASSERT(rc == 0);

	free(kernel_idxd.idxd.groups);
	free(kernel_idxd.idxd.queues);
	free(kernel_idxd.wq_ctx);

	return 0;
}

static int
test_setup(void)
{
	g_kernel_dev_cfg.config_num = 0;
	g_kernel_dev_cfg.num_groups = 1;
	g_kernel_dev_cfg.total_wqs = 1;
	g_kernel_dev_cfg.total_engines = 4;

	return 0;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("idxdi_kernel", test_setup, NULL);

	CU_ADD_TEST(suite, test_kernel_idxd_set_config);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
