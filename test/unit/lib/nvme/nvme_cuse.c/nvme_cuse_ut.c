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

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw_with_md, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf, uint32_t len, void *md_buf,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_reset, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_reset_subsystem, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_read_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *payload, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_cmd_write_with_md, int, (struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *payload, void *metadata,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_ns_get_num_sectors, uint64_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_md_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB_V(spdk_unaffinitize_thread, (void));

DEFINE_STUB(nvme_io_msg_ctrlr_register, int,
	    (struct spdk_nvme_ctrlr *ctrlr,
	     struct nvme_io_msg_producer *io_msg_producer), 0);

DEFINE_STUB_V(nvme_io_msg_ctrlr_unregister,
	      (struct spdk_nvme_ctrlr *ctrlr,
	       struct nvme_io_msg_producer *io_msg_producer));

DEFINE_STUB(spdk_nvme_ctrlr_is_active_ns, bool,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid), true);

DEFINE_STUB(fuse_reply_err, int, (fuse_req_t req, int err), 0);
DEFINE_STUB_V(fuse_session_exit, (struct fuse_session *se));
DEFINE_STUB(pthread_join, int, (pthread_t tid, void **val), 0);

DEFINE_STUB_V(nvme_ctrlr_update_namespaces, (struct spdk_nvme_ctrlr *ctrlr));

static int
nvme_ns_cmp(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	return ns1->id - ns2->id;
}

RB_GENERATE_STATIC(nvme_ns_tree, spdk_nvme_ns, node, nvme_ns_cmp);

struct cuse_io_ctx *g_ut_ctx;
struct spdk_nvme_ctrlr *g_ut_ctrlr;
uint32_t g_ut_nsid;

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata.nn;
}

uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return 1;
}

uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	if (nsid > ctrlr->cdata.nn) {
		return 0;
	}

	return nsid + 1;
}

DEFINE_RETURN_MOCK(nvme_io_msg_send, int);
int
nvme_io_msg_send(struct spdk_nvme_ctrlr *ctrlr,
		 uint32_t nsid, spdk_nvme_io_msg_fn fn, void *arg)
{
	g_ut_ctx = arg;
	g_ut_nsid = nsid;
	g_ut_ctrlr = ctrlr;

	HANDLE_RETURN_MOCK(nvme_io_msg_send);
	return 0;
}

uint32_t
spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns)
{
	return ns->sector_size;
}

static struct spdk_nvme_ns g_inactive_ns = {};

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns tmp;
	struct spdk_nvme_ns *ns;

	if (nsid < 1 || nsid > ctrlr->cdata.nn) {
		return NULL;
	}

	tmp.id = nsid;
	ns = RB_FIND(nvme_ns_tree, &ctrlr->ns, &tmp);

	if (ns == NULL) {
		return &g_inactive_ns;
	}

	return ns;
}

struct cuse_device *g_cuse_device;
DEFINE_RETURN_MOCK(fuse_req_userdata, void *);
void *
fuse_req_userdata(fuse_req_t req)
{
	return g_cuse_device;
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
	uint32_t md_size = 0;
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
				 block_size, md_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len == 0);
	CU_ASSERT(g_ut_ctx->metadata == NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0);
	CU_ASSERT(g_ut_ctx->apptag == 0);
	cuse_io_ctx_free(g_ut_ctx);

	/* Submit IO write */
	g_ut_ctx = NULL;

	cuse_nvme_submit_io_write(&cuse_device, req, 0, arg, &fi, flags,
				  block_size, md_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len == 0);
	CU_ASSERT(g_ut_ctx->metadata == NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0);
	CU_ASSERT(g_ut_ctx->apptag == 0);
	cuse_io_ctx_free(g_ut_ctx);
	free(user_io);
}

