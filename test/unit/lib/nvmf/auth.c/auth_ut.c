/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */
#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"

#include "common/lib/ut_multithread.c"
#include "nvmf/auth.c"

DEFINE_STUB(spdk_nvme_dhchap_get_digest_name, const char *, (int d), NULL);
DEFINE_STUB(spdk_nvme_dhchap_get_dhgroup_name, const char *, (int d), NULL);
DEFINE_STUB(spdk_nvme_dhchap_get_digest_length, uint8_t, (int d), 0);
DECLARE_WRAPPER(RAND_bytes, int, (unsigned char *buf, int num));

static uint8_t g_rand_val;
DEFINE_RETURN_MOCK(RAND_bytes, int) = 1;

int
__wrap_RAND_bytes(unsigned char *buf, int num)
{
	memset(buf, g_rand_val, num);
	return MOCK_GET(RAND_bytes);
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair)
{
	qpair->state = SPDK_NVMF_QPAIR_ERROR;
	return 0;
}

static bool g_req_completed;

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	g_req_completed = true;
	return 0;
}

static void
ut_clear_resp(struct spdk_nvmf_request *req)
{
	memset(&req->rsp->nvme_cpl, 0, sizeof(req->rsp->nvme_cpl));
}

#define ut_prep_cmd(_req, _cmd, _buf, _len, _lfield)		\
	do {							\
		(_req)->cmd = (void *)_cmd;			\
		(_req)->iov[0].iov_base = _buf;			\
		(_req)->iov[0].iov_len = _len;			\
		(_req)->iovcnt = 1;				\
		(_req)->length = _len;				\
		(_cmd)->secp = SPDK_NVMF_AUTH_SECP_NVME;	\
		(_cmd)->spsp0 = 1;				\
		(_cmd)->spsp1 = 1;				\
		(_cmd)->_lfield = _len;				\
	} while (0)

#define ut_prep_send_cmd(req, cmd, buf, len) ut_prep_cmd(req, cmd, buf, len, tl)
#define ut_prep_recv_cmd(req, cmd, buf, len) ut_prep_cmd(req, cmd, buf, len, al)

static void
test_auth_send_recv_error(void)
{
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_subsystem subsys = {};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsys };
	struct spdk_nvmf_qpair qpair = { .ctrlr = &ctrlr };
	struct spdk_nvmf_request req = { .qpair = &qpair, .rsp = &rsp };
	struct spdk_nvme_cpl *cpl = &rsp.nvme_cpl;
	struct spdk_nvmf_fabric_auth_send_cmd send_cmd = {};
	struct spdk_nvmf_fabric_auth_recv_cmd recv_cmd = {};
	struct spdk_nvmf_qpair_auth *auth;
	int rc;

	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	ut_prep_send_cmd(&req, &send_cmd, NULL, 255);
	ut_prep_recv_cmd(&req, &recv_cmd, NULL, 255);
	auth = qpair.auth;

	/* Bad secp (send) */
	g_req_completed = false;
	req.cmd = (void *)&send_cmd;
	ut_clear_resp(&req);
	send_cmd.secp = SPDK_NVMF_AUTH_SECP_NVME + 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	send_cmd.secp = SPDK_NVMF_AUTH_SECP_NVME;

	/* Bad secp (recv) */
	g_req_completed = false;
	req.cmd = (void *)&recv_cmd;
	ut_clear_resp(&req);
	recv_cmd.secp = SPDK_NVMF_AUTH_SECP_NVME + 1;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	recv_cmd.secp = SPDK_NVMF_AUTH_SECP_NVME;

	/* Bad spsp0 (send) */
	g_req_completed = false;
	req.cmd = (void *)&send_cmd;
	ut_clear_resp(&req);
	send_cmd.spsp0 = 2;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	send_cmd.spsp0 = 1;

	/* Bad spsp0 (recv) */
	g_req_completed = false;
	req.cmd = (void *)&recv_cmd;
	ut_clear_resp(&req);
	recv_cmd.spsp0 = 2;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	recv_cmd.spsp0 = 1;

	/* Bad spsp1 (send) */
	g_req_completed = false;
	req.cmd = (void *)&send_cmd;
	ut_clear_resp(&req);
	send_cmd.spsp1 = 2;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	send_cmd.spsp1 = 1;

	/* Bad spsp1 (recv) */
	g_req_completed = false;
	req.cmd = (void *)&recv_cmd;
	ut_clear_resp(&req);
	recv_cmd.spsp1 = 2;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	recv_cmd.spsp1 = 1;

	/* Bad length (send) */
	g_req_completed = false;
	req.cmd = (void *)&send_cmd;
	ut_clear_resp(&req);
	send_cmd.tl = req.length + 1;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	send_cmd.tl = req.length;

	/* Bad length (recv) */
	g_req_completed = false;
	req.cmd = (void *)&recv_cmd;
	ut_clear_resp(&req);
	recv_cmd.al = req.length - 1;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	recv_cmd.al = req.length;

	/* Bad length (smaller than common header) */
	g_req_completed = false;
	req.cmd = (union nvmf_h2c_msg *)&send_cmd;
	ut_clear_resp(&req);
	send_cmd.tl = req.length = sizeof(struct nvmf_auth_common_header) - 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
	send_cmd.tl = req.length = 255;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	auth->fail_reason = 0;

	nvmf_qpair_auth_destroy(&qpair);
}

