/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "spdk/bdev.h"
#include "spdk/nvme_spec.h"

#include "vhost_internal.h"

#define MAX_IO_QUEUES 31
#define MAX_IOVS 64
#define MAX_NAMESPACE 8
#define MAX_QUEUE_ENTRIES_SUPPORTED 255
#define MAX_BATCH_IO 8

struct spdk_vhost_nvme_sq {
	uint16_t sqid;
	uint16_t size;
	uint16_t cqid;
	/* Admin command for delete_io_sq may comes any time */
	volatile bool valid;
	volatile struct spdk_nvme_cmd *sq_cmd;
	uint16_t sq_head;
	uint16_t sq_tail;

	uint32_t outstanding;
};

struct spdk_vhost_nvme_cq {
	uint8_t phase;
	uint16_t size;
	uint16_t cqid;
	bool valid;
	volatile struct spdk_nvme_cpl *cq_cqe;
	uint16_t cq_head;
	uint16_t last_signaled_cq_head;
	bool irq_enabled;
	int virq;
};

struct spdk_vhost_nvme_ns {
	struct spdk_bdev *bdev;
	uint32_t block_size;
	uint64_t capacity;
	uint16_t ns_id;
	uint32_t active_ns;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	struct spdk_nvme_ns_data nsdata;
};

struct spdk_vhost_nvme_task {
	struct spdk_vhost_nvme_dev *nvme;
	struct spdk_nvme_cmd cmd;
	uint16_t sqid;
	uint16_t cqid;
	bool status;

	/** array of iovecs to transfer. */
	struct iovec iovs[MAX_IOVS];

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;

	/* parent pointer. */
	struct spdk_vhost_nvme_task *parent;
	uint32_t num_children;
	STAILQ_ENTRY(spdk_vhost_nvme_task) stailq;
};

struct spdk_vhost_nvme_dev {
	struct spdk_vhost_dev vdev;

	uint32_t num_io_queues;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	struct spdk_nvme_ctrlr_data cdata;

	uint32_t num_sqs;
	uint32_t num_cqs;

	uint32_t io_completed;

	struct rte_vhost_memory *mem;

	uint32_t num_ns;
	struct spdk_vhost_nvme_ns ns[MAX_NAMESPACE];

	volatile uint32_t *dbbuf_dbs;
	volatile uint32_t *dbbuf_eis;
	struct spdk_vhost_nvme_sq sq_queue[MAX_IO_QUEUES + 1];
	struct spdk_vhost_nvme_cq cq_queue[MAX_IO_QUEUES + 1];

	TAILQ_ENTRY(spdk_vhost_nvme_dev) tailq;
	STAILQ_HEAD(, spdk_vhost_nvme_task) task_stailq;
	struct spdk_poller *requestq_poller;
};

static struct spdk_vhost_nvme_dev *
to_nvme_dev(struct spdk_vhost_dev *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->type != SPDK_VHOST_DEV_T_NVME) {
		SPDK_ERRLOG("Controller %s: expected NVMe controller (%d) but got %d\n",
			    ctrlr->name, SPDK_VHOST_DEV_T_NVME, ctrlr->type);
		return NULL;
	}

	return SPDK_CONTAINEROF(ctrlr, struct spdk_vhost_nvme_dev, vdev);
}

static TAILQ_HEAD(, spdk_vhost_nvme_dev) g_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_ctrlrs);

static inline unsigned int sq_offset(unsigned int qid, uint32_t db_stride)
{
	return qid * 2 * db_stride;
}

static inline unsigned int cq_offset(unsigned int qid, uint32_t db_stride)
{
	return (qid * 2 + 1) * db_stride;
}

static void
nvme_inc_cq_head(struct spdk_vhost_nvme_cq *cq)
{
	cq->cq_head++;
	if (cq->cq_head >= cq->size) {
		cq->cq_head = 0;
		cq->phase = !cq->phase;
	}
}

static void
nvme_inc_sq_head(struct spdk_vhost_nvme_sq *sq)
{
	sq->sq_head = (sq->sq_head + 1) % sq->size;
}

static int
spdk_nvme_map_prps(struct spdk_vhost_nvme_dev *nvme, struct spdk_nvme_cmd *cmd,
		   struct spdk_vhost_nvme_task *task, uint32_t block_size)
{
	uint32_t len, nlba;
	uint64_t prp1, prp2;
	uintptr_t vva;
	uint32_t i;
	/* TODO: assert cc.mps == 0 */
	uint32_t residue_len, mps = 4096;
	uint64_t *prp_list;

	/* number of logical blocks, 0 based value */
	nlba = (cmd->cdw12 & 0xffff) + 1;
	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;
	len = nlba * block_size;

	/* TODO: may take 2MiB aligned boundary into consideration */
	vva = (uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, prp1);
	if (vva == 0) {
		SPDK_ERRLOG("GPA to VVA failed\n");
		return -1;
	}
	task->iovs[0].iov_base = (void *)vva;
	/* PRP1 may started with unaligned page address */
	residue_len = mps - (prp1 % mps);
	residue_len = spdk_min(len, residue_len);
	task->iovs[0].iov_len = residue_len;

