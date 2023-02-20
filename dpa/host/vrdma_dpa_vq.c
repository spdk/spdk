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

#include <math.h>
#include <libflexio/flexio.h>
#include "snap-rdma/vrdma/snap_vrdma_ctrl.h"
// #include "snap-rdma/src/snap_vrdma.h"
// #include "snap-rdma/src/mlx5_ifc.h"
#include "lib/vrdma/vrdma_providers.h"
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include "vrdma_dpa_vq.h"
#include "vrdma_dpa.h"
#include "vrdma_dpa_mm.h"
#include "../vrdma_dpa_common.h"

static char *vrdma_vq_rpc_handler[] = {
	[VRDMA_DPA_VQ_QP] = "vrdma_qp_rpc_handler",
};

static int vrdma_dpa_get_hart_to_use(struct vrdma_dpa_ctx *dpa_ctx)
{
	uint8_t hart_num = dpa_ctx->core_count * VRDMA_MAX_HARTS_PER_CORE
	                 + dpa_ctx->hart_count;
	if (dpa_ctx->core_count < VRDMA_MAX_CORES_AVAILABLE - 1) {
		dpa_ctx->core_count++;
	} else {
		dpa_ctx->core_count = 1;
		dpa_ctx->hart_count = (dpa_ctx->hart_count + 1) &
				      (VRDMA_MAX_HARTS_PER_CORE - 1);
	}
	return hart_num;
}

int vrdma_dpa_vq_pup_func_register(struct vrdma_dpa_ctx *dpa_ctx)
{
	flexio_func_t *host_stub_func_qp_rpc;
	int err;

	host_stub_func_qp_rpc = calloc(1, sizeof(flexio_func_t));
	if (!host_stub_func_qp_rpc) {
		log_error("Failed to alloc RQ RPC stub func, err(%d)", errno);
		return -ENOMEM;
	}

	err = flexio_func_pup_register(dpa_ctx->app,
				       vrdma_vq_rpc_handler[VRDMA_DPA_VQ_QP],
				       VRDMA_DPA_RPC_UNPACK_FUNC,
				       host_stub_func_qp_rpc, sizeof(uint64_t),
				       vrdma_dpa_rpc_pack_func);
	if (err) {
		log_error("Failed to register RQ RPC func, err(%d)", err);
		goto err_qp_rpc;
	}
	dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP] = host_stub_func_qp_rpc;

	return 0;

err_qp_rpc:
	free(host_stub_func_qp_rpc);
	return err;
}

void vrdma_dpa_vq_pup_func_deregister(struct vrdma_dpa_ctx *dpa_ctx)
{
	free(dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP]);
	dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP] = NULL;
}




/* we need state modify*/
static int vrdma_dpa_vq_state_modify(struct vrdma_dpa_vq *dpa_vq,
				enum vrdma_dpa_vq_state state)
{
	uint64_t rpc_ret;
	char *dst_addr;
	int value;
	int err;

	/* Just update the state. placed it as last field in struct */
	value = state;
	dst_addr = (char *)dpa_vq->heap_memory +
		   offsetof(struct vrdma_dpa_event_handler_ctx,
			    dma_qp.state);

	err = flexio_host2dev_memcpy(dpa_vq->dpa_ctx->flexio_process,
				     &value, sizeof(value),
				     (uintptr_t)dst_addr);
	if (err) {
		log_error("Failed to copy vq_state to dev, err(%d)", err);
		return err;
	}

	if (state == VRDMA_DPA_VQ_STATE_RDY) {
		err = flexio_process_call(dpa_vq->dpa_ctx->flexio_process,
					  dpa_vq->dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP],
					  &rpc_ret, dpa_vq->heap_memory);
		if (err) {
			log_error("Failed to call rpc, err(%d), rpc_ret(%ld)",
				  err, rpc_ret);
			flexio_coredump_create(dpa_vq->dpa_ctx->flexio_process, "/tmp/flexio.core");
		}
	}

 	return err;
}


#if 0

static int vrdma_dpa_vq_modify(struct vrdma_prov_vq *vq, uint64_t mask,
				 uint32_t state)
{
	struct vrdma_dpa_vq *dpa_vq = vq->dpa_q;
	int err = 0;

	// if (mask & ~VIRTNET_DPA_ALLOWED_MASK) {
	// 	log_error("Mask %#lx is not supported", mask);
	// 	return -EINVAL;
	// }

	/*Todo: later check*/
	// if (mask & SNAP_VIRTIO_NET_QUEUE_MOD_STATE) {
		err = vrdma_dpa_vq_state_modify(dpa_vq, state);
		if (err)
			log_error("Failed to modify vq(%d) state to (%d), err(%d)",
				  dpa_vq->idx, state, err);
	// }

	return err;
}
#endif

static int vrdma_event_handler_create(struct vrdma_dpa_vq *dpa_vq,
				      const char *handler,
				      flexio_func_t *handler_func,
				      struct flexio_event_handler **event_handler_ptr)
{
	struct flexio_event_handler_attr attr = {};
	int err;

	err = flexio_func_register(dpa_vq->dpa_ctx->app, handler, &handler_func);
	if (err) {
		log_error("Failed to register function, err(%d)", err);
		return err;
	}

	attr.host_stub_func = handler_func;
	attr.affinity.type = FLEXIO_AFFINITY_STRICT;
	attr.affinity.id = vrdma_dpa_get_hart_to_use(dpa_vq->dpa_ctx);

	err = flexio_event_handler_create(dpa_vq->dpa_ctx->flexio_process, &attr,
					  dpa_vq->dpa_ctx->db_outbox,
					  event_handler_ptr);
	if (err) {
		log_error("Failed to create event_handler %s, hart (%d), err(%d)", handler, attr.affinity.id, err);
		return err;
	}
	log_notice("%s use %d hart", handler, attr.affinity.id);
	return 0;
}

