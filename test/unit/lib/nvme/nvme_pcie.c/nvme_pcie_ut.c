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

#include "lib/test_env.c"

#include "nvme/nvme_pcie.c"

DEFINE_STUB(spdk_mem_register, int, (void *vaddr, size_t len, uint64_t paddr), 0);
DEFINE_STUB(spdk_mem_unregister, int, (void *vaddr, size_t len), 0);

struct spdk_trace_flag SPDK_LOG_NVME = {
	.name = "nvme",
	.enabled = false,
};

static struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};
struct nvme_driver *g_spdk_nvme_driver = &_g_nvme_driver;

int32_t spdk_nvme_retry_count = 1;

struct nvme_request *g_request = NULL;

extern bool ut_fail_vtophys;

bool fail_next_sge = false;

struct io_request {
	uint64_t address_offset;
	bool	invalid_addr;
	bool	invalid_second_addr;
};

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
	abort();
}

int
spdk_uevent_connect(void)
{
	abort();
}

int
spdk_get_uevent(int fd, struct spdk_uevent *uevent)
{
	abort();
}

struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *dev)
{
	abort();
}

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests)
{
	abort();
}

int
spdk_pci_nvme_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	abort();
}

int
spdk_pci_nvme_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
			    struct spdk_pci_addr *pci_address)
{
	abort();
}

void
spdk_pci_device_detach(struct spdk_pci_device *device)
{
	abort();
}

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	abort();
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
	abort();
}

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *dev)
{
	abort();
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset)
{
	abort();
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset)
{
	abort();
}

int
spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr)
{
	abort();
}

int
nvme_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr)
{
	abort();
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	abort();
}

int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	abort();
}

void
nvme_ctrlr_free_processes(struct spdk_nvme_ctrlr *ctrlr)
{
	abort();
}

struct spdk_pci_device *
nvme_ctrlr_proc_get_devhandle(struct spdk_nvme_ctrlr *ctrlr)
{
	abort();
}

int
nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid, void *devhandle,
		 spdk_nvme_probe_cb probe_cb, void *cb_ctx)
{
	abort();
}

int
nvme_ctrlr_get_cap(struct spdk_nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap)
{
	abort();
}

void
nvme_ctrlr_init_cap(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_cap_register *cap)
{
	abort();
}

uint64_t
nvme_get_quirks(const struct spdk_pci_id *id)
{
	abort();
}

void
nvme_free_request(struct nvme_request *req)
{
	abort();
}

bool
nvme_completion_is_retry(const struct spdk_nvme_cpl *cpl)
{
	abort();
}

void
nvme_qpair_print_command(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd)
{
	abort();
}

void
nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cpl *cpl)
{
	abort();
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	abort();
}

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_request *req)
{
	abort();
}

struct nvme_request *
nvme_allocate_request_null(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	abort();
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	abort();
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	abort();
}

void
nvme_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	abort();
}

struct spdk_nvme_ctrlr *
spdk_nvme_get_ctrlr_by_trid_unsafe(const struct spdk_nvme_transport_id *trid)
{
	return NULL;
}

union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts = {};

	return csts;
}

#if 0 /* TODO: update PCIe-specific unit test */
static void
nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	struct io_request *req = (struct io_request *)cb_arg;

	req->address_offset = 0;
	req->invalid_addr = false;
	req->invalid_second_addr = false;
	switch (sgl_offset) {
	case 0:
		req->invalid_addr = false;
		break;
	case 1:
		req->invalid_addr = true;
		break;
	case 2:
		req->invalid_addr = false;
		req->invalid_second_addr = true;
		break;
	default:
		break;
	}
	return;
}

static int
nvme_request_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;

	if (req->address_offset == 0) {
		if (req->invalid_addr) {
			*address = (void *)7;
		} else {
			*address = (void *)(4096 * req->address_offset);
		}
	} else if (req->address_offset == 1) {
		if (req->invalid_second_addr) {
			*address = (void *)7;
		} else {
			*address = (void *)(4096 * req->address_offset);
		}
	} else {
		*address = (void *)(4096 * req->address_offset);
	}

	req->address_offset += 1;
	*length = 4096;

	if (fail_next_sge) {
		return - 1;
	} else {
		return 0;
	}

}

