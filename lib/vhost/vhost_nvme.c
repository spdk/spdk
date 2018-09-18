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
#include "spdk/thread.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "spdk/bdev.h"
#include "spdk/version.h"
#include "spdk/nvme_spec.h"
#include "spdk/likely.h"

#include "vhost_internal.h"

#define MAX_IO_QUEUES 31
#define MAX_IOVS 64
#define MAX_NAMESPACE 8
#define MAX_QUEUE_ENTRIES_SUPPORTED 256
#define MAX_BATCH_IO 8

struct spdk_vhost_nvme_sq {
	uint16_t sqid;
	uint16_t size;
	uint16_t cqid;
	bool valid;
	struct spdk_nvme_cmd *sq_cmd;
	uint16_t sq_head;
	uint16_t sq_tail;
};

struct spdk_vhost_nvme_cq {
	uint8_t phase;
	uint16_t size;
	uint16_t cqid;
	bool valid;
	volatile struct spdk_nvme_cpl *cq_cqe;
	uint16_t cq_head;
	uint16_t guest_signaled_cq_head;
	uint32_t need_signaled_cnt;
	STAILQ_HEAD(, spdk_vhost_nvme_task) cq_full_waited_tasks;
	bool irq_enabled;
	int virq;
};

struct spdk_vhost_nvme_ns {
	struct spdk_bdev *bdev;
	uint32_t block_size;
	uint64_t capacity;
	uint32_t nsid;
	uint32_t active_ns;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	struct spdk_nvme_ns_data nsdata;
};

struct spdk_vhost_nvme_task {
	struct spdk_nvme_cmd cmd;
	struct spdk_vhost_nvme_dev *nvme;
	uint16_t sqid;
	uint16_t cqid;

	/** array of iovecs to transfer. */
	struct iovec iovs[MAX_IOVS];

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	struct spdk_vhost_nvme_sq *sq;
	struct spdk_vhost_nvme_ns *ns;

	/* parent pointer. */
	struct spdk_vhost_nvme_task *parent;
	uint8_t dnr;
	uint8_t sct;
	uint8_t sc;
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

	uint32_t num_ns;
	struct spdk_vhost_nvme_ns ns[MAX_NAMESPACE];

	volatile uint32_t *dbbuf_dbs;
	volatile uint32_t *dbbuf_eis;
	struct spdk_vhost_nvme_sq sq_queue[MAX_IO_QUEUES + 1];
	struct spdk_vhost_nvme_cq cq_queue[MAX_IO_QUEUES + 1];

	TAILQ_ENTRY(spdk_vhost_nvme_dev) tailq;
	STAILQ_HEAD(, spdk_vhost_nvme_task) free_tasks;
	struct spdk_poller *requestq_poller;
	struct spdk_vhost_dev_destroy_ctx destroy_ctx;
};

static const struct spdk_vhost_dev_backend spdk_vhost_nvme_device_backend;

/*
 * Report the SPDK version as the firmware revision.
 * SPDK_VERSION_STRING won't fit into FR (only 8 bytes), so try to fit the most important parts.
 */
#define FW_VERSION SPDK_VERSION_MAJOR_STRING SPDK_VERSION_MINOR_STRING SPDK_VERSION_PATCH_STRING

static int
spdk_nvme_process_sq(struct spdk_vhost_nvme_dev *nvme, struct spdk_vhost_nvme_sq *sq,
		     struct spdk_vhost_nvme_task *task);

