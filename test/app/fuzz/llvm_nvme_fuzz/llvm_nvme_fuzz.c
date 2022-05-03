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
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "spdk/file.h"

static const uint8_t *g_data;
static bool g_trid_specified = false;
static int32_t g_time_in_sec = 10;
static char *g_corpus_dir;
static uint8_t *g_repro_data;
static size_t g_repro_size;
static pthread_t g_fuzz_td;
static pthread_t g_reactor_td;
static bool g_in_fuzzer;

#define MAX_COMMANDS 5

struct fuzz_command {
	struct spdk_nvme_cmd	cmd;
	void			*buf;
	uint32_t		len;
};

static struct fuzz_command g_cmds[MAX_COMMANDS];

typedef void (*fuzz_build_cmd_fn)(struct fuzz_command *cmd);

struct fuzz_type {
	fuzz_build_cmd_fn	fn;
	uint32_t		bytes_per_cmd;
	bool	is_admin;
};

static void
fuzz_admin_command(struct fuzz_command *cmd)
{
	memcpy(&cmd->cmd, g_data, sizeof(cmd->cmd));
	g_data += sizeof(cmd->cmd);

	/* ASYNC_EVENT_REQUEST won't complete, so pick a different opcode. */
	if (cmd->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
		cmd->cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	}

	/* NVME_OPC_FABRIC is special for fabric transport, so pick a different opcode. */
	if (cmd->cmd.opc == SPDK_NVME_OPC_FABRIC) {
		cmd->cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	}

	/* Fuzz a normal operation, so set a zero value in Fused field. */
	cmd->cmd.fuse = 0;
}

static void
fuzz_admin_get_log_page_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));

	cmd->cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;

	/* Only fuzz some of the more interesting parts of the GET_LOG_PAGE command. */

	cmd->cmd.cdw10_bits.get_log_page.numdl = (g_data[0] << 8) + g_data[1];
	cmd->cmd.cdw10_bits.get_log_page.lid = g_data[2];
	cmd->cmd.cdw10_bits.get_log_page.lsp = g_data[3] & (0x60 >> 5);
	cmd->cmd.cdw10_bits.get_log_page.rae = g_data[3] & (0x80 >> 7);

	cmd->cmd.cdw11_bits.get_log_page.numdu = g_data[3] & (0x18 >> 3);

	/* Log Page Offset Lower */
	cmd->cmd.cdw12 = (g_data[4] << 8) + g_data[5];

	/* Offset Type */
	cmd->cmd.cdw14 = g_data[3] & (0x01 >> 0);

	/* Log Page Offset Upper */
	cmd->cmd.cdw13 = g_data[3] & (0x06 >> 1);

	g_data += 6;
}

static void
fuzz_admin_identify_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));

	cmd->cmd.opc = SPDK_NVME_OPC_IDENTIFY;

	cmd->cmd.cdw10_bits.identify.cns = g_data[0];
	cmd->cmd.cdw10_bits.identify.cntid = (g_data[1] << 8) + g_data[2];

	cmd->cmd.cdw11_bits.identify.nvmsetid = (g_data[3] << 8) + g_data[4];
	cmd->cmd.cdw11_bits.identify.csi = g_data[5];

	/* UUID index, bits 0-6 are used */
	cmd->cmd.cdw14 = (g_data[6] & 0x7f);

	g_data += 7;
}

static void
fuzz_admin_abort_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_ABORT;

	cmd->cmd.cdw10_bits.abort.sqid = (g_data[0] << 8) + g_data[1];
	cmd->cmd.cdw10_bits.abort.cid = (g_data[2] << 8) + g_data[3];

	g_data += 4;
}

static void
fuzz_admin_create_io_completion_queue_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	cmd->cmd.cdw10_bits.create_io_q.qid = (g_data[0] << 8) + g_data[1];
	cmd->cmd.cdw10_bits.create_io_q.qsize = (g_data[2] << 8) + g_data[3];

	cmd->cmd.cdw11_bits.create_io_cq.iv = (g_data[4] << 8) + g_data[5];
	cmd->cmd.cdw11_bits.create_io_cq.pc = (g_data[6] >> 7) & 0x01;
	cmd->cmd.cdw11_bits.create_io_cq.ien = (g_data[6] >> 6) & 0x01;

	g_data += 7;
}

