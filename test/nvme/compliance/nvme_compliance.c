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
#include "spdk/util.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

static struct spdk_nvme_transport_id g_trid;
static const char *g_trid_str;

struct status {
	bool			done;
	struct spdk_nvme_cpl	cpl;
};

static inline uint64_t
nvme_vtophys(struct spdk_nvme_transport_id *trid, const void *buf, uint64_t *size)
{
	/* vfio-user address translation with IOVA=VA mode */
	if (trid->trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		return spdk_vtophys(buf, size);
	} else {
		return (uint64_t)(uintptr_t)buf;
	}
}

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

/* Test that target correctly handles various IDENTIFY CNS=1 requests. */
static void
admin_identify_ctrlr_verify_dptr(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_ctrlr_data *ctrlr_data;
	struct status s;
	int rc;

	/* Allocate data buffer with 4KiB alignment, since we need to test some
	 * very specific PRP cases.
	 */
	ctrlr_data = spdk_dma_zmalloc(sizeof(*ctrlr_data), 4096, NULL);
	SPDK_CU_ASSERT_FATAL(ctrlr_data != NULL);

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_CTRLR;

	/* Test a properly formed IDENTIFY CNS=1 request. */
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ctrlr_data,
					   sizeof(*ctrlr_data), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Confirm the target fails an IDENTIFY CNS=1 request with incorrect
	 * DPTR lengths.
	 */
	s.done = false;

	/* Only specify 1KiB of data, and make sure it specifies a PRP offset
	 * that's 1KiB before the end of the buffer previously allocated.
	 *
	 * The controller needs to recognize that a full 4KiB of data was not
	 * specified in the PRPs, and should fail the command.
	 */
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd,
					   ((uint8_t *)ctrlr_data) + (4096 - 1024),
					   1024, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(spdk_nvme_cpl_is_error(&s.cpl));

	spdk_dma_free(ctrlr_data);
	spdk_nvme_detach(ctrlr);
}

/* Test that target correctly fails admin commands with fuse != 0 */
static void
admin_identify_ctrlr_verify_fused(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_ctrlr_data *ctrlr_data;
	struct status s, s2;
	int rc;

	ctrlr_data = spdk_dma_zmalloc(sizeof(*ctrlr_data), 0, NULL);
	SPDK_CU_ASSERT_FATAL(ctrlr_data != NULL);

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	/* The nvme driver waits until it sees both fused commands before submitting
	 * both to the queue - so construct two commands here and then check the
	 * both are completed with error status.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.fuse = 0x1;
	cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_CTRLR;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ctrlr_data,
					   sizeof(*ctrlr_data), test_cb, &s);
	CU_ASSERT(rc == 0);

	cmd.fuse = 0x2;
	s2.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ctrlr_data,
					   sizeof(*ctrlr_data), test_cb, &s2);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	wait_for_admin_completion(&s2, ctrlr);

	CU_ASSERT(spdk_nvme_cpl_is_error(&s.cpl));
	CU_ASSERT(spdk_nvme_cpl_is_error(&s2.cpl));

	spdk_nvme_detach(ctrlr);
	spdk_free(ctrlr_data);
}

/* Test that target correctly handles requests to delete admin SQ/CQ (QID = 0).
 * Associated with issue #2172.
 */
static void
admin_delete_io_sq_use_admin_qid(void)
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

	spdk_nvme_detach(ctrlr);
}

