/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/dma.h"
#include "spdk/json.h"
#include "spdk/util.h"

#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma.h"
#include "spdk_internal/accel_module.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/sgl.h"
#include "accel_mlx5.h"

#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#define ACCEL_MLX5_QP_SIZE (256u)
#define ACCEL_MLX5_NUM_REQUESTS (2048u - 1)

#define ACCEL_MLX5_MAX_SGE (16u)
#define ACCEL_MLX5_MAX_WC (64u)
#define ACCEL_MLX5_ALLOC_REQS_IN_BATCH (16u)

struct accel_mlx5_io_channel;
struct accel_mlx5_task;

struct accel_mlx5_crypto_dev_ctx {
	struct spdk_mempool *requests_pool;
	struct ibv_context *context;
	struct ibv_pd *pd;
	TAILQ_ENTRY(accel_mlx5_crypto_dev_ctx) link;
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_crypto_dev_ctx *crypto_ctxs;
	uint32_t num_crypto_ctxs;
	struct accel_mlx5_attr attr;
	bool enabled;
};

enum accel_mlx5_wrid_type {
	ACCEL_MLX5_WRID_MKEY,
	ACCEL_MLX5_WRID_WRITE,
};

struct accel_mlx5_wrid {
	uint8_t wrid;
};

struct accel_mlx5_req {
	struct accel_mlx5_task *task;
	struct mlx5dv_mkey *mkey;
	struct ibv_sge src_sg[ACCEL_MLX5_MAX_SGE];
	struct ibv_sge dst_sg[ACCEL_MLX5_MAX_SGE];
	uint16_t src_sg_count;
	uint16_t dst_sg_count;
	struct accel_mlx5_wrid mkey_wrid;
	struct accel_mlx5_wrid write_wrid;
	TAILQ_ENTRY(accel_mlx5_req) link;
};

struct accel_mlx5_task {
	struct spdk_accel_task base;
	struct accel_mlx5_dev *dev;
	TAILQ_HEAD(, accel_mlx5_req) reqs;
	uint32_t num_reqs;
	uint32_t num_completed_reqs;
	uint32_t num_submitted_reqs;
	int rc;
	struct spdk_iov_sgl src;
	struct spdk_iov_sgl dst;
	struct accel_mlx5_req *cur_req;
	/* If set, memory data will be encrypted during TX and wire data will be
	  decrypted during RX.
	  If not set, memory data will be decrypted during TX and wire data will
	  be encrypted during RX. */
	bool encrypt_on_tx;
	bool inplace;
	TAILQ_ENTRY(accel_mlx5_task) link;
};

struct accel_mlx5_qp {
	struct ibv_qp *qp;
	struct ibv_qp_ex *qpex;
	struct mlx5dv_qp_ex *mqpx; /* more qpairs to the god of qpairs */
	struct ibv_cq *cq;
	struct accel_mlx5_io_channel *ch;
	bool wr_started;
	uint16_t num_reqs;
	uint16_t num_free_reqs;
};

struct accel_mlx5_dev {
	struct accel_mlx5_qp *qp;
	struct ibv_cq *cq;
	struct spdk_rdma_mem_map *mmap;
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	uint32_t reqs_submitted;
	uint32_t max_reqs;
	/* Pending tasks waiting for requests resources */
	TAILQ_HEAD(, accel_mlx5_task) nomem;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	TAILQ_HEAD(, accel_mlx5_task) in_hw;
	/* tasks between wr_start and wr_complete */
	TAILQ_HEAD(, accel_mlx5_task) before_submit;
	TAILQ_ENTRY(accel_mlx5_dev) link;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint32_t num_devs;
	/* Index in \b devs to be used for crypto in round-robin way */
	uint32_t dev_idx;
};

struct accel_mlx5_req_init_ctx {
	struct ibv_pd *pd;
	int rc;
};

static struct accel_mlx5_module g_accel_mlx5;

