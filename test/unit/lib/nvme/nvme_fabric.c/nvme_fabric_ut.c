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
#include "nvme/nvme_fabric.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
struct spdk_nvmf_fabric_prop_set_cmd g_ut_cmd = {};
struct spdk_nvmf_fabric_prop_get_rsp g_ut_response = {};

DEFINE_STUB_V(spdk_nvme_ctrlr_get_default_ctrlr_opts,
	      (struct spdk_nvme_ctrlr_opts *opts, size_t opts_size));

DEFINE_STUB(nvme_transport_ctrlr_set_reg_4, int,
	    (struct spdk_nvme_ctrlr *ctrlr,
	     uint32_t offset, uint32_t value), 0);

DEFINE_STUB_V(nvme_ctrlr_destruct, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_ctrlr_cmd_identify, int,
	    (struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid,
	     uint32_t nsid, uint8_t csi, void *payload, size_t payload_size,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB_V(nvme_ctrlr_connected, (struct spdk_nvme_probe_ctx *probe_ctx,
				     struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB(nvme_ctrlr_add_process, int,
	    (struct spdk_nvme_ctrlr *ctrlr, void *devhandle), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_get_log_page, int,
	    (struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
	     uint32_t nsid, void *payload, uint32_t payload_size,
	     uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_transport_available_by_name, bool,
	    (const char *transport_name), true);

DEFINE_STUB(nvme_transport_ctrlr_construct, struct spdk_nvme_ctrlr *,
	    (const struct spdk_nvme_transport_id *trid,
	     const struct spdk_nvme_ctrlr_opts *opts,
	     void *devhandle), NULL);

DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *,
	    (enum spdk_nvmf_adrfam adrfam), NULL);

DEFINE_STUB(nvme_ctrlr_process_init, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

static struct spdk_nvmf_fabric_connect_data g_nvmf_data;
static struct nvme_request *g_request;

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	CU_ASSERT(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	g_request = req;
	memcpy(&g_nvmf_data, req->payload.contig_or_cb_arg, sizeof(g_nvmf_data));

	return 0;
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_completion_poll_status *status = arg;

	if (status->timed_out) {
		spdk_free(status->dma_data);
		free(status);
	}

	g_request = NULL;
}

static bool g_nvme_wait_for_completion_timeout;

int
nvme_wait_for_completion_robust_lock_timeout_poll(struct spdk_nvme_qpair *qpair,
		struct nvme_completion_poll_status *status,
		pthread_mutex_t *robust_mutex)
{
	struct spdk_nvmf_fabric_connect_rsp *rsp = (void *)&status->cpl;

	if (nvme_qpair_is_admin_queue(qpair)) {
		rsp->status_code_specific.success.cntlid = 1;
	}

	status->timed_out = g_nvme_wait_for_completion_timeout;

	return 0;
}

int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int len, i, rc;

	if (trstring == NULL) {
		return -EINVAL;
	}

	len = strnlen(trstring, SPDK_NVMF_TRSTRING_MAX_LEN);
	if (len == SPDK_NVMF_TRSTRING_MAX_LEN) {
		return -EINVAL;
	}

	rc = snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", trstring);
	if (rc < 0) {
		return rc;
	}

	/* cast official trstring to uppercase version of input. */
	for (i = 0; i < len; i++) {
		trid->trstring[i] = toupper(trid->trstring[i]);
	}
	return 0;
}

static struct spdk_nvme_transport_id g_ut_trid;
static bool g_ut_ctrlr_is_probed;

int
nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_probe_ctx *probe_ctx, void *devhandle)
{
	g_ut_trid = *trid;
	g_ut_ctrlr_is_probed = true;

	return 0;
}

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	case SPDK_NVME_TRANSPORT_TCP:
		return "TCP";
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		return "VFIOUSER";
	case SPDK_NVME_TRANSPORT_CUSTOM:
		return "CUSTOM";
	default:
		return NULL;
	}
}

DEFINE_RETURN_MOCK(nvme_wait_for_completion, int);
int
nvme_wait_for_completion(struct spdk_nvme_qpair *qpair,
			 struct nvme_completion_poll_status *status)
{
	status->timed_out = false;
	HANDLE_RETURN_MOCK(nvme_wait_for_completion);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_nvme_ctrlr_cmd_admin_raw, int);
int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fabric_prop_set_cmd *cmd_tmp = (void *)cmd;
	struct nvme_completion_poll_status *status = cb_arg;
	struct spdk_nvmf_fabric_prop_get_rsp *response = (void *)&status->cpl;

	g_ut_cmd.opcode = cmd_tmp->opcode;
	g_ut_cmd.fctype = cmd_tmp->fctype;
	g_ut_cmd.ofst = cmd_tmp->ofst;
	g_ut_cmd.attrib.size = cmd_tmp->attrib.size;

	if (cmd_tmp->fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET) {
		g_ut_cmd.value.u64 = cmd_tmp->value.u64;
	} else if (cmd_tmp->fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET) {
		memcpy(&g_ut_response, response, sizeof(g_ut_response));
	}

	HANDLE_RETURN_MOCK(spdk_nvme_ctrlr_cmd_admin_raw);
	return 0;
}