	len -= residue_len;

	if (len) {
		if (len <= mps) {
			/* 2 PRP used */
			task->iovcnt = 2;
			assert(prp2 != 0);
			vva = (uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, prp2);
			if (vva == 0) {
				return -1;
			}
			task->iovs[1].iov_base = (void *)vva;
			task->iovs[1].iov_len = len;
		} else {
			/* PRP list used */
			assert(prp2 != 0);
			vva = (uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, prp2);
			if (vva == 0) {
				return -1;
			}
			prp_list  = (uint64_t *)vva;
			i = 0;
			while (len != 0) {
				residue_len = spdk_min(len, mps);
				vva = (uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, prp_list[i]);
				if (vva == 0) {
					return -1;
				}
				task->iovs[i + 1].iov_base = (void *)vva;
				task->iovs[i + 1].iov_len = residue_len;
				len -= residue_len;
				i++;
			}
			task->iovcnt = i + 1;
		}
	} else {
		/* 1 PRP used */
		task->iovcnt = 1;
	}

	return 0;
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_nvme_task *task = cb_arg;
	struct spdk_vhost_nvme_dev *nvme = task->nvme;
	uint16_t cqid;
	volatile struct spdk_nvme_cpl *cqe;
	struct spdk_vhost_nvme_cq *cq;
	struct spdk_vhost_nvme_sq *sq;
	uint32_t cq_head;
	static uint32_t irq_coalescing = 0;

	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}

	cqid = task->cqid;
	cq = &nvme->cq_queue[cqid];
	cqe = &cq->cq_cqe[cq->cq_head];

	sq = &nvme->sq_queue[task->sqid];
	if (task->cmd.opc == SPDK_NVME_OPC_READ ||
	    task->cmd.opc == SPDK_NVME_OPC_WRITE) {
		sq->outstanding--;
	}

	cqe->sqid = task->sqid;
	cqe->sqhd = sq->sq_head;
	cqe->cid = task->cmd.cid;
	cqe->status.sct = 0;
	cqe->status.sc = 0;
	if (!success) {
		cqe->status.sc = SPDK_NVME_SCT_GENERIC;
		cqe->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		SPDK_ERRLOG("I/O error, sector %u\n", task->cmd.cdw10);
	}
	cqe->status.dnr = 1;
	cqe->status.p = cq->phase;
	nvme_inc_cq_head(cq);

	/* completion */
	cq_head = *(uint32_t volatile *)&nvme->dbbuf_dbs[cq_offset(cqid, 1)];
	if (cq_head != cq->last_signaled_cq_head) {
		cq->last_signaled_cq_head = (uint16_t)cq_head;
		/* MMIO Controll */
		spdk_wmb();
		*(uint32_t volatile *)&nvme->dbbuf_eis[cq_offset(cqid, 1)] = (uint32_t)(cq_head - 1);
	}

	if (cq->irq_enabled) {

		irq_coalescing++;
		/* Simple Interrupt Coalescing Here */
		if (sq->outstanding && (irq_coalescing % 2)) {

			if (task->cmd.opc != SPDK_NVME_OPC_READ ||
			    task->cmd.opc != SPDK_NVME_OPC_WRITE) {
				eventfd_write(cq->virq, (eventfd_t)1);
			}

		} else {
			eventfd_write(cq->virq, (eventfd_t)1);
		}
	}

	STAILQ_INSERT_HEAD(&nvme->task_stailq, task, stailq);
}

static void
blk_unmap_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_nvme_task *child = cb_arg;
	struct spdk_vhost_nvme_task *task = child->parent;
	struct spdk_vhost_nvme_dev *nvme = task->nvme;

	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}

	if (task) {
		task->num_children--;
		task->status |= success;
		if (!task->num_children) {
			blk_request_complete_cb(NULL, task->status, task);
		}
	}
	STAILQ_INSERT_HEAD(&nvme->task_stailq, child, stailq);
}

static struct spdk_vhost_nvme_ns *
spdk_vhost_nvme_get_ns_from_nsid(struct spdk_vhost_nvme_dev *dev, uint32_t nsid)
{
	assert(nsid > 0);
	assert(nsid <= dev->num_ns);

	return &dev->ns[nsid - 1];
}

static int
spdk_nvme_process_sq(struct spdk_vhost_nvme_dev *nvme, struct spdk_vhost_nvme_sq *sq,
		     struct spdk_vhost_nvme_task *task)
{
	struct spdk_vhost_nvme_task *child;
	struct spdk_nvme_cmd *cmd;
	struct spdk_vhost_nvme_ns *ns;
	int ret = -1;
	uint32_t nlba;
	uint64_t slba;
	uint32_t block_size;
	struct spdk_nvme_dsm_range *range;
	uint16_t i, num_ranges;
	uintptr_t vva;

