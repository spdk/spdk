/*
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <libflexio/flexio.h>
#include "spdk/vrdma_controller.h"
#include "vrdma_dpa_vq.h"
#include "vrdma_dpa_mm.h"
#include "vrdma_dpa.h"

int vrdma_dpa_mm_zalloc(struct flexio_process *process, size_t buff_bsize,
			  flexio_uintptr_t *dest_daddr_p)
{
	flexio_status err;

	err = flexio_buf_dev_alloc(process, buff_bsize, dest_daddr_p);
	if (err) {
		log_error("Fail to alloc buffer, err(%d)", err);
		return err;
	}

	err = flexio_buf_dev_memset(process, 0, buff_bsize, *dest_daddr_p);
	if (err) {
		log_error("Fail to memset buffer, err(%d)", err);
		goto err_memeset;
	}

	return 0;

err_memeset:
	flexio_buf_dev_free(process, *dest_daddr_p);
	return err;
}

int vrdma_dpa_mm_free(struct flexio_process *process, flexio_uintptr_t daddr)
{
	flexio_status err;

	err = flexio_buf_dev_free(process, daddr);
	if (err) {
		log_error("Fail to free buffer, err(%d)", err);
		return err;
	}

	return 0;
}


flexio_uintptr_t vrdma_dpa_mm_dbr_alloc(struct flexio_process *process)
{
	flexio_uintptr_t dbr_daddr;
	uint32_t dbr[2] = {};
	flexio_status err;

	err = vrdma_dpa_mm_zalloc(process, sizeof(dbr), &dbr_daddr);
	if (err) {
		log_error("Failed to allocate dev memory, err(%d)", err);
		errno = err;
		return 0;
	}

	return dbr_daddr;
}

/*BIT_ULL(log_depth) means 1<<log_depth*/
static
flexio_uintptr_t vrdma_dpa_mm_cq_ring_alloc(struct flexio_process *process,
					      int cq_size)
{
	struct mlx5_cqe64 *cq_ring_src;
	flexio_uintptr_t ring_daddr;
	struct mlx5_cqe64 *cqe;
	uint32_t ring_bsize;
	flexio_status err;
	int i;

	ring_bsize = cq_size * BIT_ULL(VRDMA_DPA_CQE_BSIZE);
	cq_ring_src = calloc(cq_size, BIT_ULL(VRDMA_DPA_CQE_BSIZE));
	if (!cq_ring_src) {
		log_error("Failed to allocate memory, err(%d)", errno);
		return 0;
	}

	cqe = cq_ring_src;
	/* Init CQEs and set ownership bit */
	for (i = 0; i < cq_size; i++)
		mlx5dv_set_cqe_owner(cqe++, 1);

	/* Copy CQEs from host to CQ ring */
	err = vrdma_dpa_mm_zalloc(process, ring_bsize, &ring_daddr);
	if (err) {
		log_error("Failed to allocate dev memory, err(%d)", err);
		errno = err;
		goto err_dev_alloc;
	}

	err = flexio_host2dev_memcpy(process, cq_ring_src,
				     ring_bsize, ring_daddr);
	if (err) {
		log_error("Failed to copy from host to dev, err(%d)", err);
		errno = err;
		goto err_host2dev_memcpy;
	}

	free(cq_ring_src);
	return ring_daddr;
err_host2dev_memcpy:
	vrdma_dpa_mm_free(process, ring_daddr);
err_dev_alloc:
	free(cq_ring_src);
	return 0;
}

int vrdma_dpa_mm_cq_alloc(struct flexio_process *process, int cq_size,
			  struct vrdma_dpa_cq *cq)
{
	int err;

	cq->cq_dbr_daddr = vrdma_dpa_mm_dbr_alloc(process);
	if (!cq->cq_dbr_daddr) {
		log_error("Failed to alloc cq ring, err(%d)", errno);
		return errno;
	}

	cq->cq_ring_daddr = vrdma_dpa_mm_cq_ring_alloc(process, cq_size);
	if (!cq->cq_ring_daddr) {
		log_error("Failed to alloc cq ring, err(%d)", errno);
		err = errno;
		goto err_alloc_cq_ring;
	}

	return 0;
err_alloc_cq_ring:
	vrdma_dpa_mm_free(process, cq->cq_dbr_daddr);
	return err;
}

void vrdma_dpa_mm_cq_free(struct flexio_process *process,
			    struct vrdma_dpa_cq *cq)
{
	vrdma_dpa_mm_free(process, cq->cq_ring_daddr);
	vrdma_dpa_mm_free(process, cq->cq_dbr_daddr);
}