static int vrdma_dpa_vq_init(struct vrdma_dpa_vq *dpa_vq,
				const char *db_handler,
				const char *rq_dma_q_handler,
				flexio_uintptr_t *dpa_daddr)
{

	int err;

	err = vrdma_dpa_mm_zalloc(dpa_vq->dpa_ctx->flexio_process,
				   sizeof(struct vrdma_dpa_event_handler_ctx),
				   dpa_daddr);

	if (err) {
		log_error("Failed to allocate dev buf, err(%d)", err);
		return err;
	}

	err = vrdma_event_handler_create(dpa_vq, db_handler, 
					dpa_vq->db_handler_func,
					&dpa_vq->db_handler);

	if (err) {
		goto err_db_handler_create;
	}

	err = vrdma_event_handler_create(dpa_vq, rq_dma_q_handler,
					dpa_vq->rq_dma_q_handler_func,
					&dpa_vq->rq_dma_q_handler);

	if (err) {
		goto err_rq_dma_q_handler_create;
	}
	return 0;
err_rq_dma_q_handler_create:
	flexio_event_handler_destroy(dpa_vq->db_handler);
err_db_handler_create:
	vrdma_dpa_mm_free(dpa_vq->dpa_ctx->flexio_process, *dpa_daddr);
	return err;
}

static void vrdma_dpa_vq_uninit(struct vrdma_dpa_vq *dpa_vq)
{
	flexio_event_handler_destroy(dpa_vq->rq_dma_q_handler);
	flexio_event_handler_destroy(dpa_vq->db_handler);
	vrdma_dpa_mm_free(dpa_vq->dpa_ctx->flexio_process,
			    dpa_vq->heap_memory);
}

static int vrdma_dpa_db_cq_create(struct flexio_process *process,
			     struct ibv_context *emu_ibv_ctx,
			     struct flexio_event_handler *event_handler,
			     struct vrdma_dpa_cq *dpa_cq,
			     uint32_t emu_uar_id)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	err = vrdma_dpa_mm_cq_alloc(process, BIT_ULL(VRDMA_DB_CQ_LOG_DEPTH),
				    dpa_cq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	cq_attr.log_cq_depth = VRDMA_DB_CQ_LOG_DEPTH;
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD;
	cq_attr.thread = flexio_event_handler_get_thread(event_handler);
	cq_attr.uar_id = emu_uar_id;
	cq_attr.cq_dbr_daddr = dpa_cq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr  = dpa_cq->cq_ring_daddr;
	err = flexio_cq_create(process, emu_ibv_ctx, &cq_attr, &dpa_cq->cq);
	if (err) {
		log_error("Failed to create cq, err(%d)", err);
		goto err_cq_create;
	}
	dpa_cq->cq_num = flexio_cq_get_cq_num(dpa_cq->cq);
	dpa_cq->log_cq_size = cq_attr.log_cq_depth;
	return 0;
err_cq_create:
	vrdma_dpa_mm_cq_free(process, dpa_cq);
	return err;
}

static void vrdma_dpa_db_cq_destroy(struct vrdma_dpa_vq *dpa_vq)
{
	flexio_cq_destroy(dpa_vq->db_cq.cq);
	vrdma_dpa_mm_cq_free(dpa_vq->emu_dev_ctx->flexio_process,
			       &dpa_vq->db_cq);
}

#define VRDMA_QP_SND_RCV_PSN		0x4242
static int vrdma_dpa_dma_q_create(struct vrdma_dpa_vq *dpa_vq,
				    struct vrdma_dpa_ctx *dpa_ctx,
				    struct vrdma_prov_vq_init_attr *attr,
				    struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
				    uint32_t rqcq_num,
				    uint32_t sqcq_num)