static void
abort_request(struct nvme_request *request)
{
	struct spdk_nvme_cpl cpl = {
		.status = {
			.sct = SPDK_NVME_SCT_GENERIC,
			.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION,
		}
	};

	request->cb_fn(request->cb_arg, &cpl);
}

static void
test_nvme_fabric_prop_set_cmd(void)
{
	int rc;
	struct spdk_nvme_ctrlr ctrlr = {};

	memset(&g_ut_cmd, 0, sizeof(g_ut_cmd));

	rc = nvme_fabric_prop_set_cmd_sync(&ctrlr, 1024, SPDK_NVMF_PROP_SIZE_8, 4096);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ut_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	CU_ASSERT(g_ut_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET);
	CU_ASSERT(g_ut_cmd.ofst == 1024);
	CU_ASSERT(g_ut_cmd.attrib.size == SPDK_NVMF_PROP_SIZE_8);
	CU_ASSERT(g_ut_cmd.value.u64 == 4096);
}

static void
test_nvme_fabric_prop_get_cmd(void)
{
	int rc;
	uint64_t value;
	struct spdk_nvme_ctrlr ctrlr = {};

	memset(&g_ut_cmd, 0, sizeof(g_ut_cmd));
	memset(&g_ut_response, 0, sizeof(g_ut_response));
	value = 0xFFDEADBEEF;

	/* Case 1: size is SPDK_NVMF_PROP_SIZE_4 */
	rc = nvme_fabric_prop_get_cmd_sync(&ctrlr, 1024, SPDK_NVMF_PROP_SIZE_4, &value);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ut_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	CU_ASSERT(g_ut_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET);
	CU_ASSERT(g_ut_cmd.ofst == 1024);
	CU_ASSERT(g_ut_cmd.attrib.size == SPDK_NVMF_PROP_SIZE_4);
	CU_ASSERT(g_ut_response.value.u32.low == (value & 0xFFFFFFFF));

	/* Case 2: size is SPDK_NVMF_PROP_SIZE_8 */
	memset(&g_ut_cmd, 0, sizeof(g_ut_cmd));
	memset(&g_ut_response, 0, sizeof(g_ut_response));

	rc = nvme_fabric_prop_get_cmd_sync(&ctrlr, 1024, SPDK_NVMF_PROP_SIZE_8, &value);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ut_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	CU_ASSERT(g_ut_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET);
	CU_ASSERT(g_ut_cmd.ofst == 1024);
	CU_ASSERT(g_ut_cmd.attrib.size == SPDK_NVMF_PROP_SIZE_8);
	CU_ASSERT(g_ut_response.value.u64 == value);
}