	cmd = &task->cmd;
	task->nvme = nvme;

	ns = spdk_vhost_nvme_get_ns_from_nsid(nvme, cmd->nsid);
	assert(ns != NULL);
	block_size = ns->block_size;

	if (cmd->opc == SPDK_NVME_OPC_READ || cmd->opc == SPDK_NVME_OPC_WRITE) {
		assert(cmd->psdt == SPDK_NVME_PSDT_PRP);
		ret = spdk_nvme_map_prps(nvme, cmd, task, block_size);
		if (ret != 0) {
			SPDK_ERRLOG("nvme command map prps failed\n");
			return -1;
		}
	}

	task->status = 0;
	task->num_children = 0;
	task->cqid = sq->cqid;
	task->sqid = sq->sqid;

	if (!ns->active_ns) {
		blk_request_complete_cb(NULL, false, task);
	}

	/* valid only for Read/Write commands */
	nlba = (cmd->cdw12 & 0xffff) + 1;
	slba = cmd->cdw11;
	slba = (slba << 32) | cmd->cdw10;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
		sq->outstanding++;
		nvme->io_completed++;
		ret = spdk_bdev_readv(ns->bdev_desc, ns->bdev_io_channel,
				      task->iovs, task->iovcnt, slba * block_size,
				      nlba * block_size, blk_request_complete_cb, task);
		break;
	case SPDK_NVME_OPC_WRITE:
		sq->outstanding++;
		nvme->io_completed++;
		ret = spdk_bdev_writev(ns->bdev_desc, ns->bdev_io_channel,
				       task->iovs, task->iovcnt, slba * block_size,
				       nlba * block_size, blk_request_complete_cb, task);
		break;
	case SPDK_NVME_OPC_FLUSH:
		ret = spdk_bdev_flush(ns->bdev_desc, ns->bdev_io_channel,
				      0, ns->capacity,
				      blk_request_complete_cb, task);
		break;
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		vva = (uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, cmd->dptr.prp.prp1);
		if (vva == 0) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			ret = -1;
			break;
		}
		task->iovs[0].iov_base = (void *)vva;
		task->iovcnt = 1;
		range = (struct spdk_nvme_dsm_range *)task->iovs[0].iov_base;
		num_ranges = (cmd->cdw10 & 0xff) + 1;
		for (i = 0; i < num_ranges; i++) {
			child = STAILQ_FIRST(&nvme->task_stailq);
			if (child) {
				STAILQ_REMOVE_HEAD(&nvme->task_stailq, stailq);
			} else {
				SPDK_ERRLOG("No free task now\n");
				ret = -1;
				break;
			}
			task->num_children++;
			child->parent = task;
			ret = spdk_bdev_unmap(ns->bdev_desc, ns->bdev_io_channel,
					      range[i].starting_lba * block_size,
					      range[i].length * block_size,
					      blk_unmap_complete_cb, child);
			if (ret) {
				STAILQ_INSERT_TAIL(&nvme->task_stailq, child, stailq);
				break;
			}
		}
		break;
	default:
		ret = -1;
		break;
	}

	if (ret) {
		/* post error status to cqe */
		SPDK_ERRLOG("Error Submission For Command %u, ret %d\n", cmd->opc, ret);
		blk_request_complete_cb(NULL, false, task);
	}

	return ret;
}

static void
nvme_worker(void *arg)
{
	struct spdk_vhost_nvme_dev *nvme = (struct spdk_vhost_nvme_dev *)arg;
	struct spdk_vhost_nvme_sq *sq;
	struct spdk_vhost_nvme_task *task;
	struct spdk_nvme_cmd *cmd;
	uint32_t qid, dbbuf_sq;
	int ret;
	uint16_t count;

	if (!nvme->num_sqs) {
		return;
	}

	/* worker thread can't start before the admin doorbell
	 * buffer config command
	 */
	if (!nvme->dbbuf_dbs) {
		return;
	}

	/* Submission Queue */
	for (qid = 1; qid <= MAX_IO_QUEUES; qid++) {
		sq = &nvme->sq_queue[qid];
		if (!sq->valid) {
			continue;
		}

		dbbuf_sq = *(uint32_t volatile *)&nvme->dbbuf_dbs[sq_offset(qid, 1)];

		sq->sq_tail = (uint16_t)dbbuf_sq;
		count = 0;
		while (sq->valid && sq->sq_head != sq->sq_tail) {
			if (!sq->sq_cmd) {
				break;
			}
			task = STAILQ_FIRST(&nvme->task_stailq);
			if (task) {
				STAILQ_REMOVE_HEAD(&nvme->task_stailq, stailq);
			} else {
				SPDK_ERRLOG("No free task now\n");
				abort();
			}
			cmd = &task->cmd;

			memcpy(cmd, (void *)&sq->sq_cmd[sq->sq_head], sizeof(*cmd));
			nvme_inc_sq_head(sq);

			/* processing IO */
			ret = spdk_nvme_process_sq(nvme, sq, task);
			if (ret) {
				SPDK_ERRLOG("QID %u CID %u, SQ HEAD %u, DBBUF SQ TAIL %u\n", qid, cmd->cid, sq->sq_head,
					    sq->sq_tail);
			}

			/* MMIO Control */
			spdk_wmb();
			*(uint32_t volatile *)&nvme->dbbuf_eis[sq_offset(qid, 1)] = (uint32_t)(sq->sq_head - 1);

			/* Maximum batch I/Os to pick up at once */
			if (count++ == MAX_BATCH_IO) {
				break;
			}
		}
	}

}