static struct spdk_vhost_nvme_dev *
to_nvme_dev(struct spdk_vhost_dev *vdev)
{
	if (vdev->backend != &spdk_vhost_nvme_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-nvme device\n", vdev->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(vdev, struct spdk_vhost_nvme_dev, vdev);
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

static bool
nvme_cq_is_full(struct spdk_vhost_nvme_cq *cq)
{
	return ((cq->cq_head + 1) % cq->size == cq->guest_signaled_cq_head);
}

static void
nvme_inc_sq_head(struct spdk_vhost_nvme_sq *sq)
{
	sq->sq_head = (sq->sq_head + 1) % sq->size;
}

static struct spdk_vhost_nvme_sq *
spdk_vhost_nvme_get_sq_from_qid(struct spdk_vhost_nvme_dev *dev, uint16_t qid)
{
	if (spdk_unlikely(!qid || qid > MAX_IO_QUEUES)) {
		return NULL;
	}

	return &dev->sq_queue[qid];
}

static struct spdk_vhost_nvme_cq *
spdk_vhost_nvme_get_cq_from_qid(struct spdk_vhost_nvme_dev *dev, uint16_t qid)
{
	if (spdk_unlikely(!qid || qid > MAX_IO_QUEUES)) {
		return NULL;
	}

	return &dev->cq_queue[qid];
}

static int
spdk_nvme_map_prps(struct spdk_vhost_nvme_dev *nvme, struct spdk_nvme_cmd *cmd,
		   struct spdk_vhost_nvme_task *task, uint32_t len)
{
	uint64_t prp1, prp2;
	void *vva;
	uint32_t i;
	uint32_t residue_len, nents, mps = 4096;
	uint64_t *prp_list;

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	/* PRP1 may started with unaligned page address */
	residue_len = mps - (prp1 % mps);
	residue_len = spdk_min(len, residue_len);

	vva = spdk_vhost_gpa_to_vva(&nvme->vdev, prp1, residue_len);
	if (spdk_unlikely(vva == NULL)) {
		SPDK_ERRLOG("GPA to VVA failed\n");
		return -1;
	}
	task->iovs[0].iov_base = vva;
	task->iovs[0].iov_len = residue_len;
	len -= residue_len;

	if (len) {
		if (spdk_unlikely(prp2 == 0)) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "Invalid PRP2=0 in command\n");
			return -1;
		}

		if (len <= mps) {
			/* 2 PRP used */
			task->iovcnt = 2;
			vva = spdk_vhost_gpa_to_vva(&nvme->vdev, prp2, len);
			if (spdk_unlikely(vva == NULL)) {
				return -1;
			}
			task->iovs[1].iov_base = vva;
			task->iovs[1].iov_len = len;
		} else {
			/* PRP list used */
			nents = (len + mps - 1) / mps;
			vva = spdk_vhost_gpa_to_vva(&nvme->vdev, prp2, nents * sizeof(*prp_list));
			if (spdk_unlikely(vva == NULL)) {
				return -1;
			}
			prp_list = vva;
			i = 0;
			while (len != 0) {
				residue_len = spdk_min(len, mps);
				vva = spdk_vhost_gpa_to_vva(&nvme->vdev, prp_list[i], residue_len);
				if (spdk_unlikely(vva == NULL)) {
					return -1;
				}
				task->iovs[i + 1].iov_base = vva;
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
spdk_nvme_cq_signal_fd(struct spdk_vhost_nvme_dev *nvme)
{
	struct spdk_vhost_nvme_cq *cq;
	uint32_t qid, cq_head;

	assert(nvme != NULL);

	for (qid = 1; qid <= MAX_IO_QUEUES; qid++) {
		cq = spdk_vhost_nvme_get_cq_from_qid(nvme, qid);
		if (!cq || !cq->valid) {
			continue;
		}

		cq_head = nvme->dbbuf_dbs[cq_offset(qid, 1)];
		if (cq->irq_enabled && cq->need_signaled_cnt && (cq->cq_head != cq_head)) {
			eventfd_write(cq->virq, (eventfd_t)1);
			cq->need_signaled_cnt = 0;
		}
	}
}

static void
spdk_vhost_nvme_task_complete(struct spdk_vhost_nvme_task *task)
{
	struct spdk_vhost_nvme_dev *nvme = task->nvme;
	struct spdk_nvme_cpl cqe = {0};
	struct spdk_vhost_nvme_cq *cq;
	struct spdk_vhost_nvme_sq *sq;
	struct spdk_nvme_cmd *cmd = &task->cmd;
	uint16_t cqid = task->cqid;
	uint16_t sqid = task->sqid;

	cq = spdk_vhost_nvme_get_cq_from_qid(nvme, cqid);
	sq = spdk_vhost_nvme_get_sq_from_qid(nvme, sqid);
	if (spdk_unlikely(!cq || !sq)) {
		return;
	}

	cq->guest_signaled_cq_head = nvme->dbbuf_dbs[cq_offset(cqid, 1)];
	if (spdk_unlikely(nvme_cq_is_full(cq))) {
		STAILQ_INSERT_TAIL(&cq->cq_full_waited_tasks, task, stailq);
		return;
	}

	cqe.sqid = sqid;
	cqe.sqhd = sq->sq_head;
	cqe.cid = cmd->cid;
	cqe.status.dnr = task->dnr;
	cqe.status.sct = task->sct;
	cqe.status.sc = task->sc;
	cqe.status.p = !cq->phase;
	cq->cq_cqe[cq->cq_head] = cqe;
	spdk_smp_wmb();
	cq->cq_cqe[cq->cq_head].status.p = cq->phase;

	nvme_inc_cq_head(cq);
	cq->need_signaled_cnt++;

	/* MMIO Controll */
	nvme->dbbuf_eis[cq_offset(cqid, 1)] = (uint32_t)(cq->guest_signaled_cq_head - 1);

	STAILQ_INSERT_TAIL(&nvme->free_tasks, task, stailq);
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_nvme_task *task = cb_arg;
	struct spdk_nvme_cmd *cmd = &task->cmd;
	int sc, sct;

	assert(bdev_io != NULL);

	spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
	spdk_bdev_free_io(bdev_io);

	task->dnr = !success;
	task->sct = sct;
	task->sc = sc;

	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("I/O error, sector %u\n", cmd->cdw10);
	}

	spdk_vhost_nvme_task_complete(task);
}

static void
blk_unmap_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_nvme_task *child = cb_arg;
	struct spdk_vhost_nvme_task *task = child->parent;
	struct spdk_vhost_nvme_dev *nvme = task->nvme;
	int sct, sc;

	assert(bdev_io != NULL);

	task->num_children--;
	if (!success) {
		task->dnr = 1;
		spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
		task->sct = sct;
		task->sc = sc;
	}

	spdk_bdev_free_io(bdev_io);

	if (!task->num_children) {
		spdk_vhost_nvme_task_complete(task);
	}

	STAILQ_INSERT_TAIL(&nvme->free_tasks, child, stailq);
}

static struct spdk_vhost_nvme_ns *
spdk_vhost_nvme_get_ns_from_nsid(struct spdk_vhost_nvme_dev *dev, uint32_t nsid)
{
	if (spdk_unlikely(!nsid || nsid > dev->num_ns)) {
		return NULL;
	}

	return &dev->ns[nsid - 1];
}

static void
vhost_nvme_resubmit_task(void *arg)
{
	struct spdk_vhost_nvme_task *task = (struct spdk_vhost_nvme_task *)arg;
	int rc;

	rc = spdk_nvme_process_sq(task->nvme, task->sq, task);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "vhost_nvme: task resubmit failed, rc = %d.\n", rc);
	}
}

