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
	(qp)->ring_db = true;				\
	assert((task)->num_wrs < UINT16_MAX);		\
	(task)->num_wrs++;				\
} while (0)

#define ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, task)	\
do {									\
	assert((dev)->wrs_in_cq < (dev)->wrs_in_cq_max);		\
	(dev)->wrs_in_cq++;						\
        assert((qp)->wrs_submitted < (qp)->wrs_max);			\
	(qp)->wrs_submitted++;						\
	(qp)->ring_db = true;						\
	assert((task)->num_wrs < UINT16_MAX);				\
	(task)->num_wrs++;						\
} while (0)

struct accel_mlx5_io_channel;
struct accel_mlx5_task;

struct accel_mlx5_dev_ctx {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct spdk_memory_domain *domain;
	struct spdk_mempool *psv_pool;
	TAILQ_ENTRY(accel_mlx5_dev_ctx) link;
	struct spdk_mlx5_psv **psvs;
	bool mkeys;
	bool crypto_mkeys;
	bool sig_mkeys;
	bool crypto_multi_block;
};

enum accel_mlx5_opcode {
	ACCEL_MLX5_OPC_COPY,
	ACCEL_MLX5_OPC_CRYPTO,
	ACCEL_MLX5_OPC_CRC32C,
	ACCEL_MLX5_OPC_CRYPTO_MKEY,
	ACCEL_MLX5_OPC_MKEY,
	ACCEL_MLX5_OPC_LAST
};

SPDK_STATIC_ASSERT(ACCEL_MLX5_OPC_LAST <= 0xf,
		   "accel opcode exceeds 4 bits, update accel_mlx5 struct");

struct accel_mlx5_stats {
	uint64_t crypto_umrs;
	uint64_t sig_umrs;
	uint64_t umrs;
	uint64_t rdma_reads;
	uint64_t rdma_writes;
	uint64_t polls;
	uint64_t idle_polls;
	uint64_t completions;
	uint64_t nomem_qdepth;
	uint64_t nomem_mkey;
	uint64_t opcodes[ACCEL_MLX5_OPC_LAST];
};

struct accel_mlx5_module {
	struct spdk_accel_module_if module;
	struct accel_mlx5_stats stats;
	struct spdk_spinlock lock;
	struct accel_mlx5_dev_ctx *dev_ctxs;
	uint32_t num_ctxs;
	struct accel_mlx5_attr attr;
	char **allowed_devs;
	size_t allowed_devs_count;
	bool initialized;
	bool enabled;
	bool crypto_supported;
	bool crc32c_supported;
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

struct accel_mlx5_psv_wrapper {
	uint32_t psv_index;
	struct {
		uint32_t error : 1;
		uint32_t reserved : 31;
	} bits;
	/* mlx5 engine requires DMAable memory, use this member to copy user's crc value since we don't know which
	 * memory it is in */
	uint32_t crc;
	uint32_t crc_lkey;
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
	uint16_t num_ops; /* number of allocated mkeys or number of operations */
	uint16_t num_wrs; /* Number of outstanding operations which consume qp slot */
	union {
		struct {
			uint16_t blocks_per_req;
			uint16_t num_processed_blocks;
			uint16_t num_blocks;
		};
		struct {
			struct accel_mlx5_psv_wrapper *psv;
			uint32_t last_umr_len;
			uint8_t last_mkey_idx;
		};
	};
	union {
		uint16_t raw;
		struct {
			uint16_t inplace : 1;
			uint16_t driver_seq : 1;
			uint16_t needs_data_transfer : 1;
			uint16_t enc_order : 2;
			uint16_t mlx5_opcode: 4;
		};
	};
	/* Keep this array last since not all elements might be accessed, this reduces amount of data to be
	 * cached */
	struct spdk_mlx5_mkey_pool_obj *mkeys[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
};

SPDK_STATIC_ASSERT(ACCEL_MLX5_MAX_MKEYS_IN_TASK <= UINT8_MAX, "uint8_t is used to iterate mkeys");

struct accel_mlx5_qp {
	struct spdk_mlx5_qp *qp;
	struct ibv_qp *verbs_qp;
	struct accel_mlx5_dev *dev;
	/* tasks submitted to HW. We can't complete a task even in error case until we reap completions for all
	 * submitted requests */
	STAILQ_HEAD(, accel_mlx5_task) in_hw;
	uint16_t wrs_submitted;
	uint16_t wrs_max;
	bool ring_db;
	bool recovering;
	struct spdk_poller *recover_poller;
};

struct accel_mlx5_dev {
	struct accel_mlx5_qp qp;
	struct spdk_mlx5_cq *cq;
	struct spdk_mlx5_mkey_pool *mkeys;
	struct spdk_mlx5_mkey_pool *crypto_mkeys;
	struct spdk_mlx5_mkey_pool *sig_mkeys;
	struct spdk_rdma_utils_mem_map *mmap;
	struct accel_mlx5_dev_ctx *dev_ctx;
	struct spdk_io_channel *ch;
	uint16_t wrs_in_cq;
	uint16_t wrs_in_cq_max;
	uint16_t crypto_split_blocks;
	bool crypto_multi_block;
	/* Pending tasks waiting for requests resources */
	STAILQ_HEAD(, accel_mlx5_task) nomem;
	TAILQ_ENTRY(accel_mlx5_dev) link;
	struct accel_mlx5_stats stats;
};

struct accel_mlx5_io_channel {
	struct accel_mlx5_dev *devs;
	struct spdk_poller *poller;
	uint16_t num_devs;
	/* Index in \b devs to be used for operations in round-robin way */
	uint16_t dev_idx;
	bool poller_handler_registered;
};

struct accel_mlx5_task_operations {
	int (*init)(struct accel_mlx5_task *task);
	int (*process)(struct accel_mlx5_task *task);
	int (*cont)(struct accel_mlx5_task *task);
	void (*complete)(struct accel_mlx5_task *task);
};

struct accel_mlx5_psv_pool_iter_cb_args {
	struct accel_mlx5_dev_ctx *dev;
	struct spdk_rdma_utils_mem_map *map;
	int rc;
};

struct accel_mlx5_dump_stats_ctx {
	struct accel_mlx5_stats total;
	struct spdk_json_write_ctx *w;
	enum accel_mlx5_dump_state_level level;
	accel_mlx5_dump_stat_done_cb cb;
	void *ctx;
};

static struct accel_mlx5_module g_accel_mlx5;
static struct spdk_accel_driver g_accel_mlx5_driver;

static inline int accel_mlx5_execute_sequence(struct spdk_io_channel *ch,
		struct spdk_accel_sequence *seq);
static inline void accel_mlx5_task_complete(struct accel_mlx5_task *mlx5_task);

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
accel_mlx5_iov_sgl_unwind(struct accel_mlx5_iov_sgl *s, uint32_t max_iovs, uint32_t step)
{
	SPDK_DEBUGLOG(accel_mlx5, "iov %p, iovcnt %u, max %u, offset %u, step %u\n", s->iov, s->iovcnt,
		      max_iovs, s->iov_offset, step);
	while (s->iovcnt <= max_iovs) {
		assert(s->iov != NULL);
		if (s->iov_offset >= step) {
			s->iov_offset -= step;
			SPDK_DEBUGLOG(accel_mlx5, "\tEND, iov %p, iovcnt %u, offset %u\n", s->iov, s->iovcnt,
				      s->iov_offset);
			return;
		}
		step -= s->iov_offset;
		s->iov--;
		s->iovcnt++;
		s->iov_offset = s->iov->iov_len;
		SPDK_DEBUGLOG(accel_mlx5, "\tiov %p, iovcnt %u, offset %u, step %u\n", s->iov, s->iovcnt,
			      s->iov_offset, step);
	}

	SPDK_ERRLOG("Can't unwind iovs, remaining  %u\n", step);
	assert(0);
}

static inline int
accel_mlx5_sge_unwind(struct ibv_sge *sge, uint32_t sge_count, uint32_t step)
{
	int i;

	assert(sge_count > 0);
	SPDK_DEBUGLOG(accel_mlx5, "sge %p, count %u, step %u\n", sge, sge_count, step);
	for (i = (int)sge_count - 1; i >= 0; i--) {
		if (sge[i].length > step) {
			sge[i].length -= step;
			SPDK_DEBUGLOG(accel_mlx5, "\tsge[%u] len %u, step %u\n", i, sge[i].length, step);
			return (int)i + 1;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\tsge[%u] len %u, step %u\n", i, sge[i].length, step);
		step -= sge[i].length;
	}

	SPDK_ERRLOG("Can't unwind sge, remaining  %u\n", step);
	assert(step == 0);

	return 0;
}

static inline void
accel_mlx5_crypto_task_complete(struct accel_mlx5_task *task)
{
	struct accel_mlx5_dev *dev = task->qp->dev;

	assert(task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
	spdk_accel_task_complete(&task->base, 0);
}

static inline void
accel_mlx5_task_fail(struct accel_mlx5_task *task, int rc)
{
	struct accel_mlx5_dev *dev = task->qp->dev;
	struct spdk_accel_task *next;
	struct spdk_accel_sequence *seq;
	bool driver_seq;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Fail task %p, opc %d, rc %d\n", task, task->base.op_code, rc);

	if (task->num_ops) {
		if (task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO || task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO_MKEY) {
			spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, task->mkeys, task->num_ops);
		}
		if (task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C) {
			spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, task->mkeys, task->num_ops);
			spdk_mempool_put(dev->dev_ctx->psv_pool, task->psv);
		}
		if (task->mlx5_opcode == ACCEL_MLX5_OPC_MKEY) {
			spdk_mlx5_mkey_pool_put_bulk(dev->mkeys, task->mkeys, task->num_ops);
		}
	}
	next = spdk_accel_sequence_next_task(&task->base);
	seq = task->base.seq;
	driver_seq = task->driver_seq;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Fail task %p, opc %d, rc %d\n", task, task->mlx5_opcode, rc);
	spdk_accel_task_complete(&task->base, rc);

	if (driver_seq) {
		struct spdk_io_channel *ch = task->qp->dev->ch;

		assert(seq);
		if (next) {
			accel_mlx5_execute_sequence(ch, seq);
		} else {
			spdk_accel_sequence_continue(seq);
		}
	}
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
			  struct accel_mlx5_iov_sgl *iovs, uint32_t len, uint32_t *_remaining,
			  struct spdk_memory_domain *domain, void *domain_ctx)
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
	*_remaining = remaining;

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
accel_mlx5_task_alloc_mkeys(struct accel_mlx5_task *task, struct spdk_mlx5_mkey_pool *pool)
{
	uint32_t num_ops;
	int rc;

	assert(task->num_reqs > task->num_completed_reqs);
	num_ops = task->num_reqs - task->num_completed_reqs;
	num_ops = spdk_min(num_ops, ACCEL_MLX5_MAX_MKEYS_IN_TASK);
	if (!num_ops) {
		return 0;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(pool, task->mkeys, num_ops);
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
				uint32_t mkey, uint32_t num_blocks, struct spdk_mlx5_crypto_dek_data *dek_data,
				uint64_t wr_id, uint32_t flags)
{
	struct spdk_mlx5_umr_crypto_attr cattr;
	struct spdk_mlx5_umr_attr umr_attr;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_accel_task *task = &mlx5_task->base;
	uint32_t length, remaining = 0, block_size = task->block_size;
	int rc;

	length = num_blocks * block_size;
	SPDK_DEBUGLOG(accel_mlx5, "task %p, domain %p, len %u, blocks %u\n", task, task->src_domain, length,
		      num_blocks);
	rc = accel_mlx5_fill_block_sge(dev, sge->src_sge, &mlx5_task->src,  length, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sge->src_sge_count = rc;
	if (spdk_unlikely(remaining)) {
		uint32_t new_len = length - remaining;
		uint32_t aligned_len, updated_num_blocks;

		SPDK_DEBUGLOG(accel_mlx5, "Incorrect src iovs, handled %u out of %u bytes\n", new_len, length);
		if (new_len < block_size) {
			/* We need to process at least 1 block. If buffer is too fragmented, we can't do
			 * anything */
			return -ERANGE;
		}

		/* Regular integer division, we need to round down to prev block size */
		updated_num_blocks = new_len / block_size;
		assert(updated_num_blocks);
		assert(updated_num_blocks < num_blocks);
		aligned_len = updated_num_blocks * block_size;

		if (aligned_len < new_len) {
			uint32_t dt = new_len - aligned_len;

			/* We can't process part of block, need to unwind src iov_sgl and sge to the
			 * prev block boundary */
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src sge for %u bytes\n", task, dt);
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			sge->src_sge_count = accel_mlx5_sge_unwind(sge->src_sge, sge->src_sge_count, dt);
			if (!sge->src_sge_count) {
				return -ERANGE;
			}
		}
		SPDK_DEBUGLOG(accel_mlx5, "task %p, UMR len %u -> %u\n", task, length, aligned_len);
		length = aligned_len;
		num_blocks = updated_num_blocks;
	}

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
		rc = accel_mlx5_fill_block_sge(dev, sge->dst_sge, &mlx5_task->dst, length, &remaining,
					       task->dst_domain, task->dst_domain_ctx);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		sge->dst_sge_count = rc;
		if (spdk_unlikely(remaining)) {
			uint32_t new_len = length - remaining;
			uint32_t aligned_len, updated_num_blocks, dt;

			SPDK_DEBUGLOG(accel_mlx5, "Incorrect dst iovs, handled %u out of %u bytes\n", new_len, length);
			if (new_len < block_size) {
				/* We need to process at least 1 block. If buffer is too fragmented, we can't do
				 * anything */
				return -ERANGE;
			}

			/* Regular integer division, we need to round down to prev block size */
			updated_num_blocks = new_len / block_size;
			assert(updated_num_blocks);
			assert(updated_num_blocks < num_blocks);
			aligned_len = updated_num_blocks * block_size;

			if (aligned_len < new_len) {
				dt = new_len - aligned_len;
				assert(dt > 0 && dt < length);
				/* We can't process part of block, need to unwind src and dst iov_sgl and sge to the
				 * prev block boundary */
				SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind dst sge for %u bytes\n", task, dt);
				accel_mlx5_iov_sgl_unwind(&mlx5_task->dst, task->d.iovcnt, dt);
				sge->dst_sge_count = accel_mlx5_sge_unwind(sge->dst_sge, sge->dst_sge_count, dt);
				assert(sge->dst_sge_count > 0 && sge->dst_sge_count <= ACCEL_MLX5_MAX_SGE);
				if (!sge->dst_sge_count) {
					return -ERANGE;
				}
			}
			assert(length > aligned_len);
			dt = length - aligned_len;
			SPDK_DEBUGLOG(accel_mlx5, "task %p, unwind src sge for %u bytes\n", task, dt);
			/* The same for src iov_sgl and sge. In worst case we can unwind SRC 2 times */
			accel_mlx5_iov_sgl_unwind(&mlx5_task->src, task->s.iovcnt, dt);
			sge->src_sge_count = accel_mlx5_sge_unwind(sge->src_sge, sge->src_sge_count, dt);
			assert(sge->src_sge_count > 0 && sge->src_sge_count <= ACCEL_MLX5_MAX_SGE);
			if (!sge->src_sge_count) {
				return -ERANGE;
			}
			SPDK_DEBUGLOG(accel_mlx5, "task %p, UMR len %u -> %u\n", task, length, aligned_len);
			length = aligned_len;
			num_blocks = updated_num_blocks;
		}
	}

	SPDK_DEBUGLOG(accel_mlx5,
		      "task %p: bs %u, iv %"PRIu64", enc_on_tx %d, tweak_mode %d, len %u, mkey %x, blocks %u\n",
		      mlx5_task, task->block_size, cattr.xts_iv, mlx5_task->enc_order, cattr.tweak_mode, length, mkey,
		      num_blocks);

	umr_attr.sge_count = sge->src_sge_count;
	umr_attr.umr_len = length;
	assert((uint32_t)mlx5_task->num_processed_blocks + num_blocks <= UINT16_MAX);
	mlx5_task->num_processed_blocks += num_blocks;

	rc = spdk_mlx5_umr_configure_crypto(qp->qp, &umr_attr, &cattr, wr_id, flags);

	return rc;
}

static inline int
accel_mlx5_crypto_task_process(struct accel_mlx5_task *mlx5_task)
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
						     &dek_data, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		dev->stats.crypto_umrs++;
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
		dev->stats.rdma_reads++;
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
	dev->stats.rdma_reads++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	if (spdk_unlikely(mlx5_task->num_submitted_reqs == mlx5_task->num_reqs &&
			  mlx5_task->num_blocks > mlx5_task->num_processed_blocks)) {
		/* We hit "out of sge
		 * entries" case with highly fragmented payload. In that case
		 * accel_mlx5_configure_crypto_umr function handled fewer data blocks than expected
		 * That means we need at least 1 more request to complete this task, this request will be
		 * executed once all submitted ones are completed */
		SPDK_DEBUGLOG(accel_mlx5, "task %p, processed %u/%u blocks, add extra req\n", mlx5_task,
			      mlx5_task->num_processed_blocks, mlx5_task->num_blocks);
		mlx5_task->num_reqs++;
	}

	SPDK_DEBUGLOG(accel_mlx5, "end, task, %p, reqs: total %u, submitted %u, completed %u\n", mlx5_task,
		      mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	return 0;
}

static inline int
accel_mlx5_crypto_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	assert(task->num_reqs > task->num_completed_reqs);
	if (task->num_ops == 0) {
		/* No mkeys allocated, try to allocate now */
		if (spdk_unlikely(!accel_mlx5_task_alloc_mkeys(task, dev->crypto_mkeys))) {
			/* Pool is empty, queue this task */
			STAILQ_INSERT_TAIL(&dev->nomem, task, link);
			dev->stats.nomem_mkey++;
			return -ENOMEM;
		}
	}
	/* We need to post at least 1 UMR and 1 RDMA operation */
	if (spdk_unlikely(qp_slot < 2)) {
		/* QP is full, queue this task */
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		task->qp->dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}

	return accel_mlx5_crypto_task_process(task);
}

static inline int
accel_mlx5_crypto_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	uint64_t src_nbytes = task->nbytes;
#ifdef DEBUG
	uint64_t dst_nbytes;
	uint32_t i;
#endif
	bool crypto_key_ok;

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely((task->nbytes % mlx5_task->base.block_size != 0) || !crypto_key_ok)) {
		if (crypto_key_ok) {
			SPDK_ERRLOG("src length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
				    mlx5_task->base.block_size);
		} else {
			SPDK_ERRLOG("Wrong crypto key provided\n");
		}
		return -EINVAL;
	}

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

