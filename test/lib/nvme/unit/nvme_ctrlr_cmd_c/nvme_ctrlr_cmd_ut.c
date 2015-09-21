
/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include <stdbool.h>
#include "nvme/nvme_internal.h"

#include "CUnit/Basic.h"

#include "nvme/nvme_ctrlr_cmd.c"

char outbuf[OUTBUF_SIZE];

struct nvme_command *cmd = NULL;

uint64_t nvme_vtophys(void *buf)
{
	return (uintptr_t)buf;
}

typedef void (*verify_request_fn_t)(struct nvme_request *req);
verify_request_fn_t verify_fn;

void verify_firmware_log_page(struct nvme_request *req)
{
	cmd = &req->cmd;
	CU_ASSERT(cmd->opc == NVME_OPC_GET_LOG_PAGE);
	nvme_free_request(req);
}

void verify_health_log_page(struct nvme_request *req)
{
	cmd = &req->cmd;
	CU_ASSERT(cmd->opc == NVME_OPC_GET_LOG_PAGE);
	nvme_free_request(req);
}

void verify_error_log_page(struct nvme_request *req)
{
	cmd = &req->cmd;
	CU_ASSERT(cmd->opc == NVME_OPC_GET_LOG_PAGE);
	nvme_free_request(req);
}

void verify_get_feature_cmd(struct nvme_request *req)
{
	cmd = &req->cmd;
	CU_ASSERT(cmd->opc == NVME_OPC_GET_FEATURES);
	nvme_free_request(req);
}

void verify_abort_cmd(struct nvme_request *req)
{
	cmd = &req->cmd;
	CU_ASSERT(cmd->opc == NVME_OPC_ABORT);
	nvme_free_request(req);
}

void verify_io_raw_cmd(struct nvme_request *req)
{
	struct nvme_command	command = {0};
	uint64_t		phys_addr = 0;
	int			rc = 100;


	cmd = &req->cmd;
	CU_ASSERT(cmd != NULL);
	rc = memcmp(cmd, &command, sizeof(cmd));
	CU_ASSERT(rc == 0);
	nvme_free_request(req);
}

struct nvme_request *
nvme_allocate_request(void *payload, uint32_t payload_size,
		      nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req = NULL;
	nvme_alloc_request(&req);

	if (req != NULL) {
		memset(req, 0, offsetof(struct nvme_request, children));

		if (payload == NULL || payload_size == 0) {
			req->u.payload = NULL;
			req->payload_size = 0;
		} else {
			req->u.payload = payload;
			req->payload_size = payload_size;
		}

		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
		req->timeout = true;
	}
	return req;
}

void
nvme_ctrlr_submit_io_request(struct nvme_controller *ctrlr,
			     struct nvme_request *req)
{
	verify_fn(req);
}

void
nvme_ctrlr_submit_admin_request(struct nvme_controller *ctrlr, struct nvme_request *req)
{
	verify_fn(req);
}


void
test_firmware_get_log_page()
{
	struct nvme_controller			ctrlr = {};
	struct nvme_firmware_page		*payload = NULL;
	nvme_cb_fn_t				cb_fn = NULL;
	void					*cb_arg = NULL;
	uint64_t				phys_addr = 0;

	payload = nvme_malloc("nvme_firmware_page", sizeof(struct nvme_firmware_page),
			      64, &phys_addr);
	CU_ASSERT(payload != NULL);

	verify_fn = verify_firmware_log_page;

	nvme_ctrlr_cmd_get_firmware_page(&ctrlr,
					 payload, cb_fn, cb_arg);

	nvme_free(payload);
}

void
test_health_get_log_page()
{
	struct nvme_controller			ctrlr = {};
	struct nvme_health_information_page	*payload = NULL;
	uint32_t				nsid = 0;
	nvme_cb_fn_t				cb_fn = NULL;
	void					*cb_arg = NULL;
	uint64_t				phys_addr = 0;

	payload = nvme_malloc("nvme_health_information_page", sizeof(struct nvme_health_information_page),
			      64, &phys_addr);
	CU_ASSERT(payload != NULL);

	verify_fn = verify_health_log_page;

	nvme_ctrlr_cmd_get_health_information_page(&ctrlr, nsid,
			payload, cb_fn, cb_arg);

	nvme_free(payload);
}