static int
vhost_nvme_doorbell_buffer_config(struct spdk_vhost_nvme_dev *nvme,
				  struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint64_t dbs_dma_addr, eis_dma_addr;

	dbs_dma_addr = cmd->dptr.prp.prp1;
	eis_dma_addr = cmd->dptr.prp.prp2;

	assert(dbs_dma_addr % 4096 == 0);
	assert(eis_dma_addr % 4096 == 0);
	/* Guest Physical Address to Host Virtual Address */
	nvme->dbbuf_dbs = (void *)(uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, dbs_dma_addr);
	nvme->dbbuf_eis = (void *)(uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, eis_dma_addr);
	assert(nvme->dbbuf_dbs != NULL);
	assert(nvme->dbbuf_eis != NULL);
	/* zeroed the doorbell buffer memory */
	memset((void *)nvme->dbbuf_dbs, 0, sizeof((nvme->num_sqs + 1) * 8));
	memset((void *)nvme->dbbuf_eis, 0, sizeof((nvme->num_sqs + 1) * 8));

	cpl->status.sc = 0;
	cpl->status.sct = 0;
	return 0;
}

static int
vhost_nvme_create_io_sq(struct spdk_vhost_nvme_dev *nvme,
			struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint16_t qid, qsize, cqid;
	uint64_t dma_addr;
	struct spdk_vhost_nvme_sq *sq;

	/* physical contiguous */
	assert(cmd->cdw11 & 0x1);
	cqid = (cmd->cdw11 >> 16) & 0xffff;
	qid  = cmd->cdw10 & 0xffff;
	qsize = (cmd->cdw10 >> 16) & 0xffff;
	dma_addr = cmd->dptr.prp.prp1;
	assert(dma_addr != 0);
	assert(dma_addr % 4096 == 0);

	sq = &nvme->sq_queue[qid];
	sq->sqid = qid;
	sq->cqid = cqid;
	sq->size = qsize + 1;
	sq->sq_head = sq->sq_tail = 0;
	sq->sq_cmd = (void *)(uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, dma_addr);
	assert(sq->sq_cmd != 0);
	memset((void *)sq->sq_cmd, 0, sizeof(*cmd) * sq->size);
	nvme->num_sqs++;
	sq->valid = true;

	cpl->status.sc = 0;
	cpl->status.sct = 0;
	return 0;
}

static int
vhost_nvme_delete_io_sq(struct spdk_vhost_nvme_dev *nvme,
			struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint16_t qid;
	struct spdk_vhost_nvme_sq *sq;

	qid  = cmd->cdw10 & 0xffff;
	sq = &nvme->sq_queue[qid];
	/* TODO: Need to stop the poller of the queue first */
	nvme->num_sqs--;
	sq->valid = false;

	memset(sq, 0, sizeof(*sq));
	sq->sq_cmd = NULL;

	cpl->status.sc = 0;
	cpl->status.sct = 0;
	return 0;
}

static int
vhost_nvme_create_io_cq(struct spdk_vhost_nvme_dev *nvme,
			struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint16_t qsize, qid;
	uint64_t dma_addr;
	struct spdk_vhost_nvme_cq *cq;

	/* physical contiguous */
	assert(cmd->cdw11 & 0x1);
	qid  = cmd->cdw10 & 0xffff;
	qsize = (cmd->cdw10 >> 16) & 0xffff;
	dma_addr = cmd->dptr.prp.prp1;
	assert(dma_addr != 0);
	assert(dma_addr % 4096 == 0);

	cq = &nvme->cq_queue[qid];
	cq->cqid = qid;
	cq->size = qsize + 1;
	cq->phase = 1;
	cq->irq_enabled = (cmd->cdw11 >> 1) & 0x1;
	/* Setup virq through vhost messages */
	cq->virq = -1;
	cq->cq_head = 0;
	cq->last_signaled_cq_head = 0;
	cq->cq_cqe = (void *)(uintptr_t)rte_vhost_gpa_to_vva(nvme->mem, dma_addr);
	assert(cq->cq_cqe != 0);
	memset((void *)cq->cq_cqe, 0, sizeof(*cpl) * cq->size);
	nvme->num_cqs++;
	cq->valid = true;

	cpl->status.sc = 0;
	cpl->status.sct = 0;
	return 0;
}