	if (spdk_unlikely(!accel_mlx5_task_alloc_mkeys(mlx5_task, dev->crypto_mkeys))) {
		/* Pool is empty, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in pool, dev %s\n", dev->dev_ctx->context->device->name);
		dev->stats.nomem_mkey++;
		return -ENOMEM;
	}
	if (spdk_unlikely(accel_mlx5_dev_get_available_slots(dev, &dev->qp) < 2)) {
		/* Queue is full, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "dev %s qp %p is full\n", dev->dev_ctx->context->device->name,
			      mlx5_task->qp);
		dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(accel_mlx5, "task %p, src_iovs %u, dst_iovs %u, num_reqs %u, "
		      "blocks/req %u, blocks %u, inplace %d\n", task, task->s.iovcnt, task->d.iovcnt,
		      mlx5_task->num_reqs, mlx5_task->blocks_per_req, mlx5_task->num_blocks, mlx5_task->inplace);

	return 0;
}

static inline void
accel_mlx5_copy_task_complete(struct accel_mlx5_task *mlx5_task)
{
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_copy_task_process_one(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_qp *qp,
				 uint64_t wrid, uint32_t fence)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_sge sge;
	uint32_t remaining = 0;
	uint32_t dst_len;
	int rc;

	/* Limit one RDMA_WRITE by length of dst buffer. Not all src buffers may fit into one dst buffer due to
	 * limitation on ACCEL_MLX5_MAX_SGE. If this is the case then remaining is not zero */
	assert(mlx5_task->dst.iov->iov_len > mlx5_task->dst.iov_offset);
	dst_len = mlx5_task->dst.iov->iov_len - mlx5_task->dst.iov_offset;
	rc = accel_mlx5_fill_block_sge(qp->dev, sge.src_sge, &mlx5_task->src, dst_len, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	sge.src_sge_count = rc;
	assert(dst_len > remaining);
	dst_len -= remaining;

	rc = accel_mlx5_fill_block_sge(qp->dev, sge.dst_sge, &mlx5_task->dst, dst_len,  &remaining,
				       task->dst_domain, task->dst_domain_ctx);
	if (spdk_unlikely(rc != 1)) {
		/* We use single dst entry, any result other than 1 is an error */
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
		return rc;
	}
	if (spdk_unlikely(remaining)) {
		SPDK_ERRLOG("Incorrect dst length, remaining %u\n", remaining);
		assert(0);
		return -EINVAL;
	}

	rc = spdk_mlx5_qp_rdma_write(mlx5_task->qp->qp, sge.src_sge, sge.src_sge_count,
				     sge.dst_sge[0].addr, sge.dst_sge[0].lkey, wrid, fence);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("new RDMA WRITE failed with %d\n", rc);
		return rc;
	}
	qp->dev->stats.rdma_writes++;

	return 0;
}

static inline int
accel_mlx5_copy_task_process(struct accel_mlx5_task *mlx5_task)
{

	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t i;
	int rc;

	mlx5_task->num_wrs = 0;
	assert(mlx5_task->num_reqs > 0);
	assert(mlx5_task->num_ops > 0);

	/* Handle n-1 reqs in order to simplify wrid and fence handling */
	for (i = 0; i < mlx5_task->num_ops - 1; i++) {
		rc = accel_mlx5_copy_task_process_one(mlx5_task, qp, 0, 0);
		if (spdk_unlikely(rc)) {
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		mlx5_task->num_submitted_reqs++;
	}

	rc = accel_mlx5_copy_task_process_one(mlx5_task, qp, (uint64_t)mlx5_task,
					      SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	mlx5_task->num_submitted_reqs++;
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, copy task, %p\n", mlx5_task);

	return 0;
}

static inline int
accel_mlx5_copy_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	task->num_ops = spdk_min(qp_slot, task->num_reqs - task->num_completed_reqs);
	if (spdk_unlikely(task->num_ops == 0)) {
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}
	return accel_mlx5_copy_task_process(task);
}

static inline uint32_t
accel_mlx5_get_copy_task_count(struct iovec *src_iov, uint32_t src_iovcnt,
			       struct iovec *dst_iov, uint32_t dst_iovcnt)
{
	uint32_t src = 0;
	uint32_t dst = 0;
	uint64_t src_offset = 0;
	uint64_t dst_offset = 0;
	uint32_t num_ops = 0;
	uint32_t src_sge_count = 0;

	while (src < src_iovcnt && dst < dst_iovcnt) {
		uint64_t src_len = src_iov[src].iov_len - src_offset;
		uint64_t dst_len = dst_iov[dst].iov_len - dst_offset;

		if (dst_len < src_len) {
			dst_offset = 0;
			src_offset += dst_len;
			dst++;
			num_ops++;
			src_sge_count = 0;
		} else if (src_len < dst_len) {
			dst_offset += src_len;
			src_offset = 0;
			src++;
			if (++src_sge_count >= ACCEL_MLX5_MAX_SGE) {
				num_ops++;
				src_sge_count = 0;
			}
		} else {
			dst_offset = 0;
			src_offset = 0;
			dst++;
			src++;
			num_ops++;
			src_sge_count = 0;
		}
	}

	assert(src == src_iovcnt);
	assert(dst == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);
	return num_ops;
}

static inline int
accel_mlx5_copy_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(qp->dev, qp);

	if (spdk_likely(task->s.iovcnt <= ACCEL_MLX5_MAX_SGE)) {
		mlx5_task->num_reqs = task->d.iovcnt;
	} else if (task->d.iovcnt == 1) {
		mlx5_task->num_reqs = SPDK_CEIL_DIV(task->s.iovcnt, ACCEL_MLX5_MAX_SGE);
	} else {
		mlx5_task->num_reqs = accel_mlx5_get_copy_task_count(task->s.iovs, task->s.iovcnt,
				      task->d.iovs, task->d.iovcnt);
	}
	mlx5_task->inplace = 0;
	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
	mlx5_task->num_ops = spdk_min(qp_slot, mlx5_task->num_reqs);
	if (spdk_unlikely(!mlx5_task->num_ops)) {
		qp->dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}
	SPDK_DEBUGLOG(accel_mlx5, "copy task num_reqs %u, num_ops %u\n", mlx5_task->num_reqs,
		      mlx5_task->num_ops);

	return 0;
}

static inline uint32_t
accel_mlx5_advance_iovec(struct iovec *iov, uint32_t iovcnt, size_t *iov_offset, size_t *len)
{
	uint32_t i;
	size_t iov_len;

	for (i = 0; *len != 0 && i < iovcnt; i++) {
		iov_len = iov[i].iov_len - *iov_offset;

		if (iov_len < *len) {
			*iov_offset = 0;
			*len -= iov_len;
			continue;
		}
		if (iov_len == *len) {
			*iov_offset = 0;
			i++;
		} else { /* iov_len > *len */
			*iov_offset += *len;
		}
		*len = 0;
		break;
	}

	return i;
}

static inline void
accel_mlx5_crc_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	*mlx5_task->base.crc_dst = mlx5_task->psv->crc ^ UINT32_MAX;
	/* Normal task completion without allocated mkeys is not possible */
	assert(mlx5_task->num_ops);
	spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, mlx5_task->mkeys, mlx5_task->num_ops);
	spdk_mempool_put(dev->dev_ctx->psv_pool, mlx5_task->psv);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_crc_task_configure_umr(struct accel_mlx5_task *mlx5_task, struct ibv_sge *sge,
				  uint32_t sge_count, struct spdk_mlx5_mkey_pool_obj *mkey,
				  enum spdk_mlx5_umr_sig_domain sig_domain, uint32_t umr_len,
				  bool sig_init, bool sig_check_gen)
{
	struct spdk_mlx5_umr_sig_attr sattr = {
		.seed = mlx5_task->base.seed ^ UINT32_MAX,
		.psv_index = mlx5_task->psv->psv_index,
		.domain = sig_domain,
		.sigerr_count = mkey->sig.sigerr_count,
		.raw_data_size = umr_len,
		.init = sig_init,
		.check_gen = sig_check_gen,
	};
	struct spdk_mlx5_umr_attr umr_attr = {
		.mkey = mkey->mkey,
		.umr_len = umr_len,
		.sge_count = sge_count,
		.sge = sge,
	};

	return spdk_mlx5_umr_configure_sig(mlx5_task->qp->qp, &umr_attr, &sattr, 0, 0);
}

