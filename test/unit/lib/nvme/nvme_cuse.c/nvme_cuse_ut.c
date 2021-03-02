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
#include "nvme/nvme_cuse.c"
#include "common/lib/nvme/common_stubs.h"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(spdk_nvme_ctrlr_alloc_cmb_io_buffer, void *,
	    (struct spdk_nvme_ctrlr *ctrlr, size_t size), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_admin_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, void *buf, uint32_t len,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_num_ns, uint32_t,
	    (struct spdk_nvme_ctrlr *ctrlr), 128);

DEFINE_STUB(spdk_nvme_ctrlr_reset, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_read, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	     void *payload, uint64_t lba, uint32_t lba_count,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_write, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	     void *payload, uint64_t lba, uint32_t lba_count,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags), 0);

DEFINE_STUB(spdk_nvme_ns_get_num_sectors, uint64_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_sector_size, uint32_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB_V(spdk_unaffinitize_thread, (void));

DEFINE_STUB(spdk_nvme_ctrlr_get_ns, struct spdk_nvme_ns *,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid), NULL);

DEFINE_STUB(nvme_io_msg_ctrlr_register, int,
	    (struct spdk_nvme_ctrlr *ctrlr,
	     struct nvme_io_msg_producer *io_msg_producer), 0);

DEFINE_STUB_V(nvme_io_msg_ctrlr_unregister,
	      (struct spdk_nvme_ctrlr *ctrlr,
	       struct nvme_io_msg_producer *io_msg_producer));

DEFINE_STUB(spdk_nvme_ctrlr_is_active_ns, bool,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid), true);

DEFINE_STUB(fuse_reply_err, int, (fuse_req_t req, int err), 0);

struct cuse_io_ctx *g_ut_ctx;

DEFINE_RETURN_MOCK(nvme_io_msg_send, int);
int
nvme_io_msg_send(struct spdk_nvme_ctrlr *ctrlr,
		 uint32_t nsid, spdk_nvme_io_msg_fn fn, void *arg)
{
	g_ut_ctx = arg;

	HANDLE_RETURN_MOCK(nvme_io_msg_send);
	return 0;
}

static void
test_cuse_nvme_submit_io_read_write(void)
{
	struct cuse_device cuse_device = {};
	struct fuse_file_info fi = {};
	struct nvme_user_io *user_io = NULL;
	char arg[1024] = {};
	fuse_req_t req = (void *)0xDEEACDFF;
	unsigned flags = FUSE_IOCTL_DIR;
	uint32_t block_size = 4096;
	size_t in_bufsz = 4096;
	size_t out_bufsz = 4096;

	/* Allocate memory to avoid stack buffer overflow */
	user_io = calloc(3, 4096);
	SPDK_CU_ASSERT_FATAL(user_io != NULL);
	cuse_device.ctrlr = (void *)0xDEADBEEF;
	cuse_device.nsid = 1;
	user_io->slba = 1024;
	user_io->nblocks = 1;
	g_ut_ctx = NULL;

	/* Submit IO read */
	cuse_nvme_submit_io_read(&cuse_device, req, 0, arg, &fi, flags,
				 block_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	cuse_io_ctx_free(g_ut_ctx);

	/* Submit IO write */
	g_ut_ctx = NULL;

	cuse_nvme_submit_io_write(&cuse_device, req, 0, arg, &fi, flags,
				  block_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	cuse_io_ctx_free(g_ut_ctx);
	free(user_io);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_cuse", NULL, NULL);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_io_read_write);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