{
	struct flexio_qp_attr_opt_param_mask qp_mask = {};
	struct flexio_qp_attr qp_attr = {};
	int err;

	qp_attr.transport_type = FLEXIO_QPC_ST_RC;
	qp_attr.log_sq_depth = log2(attr->tx_qsize);//VRDMA_DPA_CVQ_SQ_DEPTH;
	qp_attr.log_rq_depth = log2(attr->rx_qsize);//VRDMA_DPA_CVQ_RQ_DEPTH;
	qp_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	qp_attr.sq_cqn = sqcq_num;
	qp_attr.rq_cqn = rqcq_num;
	qp_attr.pd = attr->sf_pd;
	qp_attr.qp_access_mask = IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE;
	qp_attr.ops_flag = FLEXIO_QP_WR_RDMA_WRITE | FLEXIO_QP_WR_RDMA_READ |
			   FLEXIO_QP_WR_ATOMIC_CMP_AND_SWAP;
	dpa_vq->dma_qp.buff_daddr =
		vrdma_dpa_mm_qp_buff_alloc(dpa_ctx->flexio_process,
					     attr->rx_qsize,
					     &dpa_vq->dma_qp.rq_daddr,
					     attr->tx_qsize,
					     &dpa_vq->dma_qp.sq_daddr);
	if (!dpa_vq->dma_qp.buff_daddr) {
		log_error("Failed to alloc qp buff, err(%d)", errno);
		return errno;
	}

	dpa_vq->dma_qp.dbr_daddr =
		vrdma_dpa_mm_dbr_alloc(dpa_ctx->flexio_process);
	if (!dpa_vq->dma_qp.dbr_daddr) {
		log_error("Failed to alloc qp_dbr, err(%d)", errno);
		err = errno;
		goto err_alloc_dbr;
	}

	/* prepare rx ring*/
	err = vrdma_dpa_mm_zalloc(dpa_vq->emu_dev_ctx->flexio_process,
				   attr->rx_qsize * attr->rx_elem_size,
				   &dpa_vq->dma_qp.rx_wqe_buff);
	if (err) {
		log_error("Failed to allocate dev buffer, err(%d)", err);
		goto err_rx_wqe_buf_alloc;
	}

	err = vrdma_dpa_mkey_create(dpa_vq, &qp_attr,
				    attr->rx_qsize * attr->rx_elem_size,
					dpa_vq->dma_qp.rx_wqe_buff,
				    &dpa_vq->dma_qp.rqd_mkey);
	if (err) {
		log_error("Failed to create rx mkey, err(%d)", err);
		goto err_rqd_mkey_create;
	}

	err = vrdma_dpa_init_qp_rx_ring(dpa_vq, &dpa_vq->dma_qp.rq_daddr,
					  attr->rx_qsize,
					  sizeof(struct mlx5_wqe_data_seg),
					  attr->rx_elem_size,
					  flexio_mkey_get_id(dpa_vq->dma_qp.rqd_mkey));
	if (err) {
		log_error("Failed to init QP Rx, err(%d)", err);
		goto err_init_qp_rx_ring;
	}


	/* prepare tx ring */
	err = vrdma_dpa_mm_zalloc(dpa_vq->emu_dev_ctx->flexio_process,
				   attr->tx_qsize * attr->tx_elem_size,
				   &dpa_vq->dma_qp.tx_wqe_buff);
	if (err) {
		log_error("Failed to allocate tx dev buffer, err(%d)", err);
		goto err_tx_wqe_buff_alloc;
	}

	err = vrdma_dpa_mkey_create(dpa_vq, &qp_attr,
				      attr->tx_qsize * attr->tx_elem_size,
					  dpa_vq->dma_qp.tx_wqe_buff,
				      &dpa_vq->dma_qp.sqd_mkey);
	if (err) {
		log_error("Failed to create tx mkey, err(%d)", err);
		goto err_sqd_mkey_create;
	}

	qp_attr.qp_wq_buff_qmem.memtype = FLEXIO_MEMTYPE_DPA;
	qp_attr.qp_wq_buff_qmem.daddr = dpa_vq->dma_qp.buff_daddr;
	qp_attr.qp_wq_dbr_qmem.daddr = dpa_vq->dma_qp.dbr_daddr;
	err = flexio_qp_create(dpa_ctx->flexio_process, attr->sf_ib_ctx,
			       &qp_attr, &dpa_vq->dma_qp.qp);
	if (err) {
		log_error("Failed to create QP, err (%d)", err);
		goto err_qp_create;
	}

	dpa_vq->dma_qp.qp_num = flexio_qp_get_qp_num(dpa_vq->dma_qp.qp);
	dpa_vq->dma_qp.log_rq_depth = qp_attr.log_rq_depth;
	dpa_vq->dma_qp.log_sq_depth = qp_attr.log_sq_depth;

	/* connect qp dev and qp host */
	qp_attr.remote_qp_num = attr->tisn_or_qpn;
	qp_attr.qp_access_mask = qp_attr.qp_access_mask;
	qp_attr.fl = 1;
	qp_attr.min_rnr_nak_timer = 0x12;
	qp_attr.path_mtu = 0x3;
	qp_attr.retry_count = 0x7;
	qp_attr.vhca_port_num = 0x1;
	/*todo: why we need this?*/
	qp_attr.next_send_psn = VRDMA_QP_SND_RCV_PSN;
	qp_attr.next_rcv_psn = VRDMA_QP_SND_RCV_PSN;
	qp_attr.next_state = FLEXIO_QP_STATE_INIT;
	err = flexio_qp_modify(dpa_vq->dma_qp.qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to INIT state, err(%d)", err);
		goto err_qp_ready;
	}

	qp_attr.next_state = FLEXIO_QP_STATE_RTR;
	err = flexio_qp_modify(dpa_vq->dma_qp.qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to RTR state, err(%d)", err);
		goto err_qp_ready;
	}

	qp_attr.next_state = FLEXIO_QP_STATE_RTS;
	err = flexio_qp_modify(dpa_vq->dma_qp.qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to RTS state, err(%d)", err);
		goto err_qp_ready;
	}

	return 0;
err_qp_ready:
	flexio_qp_destroy(dpa_vq->dma_qp.qp);
err_qp_create:
	vrdma_dpa_mkey_destroy(dpa_vq->dma_qp.sqd_mkey);
err_sqd_mkey_create:
	vrdma_dpa_mm_free(dpa_vq->emu_dev_ctx->flexio_process,
		dpa_vq->dma_qp.tx_wqe_buff);
err_tx_wqe_buff_alloc:
err_init_qp_rx_ring:
	vrdma_dpa_mkey_destroy(dpa_vq->dma_qp.rqd_mkey);
err_rqd_mkey_create:
	vrdma_dpa_mm_free(dpa_vq->emu_dev_ctx->flexio_process,
                 dpa_vq->dma_qp.rx_wqe_buff);
err_rx_wqe_buf_alloc:
	vrdma_dpa_mm_free(dpa_ctx->flexio_process,
			    dpa_vq->dma_qp.dbr_daddr);
err_alloc_dbr:
	vrdma_dpa_mm_qp_buff_free(dpa_ctx->flexio_process,
				    dpa_vq->dma_qp.buff_daddr);
	return err;
}

static void vrdma_dpa_dma_q_destroy(struct vrdma_dpa_vq *dpa_vq)
{
	flexio_qp_destroy(dpa_vq->dma_qp.qp);
	// vrdma_dpa_mkey_destroy(dpa_vq);
	flexio_device_mkey_destroy(dpa_vq->dma_qp.sqd_mkey);
	vrdma_dpa_mm_free(dpa_vq->emu_dev_ctx->flexio_process,
		dpa_vq->dma_qp.tx_wqe_buff);
	flexio_device_mkey_destroy(dpa_vq->dma_qp.rqd_mkey);
	vrdma_dpa_mm_free(dpa_vq->emu_dev_ctx->flexio_process,
                 dpa_vq->dma_qp.rx_wqe_buff);
	vrdma_dpa_mm_free(dpa_vq->emu_dev_ctx->flexio_process,
			    dpa_vq->dma_qp.dbr_daddr);
	vrdma_dpa_mm_qp_buff_free(dpa_vq->emu_dev_ctx->flexio_process,
				    dpa_vq->dma_qp.buff_daddr);
}


