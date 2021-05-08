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

DEFINE_STUB_V(nvme_ctrlr_process_async_event, (struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_cpl *cpl));

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

static void
test_nvme_pcie_qpair_construct_destroy(void)
{
	struct spdk_nvme_io_qpair_opts opts = {};
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_cpl cpl[2] = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	size_t page_align = sysconf(_SC_PAGESIZE);
	uint64_t cmb_offset;
	int rc;

	opts.sq.paddr = 0xDEADBEEF;
	opts.cq.paddr = 0xDBADBEEF;
	opts.sq.vaddr = (void *)0xDCADBEEF;
	opts.cq.vaddr = cpl;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	cmb_offset = pctrlr.cmb.current_offset;
	/* Make sure that CMB size is big enough and includes page alignment */
	pctrlr.cmb.size = (1 << 16) + page_align;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	/* Allocate memory for destroying. */
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	/* Enable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == true);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == (((pctrlr.cmb.bar_pa + cmb_offset) + page_align - 1) & ~
					   (page_align - 1)));
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(pctrlr.cmb.current_offset == (uintptr_t)pqpair->cmd + (pqpair->num_entries * sizeof(
				struct spdk_nvme_cmd)) - (uintptr_t)pctrlr.cmb.bar_va);
	cmb_offset = pctrlr.cmb.current_offset;
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDEADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer, sq_vaddr and cq_vaddr invalid. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;
	MOCK_SET(spdk_vtophys, 0xDAADBEEF);

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDAADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000c);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);
	MOCK_CLEAR(spdk_vtophys);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_construct_destroy);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