static int
mlx5_qp_init_2_rts(struct ibv_qp *qp, uint32_t dest_qp_num)
{
	struct ibv_qp_attr cur_attr = {}, attr = {};
	struct ibv_qp_init_attr init_attr = {};
	struct ibv_port_attr port_attr = {};
	union ibv_gid gid = {};
	int rc;
	uint8_t port;
	int attr_mask = IBV_QP_PKEY_INDEX |
			IBV_QP_PORT |
			IBV_QP_ACCESS_FLAGS |
			IBV_QP_PATH_MTU |
			IBV_QP_AV |
			IBV_QP_DEST_QPN |
			IBV_QP_RQ_PSN |
			IBV_QP_MAX_DEST_RD_ATOMIC |
			IBV_QP_MIN_RNR_TIMER |
			IBV_QP_TIMEOUT |
			IBV_QP_RETRY_CNT |
			IBV_QP_RNR_RETRY |
			IBV_QP_SQ_PSN |
			IBV_QP_MAX_QP_RD_ATOMIC;

	if (!qp) {
		return -EINVAL;
	}

	rc = ibv_query_qp(qp, &cur_attr, attr_mask, &init_attr);
	if (rc) {
		SPDK_ERRLOG("Failed to query qp %p %u\n", qp, qp->qp_num);
		return rc;
	}

	port = cur_attr.port_num;
	rc = ibv_query_port(qp->context, port, &port_attr);
	if (rc) {
		SPDK_ERRLOG("Failed to query port num %d\n", port);
		return rc;
	}

	if (port_attr.state != IBV_PORT_ARMED && port_attr.state != IBV_PORT_ACTIVE) {
		SPDK_ERRLOG("Wrong port %d state %d\n", port, port_attr.state);
		return -ENETUNREACH;
	}

	rc = ibv_query_gid(qp->context, port, 0, &gid);
	if (rc) {
		SPDK_ERRLOG("Failed to get GID on port %d, rc %d\n", port, rc);
		return rc;
	}

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = cur_attr.pkey_index;
	attr.port_num = cur_attr.port_num;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

	rc = ibv_modify_qp(qp, &attr, attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to modify qp %p %u to INIT state, rc %d\n", qp, qp->qp_num, rc);
		return rc;
	}

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = cur_attr.path_mtu;
	/* dest_qp_num == qp_num - self loopback connection */
	attr.dest_qp_num = dest_qp_num;
	attr.rq_psn = cur_attr.rq_psn;
	attr.max_dest_rd_atomic = cur_attr.max_dest_rd_atomic;
	attr.min_rnr_timer = cur_attr.min_rnr_timer;
	attr.ah_attr = cur_attr.ah_attr;
	attr.ah_attr.dlid = port_attr.lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;

	if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		/* Ethernet requires to set GRH */
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = gid;
	} else {
		attr.ah_attr.is_global = 0;
	}

	assert(attr.ah_attr.port_num == port);

	attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		    IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | IBV_QP_AV;

	rc = ibv_modify_qp(qp, &attr, attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to modify qp %p %u to RTR state, rc %d\n", qp, qp->qp_num, rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = cur_attr.timeout;
	attr.retry_cnt = cur_attr.retry_cnt;
	attr.sq_psn = cur_attr.sq_psn;
	attr.rnr_retry = cur_attr.rnr_retry;
	attr.max_rd_atomic = cur_attr.max_rd_atomic;
	attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_SQ_PSN | IBV_QP_RNR_RETRY |
		    IBV_QP_MAX_QP_RD_ATOMIC;

	rc = ibv_modify_qp(qp, &attr, attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to modify qp %p %u to RTS state, rc %d\n", qp, qp->qp_num, rc);
		return rc;
	}

	return 0;
}

static inline enum ibv_qp_state
accel_mlx5_get_qp_state(struct ibv_qp *qp) {
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr init_attr;

	ibv_query_qp(qp, &qp_attr, IBV_QP_STATE, &init_attr);

	return qp_attr.qp_state;
}

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task)
{
	struct accel_mlx5_req *req;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d\n", task, task->base.op_code);

	TAILQ_FOREACH(req, &task->reqs, link) {
		spdk_mempool_put(task->dev->dev_ctx->requests_pool, req);
	}
	spdk_accel_task_complete(&task->base, task->rc);
}

static inline int
accel_mlx5_flush_wrs(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task;
	struct accel_mlx5_qp *qp = dev->qp;
	int rc;

	if (spdk_unlikely(!qp->wr_started)) {
		return 0;
	}

	SPDK_DEBUGLOG(accel_mlx5, "Completing WRs on dev %s\n", dev->dev_ctx->context->device->name);
	rc = ibv_wr_complete(qp->qpex);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("ibv_wr_complete rc %d\n", rc);
		/* Complete all affected requests */
		TAILQ_FOREACH(task, &dev->before_submit, link) {
			task->rc = rc;
			accel_mlx5_task_complete(task);
		}
		TAILQ_INIT(&dev->before_submit);
	} else {
		TAILQ_CONCAT(&dev->in_hw, &dev->before_submit, link);
	}

	qp->wr_started = false;

	return rc;
}

static inline int
accel_mlx5_fill_block_sge(struct accel_mlx5_req *req, struct ibv_sge *sge,
			  struct spdk_iov_sgl *iovs)
{
	struct spdk_rdma_memory_translation translation;
	void *addr;
	uint32_t remaining = req->task->base.block_size;
	uint32_t size;
	int i = 0;
	int rc;