static void
test_cuse_nvme_submit_io_read_write_with_md(void)
{
	struct cuse_device cuse_device = {};
	struct fuse_file_info fi = {};
	struct nvme_user_io *user_io = NULL;
	char arg[1024] = {};
	fuse_req_t req = (void *)0xDEEACDFF;
	unsigned flags = FUSE_IOCTL_DIR;
	uint32_t block_size = 4096;
	uint32_t md_size = 8;
	size_t in_bufsz = 4096;
	size_t out_bufsz = 4096;

	/* Allocate memory to avoid stack buffer overflow */
	user_io = calloc(4, 4096);
	SPDK_CU_ASSERT_FATAL(user_io != NULL);
	cuse_device.ctrlr = (void *)0xDEADBEEF;
	cuse_device.nsid = 1;
	user_io->slba = 1024;
	user_io->nblocks = 1;
	user_io->appmask = 0xF00D;
	user_io->apptag = 0xC0DE;
	user_io->metadata = 0xDEADDEAD;
	g_ut_ctx = NULL;

	/* Submit IO read */
	cuse_nvme_submit_io_read(&cuse_device, req, 0, arg, &fi, flags,
				 block_size, md_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len ==
		  (int)((user_io->nblocks + 1) * md_size));
	CU_ASSERT(g_ut_ctx->metadata != NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0xF00D);
	CU_ASSERT(g_ut_ctx->apptag == 0xC0DE);
	cuse_io_ctx_free(g_ut_ctx);

	/* Submit IO write */
	g_ut_ctx = NULL;

	cuse_nvme_submit_io_write(&cuse_device, req, 0, arg, &fi, flags,
				  block_size, md_size, user_io, in_bufsz, out_bufsz);
	CU_ASSERT(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = user_io->slba);
	CU_ASSERT(g_ut_ctx->lba_count == (uint32_t)(user_io->nblocks + 1));
	CU_ASSERT(g_ut_ctx->data_len ==
		  (int)((user_io->nblocks + 1) * block_size));
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len ==
		  (int)((user_io->nblocks + 1) * md_size));
	CU_ASSERT(g_ut_ctx->metadata != NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0xF00D);
	CU_ASSERT(g_ut_ctx->apptag == 0xC0DE);
	cuse_io_ctx_free(g_ut_ctx);
	free(user_io);
}

static void
test_cuse_nvme_submit_passthru_cmd(void)
{
	struct nvme_passthru_cmd *passthru_cmd = NULL;
	fuse_req_t req = (void *)0xDEEACDFF;

	passthru_cmd = calloc(1, sizeof(struct nvme_passthru_cmd));
	g_cuse_device = calloc(1, sizeof(struct cuse_device));

	/* Use fatal or we'll segfault if we didn't get memory */
	SPDK_CU_ASSERT_FATAL(passthru_cmd != NULL);
	SPDK_CU_ASSERT_FATAL(g_cuse_device != NULL);
	g_cuse_device->ctrlr = (void *)0xDEADBEEF;

	g_ut_ctx = NULL;
	/* Passthrough command */
	passthru_cmd->opcode       = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
	passthru_cmd->nsid         = 1;
	passthru_cmd->data_len     = 512;
	passthru_cmd->metadata_len = 0;
	passthru_cmd->cdw10        = 0xc0de1010;
	passthru_cmd->cdw11        = 0xc0de1111;
	passthru_cmd->cdw12        = 0xc0de1212;
	passthru_cmd->cdw13        = 0xc0de1313;
	passthru_cmd->cdw14        = 0xc0de1414;
	passthru_cmd->cdw15        = 0xc0de1515;

	/* Send IO Command IOCTL */
	cuse_nvme_passthru_cmd_send(req, passthru_cmd, NULL, NULL, NVME_IOCTL_IO_CMD);
	SPDK_CU_ASSERT_FATAL(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata == NULL);
	CU_ASSERT(g_ut_ctx->req               == req);
	CU_ASSERT(g_ut_ctx->data_len          == 512);
	CU_ASSERT(g_ut_ctx->metadata_len      == 0);
	CU_ASSERT(g_ut_ctx->nvme_cmd.opc      == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
	CU_ASSERT(g_ut_ctx->nvme_cmd.nsid     == 1);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw10    == 0xc0de1010);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw11    == 0xc0de1111);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw12    == 0xc0de1212);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw13    == 0xc0de1313);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw14    == 0xc0de1414);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw15    == 0xc0de1515);

	cuse_io_ctx_free(g_ut_ctx);
	free(passthru_cmd);
	free(g_cuse_device);
}