static int
vhost_nvme_delete_io_cq(struct spdk_vhost_nvme_dev *nvme,
			struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint16_t qid;
	struct spdk_vhost_nvme_cq *cq;

	qid  = cmd->cdw10 & 0xffff;
	cq = &nvme->cq_queue[qid];
	nvme->num_cqs--;
	cq->valid = false;

	memset(cq, 0, sizeof(*cq));
	cq->cq_cqe = NULL;

	cpl->status.sc = 0;
	cpl->status.sct = 0;
	return 0;
}

static struct spdk_vhost_nvme_dev *
spdk_vhost_nvme_get_by_name(int vid)
{
	struct spdk_vhost_nvme_dev *nvme;

	TAILQ_FOREACH(nvme, &g_nvme_ctrlrs, tailq) {
		if (nvme->vdev.vid == vid) {
			return nvme;
		}
	}

	return NULL;
}

static const char *nvme_admin_str[256] = {
	[SPDK_NVME_OPC_IDENTIFY] = "Identify",
	[SPDK_NVME_OPC_CREATE_IO_CQ] = "Create IO CQ",
	[SPDK_NVME_OPC_CREATE_IO_SQ] = "Create IO SQ",
	[SPDK_NVME_OPC_DELETE_IO_CQ] = "Delete IO CQ",
	[SPDK_NVME_OPC_DELETE_IO_SQ] = "Delete IO SQ",
	[SPDK_NVME_OPC_GET_FEATURES] = "Get Features",
	[SPDK_NVME_OPC_SET_FEATURES] = "Set Features",
	[SPDK_NVME_OPC_ABORT] = "Abort",
	[SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG] = "Doorbell Buffer Config",
};

int
spdk_vhost_nvme_get_cap(int vid, uint64_t *cap)
{
	struct spdk_vhost_nvme_dev *nvme;

	nvme  = spdk_vhost_nvme_get_by_name(vid);
	assert(nvme != NULL);

	*cap = nvme->cap.raw;
	return 0;
}

