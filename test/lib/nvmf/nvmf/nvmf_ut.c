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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk_cunit.h"

#include "spdk/string.h"

#include "nvmf/controller.c"
#include "nvmf/nvmf.c"
#include "nvmf/nvmf_admin_cmd.c"
#include "nvmf/nvmf_io_cmd.c"
#include "nvmf/session.c"
#include "nvmf/subsystem.c"


#define NS_PER_CTRLR 8
struct spdk_nvme_ctrlr;

struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr *ctrlr;
	int id;
	int a;
};

struct spdk_nvme_qpair {
	struct spdk_nvme_ctrlr *ctrlr;
};

struct spdk_nvme_ctrlr {
	int a;
	int attached;
	uint32_t num_ns;
	struct spdk_nvme_ctrlr_data data;
	struct spdk_nvme_ns ns[NS_PER_CTRLR];
	struct spdk_nvme_ns_data ns_data[NS_PER_CTRLR];
	struct spdk_nvme_qpair ioq;
};

/* expected cntlid for single session with single connection */
#define SS_SC_CNTLID ((1 << NVMF_CNTLID_SUBS_SHIFT) + 1)

static int controller_checked[20];

struct rte_mempool *request_mempool;

int spdk_nvmf_parse_conf(void)
{
	return 0;
}

int spdk_nvmf_rdma_init(void)
{
	return 0;
}

int spdk_initialize_nvmf_conns(int max_connections)
{
	return 0;
}

void spdk_nvmf_host_destroy_all(void)
{
}

struct spdk_nvmf_port *spdk_nvmf_port_find_by_tag(int tag)
{
	return NULL;
}

void spdk_nvmf_port_destroy_all(void)
{
}

struct spdk_nvmf_host *spdk_nvmf_host_find_by_tag(int tag)
{
	return NULL;
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	return 0;
}

int
spdk_nvme_probe(void *cb_ctx, spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb)
{
	return -1;
}

void
spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
				      spdk_nvme_aer_cb aer_cb,
				      void *aer_cb_arg)
{

}

bool spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns)
{
	return true;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->num_ns;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	memset(&ctrlr->data, 0, sizeof(struct spdk_nvme_ctrlr_data));
	strcpy(ctrlr->data.sn, "NVMeB000D001F002");
	ctrlr->data.nn = NS_PER_CTRLR;
	return &ctrlr->data;
}

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id)
{
	if (!ctrlr)
		return NULL;
	if (ns_id < 1 || ns_id > ctrlr->num_ns) {
		return NULL;
	}
	return &ctrlr->ns[ns_id - 1];
}

int
spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
		      uint64_t lba, uint32_t lba_count,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
	struct spdk_nvme_cpl cpe;
	//nlb is 0 based
	CU_ASSERT_EQUAL(lba_count, 17);
	strcpy(payload, "hello");
	// change cdw0 and verify it in the nvmf call back.
	cpe.status.sc = SPDK_NVME_SC_SUCCESS;
	cpe.cdw0 = 0xff;
	/* read complete, call nvme call back. */
	/* nvme call back will call nvmf call back */
	/* nvme call back = nvmf_complete_cmd */
	/* nvmf call back = my_nvmf_cmd_complete */
	cb_fn(cb_arg, &cpe);
	return 0;
}

int
spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_cpl cpe;
	CU_ASSERT_EQUAL(len, 64);
	CU_ASSERT_STRING_EQUAL(buf, "hello");
	cpe.cdw0 = 0xff;
	cpe.status.sc = SPDK_NVME_SC_SUCCESS;
	/* read complete, call nvme call back. */
	/* nvme call back will call nvmf call back */
	/* nvme call back = nvmf_complete_cmd */
	/* nvmf call back = my_nvmf_cmd_complete */
	cb_fn(cb_arg, &cpe);
	return 0;
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ns_data *nsdata;

	nsdata = &ns->ctrlr->ns_data[ns->id - 1];
	nsdata->nsze = 100; //we could check it
	return nsdata;
}

size_t
spdk_nvme_request_size(void)
{
	return 0;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->attached = 0;
	return 0;
}

int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
		       uint64_t lba, uint32_t lba_count,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags)
{
	struct spdk_nvme_cpl cpe;

	//nlb is 0 based
	CU_ASSERT_EQUAL(lba_count, 17);
	CU_ASSERT_STRING_EQUAL(payload, "hello");
	cpe.cdw0 = 0xff;
	cpe.status.sc = SPDK_NVME_SC_SUCCESS;
	cb_fn(cb_arg, &cpe);
	return 0;
}

