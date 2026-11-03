/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#include "sit_common.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"

#ifdef SPDK_CONFIG_NVME_N_MINUS_1_TRACKERS

#define TRACKER_MAX_DEPTH 1024
#define TRACKER_BURSTS 4
#define TRACKER_REFILLS 64

struct tracker_workload;

struct tracker_io {
	struct tracker_workload *workload;
	void *buf;
	uint32_t completions;
};

struct tracker_workload {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_cmd cmd;
	struct tracker_io *ios;
	uint32_t lba_size;
	uint32_t target;
	int error;
};

static void tracker_io_cb(void *arg, const struct spdk_nvme_cpl *cpl);

static int
tracker_submit(struct tracker_io *io)
{
	struct tracker_workload *w = io->workload;
	int rc;

	rc = spdk_nvme_ctrlr_cmd_io_raw(w->ctrlr, w->qpair, &w->cmd, io->buf,
					w->lba_size, tracker_io_cb, io);
	if (rc != 0) {
		SPDK_ERRLOG("Tracker capacity READ submission failed: rc=%d\n", rc);
		w->error = rc;
	}
	return rc;
}

static void
tracker_io_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct tracker_io *io = arg;
	struct tracker_workload *w = io->workload;

	io->completions++;
	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Tracker capacity READ failed: SCT=%u SC=%u\n",
			    cpl->status.sct, cpl->status.sc);
		w->error = -EIO;
	}
	if (io->completions > w->target) {
		SPDK_ERRLOG("Tracker capacity READ completed more than expected\n");
		w->error = -EIO;
	}
	if (w->error == 0 && io->completions < w->target) {
		tracker_submit(io);
	}
}

static int
tracker_run_workload(struct tracker_workload *w, uint32_t count, uint32_t target)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(w->qpair);
	struct nvme_tracker *tr;
	uint32_t i, tracked = 0;
	int rc;

	w->error = 0;
	w->target = target;
	for (i = 0; i < count; i++) {
		w->ios[i].completions = 0;
	}
	for (i = 0; i < count; i++) {
		if (tracker_submit(&w->ios[i]) != 0) {
			break;
		}
	}

	/* No polling has occurred: accepted-but-queued requests must not count. */
	TAILQ_FOREACH(tr, &pqpair->outstanding_tr, tq_list) {
		tracked++;
	}
	if (tracked != count || w->qpair->queue_depth != count ||
	    !STAILQ_EMPTY(&w->qpair->queued_req)) {
		SPDK_ERRLOG("Expected %u hardware-tracked READs, found %u "
			    "(queue_depth=%u, software queue %s)\n",
			    count, tracked, w->qpair->queue_depth,
			    STAILQ_EMPTY(&w->qpair->queued_req) ? "empty" : "not empty");
		w->error = -EIO;
	}

	rc = sit_drain_qpair(w->qpair);
	if (rc != 0) {
		SPDK_ERRLOG("Tracker capacity drain failed: depth=%u target=%u rc=%d "
			    "SQ head/tail=%u/%u CQ head/phase=%u/%u queued=%s\n",
			    pqpair->num_entries, target, rc, pqpair->sq_head, pqpair->sq_tail,
			    pqpair->cq_head, pqpair->flags.phase,
			    STAILQ_EMPTY(&w->qpair->queued_req) ? "no" : "yes");
		if (w->error == 0) {
			w->error = rc;
		}
	}
	for (i = 0; i < count; i++) {
		if (w->ios[i].completions != target) {
			SPDK_ERRLOG("READ stream %u: expected %u completions, got %u\n",
				    i, target, w->ios[i].completions);
			if (w->error == 0) {
				w->error = -EIO;
			}
			break;
		}
	}
	return w->error;
}

