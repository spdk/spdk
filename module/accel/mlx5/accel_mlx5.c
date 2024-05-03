/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include "spdk_internal/rdma_utils.h"
#include "spdk/accel_module.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/sgl.h"
#include "accel_mlx5.h"

#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#define ACCEL_MLX5_QP_SIZE (256u)
#define ACCEL_MLX5_NUM_REQUESTS (2048u - 1)
#define ACCEL_MLX5_RECOVER_POLLER_PERIOD_US (10000)
#define ACCEL_MLX5_MAX_SGE (16u)
#define ACCEL_MLX5_MAX_WC (64u)
#define ACCEL_MLX5_MAX_MKEYS_IN_TASK (16u)

/* Assume we have up to 16 devices */
#define ACCEL_MLX5_ALLOWED_DEVS_MAX_LEN ((SPDK_MLX5_DEV_MAX_NAME_LEN + 1) * 16)

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, task)	\
do {							\
	assert((qp)->wrs_submitted < (qp)->wrs_max);	\
	(qp)->wrs_submitted++;				\
	assert((task)->num_wrs < UINT16_MAX);		\
	(task)->num_wrs++;				\
} while (0)

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, task)	\
do {									\
	assert((dev)->wrs_in_cq < (dev)->wrs_in_cq_max);		\
	(dev)->wrs_in_cq++;						\
        assert((qp)->wrs_submitted < (qp)->wrs_max);			\
	(qp)->wrs_submitted++;						\
	assert((task)->num_wrs < UINT16_MAX);				\
	(task)->num_wrs++;						\
} while (0)

struct accel_mlx5_io_channel;
struct accel_mlx5_task;

struct accel_mlx5_crypto_dev_ctx {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct spdk_memory_domain *domain;
	TAILQ_ENTRY(accel_mlx5_crypto_dev_ctx) link;
	bool crypto_mkeys;
	bool crypto_multi_block;
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_crypto_dev_ctx *crypto_ctxs;
	uint32_t num_crypto_ctxs;
	struct accel_mlx5_attr attr;
	char **allowed_devs;
	size_t allowed_devs_count;
	bool initialized;
	bool enabled;
	bool crypto_supported;
};

struct accel_mlx5_sge {
	uint32_t src_sge_count;
	uint32_t dst_sge_count;
	struct ibv_sge src_sge[ACCEL_MLX5_MAX_SGE];
	struct ibv_sge dst_sge[ACCEL_MLX5_MAX_SGE];
};

struct accel_mlx5_iov_sgl {
	struct iovec	*iov;
	uint32_t	iovcnt;
	uint32_t	iov_offset;
};

struct accel_mlx5_task {
	struct spdk_accel_task base;
	struct accel_mlx5_iov_sgl src;
	struct accel_mlx5_iov_sgl dst;
	struct accel_mlx5_qp *qp;
	STAILQ_ENTRY(accel_mlx5_task) link;
	uint16_t num_reqs;
	uint16_t num_completed_reqs;
	uint16_t num_submitted_reqs;
	uint16_t num_ops; /* number of allocated mkeys */
	uint16_t blocks_per_req;
	uint16_t num_processed_blocks;
	uint16_t num_blocks;
	uint16_t num_wrs; /* Number of outstanding operations which consume qp slot */
	union {
		uint8_t raw;
		struct {
			uint8_t inplace : 1;
			uint8_t enc_order : 2;
		};
	};
	/* Keep this array last since not all elements might be accessed, this reduces amount of data to be
	 * cached */
	struct spdk_mlx5_mkey_pool_obj *mkeys[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
};

struct accel_mlx5_qp {
	struct spdk_mlx5_qp *qp;
	struct ibv_qp *verbs_qp;
	struct accel_mlx5_dev *dev;
	struct accel_mlx5_io_channel *ch;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	STAILQ_HEAD(, accel_mlx5_task) in_hw;
	uint16_t wrs_submitted;
	uint16_t wrs_max;
	bool recovering;
	struct spdk_poller *recover_poller;
};

struct accel_mlx5_dev {
	struct accel_mlx5_qp qp;
	struct spdk_mlx5_cq *cq;
	struct spdk_mlx5_mkey_pool *crypto_mkeys;
	struct spdk_rdma_utils_mem_map *mmap;
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	uint16_t wrs_in_cq;
	uint16_t wrs_in_cq_max;
	uint16_t crypto_split_blocks;
	bool crypto_multi_block;
	/* Pending tasks waiting for requests resources */
	STAILQ_HEAD(, accel_mlx5_task) nomem;
	TAILQ_ENTRY(accel_mlx5_dev) link;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint32_t num_devs;
	/* Index in \b devs to be used for crypto in round-robin way */
	uint32_t dev_idx;
};

static struct accel_mlx5_module g_accel_mlx5;

static inline void
accel_mlx5_iov_sgl_init(struct accel_mlx5_iov_sgl *s, struct iovec *iov, uint32_t iovcnt)
{
	s->iov = iov;
	s->iovcnt = iovcnt;
	s->iov_offset = 0;
}

static inline void
accel_mlx5_iov_sgl_advance(struct accel_mlx5_iov_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	while (s->iovcnt > 0) {
		assert(s->iov != NULL);
		if (s->iov_offset < s->iov->iov_len) {
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		s->iov++;
		s->iovcnt--;
	}
}

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task)
{
	struct accel_mlx5_dev *dev = task->qp->dev;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d\n", task, task->base.op_code);

	if (task->num_ops) {
		spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
	}
	spdk_accel_task_complete(&task->base, 0);
}

static inline void
accel_mlx5_task_fail(struct accel_mlx5_task *task, int rc)
{
	struct accel_mlx5_dev *dev = task->qp->dev;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Fail task %p, opc %d, rc %d\n", task, task->base.op_code, rc);

	if (task->num_ops) {
		spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
	}
	spdk_accel_task_complete(&task->base, rc);
}

static int
accel_mlx5_translate_addr(void *addr, size_t size, struct spdk_memory_domain *domain,
			  void *domain_ctx, struct accel_mlx5_dev *dev, struct ibv_sge *sge)
{
	struct spdk_rdma_utils_memory_translation map_translation;
	struct spdk_memory_domain_translation_result domain_translation;
	struct spdk_memory_domain_translation_ctx local_ctx;
	int rc;

	if (domain) {
		domain_translation.size = sizeof(struct spdk_memory_domain_translation_result);
		local_ctx.size = sizeof(local_ctx);
		local_ctx.rdma.ibv_qp = dev->qp.verbs_qp;
		rc = spdk_memory_domain_translate_data(domain, domain_ctx, dev->dev_ctx->domain,
						       &local_ctx, addr, size, &domain_translation);
		if (spdk_unlikely(rc || domain_translation.iov_count != 1)) {
			SPDK_ERRLOG("Memory domain translation failed, addr %p, length %zu, iovcnt %u\n", addr, size,
				    domain_translation.iov_count);
			if (rc == 0) {
				rc = -EINVAL;
			}

			return rc;
		}
		sge->lkey = domain_translation.rdma.lkey;
		sge->addr = (uint64_t) domain_translation.iov.iov_base;
		sge->length = domain_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_utils_get_translation(dev->mmap, addr, size,
						     &map_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", addr, size);
			return rc;
		}
		sge->lkey = spdk_rdma_utils_memory_translation_get_lkey(&map_translation);
		sge->addr = (uint64_t)addr;
		sge->length = size;
	}

	return 0;
}

static inline int
accel_mlx5_fill_block_sge(struct accel_mlx5_dev *dev, struct ibv_sge *sge,
			  struct accel_mlx5_iov_sgl *iovs, uint32_t len, struct spdk_memory_domain *domain, void *domain_ctx)
{
	void *addr;
	uint32_t remaining = len;
	uint32_t size;
	int i = 0;
	int rc;