static void
fuzz_admin_create_io_submission_queue_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	cmd->cmd.cdw10_bits.create_io_q.qid = (g_data[0] << 8) + g_data[1];
	cmd->cmd.cdw10_bits.create_io_q.qsize = (g_data[2] << 8) + g_data[3];

	cmd->cmd.cdw11_bits.create_io_sq.cqid = (g_data[4] << 8) + g_data[5];
	cmd->cmd.cdw11_bits.create_io_sq.qprio = (g_data[6] >> 6) & 0x03;
	cmd->cmd.cdw11_bits.create_io_sq.pc = (g_data[6] >> 5) & 0x01;

	/* NVM Set Identifier */
	cmd->cmd.cdw12 = (g_data[7] << 8) + g_data[8];

	g_data += 9;
}

static void
fuzz_admin_delete_io_completion_queue_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;

	cmd->cmd.cdw10_bits.delete_io_q.qid = (g_data[0] << 8) + g_data[1];

	g_data += 2;
}

static void
fuzz_admin_delete_io_submission_queue_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;

	cmd->cmd.cdw10_bits.delete_io_q.qid = (g_data[0] << 8) + g_data[1];

	g_data += 2;
}

static void
fuzz_admin_namespace_attachment_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_NS_ATTACHMENT;

	cmd->cmd.cdw10_bits.ns_attach.sel = (g_data[0] >> 4) & 0x0f;

	g_data += 1;
}

static void
fuzz_admin_namespace_management_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_NS_MANAGEMENT;

	cmd->cmd.cdw10_bits.ns_manage.sel = (g_data[0] >> 4) & 0x0f;

	g_data += 1;
}

static void
fuzz_admin_security_receive_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_SECURITY_RECEIVE;

	cmd->cmd.cdw10_bits.sec_send_recv.secp = g_data[0];
	cmd->cmd.cdw10_bits.sec_send_recv.spsp1 = g_data[1];
	cmd->cmd.cdw10_bits.sec_send_recv.spsp0 = g_data[2];
	cmd->cmd.cdw10_bits.sec_send_recv.nssf = g_data[3];

	/* Allocation Length(AL) */
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) + (g_data[6] << 8) + g_data[7];

	g_data += 8;
}

static void
fuzz_admin_security_send_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_SECURITY_SEND;

	cmd->cmd.cdw10_bits.sec_send_recv.secp = g_data[0];
	cmd->cmd.cdw10_bits.sec_send_recv.spsp1 = g_data[1];
	cmd->cmd.cdw10_bits.sec_send_recv.spsp0 = g_data[2];
	cmd->cmd.cdw10_bits.sec_send_recv.nssf = g_data[3];

	/* Transfer Length(TL) */
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) + (g_data[6] << 8) + g_data[7];

	g_data += 8;
}

static void
fuzz_admin_directive_send_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_DIRECTIVE_SEND;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];

	cmd->cmd.cdw11_bits.directive.dspec = (g_data[4] << 8) + g_data[5];
	cmd->cmd.cdw11_bits.directive.dtype = g_data[6];
	cmd->cmd.cdw11_bits.directive.doper = g_data[7];

	g_data += 8;
}

static void
fuzz_admin_directive_receive_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_DIRECTIVE_RECEIVE;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];

	cmd->cmd.cdw11_bits.directive.dspec = (g_data[4] << 8) + g_data[5];
	cmd->cmd.cdw11_bits.directive.dtype = g_data[6];
	cmd->cmd.cdw11_bits.directive.doper = g_data[7];

	g_data += 8;
}

static void
fuzz_nvm_read_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_READ;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) +
			 (g_data[6] << 8) + g_data[7];
	cmd->cmd.cdw12 = (g_data[8] << 24) + (g_data[9] << 16) +
			 (g_data[10] << 8) + g_data[11];
	cmd->cmd.cdw13 = g_data[12];
	cmd->cmd.cdw14 = (g_data[13] << 24) + (g_data[14] << 16) +
			 (g_data[15] << 8) + g_data[16];
	cmd->cmd.cdw15 = (g_data[17] << 24) + (g_data[18] << 16) +
			 (g_data[19] << 8) + g_data[20];

	g_data += 21;
}