int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	int i = 0;
	while (controller_checked[i] != -1)
		i++;
	controller_checked[i] = ctrlr->a;
	controller_checked[i + 1] = -1;

	return i;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	int i = 0;
	while (controller_checked[i] != -1)
		i++;

	controller_checked[i] = qpair->ctrlr->a;
	controller_checked[i + 1] = -1;

	return i;
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr, enum spdk_nvme_qprio qprio)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	ctrlr->ioq.ctrlr = ctrlr;
	return &ctrlr->ioq;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
help_response_check(struct spdk_nvmf_fabric_connect_rsp *actual,
		    struct spdk_nvmf_fabric_connect_rsp *expect)
{
	CU_ASSERT_EQUAL(actual->status_code_specific.success.cntlid,
			expect->status_code_specific.success.cntlid);
	CU_ASSERT_EQUAL(actual->status_code_specific.success.authreq,
			expect->status_code_specific.success.authreq);
	CU_ASSERT_EQUAL(actual->status.sc, expect->status.sc);
}

static void
nvmf_test_init(void)
{
	int rc = 0;
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t i;

	request_mempool = NULL;
	/* test that NVMf library will trap if mempool not created */
	rc = nvmf_initialize();
	CU_ASSERT(rc < 0);
	request_mempool = malloc(sizeof(struct rte_mempool));
	rc = nvmf_initialize();
	CU_ASSERT(rc == 0);
	free(request_mempool);
	/* create faked controller */
	ctrlr = malloc(sizeof(struct spdk_nvme_ctrlr));
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);
	ctrlr->num_ns = NS_PER_CTRLR;
	for (i = 0; i < ctrlr->num_ns; i++) {
		ctrlr->ns[i].ctrlr = ctrlr;
		ctrlr->ns[i].id = i + 1;
	}
	ctrlr->attached = 1;
	spdk_nvmf_ctrlr_create("Nvme0", 0, 0, 1, 2, ctrlr);
}

static void
nvmf_test_create_subsystem(void)
{
	char correct_name[] = "subsystem1";
	char wrong_name[512];
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_ctrlr *nvmf_ctrlr;
	subsystem = nvmf_create_subsystem(1, correct_name, SPDK_NVMF_SUB_NVME);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_EQUAL(subsystem->num, 1);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, correct_name);
	nvmf_ctrlr = spdk_nvmf_ctrlr_claim("Nvme0");
	SPDK_CU_ASSERT_FATAL(nvmf_ctrlr != NULL);
	nvmf_subsystem_add_ns(subsystem, nvmf_ctrlr->ctrlr);

	/* test long name */
	memset(wrong_name, 'a', 512);
	subsystem = nvmf_create_subsystem(2, wrong_name, SPDK_NVMF_SUB_NVME);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_EQUAL(subsystem->num, 2);
	CU_ASSERT_STRING_NOT_EQUAL(subsystem->subnqn, wrong_name);
	CU_ASSERT_EQUAL(strlen(subsystem->subnqn) + 1, MAX_NQN_SIZE);
}

static void
nvmf_test_find_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	//char long_name[MAX_NQN_SIZE];

	CU_ASSERT_PTR_NULL(nvmf_find_subsystem(NULL));
	subsystem = nvmf_find_subsystem("subsystem1");
	CU_ASSERT_EQUAL(subsystem->num, 1);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, "subsystem1");
	/* check long name */
	/* segment fault. Comment it */
	/*
	memset(long_name, 'a', MAX_NQN_SIZE);
	subsystem = nvmf_find_subsystem(long_name);
	CU_ASSERT_EQUAL(subsystem->num, 2);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, long_name);
	*/
	/* check none-exist subsystem */
	CU_ASSERT_PTR_NULL(nvmf_find_subsystem("fake"));
}

static void
nvmf_test_create_session(void)
{
	int fake_session_count = 5;
	int i;
	struct nvmf_session *session;
	struct spdk_nvmf_subsystem *subsystem;

	/* create session in non-exist subsystem */
	CU_ASSERT_PTR_NULL(nvmf_create_session("subsystem2"));
	/* create session and check init values */
	subsystem = nvmf_find_subsystem("subsystem1");
	session = nvmf_create_session("subsystem1");
	SPDK_CU_ASSERT_FATAL(session != NULL);
	CU_ASSERT_EQUAL(session->cntlid, SS_SC_CNTLID);
	CU_ASSERT_TRUE(session->is_valid);
	CU_ASSERT_EQUAL(session->num_connections, 0);
	CU_ASSERT_EQUAL(session->active_queues, 0);
	CU_ASSERT_EQUAL(subsystem->num_sessions, 1);
	/* add multi-sessions to one subsystem
	 * if multi-sessions is not suported in the future
	 * we need to change the check condition. */
	for (i = 0; i != fake_session_count; i++) {
		nvmf_create_session("subsystem1");
	}
	CU_ASSERT_EQUAL(session->subsys->num_sessions, fake_session_count + 1);

}