	while (remaining && i < (int)ACCEL_MLX5_MAX_SGE) {
		size = spdk_min(remaining, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		rc = accel_mlx5_translate_addr(addr, size, domain, domain_ctx, dev, &sge[i]);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\t sge[%d]: lkey %u, len %u, addr %"PRIx64"\n", i, sge[i].lkey,
			      sge[i].length, sge[i].addr);
		accel_mlx5_iov_sgl_advance(iovs, size);
		i++;
		assert(remaining >= size);
		remaining -= size;
	}
	assert(remaining == 0);

	return i;
}

static inline bool
accel_mlx5_compare_iovs(struct iovec *v1, struct iovec *v2, uint32_t iovcnt)
{
	return memcmp(v1, v2, sizeof(*v1) * iovcnt) == 0;
}

static inline uint16_t
accel_mlx5_dev_get_available_slots(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	assert(qp->wrs_max >= qp->wrs_submitted);
	assert(dev->wrs_in_cq_max >= dev->wrs_in_cq);

	/* Each time we produce only 1 CQE, so we need 1 CQ slot */
	if (spdk_unlikely(dev->wrs_in_cq == dev->wrs_in_cq_max)) {
		return 0;
	}

	return qp->wrs_max - qp->wrs_submitted;
}

static inline uint32_t
accel_mlx5_task_alloc_mkeys(struct accel_mlx5_task *task)
{
	struct accel_mlx5_dev *dev = task->qp->dev;
	uint32_t num_ops;
	int rc;

	assert(task->num_reqs > task->num_completed_reqs);
	num_ops = task->num_reqs - task->num_completed_reqs;
	num_ops = spdk_min(num_ops, ACCEL_MLX5_MAX_MKEYS_IN_TASK);
	if (!num_ops) {
		return 0;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, task->mkeys, num_ops);
	if (spdk_unlikely(rc)) {
		return 0;
	}
	assert(num_ops <= UINT16_MAX);
	task->num_ops = num_ops;

	return num_ops;
}

static inline uint8_t
bs_to_bs_selector(uint32_t bs)
{
	switch (bs) {
	case 512:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_512;
	case 520:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_520;
	case 4096:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_4096;
	case 4160:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_4160;
	default:
		return SPDK_MLX5_BLOCK_SIZE_SELECTOR_RESERVED;
	}
}

static inline int
accel_mlx5_configure_crypto_umr(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_sge *sge,
				uint32_t mkey, uint32_t num_blocks, struct spdk_mlx5_crypto_dek_data *dek_data)
{
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_attr umr_attr;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_accel_task *task = &mlx5_task->base;
	uint32_t length;
	int rc;

	length = num_blocks * task->block_size;
	SPDK_DEBUGLOG(accel_mlx5, "task %p, domain %p, len %u, blocks %u\n", task, task->src_domain, length,
		      num_blocks);
	rc = accel_mlx5_fill_block_sge(dev, sge->src_sge, &mlx5_task->src,  length, task->src_domain,
				       task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sge->src_sge_count = rc;

	cattr.xts_iv = task->iv + mlx5_task->num_processed_blocks;
	cattr.keytag = 0;
	cattr.dek_obj_id = dek_data->dek_obj_id;
	cattr.tweak_mode = dek_data->tweak_mode;
	cattr.enc_order = mlx5_task->enc_order;
	cattr.bs_selector = bs_to_bs_selector(mlx5_task->base.block_size);
	if (spdk_unlikely(cattr.bs_selector == SPDK_MLX5_BLOCK_SIZE_SELECTOR_RESERVED)) {
		SPDK_ERRLOG("unsupported block size %u\n", mlx5_task->base.block_size);
		return -EINVAL;
	}
	umr_attr.mkey = mkey;
	umr_attr.sge = sge->src_sge;

	if (!mlx5_task->inplace) {
		SPDK_DEBUGLOG(accel_mlx5, "task %p, dst sge, domain %p, len %u\n", task, task->dst_domain, length);
		rc = accel_mlx5_fill_block_sge(dev, sge->dst_sge, &mlx5_task->dst, length, task->dst_domain,
					       task->dst_domain_ctx);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		sge->dst_sge_count = rc;
	}

	SPDK_DEBUGLOG(accel_mlx5,
		      "task %p: bs %u, iv %"PRIu64", enc_on_tx %d, tweak_mode %d, len %u, mkey %x, blocks %u\n",
		      mlx5_task, task->block_size, cattr.xts_iv, mlx5_task->enc_order, cattr.tweak_mode, length, mkey,
		      num_blocks);

	umr_attr.sge_count = sge->src_sge_count;
	umr_attr.umr_len = length;
	assert((uint32_t)mlx5_task->num_processed_blocks + num_blocks <= UINT16_MAX);
	mlx5_task->num_processed_blocks += num_blocks;

	rc = spdk_mlx5_umr_configure_crypto(qp->qp, &umr_attr, &cattr, 0, 0);

	return rc;
}

static inline int
accel_mlx5_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sges[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct spdk_mlx5_crypto_dek_data dek_data;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	/* First RDMA after UMR must have a SMALL_FENCE */
	uint32_t first_rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	uint16_t num_blocks;
	uint16_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint16_t i;
	int rc;

	assert(qp_slot > 1);
	num_ops = spdk_min(num_ops, qp_slot >> 1);
	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	rc = spdk_mlx5_crypto_get_dek_data(mlx5_task->base.crypto_key->priv, dev->dev_ctx->pd, &dek_data);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	mlx5_task->num_wrs = 0;
	SPDK_DEBUGLOG(accel_mlx5, "begin, task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);
	for (i = 0; i < num_ops; i++) {
		if (mlx5_task->num_submitted_reqs + i + 1 == mlx5_task->num_reqs) {
			/* Last request may consume less than calculated if crypto_multi_block is true */
			assert(mlx5_task->num_blocks > mlx5_task->num_submitted_reqs);
			num_blocks = mlx5_task->num_blocks - mlx5_task->num_processed_blocks;
		} else {
			num_blocks = mlx5_task->blocks_per_req;
		}

		rc = accel_mlx5_configure_crypto_umr(mlx5_task, &sges[i], mlx5_task->mkeys[i]->mkey, num_blocks,
						     &dek_data);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	/* Loop `num_ops - 1` for easy flags handling */
	for (i = 0; i < num_ops - 1; i++) {
		/* UMR is used as a destination for RDMA_READ - from UMR to sge */
		if (mlx5_task->inplace) {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].src_sge, sges[i].src_sge_count, 0,
						    mlx5_task->mkeys[i]->mkey, 0, first_rdma_fence);
		} else {
			rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].dst_sge, sges[i].dst_sge_count, 0,
						    mlx5_task->mkeys[i]->mkey, 0, first_rdma_fence);
		}
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}

