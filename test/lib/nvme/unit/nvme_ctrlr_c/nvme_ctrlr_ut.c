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

#include "nvme/nvme_ctrlr.c"

struct nvme_driver g_nvme_driver = {
	.lock = NVME_MUTEX_INITIALIZER,
	.max_io_queues = DEFAULT_MAX_IO_QUEUES
};

static uint16_t g_pci_vendor_id;
static uint16_t g_pci_device_id;
static uint16_t g_pci_subvendor_id;
static uint16_t g_pci_subdevice_id;

char outbuf[OUTBUF_SIZE];

__thread int    nvme_thread_ioq_index = -1;

uint16_t
spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev)
{
	return g_pci_vendor_id;
}

uint16_t
spdk_pci_device_get_device_id(struct spdk_pci_device *dev)
{
	return g_pci_device_id;
}

uint16_t
spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev)
{
	return g_pci_subvendor_id;
}

uint16_t
spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev)
{
	return g_pci_subdevice_id;
}

int nvme_qpair_construct(struct nvme_qpair *qpair, uint16_t id,
			 uint16_t num_entries, uint16_t num_trackers,
			 struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	return 0;
}

void
nvme_qpair_fail(struct nvme_qpair *qpair)
{
}

void
nvme_qpair_submit_request(struct nvme_qpair *qpair, struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST);
}

int32_t
nvme_qpair_process_completions(struct nvme_qpair *qpair, uint32_t max_completions)
{
	return 0;
}

void
nvme_qpair_disable(struct nvme_qpair *qpair)
{
}

void
nvme_qpair_destroy(struct nvme_qpair *qpair)
{
}

void
nvme_qpair_enable(struct nvme_qpair *qpair)
{
}

void
nvme_qpair_reset(struct nvme_qpair *qpair)
{
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
}

void
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_critical_warning_state state, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
}

void
nvme_ctrlr_cmd_identify_controller(struct spdk_nvme_ctrlr *ctrlr, void *payload,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
}

void
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
}

void
nvme_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
			    struct nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg)
{
}

void
nvme_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
			    struct nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg)
{
}

void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

struct nvme_request *
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = NULL;
	nvme_alloc_request(&req);

	if (req != NULL) {
		memset(req, 0, offsetof(struct nvme_request, children));

		req->payload = *payload;
		req->payload_size = payload_size;

		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
		req->timeout = true;
	}

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

static void
test_nvme_ctrlr_fail(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.num_io_queues = 0;
	nvme_ctrlr_fail(&ctrlr);

	CU_ASSERT(ctrlr.is_failed == true);
}

static void
test_nvme_ctrlr_construct_intel_support_log_page_list(void)
{
	bool	res;
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_intel_log_page_directory	payload = {};

	/* set a invalid vendor id */
	ctrlr.cdata.vid = 0xFFFF;

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);

	/* set valid vendor id and log page directory*/
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 1;
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);

	/* set valid vendor id, device id and sub device id*/
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 0;
	g_pci_vendor_id = SPDK_PCI_VID_INTEL;
	g_pci_device_id = 0x0953;
	g_pci_subvendor_id = SPDK_PCI_VID_INTEL;
	g_pci_subdevice_id = 0x3702;
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);
}

static void
test_nvme_ctrlr_set_supported_features(void)
{
	bool	res;
	struct spdk_nvme_ctrlr			ctrlr = {};

	/* set a invalid vendor id */
	ctrlr.cdata.vid = 0xFFFF;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == false);

	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == true);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ctrlr", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test nvme_ctrlr function nvme_ctrlr_fail", test_nvme_ctrlr_fail) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_construct_intel_support_log_page_list",
			       test_nvme_ctrlr_construct_intel_support_log_page_list) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_set_supported_features",
			       test_nvme_ctrlr_set_supported_features) == NULL
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
