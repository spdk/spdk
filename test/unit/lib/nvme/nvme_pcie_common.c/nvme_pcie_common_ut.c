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

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/nvme/common_stubs.h"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);

DEFINE_STUB(nvme_ctrlr_submit_admin_request, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_request *req), 0);

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));

DEFINE_STUB(nvme_wait_for_completion, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status), 0);

DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

static void
test_nvme_pcie_ctrlr_alloc_cmb(void)
{

	struct nvme_pcie_ctrlr pctrlr = {};
	void *vaddr = NULL;
	uint64_t alignment;
	uint64_t phys_addr_var;
	uint64_t size;

	size = 64;
	alignment = 4096;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	pctrlr.cmb.size = 1 << 16;

	/* Allocate CMB */
	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == (void *)0xF9001000);
	CU_ASSERT(phys_addr_var == 0xF8001000);
	CU_ASSERT(pctrlr.cmb.current_offset == 4160);

	/* CMB size overload */
	size = 0x1000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	SPDK_CU_ASSERT_FATAL(vaddr == NULL);

	/* BAR is mapped for data */
	pctrlr.cmb.mem_register_addr = (void *)0xF0000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == NULL);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