static void
prepare_submit_request_test(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_ctrlr *ctrlr)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->free_io_qids = NULL;
	TAILQ_INIT(&ctrlr->active_io_qpairs);
	TAILQ_INIT(&ctrlr->active_procs);
	nvme_qpair_init(qpair, 1, ctrlr, 0);

	ut_fail_vtophys = false;
}

static void
cleanup_submit_request_test(struct spdk_nvme_qpair *qpair)
{
}

static void
ut_insert_cq_entry(struct spdk_nvme_qpair *qpair, uint32_t slot)
{
	struct nvme_request	*req;
	struct nvme_tracker 	*tr;
	struct spdk_nvme_cpl	*cpl;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);
	memset(req, 0, sizeof(*req));

	tr = TAILQ_FIRST(&qpair->free_tr);
	TAILQ_REMOVE(&qpair->free_tr, tr, tq_list); /* remove tr from free_tr */
	TAILQ_INSERT_HEAD(&qpair->outstanding_tr, tr, tq_list);
	req->cmd.cid = tr->cid;
	tr->req = req;
	qpair->tr[tr->cid].active = true;

	cpl = &qpair->cpl[slot];
	cpl->status.p = qpair->phase;
	cpl->cid = tr->cid;
}

static void
expected_success_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(!spdk_nvme_cpl_is_error(cpl));
}

static void
expected_failure_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(spdk_nvme_cpl_is_error(cpl));
}

static void
test4(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct spdk_nvme_ctrlr		ctrlr = {};
	char				payload[4096];

	prepare_submit_request_test(&qpair, &ctrlr);

	req = nvme_allocate_request_contig(payload, sizeof(payload), expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Force vtophys to return a failure.  This should
	 *  result in the nvme_qpair manually failing
	 *  the request with error status to signify
	 *  a bad payload buffer.
	 */
	ut_fail_vtophys = true;

	CU_ASSERT(qpair.sq_tail == 0);

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);

	CU_ASSERT(qpair.sq_tail == 0);

	cleanup_submit_request_test(&qpair);
}

static void
test_sgl_req(void)
{
	struct spdk_nvme_qpair	qpair = {};
	struct nvme_request	*req;
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct nvme_payload	payload = {};
	struct nvme_tracker 	*sgl_tr = NULL;
	uint64_t 		i;
	struct io_request	io_req = {};

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = nvme_request_reset_sgl;
	payload.u.sgl.next_sge_fn = nvme_request_next_sge;
	payload.u.sgl.cb_arg = &io_req;

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	req->payload_offset = 1;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);
	CU_ASSERT(qpair.sq_tail == 0);
	cleanup_submit_request_test(&qpair);

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	spdk_nvme_retry_count = 1;
	fail_next_sge = true;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);
	CU_ASSERT(qpair.sq_tail == 0);
	cleanup_submit_request_test(&qpair);

	fail_next_sge = false;

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, 2 * 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 15 | 0;
	req->payload_offset = 2;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);
	CU_ASSERT(qpair.sq_tail == 0);
	cleanup_submit_request_test(&qpair);

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 4095 | 0;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) == 0);

	CU_ASSERT(req->cmd.dptr.prp.prp1 == 0);
	CU_ASSERT(qpair.sq_tail == 1);
	sgl_tr = TAILQ_FIRST(&qpair.outstanding_tr);
	if (sgl_tr != NULL) {
		for (i = 0; i < NVME_MAX_PRP_LIST_ENTRIES; i++) {
			CU_ASSERT(sgl_tr->u.prp[i] == (0x1000 * (i + 1)));
		}

		TAILQ_REMOVE(&qpair.outstanding_tr, sgl_tr, tq_list);
	}
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);
}

