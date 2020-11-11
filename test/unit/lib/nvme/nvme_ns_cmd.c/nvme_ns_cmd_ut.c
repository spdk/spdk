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

#include "spdk_cunit.h"

#include "nvme/nvme_ns_cmd.c"
#include "nvme/nvme.c"

#include "common/lib/test_env.c"

static struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct nvme_request *g_request = NULL;

DEFINE_STUB_V(nvme_io_msg_ctrlr_detach, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB_V(nvme_ctrlr_destruct_async,
	      (struct spdk_nvme_ctrlr *ctrlr, struct nvme_ctrlr_detach_ctx *ctx));

DEFINE_STUB(nvme_ctrlr_destruct_poll_async,
	    int,
	    (struct spdk_nvme_ctrlr *ctrlr, struct nvme_ctrlr_detach_ctx *ctx),
	    0);

DEFINE_STUB(spdk_nvme_poll_group_process_completions,
	    int64_t,
	    (struct spdk_nvme_poll_group *group, uint32_t completions_per_qpair,
	     spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb),
	    0);

DEFINE_STUB(spdk_nvme_qpair_process_completions,
	    int32_t,
	    (struct spdk_nvme_qpair *qpair, uint32_t max_completions),
	    0);

DEFINE_STUB(spdk_nvme_ctrlr_get_regs_csts,
	    union spdk_nvme_csts_register,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    {});

DEFINE_STUB(nvme_uevent_connect, int, (void), 1);

DEFINE_STUB(nvme_transport_ctrlr_destruct,
	    int,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    0);

DEFINE_STUB(nvme_ctrlr_get_current_process,
	    struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    (struct spdk_nvme_ctrlr_process *)(uintptr_t)0x1);

int
spdk_pci_enumerate(struct spdk_pci_driver *driver, spdk_pci_enum_cb enum_cb, void *enum_ctx)
{
	return -1;
}

static void nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
}

static int nvme_request_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	uint32_t *lba_count = cb_arg;

	/*
	 * We need to set address to something here, since the SGL splitting code will
	 *  use it to determine PRP compatibility.  Just use a rather arbitrary address
	 *  for now - these tests will not actually cause data to be read from or written
	 *  to this address.
	 */
	*address = (void *)(uintptr_t)0x10000000;
	*length = *lba_count;
	return 0;
}

bool
spdk_nvme_transport_available_by_name(const char *transport_name)
{
	return true;
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

int
nvme_ctrlr_add_process(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	return 0;
}

int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

void
nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr, bool hot_remove)
{
}

struct spdk_pci_addr
spdk_pci_device_get_addr(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr;

	memset(&pci_addr, 0, sizeof(pci_addr));
	return pci_addr;
}

struct spdk_pci_id
spdk_pci_device_get_id(struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_id pci_id;

	memset(&pci_id, 0xFF, sizeof(pci_id));

	return pci_id;
}

void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	memset(opts, 0, sizeof(*opts));
}

uint32_t
spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns)
{
	return ns->sector_size;
}

uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->max_xfer_size;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_request = req;

	return 0;
}

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

int
nvme_ctrlr_get_ref_count(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
			  bool direct_connect)
{
	return 0;
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 struct spdk_nvme_qpair *qpair,
		 uint32_t sector_size, uint32_t md_size, uint32_t max_xfer_size,
		 uint32_t stripe_size, bool extended_lba)
{
	uint32_t num_requests = 32;
	uint32_t i;

	ctrlr->max_xfer_size = max_xfer_size;
	/*
	 * Clear the flags field - we especially want to make sure the SGL_SUPPORTED flag is not set
	 *  so that we test the SGL splitting path.
	 */
	ctrlr->flags = 0;
	ctrlr->min_page_size = 4096;
	ctrlr->page_size = 4096;
	memset(&ctrlr->opts, 0, sizeof(ctrlr->opts));
	memset(ns, 0, sizeof(*ns));
	ns->ctrlr = ctrlr;
	ns->sector_size = sector_size;
	ns->extended_lba_size = sector_size;
	if (extended_lba) {
		ns->flags |= SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED;
		ns->extended_lba_size += md_size;
	}
	ns->md_size = md_size;
	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->extended_lba_size;
	ns->sectors_per_stripe = stripe_size / ns->extended_lba_size;

	memset(qpair, 0, sizeof(*qpair));
	qpair->ctrlr = ctrlr;
	qpair->req_buf = calloc(num_requests, sizeof(struct nvme_request));
	SPDK_CU_ASSERT_FATAL(qpair->req_buf != NULL);

	for (i = 0; i < num_requests; i++) {
		struct nvme_request *req = qpair->req_buf + i * sizeof(struct nvme_request);

		req->qpair = qpair;
		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
	}

	g_request = NULL;
}

