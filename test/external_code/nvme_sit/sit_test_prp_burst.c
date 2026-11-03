/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#include "sit_common.h"

#define PRP_PAGE_SIZE 4096
#define PRP_NUM_QPAIRS 3

struct prp_io {
	struct spdk_nvme_qpair *qpair;
	void *buffer;
	uint64_t *prp_list;
	struct sit_io_ctx io;
	uint32_t length;
};

static int
prp_build_prps(struct prp_io *io, struct spdk_nvme_cmd *cmd)
{
	uint32_t pages = io->length / PRP_PAGE_SIZE;
	uint64_t phys;
	uint32_t i;

	for (i = 0; i < pages; i++) {
		phys = spdk_vtophys((uint8_t *)io->buffer + i * PRP_PAGE_SIZE, NULL);
		if (phys == SPDK_VTOPHYS_ERROR || (phys & (PRP_PAGE_SIZE - 1)) != 0) {
			SPDK_ERRLOG("PRP burst data-page DMA translation failed\n");
			return -EFAULT;
		}
		if (i == 0) {
			cmd->dptr.prp.prp1 = phys;
		} else if (pages == 2) {
			cmd->dptr.prp.prp2 = phys;
		} else {
			io->prp_list[i - 1] = phys;
		}
	}

	if (pages > 2) {
		phys = spdk_vtophys(io->prp_list, NULL);
		if (phys == SPDK_VTOPHYS_ERROR || (phys & (PRP_PAGE_SIZE - 1)) != 0) {
			SPDK_ERRLOG("PRP burst PRP-list DMA translation failed\n");
			return -EFAULT;
		}
		cmd->dptr.prp.prp2 = phys;
	}
	return 0;
}

static int
prp_check_registers(struct spdk_nvme_ctrlr *ctrlr, uint64_t cap, uint32_t cc)
{
	union spdk_nvme_csts_register csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);

	if (spdk_nvme_ctrlr_get_regs_cap(ctrlr).raw != cap ||
	    spdk_nvme_ctrlr_get_regs_cc(ctrlr).raw != cc ||
	    csts.raw == UINT32_MAX || !csts.bits.rdy || csts.bits.cfs) {
		SPDK_ERRLOG("PRP burst controller registers changed or controller is not ready\n");
		return -EIO;
	}
	return 0;
}

