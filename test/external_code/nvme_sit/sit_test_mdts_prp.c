/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

/*
 * Submit a near-MDTS raw READ as one command.
 *
 * The 4040-block payload exceeds SPDK PCIe's embedded PRP capacity on the
 * affected geometry, but remains below the device-advertised 2 MiB MDTS.
 */

#include "sit_common.h"

#define MDTS_PRP_BLOCKS 4040U
#define MDTS_PRP_LBA 0

static int
sit_test_mdts_prp(struct sit_context *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair = NULL;
	struct spdk_nvme_cmd cmd = {};
	struct sit_io_ctx io = {};
	const struct spdk_nvme_ctrlr_data *cdata;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_cc_register cc;
	uint32_t nsid;
	uint32_t lba_size;
	uint32_t unused_nlb;
	uint64_t length;
	uint32_t blocks, offset;
	void *buffer = NULL;
	int rc;

	if (ctx->num_ctrlrs < 1) {
		fprintf(stderr, "No NVMe controllers found\n");
		return -ENODEV;
	}

	ctrlr = ctx->ctrlrs[0];
	rc = sit_get_ns_and_sizes(ctrlr, &ns, &nsid, &lba_size, &unused_nlb);
	if (rc != 0) {
		return rc;
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	blocks = ctx->test_2mib ? 4096 : MDTS_PRP_BLOCKS;
	length = (uint64_t)blocks * lba_size;
	printf("   [MDTS PRP] MDTS=%u, block size=%u, request=%u blocks (%"PRIu64" bytes)\n",
	       cdata->mdts, lba_size, blocks, length);
	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	cc = spdk_nvme_ctrlr_get_regs_cc(ctrlr);
	/* All-ones is SPDK's sentinel for a failed register read; treat it as an
	 * infrastructure error, not a geometry mismatch, so a dead register
	 * doesn't silently look like a SIT_SKIP. CAP must be checked as well as
	 * CC: a failed CAP read yields mpsmin == 0xF, which is exactly the
	 * "wrong page geometry" the skip below tests for. */
	if (cap.raw == UINT64_MAX) {
		fprintf(stderr, "CAP register read failed\n");
		return -EIO;
	}
	if (cc.raw == 0xFFFFFFFFU) {
		fprintf(stderr, "CC register read failed\n");
		return -EIO;
	}
	if (lba_size != 512 || cap.bits.mpsmin != 0 || cc.bits.mps != 0) {
		printf("   [MDTS PRP] SKIP: requires 512-byte LBAs and 4 KiB controller pages "
		       "(CAP.MPSMIN=0, CC.MPS=0)\n");
		return SIT_SKIP;
	}

	if (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
		printf("   [MDTS PRP] SKIP: controller uses SGLs rather than the PRP mapper\n");
		return SIT_SKIP;
	}
	if (!ctx->test_2mib && (cdata->mdts == 0 ||
				(cdata->mdts < 32 && length > (UINT64_C(4096) << cdata->mdts)))) {
		printf("   [MDTS PRP] SKIP: requires a finite MDTS covering the request\n");
		return SIT_SKIP;
	}
	if (length > spdk_nvme_ctrlr_get_max_xfer_size(ctrlr)) {
		fprintf(stderr, "MDTS PRP: transfer limit is too small; build with "
			"--enable-nvme-prp-pool\n");
		return -ENOTSUP;
	}

	/* The workload reads LBA 0..MDTS_PRP_BLOCKS-1; a smaller namespace would
	 * fail with an LBA-range error and misreport a regression, so skip it. */
	if (spdk_nvme_ns_get_num_sectors(ns) < blocks) {
		printf("   [MDTS PRP] SKIP: namespace has fewer than %u sectors\n",
		       blocks);
		return SIT_SKIP;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!qpair) {
		SPDK_ERRLOG("MDTS PRP I/O qpair allocation failed\n");
		return -ENOMEM;
	}

	buffer = spdk_dma_zmalloc(length + 512, 4096, NULL);
	if (!buffer) {
		rc = -ENOMEM;
		goto cleanup;
	}

	cmd.opc = SPDK_NVME_OPC_READ;
	cmd.nsid = nsid;
	cmd.cdw10 = (uint32_t)MDTS_PRP_LBA;
	cmd.cdw11 = 0;
	cmd.cdw12 = blocks - 1;

	for (offset = 0; offset <= (ctx->test_2mib ? 512U : 0U); offset += 512) {
		memset(&io, 0, sizeof(io));
		printf("   [MDTS PRP] Raw READ: bytes=%"PRIu64" buffer offset=%u\n",
		       length, offset);
		rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, &cmd, (uint8_t *)buffer + offset,
						(uint32_t)length, sit_io_cb, &io);
		if (rc != 0) {
			goto cleanup;
		}
		rc = sit_drain_qpair(qpair);
		if (rc == 0 && (!io.done || io.error || io.completions != 1)) {
			rc = -EIO;
		}
		if (rc != 0) {
			goto cleanup;
		}
		printf("   [MDTS PRP] PASS: one-command READ completed with external PRPs\n");
	}

cleanup:
	if (rc != 0) {
		fprintf(stderr, "MDTS PRP raw READ failed: %s\n", spdk_strerror(-rc));
	}
	if (qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}
	if (buffer) {
		spdk_dma_free(buffer);
	}
	return rc;
}

SIT_REGISTER_TEST_ENTRY(test_mdts_prp, sit_test_mdts_prp, NULL, NULL);