		first_rdma_fence = 0;
		assert(mlx5_task->num_submitted_reqs < mlx5_task->num_reqs);
		assert(mlx5_task->num_submitted_reqs < UINT16_MAX);
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	if (mlx5_task->inplace) {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].src_sge, sges[i].src_sge_count, 0,
					    mlx5_task->mkeys[i]->mkey, (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	} else {
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges[i].dst_sge, sges[i].dst_sge_count, 0,
					    mlx5_task->mkeys[i]->mkey, (uint64_t)mlx5_task, first_rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	}
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}

	assert(mlx5_task->num_submitted_reqs < mlx5_task->num_reqs);
	assert(mlx5_task->num_submitted_reqs < UINT16_MAX);
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (spdk_unlikely(qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return 0;
	}

	assert(task->num_reqs > task->num_completed_reqs);
	if (task->num_ops == 0) {
		/* No mkeys allocated, try to allocate now */
		if (spdk_unlikely(!accel_mlx5_task_alloc_mkeys(task))) {
			/* Pool is empty, queue this task */
			STAILQ_INSERT_TAIL(&dev->nomem, task, link);
			return -ENOMEM;
		}
	}
	/* We need to post at least 1 UMR and 1 RDMA operation */
	if (spdk_unlikely(qp_slot < 2)) {
		/* QP is full, queue this task */
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return -ENOMEM;
	}

	return accel_mlx5_task_process(task);
}