static int
vhost_nvme_queue_task(struct spdk_vhost_nvme_task *task)
{
	int rc;

	task->bdev_io_wait.bdev = task->ns->bdev;
	task->bdev_io_wait.cb_fn = vhost_nvme_resubmit_task;
	task->bdev_io_wait.cb_arg = task;

	rc = spdk_bdev_queue_io_wait(task->ns->bdev, task->ns->bdev_io_channel, &task->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vhost_nvme_queue_task, rc=%d.\n", rc);
		task->dnr = 1;
		task->sct = SPDK_NVME_SCT_GENERIC;
		task->sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		spdk_vhost_nvme_task_complete(task);
	}

	return rc;
}

static int
spdk_nvme_process_sq(struct spdk_vhost_nvme_dev *nvme, struct spdk_vhost_nvme_sq *sq,
		     struct spdk_vhost_nvme_task *task)
{
	struct spdk_vhost_nvme_task *child;
	struct spdk_nvme_cmd *cmd = &task->cmd;
	struct spdk_vhost_nvme_ns *ns;
	int ret = -1;
	uint32_t len, nlba, block_size;
	uint64_t slba;
	struct spdk_nvme_dsm_range *range;
	uint16_t i, num_ranges = 0;

	task->nvme = nvme;
	task->dnr = 0;
	task->sct = 0;
	task->sc = 0;