static void
fuzz_nvm_write_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_WRITE;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) +
			 (g_data[6] << 8) + g_data[7];
	cmd->cmd.cdw12 = (g_data[8] << 24) + (g_data[9] << 16) +
			 (g_data[10] << 8) + g_data[11];
	cmd->cmd.cdw13 = (g_data[12] << 24) + (g_data[13] << 16) +
			 (g_data[14] << 8) + g_data[15];
	cmd->cmd.cdw14 = (g_data[16] << 24) + (g_data[17] << 16) +
			 (g_data[18] << 8) + g_data[19];
	cmd->cmd.cdw15 = (g_data[20] << 24) + (g_data[21] << 16) +
			 (g_data[22] << 8) + g_data[23];

	g_data += 24;
}

static void
fuzz_nvm_write_zeroes_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_WRITE_ZEROES;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) +
			 (g_data[6] << 8) + g_data[7];
	cmd->cmd.cdw12 = (g_data[8] << 24) + (g_data[9] << 16) +
			 (g_data[10] << 8) + g_data[11];
	cmd->cmd.cdw14 = (g_data[12] << 24) + (g_data[13] << 16) +
			 (g_data[14] << 8) + g_data[15];
	cmd->cmd.cdw15 = (g_data[16] << 24) + (g_data[17] << 16) +
			 (g_data[18] << 8) + g_data[19];

	g_data += 20;
}

static void
fuzz_nvm_write_uncorrectable_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_WRITE_UNCORRECTABLE;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) +
			 (g_data[6] << 8) + g_data[7];
	cmd->cmd.cdw12 = (g_data[8] << 8) + g_data[9];

	g_data += 10;
}

static void
fuzz_nvm_reservation_acquire_command(struct fuzz_command *cmd)
{
	struct spdk_nvme_reservation_acquire_data *payload = cmd->buf;
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_RESERVATION_ACQUIRE;

	cmd->cmd.cdw10_bits.resv_acquire.rtype = g_data[0];
	cmd->cmd.cdw10_bits.resv_acquire.iekey = (g_data[1] >> 7) & 0x01;
	cmd->cmd.cdw10_bits.resv_acquire.racqa = (g_data[1] >> 4) & 0x07;

	payload->crkey = ((uint64_t)g_data[2] << 56) + ((uint64_t)g_data[3] << 48) +
			 ((uint64_t)g_data[4] << 40) + ((uint64_t)g_data[5] << 32) +
			 ((uint64_t)g_data[6] << 24) + ((uint64_t)g_data[7] << 16) +
			 ((uint64_t)g_data[8] << 8) + (uint64_t)g_data[9];

	payload->prkey = ((uint64_t)g_data[10] << 56) + ((uint64_t)g_data[11] << 48) +
			 ((uint64_t)g_data[12] << 40) + ((uint64_t)g_data[13] << 32) +
			 ((uint64_t)g_data[14] << 24) + ((uint64_t)g_data[15] << 16) +
			 ((uint64_t)g_data[16] << 8) + (uint64_t)g_data[17];

	cmd->len = sizeof(struct spdk_nvme_reservation_acquire_data);

	g_data += 18;
}

static void
fuzz_nvm_reservation_release_command(struct fuzz_command *cmd)
{
	struct spdk_nvme_reservation_key_data *payload = cmd->buf;
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_RESERVATION_RELEASE;

	cmd->cmd.cdw10_bits.resv_release.rtype = g_data[0];
	cmd->cmd.cdw10_bits.resv_release.iekey = (g_data[1] >> 7) & 0x01;
	cmd->cmd.cdw10_bits.resv_release.rrela = (g_data[1] >> 4) & 0x07;

	payload->crkey = ((uint64_t)g_data[2] << 56) + ((uint64_t)g_data[3] << 48) +
			 ((uint64_t)g_data[4] << 40) + ((uint64_t)g_data[5] << 32) +
			 ((uint64_t)g_data[6] << 24) + ((uint64_t)g_data[7] << 16) +
			 ((uint64_t)g_data[8] << 8) + (uint64_t)g_data[9];

	cmd->len = sizeof(struct spdk_nvme_reservation_key_data);

	g_data += 10;
}