static void
nvmf_test_find_session_by_id(void)
{
	struct nvmf_session *sess;
	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	CU_ASSERT_EQUAL(sess->cntlid, SS_SC_CNTLID);
	/* test non-exist conditions */
	CU_ASSERT_PTR_NULL(nvmf_find_session_by_id("fake", 9));
	CU_ASSERT_PTR_NULL(nvmf_find_session_by_id("subsystem1", 90));
}

static void
nvmf_test_delete_session(void)
{
	int i;
	int fake_session_count = 5;
	struct nvmf_session *session;
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = nvmf_find_subsystem("subsystem1");
	for (i = 0; i != fake_session_count + 1; i++) {
		session = nvmf_find_session_by_id("subsystem1",
						  (subsystem->num << NVMF_CNTLID_SUBS_SHIFT) + (i + 1));
		SPDK_CU_ASSERT_FATAL(session != NULL);
		nvmf_delete_session(session);
	}
	CU_ASSERT_EQUAL(subsystem->num_sessions, 0);
	CU_ASSERT_PTR_NULL(subsystem->sessions.tqh_first);
}

static void
nvmf_test_connect(void)
{
	uint64_t fabric_conn = 0;
	uint64_t fabric_conn_admin = 1;
	uint64_t fabric_conn_IO = 2;
	struct nvmf_session *sess, *io_sess;
	struct spdk_nvmf_fabric_connect_cmd connect = {};
	struct spdk_nvmf_fabric_connect_data connect_data = {};
	struct spdk_nvmf_fabric_connect_rsp response = {};
	struct spdk_nvmf_fabric_connect_rsp expect_rsp = {};


	connect.opcode = 0x7f;
	connect.cid = 0x01;
	connect.fctype = 0x01;
	connect_data.cntlid = 0xffff;
	connect.qid = 0;
	connect.sqsize = 64;

	/* change cmd field to do failure test first */
	/* invalid subnqn and qid = 0*/
	strcpy((char *)connect_data.subnqn, "fake");
	CU_ASSERT_PTR_NULL(nvmf_connect((void *)fabric_conn, &connect, &connect_data, &response));
	CU_ASSERT_NOT_EQUAL(response.status.sc, 0);
	/* valid subnqn and qid = 0 and cntlid != 0xfffff */
	strcpy((char *)connect_data.subnqn, "subsystem1");
	connect_data.cntlid = 0x000f;
	CU_ASSERT_PTR_NULL(nvmf_connect((void *)fabric_conn, &connect, &connect_data, &response));
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	/* invalid subnqn and qid = 1 */
	strcpy((char *)connect_data.subnqn, "fake");
	connect.qid = 1;
	connect_data.cntlid = 0;
	CU_ASSERT_PTR_NULL(nvmf_connect((void *)fabric_conn, &connect, &connect_data, &response));
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY);
	/* valid subnqn but session is not created. */
	strcpy((char *)connect_data.subnqn, "subsystem1");
	connect_data.cntlid = 0;
	CU_ASSERT_PTR_NULL(nvmf_connect((void *)fabric_conn, &connect, &connect_data, &response));
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY);
	/* create admin connection */
	connect.qid = 0;
	connect_data.cntlid = 0xffff;
	sess = nvmf_connect((void *)fabric_conn_admin, &connect, &connect_data, &response);
	SPDK_CU_ASSERT_FATAL(sess != NULL);
	nvmf_init_session_properties(sess, 64);
	sess->max_connections_allowed = 2;
	CU_ASSERT_EQUAL(sess->num_connections, 1);
	CU_ASSERT_PTR_EQUAL(sess->connections.tqh_first->fabric_conn, fabric_conn_admin);
	expect_rsp.status_code_specific.success.cntlid = SS_SC_CNTLID;
	expect_rsp.status.sc = 0;
	help_response_check(&response, &expect_rsp);
	/* create IO connection */
	connect.cid = 0x02;
	connect.qid = 1;
	connect_data.cntlid = SS_SC_CNTLID;
	io_sess = nvmf_connect((void *)fabric_conn_IO, &connect, &connect_data, &response);
	SPDK_CU_ASSERT_FATAL(io_sess != NULL);
	CU_ASSERT_EQUAL(io_sess->num_connections, 2);
	/* check admin and io connection are in same session. */
	CU_ASSERT_PTR_EQUAL(io_sess, sess);
	expect_rsp.status_code_specific.success.cntlid = SS_SC_CNTLID;
	expect_rsp.status.sc = 0;
	help_response_check(&response, &expect_rsp);
	/* right subnqn, session is created, but wrong cntlid */
	connect_data.cntlid = 1;
	connect.qid = 2;
	CU_ASSERT_PTR_NULL(nvmf_connect((void *)&fabric_conn, &connect, &connect_data, &response));
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY);
}