static void
cleanup_after_test(struct spdk_nvme_qpair *qpair)
{
	free(qpair->req_buf);
}

static void
nvme_cmd_interpret_rw(const struct spdk_nvme_cmd *cmd,
		      uint64_t *lba, uint32_t *num_blocks)
{
	*lba = *(const uint64_t *)&cmd->cdw10;
	*num_blocks = (cmd->cdw12 & 0xFFFFu) + 1;
}

static void
split_test(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_ctrlr	ctrlr;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(512);
	lba = 0;
	lba_count = 1;

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	CU_ASSERT(g_request->num_children == 0);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(cmd_lba == lba);
	CU_ASSERT(cmd_lba_count == lba_count);

	free(payload);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
split_test2(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks).
	 * Submit an I/O of 256 KB starting at LBA 0, which should be split
	 * on the max I/O boundary into two I/Os of 128 KB.
	 */

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(256 * 1024);
	lba = 0;
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	CU_ASSERT(g_request->num_children == 2);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 0);
	CU_ASSERT(cmd_lba_count == 256); /* 256 * 512 byte blocks = 128 KB */
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 256);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
split_test3(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks).
	 * Submit an I/O of 256 KB starting at LBA 10, which should be split
	 * into two I/Os:
	 *  1) LBA = 10, count = 256 blocks
	 *  2) LBA = 266, count = 256 blocks
	 */

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(256 * 1024);
	lba = 10; /* Start at an LBA that isn't aligned to the stripe size */
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 10);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(cmd_lba == 266);
	CU_ASSERT(cmd_lba_count == 256);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