static inline int
accel_mlx5_crc_task_fill_sge(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_sge *sge)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t remaining;
	int rc;

	rc = accel_mlx5_fill_block_sge(dev, sge->src_sge, &mlx5_task->src, task->nbytes, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0)) {
		if (rc == 0) {
			rc = -EINVAL;
		}
		SPDK_ERRLOG("failed set src sge, rc %d\n", rc);
		return rc;
	}
	assert(remaining == 0);
	sge->src_sge_count = rc;

	if (!mlx5_task->inplace) {
		rc = accel_mlx5_fill_block_sge(dev, sge->dst_sge, &mlx5_task->dst, task->nbytes, &remaining,
					       task->dst_domain, task->dst_domain_ctx);
		if (spdk_unlikely(rc <= 0)) {
			if (rc == 0) {
				rc = -EINVAL;
			}
			SPDK_ERRLOG("failed set dst sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
		sge->dst_sge_count = rc;
	}

	return 0;
}

static inline int
accel_mlx5_crc_task_process_one_req(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sges;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	struct ibv_sge *sge;
	int rc;
	uint16_t sge_count;

	num_ops = spdk_min(num_ops, qp_slot >> 1);
	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}

	mlx5_task->num_wrs = 0;
	/* At this moment we have as many requests as can be submitted to a qp */
	rc = accel_mlx5_crc_task_fill_sge(mlx5_task, &sges);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	rc = accel_mlx5_crc_task_configure_umr(mlx5_task, sges.src_sge, sges.src_sge_count,
					       mlx5_task->mkeys[0], SPDK_MLX5_UMR_SIG_DOMAIN_WIRE, mlx5_task->base.nbytes, true, true);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	dev->stats.sig_umrs++;

	if (mlx5_task->inplace) {
		sge = sges.src_sge;
		sge_count = sges.src_sge_count;
	} else {
		sge = sges.dst_sge;
		sge_count = sges.dst_sge_count;
	}

	/*
	 * Add the crc destination to the end of sges. A free entry must be available for CRC
	 * because the task init function reserved it.
	 */
	assert(sge_count < ACCEL_MLX5_MAX_SGE);
	sge[sge_count].lkey = mlx5_task->psv->crc_lkey;
	sge[sge_count].addr = (uintptr_t)&mlx5_task->psv->crc;
	sge[sge_count++].length = sizeof(uint32_t);

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, *mlx5_task->base.crc_dst, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	rc = spdk_mlx5_qp_rdma_read(qp->qp, sge, sge_count, 0, mlx5_task->mkeys[0]->mkey,
				    (uint64_t)mlx5_task, rdma_fence | SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	dev->stats.rdma_reads++;

	return 0;
}

static inline int
accel_mlx5_crc_task_fill_umr_sge(struct accel_mlx5_qp *qp, struct ibv_sge *sge,
				 struct accel_mlx5_iov_sgl *umr_iovs, struct spdk_memory_domain *domain,
				 void *domain_ctx, struct accel_mlx5_iov_sgl *rdma_iovs, size_t *len)
{
	int umr_idx = 0;
	int rdma_idx = 0;
	int umr_iovcnt = spdk_min(umr_iovs->iovcnt, (int)ACCEL_MLX5_MAX_SGE);
	int rdma_iovcnt = spdk_min(rdma_iovs->iovcnt, (int)ACCEL_MLX5_MAX_SGE);
	size_t umr_iov_offset;
	size_t rdma_iov_offset;
	size_t umr_len = 0;
	void *sge_addr;
	size_t sge_len;
	size_t umr_sge_len;
	size_t rdma_sge_len;
	int rc;

	umr_iov_offset = umr_iovs->iov_offset;
	rdma_iov_offset = rdma_iovs->iov_offset;

	while (umr_idx < umr_iovcnt && rdma_idx < rdma_iovcnt) {
		umr_sge_len = umr_iovs->iov[umr_idx].iov_len - umr_iov_offset;
		rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len - rdma_iov_offset;
		sge_addr = umr_iovs->iov[umr_idx].iov_base + umr_iov_offset;

		if (umr_sge_len == rdma_sge_len) {
			rdma_idx++;
			umr_iov_offset = 0;
			rdma_iov_offset = 0;
			sge_len = umr_sge_len;
		} else if (umr_sge_len < rdma_sge_len) {
			umr_iov_offset = 0;
			rdma_iov_offset += umr_sge_len;
			sge_len = umr_sge_len;
		} else {
			size_t remaining;

			remaining = umr_sge_len - rdma_sge_len;
			while (remaining) {
				rdma_idx++;
				if (rdma_idx == (int)ACCEL_MLX5_MAX_SGE) {
					break;
				}
				rdma_sge_len = rdma_iovs->iov[rdma_idx].iov_len;
				if (remaining == rdma_sge_len) {
					rdma_idx++;
					rdma_iov_offset = 0;
					umr_iov_offset = 0;
					remaining = 0;
					break;
				}
				if (remaining < rdma_sge_len) {
					rdma_iov_offset = remaining;
					umr_iov_offset = 0;
					remaining = 0;
					break;
				}
				remaining -= rdma_sge_len;
			}
			sge_len = umr_sge_len - remaining;
		}
		rc = accel_mlx5_translate_addr(sge_addr, sge_len, domain, domain_ctx, qp->dev, &sge[umr_idx]);
		if (spdk_unlikely(rc)) {
			return -EINVAL;
		}
		SPDK_DEBUGLOG(accel_mlx5, "\t sge[%d] lkey %u, addr %p, len %u\n", umr_idx, sge[umr_idx].lkey,
			      (void *)sge[umr_idx].addr, sge[umr_idx].length);
		umr_len += sge_len;
		umr_idx++;
	}
	accel_mlx5_iov_sgl_advance(umr_iovs, umr_len);
	accel_mlx5_iov_sgl_advance(rdma_iovs, umr_len);
	*len = umr_len;

	return umr_idx;
}

static inline int
accel_mlx5_crc_task_process_multi_req(struct accel_mlx5_task *mlx5_task)
{
	size_t umr_len[ACCEL_MLX5_MAX_MKEYS_IN_TASK];
	struct ibv_sge sges[ACCEL_MLX5_MAX_SGE];
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct accel_mlx5_iov_sgl umr_sgl;
	struct accel_mlx5_iov_sgl *umr_sgl_ptr;
	struct accel_mlx5_iov_sgl rdma_sgl;
	uint64_t umr_offset;
	uint32_t rdma_fence = SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;
	int sge_count;
	uint32_t remaining;
	int rc;
	uint16_t i;
	uint16_t num_ops = spdk_min(mlx5_task->num_reqs - mlx5_task->num_completed_reqs,
				    mlx5_task->num_ops);
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	bool sig_init, sig_check_gen = false;

	num_ops = spdk_min(num_ops, qp_slot >> 1);
	if (spdk_unlikely(!num_ops)) {
		return -EINVAL;
	}
	/* Init signature on the first UMR */
	sig_init = !mlx5_task->num_submitted_reqs;

	/*
	 * accel_mlx5_crc_task_fill_umr_sge() and accel_mlx5_fill_block_sge() advance an IOV during iteration
	 * on it. We must copy accel_mlx5_iov_sgl to iterate twice or more on the same IOV.
	 *
	 * In the in-place case, we iterate on the source IOV three times. That's why we need two copies of
	 * the source accel_mlx5_iov_sgl.
	 *
	 * In the out-of-place case, we iterate on the source IOV once and on the destination IOV two times.
	 * So, we need one copy of the destination accel_mlx5_iov_sgl.
	 */
	if (mlx5_task->inplace) {
		accel_mlx5_iov_sgl_init(&umr_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
		umr_sgl_ptr = &umr_sgl;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->src.iov, mlx5_task->src.iovcnt);
	} else {
		umr_sgl_ptr = &mlx5_task->src;
		accel_mlx5_iov_sgl_init(&rdma_sgl, mlx5_task->dst.iov, mlx5_task->dst.iovcnt);
	}
	mlx5_task->num_wrs = 0;
	for (i = 0; i < num_ops; i++) {
		/*
		 * The last request may have only CRC. Skip UMR in this case because the MKey from
		 * the previous request is used.
		 */
		if (umr_sgl_ptr->iovcnt == 0) {
			assert((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs);
			break;
		}
		sge_count = accel_mlx5_crc_task_fill_umr_sge(qp, sges, umr_sgl_ptr, task->src_domain,
				task->src_domain_ctx, &rdma_sgl, &umr_len[i]);
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set UMR sge, rc %d\n", rc);
			return rc;
		}
		if (umr_sgl_ptr->iovcnt == 0) {
			/*
			 * We post RDMA without UMR if the last request has only CRC. We use an MKey from
			 * the last UMR in this case. Since the last request can be postponed to the next
			 * call of this function, we must save the MKey to the task structure.
			 */
			mlx5_task->last_umr_len = umr_len[i];
			mlx5_task->last_mkey_idx = i;
			sig_check_gen = true;
		}
		rc = accel_mlx5_crc_task_configure_umr(mlx5_task, sges, sge_count, mlx5_task->mkeys[i],
						       SPDK_MLX5_UMR_SIG_DOMAIN_WIRE, umr_len[i], sig_init,
						       sig_check_gen);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("UMR configure failed with %d\n", rc);
			return rc;
		}
		sig_init = false;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		dev->stats.sig_umrs++;
	}

	if (spdk_unlikely(mlx5_task->psv->bits.error)) {
		rc = spdk_mlx5_qp_set_psv(qp->qp, mlx5_task->psv->psv_index, *mlx5_task->base.crc_dst, 0, 0);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("SET_PSV failed with %d\n", rc);
			return rc;
		}
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
	}

	for (i = 0; i < num_ops - 1; i++) {
		if (mlx5_task->inplace) {
			sge_count = accel_mlx5_fill_block_sge(dev, sges, &mlx5_task->src, umr_len[i], &remaining,
							      task->src_domain, task->src_domain_ctx);
		} else {
			sge_count = accel_mlx5_fill_block_sge(dev, sges, &mlx5_task->dst, umr_len[i], &remaining,
							      task->dst_domain, task->dst_domain_ctx);
		}
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		rc = spdk_mlx5_qp_rdma_read(qp->qp, sges, sge_count, 0, mlx5_task->mkeys[i]->mkey,
					    0, rdma_fence);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
			return rc;
		}
		mlx5_task->num_submitted_reqs++;
		ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED(qp, mlx5_task);
		dev->stats.rdma_reads++;
		rdma_fence = SPDK_MLX5_WQE_CTRL_STRONG_ORDERING;
	}
	if ((mlx5_task->inplace && mlx5_task->src.iovcnt == 0) || (!mlx5_task->inplace &&
			mlx5_task->dst.iovcnt == 0)) {
		/*
		 * The last RDMA does not have any data, only CRC. It also does not have a paired Mkey.
		 * The CRC is handled in the previous MKey in this case.
		 */
		sge_count = 0;
		umr_offset = mlx5_task->last_umr_len;
	} else {
		umr_offset = 0;
		mlx5_task->last_mkey_idx = i;
		if (mlx5_task->inplace) {
			sge_count = accel_mlx5_fill_block_sge(dev, sges, &mlx5_task->src, umr_len[i], &remaining,
							      task->src_domain, task->src_domain_ctx);
		} else {
			sge_count = accel_mlx5_fill_block_sge(dev, sges, &mlx5_task->dst, umr_len[i], &remaining,
							      task->dst_domain, task->dst_domain_ctx);
		}
		if (spdk_unlikely(sge_count <= 0)) {
			rc = (sge_count == 0) ? -EINVAL : sge_count;
			SPDK_ERRLOG("failed set RDMA sge, rc %d\n", rc);
			return rc;
		}
		assert(remaining == 0);
	}
	if ((mlx5_task->num_completed_reqs + i + 1) == mlx5_task->num_reqs) {
		/* Ensure that there is a free sge for the CRC destination. */
		assert(sge_count < (int)ACCEL_MLX5_MAX_SGE);
		/* Add the crc destination to the end of sges. */
		sges[sge_count].lkey = mlx5_task->psv->crc_lkey;
		sges[sge_count].addr = (uintptr_t)&mlx5_task->psv->crc;
		sges[sge_count++].length = sizeof(uint32_t);
	}
	rdma_fence |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
	rc = spdk_mlx5_qp_rdma_read(qp->qp, sges, sge_count, umr_offset,
				    mlx5_task->mkeys[mlx5_task->last_mkey_idx]->mkey,
				    (uint64_t)mlx5_task, rdma_fence);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("RDMA READ/WRITE failed with %d\n", rc);
		return rc;
	}
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	dev->stats.rdma_reads++;

	return 0;
}

