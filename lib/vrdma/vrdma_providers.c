/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */
// #define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/queue.h>
#include <linux/limits.h>
#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "vrdma_providers.h"

static const struct vrdma_prov_ops *prov_ops;

int vrdma_prov_init(const struct vrdma_prov_init_attr *prov_init_attr,
		      void **prov_ctx_out)
{
	if (prov_ops && prov_ops->init)
		return prov_ops->init(prov_init_attr, prov_ctx_out);

	return 0;
}

void vrdma_prov_uninit(void *prov_ctx_in)
{
	if (prov_ops && prov_ops->uninit)
		prov_ops->uninit(prov_ctx_in);
}

int
vrdma_prov_emu_dev_init(const struct vrdma_prov_emu_dev_init_attr *emu_attr,
			  void **emu_ctx_out)
{
	if (prov_ops && prov_ops->emu_dev_init)
		return prov_ops->emu_dev_init(emu_attr, emu_ctx_out);

	return 0;
}

void vrdma_prov_emu_dev_uninit(void *emu_ctx_in)
{
	if (prov_ops && prov_ops->emu_dev_uninit)
		prov_ops->emu_dev_uninit(emu_ctx_in);
}

void vrdma_prov_vq_query(struct snap_vrdma_queue *vq)
{
	if (prov_ops && prov_ops->q_ops && prov_ops->q_ops->dbg_stats_query)
		prov_ops->q_ops->dbg_stats_query(vq);
}

// int virtnet_prov_caps_query(void *dev, struct virtnet_prov_caps *caps_out)
// {
// 	if (prov_ops && prov_ops->caps_query)
// 		return prov_ops->caps_query(dev, caps_out);

// 	return 0;
// }

int vrdma_prov_emu_msix_send(void *handler)
{
	if (prov_ops && prov_ops->msix_send)
		return prov_ops->msix_send(handler);

	return 0;
}

struct snap_vrdma_queue*
vrdma_prov_vq_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
		       struct snap_vrdma_vq_create_attr *attr)
{
	struct snap_vrdma_queue *vq = NULL;

	// attr->emu_ib_ctx = dev->ctx->emu_ib_ctx;
	// attr->emu_pd = dev->ctx->emu_ib_pd;
	// attr->emu_mkey = dev->snap.emu_x_mkey->mkey;

	// attr->sf_ib_ctx = dev->sf_ctx->dev;
	// attr->sf_pd = dev->sf_ctx->pd;
	// attr->sf_mkey = dev->snap.sf_x_mkey->mkey;
	// attr->sf_vhca_id = dev->sf_ctx->vhca_id;
	// attr->emu_vhca_id = dev->snap.pci->mpci.vhca_id;

	// attr->hw_available_index =
	// 	dev->snap.vq_attr[attr->idx].hw_available_index;
	// attr->hw_used_index =
	// 	dev->snap.vq_attr[attr->idx].hw_used_index;
	// attr->max_tunnel_desc = dev->ctx->sctx->virtio_net_caps.max_tunnel_desc;
	// virtnet_vq_get_common_config(dev, attr->idx, &attr->common);

	if (prov_ops && prov_ops->q_ops && prov_ops->q_ops->create)
		vq = prov_ops->q_ops->create(ctrl, vqp, attr);

	// if (!vq)
	// 	return NULL;
	// vq->idx = attr->idx;
	return vq;
}

void vrdma_prov_vq_destroy(struct snap_vrdma_queue *vq)
{
	if (prov_ops && prov_ops->q_ops && prov_ops->q_ops->destroy)
		prov_ops->q_ops->destroy(vq);
}

uint32_t vrdma_prov_get_emu_db_to_cq_id(struct snap_vrdma_queue *vq)
{
	if (prov_ops && prov_ops->q_ops && prov_ops->q_ops->get_emu_db_to_cq_id)
		return prov_ops->q_ops->get_emu_db_to_cq_id(vq);
	return 0xFFFFFFFF;
}


void vrdma_prov_ops_register(const struct vrdma_prov_ops *ops)
{
	if (ops) {
		prov_ops = ops;
		return;
	}

	SPDK_ERRLOG("Failed to register ops");
}

void vrdma_prov_ops_unregister(void)
{
	prov_ops = NULL;
}

// static int provider_load(const char *name)
// {
// 	void *dlhandle;
// 	char *so_name;
// 	int len;

// 	len = asprintf(&so_name, "%s.so", name);

// 	if (len < 0) {
// 		SPDK_ERRLOG("Failed to allocate memory");
// 		return -ENOMEM;
// 	}

// 	dlhandle = dlopen(so_name, RTLD_NOW);
// 	if (!dlhandle) {
// 		SPDK_ERRLOG("Failed to open %s : %s", so_name, dlerror());
// 		free(so_name);
// 		return -errno;
// 	}

// 	SPDK_ERRLOG("Provider %s is loaded", so_name);

// 	free(so_name);

// 	return 0;
// }

// int vrdma_providers_load(void)
// {
// 	int ret = 0;

// 	ret = provider_load("libprovider-dpa");

// 	return ret;
// }