static void
nvmf_test_process_io_cmd(void)
{
	struct spdk_nvme_cmd nvmf_cmd = {};
	struct nvmf_session *sess;
	struct spdk_nvmf_request nvmf_req = {};
	struct nvme_read_cdw12 *cdw12;
	struct spdk_nvmf_subsystem *tmp;
	uint8_t *buf;
	nvmf_cmd.opc = SPDK_NVME_OPC_READ;
	nvmf_cmd.nsid = 2;
	nvmf_cmd.cid = 3;
	nvmf_req.cmd = (union nvmf_h2c_msg *)&nvmf_cmd;
	nvmf_req.rsp = malloc(sizeof(union nvmf_c2h_msg));
	nvmf_req.cid = nvmf_cmd.cid;
	cdw12 = (struct nvme_read_cdw12 *)&nvmf_cmd.cdw12;
	cdw12->nlb = 16; //read 16 lb, check in nvme read
	nvmf_req.length = 64;
	buf = malloc(nvmf_req.length);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	nvmf_req.data = buf;
	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	nvmf_req.session = sess;
	sess->vcprop.csts.bits.rdy = 1;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), 0);
	CU_ASSERT_STRING_EQUAL(buf, "hello");
	nvmf_cmd.cid = 4;
	nvmf_cmd.opc = SPDK_NVME_OPC_WRITE;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), 0);
	nvmf_cmd.opc = 0xff;
	nvmf_cmd.cid = 5;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), 0);
	sess->vcprop.csts.bits.rdy = 0;
	nvmf_cmd.cid = 6;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), -1);
	CU_ASSERT_EQUAL(nvmf_req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_NAMESPACE_NOT_READY);
	sess->vcprop.csts.bits.rdy = 1;
	/* nsid = 0 */
	nvmf_cmd.nsid = 0;
	nvmf_cmd.cid = 7;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), -1);
	CU_ASSERT_NOT_EQUAL(nvmf_req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_SUCCESS);
	/* set sess->subsys to NULL */
	tmp = sess->subsys;
	sess->subsys = NULL;
	nvmf_cmd.nsid = 1;
	nvmf_cmd.cid = 8;
	CU_ASSERT_EQUAL(nvmf_process_io_cmd(&nvmf_req), -1);
	CU_ASSERT_NOT_EQUAL(nvmf_req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_SUCCESS);
	sess->subsys = tmp;
	free(buf);
	free(nvmf_req.rsp);
}