int
spdk_vhost_nvme_admin_passthrough(int vid, void *cmd, void *cqe, void *buf)
{
	struct spdk_nvme_cmd *req = (struct spdk_nvme_cmd *)cmd;
	struct spdk_nvme_cpl *cpl = (struct spdk_nvme_cpl *)cqe;
	struct spdk_vhost_nvme_ns *ns;
	int ret = 0;
	struct spdk_vhost_nvme_dev *nvme;
	uint32_t cq_head, sq_tail;
	uint32_t dw0;

	nvme  = spdk_vhost_nvme_get_by_name(vid);
	assert(nvme != NULL);

	SPDK_NOTICELOG("Admin Command %s\n", nvme_admin_str[req->opc]);
	switch (req->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		if (req->cdw10 == SPDK_NVME_IDENTIFY_CTRLR) {
			memcpy(buf, &nvme->cdata, sizeof(struct spdk_nvme_ctrlr_data));

		} else if (req->cdw10 == SPDK_NVME_IDENTIFY_NS) {
			ns = spdk_vhost_nvme_get_ns_from_nsid(nvme, req->nsid);
			memcpy(buf, &ns->nsdata, sizeof(struct spdk_nvme_ns_data));
		}
		/* successfully */
		cpl->status.sc = 0;
		cpl->status.sct = 0;
		break;
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		ret = vhost_nvme_create_io_cq(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		ret = vhost_nvme_delete_io_cq(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		ret = vhost_nvme_create_io_sq(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_DELETE_IO_SQ:
		ret = vhost_nvme_delete_io_sq(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
	case SPDK_NVME_OPC_SET_FEATURES:
		if (req->cdw10 == SPDK_NVME_FEAT_NUMBER_OF_QUEUES) {
			cpl->status.sc = 0;
			cpl->status.sct = 0;
			dw0 = (nvme->num_io_queues - 1) | ((nvme->num_io_queues - 1) << 16);
			memcpy(buf, &dw0, 4);
		} else {
			cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			cpl->status.sct = SPDK_NVME_SCT_GENERIC;
		}
		break;
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		ret = vhost_nvme_doorbell_buffer_config(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_ABORT:
		/* TODO: */
		sq_tail = nvme->dbbuf_dbs[sq_offset(1, 1)] & 0xffffu;
		cq_head = nvme->dbbuf_dbs[cq_offset(1, 1)] & 0xffffu;
		SPDK_NOTICELOG("ABORT: IO Completed %u, CID %u, SQ_TAIL %u, CQ_HEAD %u\n",
			       nvme->io_completed, (req->cdw10 >> 16) & 0xffffu, sq_tail, cq_head);
		/* successfully */
		cpl->status.sc = 0;
		cpl->status.sct = 0;
		break;
	}

	if (ret) {
		SPDK_ERRLOG("Admin Passthrough Faild with %u\n", req->opc);
		return -1;
	}

	return 0;
}

int
spdk_vhost_nvme_set_cq_call(int vid, uint16_t qid, int fd)
{
	struct spdk_vhost_nvme_dev *nvme;
	struct spdk_vhost_nvme_cq *cq;

	nvme  = spdk_vhost_nvme_get_by_name(vid);
	assert(nvme != NULL);

	cq = &nvme->cq_queue[qid];
	if (cq->irq_enabled) {
		cq->virq = fd;
	} else {
		SPDK_ERRLOG("NVMe Qid %d Disabled IRQ\n", qid);
	}

	return 0;
}

static void
free_task_pool(struct spdk_vhost_nvme_dev *nvme)
{
	struct spdk_vhost_nvme_task *task;

	while (!STAILQ_EMPTY(&nvme->task_stailq)) {
		task = STAILQ_FIRST(&nvme->task_stailq);
		STAILQ_REMOVE_HEAD(&nvme->task_stailq, stailq);
		spdk_dma_free(task);
	}
}

static int
alloc_task_pool(struct spdk_vhost_nvme_dev *nvme)
{
	uint32_t entries, i;
	struct spdk_vhost_nvme_task *task;

	entries = nvme->num_io_queues * (MAX_QUEUE_ENTRIES_SUPPORTED + 1);

	for (i = 0; i < entries; i++) {
		task = spdk_dma_zmalloc(sizeof(struct spdk_vhost_nvme_task),
					SPDK_CACHE_LINE_SIZE, NULL);
		if (task == NULL) {
			SPDK_ERRLOG("Controller %s alloc task pool failed\n",
				    nvme->vdev.name);
			free_task_pool(nvme);
			return -1;
		}
		STAILQ_INSERT_TAIL(&nvme->task_stailq, task, stailq);
	}

	return 0;
}

/* new device means enable the
 * virtual NVMe controller
 */
static int
spdk_vhost_nvme_start_device(struct spdk_vhost_dev *vdev, void *event_ctx)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_ns *ns_dev;
	uint32_t i;

	spdk_vhost_dev_mem_register(vdev);
	nvme->mem = vdev->mem;

	if (alloc_task_pool(nvme)) {
		return -1;
	}

	SPDK_NOTICELOG("Start Device %u, Path %s, lcore %d\n", vdev->vid,
		       vdev->path, vdev->lcore);

	for (i = 0; i < nvme->num_ns; i++) {
		ns_dev = &nvme->ns[i];
		ns_dev->bdev_io_channel = spdk_bdev_get_io_channel(ns_dev->bdev_desc);
		assert(ns_dev->bdev_io_channel != NULL);
	}

	/* Start the NVMe Poller */
	nvme->requestq_poller = spdk_poller_register(nvme_worker, nvme, 0);

	spdk_vhost_dev_backend_event_done(event_ctx, 0);
	return 0;
}

static void
spdk_vhost_nvme_deactive_ns(struct spdk_vhost_nvme_ns *ns)
{
	ns->active_ns = 0;
	spdk_bdev_close(ns->bdev_desc);
	ns->bdev_desc = NULL;
	ns->bdev = NULL;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_nvme_ns *ns = remove_ctx;

	SPDK_NOTICELOG("Removing NS %u, Block Device %s\n",
		       ns->ns_id, spdk_bdev_get_name(ns->bdev));

	spdk_vhost_nvme_deactive_ns(ns);
}

struct spdk_vhost_dev_destroy_ctx {
	struct spdk_vhost_nvme_dev *bvdev;
	struct spdk_poller *poller;
	void *event_ctx;
};

static void
destroy_device_poller_cb(void *arg)
{
	struct spdk_vhost_dev_destroy_ctx *ctx = arg;
	struct spdk_vhost_nvme_dev *nvme = ctx->bvdev;
	struct spdk_vhost_nvme_dev *dev, *tmp;
	struct spdk_vhost_nvme_ns *ns_dev;
	uint32_t i;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "Destroy device poller callback\n");

	TAILQ_FOREACH_SAFE(dev, &g_nvme_ctrlrs, tailq, tmp) {
		if (dev == nvme) {
			for (i = 0; i < nvme->num_ns; i++) {
				ns_dev = &nvme->ns[i];
				if (ns_dev->bdev_io_channel) {
					spdk_put_io_channel(ns_dev->bdev_io_channel);
					ns_dev->bdev_io_channel = NULL;
				}
			}
			nvme->num_sqs = 0;
			nvme->num_cqs = 0;
			nvme->io_completed = 0;
			nvme->dbbuf_dbs = NULL;
			nvme->dbbuf_eis = NULL;
			spdk_vhost_dev_mem_unregister(&nvme->vdev);
		}
	}

	spdk_poller_unregister(&ctx->poller);
	spdk_vhost_dev_backend_event_done(ctx->event_ctx, 0);
	spdk_dma_free(ctx);
}

/* Disable NVMe controller
 */
static int
spdk_vhost_nvme_stop_device(struct spdk_vhost_dev *vdev, void *event_ctx)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_dev_destroy_ctx *destroy_ctx;

	free_task_pool(nvme);
	SPDK_NOTICELOG("Stopping Device %u, Path %s\n", vdev->vid, vdev->path);

	destroy_ctx = spdk_dma_zmalloc(sizeof(*destroy_ctx), SPDK_CACHE_LINE_SIZE, NULL);
	if (destroy_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc memory for destroying device.\n");
		goto err;
	}

	destroy_ctx->bvdev = nvme;
	destroy_ctx->event_ctx = event_ctx;

	spdk_poller_unregister(&nvme->requestq_poller);
	destroy_ctx->poller = spdk_poller_register(destroy_device_poller_cb, destroy_ctx, 1000);

	return 0;

err:
	spdk_vhost_dev_backend_event_done(event_ctx, -1);
	return -1;
}

static void
spdk_vhost_nvme_dump_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_ns *ns_dev;
	struct spdk_bdev *bdev;
	uint32_t i;

	spdk_json_write_name(w, "namespaces");
	spdk_json_write_object_begin(w);

	for (i = 0; i < nvme->num_ns; i++) {
		ns_dev = &nvme->ns[i];
		if (!ns_dev->active_ns) {
			continue;
		}
		bdev = ns_dev->bdev;

		spdk_json_write_name(w, "nsid");
		spdk_json_write_uint32(w, ns_dev->ns_id);

		spdk_json_write_name(w, "bdev");
		if (bdev) {
			spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		} else {
			spdk_json_write_null(w);
		}
	}

	spdk_json_write_object_end(w);
}

static const struct spdk_vhost_dev_backend spdk_vhost_nvme_device_backend = {
	.start_device = spdk_vhost_nvme_start_device,
	.stop_device = spdk_vhost_nvme_stop_device,
	.dump_config_json = spdk_vhost_nvme_dump_config_json,
	.vhost_remove_controller = spdk_vhost_nvme_dev_remove,
};

static int
spdk_vhost_nvme_ns_identify_update(struct spdk_vhost_nvme_dev *dev)
{
	struct spdk_nvme_ctrlr_data *cdata = &dev->cdata;
	struct spdk_nvme_ns_data *nsdata;
	uint64_t num_blocks;
	uint32_t i;

	/* Identify Namespace */
	cdata->nn = dev->num_ns;
	for (i = 0; i < dev->num_ns; i++) {
		nsdata = &dev->ns[i].nsdata;
		if (dev->ns[i].active_ns) {
			num_blocks = spdk_bdev_get_num_blocks(dev->ns[i].bdev);
			nsdata->nsze = num_blocks;
			/* ncap must be non-zero for active Namespace */
			nsdata->ncap = num_blocks;
			nsdata->nuse = num_blocks;
			nsdata->nlbaf = 0;
			nsdata->flbas.format = 0;
			nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(dev->ns[i].bdev));
			nsdata->noiob = spdk_bdev_get_optimal_io_boundary(dev->ns[i].bdev);
			dev->ns[i].block_size = spdk_bdev_get_block_size(dev->ns[i].bdev);
			dev->ns[i].capacity = num_blocks * dev->ns[i].block_size;
		} else {
			memset(nsdata, 0, sizeof(*nsdata));
		}
	}
	return 0;
}