static int
_vrdma_dpa_dma_q_cq_create(struct flexio_process *process,
			     struct ibv_context *ibv_ctx,
			     struct flexio_event_handler *event_handler,
			     struct vrdma_dpa_cq *rq_dpacq,
			     struct vrdma_dpa_cq *sq_dpacq,
			     struct vrdma_prov_vq_init_attr *attr,
			     struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	/* QP RQ_CQ */
	err = vrdma_dpa_mm_cq_alloc(process, attr->rx_qsize,rq_dpacq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	log_notice("===naliu rx_qsize %d", attr->rx_qsize);
	cq_attr.log_cq_depth = log2(attr->rx_qsize);
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD;
	cq_attr.thread = flexio_event_handler_get_thread(event_handler);
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.cq_dbr_daddr = rq_dpacq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr = rq_dpacq->cq_ring_daddr;
	cq_attr.overrun_ignore = 1;
	err = flexio_cq_create(process, ibv_ctx, &cq_attr, &rq_dpacq->cq);
	if (err) {
		log_error("Failed to create dma_q rqcq, err(%d)", err);
		goto err_rqcq_create;
	}

	/* QP SQ_CQ */
	err = vrdma_dpa_mm_cq_alloc(process, attr->tx_qsize,
				    sq_dpacq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		goto err_rqcq_mem_alloc;
	}

	log_notice("===naliu tx_qsize %d", attr->tx_qsize);
	cq_attr.log_cq_depth = log2(attr->tx_qsize);
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ;
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.cq_dbr_daddr = sq_dpacq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr = sq_dpacq->cq_ring_daddr;
	log_notice("_vrdma_dpa_dma_q_cq_create");
	err = flexio_cq_create(process, ibv_ctx, &cq_attr, &sq_dpacq->cq);
	if (err) {
		log_error("\nFailed to create dma_q sqcq, err(%d)\n", err);
		goto err_sqcq_create;
	}

	return 0;
err_sqcq_create:
	vrdma_dpa_mm_cq_free(process, sq_dpacq);
err_rqcq_mem_alloc:
	flexio_cq_destroy(rq_dpacq->cq);
err_rqcq_create:
	vrdma_dpa_mm_cq_free(process, rq_dpacq);
	return err;
}

static int
vrdma_dpa_dma_q_cq_create(struct vrdma_dpa_vq *dpa_vq,
			    struct vrdma_dpa_ctx *dpa_ctx,
			    struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    struct vrdma_prov_vq_init_attr *attr)
{
	struct ibv_context *sf_ibv_ctx = attr->sf_ib_ctx;
	int err;

	err = _vrdma_dpa_dma_q_cq_create(dpa_ctx->flexio_process, sf_ibv_ctx,
					   dpa_vq->rq_dma_q_handler,
					   &dpa_vq->dma_q_rqcq,
					   &dpa_vq->dma_q_sqcq,
					   attr,
					   emu_dev_ctx);
	if (err) {
		log_error("Failed to create db_cq, err(%d)", err);
		return err;
	}

	dpa_vq->dma_q_rqcq.cq_num = flexio_cq_get_cq_num(dpa_vq->dma_q_rqcq.cq);
	dpa_vq->dma_q_sqcq.cq_num = flexio_cq_get_cq_num(dpa_vq->dma_q_sqcq.cq);
	dpa_vq->dma_q_rqcq.log_cq_size = attr->rx_elem_size;
	dpa_vq->dma_q_sqcq.log_cq_size = attr->tx_elem_size;

	return 0;
}

static void _vrdma_dpa_dma_q_cq_destroy(struct vrdma_dpa_vq *dpa_vq)
{
	flexio_cq_destroy(dpa_vq->dma_q_sqcq.cq);
	vrdma_dpa_mm_cq_free(dpa_vq->emu_dev_ctx->flexio_process,
			       &dpa_vq->dma_q_sqcq);
	flexio_cq_destroy(dpa_vq->dma_q_rqcq.cq);
	vrdma_dpa_mm_cq_free(dpa_vq->emu_dev_ctx->flexio_process,
			       &dpa_vq->dma_q_rqcq);
}

static void vrdma_dpa_dma_q_cq_destroy(struct vrdma_dpa_vq *dpa_vq,
					      struct vrdma_dpa_ctx *dpa_ctx)
{
	_vrdma_dpa_dma_q_cq_destroy(dpa_vq);
}

static int
vrdma_dpa_vq_event_handler_init(const struct vrdma_dpa_vq *dpa_vq,
				  struct vrdma_dpa_ctx *dpa_ctx,
				  struct vrdma_prov_vq_init_attr *attr,
				  struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	struct vrdma_dpa_event_handler_ctx *eh_data;
	uint32_t dbcq_num;
	int err;

	eh_data = calloc(1, sizeof(*eh_data));
	if (!eh_data) {
		log_error("Failed to allocate memory");
		return -ENOMEM;
	}

	eh_data->dbg_signature = DBG_EVENT_HANDLER_CHECK;

	/*prepare db handler cq ctx*/
	dbcq_num = flexio_cq_get_cq_num(dpa_vq->db_cq.cq);
	eh_data->guest_db_cq_ctx.cqn = dbcq_num;
	eh_data->guest_db_cq_ctx.ring =
		(struct flexio_dev_cqe64 *)dpa_vq->db_cq.cq_ring_daddr;
	eh_data->guest_db_cq_ctx.dbr = (uint32_t *)dpa_vq->db_cq.cq_dbr_daddr;
	eh_data->guest_db_cq_ctx.cqe = eh_data->guest_db_cq_ctx.ring;
	eh_data->guest_db_cq_ctx.hw_owner_bit = 1;

	/*prepare msix handler qp.rq.cq ctx*/
	eh_data->msix_cq_ctx.cqn = flexio_cq_get_cq_num(dpa_vq->dma_q_rqcq.cq);
	eh_data->msix_cq_ctx.ring =
		(struct flexio_dev_cqe64 *)dpa_vq->dma_q_rqcq.cq_ring_daddr;
	eh_data->msix_cq_ctx.dbr = (uint32_t *)dpa_vq->dma_q_rqcq.cq_dbr_daddr;
	eh_data->msix_cq_ctx.cqe = eh_data->msix_cq_ctx.ring;
	eh_data->msix_cq_ctx.log_cq_depth = dpa_vq->dma_q_rqcq.log_cq_size;
	eh_data->msix_cq_ctx.hw_owner_bit = 1;

	/*prepare dma_qp address*/
	eh_data->dma_qp.hw_qp_depth = attr->tx_qsize;
	eh_data->dma_qp.qp_rqcq = dpa_vq->dma_q_rqcq;
	eh_data->dma_qp.qp_sq_buff = dpa_vq->dma_qp.sq_daddr;
	eh_data->dma_qp.qp_rq_buff = dpa_vq->dma_qp.rq_daddr;
	eh_data->dma_qp.qp_num = flexio_qp_get_qp_num(dpa_vq->dma_qp.qp);
	eh_data->dma_qp.dbr_daddr = dpa_vq->dma_qp.dbr_daddr;

	/*prepare host and arm wr&pi address which used for rdma write*/
	eh_data->dma_qp.host_vq_ctx = attr->host_vq_ctx;
	eh_data->dma_qp.arm_vq_ctx  = attr->arm_vq_ctx;

	/* Update other pointers */
	eh_data->dma_qp.state = VRDMA_DPA_VQ_STATE_INIT;
	eh_data->emu_db_to_cq_id =
		dpa_vq->guest_db_to_cq_ctx.emu_db_to_cq_id;
	eh_data->emu_outbox = flexio_outbox_get_id(dpa_ctx->db_outbox);
	eh_data->sf_outbox = flexio_outbox_get_id(emu_dev_ctx->db_sf_outbox);

	eh_data->vq_index = attr->vq_idx;
	eh_data->window_id = flexio_window_get_id(dpa_ctx->window);
	// eh_data->vq_depth = attr->common.size;

	/*virtnet use host msix to send msix,but vrdma don't need, vrdma get cqn from dma rq.wqe*/
	// if (attr->msix_vector != 0xFFFF)
	// 	eh_data->msix.cqn =
	// 		emu_dev_ctx->msix[attr->msix_vector].cqn;

	err = flexio_host2dev_memcpy(dpa_ctx->flexio_process,
				     eh_data,
				     sizeof(*eh_data),
				     dpa_vq->heap_memory);
	if (err)
		log_error("Failed to copy ctx to dev, err(%d)", err);

	free(eh_data);
	return err;
}

static struct vrdma_dpa_vq* 
_vrdma_dpa_vq_create(struct vrdma_ctrl *ctrl,
		    struct vrdma_prov_vq_init_attr *attr)
{
	struct ibv_context *emu_ibv_ctx = attr->emu_ib_ctx;
	struct vrdma_msix_init_attr msix_attr = {};
	struct vrdma_dpa_vq *dpa_vq;
	uint32_t qpsq_cqnum;
	uint32_t qprq_cqnum;
	int err;

	dpa_vq = calloc(1, sizeof(struct vrdma_dpa_vq));
	if (!dpa_vq) {
		log_error("Failed to allocate dpa_vq memory");
		return NULL;
	}
	dpa_vq->dpa_ctx = ctrl->dpa_ctx;
	dpa_vq->emu_dev_ctx = ctrl->dpa_emu_dev_ctx;
	dpa_vq->idx = attr->vq_idx;
	dpa_vq->sf_mkey = attr->sf_mkey;
	dpa_vq->emu_mkey = attr->emu_mkey;

	err = vrdma_dpa_vq_init(dpa_vq, "vrdma_db_handler", "vrdma_msix_handler",
				&dpa_vq->heap_memory);
	if (err) {
		log_error("Failed to init vq, err(%d)", err);
		goto err_vq_init;
	}
	err = vrdma_dpa_db_cq_create(dpa_vq->dpa_ctx->flexio_process, emu_ibv_ctx,
				     dpa_vq->db_handler, &dpa_vq->db_cq,
				     dpa_vq->dpa_ctx->emu_uar->page_id);
	if (err) {
		log_error("Failed to create db_cq, err(%d)", err);
		goto err_db_cq_create;
	}

	dpa_vq->guest_db_to_cq_ctx.devx_emu_db_to_cq_ctx =
		mlx_devx_emu_db_to_cq_map(emu_ibv_ctx, attr->emu_vhca_id,
					attr->qdb_idx,
					flexio_cq_get_cq_num(dpa_vq->db_cq.cq),
					&dpa_vq->guest_db_to_cq_ctx.emu_db_to_cq_id);
	if (!dpa_vq->guest_db_to_cq_ctx.devx_emu_db_to_cq_ctx) {
		err = -EINVAL;
		log_error("Failed to map cq_to_db, err(%d)", err);
		goto err_db_cq_map;
	}

	msix_attr.emu_ib_ctx  = attr->emu_ib_ctx;
	msix_attr.emu_vhca_id = attr->emu_vhca_id;
	msix_attr.sf_ib_ctx   = attr->sf_ib_ctx;
	msix_attr.sf_vhca_id  = attr->sf_vhca_id;
	msix_attr.msix_vector = attr->sq_msix_vector;
	msix_attr.cq_size     = attr->tx_qsize;

	dpa_vq->sq_msix_vector = attr->sq_msix_vector;
	dpa_vq->rq_msix_vector = attr->rq_msix_vector;

	err = vrdma_dpa_msix_create(dpa_vq, dpa_vq->dpa_ctx->flexio_process,
				      &msix_attr, dpa_vq->emu_dev_ctx, attr->num_msix);
	if (err) {
		log_error("Failed to create vq msix, err(%d)", err);
		goto err_sq_msix_create;
	}

	if (attr->sq_msix_vector != attr->rq_msix_vector) {
		msix_attr.msix_vector = attr->rq_msix_vector;
		err = vrdma_dpa_msix_create(dpa_vq, dpa_vq->dpa_ctx->flexio_process,
				&msix_attr, dpa_vq->emu_dev_ctx, attr->num_msix);
		if (err) {
			log_error("Failed to create vq msix, err(%d)", err);
			goto err_rq_msix_create;
		}
	}

	log_notice("===naliu done vrdma_dpa_msix_create\n");
	err = vrdma_dpa_dma_q_cq_create(dpa_vq, dpa_vq->dpa_ctx, dpa_vq->emu_dev_ctx, attr);
	if (err) {
		log_error("Failed creating dma_q cq, err(%d)", err);
		goto err_dma_q_cq_create;
	}

	qprq_cqnum = flexio_cq_get_cq_num(dpa_vq->dma_q_rqcq.cq);
	qpsq_cqnum = flexio_cq_get_cq_num(dpa_vq->dma_q_sqcq.cq);
	err = vrdma_dpa_dma_q_create(dpa_vq, dpa_vq->dpa_ctx, attr, dpa_vq->emu_dev_ctx,
				       qprq_cqnum, qpsq_cqnum);
	if (err) {
		log_error("Failed to create QP, err(%d)", err);
		goto err_dma_q_create;
	}

	err = vrdma_dpa_vq_event_handler_init(dpa_vq, dpa_vq->dpa_ctx, attr,
						dpa_vq->emu_dev_ctx);
	if (err) {
		log_error("Failed to init event handler, err(%d)", err);
		goto err_handler_init;
	}

	err = flexio_event_handler_run(dpa_vq->db_handler, dpa_vq->heap_memory);
	if (err) {
		log_error("Failed to run event handler, err(%d)", err);
		goto err_db_handler_run;
	}

	err = flexio_event_handler_run(dpa_vq->rq_dma_q_handler, dpa_vq->heap_memory);
	if (err) {
		log_error("Failed to run event handler, err(%d)", err);
		goto err_rq_dma_q_handler_run;
	}
	err = vrdma_dpa_vq_state_modify(dpa_vq, VRDMA_DPA_VQ_STATE_RDY);
	if (err) {
		log_error("Failed to set vq state to READY, err(%d)", err);
		goto err_vq_state_modify;
	}

	return dpa_vq;
err_vq_state_modify:
err_rq_dma_q_handler_run:
err_db_handler_run:
err_handler_init:
	vrdma_dpa_dma_q_destroy(dpa_vq);
err_dma_q_create:
	vrdma_dpa_dma_q_cq_destroy(dpa_vq, dpa_vq->dpa_ctx);
err_dma_q_cq_create:
	if (attr->sq_msix_vector != attr->rq_msix_vector)
		vrdma_dpa_msix_destroy(attr->rq_msix_vector,
				 dpa_vq->emu_dev_ctx);
err_rq_msix_create:
	vrdma_dpa_msix_destroy(attr->sq_msix_vector,
				 dpa_vq->emu_dev_ctx);
err_sq_msix_create:
	mlx_devx_emu_db_to_cq_unmap(dpa_vq->guest_db_to_cq_ctx.devx_emu_db_to_cq_ctx);
err_db_cq_map:
	vrdma_dpa_db_cq_destroy(dpa_vq);
err_db_cq_create:
	vrdma_dpa_vq_uninit(dpa_vq);
err_vq_init:
	free(dpa_vq);
	return NULL;
}

static void _vrdma_dpa_vq_destroy(struct vrdma_dpa_vq *dpa_vq)
{
	vrdma_dpa_dma_q_destroy(dpa_vq);
	vrdma_dpa_dma_q_cq_destroy(dpa_vq, dpa_vq->dpa_ctx);
	vrdma_dpa_msix_destroy(dpa_vq->sq_msix_vector,
				 dpa_vq->emu_dev_ctx);
	if (dpa_vq->sq_msix_vector != dpa_vq->rq_msix_vector)
		vrdma_dpa_msix_destroy(dpa_vq->rq_msix_vector, dpa_vq->emu_dev_ctx);
	mlx_devx_emu_db_to_cq_unmap(dpa_vq->guest_db_to_cq_ctx.devx_emu_db_to_cq_ctx);
	vrdma_dpa_db_cq_destroy(dpa_vq);
	vrdma_dpa_vq_uninit(dpa_vq);
	free(dpa_vq);
}

static void vrdma_dpa_vq_destroy(struct snap_vrdma_queue *virtq)
{
	TAILQ_REMOVE(&virtq->ctrl->virtqs, virtq, vq);
	_vrdma_dpa_vq_destroy(virtq->dpa_vq);
	snap_dma_ep_destroy(virtq->dma_q);
	free(virtq);
}

static void vrdma_dpa_vq_dump_attribute(struct snap_dma_q_create_attr *rdma_qp_create_attr,
					struct vrdma_prov_vq_init_attr *attr,
					struct snap_vrdma_queue *virtq)
{
	log_notice("\n====================dump dma qp parameter======================");
	log_notice("\ntx_qsize %#x, rx_qsize %#x, tx_elem_size %#x, rx_elem_size %#x\n",
		   rdma_qp_create_attr->tx_qsize, rdma_qp_create_attr->rx_qsize,
		   rdma_qp_create_attr->tx_elem_size, rdma_qp_create_attr->rx_elem_size);

	log_notice("\n====================dump wr info in host==========================");
	log_notice("\nrq_wqe_buff_pa %#lx, rq_pi_paddr %#lx, rq_wqebb_cnt %#x,"
			"rq_wqebb_size %#x,\nsq_wqe_buff_pa %#lx, sq_pi_paddr %#lx,"
			"sq_wqebb_cnt %#x, sq_wqebb_size %#x,\nemu_crossing_mkey %#x,"
			"sf_crossing_mkey %#x\n",
			attr->host_vq_ctx.rq_wqe_buff_pa, attr->host_vq_ctx.rq_pi_paddr,
			attr->host_vq_ctx.rq_wqebb_cnt, attr->host_vq_ctx.rq_wqebb_size,
			attr->host_vq_ctx.sq_wqe_buff_pa, attr->host_vq_ctx.sq_pi_paddr,
			attr->host_vq_ctx.sq_wqebb_cnt, attr->host_vq_ctx.sq_wqebb_size,
			attr->host_vq_ctx.emu_crossing_mkey, attr->host_vq_ctx.sf_crossing_mkey);

	log_notice("\n====================dump wr info in arm==========================");
	log_notice("\nrq_buff_addr %#lx, sq_buff_addr %#lx, rq_lkey %#x, sq_lkey %#x\n",
			attr->arm_vq_ctx.rq_buff_addr, attr->arm_vq_ctx.sq_buff_addr,
			attr->arm_vq_ctx.rq_lkey, attr->arm_vq_ctx.sq_lkey);

	log_notice("\n====================dump virtq && dma qp info=====================");
	log_notice("\nemu_vhca_id %#x, sf_vhca_id %#x, vq_idx %#x, vq_qdb_idx %#x\n"
			"emu_db_to_cq_id %#x, hw_dbcq %#x\n"
			"sw_qp : %#x sqcq %#x rqcq %#x,\ndpa qp: %#x sqcq %#x rqcq %#x\n",
			attr->emu_vhca_id, attr->sf_vhca_id,
			attr->vq_idx, attr->qdb_idx,
			virtq->dpa_vq->guest_db_to_cq_ctx.emu_db_to_cq_id,
			virtq->dpa_vq->db_cq.cq_num,
		 	virtq->dma_q->sw_qp.dv_qp.hw_qp.qp_num,
		 	virtq->dma_q->sw_qp.dv_tx_cq.cq_num,
		 	virtq->dma_q->sw_qp.dv_rx_cq.cq_num,
			virtq->dpa_vq->dma_qp.qp_num,
		 	virtq->dpa_vq->dma_q_sqcq.cq_num,
		 	virtq->dpa_vq->dma_q_rqcq.cq_num);
}

static struct snap_vrdma_queue *
vrdma_dpa_vq_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
		    struct snap_vrdma_vq_create_attr* q_attr)
{
	struct snap_dma_q_create_attr rdma_qp_create_attr = {};
	struct vrdma_prov_vq_init_attr attr = {};
	struct snap_vrdma_queue *virtq;
	struct vrdma_dpa_vq *dpa_vq;
	int rc;