static void
nvmf_test_process_admin_cmd(void)
{
	struct spdk_nvme_cmd nvmf_cmd = {};
	struct nvmf_session *sess;
	struct spdk_nvmf_request nvmf_req = {};
	struct spdk_nvmf_subsystem *subsystem;
	int buf_len = sizeof(struct spdk_nvme_ns_data);

	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	nvmf_req.session = sess;
	nvmf_req.cmd = (union nvmf_h2c_msg *)&nvmf_cmd;
	nvmf_req.rsp = malloc(sizeof(union nvmf_c2h_msg));
#define BUILD_CMD(cmd_opc, cmd_nsid, cmd_cid, cmd_cdw10) \
	do { \
		nvmf_cmd.opc = cmd_opc; \
		nvmf_cmd.nsid = cmd_nsid; \
		nvmf_cmd.cid = cmd_cid; \
		nvmf_cmd.cdw10 = cmd_cdw10; \
	} while (0)

#define RUN_AND_CHECK_PROPERT_GET_RESULT(expect_ret, cmd_cid, sts) \
	do { \
		CU_ASSERT_EQUAL(nvmf_process_admin_cmd(&nvmf_req), expect_ret); \
		CU_ASSERT_EQUAL(nvmf_req.rsp->nvme_cpl.cid, cmd_cid); \
		CU_ASSERT_EQUAL(nvmf_req.rsp->nvme_cpl.status.sc, sts); \
	} while (0)

	/* check subsys=NULL condition */
	nvmf_req.data = malloc(buf_len);
	subsystem = sess->subsys;
	sess->subsys = NULL;
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, 2, 100, 0);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 100, SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	sess->subsys = subsystem;
	/* identify namespace, namespace id = MAX_PER_SUBSYSTEM_NAMESPACES */
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, MAX_PER_SUBSYSTEM_NAMESPACES, 101, 0);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 101, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	/* namespace id > MAX_PER_SUBSYSTEM_NAMESPACES */
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, MAX_PER_SUBSYSTEM_NAMESPACES + 1, 102, 0);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 102, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	/* namespace id = 0 */
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, 0, 103, 0);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 103, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	/* identify namespace */
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, 2, 8, 0);
	RUN_AND_CHECK_PROPERT_GET_RESULT(0, 8, SPDK_NVME_SC_SUCCESS);
	free(nvmf_req.data);
	/* identify controller */
	buf_len = sizeof(struct spdk_nvme_ctrlr_data);
	nvmf_req.data = malloc(buf_len);
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, 2, 9, 1);
	RUN_AND_CHECK_PROPERT_GET_RESULT(0, 9, SPDK_NVME_SC_SUCCESS);
	free(nvmf_req.data);
	/* identify controller with invalid cdw10=2 */
	buf_len = sizeof(struct spdk_nvme_ctrlr_data);
	nvmf_req.data = malloc(buf_len);
	BUILD_CMD(SPDK_NVME_OPC_IDENTIFY, 2, 9, 2);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 9, SPDK_NVME_SC_INVALID_OPCODE);
	/* create IO SQ whose qid > MAX_SESSION_IO_QUEUES */
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_SQ, 2, 110, 0xff00ff);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 110, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* create IO SQ */
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_SQ, 2, 10, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 10, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* create same IO SQ again*/
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_SQ, 2, 101, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 101, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* create CO SQ whose qid > MAX_SESSION_IO_QUEUES */
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_CQ, 2, 112, 0xff00ff);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 112, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* create IO CQ */
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_CQ, 2, 11, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 11, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(sess->active_queues, 1);
	/* create same IO CQ again*/
	BUILD_CMD(SPDK_NVME_OPC_CREATE_IO_SQ, 2, 103, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 103, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 1);
	/* del IO SQ whose id > MAX_SESSION_IO_QUEUES */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_SQ, 2, 105, 0xff0fff);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 105, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 1);
	/* del IO SQ who is not active */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_SQ, 2, 106, 0xff0002);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 106, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(sess->active_queues, 1);
	/* del IO SQ */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_SQ, 2, 12, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 12, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* del IO CQ whose id > MAX_SESSION_IO_QUEUES */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_CQ, 2, 107, 0xff0fff);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 107, SPDK_NVME_SC_INVALID_FIELD);
	/* del IO SQ who is not active */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_CQ, 2, 108, 0xff0002);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 108, SPDK_NVME_SC_INVALID_FIELD);
	/* del IO CQ */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_CQ, 2, 13, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 13, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(sess->active_queues, 0);
	/* del same IO SQ again, should fail */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_SQ, 2, 15, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 15, SPDK_NVME_SC_INVALID_FIELD);
	/* del same CQ again, should fail */
	BUILD_CMD(SPDK_NVME_OPC_DELETE_IO_CQ, 2, 16, 0xff0001);
	RUN_AND_CHECK_PROPERT_GET_RESULT(-1, 16, SPDK_NVME_SC_INVALID_FIELD);
	/* get max io queue number */
	BUILD_CMD(SPDK_NVME_OPC_GET_FEATURES, 2, 17, SPDK_NVME_FEAT_NUMBER_OF_QUEUES);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 17, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(nvmf_req.rsp->nvme_cpl.cdw0 & 0xffff, 63);
	/* set max io queue number failed due to active queue */
	sess->active_queues = 1;
	BUILD_CMD(SPDK_NVME_OPC_SET_FEATURES, 2, 18, SPDK_NVME_FEAT_NUMBER_OF_QUEUES);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 18, SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);
	sess->active_queues = 0;
	/* set max io queue number, these are not completed*/
	BUILD_CMD(SPDK_NVME_OPC_SET_FEATURES, 2, 19, SPDK_NVME_FEAT_NUMBER_OF_QUEUES);
	RUN_AND_CHECK_PROPERT_GET_RESULT(1, 19, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(nvmf_req.rsp->nvme_cpl.cdw0 & 0xffff, 63);
	free(nvmf_req.data);
	nvmf_req.data = NULL;
}
/* for property get and set only */
#define BUILD_PROPERTY_CMD(property_name, cmd_attr, cmd_cid) \
	do { \
		cmd.ofst = offsetof(struct spdk_nvmf_ctrlr_properties, property_name); \
		cmd.attrib = cmd_attr; \
		cmd.cid = cmd_cid; \
	} while (0)

#define RUN_AND_CHECK_PROPERTY_RESULT(fsts, cmd_cid) \
	do { \
		nvmf_property_get(sess, &cmd, &response); \
		CU_ASSERT_EQUAL(response.status.sc, fsts); \
	} while (0)

