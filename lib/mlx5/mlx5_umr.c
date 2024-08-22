/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <infiniband/verbs.h>

#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/tree.h"

#include "spdk_internal/rdma_utils.h"
#include "mlx5_priv.h"
#include "mlx5_ifc.h"

#define MLX5_UMR_POOL_VALID_FLAGS_MASK (~(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE))
#define MLX5_CRYPTO_BSF_P_TYPE_CRYPTO (0x1)
#define MLX5_CRYPTO_BSF_SIZE_64B (0x2)

#define MLX5_SIG_BSF_SIZE_32B (0x1)
/* Transaction Format Selector */
#define MLX5_SIG_BSF_TFS_CRC32C (64)
#define MLX5_SIG_BSF_TFS_SHIFT (24)
/* Transaction Init/Check_gen bits */
#define MLX5_SIG_BSF_EXT_M_T_CHECK_GEN (1u << 24)
#define MLX5_SIG_BSF_EXT_M_T_INIT (1u << 25)
#define MLX5_SIG_BSF_EXT_W_T_CHECK_GEN (1u << 28)
#define MLX5_SIG_BSF_EXT_W_T_INIT (1u << 29)

RB_HEAD(mlx5_mkeys_tree, spdk_mlx5_mkey_pool_obj);

struct mlx5_relaxed_ordering_caps {
	bool relaxed_ordering_write_pci_enabled;
	bool relaxed_ordering_write;
	bool relaxed_ordering_read;
	bool relaxed_ordering_write_umr;
	bool relaxed_ordering_read_umr;
};

struct mlx5_mkey_attr {
	uint64_t addr;
	uint64_t size;
	uint32_t log_entity_size;
	struct mlx5_wqe_data_seg *klm;
	uint32_t klm_count;
	/* Size of bsf in octowords. If 0 then bsf is disabled */
	uint32_t bsf_octowords;
	bool crypto_en;
	bool relaxed_ordering_write;
	bool relaxed_ordering_read;
};

struct mlx5_mkey {
	struct mlx5dv_devx_obj *devx_obj;
	uint32_t mkey;
	uint64_t addr;
};

struct spdk_mlx5_mkey_pool {
	struct ibv_pd *pd;
	struct spdk_mempool *mpool;
	struct mlx5_mkeys_tree tree;
	struct mlx5_mkey **mkeys;
	uint32_t num_mkeys;
	uint32_t refcnt;
	uint32_t flags;
	TAILQ_ENTRY(spdk_mlx5_mkey_pool) link;
};

static bool g_umr_implementer_registered;

static int
mlx5_key_obj_compare(struct spdk_mlx5_mkey_pool_obj *key1, struct spdk_mlx5_mkey_pool_obj *key2)
{
	return key1->mkey < key2->mkey ? -1 : key1->mkey > key2->mkey;
}

RB_GENERATE_STATIC(mlx5_mkeys_tree, spdk_mlx5_mkey_pool_obj, node, mlx5_key_obj_compare);

static TAILQ_HEAD(mlx5_mkey_pool_head,
		  spdk_mlx5_mkey_pool) g_mkey_pools = TAILQ_HEAD_INITIALIZER(g_mkey_pools);
static pthread_mutex_t g_mkey_pool_lock = PTHREAD_MUTEX_INITIALIZER;

#define SPDK_KLM_MAX_TRANSLATION_ENTRIES_NUM   128

