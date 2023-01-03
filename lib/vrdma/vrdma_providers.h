/*todo: later will move to lib vrdma */
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

#ifndef VRDMA_PROVIDERS_H
#define VRDMA_PROVIDERS_H

#include "spdk/vrdma_controller.h"
#include "snap_vrdma_virtq.h"

struct vrdma_prov_init_attr {
	struct ibv_context *emu_ctx;
	struct ibv_pd *emu_pd;
};

struct vrdma_prov_emu_dev_init_attr {
	void *dpa_handler;
	struct ibv_pd *sf_dev_pd;
	struct ibv_context *sf_ibv_ctx;
	uint16_t sf_vhca_id;
	struct ibv_context *emu_ibv_ctx;
	uint16_t emu_vhca_id;
	uint16_t num_msix;
	uint16_t msix_config_vector;
};

// struct vrdma_prov_vq {
// 	// uint16_t idx;
// 	struct vrdma_dpa_vq *dpa_vq; /*dpa_q;*/
// 	uint16_t dpa_qpn; /*it is dpa_dma_q's qpn*/
// };

struct vrdma_vq_ops {
	struct snap_vrdma_queue *(*create)(struct vrdma_ctrl *ctrl,
					   struct spdk_vrdma_qp *vqp,
					   struct snap_vrdma_vq_create_attr* q_attr);
	void (*destroy)(struct snap_vrdma_queue *virtq);
	uint32_t (*get_emu_db_to_cq_id)(struct snap_vrdma_queue *virtq);
	// int (*modify)(struct vrdma_prov_vq *vq, uint64_t mask,
				//  struct vrdma_prov_vq_attr *attr);
};

struct vrdma_prov_ops {
	struct vrdma_vq_ops *q_ops;
	int (*init)(const struct vrdma_prov_init_attr *attr, void **out);
	void (*uninit)(void *in);
	int (*emu_dev_init)(const struct vrdma_prov_emu_dev_init_attr *attr,
			    void **out);
	void (*emu_dev_uninit)(void *in);
	int (*msix_send)(void *handler);
};

int vrdma_prov_init(const struct vrdma_prov_init_attr *prov_init_attr,
		      void **prov_ctx_out);
void vrdma_prov_uninit(void *prov_ctx_in);
int vrdma_prov_emu_dev_init(const struct vrdma_prov_emu_dev_init_attr *emu_attr,
			  void **emu_ctx_out);
void vrdma_prov_emu_dev_uninit(void *emu_ctx_in);
int vrdma_prov_emu_msix_send(void *handler);

struct snap_vrdma_queue* 
vrdma_prov_vq_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
		     struct snap_vrdma_vq_create_attr *attr);
void vrdma_prov_vq_destroy(struct snap_vrdma_queue *vq);
uint32_t vrdma_prov_get_emu_db_to_cq_id(struct snap_vrdma_queue *vq);
void vrdma_prov_ops_register(const struct vrdma_prov_ops *ops);
void vrdma_prov_ops_unregister(void);
int vrdma_providers_load(void);

#define VRDMA_PROV_DECLARE(prov_ops) \
	static __attribute__((constructor)) \
	void vrdma_prov_dec_dpa(void) \
	{ \
		vrdma_prov_ops_register(&prov_ops); \
	}
#endif