static inline int
accel_mlx5_crc_task_process(struct accel_mlx5_task *mlx5_task)
{
	int rc;

	assert(mlx5_task->mlx5_opcode == ACCEL_MLX5_OPC_CRC32C);

	SPDK_DEBUGLOG(accel_mlx5, "begin, crc task, %p, reqs: total %u, submitted %u, completed %u\n",
		      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs, mlx5_task->num_completed_reqs);

	if (mlx5_task->num_reqs == 1) {
		rc = accel_mlx5_crc_task_process_one_req(mlx5_task);
	} else {
		rc = accel_mlx5_crc_task_process_multi_req(mlx5_task);
	}

	if (rc == 0) {
		STAILQ_INSERT_TAIL(&mlx5_task->qp->in_hw, mlx5_task, link);
		SPDK_DEBUGLOG(accel_mlx5, "end, crc task, %p, reqs: total %u, submitted %u, completed %u\n",
			      mlx5_task, mlx5_task->num_reqs, mlx5_task->num_submitted_reqs,
			      mlx5_task->num_completed_reqs);
	}

	return rc;
}

static inline int
accel_mlx5_task_alloc_crc_ctx(struct accel_mlx5_task *task, uint32_t qp_slot)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	if (spdk_unlikely(!accel_mlx5_task_alloc_mkeys(task, dev->sig_mkeys))) {
		SPDK_DEBUGLOG(accel_mlx5, "no mkeys in signature mkey pool, dev %s\n",
			      dev->dev_ctx->context->device->name);
		dev->stats.nomem_mkey++;
		return -ENOMEM;
	}
	task->psv = spdk_mempool_get(dev->dev_ctx->psv_pool);
	if (spdk_unlikely(!task->psv)) {
		SPDK_DEBUGLOG(accel_mlx5, "no reqs in psv pool, dev %s\n", dev->dev_ctx->context->device->name);
		spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, task->mkeys, task->num_ops);
		task->num_ops = 0;
		dev->stats.nomem_mkey++;
		return -ENOMEM;
	}
	/* One extra slot is needed for SET_PSV WQE to reset the error state in PSV. */
	if (spdk_unlikely(task->psv->bits.error)) {
		uint32_t n_slots = task->num_ops * 2 + 1;

		if (qp_slot < n_slots) {
			spdk_mempool_put(dev->dev_ctx->psv_pool, task->psv);
			spdk_mlx5_mkey_pool_put_bulk(dev->sig_mkeys, task->mkeys, task->num_ops);
			dev->stats.nomem_qdepth++;
			task->num_ops = 0;
			return -ENOMEM;
		}
	}

	return 0;
}

static inline int
accel_mlx5_crc_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	int rc;

	assert(task->num_reqs > task->num_completed_reqs);
	if (task->num_ops == 0) {
		/* No mkeys allocated, try to allocate now. */
		rc = accel_mlx5_task_alloc_crc_ctx(task, qp_slot);
		if (spdk_unlikely(rc)) {
			STAILQ_INSERT_TAIL(&dev->nomem, task, link);
			return -ENOMEM;
		}
	}
	/* We need to post at least 1 UMR and 1 RDMA operation */
	if (spdk_unlikely(qp_slot < 2)) {
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}

	return accel_mlx5_crc_task_process(task);
}

static inline uint32_t
accel_mlx5_get_crc_task_count(struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov,
			      uint32_t dst_iovcnt)
{
	uint32_t src_idx = 0;
	uint32_t dst_idx = 0;
	uint32_t num_ops = 1;
	uint32_t num_src_sge = 1;
	uint32_t num_dst_sge = 1;
	size_t src_offset = 0;
	size_t dst_offset = 0;
	uint32_t num_sge;
	size_t src_len;
	size_t dst_len;

	/* One operation is enough if both iovs fit into ACCEL_MLX5_MAX_SGE. One SGE is reserved for CRC on dst_iov. */
	if (src_iovcnt <= ACCEL_MLX5_MAX_SGE && (dst_iovcnt + 1) <= ACCEL_MLX5_MAX_SGE) {
		return 1;
	}

	while (src_idx < src_iovcnt && dst_idx < dst_iovcnt) {
		if (num_src_sge > ACCEL_MLX5_MAX_SGE || num_dst_sge > ACCEL_MLX5_MAX_SGE) {
			num_ops++;
			num_src_sge = 1;
			num_dst_sge = 1;
		}
		src_len = src_iov[src_idx].iov_len - src_offset;
		dst_len = dst_iov[dst_idx].iov_len - dst_offset;

		if (src_len == dst_len) {
			num_src_sge++;
			num_dst_sge++;
			src_offset = 0;
			dst_offset = 0;
			src_idx++;
			dst_idx++;
			continue;
		}
		if (src_len < dst_len) {
			/* Advance src_iov to reach the point that corresponds to the end of the current dst_iov. */
			num_sge = accel_mlx5_advance_iovec(&src_iov[src_idx],
							   spdk_min(ACCEL_MLX5_MAX_SGE + 1 - num_src_sge,
									   src_iovcnt - src_idx),
							   &src_offset, &dst_len);
			src_idx += num_sge;
			num_src_sge += num_sge;
			if (dst_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_SGE is reached on src_iov, and dst_len bytes
				 * are left on the current dst_iov.
				 */
				dst_offset = dst_iov[dst_idx].iov_len - dst_len;
			} else {
				/* The src_iov advance is completed, shift to the next dst_iov. */
				dst_idx++;
				num_dst_sge++;
				dst_offset = 0;
			}
		} else { /* src_len > dst_len */
			/* Advance dst_iov to reach the point that corresponds to the end of the current src_iov. */
			num_sge = accel_mlx5_advance_iovec(&dst_iov[dst_idx],
							   spdk_min(ACCEL_MLX5_MAX_SGE + 1 - num_dst_sge,
									   dst_iovcnt - dst_idx),
							   &dst_offset, &src_len);
			dst_idx += num_sge;
			num_dst_sge += num_sge;
			if (src_len != 0) {
				/*
				 * ACCEL_MLX5_MAX_SGE is reached on dst_iov, and src_len bytes
				 * are left on the current src_iov.
				 */
				src_offset = src_iov[src_idx].iov_len - src_len;
			} else {
				/* The dst_iov advance is completed, shift to the next src_iov. */
				src_idx++;
				num_src_sge++;
				src_offset = 0;
			}
		}
	}
	/* An extra operation is needed if no space is left on dst_iov because CRC takes one SGE. */
	if (num_dst_sge > ACCEL_MLX5_MAX_SGE) {
		num_ops++;
	}

	/* The above loop must reach the end of both iovs simultaneously because their size is the same. */
	assert(src_idx == src_iovcnt);
	assert(dst_idx == dst_iovcnt);
	assert(src_offset == 0);
	assert(dst_offset == 0);

	return num_ops;
}