	while (remaining) {
		size = spdk_min(remaining, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		rc = spdk_rdma_get_translation(req->task->dev->mmap, addr, size, &translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, addr %p, length %u\n", addr, size);
			return rc;
		}
		spdk_iov_sgl_advance(iovs, size);
		sge[i].lkey = spdk_rdma_memory_translation_get_lkey(&translation);
		sge[i].addr = (uint64_t)addr;
		sge[i].length = size;
		i++;
		assert(remaining >= size);
		remaining -= size;
	}

	return i;
}

static inline bool
accel_mlx5_compare_iovs(struct iovec *v1, struct iovec *v2, uint32_t iovcnt)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		if (v1[i].iov_base != v2[i].iov_base || v1[i].iov_len != v2[i].iov_len) {
			return false;
		}
	}

	return true;
}

static inline uint32_t
accel_mlx5_task_alloc_reqs(struct accel_mlx5_task *task)
{
	struct accel_mlx5_req *reqs_tmp[ACCEL_MLX5_ALLOC_REQS_IN_BATCH], *req;
	uint32_t i, num_reqs, allocated_reqs = 0;
	uint32_t remaining_reqs = task->num_reqs - task->num_completed_reqs;
	uint32_t qp_slot = task->dev->max_reqs - task->dev->reqs_submitted;
	int rc;

	assert(task->num_reqs >= task->num_completed_reqs);
	remaining_reqs = spdk_min(remaining_reqs, qp_slot);

	while (remaining_reqs) {
		num_reqs = spdk_min(ACCEL_MLX5_ALLOC_REQS_IN_BATCH, remaining_reqs);
		rc = spdk_mempool_get_bulk(task->dev->dev_ctx->requests_pool, (void **)reqs_tmp, num_reqs);
		if (spdk_unlikely(rc)) {
			return allocated_reqs;
		}
		for (i = 0; i < num_reqs; i++) {
			req = reqs_tmp[i];
			req->src_sg_count = 0;
			req->dst_sg_count = 0;
			req->task = task;
			TAILQ_INSERT_TAIL(&task->reqs, req, link);
		}
		allocated_reqs += num_reqs;
		remaining_reqs -= num_reqs;
	}

	return allocated_reqs;
}

static inline int
accel_mlx5_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->dev;
	struct accel_mlx5_qp *qp = dev->qp;
	struct ibv_qp_ex *qpx = qp->qpex;
	struct mlx5dv_qp_ex *mqpx = qp->mqpx;
	struct mlx5dv_mkey_conf_attr mkey_attr = {};
	struct mlx5dv_crypto_attr cattr;
	struct accel_mlx5_req *req;
	uint64_t iv;
	uint32_t num_setters = 3; /* access flags, layout, crypto */
	int rc;

	iv = task->iv + mlx5_task->num_completed_reqs;

	if (!qp->wr_started) {
		ibv_wr_start(qpx);
		qp->wr_started = true;
	}

	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	while (mlx5_task->cur_req && dev->reqs_submitted < dev->max_reqs) {
		req = mlx5_task->cur_req;
		rc = accel_mlx5_fill_block_sge(req, req->src_sg, &mlx5_task->src);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
			goto err_out;
		}
		req->src_sg_count = rc;

		/* prepare memory key - destination for WRITE operation */
		qpx->wr_flags = IBV_SEND_INLINE;
		qpx->wr_id = (uint64_t)&req->mkey_wrid;
		mlx5dv_wr_mkey_configure(mqpx, req->mkey, num_setters, &mkey_attr);
		mlx5dv_wr_set_mkey_access_flags(mqpx,
						IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (mlx5_task->inplace) {
			mlx5dv_wr_set_mkey_layout_list(mqpx, req->src_sg_count, req->src_sg);
		} else {
			rc = accel_mlx5_fill_block_sge(req, req->dst_sg, &mlx5_task->dst);
			if (spdk_unlikely(rc <= 0)) {
				if (rc == 0) {
					rc = -EINVAL;
				}
				SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
				mlx5_task->rc = rc;
				goto err_out;
			}
			req->dst_sg_count = rc;
			mlx5dv_wr_set_mkey_layout_list(mqpx, req->dst_sg_count, req->dst_sg);
		}
		SPDK_DEBUGLOG(accel_mlx5, "req %p, task %p crypto_attr: bs %u, iv %"PRIu64", enc_on_tx %d\n",
			      req, req->task, task->block_size, iv, mlx5_task->encrypt_on_tx);
		rc = spdk_mlx5_crypto_set_attr(&cattr, task->crypto_key->priv, dev->dev_ctx->pd, task->block_size,
					       iv++, mlx5_task->encrypt_on_tx);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("failed to set crypto attr, rc %d\n", rc);
			mlx5_task->rc = rc;
			goto err_out;
		}
		mlx5dv_wr_set_mkey_crypto(mqpx, &cattr);

		/* Prepare WRITE, use rkey from mkey, remote addr is always 0 - start of the mkey */
		qpx->wr_flags = IBV_SEND_SIGNALED;
		qpx->wr_id = (uint64_t)&req->write_wrid;
		ibv_wr_rdma_write(qpx, req->mkey->rkey, 0);
		/* local buffers, SG is already filled */
		ibv_wr_set_sge_list(qpx, req->src_sg_count, req->src_sg);

		mlx5_task->num_submitted_reqs++;
		assert(mlx5_task->num_submitted_reqs <= mlx5_task->num_reqs);
		dev->reqs_submitted++;
		mlx5_task->cur_req = TAILQ_NEXT(mlx5_task->cur_req, link);
	}

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	TAILQ_INSERT_TAIL(&dev->before_submit, mlx5_task, link);

	return 0;