static void
admin_delete_io_cq_use_admin_qid(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

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
admin_delete_io_sq_delete_sq_twice(void)
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

	/* Delete CQ 1, this is valid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_create_io_sq_verify_qsize_cqid(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	union spdk_nvme_cap_register cap;
	struct spdk_nvme_ns *ns;
	uint32_t nsid, mqes;
	struct spdk_nvme_cmd cmd;
	struct status s;
	void *buf;
	uint32_t nlbas;
	uint64_t dma_addr;
	uint32_t ncqr;
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
	spdk_dma_free(buf);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	/* Get Maximum Number of CQs */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	ncqr = ((s.cpl.cdw0 & 0xffff0000u) >> 16) + 1;

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

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	mqes = cap.bits.mqes + 1;
	buf = spdk_dma_zmalloc(mqes * sizeof(cmd), 0x1000,  NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);

	/* Create SQ 1 again, qsize is 0, this is invalid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
	cmd.cdw10_bits.create_io_q.qid = 1;
	cmd.cdw10_bits.create_io_q.qsize = 0; /* 0 based value */
	cmd.cdw11_bits.create_io_sq.pc = 1;
	cmd.cdw11_bits.create_io_sq.cqid = 1;
	dma_addr = nvme_vtophys(&g_trid, buf, NULL);
	SPDK_CU_ASSERT_FATAL(dma_addr != SPDK_VTOPHYS_ERROR);
	cmd.dptr.prp.prp1 = dma_addr;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_SIZE);

	/* Create SQ 1 again, qsize is MQES + 1, this is invalid. */
	cmd.cdw10_bits.create_io_q.qsize = (uint16_t)mqes; /* 0 based value */
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_SIZE);

	/* Create SQ 1 again, CQID is 0, this is invalid. */
	cmd.cdw10_bits.create_io_q.qsize = cap.bits.mqes; /* 0 based value, valid */
	cmd.cdw11_bits.create_io_sq.cqid = 0;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);

	/* Create SQ 1 again, CQID is NCQR + 1, this is invalid. */
	cmd.cdw11_bits.create_io_sq.cqid = ncqr + 1;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);

	/* Create SQ 1 again, CQID is 1, this is valid. */
	s.done = false;
	cmd.cdw11_bits.create_io_sq.cqid = 1;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_create_io_sq_verify_pc(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	union spdk_nvme_cap_register cap;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_cmd cmd;
	struct status s;
	void *buf;
	uint64_t dma_addr;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	/* exit the rest of test case if CAP.CQR is 0 */
	if (!cap.bits.cqr) {
		spdk_nvme_detach(ctrlr);
		return;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(qpair);

	/* Delete SQ 1 first, this is valid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	buf = spdk_dma_zmalloc((cap.bits.mqes + 1) * sizeof(cmd), 0x1000,  NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);

	/* Create SQ 1, PC is 0, this is invalid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
	cmd.cdw10_bits.create_io_q.qid = 1;
	cmd.cdw10_bits.create_io_q.qsize = cap.bits.mqes;
	cmd.cdw11_bits.create_io_sq.pc = 0;
	cmd.cdw11_bits.create_io_sq.cqid = 1;
	dma_addr = nvme_vtophys(&g_trid, buf, NULL);
	SPDK_CU_ASSERT_FATAL(dma_addr != SPDK_VTOPHYS_ERROR);
	cmd.dptr.prp.prp1 = dma_addr;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Create SQ 1 again, PC is 1, this is valid. */
	s.done = false;
	cmd.cdw11_bits.create_io_sq.pc = 1;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_delete_io_cq_delete_cq_first(void)
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

	/* Delete CQ 1, this is invalid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_DELETION);

	/* Delete SQ 1, this is valid. */
	s.done = false;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Delete CQ 1 again, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_create_io_cq_verify_iv_pc(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	union spdk_nvme_cap_register cap;
	uint32_t mqes;
	uint64_t dma_addr;
	struct status s;
	void *buf;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	mqes = cap.bits.mqes + 1;
	buf = spdk_dma_zmalloc(mqes * sizeof(struct spdk_nvme_cpl), 0x1000,  NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	dma_addr = nvme_vtophys(&g_trid, buf, NULL);
	SPDK_CU_ASSERT_FATAL(dma_addr != SPDK_VTOPHYS_ERROR);

	/* Create CQ 1, IV is 2048, this is invalid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;
	cmd.cdw10_bits.create_io_q.qid = 1;
	cmd.cdw10_bits.create_io_q.qsize = mqes - 1; /* 0 based value */
	cmd.cdw11_bits.create_io_cq.pc = 1;
	cmd.cdw11_bits.create_io_cq.ien = 1;
	cmd.cdw11_bits.create_io_cq.iv = 2048;
	cmd.dptr.prp.prp1 = dma_addr;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR);

	/* exit the rest of test case if CAP.CQR is 0 */
	if (!cap.bits.cqr) {
		goto out;
	}

	/* Create CQ 1, PC is 0, this is invalid */
	cmd.cdw11_bits.create_io_cq.pc = 0;
	cmd.cdw11_bits.create_io_cq.iv = 1;
	cmd.dptr.prp.prp1 = dma_addr;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* PC is 1, this is valid */
	cmd.cdw11_bits.create_io_cq.pc = 1;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete CQ 1, this is valid. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

out:
	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
fabric_property_get(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvmf_fabric_prop_set_cmd cmd;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	cmd.ofst = 0; /* CAP */
	cmd.attrib.size = SPDK_NVMF_PROP_SIZE_8;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	/* Non-fabrics controllers should fail an SPDK_NVME_OPC_FABRIC. */
	if (spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
		CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	} else {
		CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_OPCODE);
	}

	spdk_nvme_detach(ctrlr);
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *opts)
{
	int op;

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

static void
admin_set_features_number_of_queues(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_cmd cmd;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	/* NCQR and NSQR are 65535, invalid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;
	cmd.cdw11_bits.feat_num_of_queues.bits.ncqr = UINT16_MAX;
	cmd.cdw11_bits.feat_num_of_queues.bits.nsqr = UINT16_MAX;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(qpair);

	/* After the IO queue is created, invalid */
	cmd.cdw11_bits.feat_num_of_queues.bits.ncqr = 128;
	cmd.cdw11_bits.feat_num_of_queues.bits.nsqr = 128;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);

	spdk_nvme_detach(ctrlr);
}

/* Test the mandatory features with Get Features command:
 * 01h Arbitration.
 * 02h Power Management.
 * 04h Temperature Threshold.
 * 05h Error Recovery.
 * 07h Number of Queues.
 * 08h Interrupt Coalescing.
 * 09h Interrupt Vector Configuration.
 * 0Ah Write Atomicity Normal.
 * 0Bh Asynchronous Event Configuration.
 * 0Fh Keep Alive Timer.
 * 16h Host Behavior Support.
 */
static void
admin_get_features_mandatory_features(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct status s;
	void *buf;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	/* Arbitration */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_ARBITRATION;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Power Management */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_POWER_MANAGEMENT;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Temperature Threshold */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Error Recovery */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_ERROR_RECOVERY;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Number of Queues */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Interrupt Coalescing */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_INTERRUPT_COALESCING;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Interrupt Vector Configuration */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Write Atomicity Normal */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_WRITE_ATOMICITY;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Asynchronous Event Configuration */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Keep Alive Timer */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_KEEP_ALIVE_TIMER;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Host Behavior Support */
	buf = spdk_dma_zmalloc(sizeof(struct spdk_nvme_host_behavior), 0x1000,  NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);

	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, sizeof(struct spdk_nvme_host_behavior),
					   test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);

	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_create_io_qp_max_qps(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	struct status s;
	uint32_t ncqr, nsqr, i, num_of_queues;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	/* Number of Queues */
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	nsqr = s.cpl.cdw0 & 0xffffu;
	ncqr = (s.cpl.cdw0 & 0xffff0000u) >> 16;

	num_of_queues = spdk_min(nsqr, ncqr) + 1;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* choose a small value to save memory */
	opts.io_queue_size = 2;

	/* create all the IO queue pairs, valid */
	for (i = 0; i < num_of_queues; i++) {
		qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
		CU_ASSERT(qpair != NULL);
	}

	/* create one more, invalid */
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	CU_ASSERT(qpair == NULL);

	spdk_nvme_detach(ctrlr);
}

static void
admin_identify_ns(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ns_data *ns_data;
	struct spdk_nvme_ns *ns;
	uint32_t i, active_nsid, inactive_nsid;
	uint32_t nows, npwg, max_xfer_size;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	/* Find active NSID and inactive NSID if exist */
	active_nsid = inactive_nsid = 0;
	for (i = 1; i <= cdata->nn; i++) {
		if (spdk_nvme_ctrlr_is_active_ns(ctrlr, i)) {
			active_nsid = i;
		} else {
			inactive_nsid = i;
		}

		if (active_nsid && inactive_nsid) {
			break;
		}
	}

	ns_data = spdk_dma_zmalloc(sizeof(*ns_data), 0x1000, NULL);
	SPDK_CU_ASSERT_FATAL(ns_data != NULL);

	/* NSID is 0, invalid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.nsid = 0;
	cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_NS;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ns_data,
					   sizeof(*ns_data), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);

	/* NSID is 0xffffffff, up to OACS can support NS MANAGE or not */
	cmd.nsid = 0xffffffff;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ns_data,
					   sizeof(*ns_data), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	if (!cdata->oacs.ns_manage) {
		CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	} else {
		CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));
	}

	/* NSID is active, valid */
	if (active_nsid) {
		cmd.nsid = active_nsid;
		s.done = false;
		rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ns_data,
						   sizeof(*ns_data), test_cb, &s);
		CU_ASSERT(rc == 0);

		wait_for_admin_completion(&s, ctrlr);
		CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

		max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(ctrlr);
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, active_nsid);
		SPDK_CU_ASSERT_FATAL(ns != NULL);

		if (ns_data->nsfeat.optperf) {
			npwg = ns_data->npwg + 1;
			nows = ns_data->nows + 1;

			CU_ASSERT(npwg * spdk_nvme_ns_get_sector_size(ns) <= max_xfer_size);
			CU_ASSERT(nows * spdk_nvme_ns_get_sector_size(ns) <= max_xfer_size);
			CU_ASSERT(nows % npwg == 0);
		}
	}

	/* NSID is inactive, valid and should contain zeroed data */
	if (inactive_nsid) {
		memset(ns_data, 0x5A, sizeof(*ns_data));
		cmd.nsid = inactive_nsid;
		s.done = false;
		rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, ns_data,
						   sizeof(*ns_data), test_cb, &s);
		CU_ASSERT(rc == 0);

		wait_for_admin_completion(&s, ctrlr);
		CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));
		CU_ASSERT(spdk_mem_all_zero(ns_data, sizeof(*ns_data)));
	}

	spdk_dma_free(ns_data);
	spdk_nvme_detach(ctrlr);
}