split_test4(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct nvme_request	*child;
	void			*payload;
	uint64_t		lba, cmd_lba;
	uint32_t		lba_count, cmd_lba_count;
	int			rc;

	/*
	 * Controller has max xfer of 128 KB (256 blocks) and a stripe size of 128 KB.
	 * (Same as split_test3 except with driver-assisted striping enabled.)
	 * Submit an I/O of 256 KB starting at LBA 10, which should be split
	 * into three I/Os:
	 *  1) LBA = 10, count = 246 blocks (less than max I/O size to align to stripe size)
	 *  2) LBA = 256, count = 256 blocks (aligned to stripe size and max I/O size)
	 *  3) LBA = 512, count = 10 blocks (finish off the remaining I/O size)
	 */

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 128 * 1024, false);
	payload = malloc(256 * 1024);
	lba = 10; /* Start at an LBA that isn't aligned to the stripe size */
	lba_count = (256 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);

	SPDK_CU_ASSERT_FATAL(g_request->num_children == 3);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == (256 - 10) * 512);
	CU_ASSERT(child->payload_offset == 0);
	CU_ASSERT(cmd_lba == 10);
	CU_ASSERT(cmd_lba_count == 256 - 10);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 128 * 1024);
	CU_ASSERT(child->payload_offset == (256 - 10) * 512);
	CU_ASSERT(cmd_lba == 256);
	CU_ASSERT(cmd_lba_count == 256);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	child = TAILQ_FIRST(&g_request->children);
	nvme_request_remove_child(g_request, child);
	nvme_cmd_interpret_rw(&child->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT(child->num_children == 0);
	CU_ASSERT(child->payload_size == 10 * 512);
	CU_ASSERT(child->payload_offset == (512 - 10) * 512);
	CU_ASSERT(cmd_lba == 512);
	CU_ASSERT(cmd_lba_count == 10);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((child->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(child);

	CU_ASSERT(TAILQ_EMPTY(&g_request->children));

	free(payload);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_cmd_child_request(void)
{

	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	int				rc = 0;
	struct nvme_request		*child, *tmp;
	void				*payload;
	uint64_t			lba = 0x1000;
	uint32_t			i = 0;
	uint32_t			offset = 0;
	uint32_t			sector_size = 512;
	uint32_t			max_io_size = 128 * 1024;
	uint32_t			sectors_per_max_io = max_io_size / sector_size;

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_io_size, 0, false);

	payload = malloc(128 * 1024);
	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, sectors_per_max_io, NULL, NULL, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->payload_offset == 0);
	CU_ASSERT(g_request->num_children == 0);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, sectors_per_max_io - 1, NULL, NULL, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->payload_offset == 0);
	CU_ASSERT(g_request->num_children == 0);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, sectors_per_max_io * 4, NULL, NULL, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 4);

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, (DEFAULT_IO_QUEUE_REQUESTS + 1) * sector_size,
				   NULL,
				   NULL, 0);
	SPDK_CU_ASSERT_FATAL(rc == -EINVAL);

	TAILQ_FOREACH_SAFE(child, &g_request->children, child_tailq, tmp) {
		nvme_request_remove_child(g_request, child);
		CU_ASSERT(child->payload_offset == offset);
		CU_ASSERT(child->cmd.opc == SPDK_NVME_OPC_READ);
		CU_ASSERT(child->cmd.nsid == ns.id);
		CU_ASSERT(child->cmd.cdw10 == (lba + sectors_per_max_io * i));
		CU_ASSERT(child->cmd.cdw12 == ((sectors_per_max_io - 1) | 0));
		offset += max_io_size;
		nvme_free_request(child);
		i++;
	}

	free(payload);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_flush(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_flush(&ns, &qpair, cb_fn, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_FLUSH);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_write_zeroes(void)
{
	struct spdk_nvme_ns	ns = { 0 };
	struct spdk_nvme_ctrlr	ctrlr = { 0 };
	struct spdk_nvme_qpair	qpair;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	uint64_t		cmd_lba;
	uint32_t		cmd_lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_write_zeroes(&ns, &qpair, 0, 2, cb_fn, cb_arg, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_WRITE_ZEROES);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba, 0);
	CU_ASSERT_EQUAL(cmd_lba_count, 2);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_write_uncorrectable(void)
{
	struct spdk_nvme_ns	ns = { 0 };
	struct spdk_nvme_ctrlr	ctrlr = { 0 };
	struct spdk_nvme_qpair	qpair;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	uint64_t		cmd_lba;
	uint32_t		cmd_lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_write_uncorrectable(&ns, &qpair, 0, 2, cb_fn, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_WRITE_UNCORRECTABLE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba, 0);
	CU_ASSERT_EQUAL(cmd_lba_count, 2);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_dataset_management(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	struct spdk_nvme_dsm_range	ranges[256];
	uint16_t			i;
	int			rc = 0;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);

	for (i = 0; i < 256; i++) {
		ranges[i].starting_lba = i;
		ranges[i].length = 1;
		ranges[i].attributes.raw = 0;
	}

	/* TRIM one LBA */
	rc = spdk_nvme_ns_cmd_dataset_management(&ns, &qpair, SPDK_NVME_DSM_ATTR_DEALLOCATE,
			ranges, 1, cb_fn, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == 0);
	CU_ASSERT(g_request->cmd.cdw11_bits.dsm.ad == 1);
	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);

	/* TRIM 256 LBAs */
	rc = spdk_nvme_ns_cmd_dataset_management(&ns, &qpair, SPDK_NVME_DSM_ATTR_DEALLOCATE,
			ranges, 256, cb_fn, cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == 255u);
	CU_ASSERT(g_request->cmd.cdw11_bits.dsm.ad == 1);
	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_dataset_management(&ns, &qpair, SPDK_NVME_DSM_ATTR_DEALLOCATE,
			NULL, 0, cb_fn, cb_arg);
	CU_ASSERT(rc != 0);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_readv(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	int				rc = 0;
	void				*cb_arg;
	uint32_t			lba_count = 256;
	uint32_t			sector_size = 512;
	uint64_t			sge_length = lba_count * sector_size;

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, 128 * 1024, 0, false);
	rc = spdk_nvme_ns_cmd_readv(&ns, &qpair, 0x1000, lba_count, NULL, &sge_length, 0,
				    nvme_request_reset_sgl, nvme_request_next_sge);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_READ);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_SGL);
	CU_ASSERT(g_request->payload.reset_sgl_fn == nvme_request_reset_sgl);
	CU_ASSERT(g_request->payload.next_sge_fn == nvme_request_next_sge);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == &sge_length);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	rc = spdk_nvme_ns_cmd_readv(&ns, &qpair, 0x1000, 256, NULL, cb_arg, 0, nvme_request_reset_sgl,
				    NULL);
	CU_ASSERT(rc != 0);

	free(cb_arg);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_writev(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	int				rc = 0;
	void				*cb_arg;
	uint32_t			lba_count = 256;
	uint32_t			sector_size = 512;
	uint64_t			sge_length = lba_count * sector_size;

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, 128 * 1024, 0, false);
	rc = spdk_nvme_ns_cmd_writev(&ns, &qpair, 0x1000, lba_count, NULL, &sge_length, 0,
				     nvme_request_reset_sgl, nvme_request_next_sge);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_WRITE);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_SGL);
	CU_ASSERT(g_request->payload.reset_sgl_fn == nvme_request_reset_sgl);
	CU_ASSERT(g_request->payload.next_sge_fn == nvme_request_next_sge);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == &sge_length);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	rc = spdk_nvme_ns_cmd_writev(&ns, &qpair, 0x1000, 256, NULL, cb_arg, 0,
				     NULL, nvme_request_next_sge);
	CU_ASSERT(rc != 0);

	free(cb_arg);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_comparev(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	int				rc = 0;
	void				*cb_arg;
	uint32_t			lba_count = 256;
	uint32_t			sector_size = 512;
	uint64_t			sge_length = lba_count * sector_size;

	cb_arg = malloc(512);
	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, 128 * 1024, 0, false);
	rc = spdk_nvme_ns_cmd_comparev(&ns, &qpair, 0x1000, lba_count, NULL, &sge_length, 0,
				       nvme_request_reset_sgl, nvme_request_next_sge);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_COMPARE);
	CU_ASSERT(nvme_payload_type(&g_request->payload) == NVME_PAYLOAD_TYPE_SGL);
	CU_ASSERT(g_request->payload.reset_sgl_fn == nvme_request_reset_sgl);
	CU_ASSERT(g_request->payload.next_sge_fn == nvme_request_next_sge);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == &sge_length);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	rc = spdk_nvme_ns_cmd_comparev(&ns, &qpair, 0x1000, 256, NULL, cb_arg, 0,
				       nvme_request_reset_sgl, NULL);
	CU_ASSERT(rc != 0);

	free(cb_arg);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_comparev_with_md(void)
{
	struct spdk_nvme_ns             ns;
	struct spdk_nvme_ctrlr          ctrlr;
	struct spdk_nvme_qpair          qpair;
	int                             rc = 0;
	char				*buffer = NULL;
	char				*metadata = NULL;
	uint32_t			block_size, md_size;
	struct nvme_request		*child0, *child1;
	uint32_t			lba_count = 256;
	uint32_t			sector_size = 512;
	uint64_t			sge_length = lba_count * sector_size;

	block_size = 512;
	md_size = 128;

	buffer = malloc((block_size + md_size) * 384);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	metadata = malloc(md_size * 384);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	/*
	 * 512 byte data + 128 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * 512 bytes per block = single 128 KB I/O (no splitting required)
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 256, NULL, &sge_length, 0,
					       nvme_request_reset_sgl, nvme_request_next_sge, metadata, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 128 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * (512 + 128) bytes per block = two I/Os:
	 *   child 0: 204 blocks - 204 * (512 + 128) = 127.5 KB
	 *   child 1: 52 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 256, NULL, &sge_length, 0,
					       nvme_request_reset_sgl, nvme_request_next_sge, NULL, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 204 * (512 + 128));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 204 * (512 + 128));
	CU_ASSERT(child1->payload_size == 52 * (512 + 128));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * No protection information
	 *
	 * 256 blocks * (512 + 8) bytes per block = two I/Os:
	 *   child 0: 252 blocks - 252 * (512 + 8) = 127.96875 KB
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 256, NULL, &sge_length, 0,
					       nvme_request_reset_sgl, nvme_request_next_sge, NULL, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * (512 + 8));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * (512 + 8));
	CU_ASSERT(child1->payload_size == 4 * (512 + 8));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * Special case for 8-byte metadata + PI + PRACT: no metadata transferred
	 * In theory, 256 blocks * 512 bytes per block = one I/O (128 KB)
	 * However, the splitting code does not account for PRACT when calculating
	 * max sectors per transfer, so we actually get two I/Os:
	 *   child 0: 252 blocks
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 256, NULL, &sge_length,
					       SPDK_NVME_IO_FLAGS_PRACT, nvme_request_reset_sgl, nvme_request_next_sge, NULL, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * 512); /* NOTE: does not include metadata! */
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * 512);
	CU_ASSERT(child1->payload_size == 4 * 512);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 256, NULL, &sge_length,
					       SPDK_NVME_IO_FLAGS_PRACT, nvme_request_reset_sgl, nvme_request_next_sge, metadata, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * 384 blocks * 512 bytes = two I/Os:
	 *   child 0: 256 blocks
	 *   child 1: 128 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_comparev_with_md(&ns, &qpair, 0x1000, 384, NULL, &sge_length,
					       SPDK_NVME_IO_FLAGS_PRACT, nvme_request_reset_sgl, nvme_request_next_sge, metadata, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 256 * 512);
	CU_ASSERT(child0->md_offset == 0);
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload_offset == 256 * 512);
	CU_ASSERT(child1->payload_size == 128 * 512);
	CU_ASSERT(child1->md_offset == 256 * 8);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