static int
spdk_vhost_nvme_ctrlr_identify_update(struct spdk_vhost_nvme_dev *dev)
{
	struct spdk_nvme_ctrlr_data *cdata = &dev->cdata;
	char sn[20];

	/* Controller Capabilities */
	dev->cap.bits.cqr = 1;
	dev->cap.bits.to = 1;
	dev->cap.bits.dstrd = 0;
	dev->cap.bits.css_nvm = 1;
	dev->cap.bits.mpsmin = 0;
	dev->cap.bits.mpsmax = 0;
	dev->cap.bits.mqes = MAX_QUEUE_ENTRIES_SUPPORTED;

	/* Controller Configuration */
	dev->cc.bits.en = 0;

	/* Controller Status */
	dev->csts.bits.rdy = 0;

	/* Identify Controller */
	spdk_strcpy_pad(cdata->fr, "1708", sizeof(cdata->fr), ' ');
	cdata->vid = 0x8086;
	cdata->ssvid = 0x8086;
	spdk_strcpy_pad(cdata->mn, "SPDK Virtual NVMe Controller", sizeof(cdata->mn), ' ');
	snprintf(sn, sizeof(sn), "NVMe_%s", dev->vdev.name);
	spdk_strcpy_pad(cdata->sn, sn, sizeof(cdata->sn), ' ');
	cdata->ieee[0] = 0xe4;
	cdata->ieee[1] = 0xd2;
	cdata->ieee[2] = 0x5c;
	cdata->ver.bits.mjr = 1;
	cdata->ver.bits.mnr = 0;
	cdata->mdts = 5; /* 128 KiB */
	cdata->rab = 6;
	cdata->sqes.min = 6;
	cdata->sqes.max = 6;
	cdata->cqes.min = 4;
	cdata->cqes.max = 4;
	cdata->oncs.dsm = 1;
	/* Emualted NVMe controller */
	cdata->oacs.doorbell_buffer_config = 1;

	spdk_vhost_nvme_ns_identify_update(dev);

	return 0;
}