	ns = spdk_vhost_nvme_get_ns_from_nsid(nvme, cmd->nsid);
	if (spdk_unlikely(!ns)) {
		task->dnr = 1;
		task->sct = SPDK_NVME_SCT_GENERIC;
		task->sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		spdk_vhost_nvme_task_complete(task);
		return -1;
	}

	block_size = ns->block_size;
	task->num_children = 0;
	task->cqid = sq->cqid;
	task->sqid = sq->sqid;

	task->ns = ns;

	if (spdk_unlikely(!ns->active_ns)) {
		task->dnr = 1;
		task->sct = SPDK_NVME_SCT_GENERIC;
		task->sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		spdk_vhost_nvme_task_complete(task);
		return -1;
	}

	/* valid only for Read/Write commands */
	nlba = (cmd->cdw12 & 0xffff) + 1;
	slba = cmd->cdw11;
	slba = (slba << 32) | cmd->cdw10;

	if (cmd->opc == SPDK_NVME_OPC_READ || cmd->opc == SPDK_NVME_OPC_WRITE ||
	    cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		if (cmd->psdt != SPDK_NVME_PSDT_PRP) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "Invalid PSDT %u%ub in command\n",
				      cmd->psdt >> 1, cmd->psdt & 1u);
			task->dnr = 1;
			task->sct = SPDK_NVME_SCT_GENERIC;
			task->sc = SPDK_NVME_SC_INVALID_FIELD;
			spdk_vhost_nvme_task_complete(task);
			return -1;
		}

		if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
			num_ranges = (cmd->cdw10 & 0xff) + 1;
			len = num_ranges * sizeof(struct spdk_nvme_dsm_range);
		} else {
			len = nlba * block_size;
		}

		ret = spdk_nvme_map_prps(nvme, cmd, task, len);
		if (spdk_unlikely(ret != 0)) {
			SPDK_ERRLOG("nvme command map prps failed\n");
			task->dnr = 1;
			task->sct = SPDK_NVME_SCT_GENERIC;
			task->sc = SPDK_NVME_SC_INVALID_FIELD;
			spdk_vhost_nvme_task_complete(task);
			return -1;
		}
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
		ret = spdk_bdev_readv(ns->bdev_desc, ns->bdev_io_channel,
				      task->iovs, task->iovcnt, slba * block_size,
				      nlba * block_size, blk_request_complete_cb, task);
		break;
	case SPDK_NVME_OPC_WRITE:
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
		range = (struct spdk_nvme_dsm_range *)task->iovs[0].iov_base;
		for (i = 0; i < num_ranges; i++) {
			if (!STAILQ_EMPTY(&nvme->free_tasks)) {
				child = STAILQ_FIRST(&nvme->free_tasks);
				STAILQ_REMOVE_HEAD(&nvme->free_tasks, stailq);
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
				STAILQ_INSERT_TAIL(&nvme->free_tasks, child, stailq);
				break;
			}
		}
		break;
	default:
		ret = -1;
		break;
	}

	if (spdk_unlikely(ret)) {
		if (ret == -ENOMEM) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "No memory, start to queue io.\n");
			task->sq = sq;
			ret = vhost_nvme_queue_task(task);
		} else {
			/* post error status to cqe */
			SPDK_ERRLOG("Error Submission For Command %u, ret %d\n", cmd->opc, ret);
			task->dnr = 1;
			task->sct = SPDK_NVME_SCT_GENERIC;
			task->sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			spdk_vhost_nvme_task_complete(task);
		}
	}

	return ret;
}