	virtq = calloc(1, sizeof(*virtq));
	if (!virtq) {
		log_error("create queue %d: no memory\n", q_attr->vqpn);
		return NULL;
	}

	/* create dma_qp in arm */	
	rdma_qp_create_attr.tx_qsize = q_attr->sq_size;
	rdma_qp_create_attr.rx_qsize = q_attr->rq_size;
	rdma_qp_create_attr.tx_elem_size = q_attr->tx_elem_size;
	rdma_qp_create_attr.rx_elem_size = q_attr->rx_elem_size;
	rdma_qp_create_attr.rx_cb = q_attr->rx_cb;
	rdma_qp_create_attr.uctx = virtq;
	rdma_qp_create_attr.mode = SNAP_DMA_Q_MODE_DV;
	rdma_qp_create_attr.use_devx = true;

	virtq->dma_q = snap_dma_ep_create(ctrl->pd, &rdma_qp_create_attr);
	if (!virtq->dma_q) {
		log_error("Failed creating SW QP\n");
		goto err_free_vq;
	}

	/* Create DPA QP */
	attr.tisn_or_qpn = virtq->dma_q->sw_qp.qp->devx_qp.devx.id;
	attr.vq_idx      = vqp->qp_idx;
	attr.sq_msix_vector = vqp->sq_vcq->veq->vector_idx;
	attr.rq_msix_vector = vqp->rq_vcq->veq->vector_idx;
	attr.qdb_idx      = vqp->qdb_idx;
	attr.tx_qsize     = q_attr->sq_size;
	attr.rx_qsize     = q_attr->rq_size;
	attr.tx_elem_size = q_attr->tx_elem_size;
	attr.rx_elem_size = q_attr->rx_elem_size;
	/*prepare mkey && pd */
	attr.emu_ib_ctx  = ctrl->emu_ctx;
	attr.emu_pd      = ctrl->pd;
	attr.emu_mkey    = ctrl->sctrl->xmkey->mkey;