err_out:
	/* Abort all WRs submitted since last wr_start */
	ibv_wr_abort(qpx);
	accel_mlx5_task_complete(mlx5_task);
	TAILQ_FOREACH(mlx5_task, &dev->before_submit, link) {
		mlx5_task->rc = rc;
		accel_mlx5_task_complete(mlx5_task);
	}
	TAILQ_INIT(&dev->before_submit);

	return rc;

}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_req *req;

	TAILQ_FOREACH(req, &task->reqs, link) {
		spdk_mempool_put(task->dev->dev_ctx->requests_pool, req);
	}
	TAILQ_INIT(&task->reqs);

	if (spdk_unlikely(task->rc)) {
		accel_mlx5_task_complete(task);
		return 0;
	}

	if (spdk_unlikely(!accel_mlx5_task_alloc_reqs(task))) {
		/* Pool is empty, queue this task */
		TAILQ_INSERT_TAIL(&task->dev->nomem, task, link);
		return -ENOMEM;
	}
	task->cur_req = TAILQ_FIRST(&task->reqs);

	return accel_mlx5_task_process(task);
}

static inline int
accel_mlx5_task_init(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	size_t src_nbytes = 0, dst_nbytes = 0;
	uint32_t i;

	switch (task->op_code) {
	case ACCEL_OPC_ENCRYPT:
		mlx5_task->encrypt_on_tx = true;
		break;
	case ACCEL_OPC_DECRYPT:
		mlx5_task->encrypt_on_tx = false;
		break;
	default:
		SPDK_ERRLOG("Unsupported accel opcode %d\n", task->op_code);
		return -ENOTSUP;
	}

	for (i = 0; i < task->s.iovcnt; i++) {
		src_nbytes += task->s.iovs[i].iov_len;
	}

	for (i = 0; i < task->d.iovcnt; i++) {
		dst_nbytes += task->d.iovs[i].iov_len;
	}

	if (spdk_unlikely(src_nbytes != dst_nbytes)) {
		return -EINVAL;
	}
	if (spdk_unlikely(src_nbytes % mlx5_task->base.block_size != 0)) {
		return -EINVAL;
	}

	mlx5_task->dev = dev;
	mlx5_task->rc = 0;
	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	mlx5_task->cur_req = NULL;
	mlx5_task->num_reqs = src_nbytes / mlx5_task->base.block_size;
	spdk_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt, 0);
	if (task->d.iovcnt == 0 || (task->d.iovcnt == task->s.iovcnt &&
				    accel_mlx5_compare_iovs(task->d.iovs, task->s.iovs, task->s.iovcnt))) {
		mlx5_task->inplace = true;
	} else {
		mlx5_task->inplace = false;
		spdk_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt, 0);
	}

	TAILQ_INIT(&mlx5_task->reqs);
	if (spdk_unlikely(!accel_mlx5_task_alloc_reqs(mlx5_task))) {
		/* Pool is empty, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in pool, dev %s\n",
			      mlx5_task->dev->dev_ctx->context->device->name);
		return -ENOMEM;
	}
	mlx5_task->cur_req = TAILQ_FIRST(&mlx5_task->reqs);

	SPDK_DEBUGLOG(accel_mlx5, "task %p, inplace %d, num_reqs %d\n", mlx5_task, mlx5_task->inplace,
		      mlx5_task->num_reqs);

	return 0;
}

static int
accel_mlx5_submit_tasks(struct spdk_io_channel *_ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	struct accel_mlx5_dev *dev;
	int rc;

	if (!g_accel_mlx5.enabled || !task->crypto_key ||
	    task->crypto_key->module_if != &g_accel_mlx5.module ||
	    !task->crypto_key->priv) {
		return -EINVAL;
	}
	dev = &ch->devs[ch->dev_idx];
	ch->dev_idx++;
	if (ch->dev_idx == ch->num_devs) {
		ch->dev_idx = 0;
	}

	rc = accel_mlx5_task_init(mlx5_task, dev);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(accel_mlx5, "no reqs to handle new task %p (requred %u), put to queue\n", mlx5_task,
				      mlx5_task->num_reqs);
			TAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
			return 0;
		}
		return rc;
	}

	return accel_mlx5_task_process(mlx5_task);
}

static inline int64_t
accel_mlx5_poll_cq(struct accel_mlx5_dev *dev)
{
	struct ibv_wc wc[ACCEL_MLX5_MAX_WC];
	struct accel_mlx5_task *task;
	struct accel_mlx5_req *req;
	struct accel_mlx5_wrid *wr;
	int reaped, i, rc;

	reaped = ibv_poll_cq(dev->cq, ACCEL_MLX5_MAX_WC, wc);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return reaped;
	} else if (reaped == 0) {
		return 0;
	}

	SPDK_DEBUGLOG(accel_mlx5, "Reaped %d cpls on dev %s\n", reaped,
		      dev->dev_ctx->context->device->name);

	for (i = 0; i < reaped; i++) {
		wr = (struct accel_mlx5_wrid *)wc[i].wr_id;

		switch (wr->wrid) {
		case ACCEL_MLX5_WRID_MKEY:
			/* We only get this completion in error case */
			req = SPDK_CONTAINEROF(wr, struct accel_mlx5_req, mkey_wrid);
			if (!wc[i].status) {
				SPDK_ERRLOG("Got unexpected cpl for mkey configure, req %p, qp %p, state %d\n",
					    req, dev->qp->qp, accel_mlx5_get_qp_state(dev->qp->qp));
			} else {
				SPDK_ERRLOG("MKEY: qp %p, state %d, req %p, task %p WC status %d\n",
					    dev->qp->qp, accel_mlx5_get_qp_state(dev->qp->qp), req, req->task, wc[i].status);
			}
			break;
		case ACCEL_MLX5_WRID_WRITE:
			req = SPDK_CONTAINEROF(wr, struct accel_mlx5_req, write_wrid);
			task = req->task;
			if (wc[i].status) {
				assert(req->task);
				SPDK_ERRLOG("WRITE: qp %p, state %d, req %p, task %p WC status %d\n", dev->qp->qp,
					    accel_mlx5_get_qp_state(dev->qp->qp), req, req->task, wc[i].status);
				if (!task->rc) {
					task->rc = -EIO;
				}
			}

			task->num_completed_reqs++;
			assert(dev->reqs_submitted);
			dev->reqs_submitted--;
			SPDK_DEBUGLOG(accel_mlx5, "req %p, task %p, remaining %u\n", req, task,
				      task->num_reqs - task->num_completed_reqs);
			if (task->num_completed_reqs == task->num_reqs) {
				TAILQ_REMOVE(&dev->in_hw, task, link);
				accel_mlx5_task_complete(task);
			} else if (task->num_completed_reqs == task->num_submitted_reqs) {
				assert(task->num_submitted_reqs < task->num_reqs);
				TAILQ_REMOVE(&dev->in_hw, task, link);
				rc = accel_mlx5_task_continue(task);
				if (spdk_unlikely(rc)) {
					if (rc != -ENOMEM) {
						task->rc = rc;
						accel_mlx5_task_complete(task);
					}
				}
			}
			break;
		}
	}

	return reaped;
}