static void
test_auth_negotiate(void)
{
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_subsystem subsys = {};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsys };
	struct spdk_nvmf_qpair qpair = { .ctrlr = &ctrlr };
	struct spdk_nvmf_request req = { .qpair = &qpair, .rsp = &rsp };
	struct spdk_nvmf_fabric_auth_send_cmd cmd = {};
	struct spdk_nvmf_qpair_auth *auth;
	struct spdk_nvmf_auth_negotiate *msg;
	struct spdk_nvmf_auth_descriptor *desc;
	uint8_t msgbuf[4096];
	int rc;

	msg = (void *)msgbuf;
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	ut_prep_send_cmd(&req, &cmd, msgbuf, sizeof(*msg) + sizeof(*desc));
	auth = qpair.auth;

	/* Successful negotiation */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	msg->auth_id = SPDK_NVMF_AUTH_ID_NEGOTIATE;
	msg->sc_c = SPDK_NVMF_AUTH_SCC_DISABLED;
	msg->napd = 1;
	desc = &msg->descriptors[0];
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	desc->halen = 3;
	desc->hash_id_list[0] = SPDK_NVMF_DHCHAP_HASH_SHA256;
	desc->hash_id_list[1] = SPDK_NVMF_DHCHAP_HASH_SHA384;
	desc->hash_id_list[2] = SPDK_NVMF_DHCHAP_HASH_SHA512;
	desc->dhlen = 6;
	desc->dhg_id_list[0] = SPDK_NVMF_DHCHAP_DHGROUP_NULL;
	desc->dhg_id_list[1] = SPDK_NVMF_DHCHAP_DHGROUP_2048;
	desc->dhg_id_list[2] = SPDK_NVMF_DHCHAP_DHGROUP_3072;
	desc->dhg_id_list[3] = SPDK_NVMF_DHCHAP_DHGROUP_4096;
	desc->dhg_id_list[4] = SPDK_NVMF_DHCHAP_DHGROUP_6144;
	desc->dhg_id_list[5] = SPDK_NVMF_DHCHAP_DHGROUP_8192;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, SPDK_NVMF_DHCHAP_HASH_SHA512);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_CHALLENGE);

	/* Invalid auth state */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_ERROR;
	auth->digest = -1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);

	/* scc mismatch */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	msg->sc_c = SPDK_NVMF_AUTH_SCC_TLS;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_SCC_MISMATCH);
	msg->sc_c = SPDK_NVMF_AUTH_SCC_DISABLED;

	/* Missing DH-HMAC-CHAP protocol (napd=0) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg);
	msg->napd = 0;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE);
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg) + sizeof(*desc);
	msg->napd = 1;

	/* Missing DH-HMAC-CHAP protocol */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP + 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE);
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;

	/* No valid digests (halen=0) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->halen = 0;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_HASH_UNUSABLE);

	/* No valid digests */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->hash_id_list[0] = SPDK_NVMF_DHCHAP_HASH_SHA512 + 1;
	desc->hash_id_list[1] = SPDK_NVMF_DHCHAP_HASH_SHA512 + 2;
	desc->hash_id_list[2] = SPDK_NVMF_DHCHAP_HASH_SHA512 + 3;
	desc->halen = 3;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_HASH_UNUSABLE);
	desc->hash_id_list[0] = SPDK_NVMF_DHCHAP_HASH_SHA256;
	desc->hash_id_list[1] = SPDK_NVMF_DHCHAP_HASH_SHA384;
	desc->hash_id_list[2] = SPDK_NVMF_DHCHAP_HASH_SHA512;

	/* No valid dhgroups (dhlen=0) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->dhlen = 0;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_DHGROUP_UNUSABLE);

	/* No valid dhgroups */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->dhlen = 2;
	desc->dhg_id_list[0] = SPDK_NVMF_DHCHAP_DHGROUP_8192 + 1;
	desc->dhg_id_list[1] = SPDK_NVMF_DHCHAP_DHGROUP_8192 + 2;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_DHGROUP_UNUSABLE);
	desc->dhg_id_list[0] = SPDK_NVMF_DHCHAP_DHGROUP_NULL;
	desc->dhg_id_list[1] = SPDK_NVMF_DHCHAP_DHGROUP_2048;
	desc->dhlen = 6;

	/* Bad halen value */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->halen = 255;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
	desc->halen = 3;

	/* Bad dhlen value */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	desc->dhlen = 255;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
	desc->dhlen = 6;

	/* Invalid request length (too small) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg) - 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);

	/* Invalid request length (too small) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg);

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);

	/* Invalid request length (too small) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg) + sizeof(*desc) - 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);

	/* Invalid request length (too large) */
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg) + sizeof(*desc) + 1;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, -1);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_FAILURE1);
	CU_ASSERT_EQUAL(auth->fail_reason, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
	req.length = cmd.tl = req.iov[0].iov_len = sizeof(*msg) + sizeof(*desc);

	nvmf_qpair_auth_destroy(&qpair);
}

