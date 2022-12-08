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
// #include "snap_vrdma_virtq.h"

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

struct snap_vrdma_vq_create_dpa_attr {
	void *bdev;
	struct ibv_pd *pd;
	uint32_t sq_size;
	uint32_t rq_size;
	uint16_t tx_elem_size;
	uint16_t rx_elem_size;
	uint32_t vqpn;
	uint16_t sq_msix_vector;
	uint16_t rq_msix_vector;
	// uint16_t num_msix;
	/*host wr address*/
	// struct vrdma_q_comm host_rq_param;
	// struct vrdma_q_comm host_sq_param;

	/*both host and arm wr address*/
	struct vrdma_rq rq;
	struct vrdma_sq sq;
	/*arm mr & pi*/
	uint32_t lkey;
	uint16_t sq_pi;
	uint16_t rq_pi;
};

struct vrdma_prov_vq {
	// uint16_t idx;
	struct vrdma_dpa_vq *dpa_vq;//void    *dpa_q;
	uint16_t dpa_qpn; /*it is dpa_vq's qpn*/
	// struct snap_dma_q *dma_q;
};

struct vrdma_vq_ops {
	struct snap_vrdma_queue *(*create)(struct vrdma_ctrl *ctrl, struct snap_vrdma_vq_create_dpa_attr* q_attr);
	void (*destroy)(struct snap_vrdma_queue *virtq);
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
	// int (*msix_send)(void *handler); ---------later
};

int vrdma_prov_init(const struct vrdma_prov_init_attr *prov_init_attr,
		      void **prov_ctx_out);
void vrdma_prov_uninit(void *prov_ctx_in);
int vrdma_prov_emu_dev_init(const struct vrdma_prov_emu_dev_init_attr *emu_attr,
			  void **emu_ctx_out);
void vrdma_prov_emu_dev_uninit(void *emu_ctx_in);
struct snap_vrdma_queue* vrdma_prov_vq_create(struct vrdma_ctrl *ctrl,
		       struct snap_vrdma_vq_create_dpa_attr *attr);
void vrdma_prov_vq_destroy(struct snap_vrdma_queue *vq);
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