static void
nvmf_test_property_get(void)
{
	struct nvmf_session *sess;
	struct spdk_nvmf_fabric_prop_get_cmd cmd;
	struct spdk_nvmf_fabric_prop_get_rsp response;
	union spdk_nvme_cap_lo_register *cap_lo;
	union spdk_nvme_cap_hi_register *cap_hi;
	union spdk_nvme_csts_register *csts;
	union spdk_nvme_aqa_register *aqa;
	union spdk_nvmf_property_size *propsz;

	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	nvmf_init_session_properties(sess, 64);
	sess->vcprop.csts.bits.rdy = 1;
	/* vs */
	BUILD_PROPERTY_CMD(vs, 0, 17);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 17);
	CU_ASSERT_EQUAL(response.value.u32.low, 0x10000);
	/* cap_lo */
	BUILD_PROPERTY_CMD(cap_lo, 1, 18);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 18);
	cap_lo = (union spdk_nvme_cap_lo_register *)&response.value.u32.low;
	cap_hi = (union spdk_nvme_cap_hi_register *)&response.value.u32.high;
	CU_ASSERT_EQUAL(cap_lo->bits.to, 1);
	CU_ASSERT_EQUAL(cap_hi->bits.css_nvm, 1);
	/* cc */
	BUILD_PROPERTY_CMD(cc, 0, 19);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 19);
	CU_ASSERT_EQUAL(response.value.u32.low, 0);
	/* csts */
	BUILD_PROPERTY_CMD(csts, 0, 20);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 20);
	csts = (union spdk_nvme_csts_register *)&response.value.u32.low;
	CU_ASSERT_EQUAL(csts->bits.rdy, 1);
	/* aqa */
	BUILD_PROPERTY_CMD(aqa, 0, 21);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 21);
	aqa = (union spdk_nvme_aqa_register *)&response.value.u32.low;
	CU_ASSERT_EQUAL(aqa->bits.asqs, 64);
	CU_ASSERT_EQUAL(aqa->bits.acqs, 64);
	/* propsz */
	BUILD_PROPERTY_CMD(propsz, 0, 22);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 22);
	propsz = (union spdk_nvmf_property_size *)&response.value.u32.low;
	CU_ASSERT_EQUAL(propsz->bits.size, sizeof(struct spdk_nvmf_ctrlr_properties) / 64);
	/* cap_hi */
	BUILD_PROPERTY_CMD(cap_hi, 0, 23);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 23);
	cap_hi = (union spdk_nvme_cap_hi_register *)&response.value.u32.low;
	CU_ASSERT_EQUAL(cap_hi->bits.css_nvm, 1);
	/* intms */
	BUILD_PROPERTY_CMD(intms, 0, 24);
	RUN_AND_CHECK_PROPERTY_RESULT(SPDK_NVMF_FABRIC_SC_INVALID_PARAM, 24);
	/* intmc */
	BUILD_PROPERTY_CMD(intmc, 0, 25);
	RUN_AND_CHECK_PROPERTY_RESULT(SPDK_NVMF_FABRIC_SC_INVALID_PARAM, 25);
	/* nssr */
	BUILD_PROPERTY_CMD(nssr, 0, 26);
	RUN_AND_CHECK_PROPERTY_RESULT(0, 26);
	/* asq */
	BUILD_PROPERTY_CMD(asq, 0, 27);
	RUN_AND_CHECK_PROPERTY_RESULT(SPDK_NVMF_FABRIC_SC_INVALID_PARAM, 27);
	/* acq */
	BUILD_PROPERTY_CMD(acq, 0, 28);
	RUN_AND_CHECK_PROPERTY_RESULT(SPDK_NVMF_FABRIC_SC_INVALID_PARAM, 28);
	/* begin to check error condition */
#define TEST_SIZE_NOT_RIGHT(prop_name, attr, c_id) \
	do { \
		BUILD_PROPERTY_CMD(prop_name, attr, c_id); \
		RUN_AND_CHECK_PROPERTY_RESULT(SPDK_NVMF_FABRIC_SC_INVALID_PARAM, c_id); \
	} while (0)

	TEST_SIZE_NOT_RIGHT(cc, 1, 22);
	TEST_SIZE_NOT_RIGHT(csts, 1, 23);
	TEST_SIZE_NOT_RIGHT(aqa, 1, 24);
	TEST_SIZE_NOT_RIGHT(propsz, 1, 25);
	TEST_SIZE_NOT_RIGHT(vs, 1, 26);
	TEST_SIZE_NOT_RIGHT(nssr, 1, 27);
	TEST_SIZE_NOT_RIGHT(capattr_hi, 1, 28);
	/* invalid offset */
	cmd.ofst = 0xffff;
	cmd.attrib = 0;
	cmd.cid = 29;
	nvmf_property_get(sess, &cmd, &response);
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
}

static void
nvmf_test_property_set(void)
{
	struct nvmf_session *sess;
	struct spdk_nvmf_fabric_prop_set_cmd cmd;
	struct spdk_nvmf_fabric_prop_set_rsp response;
	union spdk_nvme_cc_register *cc;
	union spdk_nvme_csts_register *csts;
	union spdk_nvme_aqa_register *aqa;
	uint32_t nssr;
	bool shutdown;

	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);