flexio_uintptr_t vrdma_dpa_mm_qp_buff_alloc(struct flexio_process *process,
					      int rq_size,
					      flexio_uintptr_t *rq_daddr,
					      int sq_size,
					      flexio_uintptr_t *sq_daddr)
{
	flexio_uintptr_t buff_daddr;
	uint32_t buff_bsize = 0;
	uint32_t rq_bsize;
	uint32_t sq_bsize;
	int err;

	/* rq only has one mlx5_wqe_data_seg*/
	rq_bsize = rq_size * sizeof(struct mlx5_wqe_data_seg);
	buff_bsize += rq_bsize;

	/* sq has ctrl + 3segs */
	sq_bsize = sq_size * sizeof(struct mlx5_wqe_data_seg) * 4;
	buff_bsize += sq_bsize;

	err = vrdma_dpa_mm_zalloc(process, buff_bsize, &buff_daddr);
	if (err) {
		log_error("Failed to allocate dev buffer, err(%d)", err);
		return 0;
	}

	/* buff starts from RQ followed by SQ */
	*rq_daddr = buff_daddr;
	*sq_daddr = buff_daddr + rq_bsize;
	return buff_daddr;
}

void vrdma_dpa_mm_qp_buff_free(struct flexio_process *process,
				 flexio_uintptr_t buff_daddr)
{
	vrdma_dpa_mm_free(process, buff_daddr);
}

int vrdma_dpa_init_qp_rx_ring(struct vrdma_dpa_vq *dpa_vq,
				flexio_uintptr_t *rq_daddr,
				uint32_t num_of_wqes,
				uint32_t wqe_stride,
				uint32_t elem_size,
				uint32_t mkey_id)
{
	struct mlx5_wqe_data_seg *rx_wqes;
	struct mlx5_wqe_data_seg *dseg;
	uint32_t dbr[2] = {};
	uint32_t i;
	int err;
	
	rx_wqes = calloc(num_of_wqes, sizeof(struct mlx5_wqe_data_seg));
	if (!rx_wqes) {
		log_error("Failed to allocate wqe memory, err(%d)", errno);
		return -ENOMEM;
	}

	/* Initialize WQEs' data segment */
	dseg = rx_wqes;

	for (i = 0; i < num_of_wqes; i++) {
		mlx5dv_set_data_seg(dseg, elem_size, mkey_id,
				    dpa_vq->dma_qp.rx_wqe_buff +
				    (i * elem_size));
		dseg++;
	}

	/* Copy RX WQEs from host to dev memory */
	err = flexio_host2dev_memcpy(dpa_vq->emu_dev_ctx->flexio_process,
				     rx_wqes,
				     num_of_wqes * wqe_stride,
				     *rq_daddr);
	if (err) {
		log_error("Failed to copy qp_rq ring to dev, err(%d)", err);
		goto err_dev_cpy;
	}

	dbr[0] = htobe32(num_of_wqes & 0xffff);
	dbr[1] = htobe32(0);
	err = flexio_host2dev_memcpy(dpa_vq->emu_dev_ctx->flexio_process,
				     dbr, sizeof(dbr),
				     dpa_vq->dma_qp.dbr_daddr);
	if (err) {
		log_error("Failed to copy from host to dev, err(%d)", err);
		goto err_dev_cpy;
	}

	free(rx_wqes);
	return 0;
err_dev_cpy:
	free(rx_wqes);
	return err;
}

int vrdma_dpa_mkey_create(struct vrdma_dpa_vq *dpa_vq,
			    struct flexio_qp_attr *qp_attr,
			    uint32_t data_bsize,
				flexio_uintptr_t wqe_buff,
			    struct flexio_mkey *mkey)
{
	struct flexio_mkey_attr mkey_attr = {};
	int err;

	mkey_attr.access = qp_attr->qp_access_mask;
	mkey_attr.pd = qp_attr->pd;
	mkey_attr.daddr = wqe_buff;
	mkey_attr.len = data_bsize;
	err = flexio_device_mkey_create(dpa_vq->emu_dev_ctx->flexio_process,
					&mkey_attr, &mkey);
	if (err) {
		log_error("Failed to create mkey, err(%d)", err);
		return err;
	}

	return 0;
}

void vrdma_dpa_mkey_destroy(struct vrdma_dpa_vq *dpa_vq)
{
	if (dpa_vq->dma_qp.rqd_mkey) {
		flexio_device_mkey_destroy(dpa_vq->dma_qp.rqd_mkey);
		dpa_vq->dma_qp.rqd_mkey = NULL;
	}
	if (dpa_vq->dma_qp.sqd_mkey) {
		flexio_device_mkey_destroy(dpa_vq->dma_qp.sqd_mkey);
		dpa_vq->dma_qp.sqd_mkey = NULL;
	}
}
