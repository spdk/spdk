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

#ifndef __VRDMA_DPA_H__
#define __VRDMA_DPA_H__

#include "spdk/log.h"
#include <libflexio/flexio.h>
#include <infiniband/mlx5dv.h>
#include "lib/vrdma/vrdma_providers.h"

#define VRDMA_MAX_CORES_AVAILABLE 13
#define VRDMA_MAX_HARTS_PER_CORE  16

struct vrdma_dpa_ctx {
	struct flexio_process *flexio_process;
	struct flexio_outbox *db_outbox;
	struct mlx5dv_devx_uar *emu_uar;
	struct flexio_uar *flexio_uar;
	struct flexio_window *window;
	void *elf_buf;
	uint8_t hart_count;
	uint8_t core_count;
	struct vrdma_dpa_vq_data *vq_data;
	struct ibv_mr *vq_counter_mr;
};

struct vrdma_dpa_msix {
	uint32_t cqn;
	uint32_t eqn;
	struct mlx5dv_devx_obj *obj;
};

/*now no sf, emu manager responsed emu device, so here, sf_uar is emu_manager_uar */
struct vrdma_dpa_emu_dev_ctx {
	struct flexio_process *flexio_process;
	uint32_t *heap_mkey;
	struct mlx5dv_devx_uar *sf_uar; 
	struct flexio_uar *flexio_uar;
	struct flexio_outbox *db_sf_outbox;
	// flexio_uintptr_t dev_ctx_daddr; /*point to used_idx*/
	// struct flexio_mkey *dmem_key;   /*used for used_idx*/
	struct flexio_msix *flexio_msix;
	uint16_t msix_config_vector;
	struct vrdma_dpa_msix *msix;
};

#define log_error(...) spdk_log(SPDK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_debug(...) spdk_log(SPDK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_notice(...) spdk_log(SPDK_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(...)  spdk_log(SPDK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)


#ifdef DEBUG
#define vrdma_dpa_dev_print_init flexio_print_init
#else
static inline flexio_status
vrdma_dpa_dev_print_init(struct flexio_process *process,
			 struct flexio_uar *flexio_uar, size_t data_bsize,
			 FILE *out, int is_async, pthread_t *ppthread)
{
	return 0;
}
#endif



int vrdma_dpa_init(const struct vrdma_prov_init_attr *attr, void **out);
void vrdma_dpa_uninit(void *in);
int vrdma_dpa_emu_dev_init(const struct vrdma_prov_emu_dev_init_attr *attr,
			     void **out);
void vrdma_dpa_emu_dev_uninit(void *emu_dev_handler);

#endif