static inline void
accel_mlx5_resubmit_nomem_tasks(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task, *tmp;
	int rc;

	TAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		TAILQ_REMOVE(&dev->nomem, task, link);
		rc = accel_mlx5_task_continue(task);
		if (rc) {
			if (rc == -ENOMEM) {
				break;
			} else {
				task->rc = rc;
				accel_mlx5_task_complete(task);
			}
		}
	}
}

static int
accel_mlx5_poller(void *ctx)
{
	struct accel_mlx5_io_channel *ch = ctx;
	struct accel_mlx5_dev *dev;

	int64_t completions = 0, rc;
	uint32_t i;

	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		if (dev->reqs_submitted) {
			rc = accel_mlx5_poll_cq(dev);
			if (spdk_unlikely(rc < 0)) {
				SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc, dev->dev_ctx->context->device->name);
			}
			completions += rc;
			accel_mlx5_flush_wrs(dev);
		}
		if (!TAILQ_EMPTY(&dev->nomem)) {
			accel_mlx5_resubmit_nomem_tasks(dev);
		}
	}

	return !!completions;
}

static bool
accel_mlx5_supports_opcode(enum accel_opcode opc)
{
	assert(g_accel_mlx5.enabled);

	switch (opc) {
	case ACCEL_OPC_ENCRYPT:
	case ACCEL_OPC_DECRYPT:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
accel_mlx5_get_io_channel(void)
{
	assert(g_accel_mlx5.enabled);
	return spdk_get_io_channel(&g_accel_mlx5);
}

static void
accel_mlx5_qp_destroy(struct accel_mlx5_qp *qp)
{
	if (!qp) {
		return;
	}

	if (qp->qp) {
		ibv_destroy_qp(qp->qp);
		qp->qp = NULL;
	}

	free(qp);
}

static struct accel_mlx5_qp *
accel_mlx5_qp_create(struct ibv_cq *cq, struct accel_mlx5_io_channel *ch, struct ibv_pd *pd,
		     int qp_size)
{
	struct accel_mlx5_qp *qp;
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.qp_context = ch,
		.cap = {
			.max_send_wr = qp_size,
			.max_recv_wr = 0,
			.max_send_sge = ACCEL_MLX5_MAX_SGE,
			.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE,
		},
		.qp_type = IBV_QPT_RC,
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		.pd = pd,
		.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |  IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ | IBV_QP_EX_WITH_BIND_MW,
		.send_cq = cq,
		.recv_cq = cq,
	};
	/* Attrs required for MKEYs registration */
	struct mlx5dv_qp_init_attr mlx5_qp_attr = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS,
		.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE
	};
	int rc;

	if (!dv_qp_attr.send_cq || !dv_qp_attr.recv_cq) {
		return  NULL;
	}

	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		return NULL;
	}

	qp->qp = mlx5dv_create_qp(cq->context, &dv_qp_attr, &mlx5_qp_attr);
	if (!qp->qp) {
		SPDK_ERRLOG("Failed to create qpair, errno %s (%d)\n", spdk_strerror(errno), errno);
		free(qp);
		return NULL;
	}

	rc = mlx5_qp_init_2_rts(qp->qp, qp->qp->qp_num);
	if (rc) {
		SPDK_ERRLOG("Failed to create loopback connection, qp_num %u\n", qp->qp->qp_num);
		accel_mlx5_qp_destroy(qp);
		return NULL;
	}

	qp->qpex = ibv_qp_to_qp_ex(qp->qp);
	if (!qp->qpex) {
		SPDK_ERRLOG("Failed to get qpex\n");
		accel_mlx5_qp_destroy(qp);
		return NULL;
	}

	qp->mqpx = mlx5dv_qp_ex_from_ibv_qp_ex(qp->qpex);
	if (!qp->mqpx) {
		SPDK_ERRLOG("Failed to get mqpx\n");
		accel_mlx5_qp_destroy(qp);
		return NULL;
	}

	qp->num_reqs = qp_size;
	qp->cq = cq;

	return qp;
}