static void
test_nvme_fabric_get_discovery_log_page(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	char buffer[4096] = {};
	uint64_t offset = 0;
	int rc;

	rc = nvme_fabric_get_discovery_log_page(&ctrlr, buffer, sizeof(buffer), offset);
	CU_ASSERT(rc == 0);

	/* Get log page fail */
	MOCK_SET(spdk_nvme_ctrlr_cmd_get_log_page, -EINVAL);

	rc = nvme_fabric_get_discovery_log_page(&ctrlr, buffer, sizeof(buffer), offset);
	CU_ASSERT(rc == -1);
	MOCK_CLEAR(spdk_nvme_ctrlr_cmd_get_log_page);

	/* Completion time out */
	MOCK_SET(nvme_wait_for_completion, -1);

	rc = nvme_fabric_get_discovery_log_page(&ctrlr, buffer, sizeof(buffer), offset);
	CU_ASSERT(rc == -1);
	MOCK_CLEAR(nvme_wait_for_completion);
}

static void
test_nvme_fabric_discover_probe(void)
{
	struct spdk_nvmf_discovery_log_page_entry entry = {};
	struct spdk_nvme_probe_ctx probe_ctx = {};
	char hostnqn[256] = "nqn.2016-06.io.spdk:cnode1";
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN] = "192.168.100.8";
	char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN] = "4420";
	char trstring[SPDK_NVMF_TRSTRING_MAX_LEN + 1] = "RDMA";

	entry.trtype = SPDK_NVME_TRANSPORT_RDMA;
	entry.subtype = SPDK_NVMF_SUBTYPE_NVME;
	entry.adrfam = SPDK_NVMF_ADRFAM_IPV4;

	memcpy(entry.subnqn, hostnqn, 256);
	memcpy(entry.traddr, traddr, SPDK_NVMF_TRADDR_MAX_LEN);
	memcpy(entry.trsvcid, trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN);
	memcpy(probe_ctx.trid.trstring, trstring, sizeof(probe_ctx.trid.trstring));

	nvme_fabric_discover_probe(&entry, &probe_ctx, 1);
	CU_ASSERT(g_ut_ctrlr_is_probed == true);
	CU_ASSERT(g_ut_trid.trtype == SPDK_NVME_TRANSPORT_RDMA);
	CU_ASSERT(g_ut_trid.adrfam == SPDK_NVMF_ADRFAM_IPV4);
	CU_ASSERT(!strncmp(g_ut_trid.trstring, trstring, sizeof(trstring)));
	CU_ASSERT(!strncmp(g_ut_trid.subnqn, hostnqn, sizeof(hostnqn)));
	CU_ASSERT(!strncmp(g_ut_trid.traddr, traddr, sizeof(traddr)));
	CU_ASSERT(!strncmp(g_ut_trid.trsvcid, trsvcid, sizeof(trsvcid)));

	g_ut_ctrlr_is_probed = false;
	memset(&g_ut_trid, 0, sizeof(g_ut_trid));

	/* Entry type unsupported */
	entry.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;

	nvme_fabric_discover_probe(&entry, &probe_ctx, 1);
	CU_ASSERT(g_ut_ctrlr_is_probed == false);

	/* Entry type invalid */
	entry.subtype = 3;

	nvme_fabric_discover_probe(&entry, &probe_ctx, 1);
	CU_ASSERT(g_ut_ctrlr_is_probed == false);
}