int
spdk_vhost_nvme_dev_construct(const char *name, const char *cpumask, uint32_t num_io_queues)
{
	struct spdk_vhost_nvme_dev *dev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_nvme_dev),
					  SPDK_CACHE_LINE_SIZE, NULL);
	int rc;

	if (dev == NULL) {
		return -ENOMEM;
	}

	if (num_io_queues < 1 || num_io_queues > MAX_IO_QUEUES) {
		return -EINVAL;
	}

	spdk_vhost_lock();
	rc = spdk_vhost_dev_construct(&dev->vdev, name, cpumask, SPDK_VHOST_DEV_T_NVME,
				      &spdk_vhost_nvme_device_backend);

	if (rc) {
		spdk_dma_free(dev);
	}

	dev->num_io_queues = num_io_queues;
	STAILQ_INIT(&dev->task_stailq);
	TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, dev, tailq);

	spdk_vhost_nvme_ctrlr_identify_update(dev);

	spdk_vhost_unlock();
	return rc;
}

int
spdk_vhost_nvme_dev_remove(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_dev *dev, *tmp;
	struct spdk_vhost_nvme_ns *ns;
	int rc;
	uint32_t i;

	if (nvme == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH_SAFE(dev, &g_nvme_ctrlrs, tailq, tmp) {
		if (dev == nvme) {
			TAILQ_REMOVE(&g_nvme_ctrlrs, dev, tailq);
			for (i = 0; i < nvme->num_ns; i++) {
				ns = &nvme->ns[i];
				if (ns->active_ns) {
					spdk_vhost_nvme_deactive_ns(ns);
				}
			}
		}
	}

	rc = spdk_vhost_dev_remove(vdev);
	if (rc != 0) {
		return rc;
	}

	spdk_dma_free(nvme);
	return 0;
}

int
spdk_vhost_nvme_dev_add_ns(struct spdk_vhost_dev *vdev, const char *bdev_name)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_ns *ns;
	struct spdk_bdev *bdev;
	int rc = -1;

	if (nvme->num_ns == MAX_NAMESPACE) {
		SPDK_ERRLOG("Can't support %d Namespaces\n", nvme->num_ns);
		return -1;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("could not find bdev %s\n", bdev_name);
		return -1;
	}

	ns = &nvme->ns[nvme->num_ns];
	rc = spdk_bdev_open(bdev, true, bdev_remove_cb, ns, &nvme->ns[nvme->num_ns].bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open bdev '%s', error=%d\n",
			    bdev_name, rc);
		return -1;
	}

	nvme->ns[nvme->num_ns].bdev = bdev;
	nvme->ns[nvme->num_ns].active_ns = 1;
	nvme->ns[nvme->num_ns].ns_id = nvme->num_ns + 1;
	nvme->num_ns++;

	spdk_vhost_nvme_ns_identify_update(nvme);

	return rc;
}

int
spdk_vhost_nvme_controller_construct(void)
{
	struct spdk_conf_section *sp;
	const char *name;
	const char *bdev_name;
	const char *cpumask;
	int rc, i = 0;
	struct spdk_vhost_dev *vdev;
	uint32_t ctrlr_num, io_queues;

	for (sp = spdk_conf_first_section(NULL); sp != NULL; sp = spdk_conf_next_section(sp)) {
		if (!spdk_conf_section_match_prefix(sp, "VhostNvme")) {
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostNvme%u", &ctrlr_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			return -1;
		}

		name = spdk_conf_section_get_val(sp, "Name");
		if (name == NULL) {
			SPDK_ERRLOG("VhostNvme%u: missing Name\n", ctrlr_num);
			return -1;
		}

		cpumask = spdk_conf_section_get_val(sp, "Cpumask");
		rc = spdk_conf_section_get_intval(sp, "NumberOfQueues");
		if (rc > 0) {
			io_queues = rc;
		} else {
			io_queues = 1;
		}

		rc = spdk_vhost_nvme_dev_construct(name, cpumask, io_queues);
		if (rc < 0) {
			SPDK_ERRLOG("VhostNvme%u: Construct failed\n", ctrlr_num);
			return -1;
		}

		vdev = spdk_vhost_dev_find(name);
		assert(vdev);

		for (i = 0; spdk_conf_section_get_nval(sp, "Namespace", i) != NULL; i++) {
			bdev_name = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
			if (!bdev_name) {
				SPDK_ERRLOG("namespace configuration missing bdev name\n");
				break;
			}
			rc = spdk_vhost_nvme_dev_add_ns(vdev, bdev_name);
			if (rc < 0) {
				SPDK_WARNLOG("VhostNvme%u: Construct Namespace with %s failed\n",
					     ctrlr_num, bdev_name);
				break;
			}
		}
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vhost_nvme", SPDK_LOG_VHOST_NVME)
