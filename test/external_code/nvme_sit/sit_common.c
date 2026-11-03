/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#include "sit_common.h"

/*
 * Look up the first active namespace on ctrlr and compute the I/O sizing
 * parameters required for SIT READ submissions.
 *
 * Populates *ns_out, *nsid_out, *lba_size_out, and *nlb_out (NLB is
 * 0-based: number of logical blocks minus 1 for SIT_IO_SIZE).
 * Returns 0 on success, -ENODEV if no active namespace is found,
 * or -EINVAL if SIT_IO_SIZE is not a multiple of the LBA size.
 */
int
sit_get_ns_and_sizes(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns **ns_out,
		     uint32_t *nsid_out, uint32_t *lba_size_out, uint32_t *nlb_out)
{
	struct spdk_nvme_ns	*ns;
	uint32_t		 nsid;
	uint32_t		 lba_size;

	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	if (nsid == 0) {
		fprintf(stderr, "SKIP: No active namespaces found\n");
		return SIT_SKIP;
	}
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		SPDK_ERRLOG("Failed to get namespace %u\n", nsid);
		return -ENODEV;
	}
	lba_size = spdk_nvme_ns_get_sector_size(ns);
	if (spdk_nvme_ns_get_md_size(ns) != 0) {
		printf("SKIP: raw READ tests require a namespace without metadata\n");
		return SIT_SKIP;
	}
	if (lba_size == 0 || SIT_IO_SIZE % lba_size != 0) {
		printf("SKIP: SIT_IO_SIZE=%d is not a multiple of lba_size=%u\n",
		       SIT_IO_SIZE, lba_size);
		return SIT_SKIP;
	}
	*nsid_out = nsid;
	*lba_size_out = lba_size;
	*nlb_out = (SIT_IO_SIZE / lba_size) - 1;	/* NLB is 0-based */
	*ns_out = ns;
	return 0;
}

/*
 * Drain all outstanding I/Os on qpair by polling process_completions()
 * until the outstanding count reaches zero or SIT_MAX_POLL_ITERS is hit.
 * Returns 0 on success, -ETIMEDOUT if the queue does not drain in time,
 * or a negative errno forwarded from process_completions().
 */
int
sit_drain_qpair(struct spdk_nvme_qpair *qpair)
{
	int iters = 0;
	int completions;

	while (spdk_nvme_qpair_get_num_outstanding_reqs(qpair) > 0 &&
	       iters++ < SIT_MAX_POLL_ITERS) {
		completions = spdk_nvme_qpair_process_completions(qpair, 0);
		if (completions < 0) {
			SPDK_WARNLOG("process_completions failed (rc=%d)\n",
				     completions);
			return completions;
		}
		if (completions == 0) {
			spdk_delay_us(10);
		}
	}

	if (spdk_nvme_qpair_get_num_outstanding_reqs(qpair) > 0) {
		SPDK_WARNLOG("%u I/Os still outstanding after drain timeout\n",
			     spdk_nvme_qpair_get_num_outstanding_reqs(qpair));
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * Poll admin completions until *done is set true by the completion callback,
 * or SIT_MAX_POLL_ITERS is reached.  Mirrors sit_drain_qpair() for the admin
 * path which uses spdk_nvme_ctrlr_process_admin_completions() instead of
 * spdk_nvme_qpair_process_completions().
 * Returns 0 on success, -ETIMEDOUT if the limit is reached without completion,
 * or a negative errno forwarded from process_admin_completions().
 */
int
sit_drain_admin(struct spdk_nvme_ctrlr *ctrlr, bool *done)
{
	int iters = 0;
	int completions;

	while (!*done && iters++ < SIT_MAX_POLL_ITERS) {
		completions = spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		if (completions < 0) {
			SPDK_WARNLOG("process_admin_completions failed (rc=%d)\n",
				     completions);
			return completions;
		}
		if (completions == 0) {
			spdk_delay_us(10);
		}
	}

	if (!*done) {
		SPDK_WARNLOG("Admin completion timed out after %d iters\n",
			     SIT_MAX_POLL_ITERS);
		return -ETIMEDOUT;
	}

	return 0;
}

void
sit_io_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct sit_io_ctx *ctx = arg;

	ctx->done = true;
	ctx->completions++;
	ctx->error |= spdk_nvme_cpl_is_error(cpl);
	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("SIT completion failed: SCT=%u SC=%u\n",
			    cpl->status.sct, cpl->status.sc);
	}
}