static void
fuzz_nvm_reservation_register_command(struct fuzz_command *cmd)
{
	struct spdk_nvme_reservation_register_data *payload = cmd->buf;
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_RESERVATION_REGISTER;

	cmd->cmd.cdw10_bits.resv_register.cptpl = (g_data[0] >> 6) & 0x03;
	cmd->cmd.cdw10_bits.resv_register.iekey = (g_data[0] >> 5) & 0x01;
	cmd->cmd.cdw10_bits.resv_register.rrega = (g_data[0] >> 2) & 0x07;

	payload->crkey = ((uint64_t)g_data[1] << 56) + ((uint64_t)g_data[2] << 48) +
			 ((uint64_t)g_data[3] << 40) + ((uint64_t)g_data[4] << 32) +
			 ((uint64_t)g_data[5] << 24) + ((uint64_t)g_data[6] << 16) +
			 ((uint64_t)g_data[7] << 8) + (uint64_t)g_data[8];

	payload->nrkey = ((uint64_t)g_data[9] << 56) + ((uint64_t)g_data[10] << 48) +
			 ((uint64_t)g_data[11] << 40) + ((uint64_t)g_data[12] << 32) +
			 ((uint64_t)g_data[13] << 24) + ((uint64_t)g_data[14] << 16) +
			 ((uint64_t)g_data[15] << 8) + (uint64_t)g_data[16];


	cmd->len = sizeof(struct spdk_nvme_reservation_register_data);

	g_data += 17;
}

static void
fuzz_nvm_reservation_report_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_RESERVATION_REPORT;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];

	cmd->cmd.cdw11_bits.resv_report.eds = (g_data[4] >> 7) & 0x01;

	g_data += 5;
}

static void
fuzz_nvm_compare_command(struct fuzz_command *cmd)
{
	memset(&cmd->cmd, 0, sizeof(cmd->cmd));
	cmd->cmd.opc = SPDK_NVME_OPC_COMPARE;

	cmd->cmd.cdw10 = (g_data[0] << 24) + (g_data[1] << 16) +
			 (g_data[2] << 8) + g_data[3];
	cmd->cmd.cdw11 = (g_data[4] << 24) + (g_data[5] << 16) +
			 (g_data[6] << 8) + g_data[7];
	cmd->cmd.cdw12 = (g_data[8] << 24) + (g_data[9] << 16) +
			 (g_data[10] << 8) + g_data[11];
	cmd->cmd.cdw14 = (g_data[12] << 24) + (g_data[13] << 16) +
			 (g_data[14] << 8) + g_data[15];
	cmd->cmd.cdw15 = (g_data[16] << 24) + (g_data[17] << 16) +
			 (g_data[18] << 8) + g_data[19];

	g_data += 20;
}

static struct fuzz_type g_fuzzers[] = {
	{ .fn = fuzz_admin_command, .bytes_per_cmd = sizeof(struct spdk_nvme_cmd), .is_admin = true},
	{ .fn = fuzz_admin_get_log_page_command, .bytes_per_cmd = 6, .is_admin = true},
	{ .fn = fuzz_admin_identify_command, .bytes_per_cmd = 7, .is_admin = true},
	{ .fn = fuzz_admin_abort_command, .bytes_per_cmd = 4, .is_admin = true},
	{ .fn = fuzz_admin_create_io_completion_queue_command, .bytes_per_cmd = 7, .is_admin = true},
	{ .fn = fuzz_admin_create_io_submission_queue_command, .bytes_per_cmd = 9, .is_admin = true},
	{ .fn = fuzz_admin_delete_io_completion_queue_command, .bytes_per_cmd = 2, .is_admin = true},
	{ .fn = fuzz_admin_delete_io_submission_queue_command, .bytes_per_cmd = 2, .is_admin = true},
	{ .fn = fuzz_admin_namespace_attachment_command, .bytes_per_cmd = 1, .is_admin = true},
	{ .fn = fuzz_admin_namespace_management_command, .bytes_per_cmd = 1, .is_admin = true},
	{ .fn = fuzz_admin_security_receive_command, .bytes_per_cmd = 8, .is_admin = true},
	{ .fn = fuzz_admin_security_send_command, .bytes_per_cmd = 8, .is_admin = true},
	{ .fn = fuzz_admin_directive_send_command, .bytes_per_cmd = 8, .is_admin = true},
	{ .fn = fuzz_admin_directive_receive_command, .bytes_per_cmd = 8, .is_admin = true},
	{ .fn = fuzz_nvm_read_command, .bytes_per_cmd = 21, .is_admin = false},
	{ .fn = fuzz_nvm_write_command, .bytes_per_cmd = 24, .is_admin = false},
	{ .fn = fuzz_nvm_write_zeroes_command, .bytes_per_cmd = 20, .is_admin = false},
	{ .fn = fuzz_nvm_write_uncorrectable_command, .bytes_per_cmd = 10, .is_admin = false},
	{ .fn = fuzz_nvm_reservation_acquire_command, .bytes_per_cmd = 18, .is_admin = false},
	{ .fn = fuzz_nvm_reservation_release_command, .bytes_per_cmd = 10, .is_admin = false},
	{ .fn = fuzz_nvm_reservation_register_command, .bytes_per_cmd = 17, .is_admin = false},
	{ .fn = fuzz_nvm_reservation_report_command, .bytes_per_cmd = 5, .is_admin = false},
	{ .fn = fuzz_nvm_compare_command, .bytes_per_cmd = 20, .is_admin = false},
	{ .fn = NULL, .bytes_per_cmd = 0, .is_admin = 0}
};

