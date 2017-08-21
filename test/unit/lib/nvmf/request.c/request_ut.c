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

#include "request.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

void spdk_trace_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		       uint64_t object_id, uint64_t arg1)
{
}

int
spdk_nvmf_transport_req_complete(struct spdk_nvmf_request *req)
{
	return 0;
}

void
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_qpair *qpair,
			struct spdk_nvmf_fabric_connect_cmd *cmd,
			struct spdk_nvmf_fabric_connect_data *data,
			struct spdk_nvmf_fabric_connect_rsp *rsp)
{
}

int
spdk_nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	return -1;
}

int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return -1;
}

int
spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return -1;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_vs_register vs;

	vs.raw = 0;
	return vs;
}

bool
spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns)
{
	return false;
}

struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id)
{
	return NULL;
}

void
spdk_nvmf_ctrlr_disconnect(struct spdk_nvmf_qpair *qpair)
{
}

void
spdk_nvmf_property_get(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvmf_fabric_prop_get_cmd *cmd,
		       struct spdk_nvmf_fabric_prop_get_rsp *response)
{
}

void
spdk_nvmf_property_set(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		       struct spdk_nvme_cpl *rsp)
{
}

void
spdk_nvmf_get_discovery_log_page(struct spdk_nvmf_tgt *tgt, void *buffer,
				 uint64_t offset, uint32_t length)
{
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	return NULL;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	return false;
}


static void
test_nvmf_process_fabrics_cmd(void)
{
	struct	spdk_nvmf_request req = {};
	int	ret;
	struct	spdk_nvmf_qpair req_qpair = {};
	union	nvmf_h2c_msg  req_cmd = {};
	union	nvmf_c2h_msg   req_rsp = {};

	req.qpair = &req_qpair;
	req.cmd  = &req_cmd;
	req.rsp  = &req_rsp;
	req.qpair->ctrlr = NULL;

	/* No ctrlr and invalid command check */
	req.cmd->nvmf_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	ret = nvmf_process_fabrics_command(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
		CU_add_test(suite, "nvmf_process_fabrics_command", test_nvmf_process_fabrics_cmd) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
