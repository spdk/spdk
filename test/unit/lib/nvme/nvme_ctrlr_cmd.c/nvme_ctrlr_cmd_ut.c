/*   SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk_cunit.h"

#include "nvme/nvme_ctrlr_cmd.c"

#include "spdk_internal/mock.h"

#define CTRLR_CDATA_ELPE   5

pid_t g_spdk_nvme_pid;

struct nvme_request g_req;

uint32_t error_num_entries;
uint32_t health_log_nsid = 1;
uint8_t feature = 1;
uint32_t feature_cdw11 = 1;
uint32_t feature_cdw12 = 1;
uint8_t get_feature = 1;
uint32_t get_feature_cdw11 = 1;
uint32_t fw_img_size = 1024;
uint32_t fw_img_offset = 0;
uint16_t abort_cid = 1;
uint16_t abort_sqid = 1;
uint32_t namespace_management_nsid = 1;
uint64_t PRP_ENTRY_1 = 4096;
uint64_t PRP_ENTRY_2 = 4096;
uint32_t format_nvme_nsid = 1;
uint32_t sanitize_nvme_nsid = 1;
uint32_t expected_host_id_size = 0xFF;
uint32_t directive_nsid = 1;

uint32_t expected_feature_ns = 2;
uint32_t expected_feature_cdw10 = SPDK_NVME_FEAT_LBA_RANGE_TYPE;
uint32_t expected_feature_cdw11 = 1;
uint32_t expected_feature_cdw12 = 1;

typedef void (*verify_request_fn_t)(struct nvme_request *req);
verify_request_fn_t verify_fn;

DEFINE_STUB(nvme_transport_qpair_iterate_requests, int,
	    (struct spdk_nvme_qpair *qpair,
	     int (*iter_fn)(struct nvme_request *req, void *arg),
	     void *arg), 0);

DEFINE_STUB(nvme_qpair_abort_queued_reqs_with_cbarg, uint32_t,
	    (struct spdk_nvme_qpair *qpair, void *cmd_cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_admin_qp_failure_reason, spdk_nvme_qp_failure_reason,
	    (struct spdk_nvme_ctrlr *ctrlr), 0);

static int
nvme_ns_cmp(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	return ns1->id - ns2->id;
}

RB_GENERATE_STATIC(nvme_ns_tree, spdk_nvme_ns, node, nvme_ns_cmp);

static void
verify_firmware_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == SPDK_NVME_GLOBAL_NS_TAG);

	temp_cdw10 = ((sizeof(struct spdk_nvme_firmware_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_LOG_FIRMWARE_SLOT;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_health_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == health_log_nsid);

	temp_cdw10 = ((sizeof(struct spdk_nvme_health_information_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_LOG_HEALTH_INFORMATION;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_error_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == SPDK_NVME_GLOBAL_NS_TAG);

	temp_cdw10 = (((sizeof(struct spdk_nvme_error_information_entry) * error_num_entries) /
		       sizeof(uint32_t) - 1) << 16) | SPDK_NVME_LOG_ERROR;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_set_feature_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_SET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == feature);
	CU_ASSERT(req->cmd.cdw11 == feature_cdw11);
	CU_ASSERT(req->cmd.cdw12 == feature_cdw12);
}

static void
verify_set_feature_ns_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_SET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == expected_feature_cdw10);
	CU_ASSERT(req->cmd.cdw11 == expected_feature_cdw11);
	CU_ASSERT(req->cmd.cdw12 == expected_feature_cdw12);
	CU_ASSERT(req->cmd.nsid == expected_feature_ns);
}

static void
verify_get_feature_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == get_feature);
	CU_ASSERT(req->cmd.cdw11 == get_feature_cdw11);
}

static void
verify_get_feature_ns_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_FEATURES);
	CU_ASSERT(req->cmd.cdw10 == expected_feature_cdw10);
	CU_ASSERT(req->cmd.cdw11 == expected_feature_cdw11);
	CU_ASSERT(req->cmd.nsid == expected_feature_ns);
}

static void
verify_abort_cmd(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ABORT);
	CU_ASSERT(req->cmd.cdw10 == (((uint32_t)abort_cid << 16) | abort_sqid));
}

static void
verify_io_cmd_raw_no_payload_build(struct nvme_request *req)
{
	struct spdk_nvme_cmd    command = {};
	struct nvme_payload     payload = {};

	CU_ASSERT(memcmp(&req->cmd, &command, sizeof(req->cmd)) == 0);
	CU_ASSERT(memcmp(&req->payload, &payload, sizeof(req->payload)) == 0);
}

static void
verify_io_raw_cmd(struct nvme_request *req)
{
	struct spdk_nvme_cmd	command = {};

	CU_ASSERT(memcmp(&req->cmd, &command, sizeof(req->cmd)) == 0);
}

static void
verify_io_raw_cmd_with_md(struct nvme_request *req)
{
	struct spdk_nvme_cmd	command = {};

	CU_ASSERT(memcmp(&req->cmd, &command, sizeof(req->cmd)) == 0);
}

static void
verify_set_host_id_cmd(struct nvme_request *req)
{
	switch (expected_host_id_size) {
	case 8:
		CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_FEAT_HOST_IDENTIFIER);
		CU_ASSERT(req->cmd.cdw11 == 0);
		CU_ASSERT(req->cmd.cdw12 == 0);
		break;
	case 16:
		CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_FEAT_HOST_IDENTIFIER);
		CU_ASSERT(req->cmd.cdw11 == 1);
		CU_ASSERT(req->cmd.cdw12 == 0);
		break;
	default:
		CU_ASSERT(0);
	}
}

static void
verify_intel_smart_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);
	CU_ASSERT(req->cmd.nsid == health_log_nsid);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_smart_information_page) /
		       sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_SMART;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_intel_temperature_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_temperature_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_TEMPERATURE;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_intel_read_latency_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_rw_latency_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_intel_write_latency_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_rw_latency_page) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_intel_get_log_page_directory(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_log_page_directory) / sizeof(uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_intel_marketing_description_log_page(struct nvme_request *req)
{
	uint32_t temp_cdw10;

	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_GET_LOG_PAGE);

	temp_cdw10 = ((sizeof(struct spdk_nvme_intel_marketing_description_page) / sizeof(
			       uint32_t) - 1) << 16) |
		     SPDK_NVME_INTEL_MARKETING_DESCRIPTION;
	CU_ASSERT(req->cmd.cdw10 == temp_cdw10);
}

static void
verify_namespace_attach(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_NS_ATTACHMENT);
	CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_NS_CTRLR_ATTACH);
	CU_ASSERT(req->cmd.nsid == namespace_management_nsid);
}

static void
verify_namespace_detach(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_NS_ATTACHMENT);
	CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_NS_CTRLR_DETACH);
	CU_ASSERT(req->cmd.nsid == namespace_management_nsid);
}

static void
verify_namespace_create(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_NS_MANAGEMENT);
	CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_NS_MANAGEMENT_CREATE);
	CU_ASSERT(req->cmd.nsid == 0);
}

static void
verify_namespace_delete(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_NS_MANAGEMENT);
	CU_ASSERT(req->cmd.cdw10 == SPDK_NVME_NS_MANAGEMENT_DELETE);
	CU_ASSERT(req->cmd.nsid == namespace_management_nsid);
}

static void
verify_doorbell_buffer_config(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG);
	CU_ASSERT(req->cmd.dptr.prp.prp1 == PRP_ENTRY_1);
	CU_ASSERT(req->cmd.dptr.prp.prp2 == PRP_ENTRY_2);
}

static void
verify_format_nvme(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_FORMAT_NVM);
	CU_ASSERT(req->cmd.cdw10 == 0);
	CU_ASSERT(req->cmd.nsid == format_nvme_nsid);
}

static void
verify_fw_commit(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_FIRMWARE_COMMIT);
	CU_ASSERT(req->cmd.cdw10 == 0x09);
}

static void
verify_fw_image_download(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD);
	CU_ASSERT(req->cmd.cdw10 == spdk_nvme_bytes_to_numd(fw_img_size));
	CU_ASSERT(req->cmd.cdw11 == fw_img_offset >> 2);
}

static void
verify_nvme_sanitize(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_SANITIZE);
	CU_ASSERT(req->cmd.cdw10 == 0x309);
	CU_ASSERT(req->cmd.cdw11 == 0);
	CU_ASSERT(req->cmd.nsid == sanitize_nvme_nsid);
}

static void
verify_directive_receive(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_DIRECTIVE_RECEIVE);
	CU_ASSERT(req->cmd.nsid == directive_nsid);
}

static void
verify_directive_send(struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_DIRECTIVE_SEND);
	CU_ASSERT(req->cmd.nsid == directive_nsid);
}

struct nvme_request *
nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair, void *buffer, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg, bool host_to_controller)
{
	/* For the unit test, we don't actually need to copy the buffer */
	return nvme_allocate_request_contig(qpair, buffer, payload_size, cb_fn, cb_arg);
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	verify_fn(req);
	/* stop analyzer from thinking stack variable addresses are stored in a global */
	memset(req, 0, sizeof(*req));

	return 0;
}