static inline int
accel_mlx5_task_init(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_dev *dev)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	uint64_t src_nbytes = task->nbytes;
#ifdef DEBUG
	uint64_t dst_nbytes;
	uint32_t i;
#endif
	switch (task->op_code) {
	case SPDK_ACCEL_OPC_ENCRYPT:
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
		break;
	case SPDK_ACCEL_OPC_DECRYPT:
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
		break;
	default:
		SPDK_ERRLOG("Unsupported accel opcode %d\n", task->op_code);
		return -ENOTSUP;
	}

	if (spdk_unlikely(src_nbytes % mlx5_task->base.block_size != 0)) {
		return -EINVAL;
	}

	mlx5_task->qp = &dev->qp;
	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	mlx5_task->num_ops = 0;
	mlx5_task->num_processed_blocks = 0;
	assert(src_nbytes / mlx5_task->base.block_size <= UINT16_MAX);
	mlx5_task->num_blocks = src_nbytes / mlx5_task->base.block_size;
	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	if (task->d.iovcnt == 0 || (task->d.iovcnt == task->s.iovcnt &&
				    accel_mlx5_compare_iovs(task->d.iovs, task->s.iovs, task->s.iovcnt))) {
		mlx5_task->inplace = 1;
	} else {
#ifdef DEBUG
		dst_nbytes = 0;
		for (i = 0; i < task->d.iovcnt; i++) {
			dst_nbytes += task->d.iovs[i].iov_len;
		}

		if (spdk_unlikely(src_nbytes != dst_nbytes)) {
			return -EINVAL;
		}
#endif
		mlx5_task->inplace = 0;
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
	}

	if (dev->crypto_multi_block) {
		if (dev->crypto_split_blocks) {
			assert(SPDK_CEIL_DIV(mlx5_task->num_blocks, dev->crypto_split_blocks) <= UINT16_MAX);
			mlx5_task->num_reqs = SPDK_CEIL_DIV(mlx5_task->num_blocks, dev->crypto_split_blocks);
			/* Last req may consume less blocks */
			mlx5_task->blocks_per_req = spdk_min(mlx5_task->num_blocks, dev->crypto_split_blocks);
		} else {
			if (task->s.iovcnt > ACCEL_MLX5_MAX_SGE || task->d.iovcnt > ACCEL_MLX5_MAX_SGE) {
				uint32_t max_sge_count = spdk_max(task->s.iovcnt, task->d.iovcnt);

				assert(SPDK_CEIL_DIV(max_sge_count, ACCEL_MLX5_MAX_SGE) <= UINT16_MAX);
				mlx5_task->num_reqs = SPDK_CEIL_DIV(max_sge_count, ACCEL_MLX5_MAX_SGE);
				mlx5_task->blocks_per_req = SPDK_CEIL_DIV(mlx5_task->num_blocks, mlx5_task->num_reqs);
			} else {
				mlx5_task->num_reqs = 1;
				mlx5_task->blocks_per_req = mlx5_task->num_blocks;
			}
		}
	} else {
		mlx5_task->num_reqs = mlx5_task->num_blocks;
		mlx5_task->blocks_per_req = 1;
	}

	if (spdk_unlikely(!accel_mlx5_task_alloc_mkeys(mlx5_task))) {
		/* Pool is empty, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in pool, dev %s\n", dev->dev_ctx->context->device->name);
		return -ENOMEM;
	}
	if (spdk_unlikely(accel_mlx5_dev_get_available_slots(dev, &dev->qp) < 2)) {
		/* Queue is full, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "dev %s qp %p is full\n", dev->dev_ctx->context->device->name,
			      mlx5_task->qp);
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(accel_mlx5, "task %p, src_iovs %u, dst_iovs %u, num_reqs %u, "
		      "blocks/req %u, blocks %u, inplace %d\n", task, task->s.iovcnt, task->d.iovcnt,
		      mlx5_task->num_reqs, mlx5_task->blocks_per_req, mlx5_task->num_blocks, mlx5_task->inplace);

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
			SPDK_DEBUGLOG(accel_mlx5, "no reqs to handle new task %p (required %u), put to queue\n", mlx5_task,
				      mlx5_task->num_reqs);
			STAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
			return 0;
		}
		return rc;
	}

	if (spdk_unlikely(mlx5_task->qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
		return 0;
	}

	return accel_mlx5_task_process(mlx5_task);
}

static void accel_mlx5_recover_qp(struct accel_mlx5_qp *qp);

static int
accel_mlx5_recover_qp_poller(void *arg)
{
	struct accel_mlx5_qp *qp = arg;

	spdk_poller_unregister(&qp->recover_poller);
	accel_mlx5_recover_qp(qp);
	return SPDK_POLLER_BUSY;
}

static void
accel_mlx5_recover_qp(struct accel_mlx5_qp *qp)
{
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
	int rc;

	SPDK_NOTICELOG("Recovering qp %p, core %u\n", qp, spdk_env_get_current_core());
	if (qp->qp) {
		spdk_mlx5_qp_destroy(qp->qp);
		qp->qp = NULL;
	}

	mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.attr.qp_size;
	mlx5_qp_attr.cap.max_recv_wr = 0;
	mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE;

	rc = spdk_mlx5_qp_create(dev->dev_ctx->pd, dev->cq, &mlx5_qp_attr, &qp->qp);
	if (rc) {
		SPDK_ERRLOG("Failed to create mlx5 dma QP, rc %d. Retry in %d usec\n",
			    rc, ACCEL_MLX5_RECOVER_POLLER_PERIOD_US);
		qp->recover_poller = SPDK_POLLER_REGISTER(accel_mlx5_recover_qp_poller, qp,
				     ACCEL_MLX5_RECOVER_POLLER_PERIOD_US);
		return;
	}

	qp->recovering = false;
}

static inline void
accel_mlx5_process_error_cpl(struct spdk_mlx5_cq_completion *wc, struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;

	if (wc->status != IBV_WC_WR_FLUSH_ERR) {
		SPDK_WARNLOG("RDMA: qp %p, task %p, WC status %d, core %u\n",
			     qp, task, wc->status, spdk_env_get_current_core());
	} else {
		SPDK_DEBUGLOG(accel_mlx5,
			      "RDMA: qp %p, task %p, WC status %d, core %u\n",
			      qp, task, wc->status, spdk_env_get_current_core());
	}

	qp->recovering = true;
	assert(task->num_completed_reqs <= task->num_submitted_reqs);
	if (task->num_completed_reqs == task->num_submitted_reqs) {
		STAILQ_REMOVE_HEAD(&qp->in_hw, link);
		accel_mlx5_task_fail(task, -EIO);
	}
}

static inline int64_t
accel_mlx5_poll_cq(struct accel_mlx5_dev *dev)
{
	struct spdk_mlx5_cq_completion wc[ACCEL_MLX5_MAX_WC];
	struct accel_mlx5_task *task;
	struct accel_mlx5_qp *qp;
	int reaped, i, rc;
	uint16_t completed;

	reaped = spdk_mlx5_cq_poll_completions(dev->cq, wc, ACCEL_MLX5_MAX_WC);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return reaped;
	} else if (reaped == 0) {
		return 0;
	}

	SPDK_DEBUGLOG(accel_mlx5, "Reaped %d cpls on dev %s\n", reaped,
		      dev->dev_ctx->context->device->name);

	for (i = 0; i < reaped; i++) {
		if (spdk_unlikely(!wc[i].wr_id)) {
			/* Unsignaled completion with error, ignore */
			continue;
		}
		task = (struct accel_mlx5_task *)wc[i].wr_id;
		qp = task->qp;
		assert(task == STAILQ_FIRST(&qp->in_hw) && "submission mismatch");
		assert(task->num_submitted_reqs > task->num_completed_reqs);
		completed = task->num_submitted_reqs - task->num_completed_reqs;
		assert((uint32_t)task->num_completed_reqs + completed <= UINT16_MAX);
		task->num_completed_reqs += completed;
		assert(qp->wrs_submitted >= task->num_wrs);
		qp->wrs_submitted -= task->num_wrs;
		assert(dev->wrs_in_cq > 0);
		dev->wrs_in_cq--;

		if (wc[i].status) {
			accel_mlx5_process_error_cpl(&wc[i], task);
			if (qp->wrs_submitted == 0) {
				assert(STAILQ_EMPTY(&qp->in_hw));
				accel_mlx5_recover_qp(qp);
			}
			continue;
		}

		SPDK_DEBUGLOG(accel_mlx5, "task %p, remaining %u\n", task,
			      task->num_reqs - task->num_completed_reqs);
		if (task->num_completed_reqs == task->num_reqs) {
			STAILQ_REMOVE_HEAD(&qp->in_hw, link);
			accel_mlx5_task_complete(task);
		} else {
			assert(task->num_submitted_reqs < task->num_reqs);
			assert(task->num_completed_reqs == task->num_submitted_reqs);
			STAILQ_REMOVE_HEAD(&qp->in_hw, link);
			rc = accel_mlx5_task_continue(task);
			if (spdk_unlikely(rc)) {
				if (rc != -ENOMEM) {
					accel_mlx5_task_fail(task, rc);
				}
			}
		}
	}

	return reaped;
}