static struct mlx5_mkey *
mlx5_mkey_create(struct ibv_pd *pd, struct mlx5_mkey_attr *attr)
{
	struct mlx5_wqe_data_seg *klms = attr->klm;
	uint32_t klm_count = attr->klm_count;
	int in_size_dw = DEVX_ST_SZ_DW(create_mkey_in) +
			 (klm_count ? SPDK_ALIGN_CEIL(klm_count, 4) : 0) * DEVX_ST_SZ_DW(klm);
	uint32_t in[in_size_dw];
	uint32_t out[DEVX_ST_SZ_DW(create_mkey_out)] = {0};
	void *mkc;
	uint32_t translation_size;
	struct mlx5_mkey *cmkey;
	struct ibv_context *ctx = pd->context;
	uint32_t pd_id = 0;
	uint32_t i;
	uint8_t *klm;

	cmkey = calloc(1, sizeof(*cmkey));
	if (!cmkey) {
		SPDK_ERRLOG("failed to alloc cross_mkey\n");
		return NULL;
	}

	memset(in, 0, in_size_dw * 4);
	DEVX_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	mkc = DEVX_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);

	if (klm_count > 0) {
		klm = (uint8_t *)DEVX_ADDR_OF(create_mkey_in, in, klm_pas_mtt);
		translation_size = SPDK_ALIGN_CEIL(klm_count, 4);

		for (i = 0; i < klm_count; i++) {
			DEVX_SET(klm, klm, byte_count, klms[i].byte_count);
			DEVX_SET(klm, klm, mkey, klms[i].lkey);
			DEVX_SET64(klm, klm, address, klms[i].addr);
			klms += DEVX_ST_SZ_BYTES(klm);
		}

		for (; i < translation_size; i++) {
			DEVX_SET(klm, klms, byte_count, 0x0);
			DEVX_SET(klm, klms, mkey, 0x0);
			DEVX_SET64(klm, klms, address, 0x0);
			klm += DEVX_ST_SZ_BYTES(klm);
		}
	}

	DEVX_SET(mkc, mkc, access_mode_1_0, attr->log_entity_size ?
		 MLX5_MKC_ACCESS_MODE_KLMFBS :
		 MLX5_MKC_ACCESS_MODE_KLMS);
	DEVX_SET(mkc, mkc, log_page_size, attr->log_entity_size);

	mlx5_get_pd_id(pd, &pd_id);
	DEVX_SET(create_mkey_in, in, translations_octword_actual_size, klm_count);
	if (klm_count == 0) {
		DEVX_SET(mkc, mkc, free, 0x1);
	}
	DEVX_SET(mkc, mkc, lw, 0x1);
	DEVX_SET(mkc, mkc, lr, 0x1);
	DEVX_SET(mkc, mkc, rw, 0x1);
	DEVX_SET(mkc, mkc, rr, 0x1);
	DEVX_SET(mkc, mkc, umr_en, 1);
	DEVX_SET(mkc, mkc, qpn, 0xffffff);
	DEVX_SET(mkc, mkc, pd, pd_id);
	DEVX_SET(mkc, mkc, translations_octword_size,
		 SPDK_KLM_MAX_TRANSLATION_ENTRIES_NUM);
	DEVX_SET(mkc, mkc, relaxed_ordering_write,
		 attr->relaxed_ordering_write);
	DEVX_SET(mkc, mkc, relaxed_ordering_read,
		 attr->relaxed_ordering_read);
	DEVX_SET64(mkc, mkc, start_addr, attr->addr);
	DEVX_SET64(mkc, mkc, len, attr->size);
	DEVX_SET(mkc, mkc, mkey_7_0, 0x42);
	if (attr->crypto_en) {
		DEVX_SET(mkc, mkc, crypto_en, 1);
	}
	if (attr->bsf_octowords) {
		DEVX_SET(mkc, mkc, bsf_en, 1);
		DEVX_SET(mkc, mkc, bsf_octword_size, attr->bsf_octowords);
	}

	cmkey->devx_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out,
			  sizeof(out));
	if (!cmkey->devx_obj) {
		SPDK_ERRLOG("mlx5dv_devx_obj_create() failed to create mkey, errno:%d\n", errno);
		goto out_err;
	}

	cmkey->mkey = DEVX_GET(create_mkey_out, out, mkey_index) << 8 | 0x42;
	return cmkey;

out_err:
	free(cmkey);
	return NULL;
}

static int
mlx5_mkey_destroy(struct mlx5_mkey *mkey)
{
	int ret = 0;

	if (mkey->devx_obj) {
		ret = mlx5dv_devx_obj_destroy(mkey->devx_obj);
	}

	free(mkey);

	return ret;
}

static int
mlx5_query_relaxed_ordering_caps(struct ibv_context *context,
				 struct mlx5_relaxed_ordering_caps *caps)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE_CAP_2);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	if (ret) {
		return ret;
	}

	caps->relaxed_ordering_write_pci_enabled = DEVX_GET(query_hca_cap_out,
			out, capability.cmd_hca_cap.relaxed_ordering_write_pci_enabled);
	caps->relaxed_ordering_write = DEVX_GET(query_hca_cap_out, out,
						capability.cmd_hca_cap.relaxed_ordering_write);
	caps->relaxed_ordering_read = DEVX_GET(query_hca_cap_out, out,
					       capability.cmd_hca_cap.relaxed_ordering_read);
	caps->relaxed_ordering_write_umr = DEVX_GET(query_hca_cap_out,
					   out, capability.cmd_hca_cap.relaxed_ordering_write_umr);
	caps->relaxed_ordering_read_umr = DEVX_GET(query_hca_cap_out,
					  out, capability.cmd_hca_cap.relaxed_ordering_read_umr);
	return 0;
}

static int
mlx5_mkey_pool_create_mkey(struct mlx5_mkey **_mkey, struct ibv_pd *pd,
			   struct mlx5_relaxed_ordering_caps *caps, uint32_t flags)
{
	struct mlx5_mkey *mkey;
	struct mlx5_mkey_attr mkey_attr = {};
	uint32_t bsf_size = 0;

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = caps->relaxed_ordering_write;
	mkey_attr.relaxed_ordering_read = caps->relaxed_ordering_read;
	mkey_attr.klm_count = 0;
	mkey_attr.klm = NULL;
	if (flags & SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO) {
		mkey_attr.crypto_en = true;
		bsf_size += 64;
	}
	if (flags & SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE) {
		bsf_size += 64;
	}
	mkey_attr.bsf_octowords = bsf_size / 16;