#define NUM_FUZZERS (SPDK_COUNTOF(g_fuzzers) - 1)

static struct fuzz_type *g_fuzzer;

struct spdk_nvme_transport_id g_trid;
static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_qpair *g_io_qpair;
static void
nvme_fuzz_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	int *outstanding = cb_arg;

	assert(*outstanding > 0);
	(*outstanding)--;
}

static int
run_cmds(uint32_t queue_depth)
{
	int rc, outstanding = 0;
	uint32_t i;

	for (i = 0; i < queue_depth; i++) {
		struct fuzz_command *cmd = &g_cmds[i];

		g_fuzzer->fn(cmd);
		outstanding++;
		if (g_fuzzer->is_admin) {
			rc = spdk_nvme_ctrlr_cmd_admin_raw(g_ctrlr, &cmd->cmd, cmd->buf, cmd->len, nvme_fuzz_cpl_cb,
							   &outstanding);
		} else {
			rc = spdk_nvme_ctrlr_cmd_io_raw(g_ctrlr, g_io_qpair, &cmd->cmd, cmd->buf, cmd->len,
							nvme_fuzz_cpl_cb, &outstanding);
		}
		if (rc) {
			return rc;
		}
	}

	while (outstanding > 0) {
		spdk_nvme_qpair_process_completions(g_io_qpair, 0);
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}
	return 0;
}

static int TestOneInput(const uint8_t *data, size_t size)
{
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	g_ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed for transport address '%s'\n",
			g_trid.traddr);
		spdk_app_stop(-1);
	}

	g_io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (g_io_qpair == NULL) {
		fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair failed\n");
		spdk_app_stop(-1);
	}

	g_data = data;

	run_cmds(size / g_fuzzer->bytes_per_cmd);
	spdk_nvme_ctrlr_free_io_qpair(g_io_qpair);
	spdk_nvme_detach_async(g_ctrlr, &detach_ctx);

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	return 0;
}

int LLVMFuzzerRunDriver(int *argc, char ***argv, int (*UserCb)(const uint8_t *Data, size_t Size));

static void exit_handler(void)
{
	if (g_in_fuzzer) {
		spdk_app_stop(0);
		pthread_join(g_reactor_td, NULL);
	}
}

static void *
start_fuzzer(void *ctx)
{
	char *_argv[] = {
		"spdk",
		"-len_control=0",
		"-detect_leaks=1",
		NULL,
		NULL,
		NULL
	};
	char time_str[128];
	char len_str[128];
	char **argv = _argv;
	int argc = SPDK_COUNTOF(_argv);
	uint32_t len;

	spdk_unaffinitize_thread();
	len = MAX_COMMANDS * g_fuzzer->bytes_per_cmd;
	snprintf(len_str, sizeof(len_str), "-max_len=%d", len);
	argv[argc - 3] = len_str;
	snprintf(time_str, sizeof(time_str), "-max_total_time=%d", g_time_in_sec);
	argv[argc - 2] = time_str;
	argv[argc - 1] = g_corpus_dir;

	g_in_fuzzer = true;
	atexit(exit_handler);
	if (g_repro_data) {
		printf("Running single test based on reproduction data file.\n");
		TestOneInput(g_repro_data, g_repro_size);
		printf("Done.\n");
	} else {
		LLVMFuzzerRunDriver(&argc, &argv, TestOneInput);
		/* TODO: in the normal case, LLVMFuzzerRunDriver never returns - it calls exit()
		 * directly and we never get here.  But this behavior isn't really documented
		 * anywhere by LLVM, so call spdk_app_stop(0) if it does return, which will
		 * result in the app exiting like a normal SPDK application (spdk_app_start()
		 * returns to main().
		 */
	}
	g_in_fuzzer = false;
	spdk_app_stop(0);

	return NULL;
}

