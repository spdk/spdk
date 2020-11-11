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

#include "spdk_internal/mock.h"

#include "nvmf/ctrlr_bdev.c"


SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), -1);

DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");

DEFINE_STUB(spdk_bdev_get_acwu, uint16_t, (const struct spdk_bdev *bdev), 0);

DEFINE_STUB(spdk_bdev_get_data_block_size, uint32_t,
	    (const struct spdk_bdev *bdev), 512);

DEFINE_STUB(nvmf_ctrlr_process_admin_cmd, int, (struct spdk_nvmf_request *req), 0);

DEFINE_STUB(spdk_bdev_comparev_blocks, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);

DEFINE_STUB(spdk_bdev_nvme_admin_passthru, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);

DEFINE_STUB(spdk_bdev_abort, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     void *bio_cb_arg, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);

struct spdk_bdev {
	uint32_t blocklen;
	uint64_t num_blocks;
	uint32_t md_len;
};

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->num_blocks;
}

uint32_t
spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev)
{
	abort();
	return 0;
}

uint32_t
spdk_bdev_get_md_size(const struct spdk_bdev *bdev)
{
	return bdev->md_len;
}

DEFINE_STUB(spdk_bdev_comparev_and_writev_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct iovec *compare_iov, int compare_iovcnt,
	     struct iovec *write_iov, int write_iovcnt,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(nvmf_ctrlr_process_io_cmd, int, (struct spdk_nvmf_request *req), 0);

DEFINE_STUB_V(spdk_bdev_io_get_nvme_fused_status, (const struct spdk_bdev_io *bdev_io,
		uint32_t *cdw0, int *cmp_sct, int *cmp_sc, int *wr_sct, int *wr_sc));

DEFINE_STUB(spdk_bdev_is_md_interleaved, bool, (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type,
	    (const struct spdk_bdev *bdev), SPDK_DIF_DISABLE);

DEFINE_STUB(spdk_bdev_is_dif_head_of_md, bool, (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_is_dif_check_enabled, bool,
	    (const struct spdk_bdev *bdev, enum spdk_dif_check_type check_type), false);

DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *,
	    (struct spdk_bdev_desc *desc), NULL);

DEFINE_STUB(spdk_bdev_flush_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_unmap_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_io_type_supported, bool,
	    (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), false);

DEFINE_STUB(spdk_bdev_queue_io_wait, int,
	    (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
	     struct spdk_bdev_io_wait_entry *entry),
	    0);

DEFINE_STUB(spdk_bdev_write_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_writev_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_read_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_readv_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB(spdk_bdev_nvme_io_passthru, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     const struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes,
	     spdk_bdev_io_completion_cb cb, void *cb_arg),
	    0);

DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));

DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem), NULL);

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	abort();
	return NULL;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns)
{
	abort();
	return NULL;
}

DEFINE_STUB_V(spdk_bdev_io_get_nvme_status,
	      (const struct spdk_bdev_io *bdev_io, uint32_t *cdw0, int *sct, int *sc));

int
spdk_dif_ctx_init(struct spdk_dif_ctx *ctx, uint32_t block_size, uint32_t md_size,
		  bool md_interleave, bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		  uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag,
		  uint32_t data_offset, uint16_t guard_seed)
{
	ctx->block_size = block_size;
	ctx->md_size = md_size;
	ctx->init_ref_tag = init_ref_tag;

	return 0;
}

static void
test_get_rw_params(void)
{
	struct spdk_nvme_cmd cmd = {0};
	uint64_t lba;
	uint64_t count;

	lba = 0;
	count = 0;
	to_le64(&cmd.cdw10, 0x1234567890ABCDEF);
	to_le32(&cmd.cdw12, 0x9875 | SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
	nvmf_bdev_ctrlr_get_rw_params(&cmd, &lba, &count);
	CU_ASSERT(lba == 0x1234567890ABCDEF);
	CU_ASSERT(count == 0x9875 + 1); /* NOTE: this field is 0's based, hence the +1 */
}

static void
test_lba_in_range(void)
{
	/* Trivial cases (no overflow) */
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1000) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 0, 1001) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1, 999) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1, 1000) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 999, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1000, 1) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(1000, 1001, 1) == false);

	/* Overflow edge cases */
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, 0, UINT64_MAX) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, 1, UINT64_MAX) == false);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, UINT64_MAX - 1, 1) == true);
	CU_ASSERT(nvmf_bdev_ctrlr_lba_in_range(UINT64_MAX, UINT64_MAX, 1) == false);
}

