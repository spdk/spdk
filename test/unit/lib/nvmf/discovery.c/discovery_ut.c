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

#include "discovery.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops;

struct spdk_nvmf_tgt g_nvmf_tgt = {
	.subsystems = TAILQ_HEAD_INITIALIZER(g_nvmf_tgt.subsystems)
};

struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(const char *trname, const char *traddr, const char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->traddr = strdup(traddr);
	if (!listen_addr->traddr) {
		free(listen_addr);
		return NULL;
	}

	listen_addr->trsvcid = strdup(trsvcid);
	if (!listen_addr->trsvcid) {
		free(listen_addr->traddr);
		free(listen_addr);
		return NULL;
	}

	listen_addr->trname = strdup(trname);
	if (!listen_addr->trname) {
		free(listen_addr->traddr);
		free(listen_addr->trsvcid);
		free(listen_addr);
		return NULL;
	}

	return listen_addr;
}

void
spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr)
{
	return;
}

bool
spdk_bdev_claim(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb,
		void *remove_ctx)
{
	return true;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

static int
test_transport1_listen_addr_add(struct spdk_nvmf_listen_addr *listen_addr)
{
	return 0;
}

static void
test_transport1_listen_addr_discover(struct spdk_nvmf_listen_addr *listen_addr,
				     struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

static const struct spdk_nvmf_transport test_transport1 = {
	.listen_addr_add = test_transport1_listen_addr_add,
	.listen_addr_discover = test_transport1_listen_addr_discover,
};

const struct spdk_nvmf_transport *
spdk_nvmf_transport_get(const char *trname)
{
	if (!strcasecmp(trname, "test_transport1")) {
		return &test_transport1;
	}

	return NULL;
}

void
spdk_nvmf_session_destruct(struct spdk_nvmf_session *session)
{
}

int
spdk_nvmf_session_poll(struct spdk_nvmf_session *session)
{
	return -1;
}

static void
test_process_discovery_cmd(void)
{
	struct	spdk_nvmf_request req = {};
	int	ret;
	/* random request length value for testing */
	int	req_length = 122;
	struct	spdk_nvmf_conn req_conn = {};
	struct	spdk_nvmf_session req_sess = {};
	struct	spdk_nvme_ctrlr_data req_data = {};
	struct	spdk_nvmf_discovery_log_page req_page = {};
	union	nvmf_h2c_msg  req_cmd = {};
	union	nvmf_c2h_msg   req_rsp = {};

	req.conn = &req_conn;
	req.cmd  = &req_cmd;
	req.rsp  = &req_rsp;

	/* no request data check */
	ret = nvmf_discovery_ctrlr_process_admin_cmd(&req);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_INVALID_FIELD);

	/* IDENTIFY opcode return value check */
	req.cmd->nvme_cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	req.cmd->nvme_cmd.cdw10 = SPDK_NVME_IDENTIFY_CTRLR;
	req.conn->sess = &req_sess;
	req.data = &req_data;
	ret = nvmf_discovery_ctrlr_process_admin_cmd(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* GET_LOG_PAGE opcode return value check */
	req.cmd->nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	req.cmd->nvme_cmd.cdw10 = SPDK_NVME_LOG_DISCOVERY;
	req.data = &req_page;
	req.length = req_length;
	ret = nvmf_discovery_ctrlr_process_admin_cmd(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	req.cmd->nvme_cmd.cdw10 = 15;
	ret = nvmf_discovery_ctrlr_process_admin_cmd(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Invalid opcode return value check */
	req.cmd->nvme_cmd.opc = 100;
	ret = nvmf_discovery_ctrlr_process_admin_cmd(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
}

static bool
all_zero(const void *buf, size_t size)
{
	const uint8_t *b = buf;

	while (size--) {
		if (*b != 0) {
			return false;
		}
		b++;
	}

	return true;
}

static void
test_discovery_log(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	uint8_t buffer[8192];
	struct spdk_nvmf_discovery_log_page *disc_log;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvmf_listen_addr *listen_addr;

	/* Reset discovery-related globals */
	g_nvmf_tgt.discovery_genctr = 0;
	free(g_nvmf_tgt.discovery_log_page);
	g_nvmf_tgt.discovery_log_page = NULL;
	g_nvmf_tgt.discovery_log_page_size = 0;

	/* Add one subsystem and verify that the discovery log contains it */
	subsystem = spdk_nvmf_create_subsystem("nqn.2016-06.io.spdk:subsystem1", SPDK_NVMF_SUBTYPE_NVME,
					       NVMF_SUBSYSTEM_MODE_DIRECT, NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	listen_addr = spdk_nvmf_tgt_listen("test_transport1", "1234", "5678");
	SPDK_CU_ASSERT_FATAL(listen_addr != NULL);

	SPDK_CU_ASSERT_FATAL(spdk_nvmf_subsystem_add_listener(subsystem, listen_addr) == 0);

	/* Get only genctr (first field in the header) */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(disc_log->genctr));
	CU_ASSERT(disc_log->genctr == 2); /* one added subsystem + one added listen address */

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(*disc_log));
	CU_ASSERT(disc_log->genctr == 2);
	CU_ASSERT(disc_log->numrec == 1);

	/* Offset 0, exact size match */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(*disc_log) + sizeof(disc_log->entries[0]));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);

	/* Offset 0, oversize buffer */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer, 0, sizeof(buffer));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);
	CU_ASSERT(all_zero(buffer + sizeof(*disc_log) + sizeof(disc_log->entries[0]),
			   sizeof(buffer) - (sizeof(*disc_log) + sizeof(disc_log->entries[0]))));

	/* Get just the first entry, no header */
	memset(buffer, 0xCC, sizeof(buffer));
	entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
	spdk_nvmf_get_discovery_log_page(buffer,
					 offsetof(struct spdk_nvmf_discovery_log_page, entries[0]),
					 sizeof(*entry));
	CU_ASSERT(entry->trtype == 42);
	spdk_nvmf_delete_subsystem(subsystem);
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
		CU_add_test(suite, "process_discovery_command", test_process_discovery_cmd) == NULL ||
		CU_add_test(suite, "discovery_log", test_discovery_log) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