	attr.sf_ib_ctx   = ctrl->emu_ctx;
	attr.sf_pd       = ctrl->pd;
	attr.sf_mkey     = ctrl->sctrl->xmkey->mkey;
	attr.sf_vhca_id  = ctrl->sf_vhca_id;
	attr.emu_vhca_id = ctrl->sctrl->sdev->pci->mpci.vhca_id;

	/*prepare host wr parameters*/
	attr.host_vq_ctx.rq_wqe_buff_pa = vqp->rq.comm.wqe_buff_pa;
	attr.host_vq_ctx.rq_pi_paddr    = vqp->rq.comm.doorbell_pa;
	attr.host_vq_ctx.rq_wqebb_cnt   = vqp->rq.comm.wqebb_cnt;
	attr.host_vq_ctx.rq_wqebb_size  = vqp->rq.comm.wqebb_size;
	attr.host_vq_ctx.sq_wqe_buff_pa = vqp->sq.comm.wqe_buff_pa;
	attr.host_vq_ctx.sq_pi_paddr    = vqp->sq.comm.doorbell_pa;
	attr.host_vq_ctx.sq_wqebb_cnt   = vqp->sq.comm.wqebb_cnt;
	attr.host_vq_ctx.sq_wqebb_size  = vqp->sq.comm.wqebb_size;
	attr.host_vq_ctx.emu_crossing_mkey = ctrl->sctrl->xmkey->mkey;
	attr.host_vq_ctx.sf_crossing_mkey  = ctrl->sctrl->xmkey->mkey;