static void
accel_mlx5_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev *dev;
	uint32_t i;

	spdk_poller_unregister(&ch->poller);
	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		accel_mlx5_qp_destroy(dev->qp);
		if (dev->cq) {
			ibv_destroy_cq(dev->cq);
			dev->cq = NULL;
		}
		spdk_rdma_free_mem_map(&dev->mmap);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	ch->devs = calloc(g_accel_mlx5.num_crypto_ctxs, sizeof(*ch->devs));
	if (!ch->devs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_accel_mlx5.num_crypto_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.crypto_ctxs[i];
		dev = &ch->devs[i];
		dev->dev_ctx = dev_ctx;
		ch->num_devs++;
		dev->cq = ibv_create_cq(dev_ctx->context, g_accel_mlx5.attr.qp_size, ch, NULL, 0);
		if (!dev->cq) {
			SPDK_ERRLOG("Failed to create CQ on dev %s\n", dev_ctx->context->device->name);
			rc = -ENOMEM;
			goto err_out;
		}

		dev->qp = accel_mlx5_qp_create(dev->cq, ch, dev_ctx->pd, g_accel_mlx5.attr.qp_size);
		if (!dev->qp) {
			SPDK_ERRLOG("Failed to create QP on dev %s\n", dev_ctx->context->device->name);
			rc = -ENOMEM;
			goto err_out;
		}

		TAILQ_INIT(&dev->nomem);
		TAILQ_INIT(&dev->in_hw);
		TAILQ_INIT(&dev->before_submit);
		/* Each request consumes 2 WQE - MKEY and RDMA_WRITE. MKEY is unsignaled, so we count only RDMA_WRITE completions.
		 * Divide user defined qp_size by two for simplicity */
		dev->max_reqs = g_accel_mlx5.attr.qp_size / 2;
		dev->mmap = spdk_rdma_create_mem_map(dev_ctx->pd, NULL, SPDK_RDMA_MEMORY_MAP_ROLE_INITIATOR);
		if (!dev->mmap) {
			SPDK_ERRLOG("Failed to create memory map\n");
			accel_mlx5_qp_destroy(dev->qp);
			return -ENOMEM;
		}
	}

	ch->poller = SPDK_POLLER_REGISTER(accel_mlx5_poller, ch, 0);

	return 0;