static void
test_cuse_nvme_submit_passthru_cmd_with_md(void)
{
	struct nvme_passthru_cmd *passthru_cmd = NULL;
	fuse_req_t req = (void *)0xDEEACDFF;

	passthru_cmd = calloc(1, sizeof(struct nvme_passthru_cmd));
	g_cuse_device = calloc(1, sizeof(struct cuse_device));

	/* Use fatal or we'll segfault if we didn't get memory */
	SPDK_CU_ASSERT_FATAL(passthru_cmd != NULL);
	SPDK_CU_ASSERT_FATAL(g_cuse_device != NULL);
	g_cuse_device->ctrlr = (void *)0xDEADBEEF;

	g_ut_ctx = NULL;
	/* Passthrough command */
	passthru_cmd->opcode       = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
	passthru_cmd->nsid         = 1;
	passthru_cmd->data_len     = 512;
	passthru_cmd->metadata_len = 8;
	passthru_cmd->cdw10        = 0xc0de1010;
	passthru_cmd->cdw11        = 0xc0de1111;
	passthru_cmd->cdw12        = 0xc0de1212;
	passthru_cmd->cdw13        = 0xc0de1313;
	passthru_cmd->cdw14        = 0xc0de1414;
	passthru_cmd->cdw15        = 0xc0de1515;

	/* Send IO Command IOCTL */
	cuse_nvme_passthru_cmd_send(req, passthru_cmd, NULL, NULL, NVME_IOCTL_IO_CMD);
	SPDK_CU_ASSERT_FATAL(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata != NULL);
	CU_ASSERT(g_ut_ctx->req               == req);
	CU_ASSERT(g_ut_ctx->data_len          == 512);
	CU_ASSERT(g_ut_ctx->metadata_len      == 8);
	CU_ASSERT(g_ut_ctx->nvme_cmd.opc      == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
	CU_ASSERT(g_ut_ctx->nvme_cmd.nsid     == 1);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw10    == 0xc0de1010);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw11    == 0xc0de1111);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw12    == 0xc0de1212);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw13    == 0xc0de1313);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw14    == 0xc0de1414);
	CU_ASSERT(g_ut_ctx->nvme_cmd.cdw15    == 0xc0de1515);

	cuse_io_ctx_free(g_ut_ctx);
	free(passthru_cmd);
	free(g_cuse_device);
}

static void
test_nvme_cuse_get_cuse_ns_device(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct cuse_device ctrlr_device = {};
	struct cuse_device ns_device = { .nsid = 1 };
	struct cuse_device *cuse_dev = NULL;

	ctrlr.cdata.nn = 3;
	ctrlr_device.ctrlr = &ctrlr;
	TAILQ_INIT(&ctrlr_device.ns_devices);
	TAILQ_INSERT_TAIL(&ctrlr_device.ns_devices, &ns_device, tailq);

	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_ctrlr_ctx_head));
	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, &ctrlr_device, tailq);

	cuse_dev = nvme_cuse_get_cuse_ns_device(&ctrlr, 1);
	CU_ASSERT(cuse_dev == &ns_device);

	/* nsid 2 was not started */
	cuse_dev = nvme_cuse_get_cuse_ns_device(&ctrlr, 2);
	CU_ASSERT(cuse_dev == NULL);

	/* nsid invalid */
	cuse_dev = nvme_cuse_get_cuse_ns_device(&ctrlr, 0);
	CU_ASSERT(cuse_dev == NULL);

	TAILQ_REMOVE(&g_ctrlr_ctx_head, &ctrlr_device, tailq);
}

