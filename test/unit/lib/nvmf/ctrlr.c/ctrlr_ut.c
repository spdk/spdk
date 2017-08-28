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

#include "ctrlr.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	return NULL;
}

uint16_t
spdk_nvmf_tgt_gen_cntlid(struct spdk_nvmf_tgt *tgt)
{
	return 0;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return NULL;
}

struct spdk_nvmf_poll_group *
spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt)
{
	return NULL;
}

void
spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group)
{
}

void
spdk_nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair)
{
}

int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	return 0;
}

int
spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	return 0;
}

int
spdk_nvmf_poll_group_poll(struct spdk_nvmf_poll_group *group)
{
	return 0;
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns)
{
	abort();
	return NULL;
}

bool
spdk_nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	abort();
	return false;
}

int
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_bdev *bdev, struct spdk_nvme_ns_data *nsdata)
{
	abort();
	return -1;
}

void
spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, void *buffer,
				 uint64_t offset, uint32_t length)
{
	abort();
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	return -1;
}

static void
test_get_log_page(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	char data[4096];

	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	ctrlr.subsys = &subsystem;

	qpair.ctrlr = &ctrlr;

	req.qpair = &qpair;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.data = &data;
	req.length = sizeof(data);

	/* Get Log Page - all valid */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Get Log Page with invalid log ID */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = 0;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page with invalid offset (not dword aligned) */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	cmd.nvme_cmd.cdw12 = 2;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page without data buffer */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	req.data = NULL;
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);
	req.data = data;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "get_log_page", test_get_log_page) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