static void
test_auth_timeout(void)
{
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_subsystem subsys = {};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsys };
	struct spdk_nvmf_qpair qpair = { .ctrlr = &ctrlr };
	struct spdk_nvmf_request req = { .qpair = &qpair, .rsp = &rsp };
	struct spdk_nvmf_fabric_auth_send_cmd cmd = {};
	struct spdk_nvmf_qpair_auth *auth;
	struct spdk_nvmf_auth_negotiate *msg;
	struct spdk_nvmf_auth_descriptor *desc;
	uint8_t msgbuf[4096];
	int rc;

	msg = (void *)msgbuf;
	ut_prep_send_cmd(&req, &cmd, msgbuf, sizeof(*msg) + sizeof(*desc));
	MOCK_SET(spdk_get_ticks_hz, 1000 * 1000);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;

	/* Check that a timeout is correctly detected and qpair is disconnected */
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	MOCK_SET(spdk_get_ticks, NVMF_AUTH_DEFAULT_KATO_US - 1);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_AUTHENTICATING);
	MOCK_SET(spdk_get_ticks, NVMF_AUTH_DEFAULT_KATO_US);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	nvmf_qpair_auth_destroy(&qpair);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;
	MOCK_SET(spdk_get_ticks, 0);

	/* Check a case where a non-zero kato is set in controller features */
	ctrlr.feat.keep_alive_timer.bits.kato = 10 * 1000;
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	MOCK_SET(spdk_get_ticks, 10 * 1000 * 1000 - 1);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_AUTHENTICATING);
	MOCK_SET(spdk_get_ticks, 10 * 1000 * 1000);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	nvmf_qpair_auth_destroy(&qpair);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;
	ctrlr.feat.keep_alive_timer.bits.kato = 0;
	MOCK_SET(spdk_get_ticks, 0);

	/* Check that reception of a command rearms the timeout poller */
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	auth = qpair.auth;

	MOCK_SET(spdk_get_ticks, NVMF_AUTH_DEFAULT_KATO_US / 2);
	g_req_completed = false;
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	msg->auth_id = SPDK_NVMF_AUTH_ID_NEGOTIATE;
	msg->sc_c = SPDK_NVMF_AUTH_SCC_DISABLED;
	msg->napd = 1;
	desc = &msg->descriptors[0];
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	desc->halen = 1;
	desc->hash_id_list[0] = SPDK_NVMF_DHCHAP_HASH_SHA256;
	desc->dhlen = 1;
	desc->dhg_id_list[0] = SPDK_NVMF_DHCHAP_DHGROUP_NULL;

	nvmf_auth_send_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->digest, SPDK_NVMF_DHCHAP_HASH_SHA256);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_CHALLENGE);

	MOCK_SET(spdk_get_ticks, NVMF_AUTH_DEFAULT_KATO_US);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_AUTHENTICATING);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_CHALLENGE);

	MOCK_SET(spdk_get_ticks, NVMF_AUTH_DEFAULT_KATO_US + NVMF_AUTH_DEFAULT_KATO_US / 2);
	poll_threads();
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	nvmf_qpair_auth_destroy(&qpair);
	MOCK_SET(spdk_get_ticks, 0);
}