static void
begin_fuzz(void *ctx)
{
	int i;

	g_reactor_td = pthread_self();

	for (i = 0; i < MAX_COMMANDS; i++) {
		g_cmds[i].buf = spdk_malloc(4096, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		assert(g_cmds[i].buf);
		g_cmds[i].len = 4096;
	}

	pthread_create(&g_fuzz_td, NULL, start_fuzzer, NULL);
}

static void
nvme_fuzz_usage(void)
{
	fprintf(stderr, " -D                        Path of corpus directory.\n");
	fprintf(stderr, " -F                        Transport ID for subsystem that should be fuzzed.\n");
	fprintf(stderr, " -N                        Name of reproduction data file.\n");
	fprintf(stderr, " -t                        Time to run fuzz tests (in seconds). Default: 10\n");
	fprintf(stderr, " -Z                        Fuzzer to run (0 to %lu)\n", NUM_FUZZERS - 1);
}

static int
nvme_fuzz_parse(int ch, char *arg)
{
	long long tmp;
	int rc;
	FILE *repro_file;

	switch (ch) {
	case 'D':
		g_corpus_dir = strdup(optarg);
		break;
	case 'F':
		if (g_trid_specified) {
			fprintf(stderr, "Can only specify one trid\n");
			return -1;
		}
		g_trid_specified = true;
		rc = spdk_nvme_transport_id_parse(&g_trid, optarg);
		if (rc < 0) {
			fprintf(stderr, "failed to parse transport ID: %s\n", optarg);
			return -1;
		}
		break;
	case 'N':
		repro_file = fopen(optarg, "r");
		if (repro_file == NULL) {
			fprintf(stderr, "could not open %s: %s\n", optarg, spdk_strerror(errno));
			return -1;
		}
		g_repro_data = spdk_posix_file_load(repro_file, &g_repro_size);
		if (g_repro_data == NULL) {
			fprintf(stderr, "could not load data for file %s\n", optarg);
			return -1;
		}
		break;
	case 't':
	case 'Z':
		tmp = spdk_strtoll(optarg, 10);
		if (tmp < 0 || tmp >= INT_MAX) {
			fprintf(stderr, "Invalid value '%s' for option -%c.\n", optarg, ch);
			return -EINVAL;
		}
		switch (ch) {
		case 't':
			g_time_in_sec = tmp;
			break;
		case 'Z':
			if ((unsigned long)tmp >= NUM_FUZZERS) {
				fprintf(stderr, "Invalid fuzz type %lld (max %lu)\n", tmp, NUM_FUZZERS - 1);
				return -EINVAL;
			}
			g_fuzzer = &g_fuzzers[tmp];
			break;
		}
		break;
	case '?':
	default:
		return -EINVAL;
	}
	return 0;
}

static void
fuzz_shutdown(void)
{
	/* If the user terminates the fuzzer prematurely, it is likely due
	 * to an input hang.  So raise a SIGSEGV signal which will cause the
	 * fuzzer to generate a crash file for the last input.
	 *
	 * Note that the fuzzer will always generate a crash file, even if
	 * we get our TestOneInput() function (which is called by the fuzzer)
	 * to pthread_exit().  So just doing the SIGSEGV here in all cases is
	 * simpler than trying to differentiate between hung inputs and
	 * an impatient user.
	 */
	pthread_kill(g_fuzz_td, SIGSEGV);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "nvme_fuzz";
	opts.shutdown_cb = fuzz_shutdown;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "D:F:N:t:Z:", NULL, nvme_fuzz_parse,
				      nvme_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (!g_corpus_dir) {
		fprintf(stderr, "Must specify corpus dir with -D option\n");
		return -1;
	}

	if (!g_trid_specified) {
		fprintf(stderr, "Must specify trid with -F option\n");
		return -1;
	}

	if (!g_fuzzer) {
		fprintf(stderr, "Must specify fuzzer with -Z option\n");
		return -1;
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	spdk_app_fini();
	return rc;
}