static int
nvme_worker(void *arg)
{
	struct spdk_vhost_nvme_dev *nvme = (struct spdk_vhost_nvme_dev *)arg;
	struct spdk_vhost_nvme_sq *sq;
	struct spdk_vhost_nvme_cq *cq;
	struct spdk_vhost_nvme_task *task;
	uint32_t qid, dbbuf_sq;
	int ret;
	int count = -1;

	if (spdk_unlikely(!nvme->num_sqs)) {
		return -1;
	}

	/* worker thread can't start before the admin doorbell
	 * buffer config command
	 */
	if (spdk_unlikely(!nvme->dbbuf_dbs)) {
		return -1;
	}

	for (qid = 1; qid <= MAX_IO_QUEUES; qid++) {

		sq = spdk_vhost_nvme_get_sq_from_qid(nvme, qid);
		if (!sq->valid) {
			continue;
		}
		cq = spdk_vhost_nvme_get_cq_from_qid(nvme, sq->cqid);
		if (spdk_unlikely(!cq)) {
			return -1;
		}
		cq->guest_signaled_cq_head = nvme->dbbuf_dbs[cq_offset(sq->cqid, 1)];
		if (spdk_unlikely(!STAILQ_EMPTY(&cq->cq_full_waited_tasks) &&
				  !nvme_cq_is_full(cq))) {
			task = STAILQ_FIRST(&cq->cq_full_waited_tasks);
			STAILQ_REMOVE_HEAD(&cq->cq_full_waited_tasks, stailq);
			spdk_vhost_nvme_task_complete(task);
		}

		dbbuf_sq = nvme->dbbuf_dbs[sq_offset(qid, 1)];
		sq->sq_tail = (uint16_t)dbbuf_sq;
		count = 0;

		while (sq->sq_head != sq->sq_tail) {
			if (spdk_unlikely(!sq->sq_cmd)) {
				break;
			}
			if (spdk_likely(!STAILQ_EMPTY(&nvme->free_tasks))) {
				task = STAILQ_FIRST(&nvme->free_tasks);
				STAILQ_REMOVE_HEAD(&nvme->free_tasks, stailq);
			} else {
				return -1;
			}

			task->cmd = sq->sq_cmd[sq->sq_head];
			nvme_inc_sq_head(sq);

			/* processing IO */
			ret = spdk_nvme_process_sq(nvme, sq, task);
			if (spdk_unlikely(ret)) {
				SPDK_ERRLOG("QID %u CID %u, SQ HEAD %u, DBBUF SQ TAIL %u\n", qid, task->cmd.cid, sq->sq_head,
					    sq->sq_tail);
			}

			/* MMIO Control */
			nvme->dbbuf_eis[sq_offset(qid, 1)] = (uint32_t)(sq->sq_head - 1);

			/* Maximum batch I/Os to pick up at once */
			if (count++ == MAX_BATCH_IO) {
				break;
			}
		}
	}

	/* Completion Queue */
	spdk_nvme_cq_signal_fd(nvme);

	return count;
}