static void
test_get_dif_ctx(void)
{
	struct spdk_bdev bdev = {};
	struct spdk_nvme_cmd cmd = {};
	struct spdk_dif_ctx dif_ctx = {};
	bool ret;

	bdev.md_len = 0;

	ret = nvmf_bdev_ctrlr_get_dif_ctx(&bdev, &cmd, &dif_ctx);
	CU_ASSERT(ret == false);

	to_le64(&cmd.cdw10, 0x1234567890ABCDEF);
	bdev.blocklen = 520;
	bdev.md_len = 8;

	ret = nvmf_bdev_ctrlr_get_dif_ctx(&bdev, &cmd, &dif_ctx);
	CU_ASSERT(ret == true);
	CU_ASSERT(dif_ctx.block_size = 520);
	CU_ASSERT(dif_ctx.md_size == 8);
	CU_ASSERT(dif_ctx.init_ref_tag == 0x90ABCDEF);
}

static void
test_spdk_nvmf_bdev_ctrlr_compare_and_write_cmd(void)
{
	int rc;
	struct spdk_bdev bdev = {};
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel ch = {};

	struct spdk_nvmf_request cmp_req = {};
	union nvmf_c2h_msg cmp_rsp = {};

	struct spdk_nvmf_request write_req = {};
	union nvmf_c2h_msg write_rsp = {};

	struct spdk_nvmf_qpair qpair = {};

	struct spdk_nvme_cmd cmp_cmd = {};
	struct spdk_nvme_cmd write_cmd = {};

	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};

	bdev.blocklen = 512;
	bdev.num_blocks = 10;
	ns.bdev = &bdev;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	/* Enable controller */
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.subsys = &subsystem;

	group.num_sgroups = 1;
	sgroups.num_ns = 1;
	sgroups.ns_info = &ns_info;
	group.sgroups = &sgroups;

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;

	cmp_req.qpair = &qpair;
	cmp_req.cmd = (union nvmf_h2c_msg *)&cmp_cmd;
	cmp_req.rsp = &cmp_rsp;

	cmp_cmd.nsid = 1;
	cmp_cmd.fuse = SPDK_NVME_CMD_FUSE_FIRST;
	cmp_cmd.opc = SPDK_NVME_OPC_COMPARE;

	write_req.qpair = &qpair;
	write_req.cmd = (union nvmf_h2c_msg *)&write_cmd;
	write_req.rsp = &write_rsp;

	write_cmd.nsid = 1;
	write_cmd.fuse = SPDK_NVME_CMD_FUSE_SECOND;
	write_cmd.opc = SPDK_NVME_OPC_WRITE;

	/* 1. SUCCESS */
	cmp_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	cmp_cmd.cdw12 = 1;	/* NLB: CDW12 bits 15:00, 0's based */

	write_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	write_cmd.cdw12 = 1;	/* NLB: CDW12 bits 15:00, 0's based */
	write_req.length = (write_cmd.cdw12 + 1) * bdev.blocklen;

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(&bdev, desc, &ch, &cmp_req, &write_req);

	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sct == 0);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sc == 0);
	CU_ASSERT(write_rsp.nvme_cpl.status.sct == 0);
	CU_ASSERT(write_rsp.nvme_cpl.status.sc == 0);

	/* 2. Fused command start lba / num blocks mismatch */
	cmp_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	cmp_cmd.cdw12 = 2;	/* NLB: CDW12 bits 15:00, 0's based */

	write_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	write_cmd.cdw12 = 1;	/* NLB: CDW12 bits 15:00, 0's based */
	write_req.length = (write_cmd.cdw12 + 1) * bdev.blocklen;

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(&bdev, desc, &ch, &cmp_req, &write_req);

	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sct == 0);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sc == 0);
	CU_ASSERT(write_rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(write_rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* 3. SPDK_NVME_SC_LBA_OUT_OF_RANGE */
	cmp_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	cmp_cmd.cdw12 = 100;	/* NLB: CDW12 bits 15:00, 0's based */

	write_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	write_cmd.cdw12 = 100;	/* NLB: CDW12 bits 15:00, 0's based */
	write_req.length = (write_cmd.cdw12 + 1) * bdev.blocklen;

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(&bdev, desc, &ch, &cmp_req, &write_req);

	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sct == 0);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sc == 0);
	CU_ASSERT(write_rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(write_rsp.nvme_cpl.status.sc == SPDK_NVME_SC_LBA_OUT_OF_RANGE);

	/* 4. SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID */
	cmp_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	cmp_cmd.cdw12 = 1;	/* NLB: CDW12 bits 15:00, 0's based */

	write_cmd.cdw10 = 1;	/* SLBA: CDW10 and CDW11 */
	write_cmd.cdw12 = 1;	/* NLB: CDW12 bits 15:00, 0's based */
	write_req.length = (write_cmd.cdw12 + 1) * bdev.blocklen - 1;

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(&bdev, desc, &ch, &cmp_req, &write_req);

	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sct == 0);
	CU_ASSERT(cmp_rsp.nvme_cpl.status.sc == 0);
	CU_ASSERT(write_rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(write_rsp.nvme_cpl.status.sc == SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_get_rw_params);
	CU_ADD_TEST(suite, test_lba_in_range);
	CU_ADD_TEST(suite, test_get_dif_ctx);

	CU_ADD_TEST(suite, test_spdk_nvmf_bdev_ctrlr_compare_and_write_cmd);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