static inline int
accel_mlx5_crc_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	uint32_t qp_slot = accel_mlx5_dev_get_available_slots(qp->dev, qp);
	int rc;

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	if (mlx5_task->inplace) {
		/* One entry is reserved for CRC */
		mlx5_task->num_reqs = SPDK_CEIL_DIV(mlx5_task->src.iovcnt + 1, ACCEL_MLX5_MAX_SGE);
	} else {
		accel_mlx5_iov_sgl_init(&mlx5_task->dst, task->d.iovs, task->d.iovcnt);
		mlx5_task->num_reqs = accel_mlx5_get_crc_task_count(mlx5_task->src.iov, mlx5_task->src.iovcnt,
				      mlx5_task->dst.iov, mlx5_task->dst.iovcnt);
	}

	rc = accel_mlx5_task_alloc_crc_ctx(mlx5_task, qp_slot);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	if (spdk_unlikely(qp_slot < 2)) {
		/* Queue is full, queue this task */
		SPDK_DEBUGLOG(accel_mlx5, "dev %s qp %p is full\n", qp->dev->dev_ctx->context->device->name,
			      mlx5_task->qp);
		qp->dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}
	return 0;
}

static inline int
accel_mlx5_crypto_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t num_blocks;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);
	bool crypto_key_ok;

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_SGE)) {
		/* With `external mkey` we can't split task or register several UMRs */
		SPDK_ERRLOG("src buffer is too fragmented\n");
		return -EINVAL;
	}
	if (spdk_unlikely(task->src_domain == spdk_accel_get_memory_domain())) {
		SPDK_ERRLOG("accel domain is not supported\n");
		return -ENOTSUP;
	}
	if (spdk_unlikely(spdk_accel_sequence_next_task(task) != NULL)) {
		SPDK_ERRLOG("Mkey registration is only supported for single task\n");
		return -ENOTSUP;
	}

	crypto_key_ok = (task->crypto_key && task->crypto_key->module_if == &g_accel_mlx5.module &&
			 task->crypto_key->priv);
	if (spdk_unlikely(!crypto_key_ok)) {
		SPDK_ERRLOG("Wrong crypto key provided\n");
		return -EINVAL;
	}
	if (spdk_unlikely(task->nbytes % mlx5_task->base.block_size != 0)) {
		SPDK_ERRLOG("src length %"PRIu64" is not a multiple of the block size %u\n", task->nbytes,
			    mlx5_task->base.block_size);
		return -EINVAL;
	}

	num_blocks = task->nbytes / mlx5_task->base.block_size;
	if (dev->crypto_multi_block) {
		if (spdk_unlikely(g_accel_mlx5.attr.crypto_split_blocks &&
				  num_blocks > g_accel_mlx5.attr.crypto_split_blocks)) {
			SPDK_ERRLOG("Number of blocks in task %u exceeds split threshold %u, can't handle\n",
				    num_blocks, g_accel_mlx5.attr.crypto_split_blocks);
			return -E2BIG;
		}
	} else if (num_blocks != 1) {
		SPDK_ERRLOG("Task contains more than 1 block, can't handle\n");
		return -E2BIG;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_blocks = num_blocks;
	mlx5_task->num_processed_blocks = 0;
	mlx5_task->num_reqs = 1;
	mlx5_task->blocks_per_req = num_blocks;

	if (spdk_unlikely(qp_slot == 0)) {
		mlx5_task->num_ops = 0;
		dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, mlx5_task->mkeys, 1);
	if (spdk_unlikely(rc)) {
		mlx5_task->num_ops = 0;
		dev->stats.nomem_mkey++;
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto_mkey task num_blocks %u, src_len %zu\n", mlx5_task->num_reqs,
		      task->nbytes);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_sge sge;
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	struct spdk_mlx5_crypto_dek_data dek_data;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	mlx5_task->num_wrs = 0;
	rc = spdk_mlx5_crypto_get_dek_data(task->crypto_key->priv, dev->dev_ctx->pd, &dek_data);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	rc = accel_mlx5_configure_crypto_umr(mlx5_task, &sge, mlx5_task->mkeys[0]->mkey,
					     mlx5_task->num_blocks, &dek_data, (uint64_t)mlx5_task, SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.crypto_umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	return 0;
}

static inline int
accel_mlx5_crypto_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (task->num_ops == 0) {
		rc = spdk_mlx5_mkey_pool_get_bulk(dev->crypto_mkeys, task->mkeys, 1);
		if (spdk_unlikely(rc)) {
			dev->stats.nomem_mkey++;
			STAILQ_INSERT_TAIL(&dev->nomem, task, link);
			return -ENOMEM;
		}
		task->num_ops = 1;
	}
	if (spdk_unlikely(qp_slot == 0)) {
		dev->stats.nomem_qdepth++;
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return -ENOMEM;
	}
	return accel_mlx5_crypto_mkey_task_process(task);
}

static inline void
accel_mlx5_crypto_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	assert(mlx5_task->num_ops);
	assert(mlx5_task->num_processed_blocks == mlx5_task->num_blocks);
	assert(mlx5_task->base.seq);

	spdk_mlx5_mkey_pool_put_bulk(dev->crypto_mkeys, mlx5_task->mkeys, 1);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static inline int
accel_mlx5_mkey_task_init(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (spdk_unlikely(task->s.iovcnt > ACCEL_MLX5_MAX_SGE)) {
		/* With `external mkey` we can't split task or register several UMRs */
		SPDK_ERRLOG("src buffer is too fragmented\n");
		return -EINVAL;
	}
	if (spdk_unlikely(task->src_domain == spdk_accel_get_memory_domain())) {
		SPDK_ERRLOG("accel domain is not supported\n");
		return -EINVAL;
	}
	if (spdk_unlikely(spdk_accel_sequence_next_task(task) != NULL)) {
		SPDK_ERRLOG("Mkey registration is only supported for single task\n");
		return -ENOTSUP;
	}

	accel_mlx5_iov_sgl_init(&mlx5_task->src, task->s.iovs, task->s.iovcnt);
	mlx5_task->num_reqs = 1;

	if (spdk_unlikely(qp_slot == 0)) {
		mlx5_task->num_ops = 0;
		dev->stats.nomem_qdepth++;
		return -ENOMEM;
	}
	rc = spdk_mlx5_mkey_pool_get_bulk(dev->mkeys, mlx5_task->mkeys, 1);
	if (spdk_unlikely(rc)) {
		mlx5_task->num_ops = 0;
		dev->stats.nomem_mkey++;
		return -ENOMEM;
	}
	mlx5_task->num_ops = 1;

	SPDK_DEBUGLOG(accel_mlx5, "crypto_mkey task num_blocks %u, src_len %zu\n", mlx5_task->num_reqs,
		      task->nbytes);

	return 0;
}

static inline int
accel_mlx5_mkey_task_process(struct accel_mlx5_task *mlx5_task)
{
	struct spdk_mlx5_umr_attr umr_attr;
	struct ibv_sge src_sge[ACCEL_MLX5_MAX_SGE];
	struct spdk_accel_task *task = &mlx5_task->base;
	struct accel_mlx5_qp *qp = mlx5_task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	uint32_t remaining = 0;
	int rc;

	if (spdk_unlikely(!mlx5_task->num_ops)) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(accel_mlx5, "begin, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	mlx5_task->num_wrs = 0;

	rc = accel_mlx5_fill_block_sge(dev, src_sge, &mlx5_task->src,  task->nbytes, &remaining,
				       task->src_domain, task->src_domain_ctx);
	if (spdk_unlikely(rc <= 0 || remaining)) {
		rc = rc ? rc : -EINVAL;
		SPDK_ERRLOG("Failed to set src sge, rc %d, remaining %u\n", rc, remaining);
		return rc;
	}
	umr_attr.mkey = mlx5_task->mkeys[0]->mkey;
	umr_attr.sge = src_sge;
	umr_attr.sge_count = rc;
	umr_attr.umr_len = task->nbytes;

	rc = spdk_mlx5_umr_configure(qp->qp, &umr_attr, (uint64_t)mlx5_task,
				     SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("UMR configure failed with %d\n", rc);
		return rc;
	}
	dev->stats.umrs++;
	mlx5_task->num_submitted_reqs++;
	ACCEL_MLX5_UPDATE_ON_WR_SUBMITTED_SIGNALED(dev, qp, mlx5_task);
	STAILQ_INSERT_TAIL(&qp->in_hw, mlx5_task, link);

	SPDK_DEBUGLOG(accel_mlx5, "end, task %p, dst_domain_ctx %p\n", mlx5_task, task->dst_domain_ctx);

	return 0;
}

static inline int
accel_mlx5_mkey_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;
	int rc;
	uint16_t qp_slot = accel_mlx5_dev_get_available_slots(dev, qp);

	if (task->num_ops == 0) {
		rc = spdk_mlx5_mkey_pool_get_bulk(dev->mkeys, task->mkeys, 1);
		if (spdk_unlikely(rc)) {
			dev->stats.nomem_mkey++;
			STAILQ_INSERT_TAIL(&dev->nomem, task, link);
			return -ENOMEM;
		}
		task->num_ops = 1;
	}
	if (spdk_unlikely(qp_slot == 0)) {
		dev->stats.nomem_qdepth++;
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return -ENOMEM;
	}
	return accel_mlx5_mkey_task_process(task);
}

static inline void
accel_mlx5_mkey_task_complete(struct accel_mlx5_task *mlx5_task)
{
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;

	assert(mlx5_task->num_ops);
	assert(mlx5_task->base.seq);

	spdk_mlx5_mkey_pool_put_bulk(dev->mkeys, mlx5_task->mkeys, 1);
	spdk_accel_task_complete(&mlx5_task->base, 0);
}

static int
accel_mlx5_task_op_not_implemented(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("wrong function called\n");
	SPDK_UNREACHABLE();
}

static void
accel_mlx5_task_op_not_implemented_v(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("wrong function called\n");
	SPDK_UNREACHABLE();
}

static int
accel_mlx5_task_op_not_supported(struct accel_mlx5_task *mlx5_task)
{
	SPDK_ERRLOG("Unsupported opcode %d\n", mlx5_task->base.op_code);

	return -ENOTSUP;
}

static struct accel_mlx5_task_operations g_accel_mlx5_tasks_ops[] = {
	[ACCEL_MLX5_OPC_COPY] = {
		.init = accel_mlx5_copy_task_init,
		.process = accel_mlx5_copy_task_process,
		.cont = accel_mlx5_copy_task_continue,
		.complete = accel_mlx5_copy_task_complete,
	},
	[ACCEL_MLX5_OPC_CRYPTO] = {
		.init = accel_mlx5_crypto_task_init,
		.process = accel_mlx5_crypto_task_process,
		.cont = accel_mlx5_crypto_task_continue,
		.complete = accel_mlx5_crypto_task_complete,
	},
	[ACCEL_MLX5_OPC_CRC32C] = {
		.init = accel_mlx5_crc_task_init,
		.process = accel_mlx5_crc_task_process,
		.cont = accel_mlx5_crc_task_continue,
		.complete = accel_mlx5_crc_task_complete,
	},
	[ACCEL_MLX5_OPC_CRYPTO_MKEY] = {
		.init = accel_mlx5_crypto_mkey_task_init,
		.process = accel_mlx5_crypto_mkey_task_process,
		.cont = accel_mlx5_crypto_mkey_task_continue,
		.complete = accel_mlx5_crypto_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_MKEY] = {
		.init = accel_mlx5_mkey_task_init,
		.process = accel_mlx5_mkey_task_process,
		.cont = accel_mlx5_mkey_task_continue,
		.complete = accel_mlx5_mkey_task_complete,
	},
	[ACCEL_MLX5_OPC_LAST] = {
		.init = accel_mlx5_task_op_not_supported,
		.process = accel_mlx5_task_op_not_implemented,
		.cont = accel_mlx5_task_op_not_implemented,
		.complete = accel_mlx5_task_op_not_implemented_v
	},
};