static int
vhost_nvme_doorbell_buffer_config(struct spdk_vhost_nvme_dev *nvme,
				  struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *cpl)
{
	uint64_t dbs_dma_addr, eis_dma_addr;

	dbs_dma_addr = cmd->dptr.prp.prp1;
	eis_dma_addr = cmd->dptr.prp.prp2;

	if ((dbs_dma_addr % 4096) || (eis_dma_addr % 4096)) {
		return -1;
	}
	/* Guest Physical Address to Host Virtual Address */
	nvme->dbbuf_dbs = spdk_vhost_gpa_to_vva(&nvme->vdev, dbs_dma_addr, 4096);
	nvme->dbbuf_eis = spdk_vhost_gpa_to_vva(&nvme->vdev, eis_dma_addr, 4096);
	if (!nvme->dbbuf_dbs || !nvme->dbbuf_eis) {
		return -1;
	}
	/* zeroed the doorbell buffer memory */
	memset((void *)nvme->dbbuf_dbs, 0, 4096);
	memset((void *)nvme->dbbuf_eis, 0, 4096);

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
	uint64_t requested_len;
	struct spdk_vhost_nvme_cq *cq;
	struct spdk_vhost_nvme_sq *sq;

	/* physical contiguous */
	if (!(cmd->cdw11 & 0x1)) {
		return -1;
	}

	cqid = (cmd->cdw11 >> 16) & 0xffff;
	qid = cmd->cdw10 & 0xffff;
	qsize = (cmd->cdw10 >> 16) & 0xffff;
	dma_addr = cmd->dptr.prp.prp1;
	if (!dma_addr || dma_addr % 4096) {
		return -1;
	}

	sq = spdk_vhost_nvme_get_sq_from_qid(nvme, qid);
	cq = spdk_vhost_nvme_get_cq_from_qid(nvme, cqid);
	if (!sq || !cq) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "User requested invalid QID %u or CQID %u\n",
			      qid, cqid);
		cpl->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		cpl->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return -1;
	}

	sq->sqid = qid;
	sq->cqid = cqid;
	sq->size = qsize + 1;
	sq->sq_head = sq->sq_tail = 0;
	requested_len = sizeof(struct spdk_nvme_cmd) * sq->size;
	sq->sq_cmd = spdk_vhost_gpa_to_vva(&nvme->vdev, dma_addr, requested_len);
	if (!sq->sq_cmd) {
		return -1;
	}
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

	qid = cmd->cdw10 & 0xffff;
	sq = spdk_vhost_nvme_get_sq_from_qid(nvme, qid);
	if (!sq) {
		return -1;
	}

	/* We didn't see scenarios when deleting submission
	 * queue while I/O is running against the submisson
	 * queue for now, otherwise, we must ensure the poller
	 * will not run with this submission queue.
	 */
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
	uint64_t requested_len;

	/* physical contiguous */
	if (!(cmd->cdw11 & 0x1)) {
		return -1;
	}

	qid = cmd->cdw10 & 0xffff;
	qsize = (cmd->cdw10 >> 16) & 0xffff;
	dma_addr = cmd->dptr.prp.prp1;
	if (!dma_addr || dma_addr % 4096) {
		return -1;
	}

	cq = spdk_vhost_nvme_get_cq_from_qid(nvme, qid);
	if (!cq) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "User requested invalid QID %u\n", qid);
		cpl->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		cpl->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return -1;
	}
	cq->cqid = qid;
	cq->size = qsize + 1;
	cq->phase = 1;
	cq->irq_enabled = (cmd->cdw11 >> 1) & 0x1;
	/* Setup virq through vhost messages */
	cq->virq = -1;
	cq->cq_head = 0;
	cq->guest_signaled_cq_head = 0;
	cq->need_signaled_cnt = 0;
	requested_len = sizeof(struct spdk_nvme_cpl) * cq->size;
	cq->cq_cqe = spdk_vhost_gpa_to_vva(&nvme->vdev, dma_addr, requested_len);
	if (!cq->cq_cqe) {
		return -1;
	}
	nvme->num_cqs++;
	cq->valid = true;
	STAILQ_INIT(&cq->cq_full_waited_tasks);

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

	qid = cmd->cdw10 & 0xffff;
	cq = spdk_vhost_nvme_get_cq_from_qid(nvme, qid);
	if (!cq) {
		return -1;
	}
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