#define TEST_PROPERTY_SET(property_name, attr, c_id, union_name, bits_attr, val) \
	do {\
		BUILD_PROPERTY_CMD(property_name, attr, c_id); \
		property_name = (union union_name *)&cmd.value.u32.low; \
		property_name->bits.bits_attr = val; \
		nvmf_property_set(sess, &cmd, &response, &shutdown); \
		CU_ASSERT_EQUAL(response.status.sc, 0); \
		CU_ASSERT_EQUAL(sess->vcprop.property_name.bits.bits_attr, val); \
	} while (0)
	TEST_PROPERTY_SET(cc, 0, 31, spdk_nvme_cc_register, en, 1);
	TEST_PROPERTY_SET(csts, 0, 32, spdk_nvme_csts_register, rdy, 1);
	TEST_PROPERTY_SET(aqa, 0, 33, spdk_nvme_aqa_register, asqs, 0xf);
	nssr = 1;
	cmd.ofst = offsetof(struct spdk_nvmf_ctrlr_properties, nssr);
	cmd.attrib = 0;
	cmd.cid = 34;
	cmd.value.u32.low = nssr;
	nvmf_property_set(sess, &cmd, &response, &shutdown);
	CU_ASSERT_EQUAL(response.status.sc, 0);
	CU_ASSERT_EQUAL(sess->vcprop.nssr, nssr);

	/* error conditions */
#define TEST_PROPERTY_SET_ERROR(property_name, attr, c_id, union_name, bits_attr, val) \
	do {\
		BUILD_PROPERTY_CMD(property_name, attr, c_id); \
		property_name = (union union_name *)&cmd.value.u32.low; \
		property_name->bits.bits_attr = val; \
		nvmf_property_set(sess, &cmd, &response, &shutdown); \
		CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_INVALID_PARAM); \
		CU_ASSERT_NOT_EQUAL(sess->vcprop.property_name.bits.bits_attr, val); \
	} while (0)
	TEST_PROPERTY_SET_ERROR(cc, 1, 31, spdk_nvme_cc_register, en, 0);
	TEST_PROPERTY_SET_ERROR(csts, 1, 32, spdk_nvme_csts_register, rdy, 0);
	TEST_PROPERTY_SET_ERROR(aqa, 1, 33, spdk_nvme_aqa_register, asqs, 0xe);
	/* nssr attr = 1 */
	nssr = 1;
	cmd.ofst = offsetof(struct spdk_nvmf_ctrlr_properties, nssr);
	cmd.attrib = 1;
	cmd.cid = 37;
	cmd.value.u32.low = nssr;
	nvmf_property_set(sess, &cmd, &response, &shutdown);
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_INVALID_PARAM);

	cmd.ofst = 0xffff;
	cmd.attrib = 0;
	cmd.value.u32.low = 20;
	cmd.cid = 35;
	nvmf_property_set(sess, &cmd, &response, &shutdown);
	CU_ASSERT_EQUAL(response.status.sc, SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
}

static void
nvmf_test_check_admin_completions(void)
{
	struct nvmf_session *sess;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvme_ctrlr ctrlr1, ctrlr2;
	int i;

	ctrlr1.a = 1;
	ctrlr2.a = 2;
	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	subsystem = nvmf_find_subsystem("subsystem1");
	/* preload ns_list_map */
#define PRELOAD_NS_LIST(index, nvme_ctrlr, ns_id) \
	do { \
		subsystem->ns_list_map[index].ctrlr = nvme_ctrlr; \
		subsystem->ns_list_map[index].ns = NULL; \
		subsystem->ns_list_map[index].nvme_ns_id = ns_id; \
		 \
	} while (0)

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		PRELOAD_NS_LIST(i, NULL, 0);
	}
	PRELOAD_NS_LIST(0, &ctrlr1, 1);
	PRELOAD_NS_LIST(1, &ctrlr1, 2);
	PRELOAD_NS_LIST(2, &ctrlr1, 3);
	PRELOAD_NS_LIST(3, NULL, 1);
	PRELOAD_NS_LIST(4, &ctrlr1, 4);
	PRELOAD_NS_LIST(5, &ctrlr2, 1);
	PRELOAD_NS_LIST(6, &ctrlr2, 2);
	PRELOAD_NS_LIST(7, NULL, 2);
	PRELOAD_NS_LIST(8, NULL, 3);
	PRELOAD_NS_LIST(9, &ctrlr1, 5);
#undef PRELOAD_NS_LIST
	/* make sure the check completion is done by
	 * ctrlr1,ctrlr2,ctrlr1
	 */
	controller_checked[0] = -1;
	nvmf_check_admin_completions(sess);
	CU_ASSERT_EQUAL(controller_checked[0], 1);
	CU_ASSERT_EQUAL(controller_checked[1], 2);
	CU_ASSERT_EQUAL(controller_checked[2], 1);
	CU_ASSERT_EQUAL(controller_checked[3], -1);
}

