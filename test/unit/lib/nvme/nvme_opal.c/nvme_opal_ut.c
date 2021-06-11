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
#include "nvme/nvme_opal.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(spdk_nvme_ctrlr_cmd_security_receive, int,
	    (struct spdk_nvme_ctrlr *ctrlr, uint8_t secp, uint16_t spsp,
	     uint8_t nssf, void *payload, uint32_t payload_size,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 1);

DEFINE_STUB(spdk_nvme_ctrlr_security_receive, int,
	    (struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
	     uint16_t spsp, uint8_t nssf, void *payload, size_t size), 0);

DEFINE_STUB(spdk_nvme_ctrlr_process_admin_completions, int,
	    (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_security_send, int,
	    (struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
	     uint16_t spsp, uint8_t nssf, void *payload,
	     uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

static int g_ut_recv_status = 0;
static void *g_ut_sess_ctx;

static void
ut_opal_sess_cb(struct opal_session *sess, int status, void *ctx)
{
	g_ut_recv_status = status;
	g_ut_sess_ctx = ctx;
}

static void
reset_ut_global_variables(void)
{
	g_ut_recv_status = 0;
	g_ut_sess_ctx = NULL;
}

static void
test_opal_nvme_security_recv_send_done(void)
{
	struct spdk_nvme_cpl cpl = {};
	struct spdk_opal_compacket header = {};
	struct spdk_opal_dev dev = {};
	struct opal_session sess = {};

	sess.sess_cb = ut_opal_sess_cb;
	sess.cb_arg = (void *)0xDEADBEEF;
	sess.dev = &dev;
	memcpy(sess.resp, &header, sizeof(header));

	/* Case 1: receive/send IO error */
	reset_ut_global_variables();
	cpl.status.sct = SPDK_NVME_SCT_MEDIA_ERROR;

	opal_nvme_security_recv_done(&sess, &cpl);
	CU_ASSERT(g_ut_recv_status == -EIO);
	CU_ASSERT(g_ut_sess_ctx == (void *)0xDEADBEEF);

	reset_ut_global_variables();
	opal_nvme_security_send_done(&sess, &cpl);
	CU_ASSERT(g_ut_recv_status == -EIO);
	CU_ASSERT(g_ut_sess_ctx == (void *)0xDEADBEEF);

	/* Case 2: receive with opal header no outstanding data */
	reset_ut_global_variables();
	memset(&header, 0, sizeof(header));
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	opal_nvme_security_recv_done(&sess, &cpl);
	CU_ASSERT(g_ut_recv_status == 0);
	CU_ASSERT(g_ut_sess_ctx == (void *)0xDEADBEEF);

	/* Case 3: receive with opal header outstanding data and send done success */
	reset_ut_global_variables();
	header.outstanding_data = 0xff;
	memcpy(sess.resp, &header, sizeof(header));
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	opal_nvme_security_recv_done(&sess, &cpl);
	CU_ASSERT(g_ut_recv_status == 1);
	CU_ASSERT(g_ut_sess_ctx == (void *)0xDEADBEEF);

	reset_ut_global_variables();
	opal_nvme_security_send_done(&sess, &cpl);
	CU_ASSERT(g_ut_recv_status == 1);
	CU_ASSERT(g_ut_sess_ctx == (void *)0xDEADBEEF);
}

static void
test_opal_add_short_atom_header(void)
{
	struct opal_session sess = {};
	int err = 0;

	/* short atom header */
	memset(&sess, 0, sizeof(sess));
	sess.cmd_pos = 0;

	opal_add_token_bytestring(&err, &sess, spdk_opal_uid[UID_SMUID],
				  OPAL_UID_LENGTH);
	CU_ASSERT(sess.cmd[0] & SPDK_SHORT_ATOM_ID);
	CU_ASSERT(sess.cmd[0] & SPDK_SHORT_ATOM_BYTESTRING_FLAG);
	CU_ASSERT((sess.cmd[0] & SPDK_SHORT_ATOM_SIGN_FLAG) == 0);
	CU_ASSERT(sess.cmd_pos == OPAL_UID_LENGTH +  1);
	CU_ASSERT(!memcmp(&sess.cmd[1], spdk_opal_uid, OPAL_UID_LENGTH + 1));

	/* medium atom header */
	memset(&sess, 0, sizeof(sess));
	sess.cmd_pos = 0;

	opal_add_token_bytestring(&err, &sess, spdk_opal_uid[UID_SMUID],
				  0x10);
	CU_ASSERT(sess.cmd[0] & SPDK_SHORT_ATOM_ID);
	CU_ASSERT(sess.cmd[0] & SPDK_MEDIUM_ATOM_BYTESTRING_FLAG);
	CU_ASSERT((sess.cmd[0] & SPDK_MEDIUM_ATOM_SIGN_FLAG) == 0);
	CU_ASSERT(sess.cmd_pos == 0x12);
	CU_ASSERT(!memcmp(&sess.cmd[2], spdk_opal_uid, 0x10));

	/* Invalid length */
	memset(&sess, 0, sizeof(sess));
	err = 0;

	opal_add_token_bytestring(&err, &sess, spdk_opal_uid[UID_SMUID],
				  0x1000);
	CU_ASSERT(err == -ERANGE);
	CU_ASSERT(sess.cmd_pos == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_opal", NULL, NULL);
	CU_ADD_TEST(suite, test_opal_nvme_security_recv_send_done);
	CU_ADD_TEST(suite, test_opal_add_short_atom_header);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