static inline void
accel_mlx5_resubmit_nomem_tasks(struct accel_mlx5_dev *dev)
{
	struct accel_mlx5_task *task, *tmp, *last;
	int rc;

	last = STAILQ_LAST(&dev->nomem, accel_mlx5_task, link);
	STAILQ_FOREACH_SAFE(task, &dev->nomem, link, tmp) {
		STAILQ_REMOVE_HEAD(&dev->nomem, link);
		rc = accel_mlx5_task_continue(task);
		if (spdk_unlikely(rc)) {
			if (rc != -ENOMEM) {
				accel_mlx5_task_fail(task, rc);
			}
			break;
		}
		/* If qpair is recovering, task is added back to the nomem list and 0 is returned. In that case we
		 * need a special condition to iterate the list once and stop this FOREACH loop */
		if (task == last) {
			break;
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
		if (dev->wrs_in_cq) {
			rc = accel_mlx5_poll_cq(dev);
			if (spdk_unlikely(rc < 0)) {
				SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc, dev->dev_ctx->context->device->name);
			}
			completions += rc;
			if (dev->qp.wrs_submitted) {
				spdk_mlx5_qp_complete_send(dev->qp.qp);
			}
		}
		if (!STAILQ_EMPTY(&dev->nomem)) {
			accel_mlx5_resubmit_nomem_tasks(dev);
		}
	}

	return !!completions;
}