static void
test_auth_failure1(void)
{
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_subsystem subsys = {};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsys };
	struct spdk_nvmf_qpair qpair = { .ctrlr = &ctrlr };
	struct spdk_nvmf_request req = { .qpair = &qpair, .rsp = &rsp };
	struct spdk_nvmf_fabric_auth_recv_cmd cmd = {
		.fctype = SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV
	};
	struct spdk_nvme_cpl *cpl = &rsp.nvme_cpl;
	struct spdk_nvmf_qpair_auth *auth;
	struct spdk_nvmf_auth_failure *msg;
	uint8_t msgbuf[sizeof(*msg)];
	int rc;

	msg = (void *)msgbuf;
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	auth = qpair.auth;
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;

	/* Check failure1 message fields */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(*msg));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_FAILURE1;
	auth->fail_reason = SPDK_NVMF_AUTH_FAILED;
	auth->tid = 8;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, 0);
	CU_ASSERT_EQUAL(cpl->status.sc, 0);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_ERROR);
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	CU_ASSERT_EQUAL(msg->auth_type, SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE);
	CU_ASSERT_EQUAL(msg->auth_id, SPDK_NVMF_AUTH_ID_FAILURE1);
	CU_ASSERT_EQUAL(msg->t_id, 8);
	CU_ASSERT_EQUAL(msg->rc, SPDK_NVMF_AUTH_FAILURE);
	CU_ASSERT_EQUAL(msg->rce, SPDK_NVMF_AUTH_FAILED);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;

	/* Do a receive while expecting an auth send command */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(*msg));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_NEGOTIATE;
	auth->fail_reason = 0;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, 0);
	CU_ASSERT_EQUAL(cpl->status.sc, 0);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_ERROR);
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	CU_ASSERT_EQUAL(msg->auth_type, SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE);
	CU_ASSERT_EQUAL(msg->auth_id, SPDK_NVMF_AUTH_ID_FAILURE1);
	CU_ASSERT_EQUAL(msg->t_id, 8);
	CU_ASSERT_EQUAL(msg->rc, SPDK_NVMF_AUTH_FAILURE);
	CU_ASSERT_EQUAL(msg->rce, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;

	/* Do a receive but specify a buffer that's too small */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(*msg));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_FAILURE1;
	auth->fail_reason = SPDK_NVMF_AUTH_FAILED;
	req.iov[0].iov_len = cmd.al = req.length = sizeof(*msg) - 1;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(cpl->status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(cpl->status.sc, SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT_EQUAL(cpl->status.dnr, 1);
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	req.iov[0].iov_len = cmd.al = req.length = sizeof(*msg);

	nvmf_qpair_auth_destroy(&qpair);
}

static void
test_auth_challenge(void)
{
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_subsystem subsys = { .mutex = PTHREAD_MUTEX_INITIALIZER };
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsys };
	struct spdk_nvmf_qpair qpair = { .ctrlr = &ctrlr };
	struct spdk_nvmf_request req = { .qpair = &qpair, .rsp = &rsp };
	struct spdk_nvmf_fabric_auth_recv_cmd cmd = {
		.fctype = SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV
	};
	struct spdk_nvmf_qpair_auth *auth;
	struct spdk_nvmf_dhchap_challenge *msg;
	struct spdk_nvmf_auth_failure *fail;
	uint8_t msgbuf[4096], cval[4096];
	int rc;

	msg = (void *)msgbuf;
	fail = (void *)msgbuf;
	rc = nvmf_qpair_auth_init(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	auth = qpair.auth;
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;

	/* Successfully receive a challenge message */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(msgbuf));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_CHALLENGE;
	MOCK_SET(spdk_nvme_dhchap_get_digest_length, 48);
	g_rand_val = 0xa5;
	memset(cval, g_rand_val, sizeof(cval));
	auth->digest = SPDK_NVMF_DHCHAP_HASH_SHA384;
	auth->tid = 8;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(auth->state, NVMF_QPAIR_AUTH_REPLY);
	CU_ASSERT_EQUAL(msg->auth_type, SPDK_NVMF_AUTH_TYPE_DHCHAP);
	CU_ASSERT_EQUAL(msg->auth_id, SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE);
	CU_ASSERT_EQUAL(msg->t_id, 8);
	CU_ASSERT_EQUAL(msg->hl, 48);
	CU_ASSERT_EQUAL(msg->hash_id, SPDK_NVMF_DHCHAP_HASH_SHA384);
	CU_ASSERT_EQUAL(msg->dhg_id, SPDK_NVMF_DHCHAP_DHGROUP_NULL);
	CU_ASSERT_EQUAL(msg->dhvlen, 0);
	CU_ASSERT_EQUAL(memcmp(msg->cval, cval, 48), 0);
	CU_ASSERT(msg->seqnum != 0);

	/* Check RAND_bytes failure */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(msgbuf));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_CHALLENGE;
	MOCK_SET(spdk_nvme_dhchap_get_digest_length, 48);
	auth->digest = SPDK_NVMF_DHCHAP_HASH_SHA384;
	auth->tid = 8;
	MOCK_SET(RAND_bytes, -1);

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	CU_ASSERT_EQUAL(fail->auth_type, SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE);
	CU_ASSERT_EQUAL(fail->auth_id, SPDK_NVMF_AUTH_ID_FAILURE1);
	CU_ASSERT_EQUAL(fail->t_id, 8);
	CU_ASSERT_EQUAL(fail->rc, SPDK_NVMF_AUTH_FAILURE);
	CU_ASSERT_EQUAL(fail->rce, SPDK_NVMF_AUTH_FAILED);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;
	MOCK_SET(RAND_bytes, 1);

	/* Check insufficient buffer size */
	ut_prep_recv_cmd(&req, &cmd, msgbuf, sizeof(msgbuf));
	g_req_completed = false;
	auth->state = NVMF_QPAIR_AUTH_CHALLENGE;
	MOCK_SET(spdk_nvme_dhchap_get_digest_length, 48);
	auth->tid = 8;
	cmd.al = req.length = req.iov[0].iov_len = sizeof(msg) + 47;

	nvmf_auth_recv_exec(&req);
	CU_ASSERT(g_req_completed);
	CU_ASSERT_EQUAL(qpair.state, SPDK_NVMF_QPAIR_ERROR);
	CU_ASSERT_EQUAL(fail->auth_type, SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE);
	CU_ASSERT_EQUAL(fail->auth_id, SPDK_NVMF_AUTH_ID_FAILURE1);
	CU_ASSERT_EQUAL(fail->t_id, 8);
	CU_ASSERT_EQUAL(fail->rc, SPDK_NVMF_AUTH_FAILURE);
	CU_ASSERT_EQUAL(fail->rce, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
	qpair.state = SPDK_NVMF_QPAIR_AUTHENTICATING;
	MOCK_CLEAR(spdk_nvme_dhchap_get_digest_length);

	nvmf_qpair_auth_destroy(&qpair);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("nvmf_auth", NULL, NULL);
	CU_ADD_TEST(suite, test_auth_send_recv_error);
	CU_ADD_TEST(suite, test_auth_negotiate);
	CU_ADD_TEST(suite, test_auth_timeout);
	CU_ADD_TEST(suite, test_auth_failure1);
	CU_ADD_TEST(suite, test_auth_challenge);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