err_out:
	accel_mlx5_destroy_cb(&g_accel_mlx5, ctx_buf);
	return rc;
}

void
accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr)
{
	assert(attr);

	attr->qp_size = ACCEL_MLX5_QP_SIZE;
	attr->num_requests = ACCEL_MLX5_NUM_REQUESTS;
}

int
accel_mlx5_enable(struct accel_mlx5_attr *attr)
{
	if (g_accel_mlx5.enabled) {
		return -EEXIST;
	}
	if (attr) {
		g_accel_mlx5.attr = *attr;
	} else {
		accel_mlx5_get_default_attr(&g_accel_mlx5.attr);
	}

	g_accel_mlx5.enabled = true;
	spdk_accel_module_list_add(&g_accel_mlx5.module);

	return 0;
}

static void
accel_mlx5_release_crypto_req(struct spdk_mempool *mp, void *cb_arg, void *_req, unsigned obj_idx)
{
	struct accel_mlx5_req *req = _req;

	if (req->mkey) {
		mlx5dv_destroy_mkey(req->mkey);
	}
}


static void
accel_mlx5_release_reqs(struct accel_mlx5_crypto_dev_ctx *dev_ctx)
{
	if (!dev_ctx->requests_pool) {
		return;
	}

	spdk_mempool_obj_iter(dev_ctx->requests_pool, accel_mlx5_release_crypto_req, NULL);
}

static void
accel_mlx5_free_resources(void)
{
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_crypto_ctxs; i++) {
		accel_mlx5_release_reqs(&g_accel_mlx5.crypto_ctxs[i]);
		spdk_rdma_put_pd(g_accel_mlx5.crypto_ctxs[i].pd);
	}

	free(g_accel_mlx5.crypto_ctxs);
	g_accel_mlx5.crypto_ctxs = NULL;
}

static void
accel_mlx5_deinit_cb(void *ctx)
{
	accel_mlx5_free_resources();
	spdk_accel_module_finish();
}

static void
accel_mlx5_deinit(void *ctx)
{
	if (g_accel_mlx5.crypto_ctxs) {
		spdk_io_device_unregister(&g_accel_mlx5, accel_mlx5_deinit_cb);
	} else {
		spdk_accel_module_finish();
	}
}

static void
accel_mlx5_configure_crypto_req(struct spdk_mempool *mp, void *cb_arg, void *_req, unsigned obj_idx)
{
	struct accel_mlx5_req *req = _req;
	struct accel_mlx5_req_init_ctx *ctx = cb_arg;
	struct mlx5dv_mkey_init_attr mkey_attr = {
		.pd = ctx->pd,
		.max_entries = ACCEL_MLX5_MAX_SGE, /* This MKEY refers to N base MKEYs/buffers */
		.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT | /* This MKEY refers to another MKEYs */
		MLX5DV_MKEY_INIT_ATTR_FLAGS_CRYPTO
	};

	memset(req, 0, sizeof(*req));
	if (ctx->rc) {
		return;
	}

	req->mkey = mlx5dv_create_mkey(&mkey_attr);
	if (!req->mkey) {
		SPDK_ERRLOG("Failed to create mkey on dev %s, errno %d\n", ctx->pd->context->device->name, errno);
		ctx->rc = errno;
		return;
	}

	req->mkey_wrid.wrid = ACCEL_MLX5_WRID_MKEY;
	req->write_wrid.wrid = ACCEL_MLX5_WRID_WRITE;
}

static int
accel_mlx5_crypto_ctx_mempool_create(struct accel_mlx5_crypto_dev_ctx *crypto_dev_ctx,
				     size_t num_entries)
{
	struct accel_mlx5_req_init_ctx init_ctx = {.pd = crypto_dev_ctx->pd };
	char pool_name[32];
	int rc;

	/* Compiler may produce a warning like
	 * warning: ‘%s’ directive output may be truncated writing up to 63 bytes into a region of size 21
	 * [-Wformat-truncation=]
	 * That is expected and that is due to ibv device name is 64 bytes while DPDK mempool API allows
	 * name to be max 32 bytes.
	 * To suppress this warning check the value returned by snprintf */
	rc = snprintf(pool_name, 32, "accel_mlx5_%s", crypto_dev_ctx->context->device->name);
	if (rc < 0) {
		assert(0);
		return -EINVAL;
	}
	crypto_dev_ctx->requests_pool = spdk_mempool_create_ctor(pool_name, num_entries,
					sizeof(struct accel_mlx5_req),
					SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, SPDK_ENV_SOCKET_ID_ANY,
					accel_mlx5_configure_crypto_req, &init_ctx);
	if (!crypto_dev_ctx->requests_pool || init_ctx.rc) {
		SPDK_ERRLOG("Failed to create memory pool\n");
		return init_ctx.rc ? : -ENOMEM;
	}

	return 0;
}