static int
sit_test_prp_burst(struct sit_context *ctx)
{
	struct prp_io ios[PRP_NUM_QPAIRS] = {};
	const uint32_t lengths[PRP_NUM_QPAIRS] = {4096, 8192, 16384};
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_health_information_page *health = NULL;
	struct sit_io_ctx health_io = {};
	struct spdk_nvme_cmd cmd;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_cc_register cc;
	uint32_t nsid, lba_size, unused_nlb;
	bool health_pending = false;
	uint32_t phase, i;
	int rc, drain_rc;

	if (ctx->num_ctrlrs == 0) {
		SPDK_ERRLOG("PRP burst requires an attached NVMe controller\n");
		return -ENODEV;
	}
	ctrlr = ctx->ctrlrs[0];
	rc = sit_get_ns_and_sizes(ctrlr, &ns, &nsid, &lba_size, &unused_nlb);
	if (rc != 0) {
		return rc;
	}

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	cc = spdk_nvme_ctrlr_get_regs_cc(ctrlr);
	if (cap.raw == UINT64_MAX || cc.raw == UINT32_MAX) {
		SPDK_ERRLOG("PRP burst failed to read controller registers\n");
		return -EIO;
	}
	if (cc.bits.mps != 0 ||
	    (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_SGL_SUPPORTED) ||
	    spdk_nvme_ns_get_num_sectors(ns) < lengths[2] / lba_size ||
	    spdk_nvme_ctrlr_get_max_xfer_size(ctrlr) < lengths[2]) {
		printf("PRP burst SKIP: requires 4 KiB pages, PRP mapping, and 16 KiB READ support\n");
		return SIT_SKIP;
	}

	health = spdk_dma_zmalloc(sizeof(*health), PRP_PAGE_SIZE, NULL);
	if (health == NULL) {
		rc = -ENOMEM;
		goto cleanup;
	}
	for (i = 0; i < PRP_NUM_QPAIRS; i++) {
		ios[i].length = lengths[i];
		ios[i].qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
		ios[i].buffer = spdk_dma_zmalloc(lengths[i], PRP_PAGE_SIZE, NULL);
		if (lengths[i] > 2 * PRP_PAGE_SIZE) {
			ios[i].prp_list = spdk_dma_zmalloc(PRP_PAGE_SIZE, PRP_PAGE_SIZE, NULL);
		}
		if (ios[i].qpair == NULL || ios[i].buffer == NULL ||
		    (lengths[i] > 2 * PRP_PAGE_SIZE && ios[i].prp_list == NULL)) {
			rc = -ENOMEM;
			goto cleanup;
		}
	}

	for (phase = 0; phase < 2; phase++) {
		printf("PRP burst: simultaneous 4/8/16 KiB READs using %s\n",
		       phase == 0 ? "driver-built PRPs" : "prebuilt raw PRPs");
		for (i = 0; i < PRP_NUM_QPAIRS; i++) {
			memset(&ios[i].io, 0, sizeof(ios[i].io));
			memset(&cmd, 0, sizeof(cmd));
			cmd.opc = SPDK_NVME_OPC_READ;
			cmd.nsid = nsid;
			cmd.cdw12 = lengths[i] / lba_size - 1;
			if (phase == 0) {
				rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, ios[i].qpair, &cmd,
								ios[i].buffer, lengths[i],
								sit_io_cb, &ios[i].io);
			} else {
				rc = prp_build_prps(&ios[i], &cmd);
				if (rc == 0) {
					rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, ios[i].qpair,
										&cmd, NULL, 0, NULL, sit_io_cb, &ios[i].io);
				}
			}
			if (rc != 0) {
				goto cleanup;
			}
		}

		memset(&health_io, 0, sizeof(health_io));
		rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
						      SPDK_NVME_GLOBAL_NS_TAG, health, sizeof(*health), 0,
						      sit_io_cb, &health_io);
		if (rc != 0) {
			goto cleanup;
		}
		health_pending = true;
		rc = prp_check_registers(ctrlr, cap.raw, cc.raw);
		if (rc != 0) {
			goto cleanup;
		}
		rc = sit_drain_admin(ctrlr, &health_io.done);
		if (rc != 0) {
			goto cleanup;
		}
		health_pending = false;
		if (health_io.error || health_io.completions != 1) {
			rc = -EIO;
			goto cleanup;
		}

		for (i = 0; i < PRP_NUM_QPAIRS; i++) {
			rc = sit_drain_qpair(ios[i].qpair);
			if (rc != 0) {
				goto cleanup;
			}
			if (!ios[i].io.done || ios[i].io.error || ios[i].io.completions != 1) {
				rc = -EIO;
				goto cleanup;
			}
			rc = prp_check_registers(ctrlr, cap.raw, cc.raw);
			if (rc != 0) {
				goto cleanup;
			}
		}
	}

cleanup:
	for (i = 0; i < PRP_NUM_QPAIRS; i++) {
		if (ios[i].qpair != NULL) {
			drain_rc = sit_drain_qpair(ios[i].qpair);
			if (drain_rc != 0 && rc == 0) {
				rc = drain_rc;
			}
			spdk_nvme_ctrlr_free_io_qpair(ios[i].qpair);
		}
		spdk_dma_free(ios[i].prp_list);
		spdk_dma_free(ios[i].buffer);
	}
	if (health_pending && !health_io.done) {
		/* Detach before freeing a buffer still owned by an admin command. */
		sit_env_fini();
	}
	spdk_dma_free(health);
	if (rc != 0) {
		SPDK_ERRLOG("PRP burst PRP burst failed: %s\n", spdk_strerror(-rc));
	}
	return rc;
}

SIT_REGISTER_TEST_ENTRY(test_prp_burst, sit_test_prp_burst, NULL, NULL);