static void
test_nvme_ns_cmd_compare_and_write(void)
{
	struct spdk_nvme_ns		ns;
	struct spdk_nvme_ctrlr		ctrlr;
	struct spdk_nvme_qpair		qpair;
	int				rc = 0;
	uint64_t			lba = 0x1000;
	uint32_t			lba_count = 256;
	uint64_t			cmd_lba;
	uint32_t			cmd_lba_count;
	uint32_t			sector_size = 512;

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_compare(&ns, &qpair, NULL, lba, lba_count, NULL, NULL,
				      SPDK_NVME_IO_FLAGS_FUSE_FIRST);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_COMPARE);
	CU_ASSERT(g_request->cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba, lba);
	CU_ASSERT_EQUAL(cmd_lba_count, lba_count);

	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_write(&ns, &qpair, NULL, lba, lba_count, NULL, NULL,
				    SPDK_NVME_IO_FLAGS_FUSE_SECOND);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_WRITE);
	CU_ASSERT(g_request->cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba, lba);
	CU_ASSERT_EQUAL(cmd_lba_count, lba_count);

	nvme_free_request(g_request);

	cleanup_after_test(&qpair);
}

static void
test_io_flags(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	void			*payload;
	uint64_t		lba;
	uint32_t		lba_count;
	uint64_t		cmd_lba;
	uint32_t		cmd_lba_count;
	int			rc;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 128 * 1024, false);
	payload = malloc(256 * 1024);
	lba = 0;
	lba_count = (4 * 1024) / 512;

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) == 0);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_read(&ns, &qpair, payload, lba, lba_count, NULL, NULL,
				   SPDK_NVME_IO_FLAGS_LIMITED_RETRY);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) == 0);
	CU_ASSERT((g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_LIMITED_RETRY) != 0);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_write(&ns, &qpair, payload, lba, lba_count, NULL, NULL,
				    SPDK_NVME_IO_FLAGS_VALID_MASK);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	nvme_cmd_interpret_rw(&g_request->cmd, &cmd_lba, &cmd_lba_count);
	CU_ASSERT_EQUAL(cmd_lba_count, lba_count);
	CU_ASSERT_EQUAL(cmd_lba, lba);
	CU_ASSERT_EQUAL(g_request->cmd.cdw12 & SPDK_NVME_IO_FLAGS_CDW12_MASK,
			SPDK_NVME_IO_FLAGS_CDW12_MASK);
	nvme_free_request(g_request);

	rc = spdk_nvme_ns_cmd_write(&ns, &qpair, payload, lba, lba_count, NULL, NULL,
				    ~SPDK_NVME_IO_FLAGS_VALID_MASK);
	CU_ASSERT(rc == -EINVAL);

	free(payload);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_reservation_register(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_reservation_register_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(sizeof(struct spdk_nvme_reservation_register_data));

	rc = spdk_nvme_ns_cmd_reservation_register(&ns, &qpair, payload, ignore_key,
			SPDK_NVME_RESERVE_REGISTER_KEY,
			SPDK_NVME_RESERVE_PTPL_NO_CHANGES,
			cb_fn, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_REGISTER);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_REGISTER_KEY;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_PTPL_NO_CHANGES << 30;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);
	free(payload);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_reservation_release(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_reservation_key_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(sizeof(struct spdk_nvme_reservation_key_data));

	rc = spdk_nvme_ns_cmd_reservation_release(&ns, &qpair, payload, ignore_key,
			SPDK_NVME_RESERVE_RELEASE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			cb_fn, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_RELEASE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_RELEASE;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_WRITE_EXCLUSIVE << 8;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);
	free(payload);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_reservation_acquire(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_reservation_acquire_data *payload;
	bool			ignore_key = 1;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t		tmp_cdw10;

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);
	payload = malloc(sizeof(struct spdk_nvme_reservation_acquire_data));

	rc = spdk_nvme_ns_cmd_reservation_acquire(&ns, &qpair, payload, ignore_key,
			SPDK_NVME_RESERVE_ACQUIRE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			cb_fn, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_ACQUIRE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	tmp_cdw10 = SPDK_NVME_RESERVE_ACQUIRE;
	tmp_cdw10 |= ignore_key ? 1 << 3 : 0;
	tmp_cdw10 |= (uint32_t)SPDK_NVME_RESERVE_WRITE_EXCLUSIVE << 8;

	CU_ASSERT(g_request->cmd.cdw10 == tmp_cdw10);

	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);
	free(payload);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_reservation_report(void)
{
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_reservation_status_data *payload;
	spdk_nvme_cmd_cb	cb_fn = NULL;
	void			*cb_arg = NULL;
	int			rc = 0;
	uint32_t size = sizeof(struct spdk_nvme_reservation_status_data);

	prepare_for_test(&ns, &ctrlr, &qpair, 512, 0, 128 * 1024, 0, false);

	payload = calloc(1, size);
	SPDK_CU_ASSERT_FATAL(payload != NULL);

	rc = spdk_nvme_ns_cmd_reservation_report(&ns, &qpair, payload, size, cb_fn, cb_arg);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->cmd.opc == SPDK_NVME_OPC_RESERVATION_REPORT);
	CU_ASSERT(g_request->cmd.nsid == ns.id);

	CU_ASSERT(g_request->cmd.cdw10 == (size / 4));

	spdk_free(g_request->payload.contig_or_cb_arg);
	nvme_free_request(g_request);
	free(payload);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ns_cmd_write_with_md(void)
{
	struct spdk_nvme_ns             ns;
	struct spdk_nvme_ctrlr          ctrlr;
	struct spdk_nvme_qpair          qpair;
	int                             rc = 0;
	char				*buffer = NULL;
	char				*metadata = NULL;
	uint32_t			block_size, md_size;
	struct nvme_request		*child0, *child1;

	block_size = 512;
	md_size = 128;

	buffer = malloc((block_size + md_size) * 384);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	metadata = malloc(md_size * 384);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	/*
	 * 512 byte data + 128 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * 512 bytes per block = single 128 KB I/O (no splitting required)
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, metadata, 0x1000, 256, NULL, NULL, 0, 0,
					    0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->md_size == 256 * 128);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 128 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * (512 + 128) bytes per block = two I/Os:
	 *   child 0: 204 blocks - 204 * (512 + 128) = 127.5 KB
	 *   child 1: 52 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256, NULL, NULL, 0, 0,
					    0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 204 * (512 + 128));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 204 * (512 + 128));
	CU_ASSERT(child1->payload_size == 52 * (512 + 128));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * No protection information
	 *
	 * 256 blocks * (512 + 8) bytes per block = two I/Os:
	 *   child 0: 252 blocks - 252 * (512 + 8) = 127.96875 KB
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256, NULL, NULL, 0, 0,
					    0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * (512 + 8));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * (512 + 8));
	CU_ASSERT(child1->payload_size == 4 * (512 + 8));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * Special case for 8-byte metadata + PI + PRACT: no metadata transferred
	 * In theory, 256 blocks * 512 bytes per block = one I/O (128 KB)
	 * However, the splitting code does not account for PRACT when calculating
	 * max sectors per transfer, so we actually get two I/Os:
	 *   child 0: 252 blocks
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256, NULL, NULL,
					    SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * 512); /* NOTE: does not include metadata! */
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * 512);
	CU_ASSERT(child1->payload_size == 4 * 512);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, metadata, 0x1000, 256, NULL, NULL,
					    SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->md_size == 256 * 8);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * 384 blocks * 512 bytes = two I/Os:
	 *   child 0: 256 blocks
	 *   child 1: 128 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_write_with_md(&ns, &qpair, buffer, metadata, 0x1000, 384, NULL, NULL,
					    SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 256 * 512);
	CU_ASSERT(child0->md_offset == 0);
	CU_ASSERT(child0->md_size == 256 * 8);
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload_offset == 256 * 512);
	CU_ASSERT(child1->payload_size == 128 * 512);
	CU_ASSERT(child1->md_offset == 256 * 8);
	CU_ASSERT(child1->md_size == 128 * 8);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

