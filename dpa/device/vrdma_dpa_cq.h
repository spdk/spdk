/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef __VRDMA_DPA_CQ_H__
#define __VRDMA_DPA_CQ_H__

struct vrdma_dpa_cq_ctx;

void vrdma_dpa_cq_incr(struct vrdma_dpa_cq_ctx *cq_ctx, uint16_t mask);
struct flexio_dev_cqe64 *
vrdma_dpa_cqe_get(struct vrdma_dpa_cq_ctx *ctx, uint16_t mask);
void vrdma_dpa_cq_wait(struct vrdma_dpa_cq_ctx *ctx, uint16_t mask);

static inline void vrdma_dpa_db_cq_incr(struct vrdma_dpa_cq_ctx *cq_ctx)
{
	cq_ctx->ci++;
}

#endif