int
spdk_vhost_nvme_get_cap(int vid, uint64_t *cap)
{
	struct spdk_vhost_nvme_dev *nvme;

	nvme = spdk_vhost_nvme_get_by_name(vid);
	if (!nvme) {
		return -1;
	}

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

	nvme = spdk_vhost_nvme_get_by_name(vid);
	if (!nvme) {
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_NVME, "Admin Command Opcode %u\n", req->opc);
	switch (req->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		if (req->cdw10 == SPDK_NVME_IDENTIFY_CTRLR) {
			memcpy(buf, &nvme->cdata, sizeof(struct spdk_nvme_ctrlr_data));

		} else if (req->cdw10 == SPDK_NVME_IDENTIFY_NS) {
			ns = spdk_vhost_nvme_get_ns_from_nsid(nvme, req->nsid);
			if (!ns) {
				cpl->status.sc = SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE;
				cpl->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
				break;
			}
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
			cpl->cdw0 = (nvme->num_io_queues - 1) | ((nvme->num_io_queues - 1) << 16);
		} else {
			cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			cpl->status.sct = SPDK_NVME_SCT_GENERIC;
		}
		break;
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		ret = vhost_nvme_doorbell_buffer_config(nvme, req, cpl);
		break;
	case SPDK_NVME_OPC_ABORT:
		sq_tail = nvme->dbbuf_dbs[sq_offset(1, 1)] & 0xffffu;
		cq_head = nvme->dbbuf_dbs[cq_offset(1, 1)] & 0xffffu;
		SPDK_NOTICELOG("ABORT: CID %u, SQ_TAIL %u, CQ_HEAD %u\n",
			       (req->cdw10 >> 16) & 0xffffu, sq_tail, cq_head);
		/* TODO: ABORT failed fow now */
		cpl->cdw0 = 1;
		cpl->status.sc = 0;
		cpl->status.sct = 0;
		break;
	}

	if (ret) {
		SPDK_ERRLOG("Admin Passthrough Faild with %u\n", req->opc);
	}

	return 0;
}

int
spdk_vhost_nvme_set_cq_call(int vid, uint16_t qid, int fd)
{
	struct spdk_vhost_nvme_dev *nvme;
	struct spdk_vhost_nvme_cq *cq;

	nvme = spdk_vhost_nvme_get_by_name(vid);
	if (!nvme) {
		return -1;
	}

	cq = spdk_vhost_nvme_get_cq_from_qid(nvme, qid);
	if (!cq) {
		return -1;
	}
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

	while (!STAILQ_EMPTY(&nvme->free_tasks)) {
		task = STAILQ_FIRST(&nvme->free_tasks);
		STAILQ_REMOVE_HEAD(&nvme->free_tasks, stailq);
		spdk_dma_free(task);
	}
}

static int
alloc_task_pool(struct spdk_vhost_nvme_dev *nvme)
{
	uint32_t entries, i;
	struct spdk_vhost_nvme_task *task;

	entries = nvme->num_io_queues * MAX_QUEUE_ENTRIES_SUPPORTED;

	for (i = 0; i < entries; i++) {
		task = spdk_dma_zmalloc(sizeof(struct spdk_vhost_nvme_task),
					SPDK_CACHE_LINE_SIZE, NULL);
		if (task == NULL) {
			SPDK_ERRLOG("Controller %s alloc task pool failed\n",
				    nvme->vdev.name);
			free_task_pool(nvme);
			return -1;
		}
		STAILQ_INSERT_TAIL(&nvme->free_tasks, task, stailq);
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

	if (nvme == NULL) {
		return -1;
	}

	if (alloc_task_pool(nvme)) {
		return -1;
	}

	SPDK_NOTICELOG("Start Device %u, Path %s, lcore %d\n", vdev->vid,
		       vdev->path, vdev->lcore);

	for (i = 0; i < nvme->num_ns; i++) {
		ns_dev = &nvme->ns[i];
		ns_dev->bdev_io_channel = spdk_bdev_get_io_channel(ns_dev->bdev_desc);
		if (!ns_dev->bdev_io_channel) {
			return -1;
		}
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
		       ns->nsid, spdk_bdev_get_name(ns->bdev));

	spdk_vhost_nvme_deactive_ns(ns);
}

static int
destroy_device_poller_cb(void *arg)
{
	struct spdk_vhost_nvme_dev *nvme = arg;
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
			nvme->dbbuf_dbs = NULL;
			nvme->dbbuf_eis = NULL;
		}
	}

	spdk_poller_unregister(&nvme->destroy_ctx.poller);
	spdk_vhost_dev_backend_event_done(nvme->destroy_ctx.event_ctx, 0);

	return -1;
}

/* Disable NVMe controller
 */
static int
spdk_vhost_nvme_stop_device(struct spdk_vhost_dev *vdev, void *event_ctx)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);

	if (nvme == NULL) {
		return -1;
	}

	free_task_pool(nvme);
	SPDK_NOTICELOG("Stopping Device %u, Path %s\n", vdev->vid, vdev->path);

	nvme->destroy_ctx.event_ctx = event_ctx;
	spdk_poller_unregister(&nvme->requestq_poller);
	nvme->destroy_ctx.poller = spdk_poller_register(destroy_device_poller_cb, nvme, 1000);

	return 0;
}