	mkey = mlx5_mkey_create(pd, &mkey_attr);
	if (!mkey) {
		SPDK_ERRLOG("Failed to create mkey on dev %s\n", pd->context->device->name);
		return -EINVAL;
	}
	*_mkey = mkey;

	return 0;
}

static void
mlx5_set_mkey_in_pool(struct spdk_mempool *mp, void *cb_arg, void *_mkey, unsigned obj_idx)
{
	struct spdk_mlx5_mkey_pool_obj *mkey = _mkey;
	struct spdk_mlx5_mkey_pool *pool = cb_arg;

	assert(obj_idx < pool->num_mkeys);
	assert(pool->mkeys[obj_idx] != NULL);
	mkey->mkey = pool->mkeys[obj_idx]->mkey;
	mkey->pool_flag = pool->flags & 0xf;
	mkey->sig.sigerr_count = 1;
	mkey->sig.sigerr = false;

	RB_INSERT(mlx5_mkeys_tree, &pool->tree, mkey);
}

static const char *g_mkey_pool_names[] = {
	[SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO] = "crypto",
	[SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE] = "signature",
};

static void
mlx5_mkey_pool_destroy(struct spdk_mlx5_mkey_pool *pool)
{
	uint32_t i;

	if (pool->mpool) {
		spdk_mempool_free(pool->mpool);
	}
	if (pool->mkeys) {
		for (i = 0; i < pool->num_mkeys; i++) {
			if (pool->mkeys[i]) {
				mlx5_mkey_destroy(pool->mkeys[i]);
				pool->mkeys[i] = NULL;
			}
		}
		free(pool->mkeys);
	}
	TAILQ_REMOVE(&g_mkey_pools, pool, link);
	free(pool);
}

static int
mlx5_mkey_pools_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd *pd)
{
	struct spdk_mlx5_mkey_pool *new_pool;
	struct mlx5_mkey **mkeys;
	struct mlx5_relaxed_ordering_caps caps;
	uint32_t j, pdn;
	int rc;
	char pool_name[32];

	new_pool = calloc(1, sizeof(*new_pool));
	if (!new_pool) {
		rc = -ENOMEM;
		goto err;
	}
	TAILQ_INSERT_TAIL(&g_mkey_pools, new_pool, link);
	rc = mlx5_query_relaxed_ordering_caps(pd->context, &caps);
	if (rc) {
		SPDK_ERRLOG("Failed to get relaxed ordering capabilities, dev %s\n",
			    pd->context->device->dev_name);
		goto err;
	}
	mkeys = calloc(params->mkey_count, sizeof(struct mlx5_mkey *));
	if (!mkeys) {
		rc = -ENOMEM;
		goto err;
	}
	new_pool->mkeys = mkeys;
	new_pool->num_mkeys = params->mkey_count;
	new_pool->pd = pd;
	new_pool->flags = params->flags;
	for (j = 0; j < params->mkey_count; j++) {
		rc = mlx5_mkey_pool_create_mkey(&mkeys[j], pd, &caps, params->flags);
		if (rc) {
			goto err;
		}
	}
	rc = mlx5_get_pd_id(pd, &pdn);
	if (rc) {
		SPDK_ERRLOG("Failed to get pdn, pd %p\n", pd);
		goto err;
	}
	rc = snprintf(pool_name, 32, "%s_%s_%04u", pd->context->device->name,
		      g_mkey_pool_names[new_pool->flags], pdn);
	if (rc < 0) {
		goto err;
	}
	RB_INIT(&new_pool->tree);
	new_pool->mpool = spdk_mempool_create_ctor(pool_name, params->mkey_count,
			  sizeof(struct spdk_mlx5_mkey_pool_obj),
			  params->cache_per_thread, SPDK_ENV_NUMA_ID_ANY,
			  mlx5_set_mkey_in_pool, new_pool);
	if (!new_pool->mpool) {
		SPDK_ERRLOG("Failed to create mempool\n");
		rc = -ENOMEM;
		goto err;
	}

	return 0;

err:
	mlx5_mkey_pool_destroy(new_pool);

	return rc;
}

static struct spdk_mlx5_mkey_pool *
mlx5_mkey_pool_get(struct ibv_pd *pd, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool *pool;

	TAILQ_FOREACH(pool, &g_mkey_pools, link) {
		if (pool->pd == pd && pool->flags == flags) {
			return pool;
		}
	}

	return NULL;
}