static void
test_nvme_ns_cmd_read_with_md(void)
{
	struct spdk_nvme_ns             ns;
	struct spdk_nvme_ctrlr          ctrlr;
	struct spdk_nvme_qpair          qpair;
	int                             rc = 0;
	char				*buffer = NULL;
	char				*metadata = NULL;
	uint32_t			block_size, md_size;

	block_size = 512;
	md_size = 128;

	buffer = malloc(block_size * 256);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	metadata = malloc(md_size * 256);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	/*
	 * 512 byte data + 128 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * 512 bytes per block = single 128 KB I/O (no splitting required)
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_read_with_md(&ns, &qpair, buffer, metadata, 0x1000, 256, NULL, NULL, 0, 0,
					   0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->md_size == 256 * md_size);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
	free(buffer);
	free(metadata);
}

static void
test_nvme_ns_cmd_compare_with_md(void)
{
	struct spdk_nvme_ns             ns;
	struct spdk_nvme_ctrlr          ctrlr;
	struct spdk_nvme_qpair          qpair;
	int                             rc = 0;
	char				*buffer = NULL;
	char				*metadata = NULL;
	uint32_t			block_size, md_size;
	struct nvme_request		*child0, *child1;

	block_size = 512;
	md_size = 128;

	buffer = malloc((block_size + md_size) * 384);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	metadata = malloc(md_size * 384);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	/*
	 * 512 byte data + 128 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * 512 bytes per block = single 128 KB I/O (no splitting required)
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, false);

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, metadata, 0x1000, 256,
					      NULL, NULL, 0, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 128 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 *
	 * 256 blocks * (512 + 128) bytes per block = two I/Os:
	 *   child 0: 204 blocks - 204 * (512 + 128) = 127.5 KB
	 *   child 1: 52 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 128, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256,
					      NULL, NULL, 0, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 204 * (512 + 128));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 204 * (512 + 128));
	CU_ASSERT(child1->payload_size == 52 * (512 + 128));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * No protection information
	 *
	 * 256 blocks * (512 + 8) bytes per block = two I/Os:
	 *   child 0: 252 blocks - 252 * (512 + 8) = 127.96875 KB
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256,
					      NULL, NULL, 0, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload.md == NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * (512 + 8));
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * (512 + 8));
	CU_ASSERT(child1->payload_size == 4 * (512 + 8));

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Extended LBA
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * Special case for 8-byte metadata + PI + PRACT: no metadata transferred
	 * In theory, 256 blocks * 512 bytes per block = one I/O (128 KB)
	 * However, the splitting code does not account for PRACT when calculating
	 * max sectors per transfer, so we actually get two I/Os:
	 *   child 0: 252 blocks
	 *   child 1: 4 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, true);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, NULL, 0x1000, 256,
					      NULL, NULL, SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 252 * 512); /* NOTE: does not include metadata! */
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload.md == NULL);
	CU_ASSERT(child1->payload_offset == 252 * 512);
	CU_ASSERT(child1->payload_size == 4 * 512);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, metadata, 0x1000, 256,
					      NULL, NULL, SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == 256 * 512);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	/*
	 * 512 byte data + 8 byte metadata
	 * Separate metadata buffer
	 * Max data transfer size 128 KB
	 * No stripe size
	 * Protection information enabled + PRACT
	 *
	 * 384 blocks * 512 bytes = two I/Os:
	 *   child 0: 256 blocks
	 *   child 1: 128 blocks
	 */
	prepare_for_test(&ns, &ctrlr, &qpair, 512, 8, 128 * 1024, 0, false);
	ns.flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;

	rc = spdk_nvme_ns_cmd_compare_with_md(&ns, &qpair, buffer, metadata, 0x1000, 384,
					      NULL, NULL, SPDK_NVME_IO_FLAGS_PRACT, 0, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 2);
	child0 = TAILQ_FIRST(&g_request->children);

	SPDK_CU_ASSERT_FATAL(child0 != NULL);
	CU_ASSERT(child0->payload_offset == 0);
	CU_ASSERT(child0->payload_size == 256 * 512);
	CU_ASSERT(child0->md_offset == 0);
	child1 = TAILQ_NEXT(child0, child_tailq);

	SPDK_CU_ASSERT_FATAL(child1 != NULL);
	CU_ASSERT(child1->payload_offset == 256 * 512);
	CU_ASSERT(child1->payload_size == 128 * 512);
	CU_ASSERT(child1->md_offset == 256 * 8);

	nvme_request_free_children(g_request);
	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_ns_cmd", NULL, NULL);

	CU_ADD_TEST(suite, split_test);
	CU_ADD_TEST(suite, split_test2);
	CU_ADD_TEST(suite, split_test3);
	CU_ADD_TEST(suite, split_test4);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_flush);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_dataset_management);
	CU_ADD_TEST(suite, test_io_flags);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_write_zeroes);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_write_uncorrectable);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_reservation_register);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_reservation_release);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_reservation_acquire);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_reservation_report);
	CU_ADD_TEST(suite, test_cmd_child_request);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_readv);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_read_with_md);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_writev);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_write_with_md);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_comparev);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_compare_and_write);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_compare_with_md);
	CU_ADD_TEST(suite, test_nvme_ns_cmd_comparev_with_md);

	g_spdk_nvme_driver = &_g_nvme_driver;

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