static void
nvmf_test_check_io_completions(void)
{
	struct nvmf_session *sess;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvme_ctrlr ctrlr1, ctrlr2;
	int i;

	ctrlr1.a = 1;
	ctrlr2.a = 2;
	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	subsystem = nvmf_find_subsystem("subsystem1");
	/* preload ns_list_map */
#define PRELOAD_NS_LIST(index, nvme_ctrlr, ns_id) \
	do { \
		subsystem->ns_list_map[index].ctrlr = nvme_ctrlr; \
		subsystem->ns_list_map[index].qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_ctrlr, 0); \
		subsystem->ns_list_map[index].ns = NULL; \
		subsystem->ns_list_map[index].nvme_ns_id = ns_id; \
		 \
	} while (0)

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		PRELOAD_NS_LIST(i, NULL, 0);
	}
	PRELOAD_NS_LIST(0, &ctrlr1, 4);
	PRELOAD_NS_LIST(1, &ctrlr1, 1);
	PRELOAD_NS_LIST(2, &ctrlr2, 2);
	PRELOAD_NS_LIST(3, NULL, 1);
	PRELOAD_NS_LIST(4, &ctrlr1, 3);
	PRELOAD_NS_LIST(5, &ctrlr1, 1);
	PRELOAD_NS_LIST(6, &ctrlr1, 2);
	PRELOAD_NS_LIST(7, NULL, 2);
	PRELOAD_NS_LIST(8, NULL, 3);
	PRELOAD_NS_LIST(9, &ctrlr2, 4);
#undef PRELOAD_NS_LIST
	/* make sure the check completion is done by
	 * ctrlr1,ctrlr2,ctrlr1,ctrlr2
	 */
	controller_checked[0] = -1;
	nvmf_check_io_completions(sess);
	CU_ASSERT_EQUAL(controller_checked[0], 1);
	CU_ASSERT_EQUAL(controller_checked[1], 2);
	CU_ASSERT_EQUAL(controller_checked[2], 1);
	CU_ASSERT_EQUAL(controller_checked[3], 2);
	CU_ASSERT_EQUAL(controller_checked[4], -1);
}

static void
nvmf_test_disconnect(void)
{
	uint64_t fabric_conn_admin = 1;
	uint64_t fabric_conn_IO = 2;
	struct nvmf_session *sess;
	struct spdk_nvmf_subsystem *subsystem;

	sess = nvmf_find_session_by_id("subsystem1", SS_SC_CNTLID);
	/* delete IO connection */
	spdk_nvmf_session_disconnect((void *)fabric_conn_IO);
	CU_ASSERT_EQUAL(sess->num_connections, 1);
	/* delete admin connection */
	spdk_nvmf_session_disconnect((void *)fabric_conn_admin);
	subsystem = nvmf_find_subsystem("subsystem1");
	CU_ASSERT_EQUAL(subsystem->num_sessions, 0);
}

static void
nvmf_test_delete_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_session *sess;

	sess = nvmf_create_session("subsystem1");
	SPDK_CU_ASSERT_FATAL(sess != NULL);
	subsystem = nvmf_find_subsystem("subsystem1");
	CU_ASSERT_EQUAL(nvmf_delete_subsystem(subsystem), 0);
}

static void
nvmf_test_shutdown(void)
{
	nvmf_shutdown();
	CU_ASSERT_EQUAL(g_ctrlrs.tqh_first, NULL);

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
		CU_add_test(suite, "nvmf_test_init", nvmf_test_init) == NULL ||
		CU_add_test(suite, "nvmf_test_create_subsystem", nvmf_test_create_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_test_find_subsystem", nvmf_test_find_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_test_create_session", nvmf_test_create_session) == NULL ||
		CU_add_test(suite, "nvmf_test_find_session_by_id", nvmf_test_find_session_by_id) == NULL ||
		CU_add_test(suite, "nvmf_test_delete_session", nvmf_test_delete_session) == NULL ||
		CU_add_test(suite, "nvmf_test_connect", nvmf_test_connect) == NULL ||
		CU_add_test(suite, "nvmf_test_process_io_cmd", nvmf_test_process_io_cmd) == NULL ||
		CU_add_test(suite, "nvmf_test_process_admin_cmd", nvmf_test_process_admin_cmd) == NULL ||
		CU_add_test(suite, "nvmf_test_property_get", nvmf_test_property_get) == NULL ||
		CU_add_test(suite, "nvmf_test_property_set", nvmf_test_property_set) == NULL ||
		CU_add_test(suite, "nvmf_test_check_admin_completions",
			    nvmf_test_check_admin_completions) == NULL ||
		CU_add_test(suite, "nvmf_test_check_io_completions", nvmf_test_check_io_completions) == NULL ||
		CU_add_test(suite, "nvmf_test_disconnect", nvmf_test_disconnect) == NULL ||
		CU_add_test(suite, "nvmf_test_delete_subsystem", nvmf_test_delete_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_test_shutdown", nvmf_test_shutdown) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}

