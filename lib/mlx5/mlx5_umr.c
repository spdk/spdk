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

#define MLX5_UMR_POOL_VALID_FLAGS_MASK (~(SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO))

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
			  params->cache_per_thread, SPDK_ENV_SOCKET_ID_ANY,
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