static void
test_hw_sgl_req(void)
{
	struct spdk_nvme_qpair	qpair = {};
	struct nvme_request	*req;
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct nvme_payload	payload = {};
	struct nvme_tracker 	*sgl_tr = NULL;
	uint64_t 		i;
	struct io_request	io_req = {};

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = nvme_request_reset_sgl;
	payload.u.sgl.next_sge_fn = nvme_request_next_sge;
	payload.u.sgl.cb_arg = &io_req;

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	req->payload_offset = 0;
	ctrlr.flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;

	nvme_qpair_submit_request(&qpair, req);

	sgl_tr = TAILQ_FIRST(&qpair.outstanding_tr);
	CU_ASSERT(sgl_tr != NULL);
	CU_ASSERT(sgl_tr->u.sgl[0].generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(sgl_tr->u.sgl[0].generic.subtype == 0);
	CU_ASSERT(sgl_tr->u.sgl[0].unkeyed.length == 4096);
	CU_ASSERT(sgl_tr->u.sgl[0].address == 0);
	CU_ASSERT(req->cmd.dptr.sgl1.generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	TAILQ_REMOVE(&qpair.outstanding_tr, sgl_tr, tq_list);
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);

	prepare_submit_request_test(&qpair, &ctrlr);
	req = nvme_allocate_request(&payload, NVME_MAX_SGL_DESCRIPTORS * 0x1000, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 2023 | 0;
	req->payload_offset = 0;
	ctrlr.flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;

	nvme_qpair_submit_request(&qpair, req);

	sgl_tr = TAILQ_FIRST(&qpair.outstanding_tr);
	CU_ASSERT(sgl_tr != NULL);
	for (i = 0; i < NVME_MAX_SGL_DESCRIPTORS; i++) {
		CU_ASSERT(sgl_tr->u.sgl[i].generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
		CU_ASSERT(sgl_tr->u.sgl[i].generic.subtype == 0);
		CU_ASSERT(sgl_tr->u.sgl[i].unkeyed.length == 4096);
		CU_ASSERT(sgl_tr->u.sgl[i].address == i * 4096);
	}
	CU_ASSERT(req->cmd.dptr.sgl1.generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	TAILQ_REMOVE(&qpair.outstanding_tr, sgl_tr, tq_list);
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);
}

static void test_nvme_qpair_fail(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req = NULL;
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct nvme_tracker		*tr_temp;

	prepare_submit_request_test(&qpair, &ctrlr);

	tr_temp = TAILQ_FIRST(&qpair.free_tr);
	SPDK_CU_ASSERT_FATAL(tr_temp != NULL);
	TAILQ_REMOVE(&qpair.free_tr, tr_temp, tq_list);
	tr_temp->req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(tr_temp->req != NULL);
	tr_temp->req->cmd.cid = tr_temp->cid;

	TAILQ_INSERT_HEAD(&qpair.outstanding_tr, tr_temp, tq_list);
	nvme_qpair_fail(&qpair);
	CU_ASSERT_TRUE(TAILQ_EMPTY(&qpair.outstanding_tr));

	req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	STAILQ_INSERT_HEAD(&qpair.queued_req, req, stailq);
	nvme_qpair_fail(&qpair);
	CU_ASSERT_TRUE(STAILQ_EMPTY(&qpair.queued_req));

	cleanup_submit_request_test(&qpair);
}

static void
test_nvme_qpair_process_completions_limit(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};

	prepare_submit_request_test(&qpair, &ctrlr);
	qpair.is_enabled = true;

	/* Insert 4 entries into the completion queue */
	CU_ASSERT(qpair.cq_head == 0);
	ut_insert_cq_entry(&qpair, 0);
	ut_insert_cq_entry(&qpair, 1);
	ut_insert_cq_entry(&qpair, 2);
	ut_insert_cq_entry(&qpair, 3);

	/* This should only process 2 completions, and 2 should be left in the queue */
	spdk_nvme_qpair_process_completions(&qpair, 2);
	CU_ASSERT(qpair.cq_head == 2);

	/* This should only process 1 completion, and 1 should be left in the queue */
	spdk_nvme_qpair_process_completions(&qpair, 1);
	CU_ASSERT(qpair.cq_head == 3);

	/* This should process the remaining completion */
	spdk_nvme_qpair_process_completions(&qpair, 5);
	CU_ASSERT(qpair.cq_head == 4);

	cleanup_submit_request_test(&qpair);
}

static void test_nvme_qpair_destroy(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct nvme_tracker		*tr_temp;

	memset(&ctrlr, 0, sizeof(ctrlr));
	TAILQ_INIT(&ctrlr.free_io_qpairs);
	TAILQ_INIT(&ctrlr.active_io_qpairs);
	TAILQ_INIT(&ctrlr.active_procs);

	nvme_qpair_init(&qpair, 1, 128, &ctrlr);
	nvme_qpair_destroy(&qpair);


	nvme_qpair_init(&qpair, 0, 128, &ctrlr);
	tr_temp = TAILQ_FIRST(&qpair.free_tr);
	SPDK_CU_ASSERT_FATAL(tr_temp != NULL);
	TAILQ_REMOVE(&qpair.free_tr, tr_temp, tq_list);
	tr_temp->req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(tr_temp->req != NULL);

	tr_temp->req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	tr_temp->req->cmd.cid = tr_temp->cid;
	TAILQ_INSERT_HEAD(&qpair.outstanding_tr, tr_temp, tq_list);

	nvme_qpair_destroy(&qpair);
	CU_ASSERT(TAILQ_EMPTY(&qpair.outstanding_tr));
}
#endif

static void
prp_list_prep(struct nvme_tracker *tr, struct nvme_request *req, uint32_t *prp_index)
{
	memset(req, 0, sizeof(*req));
	memset(tr, 0, sizeof(*tr));
	tr->req = req;
	tr->prp_sgl_bus_addr = 0xDEADBEEF;
	*prp_index = 0;
}

static void
test_prp_list_append(void)
{
	struct nvme_request req;
	struct nvme_tracker tr;
	uint32_t prp_index;

	/* Non-DWORD-aligned buffer (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100001, 0x1000, 0x1000) == -EINVAL);

	/* 512-byte buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 512-byte buffer, non-4K-aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x108000, 0x200, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x108000);

	/* 4K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);

	/* 4K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x2000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x101000);

	/* 8K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x2000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x3000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);

	/* 12K buffer, non-4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x3000, 0x1000) == 0);
	CU_ASSERT(prp_index == 4);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x102000);
	CU_ASSERT(tr.u.prp[2] == 0x103000);

	/* Two 4K buffers, both 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 1);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100000);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == 0x900000);

	/* Two 4K buffers, first non-4K aligned, second 4K aligned */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900000, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 3);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x100800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr.prp_sgl_bus_addr);
	CU_ASSERT(tr.u.prp[0] == 0x101000);
	CU_ASSERT(tr.u.prp[1] == 0x900000);

	/* Two 4K buffers, both non-4K aligned (invalid) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800, 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == 2);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x900800, 0x1000, 0x1000) == -EINVAL);
	CU_ASSERT(prp_index == 2);

	/* 4K buffer, 4K aligned, but vtophys fails */
	ut_fail_vtophys = true;
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000, 0x1000, 0x1000) == -EINVAL);
	ut_fail_vtophys = false;

	/* Largest aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Largest non-4K-aligned buffer that can be described in NVME_MAX_PRP_LIST_ENTRIES (plus PRP1) */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800,
					    NVME_MAX_PRP_LIST_ENTRIES * 0x1000, 0x1000) == 0);
	CU_ASSERT(prp_index == NVME_MAX_PRP_LIST_ENTRIES + 1);

	/* Buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100000,
					    (NVME_MAX_PRP_LIST_ENTRIES + 2) * 0x1000, 0x1000) == -EINVAL);

	/* Non-4K-aligned buffer too large to be described in NVME_MAX_PRP_LIST_ENTRIES */
	prp_list_prep(&tr, &req, &prp_index);
	CU_ASSERT(nvme_pcie_prp_list_append(&tr, &prp_index, (void *)0x100800,
					    (NVME_MAX_PRP_LIST_ENTRIES + 1) * 0x1000, 0x1000) == -EINVAL);
}

static void test_shadow_doorbell_update(void)
{
	bool ret;

	/* nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old) */
	ret = nvme_pcie_qpair_need_event(10, 15, 14);
	CU_ASSERT(ret == false);

	ret = nvme_pcie_qpair_need_event(14, 15, 14);
	CU_ASSERT(ret == true);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_pcie", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "prp_list_append", test_prp_list_append) == NULL
	    || CU_add_test(suite, "shadow_doorbell_update",
			   test_shadow_doorbell_update) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