	/*prepare arm wr parameters*/
	attr.arm_vq_ctx.rq_buff_addr = (uint64_t)vqp->rq.rq_buff;
	attr.arm_vq_ctx.sq_buff_addr = (uint64_t)vqp->sq.sq_buff;
	attr.arm_vq_ctx.rq_lkey      = vqp->qp_mr->lkey;
	attr.arm_vq_ctx.sq_lkey      = vqp->qp_mr->lkey;

	/*prepare msix parameters*/
	attr.num_msix = ctrl->sctrl->bar_curr->num_msix;

	dpa_vq = _vrdma_dpa_vq_create(ctrl, &attr);

	if (!dpa_vq) {
		log_error("Failed to create control snap dpa_vq, errno(%s)",
			  strerror(errno));
		goto err_vq_create;
	}
	virtq->dpa_vq = dpa_vq;

	/* Connect SW_QP to remote DPA qpn */
	rc = snap_dma_ep_connect_remote_qpn(virtq->dma_q, dpa_vq->dma_qp.qp_num);
	if (rc) {
		log_error("Failed to connect to remote qpn %d, err(%d)", dpa_vq->dma_qp.qp_num, rc);
		goto err_ep_connect;
	}

	/* Post recv buffers on SW_QP */
	rc = snap_dma_q_post_recv(virtq->dma_q);
	if (rc)
		goto err_post_recv;

	vrdma_dpa_vq_dump_attribute(&rdma_qp_create_attr, &attr, virtq);
	virtq->ctrl = ctrl->sctrl;
	virtq->idx = q_attr->vqpn; 
	/* Todo: later need confirm, because in snap_vrdma_vq_create: virtq->pd = q_attr->pd;*/
	virtq->pd = ctrl->pd;
	virtq->dma_mkey = ctrl->sctrl->xmkey->mkey;
	
	TAILQ_INSERT_TAIL(&ctrl->sctrl->virtqs, virtq, vq);

	return virtq;
err_post_recv:
err_ep_connect:
	_vrdma_dpa_vq_destroy(virtq->dpa_vq);
err_vq_create:
	snap_dma_ep_destroy(virtq->dma_q);
err_free_vq:
	free(virtq);
	return NULL;
}


static void generate_access_key(unsigned int seed, uint32_t *buf)
{
	int i;

	srand(seed);
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		buf[i] = (rand() % (UINT32_MAX - 1)) + 1;
}

static struct mlx5dv_devx_obj *
vrdma_create_alias_eq(struct ibv_context *emu_mgr_ibv_ctx,
			struct ibv_context *sf_ibv_ctx,
			uint16_t emu_mgr_vhca_id, uint32_t eqn,
			uint32_t *alias_eqn)
{
	uint32_t access_key[VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD] = {};
	struct vrdma_allow_other_vhca_access_attr attr;
	struct mlx5dv_devx_obj *alias_obj = NULL;
	struct vrdma_alias_attr alias_attr;
	int i, err;

	attr.type = MLX5_OBJ_TYPE_EMULATED_DEV_EQ;
	attr.obj_id = eqn;
	generate_access_key(eqn, access_key);
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		attr.access_key_be[i] = htobe32(access_key[i]);

	err = mlx_devx_allow_other_vhca_access(emu_mgr_ibv_ctx, &attr);
	if (err) {
		log_error("Failed to allow access to object, err(%d)", err);
		return 0;
	}

	alias_attr.orig_vhca_id = emu_mgr_vhca_id;
	alias_attr.type = MLX5_OBJ_TYPE_EMULATED_DEV_EQ;
	alias_attr.orig_obj_id = eqn;
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		alias_attr.access_key_be[i] = htobe32(access_key[i]);

	alias_obj = mlx_devx_create_alias_obj(sf_ibv_ctx, &alias_attr, alias_eqn);
	return alias_obj;
}

