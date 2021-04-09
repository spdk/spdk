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

#include "nvme/nvme_ns_ocssd_cmd.c"
#include "nvme/nvme_ns_cmd.c"
#include "nvme/nvme.c"

#include "common/lib/test_env.c"

#define OCSSD_SECTOR_SIZE 0x1000

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

DEFINE_STUB(spdk_pci_event_listen, int, (void), 1);

DEFINE_STUB_V(nvme_ctrlr_fail,
	      (struct spdk_nvme_ctrlr *ctrlr, bool hotremove));

DEFINE_STUB(nvme_transport_ctrlr_destruct,
	    int,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    0);

DEFINE_STUB(nvme_ctrlr_get_current_process,
	    struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    (struct spdk_nvme_ctrlr_process *)(uintptr_t)0x1);

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_request = req;

	return 0;
}

void
nvme_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
}

void
nvme_ctrlr_proc_get_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

int
nvme_ctrlr_process_init(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

void
nvme_ctrlr_proc_put_ref(struct spdk_nvme_ctrlr *ctrlr)
{
	return;
}

void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	memset(opts, 0, sizeof(*opts));
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

uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->max_xfer_size;
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 struct spdk_nvme_qpair *qpair,
		 uint32_t sector_size, uint32_t md_size, uint32_t max_xfer_size,
		 uint32_t stripe_size, bool extended_lba)
{
	uint32_t num_requests = 32;
	uint32_t i;

	memset(ctrlr, 0, sizeof(*ctrlr));
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
test_nvme_ocssd_ns_cmd_vector_reset_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	uint64_t lba_list = 0x12345678;
	spdk_nvme_ocssd_ns_cmd_vector_reset(&ns, &qpair, &lba_list, 1,
					    NULL, NULL, NULL);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_RESET);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ocssd_ns_cmd_vector_reset(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	uint64_t lba_list[vector_size];
	spdk_nvme_ocssd_ns_cmd_vector_reset(&ns, &qpair, lba_list, vector_size,
					    NULL, NULL, NULL);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_RESET);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ocssd_ns_cmd_vector_read_with_md_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	md_size = 0x80;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size);
	char *metadata = malloc(md_size);
	uint64_t lba_list = 0x12345678;

	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, md_size, max_xfer_size, 0, false);
	rc = spdk_nvme_ocssd_ns_cmd_vector_read_with_md(&ns, &qpair, buffer, metadata,
			&lba_list, 1, NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == OCSSD_SECTOR_SIZE);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_READ);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

static void
test_nvme_ocssd_ns_cmd_vector_read_with_md(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	md_size = 0x80;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size * vector_size);
	char *metadata = malloc(md_size * vector_size);
	uint64_t lba_list[vector_size];

	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, md_size, max_xfer_size, 0, false);
	rc = spdk_nvme_ocssd_ns_cmd_vector_read_with_md(&ns, &qpair, buffer, metadata,
			lba_list, vector_size,
			NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == max_xfer_size);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_READ);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

static void
test_nvme_ocssd_ns_cmd_vector_read_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size);
	uint64_t lba_list = 0x12345678;

	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	rc = spdk_nvme_ocssd_ns_cmd_vector_read(&ns, &qpair, buffer, &lba_list, 1,
						NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload_size == OCSSD_SECTOR_SIZE);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_READ);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
	free(buffer);
}

static void
test_nvme_ocssd_ns_cmd_vector_read(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size * vector_size);
	uint64_t lba_list[vector_size];

	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	rc = spdk_nvme_ocssd_ns_cmd_vector_read(&ns, &qpair, buffer, lba_list, vector_size,
						NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload_size == max_xfer_size);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_READ);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
	free(buffer);
}

static void
test_nvme_ocssd_ns_cmd_vector_write_with_md_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	md_size = 0x80;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size);
	char *metadata = malloc(md_size);
	uint64_t lba_list = 0x12345678;

	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, md_size, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_write_with_md(&ns, &qpair, buffer, metadata,
			&lba_list, 1, NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == OCSSD_SECTOR_SIZE);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_WRITE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}


static void
test_nvme_ocssd_ns_cmd_vector_write_with_md(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	md_size = 0x80;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size * vector_size);
	char *metadata = malloc(md_size * vector_size);
	uint64_t lba_list[vector_size];

	SPDK_CU_ASSERT_FATAL(buffer != NULL);
	SPDK_CU_ASSERT_FATAL(metadata != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, md_size, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_write_with_md(&ns, &qpair, buffer, metadata,
			lba_list, vector_size,
			NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload.md == metadata);
	CU_ASSERT(g_request->payload_size == max_xfer_size);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_WRITE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
	free(metadata);
}

static void
test_nvme_ocssd_ns_cmd_vector_write_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size);
	uint64_t lba_list = 0x12345678;

	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_write(&ns, &qpair, buffer,
					    &lba_list, 1,    NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload_size == OCSSD_SECTOR_SIZE);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_WRITE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
}

static void
test_nvme_ocssd_ns_cmd_vector_write(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	char *buffer = malloc(sector_size * vector_size);
	uint64_t lba_list[vector_size];

	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_write(&ns, &qpair, buffer,
					    lba_list, vector_size,
					    NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);

	CU_ASSERT(g_request->payload_size == max_xfer_size);
	CU_ASSERT(g_request->payload.contig_or_cb_arg == buffer);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_WRITE);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);

	free(buffer);
}

static void
test_nvme_ocssd_ns_cmd_vector_copy_single_entry(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	uint64_t src_lba_list = 0x12345678;
	uint64_t dst_lba_list = 0x87654321;

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_copy(&ns, &qpair, &dst_lba_list, &src_lba_list, 1,
					   NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_COPY);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw10 == src_lba_list);
	CU_ASSERT(g_request->cmd.cdw12 == 0);
	CU_ASSERT(g_request->cmd.cdw14 == dst_lba_list);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_nvme_ocssd_ns_cmd_vector_copy(void)
{
	const uint32_t	max_xfer_size = 0x10000;
	const uint32_t	sector_size = OCSSD_SECTOR_SIZE;
	const uint32_t	vector_size = 0x10;

	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;

	int rc = 0;

	uint64_t src_lba_list[vector_size];
	uint64_t dst_lba_list[vector_size];

	prepare_for_test(&ns, &ctrlr, &qpair, sector_size, 0, max_xfer_size, 0, false);
	spdk_nvme_ocssd_ns_cmd_vector_copy(&ns, &qpair,
					   dst_lba_list, src_lba_list, vector_size,
					   NULL, NULL, 0);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	SPDK_CU_ASSERT_FATAL(g_request->num_children == 0);
	CU_ASSERT(g_request->cmd.opc == SPDK_OCSSD_OPC_VECTOR_COPY);
	CU_ASSERT(g_request->cmd.nsid == ns.id);
	CU_ASSERT(g_request->cmd.cdw12 == vector_size - 1);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_ns_cmd", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_reset);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_reset_single_entry);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_read_with_md);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_read_with_md_single_entry);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_read);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_read_single_entry);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_write_with_md);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_write_with_md_single_entry);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_write);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_write_single_entry);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_copy);
	CU_ADD_TEST(suite, test_nvme_ocssd_ns_cmd_vector_copy_single_entry);

	g_spdk_nvme_driver = &_g_nvme_driver;

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