static void
accel_mlx5_memory_domain_transfer_cpl(void *ctx, int rc)
{
	struct accel_mlx5_task *task = ctx;

	assert(task->needs_data_transfer);
	task->needs_data_transfer = 0;

	if (spdk_likely(!rc)) {
		SPDK_DEBUGLOG(accel_mlx5, "task %p, data transfer done\n", task);
		accel_mlx5_task_complete(task);
	} else {
		SPDK_ERRLOG("Task %p, data transfer failed, rc %d\n", task, rc);
		accel_mlx5_task_fail(task, rc);
	}
}

static inline void
accel_mlx5_memory_domain_transfer(struct accel_mlx5_task *task)
{
	struct spdk_memory_domain_translation_result translation;
	struct spdk_accel_task *base = &task->base;
	struct accel_mlx5_dev *dev = task->qp->dev;
	int rc;

	assert(task->mlx5_opcode == ACCEL_MLX5_OPC_CRYPTO_MKEY || task->mlx5_opcode == ACCEL_MLX5_OPC_MKEY);
	/* UMR is an offset in the addess space, so the start address is 0 */
	translation.iov.iov_base = NULL;
	translation.iov.iov_len = base->nbytes;
	translation.iov_count = 1;
	translation.size = sizeof(translation);
	translation.rdma.rkey = task->mkeys[0]->mkey;
	translation.rdma.lkey = task->mkeys[0]->mkey;

	SPDK_DEBUGLOG(accel_mlx5, "start transfer, task %p, dst_domain_ctx %p, mkey %u\n", task,
		      task->base.dst_domain_ctx, task->mkeys[0]->mkey);
	rc = spdk_memory_domain_transfer_data(base->dst_domain, base->dst_domain_ctx, &translation.iov, 1,
					      dev->dev_ctx->domain, task, &translation.iov, 1, &translation,
					      accel_mlx5_memory_domain_transfer_cpl, task);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to start data transfer, task %p rc %d\n", task, rc);
		accel_mlx5_task_fail(task, rc);
	}
}

static inline void
accel_mlx5_task_complete(struct accel_mlx5_task *task)
{
	struct spdk_accel_sequence *seq = task->base.seq;
	struct spdk_accel_task *next;
	bool driver_seq;

	if (task->needs_data_transfer) {
		accel_mlx5_memory_domain_transfer(task);
		return;
	}

	next = spdk_accel_sequence_next_task(&task->base);
	driver_seq = task->driver_seq;

	assert(task->num_reqs == task->num_completed_reqs);
	SPDK_DEBUGLOG(accel_mlx5, "Complete task %p, opc %d\n", task, task->mlx5_opcode);

	g_accel_mlx5_tasks_ops[task->mlx5_opcode].complete(task);

	if (driver_seq) {
		struct spdk_io_channel *ch = task->qp->dev->ch;

		assert(seq);
		if (next) {
			accel_mlx5_execute_sequence(ch, seq);
		} else {
			spdk_accel_sequence_continue(seq);
		}
	}
}

static inline int
accel_mlx5_task_continue(struct accel_mlx5_task *task)
{
	struct accel_mlx5_qp *qp = task->qp;
	struct accel_mlx5_dev *dev = qp->dev;

	if (spdk_unlikely(qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, task, link);
		return 0;
	}

	return g_accel_mlx5_tasks_ops[task->mlx5_opcode].cont(task);
}
static inline void
accel_mlx5_task_init_opcode(struct accel_mlx5_task *mlx5_task)
{
	uint8_t base_opcode = mlx5_task->base.op_code;

	switch (base_opcode) {
	case SPDK_ACCEL_OPC_COPY:
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_COPY;
		break;
	case SPDK_ACCEL_OPC_ENCRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
		mlx5_task->mlx5_opcode =  ACCEL_MLX5_OPC_CRYPTO;
		break;
	case SPDK_ACCEL_OPC_DECRYPT:
		assert(g_accel_mlx5.crypto_supported);
		mlx5_task->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO;
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		mlx5_task->inplace = 1;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		mlx5_task->inplace = 0;
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_CRC32C;
		break;
	default:
		SPDK_ERRLOG("wrong opcode %d\n", base_opcode);
		mlx5_task->mlx5_opcode = ACCEL_MLX5_OPC_LAST;
	}
}

static void
accel_mlx5_post_poller_handler(void *fn_arg)
{
	struct accel_mlx5_io_channel *ch = fn_arg;
	struct accel_mlx5_dev *dev;
	uint32_t i;

	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];

		if (dev->qp.ring_db) {
			spdk_mlx5_qp_complete_send(dev->qp.qp);
			dev->qp.ring_db = false;
		}
	}

	ch->poller_handler_registered = false;
}

static inline int
_accel_mlx5_submit_tasks(struct accel_mlx5_io_channel *accel_ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	struct accel_mlx5_dev *dev = mlx5_task->qp->dev;
	int rc;

	/* We should not receive any tasks if the module was not enabled */
	assert(g_accel_mlx5.enabled);

	dev->stats.opcodes[mlx5_task->mlx5_opcode]++;
	rc = g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].init(mlx5_task);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(accel_mlx5, "no reqs to handle new task %p (required %u), put to queue\n", mlx5_task,
				      mlx5_task->num_reqs);
			STAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
			return 0;
		}
		SPDK_ERRLOG("Task opc %d init failed, rc %d\n", task->op_code, rc);
		return rc;
	}

	if (spdk_unlikely(mlx5_task->qp->recovering)) {
		STAILQ_INSERT_TAIL(&dev->nomem, mlx5_task, link);
		return 0;
	}

	if (!accel_ch->poller_handler_registered) {
		spdk_thread_register_post_poller_handler(accel_mlx5_post_poller_handler, accel_ch);
		/* Function above may fail to register our handler, in that case we ring doorbells on next polling
		 * cycle. That is less efficient but still works */
		accel_ch->poller_handler_registered = true;
	}

	return g_accel_mlx5_tasks_ops[mlx5_task->mlx5_opcode].process(mlx5_task);
}

static inline void
accel_mlx5_task_assign_qp(struct accel_mlx5_task *mlx5_task, struct accel_mlx5_io_channel *accel_ch)
{
	struct accel_mlx5_dev *dev;

	dev = &accel_ch->devs[accel_ch->dev_idx];
	accel_ch->dev_idx++;
	if (accel_ch->dev_idx == accel_ch->num_devs) {
		accel_ch->dev_idx = 0;
	}

	mlx5_task->qp = &dev->qp;
}

static inline void
accel_mlx5_task_reset(struct accel_mlx5_task *mlx5_task)
{
	mlx5_task->num_completed_reqs = 0;
	mlx5_task->num_submitted_reqs = 0;
	mlx5_task->num_ops = 0;
	mlx5_task->num_processed_blocks = 0;
	mlx5_task->raw = 0;
}

static int
accel_mlx5_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct accel_mlx5_task *mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	struct accel_mlx5_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_mlx5_task_assign_qp(mlx5_task, accel_ch);
	accel_mlx5_task_reset(mlx5_task);
	accel_mlx5_task_init_opcode(mlx5_task);

	return _accel_mlx5_submit_tasks(accel_ch, task);
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

	dev->stats.polls++;
	reaped = spdk_mlx5_cq_poll_completions(dev->cq, wc, ACCEL_MLX5_MAX_WC);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n", errno, spdk_strerror(errno));
		return reaped;
	} else if (reaped == 0) {
		dev->stats.idle_polls++;
		return 0;
	}
	dev->stats.completions += reaped;

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

		if (spdk_unlikely(wc[i].status)) {
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

	/* reaped completions may register a post poller handler, that makes no sense in the scope of our own poller */
	ch->poller_handler_registered = true;
	for (i = 0; i < ch->num_devs; i++) {
		dev = &ch->devs[i];
		if (dev->wrs_in_cq) {
			rc = accel_mlx5_poll_cq(dev);
			if (spdk_unlikely(rc < 0)) {
				SPDK_ERRLOG("Error %"PRId64" on CQ, dev %s\n", rc, dev->dev_ctx->context->device->name);
			}
			completions += rc;
			if (dev->qp.ring_db) {
				spdk_mlx5_qp_complete_send(dev->qp.qp);
				dev->qp.ring_db = false;
			}
		}
		if (!STAILQ_EMPTY(&dev->nomem)) {
			accel_mlx5_resubmit_nomem_tasks(dev);
		}
	}
	ch->poller_handler_registered = false;

	return !!completions;
}

static bool
accel_mlx5_supports_opcode(enum spdk_accel_opcode opc)
{
	assert(g_accel_mlx5.enabled);

	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
		return true;
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
		return g_accel_mlx5.crypto_supported;
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		return g_accel_mlx5.crc32c_supported;
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
accel_mlx5_add_stats(struct accel_mlx5_stats *stats, const struct accel_mlx5_stats *to_add)
{
	int i;

	stats->crypto_umrs += to_add->crypto_umrs;
	stats->sig_umrs += to_add->sig_umrs;
	stats->umrs += to_add->umrs;
	stats->rdma_reads += to_add->rdma_reads;
	stats->rdma_writes += to_add->rdma_writes;
	stats->polls += to_add->polls;
	stats->idle_polls += to_add->idle_polls;
	stats->completions += to_add->completions;
	stats->nomem_qdepth += to_add->nomem_qdepth;
	stats->nomem_mkey += to_add->nomem_mkey;
	for (i = 0; i < ACCEL_MLX5_OPC_LAST; i++) {
		stats->opcodes[i] += to_add->opcodes[i];
	}
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
		if (dev->mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->mkeys);
		}
		if (dev->crypto_mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->crypto_mkeys);
		}
		if (dev->sig_mkeys) {
			spdk_mlx5_mkey_pool_put_ref(dev->sig_mkeys);
		}
		spdk_rdma_utils_free_mem_map(&dev->mmap);
		spdk_spin_lock(&g_accel_mlx5.lock);
		accel_mlx5_add_stats(&g_accel_mlx5.stats, &dev->stats);
		spdk_spin_unlock(&g_accel_mlx5.lock);
	}
	free(ch->devs);
}

