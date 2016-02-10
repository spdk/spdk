
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

#include "nvme/nvme_ctrlr_cmd.c"

#define CTRLR_CDATA_ELPE   5

char outbuf[OUTBUF_SIZE];

struct nvme_request g_req;

uint32_t error_num_entries;
uint32_t health_log_nsid = 1;
uint8_t feature = 1;
uint32_t feature_cdw11 = 1;
uint32_t feature_cdw12 = 1;
uint8_t get_feature = 1;
uint32_t get_feature_cdw11 = 1;
uint16_t abort_cid = 1;
uint16_t abort_sqid = 1;


typedef void (*verify_request_fn_t)(struct nvme_request *req);
verify_request_fn_t verify_fn;

static void verify_firmware_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == SPDK_NVME_GLOBAL_NS_TAG);

	temp_cdw10 = ((sizeof(struct spdk_nvme_firmware_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_LOG_FIRMWARE_SLOT;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_health_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == health_log_nsid);

	temp_cdw10 = ((sizeof(struct spdk_nvme_health_information_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_LOG_HEALTH_INFORMATION;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_error_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == SPDK_NVME_GLOBAL_NS_TAG);

	temp_cdw10 = (((sizeof(struct spdk_nvme_error_information_entry) * error_num_entries) /
		       sizeof(uint32_t) - 1) << 16) | SPDK_NVME_LOG_ERROR;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_set_feature_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_SET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == feature);
	CU_ASSERT(req->cmd.cdw11 == feature_cdw11);
	CU_ASSERT(req->cmd.cdw12 == feature_cdw12);
}

static void verify_get_feature_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == get_feature);
	CU_ASSERT(req->cmd.cdw11 == get_feature_cdw11);
}

static void verify_abort_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ABORT);
	CU_ASSERT(req->cmd.cdw10 == (((uint32_t)abort_cid << 16) | abort_sqid));
}

static void verify_io_raw_cmd(struct nvme_request *req)
{
	struct spdk_nvme_cmd	command = {};

	CU_ASSERT(memcmp(&req->cmd, &command, sizeof(req->cmd)) == 0);
}

static void verify_intel_smart_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == health_log_nsid);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_smart_information_page) /
		       sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_SMART;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_intel_temperature_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_temperature_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_TEMPERATURE;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_intel_read_latency_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_rw_latency_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_intel_write_latency_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_rw_latency_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void verify_intel_get_log_page_directory(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_log_page_directory) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

struct nvme_request *
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = &g_req;

	memset(req, 0, sizeof(*req));

	req->payload = *payload;
	req->payload_size = payload_size;

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->timeout = true;

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	return nvme_allocate_request(&payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(NULL, 0, cb_fn, cb_arg);
}

void
nvme_ctrlr_submit_io_request(struct spdk_nvme_ctrlr *ctrlr,
			     struct nvme_request *req)
{
	verify_fn(req);
	/* stop analyzer from thinking stack variable addresses are stored in a global */
	memset(req, 0, sizeof(*req));
}

void
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr, struct nvme_request *req)
{
	verify_fn(req);
	/* stop analyzer from thinking stack variable addresses are stored in a global */
	memset(req, 0, sizeof(*req));
}

static void
test_firmware_get_log_page(void)
{
	struct spdk_nvme_ctrlr			ctrlr = {};
	struct spdk_nvme_firmware_page		payload = {};

	verify_fn = verify_firmware_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_FIRMWARE_SLOT, SPDK_NVME_GLOBAL_NS_TAG,
					 &payload,
					 sizeof(payload), NULL, NULL);
}

static void
test_health_get_log_page(void)
{
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_health_information_page	payload = {};

	verify_fn = verify_health_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION, health_log_nsid,
					 &payload,
					 sizeof(payload), NULL, NULL);
}

static void
test_error_get_log_page(void)
{
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_error_information_entry	payload = {};

	ctrlr.cdata.elpe = CTRLR_CDATA_ELPE;

	verify_fn = verify_error_log_page;

	/* valid page */
	error_num_entries = 1;
	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_ERROR, SPDK_NVME_GLOBAL_NS_TAG, &payload,
					 sizeof(payload), NULL, NULL);
}

static void test_intel_smart_get_log_page(void)
{
	struct spdk_nvme_ctrlr			ctrlr = {};
	struct spdk_nvme_intel_smart_information_page	payload = {};

	verify_fn = verify_intel_smart_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_SMART, health_log_nsid, &payload,
					 sizeof(payload), NULL, NULL);
}

static void test_intel_temperature_get_log_page(void)
{
	struct spdk_nvme_ctrlr			ctrlr = {};
	struct spdk_nvme_intel_temperature_page	payload = {};

	verify_fn = verify_intel_temperature_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE, SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), NULL, NULL);
}

static void test_intel_read_latency_get_log_page(void)
{
	struct spdk_nvme_ctrlr			ctrlr = {};
	struct spdk_nvme_intel_rw_latency_page	payload = {};

	verify_fn = verify_intel_read_latency_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), NULL, NULL);
}

static void test_intel_write_latency_get_log_page(void)
{
	struct spdk_nvme_ctrlr			ctrlr = {};
	struct spdk_nvme_intel_rw_latency_page	payload = {};

	verify_fn = verify_intel_write_latency_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), NULL, NULL);
}

static void test_intel_get_log_page_directory(void)
{
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_intel_log_page_directory	payload = {};

	verify_fn = verify_intel_get_log_page_directory;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), NULL, NULL);
}

static void test_generic_get_log_pages(void)
{
	test_error_get_log_page();
	test_health_get_log_page();
	test_firmware_get_log_page();
}

static void test_intel_get_log_pages(void)
{
	test_intel_get_log_page_directory();
	test_intel_smart_get_log_page();
	test_intel_temperature_get_log_page();
	test_intel_read_latency_get_log_page();
	test_intel_write_latency_get_log_page();
}

static void
test_set_feature_cmd(void)
{
	struct spdk_nvme_ctrlr  ctrlr = {};

	verify_fn = verify_set_feature_cmd;

	spdk_nvme_ctrlr_cmd_set_feature(&ctrlr, feature, feature_cdw11, feature_cdw12, NULL, 0, NULL, NULL);
}


static void
test_get_feature_cmd(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	verify_fn = verify_get_feature_cmd;

	spdk_nvme_ctrlr_cmd_get_feature(&ctrlr, get_feature, get_feature_cdw11, NULL, 0, NULL, NULL);
}

static void
test_abort_cmd(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	verify_fn = verify_abort_cmd;

	nvme_ctrlr_cmd_abort(&ctrlr, abort_cid, abort_sqid, NULL, NULL);
}

static void
test_io_raw_cmd(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct spdk_nvme_cmd	cmd = {};

	verify_fn = verify_io_raw_cmd;

	spdk_nvme_ctrlr_cmd_io_raw(&ctrlr, &cmd, NULL, 1, NULL, NULL);
}

static void
test_get_log_pages(void)
{
	test_generic_get_log_pages();
	test_intel_get_log_pages();
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
		CU_add_test(suite, "test ctrlr cmd get_log_pages", test_get_log_pages) == NULL
		|| CU_add_test(suite, "test ctrlr cmd set_feature", test_set_feature_cmd) == NULL
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