static void
spdk_vhost_nvme_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_ns *ns_dev;
	uint32_t i;

	if (nvme == NULL) {
		return;
	}

	spdk_json_write_named_array_begin(w, "namespaces");

	for (i = 0; i < nvme->num_ns; i++) {
		ns_dev = &nvme->ns[i];
		if (!ns_dev->active_ns) {
			continue;
		}

		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "nsid", ns_dev->nsid);
		spdk_json_write_named_string(w, "bdev",  spdk_bdev_get_name(ns_dev->bdev));
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static void
spdk_vhost_nvme_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_nvme_dev *nvme = to_nvme_dev(vdev);
	struct spdk_vhost_nvme_ns *ns_dev;
	uint32_t i;

	if (nvme == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_vhost_nvme_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", nvme->vdev.name);
	spdk_json_write_named_uint32(w, "io_queues", nvme->num_io_queues);
	spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(nvme->vdev.cpumask));
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	for (i = 0; i < nvme->num_ns; i++) {
		ns_dev = &nvme->ns[i];
		if (!ns_dev->active_ns) {
			continue;
		}

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "add_vhost_nvme_ns");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr", nvme->vdev.name);
		spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(ns_dev->bdev));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

static const struct spdk_vhost_dev_backend spdk_vhost_nvme_device_backend = {
	.start_device = spdk_vhost_nvme_start_device,
	.stop_device = spdk_vhost_nvme_stop_device,
	.dump_info_json = spdk_vhost_nvme_dump_info_json,
	.write_config_json = spdk_vhost_nvme_write_config_json,
	.remove_device = spdk_vhost_nvme_dev_remove,
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
	dev->cap.bits.css = SPDK_NVME_CAP_CSS_NVM;
	dev->cap.bits.mpsmin = 0;
	dev->cap.bits.mpsmax = 0;
	/* MQES is 0 based value */
	dev->cap.bits.mqes = MAX_QUEUE_ENTRIES_SUPPORTED - 1;

	/* Controller Configuration */
	dev->cc.bits.en = 0;

	/* Controller Status */
	dev->csts.bits.rdy = 0;

	/* Identify Controller */
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' ');
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
	/* Emulated NVMe controller */
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
		spdk_dma_free(dev);
		return -EINVAL;
	}

	spdk_vhost_lock();
	rc = spdk_vhost_dev_register(&dev->vdev, name, cpumask,
				     &spdk_vhost_nvme_device_backend);

	if (rc) {
		spdk_dma_free(dev);
		spdk_vhost_unlock();
		return rc;
	}

	dev->num_io_queues = num_io_queues;
	STAILQ_INIT(&dev->free_tasks);
	TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, dev, tailq);

	spdk_vhost_nvme_ctrlr_identify_update(dev);

	SPDK_NOTICELOG("Controller %s: Constructed\n", name);
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

	rc = spdk_vhost_dev_unregister(vdev);
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

	if (nvme == NULL) {
		return -ENODEV;
	}

	if (nvme->num_ns == MAX_NAMESPACE) {
		SPDK_ERRLOG("Can't support %d Namespaces\n", nvme->num_ns);
		return -ENOSPC;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("could not find bdev %s\n", bdev_name);
		return -ENODEV;
	}

	ns = &nvme->ns[nvme->num_ns];
	rc = spdk_bdev_open(bdev, true, bdev_remove_cb, ns, &nvme->ns[nvme->num_ns].bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open bdev '%s', error=%d\n",
			    bdev_name, rc);
		return rc;
	}

	nvme->ns[nvme->num_ns].bdev = bdev;
	nvme->ns[nvme->num_ns].active_ns = 1;
	nvme->ns[nvme->num_ns].nsid = nvme->num_ns + 1;
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
		if (!vdev) {
			return -1;
		}

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