static bool
accel_mlx5_supports_opcode(enum spdk_accel_opcode opc)
{
	assert(g_accel_mlx5.enabled);

	switch (opc) {
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
		return g_accel_mlx5.crypto_supported;
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

static int
accel_mlx5_create_qp(struct accel_mlx5_dev *dev, struct accel_mlx5_qp *qp)
{
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {};
	int rc;

	mlx5_qp_attr.cap.max_send_wr = g_accel_mlx5.attr.qp_size;
	mlx5_qp_attr.cap.max_recv_wr = 0;
	mlx5_qp_attr.cap.max_send_sge = ACCEL_MLX5_MAX_SGE;
	mlx5_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * ACCEL_MLX5_MAX_SGE;

	rc = spdk_mlx5_qp_create(dev->dev_ctx->pd, dev->cq, &mlx5_qp_attr, &qp->qp);
	if (rc) {
		return rc;
	}

	STAILQ_INIT(&qp->in_hw);
	qp->dev = dev;
	qp->verbs_qp = spdk_mlx5_qp_get_verbs_qp(qp->qp);
	assert(qp->verbs_qp);
	qp->wrs_max = g_accel_mlx5.attr.qp_size;

	return 0;
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
		spdk_mlx5_qp_destroy(dev->qp.qp);
		if (dev->cq) {
			spdk_mlx5_cq_destroy(dev->cq);
		}
		spdk_poller_unregister(&dev->qp.recover_poller);
		if (dev->crypto_mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->crypto_mkeys);
		}
		spdk_rdma_utils_free_mem_map(&dev->mmap);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_mlx5_cq_attr cq_attr = {};
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

		if (dev_ctx->crypto_mkeys) {
			dev->crypto_mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
			if (!dev->crypto_mkeys) {
				SPDK_ERRLOG("Failed to get crypto mkey pool channel, dev %s\n", dev_ctx->context->device->name);
				/* Should not happen since mkey pool is created on accel_mlx5 initialization.
				 * We should not be here if pool creation failed */
				assert(0);
				goto err_out;
			}
		}

		memset(&cq_attr, 0, sizeof(cq_attr));
		cq_attr.cqe_cnt = g_accel_mlx5.attr.qp_size;
		cq_attr.cqe_size = 64;
		cq_attr.cq_context = dev;

		ch->num_devs++;
		rc = spdk_mlx5_cq_create(dev_ctx->pd, &cq_attr, &dev->cq);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 CQ, rc %d\n", rc);
			goto err_out;
		}

		rc = accel_mlx5_create_qp(dev, &dev->qp);
		if (rc) {
			SPDK_ERRLOG("Failed to create mlx5 QP, rc %d\n", rc);
			goto err_out;
		}

		dev->mmap = spdk_rdma_utils_create_mem_map(dev_ctx->pd, NULL,
				IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
		if (!dev->mmap) {
			SPDK_ERRLOG("Failed to create memory map\n");
			rc = -ENOMEM;
			goto err_out;
		}
		dev->crypto_multi_block = dev_ctx->crypto_multi_block;
		dev->crypto_split_blocks = dev_ctx->crypto_multi_block ? g_accel_mlx5.attr.crypto_split_blocks : 0;
		dev->wrs_in_cq_max = g_accel_mlx5.attr.qp_size;
		STAILQ_INIT(&dev->nomem);
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
	attr->allowed_devs = NULL;
	attr->crypto_split_blocks = 0;
}

static void
accel_mlx5_allowed_devs_free(void)
{
	size_t i;

	if (!g_accel_mlx5.allowed_devs) {
		return;
	}

	for (i = 0; i < g_accel_mlx5.allowed_devs_count; i++) {
		free(g_accel_mlx5.allowed_devs[i]);
	}
	free(g_accel_mlx5.attr.allowed_devs);
	free(g_accel_mlx5.allowed_devs);
	g_accel_mlx5.attr.allowed_devs = NULL;
	g_accel_mlx5.allowed_devs = NULL;
	g_accel_mlx5.allowed_devs_count = 0;
}

static int
accel_mlx5_allowed_devs_parse(const char *allowed_devs)
{
	char *str, *tmp, *tok;
	size_t devs_count = 0;

	str = strdup(allowed_devs);
	if (!str) {
		return -ENOMEM;
	}

	accel_mlx5_allowed_devs_free();

	tmp = str;
	while ((tmp = strchr(tmp, ',')) != NULL) {
		tmp++;
		devs_count++;
	}
	devs_count++;

	g_accel_mlx5.allowed_devs = calloc(devs_count, sizeof(char *));
	if (!g_accel_mlx5.allowed_devs) {
		free(str);
		return -ENOMEM;
	}

	devs_count = 0;
	tok = strtok(str, ",");
	while (tok) {
		g_accel_mlx5.allowed_devs[devs_count] = strdup(tok);
		if (!g_accel_mlx5.allowed_devs[devs_count]) {
			free(str);
			accel_mlx5_allowed_devs_free();
			return -ENOMEM;
		}
		tok = strtok(NULL, ",");
		devs_count++;
		g_accel_mlx5.allowed_devs_count++;
	}

	free(str);

	return 0;
}

int
accel_mlx5_enable(struct accel_mlx5_attr *attr)
{
	int rc;

	if (g_accel_mlx5.enabled) {
		return -EEXIST;
	}
	if (attr) {
		g_accel_mlx5.attr = *attr;
		g_accel_mlx5.attr.allowed_devs = NULL;

		if (attr->allowed_devs) {
			/* Contains a copy of user's string */
			g_accel_mlx5.attr.allowed_devs = strndup(attr->allowed_devs, ACCEL_MLX5_ALLOWED_DEVS_MAX_LEN);
			if (!g_accel_mlx5.attr.allowed_devs) {
				return -ENOMEM;
			}
			rc = accel_mlx5_allowed_devs_parse(g_accel_mlx5.attr.allowed_devs);
			if (rc) {
				return rc;
			}
			rc = spdk_mlx5_crypto_devs_allow((const char *const *)g_accel_mlx5.allowed_devs,
							 g_accel_mlx5.allowed_devs_count);
			if (rc) {
				accel_mlx5_allowed_devs_free();
				return rc;
			}
		}
	} else {
		accel_mlx5_get_default_attr(&g_accel_mlx5.attr);
	}

	g_accel_mlx5.enabled = true;
	spdk_accel_module_list_add(&g_accel_mlx5.module);

	return 0;
}

static void
accel_mlx5_free_resources(void)
{
	struct accel_mlx5_crypto_dev_ctx *dev_ctx;
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_crypto_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.crypto_ctxs[i];
		if (dev_ctx->pd) {
			if (dev_ctx->crypto_mkeys) {
				spdk_mlx5_mkey_pool_destroy(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO, dev_ctx->pd);
			}
			spdk_rdma_utils_put_pd(dev_ctx->pd);
		}
		if (dev_ctx->domain) {
			spdk_rdma_utils_put_memory_domain(dev_ctx->domain);
		}
	}

	free(g_accel_mlx5.crypto_ctxs);
	g_accel_mlx5.crypto_ctxs = NULL;
	g_accel_mlx5.initialized = false;
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
	if (g_accel_mlx5.allowed_devs) {
		accel_mlx5_allowed_devs_free();
	}
	spdk_mlx5_crypto_devs_allow(NULL, 0);
	if (g_accel_mlx5.initialized) {
		spdk_io_device_unregister(&g_accel_mlx5, accel_mlx5_deinit_cb);
	} else {
		spdk_accel_module_finish();
	}
}