DEFINE_RETURN_MOCK(nvme_ctrlr_submit_admin_request, int);
int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr, struct nvme_request *req)
{
	HANDLE_RETURN_MOCK(nvme_ctrlr_submit_admin_request);
	verify_fn(req);
	/* stop analyzer from thinking stack variable addresses are stored in a global */
	memset(req, 0, sizeof(*req));

	return 0;
}

static struct spdk_nvme_ns g_inactive_ns = {};

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp;
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return NULL;
	}

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns == NULL) {
		return &g_inactive_ns;
	}

	return ns;
}

#define DECLARE_AND_CONSTRUCT_CTRLR()		\
	struct spdk_nvme_ctrlr	ctrlr = {};	\
	struct spdk_nvme_qpair	adminq = {};	\
	struct nvme_request	req;		\
						\
	STAILQ_INIT(&adminq.free_req);		\
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);	\
	ctrlr.adminq = &adminq;	\
	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

#define DECONSTRUCT_CTRLR() \
	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);

static void
test_firmware_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_firmware_page		payload = {};

	verify_fn = verify_firmware_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_FIRMWARE_SLOT, SPDK_NVME_GLOBAL_NS_TAG,
					 &payload,
					 sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_health_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_health_information_page	payload = {};

	verify_fn = verify_health_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION, health_log_nsid,
					 &payload,
					 sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_error_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_error_information_entry	payload = {};

	ctrlr.cdata.elpe = CTRLR_CDATA_ELPE;

	verify_fn = verify_error_log_page;

	/* valid page */
	error_num_entries = 1;
	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_LOG_ERROR, SPDK_NVME_GLOBAL_NS_TAG, &payload,
					 sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_smart_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_smart_information_page	payload = {};

	verify_fn = verify_intel_smart_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_SMART, health_log_nsid, &payload,
					 sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_temperature_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_temperature_page	payload = {};

	verify_fn = verify_intel_temperature_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE, SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_read_latency_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_rw_latency_page	payload = {};

	verify_fn = verify_intel_read_latency_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_write_latency_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_rw_latency_page	payload = {};

	verify_fn = verify_intel_write_latency_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_get_log_page_directory(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_log_page_directory	payload = {};

	verify_fn = verify_intel_get_log_page_directory;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_intel_marketing_description_get_log_page(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_intel_marketing_description_page	payload = {};

	verify_fn = verify_intel_marketing_description_log_page;

	spdk_nvme_ctrlr_cmd_get_log_page(&ctrlr, SPDK_NVME_INTEL_MARKETING_DESCRIPTION,
					 SPDK_NVME_GLOBAL_NS_TAG,
					 &payload, sizeof(payload), 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_generic_get_log_pages(void)
{
	test_error_get_log_page();
	test_health_get_log_page();
	test_firmware_get_log_page();
}

static void
test_intel_get_log_pages(void)
{
	test_intel_get_log_page_directory();
	test_intel_smart_get_log_page();
	test_intel_temperature_get_log_page();
	test_intel_read_latency_get_log_page();
	test_intel_write_latency_get_log_page();
	test_intel_marketing_description_get_log_page();
}

static void
test_set_feature_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_set_feature_cmd;

	spdk_nvme_ctrlr_cmd_set_feature(&ctrlr, feature, feature_cdw11, feature_cdw12, NULL, 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_get_feature_ns_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_get_feature_ns_cmd;

	spdk_nvme_ctrlr_cmd_get_feature_ns(&ctrlr, expected_feature_cdw10,
					   expected_feature_cdw11, NULL, 0,
					   NULL, NULL, expected_feature_ns);

	DECONSTRUCT_CTRLR();
}

static void
test_set_feature_ns_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_set_feature_ns_cmd;

	spdk_nvme_ctrlr_cmd_set_feature_ns(&ctrlr, expected_feature_cdw10,
					   expected_feature_cdw11, expected_feature_cdw12,
					   NULL, 0, NULL, NULL, expected_feature_ns);

	DECONSTRUCT_CTRLR();
}

static void
test_get_feature_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_get_feature_cmd;

	spdk_nvme_ctrlr_cmd_get_feature(&ctrlr, get_feature, get_feature_cdw11, NULL, 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_abort_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_qpair	qpair = {};

	STAILQ_INIT(&ctrlr.queued_aborts);

	verify_fn = verify_abort_cmd;

	qpair.id = abort_sqid;
	spdk_nvme_ctrlr_cmd_abort(&ctrlr, &qpair, abort_cid, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_io_cmd_raw_no_payload_build(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_qpair  qpair = {};
	struct spdk_nvme_cmd    cmd = {};

	verify_fn = verify_io_cmd_raw_no_payload_build;

	spdk_nvme_ctrlr_io_cmd_raw_no_payload_build(&ctrlr, &qpair, &cmd, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_io_raw_cmd(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_qpair	qpair = {};
	struct spdk_nvme_cmd	cmd = {};

	verify_fn = verify_io_raw_cmd;

	spdk_nvme_ctrlr_cmd_io_raw(&ctrlr, &qpair, &cmd, NULL, 1, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_io_raw_cmd_with_md(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_qpair	qpair = {};
	struct spdk_nvme_cmd	cmd = {};

	verify_fn = verify_io_raw_cmd_with_md;

	spdk_nvme_ctrlr_cmd_io_raw_with_md(&ctrlr, &qpair, &cmd, NULL, 1, NULL, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static int
test_set_host_id_by_case(uint32_t host_id_size)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	int rc = 0;

	expected_host_id_size = host_id_size;
	verify_fn = verify_set_host_id_cmd;

	rc = nvme_ctrlr_cmd_set_host_id(&ctrlr, NULL, expected_host_id_size, NULL, NULL);

	DECONSTRUCT_CTRLR();
	return rc;
}

static void
test_set_host_id_cmds(void)
{
	int rc = 0;

	rc = test_set_host_id_by_case(8);
	CU_ASSERT(rc == 0);
	rc = test_set_host_id_by_case(16);
	CU_ASSERT(rc == 0);
	rc = test_set_host_id_by_case(1024);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_get_log_pages(void)
{
	test_generic_get_log_pages();
	test_intel_get_log_pages();
}

static void
test_namespace_attach(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_ctrlr_list		payload = {};

	verify_fn = verify_namespace_attach;

	nvme_ctrlr_cmd_attach_ns(&ctrlr, namespace_management_nsid, &payload, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_namespace_detach(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_ctrlr_list		payload = {};

	verify_fn = verify_namespace_detach;

	nvme_ctrlr_cmd_detach_ns(&ctrlr, namespace_management_nsid, &payload, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_namespace_create(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_ns_data		payload = {};

	verify_fn = verify_namespace_create;
	nvme_ctrlr_cmd_create_ns(&ctrlr, &payload, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_namespace_delete(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_namespace_delete;
	nvme_ctrlr_cmd_delete_ns(&ctrlr, namespace_management_nsid, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_doorbell_buffer_config(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_doorbell_buffer_config;

	nvme_ctrlr_cmd_doorbell_buffer_config(&ctrlr, PRP_ENTRY_1, PRP_ENTRY_2, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_format_nvme(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_format format = {};

	verify_fn = verify_format_nvme;

	nvme_ctrlr_cmd_format(&ctrlr, format_nvme_nsid, &format, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_fw_commit(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_fw_commit fw_commit = {};

	fw_commit.ca = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;
	fw_commit.fs = 1;

	verify_fn = verify_fw_commit;

	nvme_ctrlr_cmd_fw_commit(&ctrlr, &fw_commit, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_fw_image_download(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	verify_fn = verify_fw_image_download;

	nvme_ctrlr_cmd_fw_image_download(&ctrlr, fw_img_size, fw_img_offset, NULL,
					 NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_sanitize(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_sanitize sanitize = {};

	sanitize.sanact = 1;
	sanitize.ause   = 1;
	sanitize.oipbp  = 1;
	sanitize.ndas   = 1;

	verify_fn = verify_nvme_sanitize;

	nvme_ctrlr_cmd_sanitize(&ctrlr, sanitize_nvme_nsid, &sanitize, 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_directive_receive(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	verify_fn = verify_directive_receive;

	spdk_nvme_ctrlr_cmd_directive_receive(&ctrlr, directive_nsid, 0, 0, 0, NULL, 0,
					      0, 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_directive_send(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	verify_fn = verify_directive_send;

	spdk_nvme_ctrlr_cmd_directive_send(&ctrlr, directive_nsid, 0, 0, 0, NULL, 0,
					   0, 0, NULL, NULL);

	DECONSTRUCT_CTRLR();
}

static void
test_directive(void)
{
	test_directive_receive();
	test_directive_send();
}

static void
test_nvme_request_add_abort(void)
{
	int rc;
	struct nvme_request req = {};
	struct nvme_request parent = {};
	struct nvme_request child = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_qpair admin_qpair = {};
	struct spdk_nvme_ctrlr ctrlr = {};

	parent.qpair = &qpair;
	qpair.ctrlr = &ctrlr;
	ctrlr.adminq = &admin_qpair;

	parent.user_cb_arg = req.cb_arg;
	parent.cmd.cdw10_bits.abort.sqid = 1;
	req.cmd.cid = 2;

	/* For allocating request */
	TAILQ_INIT(&parent.children);
	STAILQ_INIT(&admin_qpair.free_req);
	STAILQ_INSERT_HEAD(&admin_qpair.free_req, &child, stailq);

	rc = nvme_request_add_abort(&req, &parent);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(&child == TAILQ_FIRST(&parent.children));
	CU_ASSERT(child.cb_arg == &child);
	CU_ASSERT(child.cmd.opc == SPDK_NVME_OPC_ABORT);
	CU_ASSERT(child.parent == &parent);
	CU_ASSERT(child.cmd.cdw10_bits.abort.sqid == 1);
	CU_ASSERT(child.cmd.cdw10_bits.abort.cid == 2);
	CU_ASSERT(parent.num_children == 1);

	nvme_request_remove_child(&parent, &child);
	CU_ASSERT(STAILQ_EMPTY(&admin_qpair.free_req));
	CU_ASSERT(TAILQ_EMPTY(&parent.children));
	CU_ASSERT(parent.num_children == 0);
	CU_ASSERT(child.parent == NULL);

	/* Parent invalid */
	req.cb_arg = (void *)0xDEADBEEF;
	req.parent = NULL;
	parent.user_cb_arg = (void *)0xDDADBEEF;
	STAILQ_INSERT_HEAD(&admin_qpair.free_req, &child, stailq);

	rc = nvme_request_add_abort(&req, &parent);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!STAILQ_EMPTY(&admin_qpair.free_req));
	STAILQ_REMOVE_HEAD(&admin_qpair.free_req, stailq);
	CU_ASSERT(STAILQ_EMPTY(&admin_qpair.free_req));

	/* Child request empty */
	parent.user_cb_arg = req.cb_arg;

	rc = nvme_request_add_abort(&req, &parent);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_spdk_nvme_ctrlr_cmd_abort(void)
{
	int rc;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair admin_qpair = {};
	struct spdk_nvme_qpair *qpair = NULL;
	struct nvme_request req = {};

	/* For allocating request */
	STAILQ_INIT(&admin_qpair.free_req);
	STAILQ_INSERT_HEAD(&admin_qpair.free_req, &req, stailq);
	ctrlr.adminq = &admin_qpair;
	admin_qpair.id = 0;
	MOCK_SET(nvme_ctrlr_submit_admin_request, 0);
	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

	rc = spdk_nvme_ctrlr_cmd_abort(&ctrlr, qpair, 2, (void *)0xDEADBEEF, (void *)0xDCADBEEF);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cb_arg == &req);
	CU_ASSERT(req.user_cb_fn == (void *)0xDEADBEEF);
	CU_ASSERT(req.user_cb_arg == (void *)0xDCADBEEF);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_ABORT);
	CU_ASSERT(req.cmd.cdw10_bits.abort.sqid == 0);
	CU_ASSERT(req.cmd.cdw10_bits.abort.cid == 2);
	CU_ASSERT(STAILQ_EMPTY(&admin_qpair.free_req));

	/* Request empty */
	rc = spdk_nvme_ctrlr_cmd_abort(&ctrlr, qpair, 2, (void *)0xDEADBEEF, (void *)0xDCADBEEF);
	CU_ASSERT(rc == -ENOMEM);
	MOCK_CLEAR(nvme_ctrlr_submit_admin_request);
	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);
}

static void
test_nvme_ctrlr_cmd_identify(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct nvme_payload payload = {};
	int rc;
	MOCK_SET(nvme_ctrlr_submit_admin_request, 0);

	rc = nvme_ctrlr_cmd_identify(&ctrlr, SPDK_NVME_IDENTIFY_NS, 2, 1, 0, &payload,
				     SPDK_NVME_IDENTIFY_BUFLEN, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_IDENTIFY);
	CU_ASSERT(req.cmd.cdw10_bits.identify.cns == SPDK_NVME_IDENTIFY_NS);
	CU_ASSERT(req.cmd.cdw10_bits.identify.cntid == 2);
	CU_ASSERT(req.cmd.cdw11_bits.identify.csi == 0);
	CU_ASSERT(req.cmd.nsid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));
	DECONSTRUCT_CTRLR();
	MOCK_CLEAR(nvme_ctrlr_submit_admin_request);
}

static void
test_spdk_nvme_ctrlr_cmd_security_receive_send(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct nvme_payload payload = {};
	int rc;
	MOCK_SET(nvme_ctrlr_submit_admin_request, 0);

	rc = spdk_nvme_ctrlr_cmd_security_send(&ctrlr, 0xea, 0xaabb, 0xcc, &payload, 4096, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_SECURITY_SEND);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.nssf == 0xcc);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.spsp0 == 0xbb);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.spsp1 == 0xaa);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.secp == 0xea);
	CU_ASSERT(req.cmd.cdw11 == 4096);
	SPDK_CU_ASSERT_FATAL(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);
	rc = spdk_nvme_ctrlr_cmd_security_receive(&ctrlr, 0xea, 0xaabb, 0xcc, &payload, 4096, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_SECURITY_RECEIVE);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.nssf == 0xcc);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.spsp0 == 0xbb);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.spsp1 == 0xaa);
	CU_ASSERT(req.cmd.cdw10_bits.sec_send_recv.secp == 0xea);
	CU_ASSERT(req.cmd.cdw11 == 4096);
	SPDK_CU_ASSERT_FATAL(STAILQ_EMPTY(&ctrlr.adminq->free_req));
	MOCK_CLEAR(nvme_ctrlr_submit_admin_request);

	/* Without request valid. */
	rc = spdk_nvme_ctrlr_cmd_security_send(&ctrlr, 0xea, 0xaabb, 0xcc, &payload, 4096, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = spdk_nvme_ctrlr_cmd_security_receive(&ctrlr, 0xea, 0xaabb, 0xcc, &payload, 4096, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);
	DECONSTRUCT_CTRLR();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_ctrlr_cmd", NULL, NULL);

	CU_ADD_TEST(suite, test_get_log_pages);
	CU_ADD_TEST(suite, test_set_feature_cmd);
	CU_ADD_TEST(suite, test_set_feature_ns_cmd);
	CU_ADD_TEST(suite, test_get_feature_cmd);
	CU_ADD_TEST(suite, test_get_feature_ns_cmd);
	CU_ADD_TEST(suite, test_abort_cmd);
	CU_ADD_TEST(suite, test_set_host_id_cmds);
	CU_ADD_TEST(suite, test_io_cmd_raw_no_payload_build);
	CU_ADD_TEST(suite, test_io_raw_cmd);
	CU_ADD_TEST(suite, test_io_raw_cmd_with_md);
	CU_ADD_TEST(suite, test_namespace_attach);
	CU_ADD_TEST(suite, test_namespace_detach);
	CU_ADD_TEST(suite, test_namespace_create);
	CU_ADD_TEST(suite, test_namespace_delete);
	CU_ADD_TEST(suite, test_doorbell_buffer_config);
	CU_ADD_TEST(suite, test_format_nvme);
	CU_ADD_TEST(suite, test_fw_commit);
	CU_ADD_TEST(suite, test_fw_image_download);
	CU_ADD_TEST(suite, test_sanitize);
	CU_ADD_TEST(suite, test_directive);
	CU_ADD_TEST(suite, test_nvme_request_add_abort);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_cmd_abort);
	CU_ADD_TEST(suite, test_nvme_ctrlr_cmd_identify);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_cmd_security_receive_send);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