static void
test_cuse_nvme_submit_io(void)
{
	struct cuse_device cuse_device = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct fuse_file_info fi = {};
	struct spdk_nvme_ns ns = {};
	struct nvme_user_io *user_io = NULL;
	char arg[1024] = {};
	fuse_req_t req = (void *)0xDEEACDFF;

	/* Allocate memory to avoid stack buffer overflow */
	user_io = calloc(3, 4096);
	SPDK_CU_ASSERT_FATAL(user_io != NULL);

	RB_INIT(&ctrlr.ns);
	ns.id = 1;
	RB_INSERT(nvme_ns_tree, &ctrlr.ns, &ns);

	cuse_device.ctrlr = &ctrlr;
	ctrlr.cdata.nn = 1;
	ns.sector_size = 4096;
	ns.id = 1;
	user_io->slba = 1024;
	user_io->nblocks = 1;
	cuse_device.nsid = 1;
	g_cuse_device = &cuse_device;

	/* Read */
	user_io->opcode = SPDK_NVME_OPC_READ;
	g_ut_ctx = NULL;

	cuse_nvme_submit_io(req, 0, arg, &fi, FUSE_IOCTL_DIR, user_io, 4096, 4096);
	SPDK_CU_ASSERT_FATAL(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_nsid == 1);
	CU_ASSERT(g_ut_ctx->req == (void *)0xDEEACDFF);
	CU_ASSERT(g_ut_ctx->lba = 1024);
	CU_ASSERT(g_ut_ctx->lba_count == 2);
	CU_ASSERT(g_ut_ctx->data_len == 2 * 4096);
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len == 0);
	CU_ASSERT(g_ut_ctx->metadata == NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0);
	CU_ASSERT(g_ut_ctx->apptag == 0);

	cuse_io_ctx_free(g_ut_ctx);

	/* Write */
	user_io->opcode = SPDK_NVME_OPC_WRITE;
	g_ut_ctx = NULL;

	cuse_nvme_submit_io(req, 0, arg, &fi, FUSE_IOCTL_DIR, user_io, 4096, 4096);
	SPDK_CU_ASSERT_FATAL(g_ut_ctx != NULL);
	CU_ASSERT(g_ut_nsid == 1);
	CU_ASSERT(g_ut_ctx->req == req);
	CU_ASSERT(g_ut_ctx->lba = 1024);
	CU_ASSERT(g_ut_ctx->lba_count == 2);
	CU_ASSERT(g_ut_ctx->data_len == 2 * 4096);
	CU_ASSERT(g_ut_ctx->data != NULL);
	CU_ASSERT(g_ut_ctx->metadata_len == 0);
	CU_ASSERT(g_ut_ctx->metadata == NULL);
	CU_ASSERT(g_ut_ctx->appmask == 0);
	CU_ASSERT(g_ut_ctx->apptag == 0);
	cuse_io_ctx_free(g_ut_ctx);

	/* Invalid */
	g_ut_ctx = NULL;
	user_io->opcode = SPDK_NVME_OPC_FLUSH;

	cuse_nvme_submit_io(req, 0, arg, &fi, FUSE_IOCTL_DIR, user_io, 4096, 4096);
	SPDK_CU_ASSERT_FATAL(g_ut_ctx == NULL);

	free(user_io);
}

static void
test_cuse_nvme_reset(void)
{
	struct cuse_device cuse_device = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	fuse_req_t req = (void *)0xDEADBEEF;

	cuse_device.ctrlr = &ctrlr;
	g_cuse_device = &cuse_device;

	/* Invalid nsid  */
	cuse_device.nsid = 1;
	g_ut_ctx = NULL;

	cuse_nvme_reset(req, 0, NULL, NULL, 0, NULL, 4096, 4096);
	CU_ASSERT(g_ut_ctx == NULL);

	/* Valid nsid, check IO message sent value */
	cuse_device.nsid = 0;

	cuse_nvme_reset(req, 0, NULL, NULL, 0, NULL, 4096, 4096);
	CU_ASSERT(g_ut_ctx == (void *)0xDEADBEEF);
	CU_ASSERT(g_ut_ctrlr == &ctrlr);
	CU_ASSERT(g_ut_nsid == 0);
}

static void
test_nvme_cuse_stop(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct cuse_device *ctrlr_device = NULL;
	struct cuse_device *ns_dev1, *ns_dev2;

	/* Allocate memory for nvme_cuse_stop() to free. */
	ctrlr_device = calloc(1, sizeof(struct cuse_device));
	SPDK_CU_ASSERT_FATAL(ctrlr_device != NULL);

	TAILQ_INIT(&ctrlr_device->ns_devices);
	ns_dev1 = calloc(1, sizeof(struct cuse_device));
	SPDK_CU_ASSERT_FATAL(ns_dev1 != NULL);
	ns_dev2 = calloc(1, sizeof(struct cuse_device));
	SPDK_CU_ASSERT_FATAL(ns_dev2 != NULL);

	g_ctrlr_started = spdk_bit_array_create(128);
	SPDK_CU_ASSERT_FATAL(g_ctrlr_started != NULL);

	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_dev1, tailq);
	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_dev2, tailq);
	ctrlr.cdata.nn = 2;
	ctrlr_device->ctrlr = &ctrlr;
	pthread_mutex_init(&g_cuse_mtx, NULL);
	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);

	nvme_cuse_stop(&ctrlr);
	CU_ASSERT(g_ctrlr_started == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_ctrlr_ctx_head));
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_cuse", NULL, NULL);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_io_read_write);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_io_read_write_with_md);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_passthru_cmd);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_passthru_cmd_with_md);
	CU_ADD_TEST(suite, test_nvme_cuse_get_cuse_ns_device);
	CU_ADD_TEST(suite, test_cuse_nvme_submit_io);
	CU_ADD_TEST(suite, test_cuse_nvme_reset);
	CU_ADD_TEST(suite, test_nvme_cuse_stop);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