void
test_error_get_log_page()
{
	struct nvme_controller			*ctrlr = NULL;
	struct nvme_controller_data		*ctrldata = NULL;
	struct nvme_error_information_entry	*payload = NULL;
	uint32_t				num_entries = 1;
	nvme_cb_fn_t				cb_fn = NULL;
	void					*cb_arg = NULL;
	uint64_t				phys_addr = 0;

	payload = nvme_malloc("nvme_error_information_entry", sizeof(struct nvme_error_information_entry),
			      64, &phys_addr);
	CU_ASSERT(payload != NULL);

	ctrlr = nvme_malloc("nvme_controller", sizeof(struct nvme_controller),
			    64, &phys_addr);
	CU_ASSERT(ctrlr != NULL);

	ctrldata = nvme_malloc("nvme_controller_data", sizeof(struct nvme_controller_data),
			       64, &phys_addr);
	CU_ASSERT(ctrldata != NULL);

	ctrlr->cdata = *ctrldata;
	ctrlr->cdata.elpe = 5;

	verify_fn = verify_error_log_page;

	nvme_ctrlr_cmd_get_error_page(ctrlr, payload,
				      num_entries, cb_fn, cb_arg);
	num_entries = 50;
	nvme_ctrlr_cmd_get_error_page(ctrlr, payload,
				      num_entries, cb_fn, cb_arg);


	nvme_free(payload);
	nvme_free(ctrlr);
	nvme_free(ctrldata);
}

void
test_get_feature_cmd()
{
	struct nvme_controller	ctrlr = {};
	uint8_t			feature = 1;
	uint32_t		cdw11 = 1;
	void			*payload = NULL;
	uint32_t		payload_size = 0;
	nvme_cb_fn_t		cb_fn = NULL;
	void			*cb_arg = NULL;

	verify_fn = verify_get_feature_cmd;

	nvme_ctrlr_cmd_get_feature(&ctrlr, feature, cdw11, payload,
				   payload_size, cb_fn, cb_arg);
}

void
test_abort_cmd()
{
	struct nvme_controller ctrlr = {};
	uint16_t		cid = 0;
	uint16_t		sqid = 0;
	nvme_cb_fn_t		cb_fn = NULL;
	void			*cb_arg = NULL;

	verify_fn = verify_abort_cmd;

	nvme_ctrlr_cmd_abort(&ctrlr, cid, sqid, cb_fn, cb_arg);
}

void
test_io_raw_cmd()
{
	struct nvme_controller ctrlr = {};
	struct nvme_command	*cmd = NULL;
	void			*buf = NULL;
	uint32_t		len = 1;
	nvme_cb_fn_t		cb_fn = NULL;
	void			*cb_arg = NULL;
	uint64_t		phys_addr = 0;

	cmd = nvme_malloc("nvme_command", sizeof(struct nvme_command),
			  64, &phys_addr);
	CU_ASSERT(cmd != NULL);
	memset(cmd, 0, sizeof(cmd));

	verify_fn = verify_io_raw_cmd;

	nvme_ctrlr_cmd_io_raw(&ctrlr, cmd, buf, len, cb_fn, cb_arg);
	nvme_free(cmd);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ctrlr_cmd", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test ctrlr cmd get_firmware_page", test_firmware_get_log_page) == NULL
		|| CU_add_test(suite, "test ctrlr cmd get_health_page", test_health_get_log_page) == NULL
		|| CU_add_test(suite, "test ctrlr cmd get_error_page", test_error_get_log_page) == NULL
		|| CU_add_test(suite, "test ctrlr cmd get_feature", test_get_feature_cmd) == NULL
		|| CU_add_test(suite, "test ctrlr cmd abort_cmd", test_abort_cmd) == NULL
		|| CU_add_test(suite, "test ctrlr cmd io_raw_cmd", test_io_raw_cmd) == NULL
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