int
spdk_mlx5_mkey_pool_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd *pd)
{
	int rc;

	if (!pd) {
		return -EINVAL;
	}

	if (!params || !params->mkey_count) {
		return -EINVAL;
	}
	if ((params->flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		SPDK_ERRLOG("Invalid flags %x\n", params->flags);
		return -EINVAL;
	}
	if ((params->flags & (SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE)) ==
	    (SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO | SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE)) {
		SPDK_ERRLOG("Both crypto and signature capabilities are not supported\n");
		return -EINVAL;
	}
	if (params->cache_per_thread > params->mkey_count || !params->cache_per_thread) {
		params->cache_per_thread = params->mkey_count * 3 / 4 / spdk_env_get_core_count();
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	if (mlx5_mkey_pool_get(pd, params->flags) != NULL) {
		pthread_mutex_unlock(&g_mkey_pool_lock);
		return -EEXIST;
	}

	rc = mlx5_mkey_pools_init(params, pd);
	pthread_mutex_unlock(&g_mkey_pool_lock);

	return rc;
}

int
spdk_mlx5_mkey_pool_destroy(uint32_t flags, struct ibv_pd *pd)
{
	struct spdk_mlx5_mkey_pool *pool;
	int rc = 0;

	if (!pd) {
		return -EINVAL;
	}

	if ((flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		SPDK_ERRLOG("Invalid flags %x\n", flags);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	pool = mlx5_mkey_pool_get(pd, flags);
	if (!pool) {
		SPDK_ERRLOG("Cant find a pool for PD %p, flags %x\n", pd, flags);
		pthread_mutex_unlock(&g_mkey_pool_lock);
		return -ENODEV;
	}
	if (pool->refcnt) {
		SPDK_WARNLOG("Can't delete pool pd %p, dev %s\n", pool->pd, pool->pd->context->device->dev_name);
		rc = -EAGAIN;
	} else {
		mlx5_mkey_pool_destroy(pool);
	}
	pthread_mutex_unlock(&g_mkey_pool_lock);

	return rc;
}

struct spdk_mlx5_mkey_pool *
spdk_mlx5_mkey_pool_get_ref(struct ibv_pd *pd, uint32_t flags)
{
	struct spdk_mlx5_mkey_pool *pool;

	if ((flags & MLX5_UMR_POOL_VALID_FLAGS_MASK) != 0) {
		SPDK_ERRLOG("Invalid flags %x\n", flags);
		return NULL;
	}

	pthread_mutex_lock(&g_mkey_pool_lock);
	pool = mlx5_mkey_pool_get(pd, flags);
	if (pool) {
		pool->refcnt++;
	}
	pthread_mutex_unlock(&g_mkey_pool_lock);

	return pool;
}

void
spdk_mlx5_mkey_pool_put_ref(struct spdk_mlx5_mkey_pool *pool)
{
	pthread_mutex_lock(&g_mkey_pool_lock);
	pool->refcnt--;
	pthread_mutex_unlock(&g_mkey_pool_lock);
}

int
spdk_mlx5_mkey_pool_get_bulk(struct spdk_mlx5_mkey_pool *pool,
			     struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count)
{
	assert(pool->mpool);

	return spdk_mempool_get_bulk(pool->mpool, (void **)mkeys, mkeys_count);
}

void
spdk_mlx5_mkey_pool_put_bulk(struct spdk_mlx5_mkey_pool *pool,
			     struct spdk_mlx5_mkey_pool_obj **mkeys, uint32_t mkeys_count)
{
	assert(pool->mpool);

	spdk_mempool_put_bulk(pool->mpool, (void **)mkeys, mkeys_count);
}

static inline void
_mlx5_set_umr_ctrl_seg_mtt(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords,
			   uint64_t mkey_mask)
{
	ctrl->flags |= MLX5_WQE_UMR_CTRL_FLAG_INLINE;
	ctrl->klm_octowords = htobe16(klms_octowords);
	/*
	 * Going to modify two properties of KLM mkey:
	 *  1. 'free' field: change this mkey from in free to in use
	 *  2. 'len' field: to include the total bytes in iovec
	 */
	mkey_mask |= MLX5_WQE_UMR_CTRL_MKEY_MASK_FREE | MLX5_WQE_UMR_CTRL_MKEY_MASK_LEN;

	ctrl->mkey_mask |= htobe64(mkey_mask);
}

static inline void
mlx5_set_umr_ctrl_seg_mtt(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords)
{
	_mlx5_set_umr_ctrl_seg_mtt(ctrl, klms_octowords, 0);
}

static inline void
mlx5_set_umr_ctrl_seg_mtt_sig(struct mlx5_wqe_umr_ctrl_seg *ctrl, uint32_t klms_octowords)
{
	_mlx5_set_umr_ctrl_seg_mtt(ctrl, klms_octowords, MLX5_WQE_UMR_CTRL_MKEY_MASK_SIG_ERR);
}

static inline void
mlx5_set_umr_ctrl_seg_bsf_size(struct mlx5_wqe_umr_ctrl_seg *ctrl, int bsf_size)
{
	ctrl->bsf_octowords = htobe16(SPDK_ALIGN_CEIL(SPDK_CEIL_DIV(bsf_size, 16), 4));
}

static inline void
mlx5_set_umr_mkey_seg_mtt(struct mlx5_wqe_mkey_context_seg *mkey,
			  struct spdk_mlx5_umr_attr *umr_attr)
{
	mkey->len = htobe64(umr_attr->umr_len);
}

static void
mlx5_set_umr_mkey_seg(struct mlx5_wqe_mkey_context_seg *mkey,
		      struct spdk_mlx5_umr_attr *umr_attr)
{
	memset(mkey, 0, 64);
	mlx5_set_umr_mkey_seg_mtt(mkey, umr_attr);
}

static void
mlx5_set_umr_mkey_seg_sig(struct mlx5_wqe_mkey_context_seg *mkey,
			  struct spdk_mlx5_umr_sig_attr *sig_attr)
{
	mkey->flags_pd = htobe32((sig_attr->sigerr_count & 1) << 26);
}

static inline void
mlx5_set_umr_inline_klm_seg(struct mlx5_wqe_umr_klm_seg *klm, struct ibv_sge *sge)
{
	klm->byte_count = htobe32(sge->length);
	klm->mkey = htobe32(sge->lkey);
	klm->address = htobe64(sge->addr);
}

static void *
mlx5_build_inline_mtt(struct mlx5_hw_qp *qp, uint32_t *to_end, struct mlx5_wqe_umr_klm_seg *dst_klm,
		      struct spdk_mlx5_umr_attr *umr_attr)
{
	struct ibv_sge *src_sge = umr_attr->sge;
	int num_wqebbs = umr_attr->sge_count / 4;
	int tail = umr_attr->sge_count & 0x3;
	int i;

	for (i = 0; i < num_wqebbs; i++) {
		mlx5_set_umr_inline_klm_seg(&dst_klm[0], src_sge++);
		mlx5_set_umr_inline_klm_seg(&dst_klm[1], src_sge++);
		mlx5_set_umr_inline_klm_seg(&dst_klm[2], src_sge++);
		mlx5_set_umr_inline_klm_seg(&dst_klm[3], src_sge++);
		/* sizeof(*dst_klm) * 4 == MLX5_SEND_WQE_BB */
		dst_klm = mlx5_qp_get_next_wqebb(qp, to_end, dst_klm);
	}

	if (!tail) {
		return dst_klm;
	}

	for (i = 0; i < tail; i++) {
		mlx5_set_umr_inline_klm_seg(&dst_klm[i], src_sge++);
	}

	/* Fill PAD entries to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB) */
	memset(&dst_klm[i], 0, MLX5_SEND_WQE_BB - sizeof(struct mlx5_wqe_umr_klm_seg) * tail);

	return mlx5_qp_get_next_wqebb(qp, to_end, dst_klm);
}

static inline void
mlx5_set_umr_crypto_bsf_seg(struct mlx5_crypto_bsf_seg *bsf, struct spdk_mlx5_umr_crypto_attr *attr,
			    uint32_t raw_data_size, uint8_t bsf_size)
{
	uint64_t *iv = (void *)bsf->xts_initial_tweak;

	memset(bsf, 0, sizeof(*bsf));
	switch (attr->tweak_mode) {
	case SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE:
		iv[0] = htole64(attr->xts_iv);
		iv[1] = 0;
		break;
	case SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE:
		iv[0] = 0;
		iv[1] = htobe64(attr->xts_iv);
		break;
	default:
		assert(false && "unsupported tweak mode");
		break;
	}

	bsf->size_type = (bsf_size << 6) | MLX5_CRYPTO_BSF_P_TYPE_CRYPTO;
	bsf->enc_order = attr->enc_order;
	bsf->raw_data_size = htobe32(raw_data_size);
	bsf->crypto_block_size_pointer = attr->bs_selector;
	bsf->dek_pointer = htobe32(attr->dek_obj_id);
	*((uint64_t *)bsf->keytag) = attr->keytag;
}

static inline uint8_t
mlx5_get_crc32c_tfs(uint32_t seed)
{
	assert(seed == 0 || seed == 0xffffffff);
	return MLX5_SIG_BSF_TFS_CRC32C | !seed;
}

static inline void
mlx5_set_umr_sig_bsf_seg(struct mlx5_sig_bsf_seg *bsf,
			 struct spdk_mlx5_umr_sig_attr *attr)
{
	uint8_t bsf_size = MLX5_SIG_BSF_SIZE_32B;
	uint32_t tfs_psv;
	uint32_t init_gen;

	memset(bsf, 0, sizeof(*bsf));
	bsf->basic.bsf_size_sbs = (bsf_size << 6);
	bsf->basic.raw_data_size = htobe32(attr->raw_data_size);
	bsf->basic.check_byte_mask = 0xff;

	tfs_psv = mlx5_get_crc32c_tfs(attr->seed);
	tfs_psv = tfs_psv << MLX5_SIG_BSF_TFS_SHIFT;
	tfs_psv |= attr->psv_index & 0xffffff;

	if (attr->domain == SPDK_MLX5_UMR_SIG_DOMAIN_WIRE) {
		bsf->ext.w_tfs_psv = htobe32(tfs_psv);
		init_gen = attr->init ? MLX5_SIG_BSF_EXT_W_T_INIT : 0;
		if (attr->check_gen) {
			init_gen |= MLX5_SIG_BSF_EXT_W_T_CHECK_GEN;
		}
		bsf->ext.t_init_gen_pro_size = htobe32(init_gen);
	} else {
		bsf->ext.m_tfs_psv = htobe32(tfs_psv);
		init_gen = attr->init ? MLX5_SIG_BSF_EXT_M_T_INIT : 0;
		if (attr->check_gen) {
			init_gen |= MLX5_SIG_BSF_EXT_M_T_CHECK_GEN;
		}
		bsf->ext.t_init_gen_pro_size = htobe32(init_gen);
	}
}

static inline void
mlx5_umr_configure_with_wrap_around_crypto(struct spdk_mlx5_qp *qp,
		struct spdk_mlx5_umr_attr *umr_attr, struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id,
		uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb, uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	struct mlx5_crypto_bsf_seg *bsf;
	uint8_t fm_ce_se;
	uint32_t pi, to_end;

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_crypto_bsf_seg));

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	mlx5_set_umr_mkey_seg(mkey, umr_attr);

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	bsf = mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);

	mlx5_set_umr_crypto_bsf_seg(bsf, crypto_attr, umr_attr->umr_len, MLX5_CRYPTO_BSF_SIZE_64B);

	mlx5_qp_wqe_submit(qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(qp->tx_available >= umr_wqe_n_bb);
	qp->tx_available -= umr_wqe_n_bb;
}

static inline void
mlx5_umr_configure_full_crypto(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			       struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id,
			       uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			       uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	struct mlx5_crypto_bsf_seg *bsf;
	uint8_t fm_ce_se;
	uint32_t pi;
	uint32_t i;

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_crypto_bsf_seg));

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	mlx5_set_umr_mkey_seg(mkey, umr_attr);

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	for (i = 0; i < umr_attr->sge_count; i++) {
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen wrap around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	bsf = (struct mlx5_crypto_bsf_seg *)klm;
	mlx5_set_umr_crypto_bsf_seg(bsf, crypto_attr, umr_attr->umr_len, MLX5_CRYPTO_BSF_SIZE_64B);

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	dv_qp->tx_available -= umr_wqe_n_bb;
}

int
spdk_mlx5_umr_configure_crypto(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			       struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	uint32_t pi, to_end, umr_wqe_n_bb;
	uint32_t wqe_size, mtt_size;
	uint32_t inline_klm_size;

	if (!spdk_unlikely(umr_attr->sge_count)) {
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/*
	 * UMR WQE LAYOUT:
	 * -----------------------------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt | inline crypto bsf |
	 * -----------------------------------------------------------------------
	 *   16bytes    48bytes    64bytes   sge_count*16 bytes      64 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) +
		   sizeof(struct mlx5_wqe_mkey_context_seg);
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	inline_klm_size = mtt_size * sizeof(struct mlx5_wqe_umr_klm_seg);
	wqe_size += inline_klm_size;
	wqe_size += sizeof(struct mlx5_crypto_bsf_seg);

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		mlx5_umr_configure_with_wrap_around_crypto(qp, umr_attr, crypto_attr, wr_id, flags, wqe_size,
				umr_wqe_n_bb,
				mtt_size);
	} else {
		mlx5_umr_configure_full_crypto(qp, umr_attr, crypto_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
					       mtt_size);
	}

	return 0;
}

static inline void
mlx5_umr_configure_with_wrap_around_sig(struct spdk_mlx5_qp *dv_qp,
					struct spdk_mlx5_umr_attr *umr_attr,
					struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id,
					uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb, uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	struct mlx5_sig_bsf_seg *bsf;
	uint8_t fm_ce_se;
	uint32_t pi, to_end;

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt_sig(umr_ctrl, mtt_size);
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_sig_bsf_seg));

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	mlx5_set_umr_mkey_seg(mkey, umr_attr);
	mlx5_set_umr_mkey_seg_sig(mkey, sig_attr);

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	bsf = mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);

	mlx5_set_umr_sig_bsf_seg(bsf, sig_attr);

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	dv_qp->tx_available -= umr_wqe_n_bb;
}

static inline void
mlx5_umr_configure_full_sig(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			    struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id,
			    uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			    uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	struct mlx5_sig_bsf_seg *bsf;
	uint8_t fm_ce_se;
	uint32_t pi;
	uint32_t i;

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt_sig(umr_ctrl, mtt_size);
	mlx5_set_umr_ctrl_seg_bsf_size(umr_ctrl, sizeof(struct mlx5_sig_bsf_seg));

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	memset(mkey, 0, sizeof(*mkey));
	mlx5_set_umr_mkey_seg_mtt(mkey, umr_attr);
	mlx5_set_umr_mkey_seg_sig(mkey, sig_attr);

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	for (i = 0; i < umr_attr->sge_count; i++) {
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen warp around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	bsf = (struct mlx5_sig_bsf_seg *)klm;
	mlx5_set_umr_sig_bsf_seg(bsf, sig_attr);

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	dv_qp->tx_available -= umr_wqe_n_bb;
}

int
spdk_mlx5_umr_configure_sig(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			    struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	uint32_t pi, to_end, umr_wqe_n_bb;
	uint32_t wqe_size, mtt_size;
	uint32_t inline_klm_size;

	if (!spdk_unlikely(umr_attr->sge_count)) {
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/*
	 * UMR WQE LAYOUT:
	 * -----------------------------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt | inline sig bsf |
	 * -----------------------------------------------------------------------
	 *   16bytes    48bytes    64bytes   sg_count*16 bytes      64 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) +
		   sizeof(struct mlx5_wqe_mkey_context_seg);
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	inline_klm_size = mtt_size * sizeof(struct mlx5_wqe_umr_klm_seg);
	wqe_size += inline_klm_size;
	wqe_size += sizeof(struct mlx5_sig_bsf_seg);

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		mlx5_umr_configure_with_wrap_around_sig(qp, umr_attr, sig_attr, wr_id, flags, wqe_size,
							umr_wqe_n_bb, mtt_size);
	} else {
		mlx5_umr_configure_full_sig(qp, umr_attr, sig_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
					    mtt_size);
	}

	return 0;
}

static inline void
mlx5_umr_configure_full(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
			uint64_t wr_id, uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
			uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	uint8_t fm_ce_se;
	uint32_t pi;
	uint32_t i;

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);

	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);

	/* build mkey context segment */
	mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	mlx5_set_umr_mkey_seg(mkey, umr_attr);

	klm = (struct mlx5_wqe_umr_klm_seg *)(mkey + 1);
	for (i = 0; i < umr_attr->sge_count; i++) {
		mlx5_set_umr_inline_klm_seg(klm, &umr_attr->sge[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		klm = klm + 1;
	}
	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen warp around during fill PAD entries. */
	for (; i < mtt_size; i++) {
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	dv_qp->tx_available -= umr_wqe_n_bb;
}

static inline void
mlx5_umr_configure_with_wrap_around(struct spdk_mlx5_qp *dv_qp, struct spdk_mlx5_umr_attr *umr_attr,
				    uint64_t wr_id, uint32_t flags, uint32_t wqe_size, uint32_t umr_wqe_n_bb,
				    uint32_t mtt_size)
{
	struct mlx5_hw_qp *hw = &dv_qp->hw;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	struct mlx5_wqe_umr_klm_seg *klm;
	uint8_t fm_ce_se;
	uint32_t pi, to_end;

	fm_ce_se = mlx5_qp_fm_ce_se_update(dv_qp, (uint8_t)flags);

	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;
	/*
	 * sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer wrap around.
	 *
	 * build genenal ctrl segment
	 */
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_UMR, 0,
			  hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0,
			  htobe32(umr_attr->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	memset(umr_ctrl, 0, sizeof(*umr_ctrl));
	mlx5_set_umr_ctrl_seg_mtt(umr_ctrl, mtt_size);

	/* build mkey context segment */
	mkey = mlx5_qp_get_next_wqebb(hw, &to_end, ctrl);
	mlx5_set_umr_mkey_seg(mkey, umr_attr);

	klm = mlx5_qp_get_next_wqebb(hw, &to_end, mkey);
	mlx5_build_inline_mtt(hw, &to_end, klm, umr_attr);

	mlx5_qp_wqe_submit(dv_qp, ctrl, umr_wqe_n_bb, pi);

	mlx5_qp_set_comp(dv_qp, pi, wr_id, fm_ce_se, umr_wqe_n_bb);
	assert(dv_qp->tx_available >= umr_wqe_n_bb);
	dv_qp->tx_available -= umr_wqe_n_bb;
}

int
spdk_mlx5_umr_configure(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			uint64_t wr_id, uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	uint32_t pi, to_end, umr_wqe_n_bb;
	uint32_t wqe_size, mtt_size;
	uint32_t inline_klm_size;

	if (!spdk_unlikely(umr_attr->sge_count)) {
		return -EINVAL;
	}

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/*
	 * UMR WQE LAYOUT:
	 * ---------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx | inline klm mtt |
	 * ---------------------------------------------------
	 *   16bytes    48bytes    64bytes   sg_count*16 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_umr_ctrl_seg) + sizeof(
			   struct mlx5_wqe_mkey_context_seg);
	mtt_size = SPDK_ALIGN_CEIL(umr_attr->sge_count, 4);
	inline_klm_size = mtt_size * sizeof(union mlx5_wqe_umr_inline_seg);
	wqe_size += inline_klm_size;

	umr_wqe_n_bb = SPDK_CEIL_DIV(wqe_size, MLX5_SEND_WQE_BB);
	if (spdk_unlikely(umr_wqe_n_bb > qp->tx_available)) {
		return -ENOMEM;
	}
	if (spdk_unlikely(umr_attr->sge_count > qp->max_send_sge)) {
		return -E2BIG;
	}

	if (spdk_unlikely(to_end < wqe_size)) {
		mlx5_umr_configure_with_wrap_around(qp, umr_attr, wr_id, flags, wqe_size, umr_wqe_n_bb,
						    mtt_size);
	} else {
		mlx5_umr_configure_full(qp, umr_attr, wr_id, flags, wqe_size, umr_wqe_n_bb, mtt_size);
	}

	return 0;
}

static struct mlx5dv_devx_obj *
mlx5_cmd_create_psv(struct ibv_context *context, uint32_t pdn, uint32_t *psv_index)
{
	uint32_t out[DEVX_ST_SZ_DW(create_psv_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(create_psv_in)] = {};
	struct mlx5dv_devx_obj *obj;

	assert(context);
	assert(psv_index);

	DEVX_SET(create_psv_in, in, opcode, MLX5_CMD_OP_CREATE_PSV);
	DEVX_SET(create_psv_in, in, pd, pdn);
	DEVX_SET(create_psv_in, in, num_psv, 1);

	obj = mlx5dv_devx_obj_create(context, in, sizeof(in), out, sizeof(out));
	if (obj) {
		*psv_index = DEVX_GET(create_psv_out, out, psv0_index);
	}

	return obj;
}

struct spdk_mlx5_psv *
spdk_mlx5_create_psv(struct ibv_pd *pd)
{
	uint32_t pdn;
	struct spdk_mlx5_psv *psv;
	int err;

	assert(pd);

	err = mlx5_get_pd_id(pd, &pdn);
	if (err) {
		return NULL;
	}

	psv = calloc(1, sizeof(*psv));
	if (!psv) {
		return NULL;
	}

	psv->devx_obj = mlx5_cmd_create_psv(pd->context, pdn, &psv->index);
	if (!psv->devx_obj) {
		free(psv);
		return NULL;
	}

	return psv;
}

int
spdk_mlx5_destroy_psv(struct spdk_mlx5_psv *psv)
{
	int ret;

	ret = mlx5dv_devx_obj_destroy(psv->devx_obj);
	if (!ret) {
		free(psv);
	}

	return ret;
}

int
spdk_mlx5_qp_set_psv(struct spdk_mlx5_qp *qp, uint32_t psv_index, uint32_t crc_seed, uint64_t wr_id,
		     uint32_t flags)
{
	struct mlx5_hw_qp *hw = &qp->hw;
	uint32_t pi, wqe_size, wqe_n_bb;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_ctrl_seg *gen_ctrl;
	struct mlx5_wqe_set_psv_seg *psv;
	uint8_t fm_ce_se;
	uint64_t transient_signature = (uint64_t)crc_seed << 32;

	wqe_size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_set_psv_seg);
	/* The size of SET_PSV WQE is constant and smaller than WQE BB. */
	assert(wqe_size < MLX5_SEND_WQE_BB);
	wqe_n_bb = 1;
	if (spdk_unlikely(wqe_n_bb > qp->tx_available)) {
		return -ENOMEM;
	}

	fm_ce_se = mlx5_qp_fm_ce_se_update(qp, (uint8_t)flags);
	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	ctrl = (struct mlx5_wqe_ctrl_seg *)mlx5_qp_get_wqe_bb(hw);
	gen_ctrl = ctrl;
	mlx5_set_ctrl_seg(gen_ctrl, hw->sq_pi, MLX5_OPCODE_SET_PSV, 0, hw->qp_num, fm_ce_se,
			  SPDK_CEIL_DIV(wqe_size, 16), 0, 0);

	/* build umr PSV segment */
	psv = (struct mlx5_wqe_set_psv_seg *)(gen_ctrl + 1);
	/* Zeroing the set_psv segment and WQE padding. */
	memset(psv, 0, MLX5_SEND_WQE_BB - sizeof(struct mlx5_wqe_ctrl_seg));
	psv->psv_index = htobe32(psv_index);
	psv->transient_signature = htobe64(transient_signature);

	mlx5_qp_wqe_submit(qp, ctrl, wqe_n_bb, pi);
	mlx5_qp_set_comp(qp, pi, wr_id, fm_ce_se, wqe_n_bb);
	assert(qp->tx_available >= wqe_n_bb);
	qp->tx_available -= wqe_n_bb;

	return 0;
}

void
spdk_mlx5_umr_implementer_register(bool registered)
{
	g_umr_implementer_registered = registered;
}

bool
spdk_mlx5_umr_implementer_is_registered(void)
{
	return g_umr_implementer_registered;
}