static int
accel_mlx5_init(void)
{
	struct accel_mlx5_crypto_dev_ctx *crypto_dev_ctx;
	struct ibv_context **rdma_devs, *dev;
	struct ibv_pd *pd;
	int num_devs = 0, rc = 0, i;

	if (!g_accel_mlx5.enabled) {
		return -EINVAL;
	}

	rdma_devs = spdk_mlx5_crypto_devs_get(&num_devs);
	if (!rdma_devs || !num_devs) {
		SPDK_NOTICELOG("No crypto devs found\n");
		return -ENOTSUP;
	}

	g_accel_mlx5.crypto_ctxs = calloc(num_devs, sizeof(*g_accel_mlx5.crypto_ctxs));
	if (!g_accel_mlx5.crypto_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < num_devs; i++) {
		crypto_dev_ctx = &g_accel_mlx5.crypto_ctxs[i];
		dev = rdma_devs[i];
		pd = spdk_rdma_get_pd(dev);
		if (!pd) {
			SPDK_ERRLOG("Failed to get PD for context %p, dev %s\n", dev, dev->device->name);
			rc = -EINVAL;
			goto cleanup;
		}
		crypto_dev_ctx->context = dev;
		crypto_dev_ctx->pd = pd;
		g_accel_mlx5.num_crypto_ctxs++;
		rc = accel_mlx5_crypto_ctx_mempool_create(crypto_dev_ctx, g_accel_mlx5.attr.num_requests);
		if (rc) {
			goto cleanup;
		}
	}

	SPDK_NOTICELOG("Accel framework mlx5 initialized, found %d devices.\n", num_devs);
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");

	spdk_mlx5_crypto_devs_release(rdma_devs);

	return rc;

cleanup:
	spdk_mlx5_crypto_devs_release(rdma_devs);
	accel_mlx5_free_resources();

	return rc;
}

static void
accel_mlx5_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_accel_mlx5.enabled) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "mlx5_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint16(w, "qp_size", g_accel_mlx5.attr.qp_size);
		spdk_json_write_named_uint32(w, "num_requests", g_accel_mlx5.attr.num_requests);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

static size_t
accel_mlx5_get_ctx_size(void)
{
	return sizeof(struct accel_mlx5_task);
}

static int
accel_mlx5_crypto_key_init(struct spdk_accel_crypto_key *key)
{
	struct spdk_mlx5_crypto_dek_create_attr attr = {};
	struct spdk_mlx5_crypto_keytag *keytag;
	int rc;

	if (!key || !key->key || !key->key2 || !key->key_size || !key->key2_size) {
		return -EINVAL;
	}

	attr.dek = calloc(1, key->key_size + key->key2_size);
	if (!attr.dek) {
		return -ENOMEM;
	}

	memcpy(attr.dek, key->key, key->key_size);
	memcpy(attr.dek + key->key_size, key->key2, key->key2_size);
	attr.dek_len = key->key_size + key->key2_size;

	rc = spdk_mlx5_crypto_keytag_create(&attr, &keytag);
	spdk_memset_s(attr.dek, attr.dek_len, 0, attr.dek_len);
	free(attr.dek);
	if (rc) {
		SPDK_ERRLOG("Failed to create a keytag, rc %d\n", rc);
		return rc;
	}

	key->priv = keytag;

	return 0;
}

static void
accel_mlx5_crypto_key_deinit(struct spdk_accel_crypto_key *key)
{
	if (!key || key->module_if != &g_accel_mlx5.module || !key->priv) {
		return;
	}

	spdk_mlx5_crypto_keytag_destroy(key->priv);
}

static struct accel_mlx5_module g_accel_mlx5 = {
	.module = {
		.module_init		= accel_mlx5_init,
		.module_fini		= accel_mlx5_deinit,
		.write_config_json	= accel_mlx5_write_config_json,
		.get_ctx_size		= accel_mlx5_get_ctx_size,
		.name			= "mlx5",
		.supports_opcode	= accel_mlx5_supports_opcode,
		.get_io_channel		= accel_mlx5_get_io_channel,
		.submit_tasks		= accel_mlx5_submit_tasks,
		.crypto_key_init	= accel_mlx5_crypto_key_init,
		.crypto_key_deinit	= accel_mlx5_crypto_key_deinit,
	}
};

SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