static void
test_nvme_fabric_qpair_connect(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct nvme_request	reserved_req = {};
	struct nvme_request	req = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvmf_fabric_connect_cmd *cmd = NULL;
	int rc;
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1] = "nqn.2016-06.io.spdk:host1";
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1] = "nqn.2016-06.io.spdk:subsystem1";
	const uint8_t hostid[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
	};

	cmd = (void *)&reserved_req.cmd;
	qpair.ctrlr = &ctrlr;
	req.qpair = &qpair;
	reserved_req.qpair = &qpair;
	STAILQ_INIT(&qpair.free_req);
	STAILQ_INSERT_HEAD(&qpair.free_req, &req, stailq);
	qpair.reserved_req = &reserved_req;
	memset(&g_nvmf_data, 0, sizeof(g_nvmf_data));

	qpair.id = 1;
	ctrlr.opts.keep_alive_timeout_ms = 100;
	ctrlr.cntlid = 2;
	memcpy(ctrlr.opts.extended_host_id, hostid, sizeof(hostid));
	memcpy(ctrlr.opts.hostnqn, hostnqn, sizeof(hostnqn));
	memcpy(ctrlr.trid.subnqn, subnqn, sizeof(subnqn));

	rc = nvme_fabric_qpair_connect(&qpair, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cmd->opcode == SPDK_NVME_OPC_FABRIC);
	CU_ASSERT(cmd->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT);
	CU_ASSERT(cmd->qid == 1);
	CU_ASSERT(cmd->sqsize == 0);
	CU_ASSERT(cmd->kato == 100);
	CU_ASSERT(g_nvmf_data.cntlid == 2);
	CU_ASSERT(!strncmp(g_nvmf_data.hostid, ctrlr.opts.extended_host_id, sizeof(g_nvmf_data.hostid)));
	CU_ASSERT(!strncmp(g_nvmf_data.hostnqn, ctrlr.opts.hostnqn, sizeof(ctrlr.opts.hostnqn)));
	CU_ASSERT(!strncmp(g_nvmf_data.subnqn, ctrlr.trid.subnqn, sizeof(ctrlr.trid.subnqn)));
	/* Make sure we used the qpair's reserved_req, and not one from the STAILQ */
	CU_ASSERT(g_request == qpair.reserved_req);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.free_req));

	/* qid is adminq */
	memset(&g_nvmf_data, 0, sizeof(g_nvmf_data));
	memset(&reserved_req, 0, sizeof(reserved_req));
	qpair.id = 0;
	ctrlr.cntlid = 0;

	rc = nvme_fabric_qpair_connect(&qpair, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cmd->opcode == SPDK_NVME_OPC_FABRIC);
	CU_ASSERT(cmd->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT);
	CU_ASSERT(cmd->qid == 0);
	CU_ASSERT(cmd->sqsize == 0);
	CU_ASSERT(cmd->kato == 100);
	CU_ASSERT(ctrlr.cntlid == 1);
	CU_ASSERT(g_nvmf_data.cntlid == 0xffff);
	CU_ASSERT(!strncmp(g_nvmf_data.hostid, ctrlr.opts.extended_host_id, sizeof(g_nvmf_data.hostid)));
	CU_ASSERT(!strncmp(g_nvmf_data.hostnqn, ctrlr.opts.hostnqn, sizeof(ctrlr.opts.hostnqn)));
	CU_ASSERT(!strncmp(g_nvmf_data.subnqn, ctrlr.trid.subnqn, sizeof(ctrlr.trid.subnqn)));
	/* Make sure we used the qpair's reserved_req, and not one from the STAILQ */
	CU_ASSERT(g_request == qpair.reserved_req);
	CU_ASSERT(!STAILQ_EMPTY(&qpair.free_req));

	/* Wait_for completion timeout */
	g_nvme_wait_for_completion_timeout = true;

	rc = nvme_fabric_qpair_connect(&qpair, 1);
	CU_ASSERT(rc == -ECANCELED);
	g_nvme_wait_for_completion_timeout = false;
	abort_request(g_request);

	/* Input parameters invalid */
	rc = nvme_fabric_qpair_connect(&qpair, 0);
	CU_ASSERT(rc == -EINVAL);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_fabric", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_fabric_prop_set_cmd);
	CU_ADD_TEST(suite, test_nvme_fabric_prop_get_cmd);
	CU_ADD_TEST(suite, test_nvme_fabric_get_discovery_log_page);
	CU_ADD_TEST(suite, test_nvme_fabric_discover_probe);
	CU_ADD_TEST(suite, test_nvme_fabric_qpair_connect);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