static int
accel_mlx5_mkeys_create(struct ibv_pd *pd, uint32_t num_mkeys, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool_param pool_param = {};

	pool_param.mkey_count = num_mkeys;
	pool_param.cache_per_thread = num_mkeys * 3 / 4 / spdk_env_get_core_count();
	pool_param.flags = flags;

	return spdk_mlx5_mkey_pool_init(&pool_param, pd);
}

static int
accel_mlx5_dev_ctx_init(struct accel_mlx5_crypto_dev_ctx *dev_ctx, struct ibv_context *dev,
			struct spdk_mlx5_device_caps *caps)
{
	struct ibv_pd *pd;
	int rc;

	pd = spdk_rdma_utils_get_pd(dev);
	if (!pd) {
		SPDK_ERRLOG("Failed to get PD for context %p, dev %s\n", dev, dev->device->name);
		return -EINVAL;
	}
	dev_ctx->context = dev;
	dev_ctx->pd = pd;
	dev_ctx->domain = spdk_rdma_utils_get_memory_domain(pd);
	if (!dev_ctx->domain) {
		return -ENOMEM;
	}

	if (g_accel_mlx5.crypto_supported) {
		dev_ctx->crypto_multi_block = caps->crypto.multi_block_be_tweak;
		if (!dev_ctx->crypto_multi_block && g_accel_mlx5.attr.crypto_split_blocks) {
			SPDK_WARNLOG("\"crypto_split_blocks\" is set but dev %s doesn't support multi block crypto\n",
				     dev->device->name);
		}
		rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests, SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO);
		if (rc) {
			SPDK_ERRLOG("Failed to create crypto mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		dev_ctx->crypto_mkeys = true;
	}

	return 0;
}

static struct ibv_context **
accel_mlx5_get_devices(int *_num_devs)
{
	struct ibv_context **rdma_devs, **rdma_devs_out = NULL, *dev;
	struct ibv_device_attr dev_attr;
	size_t j;
	int num_devs = 0, i, rc;
	int num_devs_out = 0;
	bool dev_allowed;

	rdma_devs = rdma_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		*_num_devs = 0;
		return NULL;
	}

	rdma_devs_out = calloc(num_devs + 1, sizeof(struct ibv_context *));
	if (!rdma_devs_out) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rdma_free_devices(rdma_devs);
		*_num_devs = 0;
		return NULL;
	}

	for (i = 0; i < num_devs; i++) {
		dev = rdma_devs[i];
		rc = ibv_query_device(dev, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Failed to query dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (dev_attr.vendor_id != SPDK_MLX5_VENDOR_ID_MELLANOX) {
			SPDK_DEBUGLOG(accel_mlx5, "dev %s is not Mellanox device, skipping\n", dev->device->name);
			continue;
		}

		if (g_accel_mlx5.allowed_devs_count) {
			dev_allowed = false;
			for (j = 0; j < g_accel_mlx5.allowed_devs_count; j++) {
				if (strcmp(g_accel_mlx5.allowed_devs[j], dev->device->name) == 0) {
					dev_allowed = true;
					break;
				}
			}
			if (!dev_allowed) {
				continue;
			}
		}

		rdma_devs_out[num_devs_out] = dev;
		num_devs_out++;
	}

	rdma_free_devices(rdma_devs);
	*_num_devs = num_devs_out;

	return rdma_devs_out;
}