/* Mandatory Log Page Identifiers
 * 01h Error Information
 * 02h SMART / Health Information
 * 03h Firmware Slot Information
 */
static void
admin_get_log_page_mandatory_logs(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	void *buf;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	buf = spdk_dma_zmalloc(0x1000, 0x1000, NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);

	/* 01h Error Information, valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.cdw10_bits.get_log_page.numdl = sizeof(struct spdk_nvme_error_information_entry) / 4 - 1;
	cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_error_information_entry), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* 02h SMART / Health Information, valid */
	cmd.cdw10_bits.get_log_page.numdl = sizeof(struct spdk_nvme_health_information_page) / 4 - 1;
	cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_HEALTH_INFORMATION;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_health_information_page), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* 03h Firmware Slot Information, valid */
	cmd.cdw10_bits.get_log_page.numdl = sizeof(struct spdk_nvme_firmware_page) / 4 - 1;
	cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_FIRMWARE_SLOT;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_firmware_page), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_get_log_page_with_lpo(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	void *buf;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	buf = spdk_dma_zmalloc(0x1000, 0x1000, NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);

	/* 03h Firmware Slot Information, valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.cdw10_bits.get_log_page.numdl = sizeof(struct spdk_nvme_firmware_page) / 4 - 1;
	cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_FIRMWARE_SLOT;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_firmware_page), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Log Page Offset Lower is greater than spdk_nvme_firmware_page, invalid */
	cmd.cdw12 = sizeof(struct spdk_nvme_firmware_page) + 4;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_firmware_page), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Log Page Offset Lower is less than spdk_nvme_firmware_page, but greater than 0, valid */
	cmd.cdw12 = 4;
	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf,
					   sizeof(struct spdk_nvme_firmware_page), test_cb, &s);
	CU_ASSERT(rc == 0);

	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
}

