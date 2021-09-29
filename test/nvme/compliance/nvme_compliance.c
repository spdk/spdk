/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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
#include "spdk/log.h"
#include "spdk/nvme.h"

static struct spdk_nvme_transport_id g_trid;
static const char *g_trid_str;

struct status {
	bool			done;
	struct spdk_nvme_cpl	cpl;
};

static void
wait_for_admin_completion(struct status *s, struct spdk_nvme_ctrlr *ctrlr)
{
	/* Timeout if command does not complete within 1 second. */
	uint64_t timeout = spdk_get_ticks() + spdk_get_ticks_hz();

	while (!s->done && spdk_get_ticks() < timeout) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (!s->done) {
		CU_ASSERT(false && "completion timeout");
	}
}

static void
wait_for_io_completion(struct status *s, struct spdk_nvme_qpair *qpair)
{
	/* Timeout if command does not complete within 1 second. */
	uint64_t timeout = spdk_get_ticks() + spdk_get_ticks_hz();

	while (!s->done && spdk_get_ticks() < timeout) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	if (!s->done) {
		CU_ASSERT(false && "completion timeout");
	}
}

static void
test_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct status *s = ctx;

	s->done = true;
	s->cpl = *cpl;
}

/* Test that target correctly handles requests to delete admin SQ/CQ (QID = 0).
 * Associated with issue #2172.
 */
static void
delete_admin_queue(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	/* Try deleting SQ for QID 0 (admin queue).  This is invalid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 0; /* admin queue */

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);

	/* Try deleting CQ for QID 0 (admin queue).  This is invalid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 0; /* admin queue */

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);

	spdk_nvme_detach(ctrlr);
}

static void
delete_io_sq_twice(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_ns *ns;
	uint32_t nsid;
	struct spdk_nvme_cmd cmd;
	struct status s;
	void *buf;
	uint32_t nlbas;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(qpair);

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	SPDK_CU_ASSERT_FATAL(ns != NULL);

	/* READ command should execute successfully. */
	nlbas = 1;
	buf = spdk_dma_zmalloc(nlbas * spdk_nvme_ns_get_sector_size(ns), 0x1000,  NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	s.done = false;
	rc = spdk_nvme_ns_cmd_read_with_md(ns, qpair, buf, NULL, 0, nlbas, test_cb, &s, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	wait_for_io_completion(&s, qpair);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Delete SQ 1, this is valid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Try deleting SQ 1 again, this is invalid. */
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *opts)
{
	char op;

	while ((op = getopt(argc, argv, "gr:")) != -1) {
		switch (op) {
		case 'g':
			opts->hugepage_single_segments = true;
			break;
		case 'r':
			g_trid_str = optarg;
			break;
		default:
			SPDK_ERRLOG("Unknown op '%c'\n", op);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct spdk_env_opts	opts;
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_compliance", NULL, NULL);

	spdk_env_opts_init(&opts);
	opts.name = "nvme_compliance";
	if (parse_args(argc, argv, &opts)) {
		fprintf(stderr, "could not parse_args\n");
		return -1;
	}

	if (g_trid_str == NULL) {
		fprintf(stderr, "-t <trid> not specified\n");
		return -1;
	}

	if (spdk_env_init(&opts)) {
		fprintf(stderr, "could not spdk_env_init\n");
		return -1;
	}

	CU_ADD_TEST(suite, delete_admin_queue);
	CU_ADD_TEST(suite, delete_io_sq_twice);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