static int
accel_mlx5_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_mlx5_cq_attr cq_attr = {};
	struct accel_mlx5_io_channel *ch = ctx_buf;
	struct accel_mlx5_dev_ctx *dev_ctx;
	struct accel_mlx5_dev *dev;
	uint32_t i;
	int rc;

	ch->devs = calloc(g_accel_mlx5.num_ctxs, sizeof(*ch->devs));
	if (!ch->devs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_accel_mlx5.num_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.dev_ctxs[i];
		dev = &ch->devs[i];
		dev->dev_ctx = dev_ctx;

		assert(dev_ctx->mkeys);
		dev->mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, 0);
		if (!dev->mkeys) {
			SPDK_ERRLOG("Failed to get mkey pool channel, dev %s\n", dev_ctx->context->device->name);
			/* Should not happen since mkey pool is created on accel_mlx5 initialization.
			 * We should not be here if pool creation failed */
			assert(0);
			goto err_out;
		}

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
		if (dev_ctx->sig_mkeys) {
			dev->sig_mkeys = spdk_mlx5_mkey_pool_get_ref(dev_ctx->pd, SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
			if (!dev->sig_mkeys) {
				SPDK_ERRLOG("Failed to get sig mkey pool channel, dev %s\n", dev_ctx->context->device->name);
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
		dev->ch = spdk_io_channel_from_ctx(ctx_buf);
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
	attr->enable_driver = false;
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
	char *str, *tmp, *tok, *sp = NULL;
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
	tok = strtok_r(str, ",", &sp);
	while (tok) {
		g_accel_mlx5.allowed_devs[devs_count] = strdup(tok);
		if (!g_accel_mlx5.allowed_devs[devs_count]) {
			free(str);
			accel_mlx5_allowed_devs_free();
			return -ENOMEM;
		}
		tok = strtok_r(NULL, ",", &sp);
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
		if (attr->num_requests / spdk_env_get_core_count() < ACCEL_MLX5_MAX_MKEYS_IN_TASK) {
			SPDK_ERRLOG("num requests per core must not be less than %u, current value %u\n",
				    ACCEL_MLX5_MAX_MKEYS_IN_TASK, attr->num_requests / spdk_env_get_core_count());
			return -EINVAL;
		}
		if (attr->qp_size < 8) {
			SPDK_ERRLOG("qp_size must be at least 8\n");
			return -EINVAL;
		}
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
accel_mlx5_psvs_release(struct accel_mlx5_dev_ctx *dev_ctx)
{
	uint32_t i, num_psvs, num_psvs_in_pool;

	if (!dev_ctx->psvs) {
		return;
	}

	num_psvs = g_accel_mlx5.attr.num_requests;

	for (i = 0; i < num_psvs; i++) {
		if (dev_ctx->psvs[i]) {
			spdk_mlx5_destroy_psv(dev_ctx->psvs[i]);
			dev_ctx->psvs[i] = NULL;
		}
	}
	free(dev_ctx->psvs);

	if (!dev_ctx->psv_pool) {
		return;
	}
	num_psvs_in_pool = spdk_mempool_count(dev_ctx->psv_pool);
	if (num_psvs_in_pool != num_psvs) {
		SPDK_ERRLOG("Expected %u reqs in the pool, but got only %u\n", num_psvs, num_psvs_in_pool);
	}
	spdk_mempool_free(dev_ctx->psv_pool);
}

static void
accel_mlx5_free_resources(void)
{
	struct accel_mlx5_dev_ctx *dev_ctx;
	uint32_t i;

	for (i = 0; i < g_accel_mlx5.num_ctxs; i++) {
		dev_ctx = &g_accel_mlx5.dev_ctxs[i];
		accel_mlx5_psvs_release(dev_ctx);
		if (dev_ctx->pd) {
			if (dev_ctx->mkeys) {
				spdk_mlx5_mkey_pool_destroy(0, dev_ctx->pd);
			}
			if (dev_ctx->crypto_mkeys) {
				spdk_mlx5_mkey_pool_destroy(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO, dev_ctx->pd);
			}
			if (dev_ctx->sig_mkeys) {
				spdk_mlx5_mkey_pool_destroy(SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE, dev_ctx->pd);
			}
			spdk_rdma_utils_put_pd(dev_ctx->pd);
		}
		if (dev_ctx->domain) {
			spdk_rdma_utils_put_memory_domain(dev_ctx->domain);
		}
	}

	free(g_accel_mlx5.dev_ctxs);
	g_accel_mlx5.dev_ctxs = NULL;
	g_accel_mlx5.initialized = false;
}

static void
accel_mlx5_deinit_cb(void *ctx)
{
	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);
	spdk_mlx5_umr_implementer_register(false);
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

static void
accel_mlx5_set_psv_in_pool(struct spdk_mempool *mp, void *cb_arg, void *_psv, unsigned obj_idx)
{
	struct spdk_rdma_utils_memory_translation translation = {};
	struct accel_mlx5_psv_pool_iter_cb_args *args = cb_arg;
	struct accel_mlx5_psv_wrapper *wrapper = _psv;
	struct accel_mlx5_dev_ctx *dev_ctx = args->dev;
	int rc;

	if (args->rc) {
		return;
	}
	assert(obj_idx < g_accel_mlx5.attr.num_requests);
	assert(dev_ctx->psvs[obj_idx] != NULL);
	memset(wrapper, 0, sizeof(*wrapper));
	wrapper->psv_index = dev_ctx->psvs[obj_idx]->index;

	rc = spdk_rdma_utils_get_translation(args->map, &wrapper->crc, sizeof(uint32_t), &translation);
	if (rc) {
		SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", &wrapper->crc, sizeof(uint32_t));
		args->rc = -EINVAL;
	} else {
		wrapper->crc_lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
	}
}

static int
accel_mlx5_psvs_create(struct accel_mlx5_dev_ctx *dev_ctx)
{
	struct accel_mlx5_psv_pool_iter_cb_args args = {
		.dev = dev_ctx
	};
	char pool_name[32];
	uint32_t i;
	uint32_t num_psvs = g_accel_mlx5.attr.num_requests;
	uint32_t cache_size;
	int rc;

	dev_ctx->psvs = calloc(num_psvs, (sizeof(struct spdk_mlx5_psv *)));
	if (!dev_ctx->psvs) {
		SPDK_ERRLOG("Failed to alloc PSVs array\n");
		return -ENOMEM;
	}
	for (i = 0; i < num_psvs; i++) {
		dev_ctx->psvs[i] = spdk_mlx5_create_psv(dev_ctx->pd);
		if (!dev_ctx->psvs[i]) {
			SPDK_ERRLOG("Failed to create PSV on dev %s\n", dev_ctx->context->device->name);
			return -EINVAL;
		}
	}

	rc = snprintf(pool_name, sizeof(pool_name), "accel_psv_%s", dev_ctx->context->device->name);
	if (rc < 0) {
		assert(0);
		return -EINVAL;
	}
	cache_size = num_psvs * 3 / 4 / spdk_env_get_core_count();
	args.map = spdk_rdma_utils_create_mem_map(dev_ctx->pd, NULL,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!args.map) {
		return -ENOMEM;
	}
	dev_ctx->psv_pool = spdk_mempool_create_ctor(pool_name, num_psvs,
			    sizeof(struct accel_mlx5_psv_wrapper),
			    cache_size, SPDK_ENV_SOCKET_ID_ANY,
			    accel_mlx5_set_psv_in_pool, &args);
	spdk_rdma_utils_free_mem_map(&args.map);
	if (!dev_ctx->psv_pool) {
		SPDK_ERRLOG("Failed to create PSV memory pool\n");
		return -ENOMEM;
	}
	if (args.rc) {
		SPDK_ERRLOG("Failed to init PSV memory pool objects, rc %d\n", args.rc);
		return args.rc;
	}

	return 0;
}


static int
accel_mlx5_dev_ctx_init(struct accel_mlx5_dev_ctx *dev_ctx, struct ibv_context *dev,
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

	rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests, 0);
	if (rc) {
		SPDK_ERRLOG("Failed to create mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
		return rc;
	}
	dev_ctx->mkeys = true;

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
	if (g_accel_mlx5.crc32c_supported) {
		rc = accel_mlx5_mkeys_create(pd, g_accel_mlx5.attr.num_requests,
					     SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE);
		if (rc) {
			SPDK_ERRLOG("Failed to create signature mkeys pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
		dev_ctx->sig_mkeys = true;
		rc = accel_mlx5_psvs_create(dev_ctx);
		if (rc) {
			SPDK_ERRLOG("Failed to create PSVs pool, rc %d, dev %s\n", rc, dev->device->name);
			return rc;
		}
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
	int best_dev_stat = 0, dev_stat;
	bool supports_crypto;
	bool find_best_dev = g_accel_mlx5.allowed_devs_count == 0;

	if (!g_accel_mlx5.enabled) {
		return -EINVAL;
	}

	spdk_spin_init(&g_accel_mlx5.lock);
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
	g_accel_mlx5.crc32c_supported = true;
	g_accel_mlx5.num_ctxs = 0;

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
		if (!caps[i].crc32c_supported) {
			SPDK_DEBUGLOG(accel_mlx5, "Disable crc32c support because dev %s doesn't support it\n",
				      rdma_devs[i]->device->name);
			g_accel_mlx5.crc32c_supported = false;
		}
		if (find_best_dev) {
			/* Find device which supports max number of offloads */
			dev_stat = (int)supports_crypto + (int)caps[i].crc32c_supported;
			if (dev_stat > best_dev_stat) {
				best_dev_stat = dev_stat;
				best_dev = i;
			}
		}
	}

	/* User didn't specify devices to use, try to select the best one */
	if (find_best_dev) {
		if (best_dev == -1) {
			best_dev = 0;
		}
		g_accel_mlx5.crypto_supported = accel_mlx5_dev_supports_crypto(&caps[best_dev]);
		g_accel_mlx5.crc32c_supported = caps[best_dev].crc32c_supported;
		SPDK_NOTICELOG("Select dev %s, crypto %d, crc32c %d\n", rdma_devs[best_dev]->device->name,
			       g_accel_mlx5.crypto_supported, g_accel_mlx5.crc32c_supported);
		first_dev = best_dev;
		num_devs = 1;
		if (g_accel_mlx5.crypto_supported) {
			const char *const dev_name[] = { rdma_devs[best_dev]->device->name };
			/* Let mlx5 library know which device to use */
			spdk_mlx5_crypto_devs_allow(dev_name, 1);
		}
	} else {
		SPDK_NOTICELOG("Found %d devices, crypto %d\n", num_devs, g_accel_mlx5.crypto_supported);
	}

	g_accel_mlx5.dev_ctxs = calloc(num_devs, sizeof(*g_accel_mlx5.dev_ctxs));
	if (!g_accel_mlx5.dev_ctxs) {
		SPDK_ERRLOG("Memory allocation failed\n");
		rc = -ENOMEM;
		goto cleanup;
	}

	for (i = first_dev; i < first_dev + num_devs; i++) {
		rc = accel_mlx5_dev_ctx_init(&g_accel_mlx5.dev_ctxs[g_accel_mlx5.num_ctxs++],
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

	if (g_accel_mlx5.attr.enable_driver) {
		SPDK_NOTICELOG("Enabling mlx5 platform driver\n");
		spdk_accel_driver_register(&g_accel_mlx5_driver);
		spdk_accel_set_driver(g_accel_mlx5_driver.name);
		spdk_mlx5_umr_implementer_register(true);
	}

	return 0;

cleanup:
	free(rdma_devs);
	free(caps);
	accel_mlx5_free_resources();
	spdk_spin_destroy(&g_accel_mlx5.lock);

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
		spdk_json_write_named_bool(w, "enable_driver", g_accel_mlx5.attr.enable_driver);
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

static void
accel_mlx5_dump_stats_json(struct spdk_json_write_ctx *w, const char *header,
			   const struct accel_mlx5_stats *stats)
{
	double idle_polls_percentage = 0;
	double cpls_per_poll = 0;
	uint64_t total_tasks = 0;
	int i;

	if (stats->polls) {
		idle_polls_percentage = (double) stats->idle_polls * 100 / stats->polls;
	}
	if (stats->polls > stats->idle_polls) {
		cpls_per_poll = (double) stats->completions / (stats->polls - stats->idle_polls);
	}
	for (i = 0; i < ACCEL_MLX5_OPC_LAST; i++) {
		total_tasks += stats->opcodes[i];
	}

	spdk_json_write_named_object_begin(w, header);

	spdk_json_write_named_object_begin(w, "umrs");
	spdk_json_write_named_uint64(w, "crypto_umrs", stats->crypto_umrs);
	spdk_json_write_named_uint64(w, "sig_umrs", stats->sig_umrs);
	spdk_json_write_named_uint64(w, "umrs", stats->umrs);
	spdk_json_write_named_uint64(w, "total", stats->crypto_umrs + stats->sig_umrs + stats->umrs);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "rdma");
	spdk_json_write_named_uint64(w, "read", stats->rdma_reads);
	spdk_json_write_named_uint64(w, "write", stats->rdma_writes);
	spdk_json_write_named_uint64(w, "total", stats->rdma_reads + stats->rdma_writes);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "polling");
	spdk_json_write_named_uint64(w, "polls", stats->polls);
	spdk_json_write_named_uint64(w, "idle_polls", stats->idle_polls);
	spdk_json_write_named_uint64(w, "completions", stats->completions);
	spdk_json_write_named_double(w, "idle_polls_percentage", idle_polls_percentage);
	spdk_json_write_named_double(w, "cpls_per_poll", cpls_per_poll);
	spdk_json_write_named_uint64(w, "nomem_qdepth", stats->nomem_qdepth);
	spdk_json_write_named_uint64(w, "nomem_mkey", stats->nomem_mkey);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "tasks");
	spdk_json_write_named_uint64(w, "copy", stats->opcodes[ACCEL_MLX5_OPC_COPY]);
	spdk_json_write_named_uint64(w, "crypto", stats->opcodes[ACCEL_MLX5_OPC_CRYPTO]);
	spdk_json_write_named_uint64(w, "crypto_mkey", stats->opcodes[ACCEL_MLX5_OPC_CRYPTO_MKEY]);
	spdk_json_write_named_uint64(w, "crc32c", stats->opcodes[ACCEL_MLX5_OPC_CRC32C]);
	spdk_json_write_named_uint64(w, "mkey", stats->opcodes[ACCEL_MLX5_OPC_MKEY]);
	spdk_json_write_named_uint64(w, "total", total_tasks);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
accel_mlx5_dump_channel_stat(struct spdk_io_channel_iter *i)
{
	struct accel_mlx5_stats ch_stat = {};
	struct accel_mlx5_dump_stats_ctx *ctx;
	struct spdk_io_channel *_ch;
	struct accel_mlx5_io_channel *ch;
	struct accel_mlx5_dev *dev;
	uint32_t j;

	ctx = spdk_io_channel_iter_get_ctx(i);
	_ch = spdk_io_channel_iter_get_channel(i);
	ch = spdk_io_channel_get_ctx(_ch);

	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_object_begin(ctx->w, spdk_thread_get_name(spdk_get_thread()));
	}
	if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
		spdk_json_write_named_array_begin(ctx->w, "devices");
	}

	for (j = 0; j < ch->num_devs; j++) {
		dev = &ch->devs[j];
		/* Save grand total and channel stats */
		accel_mlx5_add_stats(&ctx->total, &dev->stats);
		accel_mlx5_add_stats(&ch_stat, &dev->stats);
		if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
			spdk_json_write_object_begin(ctx->w);
			accel_mlx5_dump_stats_json(ctx->w, dev->dev_ctx->context->device->name, &dev->stats);
			spdk_json_write_object_end(ctx->w);
		}
	}

	if (ctx->level == ACCEL_MLX5_DUMP_STAT_LEVEL_DEV) {
		spdk_json_write_array_end(ctx->w);
	}
	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		accel_mlx5_dump_stats_json(ctx->w, "channel_total", &ch_stat);
		spdk_json_write_object_end(ctx->w);
		spdk_json_write_object_end(ctx->w);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
accel_mlx5_dump_channel_stat_done(struct spdk_io_channel_iter *i, int status)
{
	struct accel_mlx5_dump_stats_ctx *ctx;

	ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_spin_lock(&g_accel_mlx5.lock);
	/* Add statistics from destroyed channels */
	accel_mlx5_add_stats(&ctx->total, &g_accel_mlx5.stats);
	spdk_spin_unlock(&g_accel_mlx5.lock);

	if (ctx->level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		/* channels[] */
		spdk_json_write_array_end(ctx->w);
	}

	accel_mlx5_dump_stats_json(ctx->w, "total", &ctx->total);

	/* Ends the whole response which was begun in accel_mlx5_dump_stats */
	spdk_json_write_object_end(ctx->w);

	ctx->cb(ctx->ctx, 0);
	free(ctx);
}

int
accel_mlx5_dump_stats(struct spdk_json_write_ctx *w, enum accel_mlx5_dump_state_level level,
		      accel_mlx5_dump_stat_done_cb cb, void *ctx)
{
	struct accel_mlx5_dump_stats_ctx *stat_ctx;

	if (!w || !cb) {
		return -EINVAL;
	}
	if (!g_accel_mlx5.initialized) {
		return -ENODEV;
	}

	stat_ctx = calloc(1, sizeof(*stat_ctx));
	if (!stat_ctx) {
		return -ENOMEM;
	}
	stat_ctx->cb = cb;
	stat_ctx->ctx = ctx;
	stat_ctx->level = level;
	stat_ctx->w = w;

	spdk_json_write_object_begin(w);

	if (level != ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL) {
		spdk_json_write_named_array_begin(w, "channels");
	}

	spdk_for_each_channel(&g_accel_mlx5, accel_mlx5_dump_channel_stat, stat_ctx,
			      accel_mlx5_dump_channel_stat_done);

	return 0;
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
		return (int)g_accel_mlx5.num_ctxs;
	}

	size = spdk_min(array_size, (int)g_accel_mlx5.num_ctxs);

	for (i = 0; i < size; i++) {
		domains[i] = g_accel_mlx5.dev_ctxs[i].domain;
	}

	return (int)g_accel_mlx5.num_ctxs;
}

static inline struct accel_mlx5_dev *
accel_mlx5_ch_get_dev_by_pd(struct accel_mlx5_io_channel *accel_ch, struct ibv_pd *pd)
{
	uint32_t i;

	for (i = 0; i < accel_ch->num_devs; i++) {
		if (accel_ch->devs[i].dev_ctx->pd == pd) {
			return &accel_ch->devs[i];
		}
	}

	return NULL;
}

static inline int
accel_mlx5_task_assign_qp_by_domain_pd(struct accel_mlx5_task *task,
				       struct accel_mlx5_io_channel *acce_ch, struct spdk_memory_domain *domain)
{
	struct spdk_memory_domain_rdma_ctx *domain_ctx;
	struct accel_mlx5_dev *dev;
	struct ibv_pd *domain_pd;
	size_t ctx_size;

	domain_ctx = spdk_memory_domain_get_user_context(domain, &ctx_size);
	if (spdk_unlikely(!domain_ctx || domain_ctx->size != ctx_size)) {
		SPDK_ERRLOG("no domain context or wrong size, ctx ptr %p, size %zu\n", domain_ctx, ctx_size);
		return -ENOTSUP;
	}
	domain_pd = domain_ctx->ibv_pd;
	if (spdk_unlikely(!domain_pd)) {
		SPDK_ERRLOG("no destination domain PD, task %p", task);
		return -ENOTSUP;
	}
	dev = accel_mlx5_ch_get_dev_by_pd(acce_ch, domain_pd);
	if (spdk_unlikely(!dev)) {
		SPDK_ERRLOG("No dev for PD %p dev %s\n", domain_pd, domain_pd->context->device->name);
		return -ENODEV;
	}

	if (spdk_unlikely(!dev)) {
		return -ENODEV;
	}
	task->qp = &dev->qp;

	return 0;
}

static inline int
accel_mlx5_driver_examine_sequence(struct spdk_accel_sequence *seq,
				   struct accel_mlx5_io_channel *accel_ch)
{
	struct spdk_accel_task *first_base = spdk_accel_sequence_first_task(seq);
	struct accel_mlx5_task *first = SPDK_CONTAINEROF(first_base, struct accel_mlx5_task, base);
	struct spdk_accel_task *next_base = TAILQ_NEXT(first_base, seq_link);
	struct accel_mlx5_task *next;
	int rc;

	accel_mlx5_task_reset(first);
	SPDK_DEBUGLOG(accel_mlx5, "first %p, opc %d; next %p, opc %d\n", first_base, first_base->op_code,
		      next_base,  next_base ? next_base->op_code : -1);
	if (!next_base) {
		if (first_base->op_code == SPDK_ACCEL_OPC_COPY && first_base->dst_domain &&
		    spdk_memory_domain_get_dma_device_type(first_base->dst_domain) ==
		    SPDK_DMA_DEVICE_TYPE_RDMA &&
		    accel_mlx5_compare_iovs(first_base->d.iovs, first_base->s.iovs, first_base->s.iovcnt)) {
			SPDK_DEBUGLOG(accel_mlx5, "MKEY task %p\n", first);
			rc = accel_mlx5_task_assign_qp_by_domain_pd(first, accel_ch, first_base->dst_domain);
			if (spdk_unlikely(rc)) {
				return rc;
			}
			first->mlx5_opcode = ACCEL_MLX5_OPC_MKEY;
			first->needs_data_transfer = 1;
			first->inplace = 1;
			return 0;
		}
	} else {
		switch (first_base->op_code) {
		case SPDK_ACCEL_OPC_COPY:
			if (next_base->op_code == SPDK_ACCEL_OPC_DECRYPT &&
			    first_base->dst_domain &&  spdk_memory_domain_get_dma_device_type(first_base->dst_domain) ==
			    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
				next = SPDK_CONTAINEROF(next_base, struct accel_mlx5_task, base);
				rc = accel_mlx5_task_assign_qp_by_domain_pd(next, accel_ch, first_base->dst_domain);
				if (spdk_unlikely(rc)) {
					return rc;
				}
				/* Update decrypt task memory domain, complete copy task */
				SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and decrypt (%p)\n", first, next);
				next_base->dst_domain = first_base->dst_domain;
				next_base->dst_domain_ctx = first_base->dst_domain_ctx;
				accel_mlx5_task_reset(next);
				next->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO_MKEY;
				next->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
				next->needs_data_transfer = 1;
				next->inplace = 1;
				spdk_accel_task_complete(first_base, 0);
				return 0;
			}
			break;
		case SPDK_ACCEL_OPC_ENCRYPT:
			if (next_base->op_code == SPDK_ACCEL_OPC_COPY &&
			    next_base->dst_domain && spdk_memory_domain_get_dma_device_type(next_base->dst_domain) ==
			    SPDK_DMA_DEVICE_TYPE_RDMA && TAILQ_NEXT(next_base, seq_link) == NULL) {
				rc = accel_mlx5_task_assign_qp_by_domain_pd(first, accel_ch, next_base->dst_domain);
				if (spdk_unlikely(rc)) {
					return rc;
				}

				/* Update encrypt task memory domain, complete copy task */
				SPDK_DEBUGLOG(accel_mlx5, "Merge copy task (%p) and decrypt (%p)\n",
					      SPDK_CONTAINEROF(next_base,
							       struct accel_mlx5_task, base), first);
				first_base->dst_domain = next_base->dst_domain;
				first_base->dst_domain_ctx = next_base->dst_domain_ctx;
				first->mlx5_opcode = ACCEL_MLX5_OPC_CRYPTO_MKEY;
				first->enc_order = SPDK_MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
				first->needs_data_transfer = 1;
				first->inplace = 1;
				spdk_accel_task_complete(next_base, 0);
				return 0;
			}
			break;

		default:
			break;
		}
	}

	SPDK_DEBUGLOG(accel_mlx5, "seq %p, task %p nothing to merge\n", seq, first_base);
	/* Nothing to merge, execute tasks one by one */
	accel_mlx5_task_assign_qp(first, accel_ch);
	accel_mlx5_task_init_opcode(first);

	return 0;
}

static inline int
accel_mlx5_execute_sequence(struct spdk_io_channel *ch, struct spdk_accel_sequence *seq)
{
	struct accel_mlx5_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct accel_mlx5_task *mlx5_task;
	int rc;

	rc = accel_mlx5_driver_examine_sequence(seq, accel_ch);
	if (spdk_unlikely(rc)) {
		return rc;
	}
	task = spdk_accel_sequence_first_task(seq);
	assert(task);
	mlx5_task = SPDK_CONTAINEROF(task, struct accel_mlx5_task, base);
	mlx5_task->driver_seq = 1;

	SPDK_DEBUGLOG(accel_mlx5, "driver starts seq %p, ch %p, task %p\n", seq, accel_ch, task);

	return _accel_mlx5_submit_tasks(accel_ch, task);
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

static struct spdk_accel_driver g_accel_mlx5_driver = {
	.name			= "mlx5",
	.execute_sequence	= accel_mlx5_execute_sequence,
	.get_io_channel		= accel_mlx5_get_io_channel
};

SPDK_LOG_REGISTER_COMPONENT(accel_mlx5)