static int
tracker_run_depth(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		  uint32_t lba_size, uint32_t depth)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct tracker_workload w = {};
	struct nvme_pcie_qpair *pqpair;
	uint32_t count = depth - 1;
	uint32_t i;
	int result = 0, rc;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = depth;
	opts.io_queue_requests = depth;
	w.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	if (w.qpair == NULL) {
		SPDK_ERRLOG("Failed to allocate tracker capacity qpair at depth %u\n", depth);
		return -ENOMEM;
	}
	pqpair = nvme_pcie_qpair(w.qpair);
	if (pqpair->num_entries != depth || nvme_pcie_qpair_num_trackers(pqpair) != count) {
		SPDK_ERRLOG("Requested depth %u, effective depth %u, trackers %u\n",
			    depth, pqpair->num_entries, nvme_pcie_qpair_num_trackers(pqpair));
		result = -EINVAL;
		goto out;
	}
	w.ctrlr = ctrlr;
	w.lba_size = lba_size;
	w.cmd.opc = SPDK_NVME_OPC_READ;
	w.cmd.nsid = nsid;
	w.ios = calloc(count, sizeof(*w.ios));
	if (w.ios == NULL) {
		SPDK_ERRLOG("Failed to allocate tracker capacity I/O contexts\n");
		result = -ENOMEM;
		goto out;
	}
	for (i = 0; i < count; i++) {
		w.ios[i].workload = &w;
		w.ios[i].buf = spdk_dma_zmalloc(lba_size, SIT_IO_ALIGN, NULL);
		if (w.ios[i].buf == NULL) {
			SPDK_ERRLOG("Failed to allocate tracker capacity buffer %u\n", i);
			result = -ENOMEM;
			goto out;
		}
	}

	for (i = 0; i < TRACKER_BURSTS; i++) {
		result = tracker_run_workload(&w, count, 1);
		if (result != 0) {
			goto out;
		}
	}
	printf("Tracker capacity: depth=%u sustained %u concurrent READs "
	       "in %u bursts\n", depth, count, TRACKER_BURSTS);

	result = tracker_run_workload(&w, count, TRACKER_REFILLS);
	if (result == 0) {
		printf("Tracker capacity: depth=%u completed %u callback-resubmitted READs\n",
		       depth, count * TRACKER_REFILLS);
	}

out:
	/* Stop refill callbacks before deletion aborts any remaining requests. */
	w.error = -ECANCELED;
	rc = spdk_nvme_ctrlr_free_io_qpair(w.qpair);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to free tracker capacity qpair: rc=%d\n", rc);
		/* Keep buffers and callback contexts alive until the process exits. */
		exit(EXIT_FAILURE);
	}
	if (w.ios != NULL) {
		for (i = 0; i < count; i++) {
			spdk_dma_free(w.ios[i].buf);
		}
		free(w.ios);
	}
	return result;
}

static int
tracker_run_controller(struct spdk_nvme_ctrlr *ctrlr)
{
	static const uint32_t depths[] = { 64, 512, TRACKER_MAX_DEPTH };
	struct spdk_nvme_ns *ns;
	union spdk_nvme_cap_register cap;
	uint32_t nsid, lba_size, nlb, ran = 0;
	size_t i;
	int rc;

	rc = sit_get_ns_and_sizes(ctrlr, &ns, &nsid, &lba_size, &nlb);
	if (rc != 0) {
		return rc;
	}
	if (spdk_nvme_ns_get_num_sectors(ns) == 0) {
		SPDK_ERRLOG("Tracker capacity test requires a nonempty namespace\n");
		return -EINVAL;
	}
	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	for (i = 0; i < SPDK_COUNTOF(depths); i++) {
		if (depths[i] > (uint32_t)cap.bits.mqes + 1) {
			printf("Tracker capacity SKIP: depth=%u exceeds CAP.MQES+1=%u\n",
			       depths[i], (uint32_t)cap.bits.mqes + 1);
			continue;
		}
		rc = tracker_run_depth(ctrlr, nsid, lba_size, depths[i]);
		if (rc != 0) {
			return rc;
		}
		ran++;
	}
	return ran == 0 ? SIT_SKIP : 0;
}

#endif

static int
run_tracker_capacity(struct sit_context *ctx)
{
#ifdef SPDK_CONFIG_NVME_N_MINUS_1_TRACKERS
	int i, rc, ran = 0;

	for (i = 0; i < ctx->num_ctrlrs; i++) {
		rc = tracker_run_controller(ctx->ctrlrs[i]);
		if (rc == SIT_SKIP) {
			continue;
		}
		if (rc != 0) {
			return rc;
		}
		ran++;
	}
	return ran == 0 ? SIT_SKIP : 0;
#else
	printf("Tracker capacity SKIP: build with --enable-nvme-n-minus-1-trackers\n");
	return SIT_SKIP;
#endif
}

SIT_REGISTER_TEST_ENTRY(test_tracker_capacity, run_tracker_capacity, NULL, NULL)