static inline bool
accel_mlx5_dev_supports_crypto(struct spdk_mlx5_device_caps *caps)
{
	return caps->crypto_supported && !caps->crypto.wrapped_import_method_aes_xts &&
	       (caps->crypto.single_block_le_tweak ||
		caps->crypto.multi_block_le_tweak || caps->crypto.multi_block_be_tweak);
}

static int
accel_mlx5_init(void)
{
	struct spdk_mlx5_device_caps *caps;
	struct ibv_context **rdma_devs, *dev;
	int num_devs = 0,  rc = 0, i;
	int best_dev = -1, first_dev = 0;
	bool supports_crypto;
	bool find_best_dev = g_accel_mlx5.allowed_devs_count == 0;

	if (!g_accel_mlx5.enabled) {
		return -EINVAL;
	}

	rdma_devs = accel_mlx5_get_devices(&num_devs);
	if (!rdma_devs || !num_devs) {
		return -ENODEV;
	}
	caps = calloc(num_devs, sizeof(*caps));
	if (!caps) {
		rc = -ENOMEM;
		goto cleanup;
	}

	g_accel_mlx5.crypto_supported = true;
	g_accel_mlx5.num_crypto_ctxs = 0;

	/* Iterate devices. We support an offload if all devices support it */
	for (i = 0; i < num_devs; i++) {
		dev = rdma_devs[i];

		rc = spdk_mlx5_device_query_caps(dev, &caps[i]);
		if (rc) {
			SPDK_ERRLOG("Failed to get crypto caps, dev %s\n", dev->device->name);
			goto cleanup;
		}
		supports_crypto = accel_mlx5_dev_supports_crypto(&caps[i]);
		if (!supports_crypto) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crypto support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crypto_supported = false;
		}
		if (find_best_dev) {
			if (supports_crypto && best_dev == -1) {
				best_dev = i;
			}
		}
	}

	/* User didn't specify devices to use, try to select the best one */
	if (find_best_dev) {
		if (best_dev == -1) {
			best_dev = 0;
		}
		supports_crypto = accel_mlx5_dev_supports_crypto(&caps[best_dev]);
		SPDK_NOTICELOG("Select dev %s, crypto %d\n", rdma_devs[best_dev]->device->name, supports_crypto);
		g_accel_mlx5.crypto_supported = supports_crypto;
		first_dev = best_dev;
		num_devs = 1;
		if (supports_crypto) {
			const char *const dev_name[] = { rdma_devs[best_dev]->device->name };
			/* Let mlx5 library know which device to use */
			spdk_mlx5_crypto_devs_allow(dev_name, 1);
		}
	} else {
		SPDK_NOTICELOG("Found %d devices, crypto %d\n", num_devs, g_accel_mlx5.crypto_supported);
	}

	if (!g_accel_mlx5.crypto_supported) {
		/* Now accel_mlx5 supports only crypto, exit if no devs supports crypto offload */
		rc = -ENODEV;
		goto cleanup;
	}

	g_accel_mlx5.crypto_ctxs = calloc(num_devs, sizeof(*g_accel_mlx5.crypto_ctxs));
	if (!g_accel_mlx5.crypto_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = first_dev; i < first_dev + num_devs; i++) {
		rc = accel_mlx5_dev_ctx_init(&g_accel_mlx5.crypto_ctxs[g_accel_mlx5.num_crypto_ctxs++],
					     rdma_devs[i], &caps[i]);
		if (rc) {
			goto cleanup;
		}
	}

	SPDK_NOTICELOG("Accel framework mlx5 initialized, found %d devices.\n", num_devs);
	spdk_io_device_register(&g_accel_mlx5, accel_mlx5_create_cb, accel_mlx5_destroy_cb,
				sizeof(struct accel_mlx5_io_channel), "accel_mlx5");
	g_accel_mlx5.initialized = true;
	free(rdma_devs);
	free(caps);

	return 0;

cleanup:
	free(rdma_devs);
	free(caps);
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
		if (g_accel_mlx5.attr.allowed_devs) {
			spdk_json_write_named_string(w, "allowed_devs", g_accel_mlx5.attr.allowed_devs);
		}
		spdk_json_write_named_uint16(w, "crypto_split_blocks", g_accel_mlx5.attr.crypto_split_blocks);
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

static bool
accel_mlx5_crypto_supports_cipher(enum spdk_accel_cipher cipher, size_t key_size)
{
	switch (cipher) {
	case SPDK_ACCEL_CIPHER_AES_XTS:
		return key_size == SPDK_ACCEL_AES_XTS_128_KEY_SIZE || key_size == SPDK_ACCEL_AES_XTS_256_KEY_SIZE;
	default:
		return false;
	}
}

static int
accel_mlx5_get_memory_domains(struct spdk_memory_domain **domains, int array_size)
{
	int i, size;

	if (!domains || !array_size) {
		return (int)g_accel_mlx5.num_crypto_ctxs;
	}

	size = spdk_min(array_size, (int)g_accel_mlx5.num_crypto_ctxs);

	for (i = 0; i < size; i++) {
		domains[i] = g_accel_mlx5.crypto_ctxs[i].domain;
	}

	return (int)g_accel_mlx5.num_crypto_ctxs;
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
		.crypto_supports_cipher	= accel_mlx5_crypto_supports_cipher,
		.get_memory_domains	= accel_mlx5_get_memory_domains,
	}
};

SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