static void
admin_create_io_sq_shared_cq(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;
	void *buf;
	uint64_t dma_addr;
	struct status s;
	int rc;

	SPDK_CU_ASSERT_FATAL(spdk_nvme_transport_id_parse(&g_trid, g_trid_str) == 0);
	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr);

	/* we will create 4 SQs and 2 CQs, each queue will use 1 page */
	buf = spdk_dma_zmalloc(0x6000, 0x1000, NULL);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	dma_addr = nvme_vtophys(&g_trid, buf, NULL);
	SPDK_CU_ASSERT_FATAL(dma_addr != SPDK_VTOPHYS_ERROR);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	/* Number of Queues */
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;
	cmd.cdw11_bits.feat_num_of_queues.bits.ncqr = 1; /* 0 based value */
	cmd.cdw11_bits.feat_num_of_queues.bits.nsqr = 3; /* 0 based value */

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create CQ 1, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;
	cmd.cdw10_bits.create_io_q.qid = 1;
	cmd.cdw10_bits.create_io_q.qsize = 7; /* 0 based value */
	cmd.cdw11_bits.create_io_cq.pc = 1;
	cmd.cdw11_bits.create_io_cq.ien = 1;
	cmd.cdw11_bits.create_io_cq.iv = 1;
	cmd.dptr.prp.prp1 = dma_addr;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create CQ 2, this is valid */
	cmd.cdw10_bits.create_io_q.qid = 2;
	cmd.cdw11_bits.create_io_cq.iv = 2;
	cmd.dptr.prp.prp1 = dma_addr + 0x1000;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create SQ 1, CQID 2, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
	cmd.cdw10_bits.create_io_q.qid = 1;
	cmd.cdw10_bits.create_io_q.qsize = 7; /* 0 based value */
	cmd.cdw11_bits.create_io_sq.pc = 1;
	cmd.cdw11_bits.create_io_sq.cqid = 2;
	cmd.dptr.prp.prp1 = dma_addr + 0x2000;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create SQ 2, CQID 2, this is valid */
	cmd.cdw10_bits.create_io_q.qid = 2;
	cmd.dptr.prp.prp1 = dma_addr + 0x3000;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create SQ 3, CQID 2, this is valid */
	cmd.cdw10_bits.create_io_q.qid = 3;
	cmd.dptr.prp.prp1 = dma_addr + 0x4000;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Create SQ 4, CQID 1, this is valid */
	cmd.cdw10_bits.create_io_q.qid = 4;
	cmd.cdw11_bits.create_io_sq.cqid = 1;
	cmd.dptr.prp.prp1 = dma_addr + 0x5000;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete SQ 1, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete SQ 2, this is valid */
	cmd.cdw10_bits.delete_io_q.qid = 2;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete CQ 2, this is invalid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 2;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(s.cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(s.cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_DELETION);

	/* Delete SQ 3, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 3;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete SQ 4, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd.cdw10_bits.delete_io_q.qid = 4;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete CQ 2, this is valid */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd.cdw10_bits.delete_io_q.qid = 2;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	/* Delete CQ 1, this is valid */
	cmd.cdw10_bits.delete_io_q.qid = 1;

	s.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, test_cb, &s);
	CU_ASSERT(rc == 0);
	wait_for_admin_completion(&s, ctrlr);
	CU_ASSERT(!spdk_nvme_cpl_is_error(&s.cpl));

	spdk_dma_free(buf);
	spdk_nvme_detach(ctrlr);
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

	CU_ADD_TEST(suite, admin_identify_ctrlr_verify_dptr);
	CU_ADD_TEST(suite, admin_identify_ctrlr_verify_fused);
	CU_ADD_TEST(suite, admin_identify_ns);
	CU_ADD_TEST(suite, admin_get_features_mandatory_features);
	CU_ADD_TEST(suite, admin_set_features_number_of_queues);
	CU_ADD_TEST(suite, admin_get_log_page_mandatory_logs);
	CU_ADD_TEST(suite, admin_get_log_page_with_lpo);
	CU_ADD_TEST(suite, fabric_property_get);
	CU_ADD_TEST(suite, admin_delete_io_sq_use_admin_qid);
	CU_ADD_TEST(suite, admin_delete_io_sq_delete_sq_twice);
	CU_ADD_TEST(suite, admin_delete_io_cq_use_admin_qid);
	CU_ADD_TEST(suite, admin_delete_io_cq_delete_cq_first);
	CU_ADD_TEST(suite, admin_create_io_cq_verify_iv_pc);
	CU_ADD_TEST(suite, admin_create_io_sq_verify_qsize_cqid);
	CU_ADD_TEST(suite, admin_create_io_sq_verify_pc);
	CU_ADD_TEST(suite, admin_create_io_qp_max_qps);
	CU_ADD_TEST(suite, admin_create_io_sq_shared_cq);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