static int
vrdma_dpa_alias_cq_create(struct flexio_process *process,
			    struct vrdma_msix_init_attr *attr,
			    struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    uint32_t alias_eqn)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	err = vrdma_dpa_mm_cq_alloc(process, log2(attr->cq_size),
			  &emu_dev_ctx->msix[attr->msix_vector].alias_cq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_MSIX_EMULATED_CQ;
	cq_attr.emulated_eqn = alias_eqn;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.cq_dbr_daddr =
		emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr  =
		emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq_ring_daddr;

	err = flexio_cq_create(process, attr->sf_ib_ctx, &cq_attr,
			&emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq);
	if (err) {
		log_error("Failed to create alias_cq msix(%#x), err(%d)",
				attr->msix_vector, err);
		goto err_alias_cq_create;
	}

	emu_dev_ctx->msix[attr->msix_vector].cqn =
		flexio_cq_get_cq_num(emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq);
	return 0;

err_alias_cq_create:
	vrdma_dpa_mm_cq_free(process,
			&emu_dev_ctx->msix[attr->msix_vector].alias_cq);
	return err;
}

static void
vrdma_dpa_alias_cq_destroy(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			     uint16_t msix_vector)
{
	if (emu_dev_ctx->msix[msix_vector].cqn) {
		flexio_cq_destroy(emu_dev_ctx->msix[msix_vector].alias_cq.cq);
		vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process,
				&emu_dev_ctx->msix[msix_vector].alias_cq);
	}
}

int vrdma_dpa_msix_create(struct vrdma_dpa_vq *dpa_vq,
			    struct flexio_process *process,
			    struct vrdma_msix_init_attr *attr,
			    struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    int max_msix)
{
	uint32_t eqn = 0, alias_eqn = 0;
	int err;

	/* msix vector could be 0xFFFF for traffic vq when using
	 * dpdk based applications/driver. Bypass map creation in
	 * such case.
	 */
	if (attr->msix_vector == 0xFFFF) {
		log_notice("msix_vector %d",attr->msix_vector);
		return 0;
	}

	if (attr->msix_vector > max_msix) {
		log_error("Msix vector (%d) is out of range, max(%d)",
			  attr->msix_vector, max_msix);
		return -EINVAL;
	}

	/* If msix is already created, reuse it for this VQ as well */
	if (emu_dev_ctx->msix[attr->msix_vector].eqn &&
	    emu_dev_ctx->msix[attr->msix_vector].cqn) {
		log_notice("idx %d, %s, msix %#x, (reuse) eqn %#0x, cqn %#0x",
			  dpa_vq ? dpa_vq->idx : -1,
			  dpa_vq ? "qp" : "dev",
			  attr->msix_vector,
			  emu_dev_ctx->msix[attr->msix_vector].eqn,
			  emu_dev_ctx->msix[attr->msix_vector].cqn);
		atomic32_inc(&emu_dev_ctx->msix[attr->msix_vector].msix_refcount);
		return 0;
	}
	/* alias_cq->alias_eq->eq->msix_vector. */
	emu_dev_ctx->msix[attr->msix_vector].obj =
		mlx_devx_create_eq(attr->emu_ib_ctx, attr->emu_vhca_id,
				   attr->msix_vector, &eqn);
	if (!emu_dev_ctx->msix[attr->msix_vector].obj) {
		log_error("Failed to create devx eq, errno(%d)", errno);
		return -errno;
	}

	emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj =
		vrdma_create_alias_eq(attr->emu_ib_ctx, attr->sf_ib_ctx,
					emu_dev_ctx->dpa_ctx->emu_mgr_vhca_id,
					eqn, &alias_eqn);
	if (!emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj) {
		log_error("Failed to create devx alias eq, errno(%d)", errno);
		err = -errno;
		goto err_alias_eq_create;
	}

	emu_dev_ctx->msix[attr->msix_vector].eqn = alias_eqn;
	err = vrdma_dpa_alias_cq_create(process, attr, emu_dev_ctx, alias_eqn);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		goto err_alias_cq_create;
	}
	atomic32_inc(&emu_dev_ctx->msix[attr->msix_vector].msix_refcount);

	log_notice("idx %d, %s, msix %#x, devx_eqn %#x, alias_eqn %#x, alias_cqn %#x",
		  dpa_vq ? dpa_vq->idx : -1,
		  dpa_vq ? "qp" : "dev",
		  attr->msix_vector, eqn,
		  alias_eqn,
		  emu_dev_ctx->msix[attr->msix_vector].cqn);

	return 0;

err_alias_cq_create:
	mlx_devx_destroy_eq(emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj);
err_alias_eq_create:
	mlx_devx_destroy_eq(emu_dev_ctx->msix[attr->msix_vector].obj);
	return err;
}


void vrdma_dpa_msix_destroy(uint16_t msix_vector,
			      struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if ((msix_vector == 0xFFFF) ||
	    !atomic32_dec_and_test(&emu_dev_ctx->msix[msix_vector].msix_refcount))
		return;

	log_notice("Destroy msix %#x, alias_eqn %#x, alias_cqn %#x",
		  msix_vector,
		  emu_dev_ctx->msix[msix_vector].eqn,
		  emu_dev_ctx->msix[msix_vector].cqn);

	vrdma_dpa_alias_cq_destroy(emu_dev_ctx, msix_vector);
	mlx_devx_destroy_eq(emu_dev_ctx->msix[msix_vector].alias_eq_obj);
	mlx_devx_destroy_eq(emu_dev_ctx->msix[msix_vector].obj);
	memset(&emu_dev_ctx->msix[msix_vector], 0,
	       sizeof(struct vrdma_dpa_msix));
}

static uint32_t vrdma_dpa_emu_db_to_cq_ctx_get_id(struct snap_vrdma_queue *virtq)
{
	return virtq->dpa_vq->guest_db_to_cq_ctx.emu_db_to_cq_id;
}

struct vrdma_vq_ops vrdma_dpa_vq_ops = {
	.create = vrdma_dpa_vq_create,
	// .modify = vrdma_dpa_vq_modify,
	.destroy = vrdma_dpa_vq_destroy,
	.get_emu_db_to_cq_id = vrdma_dpa_emu_db_to_cq_ctx_get_id,
};

