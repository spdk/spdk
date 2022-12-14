/*
 *	 Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *	 Redistribution and use in source and binary forms, with or without
 *	 modification, are permitted provided that the following conditions
 *	 are met:
 *
 *	   * Redistributions of source code must retain the above copyright
 *		 notice, this list of conditions and the following disclaimer.
 *	   * Redistributions in binary form must reproduce the above copyright
 *		 notice, this list of conditions and the following disclaimer in
 *		 the documentation and/or other materials provided with the
 *		 distribution.
 *	   * Neither the name of Intel Corporation nor the names of its
 *		 contributors may be used to endorse or promote products derived
 *		 from this software without specific prior written permission.
 *
 *	 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *	 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *	 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *	 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *	 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *	 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *	 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *	 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *	 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *	 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *	 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/time.h>

#include "spdk/env.h"
#include "spdk/cpuset.h"
#include "spdk/thread.h"
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_srv.h"

#include "snap_dma.h"
#include "snap_vrdma_ctrl.h"

#define SPDK_IO_MGR_THREAD_NAME_PREFIX "VrdmaSnapThread"
#define SPDK_IO_MGR_THREAD_NAME_LEN 32

#define MLX5_ATOMIC_SIZE 8
#define WQE_DBG
//#define VCQ_ERR
//#define POLL_PI_DBG

struct mlx5_wqe_inline_seg {
	__be32		byte_count;
};

static const uint32_t vrdma_ib_opcode[] = {
	[IBV_WR_SEND]			= MLX5_OPCODE_SEND,
	[IBV_WR_SEND_WITH_INV]		= MLX5_OPCODE_SEND_INVAL,
	[IBV_WR_SEND_WITH_IMM]		= MLX5_OPCODE_SEND_IMM,
	[IBV_WR_RDMA_WRITE]		= MLX5_OPCODE_RDMA_WRITE,
	[IBV_WR_RDMA_WRITE_WITH_IMM]	= MLX5_OPCODE_RDMA_WRITE_IMM,
	[IBV_WR_RDMA_READ]		= MLX5_OPCODE_RDMA_READ,
	[IBV_WR_ATOMIC_CMP_AND_SWP]	= MLX5_OPCODE_ATOMIC_CS,
	[IBV_WR_ATOMIC_FETCH_AND_ADD]	= MLX5_OPCODE_ATOMIC_FA,
	[IBV_WR_BIND_MW]		= MLX5_OPCODE_UMR,
	[IBV_WR_LOCAL_INV]		= MLX5_OPCODE_UMR,
	[IBV_WR_TSO]			= MLX5_OPCODE_TSO,
	[IBV_WR_DRIVER1]		= MLX5_OPCODE_UMR,
};

static size_t g_num_spdk_threads;
static struct spdk_thread **g_spdk_threads;
static struct spdk_thread *app_thread;

size_t spdk_io_mgr_get_num_threads(void)
{
	return g_num_spdk_threads;
}

struct spdk_thread *spdk_io_mgr_get_thread(int id)
{
	if (id == -1)
		return app_thread;
	return g_spdk_threads[id];
}

static void spdk_thread_exit_wrapper(void *uarg)
{
	(void)spdk_thread_exit((struct spdk_thread *)uarg);
}

int spdk_io_mgr_init(void)
{
	struct spdk_cpuset *cpumask;
	uint32_t i;
	int  j;
	char thread_name[SPDK_IO_MGR_THREAD_NAME_LEN];

	app_thread = spdk_get_thread();

	g_num_spdk_threads = spdk_env_get_core_count();
	g_spdk_threads = calloc(g_num_spdk_threads, sizeof(*g_spdk_threads));
	if (!g_spdk_threads) {
		SPDK_ERRLOG("Failed to allocate IO threads");
		goto err;
	}

	cpumask = spdk_cpuset_alloc();
	if (!cpumask) {
		SPDK_ERRLOG("Failed to allocate SPDK CPU mask");
		goto free_threads;
	}

	j = 0;
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_zero(cpumask);
		spdk_cpuset_set_cpu(cpumask, i, true);
		snprintf(thread_name, SPDK_IO_MGR_THREAD_NAME_LEN, "%s%d",
				 SPDK_IO_MGR_THREAD_NAME_PREFIX, j);
		g_spdk_threads[j] = spdk_thread_create(thread_name, cpumask);
		if (!g_spdk_threads[j]) {
			SPDK_ERRLOG("Failed to create thread %s", thread_name);
			spdk_cpuset_free(cpumask);
			goto exit_threads;
		}

		j++;
	}
	spdk_cpuset_free(cpumask);

	return 0;

exit_threads:
	for (j--; j >= 0; j--)
		spdk_thread_send_msg(g_spdk_threads[j], spdk_thread_exit_wrapper,
							 g_spdk_threads[j]);
free_threads:
	free(g_spdk_threads);
	g_spdk_threads = NULL;
	g_num_spdk_threads = 0;
err:
	return -1;
}

void spdk_io_mgr_clear(void)
{
	uint32_t i;

	for (i = 0; i < g_num_spdk_threads; i++)
		spdk_thread_send_msg(g_spdk_threads[i], spdk_thread_exit_wrapper,
							 g_spdk_threads[i]);
	free(g_spdk_threads);
	g_spdk_threads = NULL;
	g_num_spdk_threads = 0;
}

//need invoker to guarantee pi is bigger than pre_pi
static inline int vrdma_vq_rollback(uint16_t pre_pi, uint16_t pi,
								   uint16_t q_size)
{
	if (pi % q_size == 0) {
		return 0;
	}
	return !(pi % q_size > pre_pi % q_size);
}

static inline unsigned long DIV_ROUND_UP(unsigned long n, unsigned long d)
{
	return ((n) + (d) - 1) / (d);
}

static bool vrdma_qp_sm_idle(struct spdk_vrdma_qp *vqp,
							enum vrdma_qp_sm_op_status status)
{
	SPDK_ERRLOG("vrdma sq in invalid state %d\n",
					   VRDMA_QP_STATE_IDLE);
	return false;
}

static bool vrdma_qp_sm_poll_pi(struct spdk_vrdma_qp *vqp,
								   enum vrdma_qp_sm_op_status status)
{
	int ret;
	uint64_t pi_addr = vqp->sq.comm.doorbell_pa;

	if (status != VRDMA_QP_SM_OP_OK) {
		SPDK_ERRLOG("failed in previous step, status %d\n", status);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	if (vqp->snap_queue->swq_state == SW_VIRTQ_FLUSHING) {
		SPDK_NOTICELOG("vqp is in flushing status, stop poll pi\n");
		return false;
	}

#ifdef POLL_PI_DBG
	SPDK_NOTICELOG("vrdam poll sq pi: loop %d, pi pa 0x%lx, pi %d, pre pi %d\n",
					pi_addr, vqp->qp_pi->pi.sq_pi, vqp->sq.comm.pre_pi);
#endif
	vqp->sm_state = VRDMA_QP_STATE_HANDLE_PI;
	vqp->q_comp.func = vrdma_qp_sm_dma_cb;
	vqp->q_comp.count = 1;

	ret = snap_dma_q_read(vqp->snap_queue->dma_q, &vqp->qp_pi->pi.sq_pi, sizeof(uint16_t),
						vqp->qp_mr->lkey, pi_addr,
						vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read sq PI, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	return false;
}

static bool vrdma_qp_sm_handle_pi(struct spdk_vrdma_qp *vqp,
									enum vrdma_qp_sm_op_status status)
{
	if (status != VRDMA_QP_SM_OP_OK) {
		SPDK_ERRLOG("failed to get vq PI, status %d\n", status);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	if (vqp->qp_pi->pi.sq_pi != vqp->sq.comm.pre_pi) {
		vqp->sm_state = VRDMA_QP_STATE_WQE_READ;
	} else {
		vqp->sm_state = VRDMA_QP_STATE_POLL_CQ_CI;
	}
	return true;
}
#define MAX_POLL_WQE_NUM 32
static bool vrdma_qp_wqe_sm_read(struct spdk_vrdma_qp *vqp,
									enum vrdma_qp_sm_op_status status)
{
	uint16_t pi = vqp->qp_pi->pi.sq_pi;
	uint16_t pre_pi = vqp->sq.comm.pre_pi;
	uint32_t sq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = vqp->sq.comm.wqebb_cnt;
	int ret;

	SPDK_NOTICELOG("vrdam poll sq wqe: sq pa 0x%lx\n", vqp->sq.comm.wqe_buff_pa);

	vqp->sm_state = VRDMA_QP_STATE_WQE_PARSE;
	//vqp->sq.comm.num_to_parse = spdk_min((pi - pre_pi), MAX_POLL_WQE_NUM);
	vqp->sq.comm.num_to_parse = pi - pre_pi;

	//fetch the delta PI number entry in one time
	if (!vrdma_vq_rollback(pre_pi, pi, q_size)) {
		vqp->q_comp.count = 1;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = vqp->sq.comm.num_to_parse;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
		local_ring_addr = (uint8_t *)vqp->sq.sq_buff + offset;
		host_ring_addr = vqp->sq.comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, local_ring_addr, sq_poll_size,
							vqp->qp_mr->lkey, host_ring_addr,
							vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read sq WQE entry, ret %d\n", ret);
			vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			 return true;
		}
	} else {
		/* vq roll back case, first part */
		vqp->q_comp.count = 1;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = q_size - (pre_pi % q_size);
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
		local_ring_addr = (uint8_t *)vqp->sq.sq_buff + offset;
		host_ring_addr = vqp->sq.comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, local_ring_addr, sq_poll_size,
							vqp->qp_mr->lkey, host_ring_addr,
							vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read sq WQE entry, ret %d\n", ret);
			vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			return true;
		}

		/* calculate second poll size */
		vqp->q_comp.count++;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = pi % q_size;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		local_ring_addr = (uint8_t *)vqp->sq.sq_buff;
		host_ring_addr = vqp->sq.comm.wqe_buff_pa;
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, local_ring_addr, sq_poll_size,
							  vqp->qp_mr->lkey, vqp->sq.comm.wqe_buff_pa,
							  vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second read sq WQE entry, ret %d\n", ret);
				vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			return true;
		}
	}

	return false;
}

static bool vrdma_qp_wqe_sm_parse(struct spdk_vrdma_qp *vqp,
								   enum vrdma_qp_sm_op_status status)
{
	if (status != VRDMA_QP_SM_OP_OK) {
		SPDK_ERRLOG("failed to read vq wqe, status %d\n", status);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	vqp->stats.sq_wqe_fetched += vqp->sq.comm.num_to_parse;
	SPDK_NOTICELOG("vrdam parse sq wqe: vq pi %d, pre_pi %d\n",
		vqp->qp_pi->pi.sq_pi, vqp->sq.comm.pre_pi);
	vqp->sm_state = VRDMA_QP_STATE_WQE_MAP_BACKEND;

	/* TODO: parse wqe handling */
	return true;
}

static inline struct vrdma_backend_qp *vrdma_vq_get_mqp(struct spdk_vrdma_qp *vqp)
{
	/* TODO: currently, only one-to-one map */
	return vqp->bk_qp;
}

static bool vrdma_qp_wqe_sm_map_backend(struct spdk_vrdma_qp *vqp,
											enum vrdma_qp_sm_op_status status)
{
	vqp->bk_qp = vrdma_vq_get_mqp(vqp);

/* todo for error vcqe handling */

	if (!vqp->bk_qp) {
#ifdef VCQ_ERR
		vqp->sm_state = VRDMA_QP_STATE_POLL_CQ_CI;
		vqp->flags |= VRDMA_SEND_ERR_CQE;
#else
		vqp->sm_state = VRDMA_QP_STATE_POLL_PI;
#endif
		return true;
	}

	SPDK_NOTICELOG("vrdam map sq wqe: vq pi %d, mqp %p\n",
			vqp->qp_pi->pi.sq_pi, vqp->bk_qp);
	vqp->sm_state = VRDMA_QP_STATE_WQE_SUBMIT;
	return true;
}

static inline uint8_t vrdma_get_send_flags(struct vrdma_send_wqe *wqe)
{
	uint8_t fm_ce_se = 0;
	
	if (wqe->meta.send_flags & IBV_SEND_SIGNALED)
		fm_ce_se |= MLX5_WQE_CTRL_CQ_UPDATE;
	if (wqe->meta.send_flags & IBV_SEND_FENCE)
		fm_ce_se |= MLX5_WQE_CTRL_FENCE;
	if (wqe->meta.send_flags & IBV_SEND_SOLICITED)
		fm_ce_se |= MLX5_WQE_CTRL_SOLICITED;

	return fm_ce_se;
}

static inline void vrdma_set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
										uint64_t remote_addr, uint32_t rkey)
{
	rseg->raddr    = htobe64(remote_addr);
	rseg->rkey	   = htobe32(rkey);
	rseg->reserved = 0;
}

static inline void vrdma_set_atomic_seg(struct mlx5_wqe_atomic_seg *aseg,
										uint8_t opcode, uint64_t swap,
										uint64_t compare_add)
{
	if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
		aseg->swap_add = htobe64(swap);
		aseg->compare = htobe64(compare_add);
	} else {
		aseg->swap_add = htobe64(compare_add);
	}
}

static inline void *vrdma_get_wqe_bb(struct snap_vrdma_backend_qp *bk_qp)
{
	return (void *)bk_qp->hw_qp.sq.addr + (bk_qp->hw_qp.sq.pi & (bk_qp->hw_qp.sq.wqe_cnt - 1)) *
		   MLX5_SEND_WQE_BB;
}

static inline void vrdma_update_tx_db(struct snap_vrdma_backend_qp *bk_qp)
{
	/*
	 * Use cpu barrier to prevent code reordering
	 */
	snap_memory_cpu_store_fence();

	((uint32_t *)bk_qp->hw_qp.dbr_addr)[MLX5_SND_DBR] = htobe32(bk_qp->hw_qp.sq.pi);
}

static inline void vrdma_flush_tx_db(struct snap_vrdma_backend_qp *bk_qp,
									struct mlx5_wqe_ctrl_seg *ctrl)
{
	*(uint64_t *)(bk_qp->hw_qp.sq.bf_addr) = *(uint64_t *)ctrl;
	++bk_qp->stat.tx.total_dbs;
}


static inline void vrdma_ring_tx_db(struct snap_vrdma_backend_qp *bk_qp,
									struct mlx5_wqe_ctrl_seg *ctrl)
{
	/* 8.9.3.1	Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 *	  WQE (on WQEBB granularity)
	 *
	 * 2. Update Doorbell Record associated with that queue by writing
	 *	  the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 **/
	vrdma_update_tx_db(bk_qp);

	/* Make sure that doorbell record is written before ringing the doorbell
	 **/
	snap_memory_bus_store_fence();

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *	  Register field in the UAR associated with that queue
	 */
	vrdma_flush_tx_db(bk_qp, ctrl);

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	if (!bk_qp->hw_qp.sq.tx_db_nc)
		snap_memory_bus_store_fence();
#endif

}

static inline void
vrdma_set_ctrl_seg(struct mlx5_wqe_ctrl_seg *ctrl, uint16_t pi,
				 uint8_t opcode, uint8_t opmod, uint32_t qp_num,
				 uint8_t fm_ce_se, uint8_t ds,
				 uint8_t signature, uint32_t imm)
{
	*(uint32_t *)((void *)ctrl + 8) = 0;
	mlx5dv_set_ctrl_seg(ctrl, pi, opcode, opmod, qp_num,
						fm_ce_se, ds, signature, imm);
}

static inline void vrdma_wqe_submit(struct snap_vrdma_backend_qp *bk_qp,
									struct mlx5_wqe_ctrl_seg *ctrl)
{
	uint8_t ds = be32toh(ctrl->qpn_ds) & 0xFF;
	
	bk_qp->hw_qp.sq.pi += DIV_ROUND_UP(ds * 16, MLX5_SEND_WQE_BB);
	if (bk_qp->db_flag == SNAP_DB_RING_BATCH) {
		bk_qp->tx_need_ring_db = true;
		bk_qp->ctrl = ctrl;
		return;
	}
	vrdma_ring_tx_db(bk_qp, ctrl);
}

static inline void vrdma_tx_complete(struct snap_vrdma_backend_qp *bk_qp)
{
	if (bk_qp->tx_need_ring_db) {
		bk_qp->tx_need_ring_db = false;
		vrdma_ring_tx_db(bk_qp, bk_qp->ctrl);
	}
}

static void *vrdma_get_send_wqe(struct snap_vrdma_backend_qp *qp, int n)
{
	return qp->hw_qp.sq.addr + (n << MLX5_SEND_WQE_SHIFT);
}

static void vrdma_dump_wqe(int idx, int size_16,
								struct snap_vrdma_backend_qp *qp)
{
	uint32_t *p = NULL;
	int i, j;
	int tidx = idx;

	printf("dump wqe at %p, len %d, wqe_id %d\n",
			vrdma_get_send_wqe(qp, tidx), size_16, idx);
	for (i = 0, j = 0; i < size_16 * 4; i += 4, j += 4) {
		if ((i & 0xf) == 0) {
			void *buf = vrdma_get_send_wqe(qp, tidx);
			tidx = (tidx + 1) & (qp->hw_qp.sq.wqe_cnt - 1);
			p = (uint32_t *)buf;
			j = 0;
		}
		printf("%08x %08x %08x %08x\n", be32toh(p[j]), be32toh(p[j + 1]),
			be32toh(p[j + 2]), be32toh(p[j + 3]));
	}
}

static void vrdma_dump_tencent_wqe(struct vrdma_send_wqe *wqe)
{
	uint16_t i;
	
	printf("\ndump tencent wqe start\n");

	printf("meta.opcode %x \n", wqe->meta.opcode);
	printf("meta.imm_data %x \n", wqe->meta.imm_data);
	printf("meta.invalid_key %x \n", wqe->meta.invalid_key);
	printf("meta.length %x \n", wqe->meta.length);
	printf("meta.req_id %x \n", wqe->meta.req_id);
	printf("meta.send_flags %x \n", wqe->meta.send_flags);
	printf("meta.sge_num %x \n", wqe->meta.sge_num);

	switch (wqe->meta.opcode) {
		case IBV_WR_RDMA_READ:
		case IBV_WR_RDMA_WRITE:
		case IBV_WR_RDMA_WRITE_WITH_IMM:
			printf("rdma_rw.req_id 0x%lx \n", wqe->rdma_rw.remote_addr);
			printf("rdma_rw.rkey 0x%lx \n", wqe->rdma_rw.rkey);
			if (wqe->meta.sge_num) {
				for (i = 0; i < wqe->meta.sge_num; i++) {
					printf("sge[%d].buf_addr_hi 0x%x \n", i, wqe->sgl[i].buf_addr_hi);
					printf("sge[%d].buf_addr_lo 0x%x \n", i, wqe->sgl[i].buf_addr_lo);
					printf("sge[%d].buf_length 0x%x \n", i, wqe->sgl[i].buf_length);
					printf("sge[%d].lkey 0x%x \n", i, wqe->sgl[i].lkey);
				}
			}
			break;
		case IBV_WR_ATOMIC_CMP_AND_SWP:
		case IBV_WR_ATOMIC_FETCH_AND_ADD:
			printf("rdma_atomic.compare_add 0x%lx \n", wqe->rdma_atomic.compare_add);
			printf("rdma_atomic.remote_addr 0x%lx \n", wqe->rdma_atomic.remote_addr);
			printf("rdma_atomic.swap 0x%lx \n", wqe->rdma_atomic.swap);
			printf("rdma_atomic.rkey 0x%x \n", wqe->rdma_atomic.rkey);
			break;
		default:
			printf(" tencent wqe unsupported type %x\n", wqe->meta.opcode);
			break;
	}
	
	printf(" tencent wqe dump done\n");
}

static inline unsigned long align(unsigned long val, unsigned long align)
{
	return (val + align - 1) & ~(align - 1);
}

static inline uint16_t vrdma_set_inl_data_seg(struct vrdma_send_wqe *wqe, void *seg)
{
	uint16_t len = wqe->meta.length;
	struct mlx5_wqe_inline_seg *dseg = seg;

	if (len > 64) {
		len = 64;
		SPDK_ERRLOG("wqe inline length %d exceeds length of data array 64 Bytes\n", len);
	}
	
	memcpy((void *)((uint8_t *)dseg + sizeof(*dseg)), wqe->inline_data, len);
	dseg->byte_count = htobe32(len | MLX5_INLINE_SEG);
	return (align(len + sizeof dseg->byte_count, 16) / 16);
}

static inline uint16_t vrdma_set_data_seg(struct vrdma_send_wqe *wqe, void *seg,
												uint8_t inl)
{
	uint16_t ds = 0;
	uint16_t sge_num;
	uint16_t i;
	struct mlx5_wqe_data_seg *dseg;
	struct vrdma_buf_desc sge;
	uint64_t sge_addr;
	
	if (inl) {
		ds = vrdma_set_inl_data_seg(wqe, seg);
	} else {
		dseg = seg;
		sge_num = wqe->meta.sge_num;
		for (i = 0; i < sge_num; i++) {
			sge = wqe->sgl[i];
			if (spdk_likely(sge.buf_length)) {
				sge_addr = ((uint64_t)sge.buf_addr_hi << 32) + sge.buf_addr_lo;
				mlx5dv_set_data_seg(dseg, sge.buf_length, sge.lkey, (intptr_t)sge_addr);
				++dseg;
				ds += sizeof(*dseg) / 16;
			}
		}
	}
	return ds;
}

static int vrdma_rw_wqe_submit(struct vrdma_send_wqe *wqe,
								struct snap_vrdma_backend_qp *bk_qp,
								uint8_t opcode)
{
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	void *seg;
	uint8_t fm_ce_se = 0;
	uint8_t ds = 0;
	uint8_t sig = 0;
	uint32_t imm = 0;
	uint8_t inl;
#ifdef WQE_DBG
	uint32_t idx;
#endif

	fm_ce_se = vrdma_get_send_flags(wqe);
	inl = !!(wqe->meta.send_flags & IBV_SEND_INLINE);
	ctrl = seg = (struct mlx5_wqe_ctrl_seg *)vrdma_get_wqe_bb(bk_qp);
	seg += sizeof(*ctrl);
	ds += sizeof(*ctrl) / 16;

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	vrdma_set_raddr_seg(rseg, (uintptr_t)wqe->rdma_rw.remote_addr, wqe->rdma_rw.rkey);

	seg  += sizeof(*rseg);
	ds += sizeof(*rseg) / 16;
	/* prepare data segement */
	ds += vrdma_set_data_seg(wqe, seg, inl);

	vrdma_set_ctrl_seg(ctrl, bk_qp->hw_qp.sq.pi, opcode, 0, bk_qp->hw_qp.qp_num,
					fm_ce_se, ds, sig, imm);
#ifdef WQE_DBG
	idx = bk_qp->hw_qp.sq.pi & (bk_qp->hw_qp.sq.wqe_cnt - 1);
	vrdma_dump_wqe(idx, ds, bk_qp);
#endif
	vrdma_wqe_submit(bk_qp, ctrl);
	return 0;
	
}

static int vrdma_atomic_wqe_submit(struct vrdma_send_wqe *wqe,
											struct snap_vrdma_backend_qp *bk_qp,
											uint8_t opcode)
{
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_atomic_seg *aseg;
	void *seg;
	uint8_t fm_ce_se = 0;
	uint8_t ds = 0;
	uint8_t sig = 0;
	uint32_t imm = 0;
	uint8_t inl;

	fm_ce_se = vrdma_get_send_flags(wqe);
	inl = !!(wqe->meta.send_flags & IBV_SEND_INLINE);
	ctrl = seg = (struct mlx5_wqe_ctrl_seg *)vrdma_get_wqe_bb(bk_qp);

	seg += sizeof(*ctrl); 
	ds += sizeof(*ctrl) / 16;

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	vrdma_set_raddr_seg(rseg, (uintptr_t)wqe->rdma_atomic.remote_addr,
						wqe->rdma_atomic.rkey);
	seg += sizeof(*rseg);

	aseg = seg;
	vrdma_set_atomic_seg(aseg, opcode, wqe->rdma_atomic.swap,
						wqe->rdma_atomic.compare_add);
	seg += sizeof(*aseg); 
	ds += (sizeof(*rseg) + sizeof(*aseg)) / 16;
	
	/* prepare data segement */
	ds += vrdma_set_data_seg(wqe, seg, inl);

	vrdma_set_ctrl_seg(ctrl, bk_qp->hw_qp.sq.pi, opcode, 0, bk_qp->hw_qp.qp_num,
					fm_ce_se, ds, sig, imm);
	vrdma_wqe_submit(bk_qp, ctrl);
	return 0;

}

static int vrdma_ud_wqe_submit(struct vrdma_send_wqe *wqe,
										struct snap_vrdma_backend_qp *bk_qp, 
										uint8_t opcode)
{
	//TODO:
	return 0;
}

//translate and submit vqp wqe to mqp
static bool vrdma_qp_wqe_sm_submit(struct spdk_vrdma_qp *vqp,
											enum vrdma_qp_sm_op_status status)
{
	uint16_t num_to_parse = vqp->sq.comm.num_to_parse;
	struct snap_vrdma_backend_qp *backend_qp = &vqp->bk_qp->bk_qp;
	uint16_t i;
	struct vrdma_send_wqe *wqe;
	uint8_t opcode = 0;
	uint16_t q_size = vqp->sq.comm.wqebb_cnt;

	SPDK_NOTICELOG("vrdam submit sq wqe: pi %d, pre_pi %d, num_to_submit %d\n",
					vqp->qp_pi->pi.sq_pi, vqp->sq.comm.pre_pi, num_to_parse);
	vqp->sm_state = VRDMA_QP_STATE_POLL_CQ_CI;

	for (i = 0; i < num_to_parse; i++) {
		wqe = vqp->sq.sq_buff + ((vqp->sq.comm.pre_pi + i) % q_size);
		opcode = vrdma_ib_opcode[wqe->meta.opcode];
		//SPDK_NOTICELOG("vrdam sq submit wqe start, m_qpn %d, opcode 0x%x\n",
			//			backend_qp->hw_qp.qp_num, opcode);
#ifdef WQE_DBG
		vrdma_dump_tencent_wqe(wqe);
#endif
		switch (opcode) {
			case MLX5_OPCODE_RDMA_READ:
			case MLX5_OPCODE_RDMA_WRITE:
			case MLX5_OPCODE_RDMA_WRITE_IMM:
				vrdma_rw_wqe_submit(wqe, backend_qp, opcode);
				vqp->stats.sq_wqe_wr++;
				break;
			case MLX5_OPCODE_ATOMIC_CS:
			case MLX5_OPCODE_ATOMIC_FA:
				vrdma_atomic_wqe_submit(wqe, backend_qp, opcode);
				vqp->stats.sq_wqe_atomic++;
				break;
			default:
				// place holder, will be replaced in future
				vrdma_ud_wqe_submit(wqe, backend_qp, opcode);
				vqp->stats.sq_wqe_ud++;
				vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
				return false;
		}
	}
	vrdma_tx_complete(backend_qp);
	vqp->stats.msq_dbred_pi = backend_qp->hw_qp.sq.pi;
	vqp->stats.sq_wqe_submitted += num_to_parse;
	vqp->sq.comm.pre_pi += num_to_parse;
	SPDK_NOTICELOG("vrdam sq submit wqe done \n");
	return true;
}

static const char *vrdma_mcqe_err_opcode(struct mlx5_err_cqe *ecqe)
{
	uint8_t wqe_err_opcode = be32toh(ecqe->s_wqe_opcode_qpn) >> 24;

	switch (ecqe->op_own >> 4) {
	case MLX5_CQE_REQ_ERR:
		switch (wqe_err_opcode) {
		case MLX5_OPCODE_RDMA_WRITE_IMM:
		case MLX5_OPCODE_RDMA_WRITE:
			return "RDMA_WRITE";
		case MLX5_OPCODE_SEND_IMM:
		case MLX5_OPCODE_SEND:
		case MLX5_OPCODE_SEND_INVAL:
			return "SEND";
		case MLX5_OPCODE_RDMA_READ:
			return "RDMA_READ";
		case MLX5_OPCODE_ATOMIC_CS:
			return "COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_FA:
			return "FETCH_ADD";
		case MLX5_OPCODE_ATOMIC_MASKED_CS:
			return "MASKED_COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_MASKED_FA:
			return "MASKED_FETCH_ADD";
		default:
			return "";
			}
	case MLX5_CQE_RESP_ERR:
		return "RECV";
	default:
		return "";
	}
}

static void vrdma_mcqe_err(struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;
	uint16_t wqe_counter;
	uint32_t qp_num = 0;
	char info[200] = {0};

	wqe_counter = be16toh(ecqe->wqe_counter);
	qp_num = be32toh(ecqe->s_wqe_opcode_qpn) & ((1<<24)-1);

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR) {
		SPDK_ERRLOG("QP 0x%x wqe[%d] is flushed\n", qp_num, wqe_counter);
		return;
	}

	switch (ecqe->syndrome) {
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		snprintf(info, sizeof(info), "Local length");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		snprintf(info, sizeof(info), "Local QP operation");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		snprintf(info, sizeof(info), "Local protection");
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		snprintf(info, sizeof(info), "WR flushed because QP in error state");
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		snprintf(info, sizeof(info), "Memory window bind");
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		snprintf(info, sizeof(info), "Bad response");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		snprintf(info, sizeof(info), "Local access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		snprintf(info, sizeof(info), "Invalid request");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		snprintf(info, sizeof(info), "Remote access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		snprintf(info, sizeof(info), "Remote QP");
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Transport retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Receive-no-ready retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		snprintf(info, sizeof(info), "Remote side aborted");
		break;
	default:
		snprintf(info, sizeof(info), "Generic");
		break;
	}
	SPDK_ERRLOG("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x) opcode %s\n",
		   qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd,
		   vrdma_mcqe_err_opcode(ecqe));
}

static inline struct mlx5_cqe64 *vrdma_get_mqp_cqe(struct snap_hw_cq *dv_cq,
													int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	/* note: that the cq_size is known at the compilation time. We pass it
	 * down here so that branch and multiplication will be done at the
	 * compile time during inlining
	 **/
	cqe = (struct mlx5_cqe64 *)(dv_cq->cq_addr + (dv_cq->ci & (dv_cq->cqe_cnt - 1)) *
								cqe_size);
	return cqe_size == 64 ? cqe : cqe + 1;
}

static inline struct mlx5_cqe64 *vrdma_poll_mqp_scq(struct snap_hw_cq *dv_cq,
															int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	cqe = vrdma_get_mqp_cqe(dv_cq, cqe_size);

	/* cqe is hw owned */
	if (mlx5dv_get_cqe_owner(cqe) == !(dv_cq->ci & dv_cq->cqe_cnt)) {
		return NULL;
	}

	/* and must have valid opcode */
	if (mlx5dv_get_cqe_opcode(cqe) == MLX5_CQE_INVALID) {
		return NULL;
	}
		
	dv_cq->ci++;
#ifdef POLL_PI_DBG
	SPDK_NOTICELOG("cq: 0x%x ci: %d CQ opcode %d size %d wqe_counter %d,"
					"scatter32 %d scatter64 %d\n",
		   			dv_cq->cq_num, dv_cq->ci,
		   			mlx5dv_get_cqe_opcode(cqe),
		   			be32toh(cqe->byte_cnt),
		   			be16toh(cqe->wqe_counter),
		   			cqe->op_own & MLX5_INLINE_SCATTER_32,
		   			cqe->op_own & MLX5_INLINE_SCATTER_64);
#endif
	return cqe;
}

static inline uint32_t vrdma_get_wqe_id(struct spdk_vrdma_qp *vqp, uint32_t mwqe_idx)
{
	return mwqe_idx;
}

static bool vrdma_qp_sm_poll_cq_ci(struct spdk_vrdma_qp *vqp,
									enum vrdma_qp_sm_op_status status)
{
	int ret;
	uint64_t ci_addr = vqp->sq_vcq->ci_pa;

	if (status != VRDMA_QP_SM_OP_OK) {
		SPDK_ERRLOG("failed to update admq CI, status %d\n", status);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

#ifdef POLL_PI_DBG
	SPDK_NOTICELOG("vrdam poll sq vcq ci: doorbell pa 0x%lx\n", ci_addr);
#endif

	vqp->sm_state = VRDMA_QP_STATE_GEN_COMP;
	vqp->q_comp.func = vrdma_qp_sm_dma_cb;
	vqp->q_comp.count = 1;

	ret = snap_dma_q_read(vqp->snap_queue->dma_q, &vqp->sq_vcq->pici->ci, sizeof(uint32_t),
					  vqp->sq_vcq->cqe_ci_mr->lkey, ci_addr,
					  vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read sq vcq CI, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	return false;
}

static void vrdma_ring_mcq_db(struct snap_hw_cq *mcq)
{
	uint32_t *dbrec = (uint32_t *)mcq->dbr_addr;
	uint64_t sn_ci_cmd, doorbell;
	uint32_t sn, ci;

	sn = mcq->cq_sn & 3;
	ci = mcq->ci & 0xffffff;
	sn_ci_cmd = (sn << 28) | ci;

	dbrec[SNAP_MLX5_CQ_SET_CI] = htobe32(mcq->ci & 0xffffff);
	snap_memory_cpu_fence();

	doorbell = (sn_ci_cmd << 32) | mcq->cq_num;
	*(uint64_t *)((uint8_t *)mcq->uar_addr + MLX5_CQ_DOORBELL) = htobe64(doorbell);
	snap_memory_bus_store_fence();
	mcq->cq_sn++;

	SPDK_NOTICELOG("test update mcq ci %d\n", mcq->ci);
}

static int vrdma_write_back_sq_cqe(struct spdk_vrdma_qp *vqp)
{
	struct spdk_vrdma_cq *vcq = vqp->sq_vcq;
	uint16_t pi = vcq->pi;
	uint16_t pre_pi = vcq->pre_pi;
	uint32_t write_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = vcq->cqe_entry_num;
	int ret;

	SPDK_NOTICELOG("vrdam write back cqe start: vcq pi %d, pre_pi %d, ci %d\n",
					vcq->pi, vcq->pre_pi, vcq->pici->ci);
	//fetch the delta PI number entry in one time
	if (!vrdma_vq_rollback(pre_pi, pi, q_size)) {
		vqp->q_comp.count = 1;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = pi - pre_pi;
		write_size = num * vcq->cqebb_size;
		
		offset = (pre_pi % q_size) * vcq->cqebb_size;
		host_ring_addr = vcq->host_pa + offset;
		local_ring_addr = (uint8_t *)((uint8_t *)vcq->cqe_buff + offset);
		SPDK_NOTICELOG("write cqe: offset %d host base addr 0x%lx host ring addr 0x%lx"
						"local base 0x%p local ring 0x%p\n",
						offset, vcq->host_pa, host_ring_addr,
						vcq->cqe_buff, local_ring_addr);
		ret = snap_dma_q_write(vqp->snap_queue->dma_q, local_ring_addr, write_size,
							vcq->cqe_ci_mr->lkey, host_ring_addr,
							vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to write back sq cqe, ret %d\n", ret);
			vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			 return -1;
		}
	} else {
		/* vq roll back case, first part */
		vqp->q_comp.count = 1;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = q_size - (pre_pi % q_size);
		write_size = num * vcq->cqebb_size;
		
		offset = (pre_pi % q_size) * vcq->cqebb_size;
		host_ring_addr = vcq->host_pa + offset;
		local_ring_addr = (uint8_t *)((uint8_t *)vcq->cqe_buff + offset);
		SPDK_NOTICELOG("write cqe first: offset %d host base addr 0x%lx host ring addr 0x%lx"
						"local base 0x%p local ring 0x%p\n",
						offset, vcq->host_pa, host_ring_addr,
						vcq->cqe_buff, local_ring_addr);
		ret = snap_dma_q_write(vqp->snap_queue->dma_q, local_ring_addr, write_size,
							vcq->cqe_ci_mr->lkey, host_ring_addr,
							vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to write back sq cqe, ret %d\n", ret);
			vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			return -1;
		}

		/* calculate second write size */
		vqp->q_comp.count++;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = pi % q_size;
		write_size = num * vcq->cqebb_size;
		local_ring_addr = (uint8_t *)vcq->cqe_buff;
		host_ring_addr = vcq->host_pa;

		SPDK_NOTICELOG("write cqe second: num %d host base addr 0x%lx host ring addr 0x%lx"
						"local base 0x%p local ring 0x%p\n",
						num, vcq->host_pa, host_ring_addr,
						vcq->cqe_buff, local_ring_addr);
		ret = snap_dma_q_write(vqp->snap_queue->dma_q, local_ring_addr, write_size,
							  vcq->cqe_ci_mr->lkey, host_ring_addr,
							  vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second write back sq cqe, ret %d\n", ret);
				vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
			return -1;
		}
	}

	return 0;
}

static bool vrdma_vqp_send_err_cqe(struct spdk_vrdma_qp *vqp)
{
	struct spdk_vrdma_cq *vcq = vqp->sq_vcq;
	struct vrdma_cqe *vcqe;
	uint32_t wqe_idx;
	uint32_t cqe_idx;
	int ret;
	uint32_t i;

	for (i = 0; i < vqp->sq.comm.num_to_parse; i++) {
		if (vcq->pi - vcq->pici->ci == vcq->cqe_entry_num) {
			SPDK_ERRLOG("send err cqe, cq full: vcq new pi %d, pre_pi %d, ci %d\n",
					vcq->pi, vcq->pre_pi, vcq->pici->ci);
			goto write_err_cqe;
		}
		wqe_idx  = vqp->sq.comm.pre_pi + i;
		cqe_idx = vcq->pi & (vcq->cqe_entry_num - 1);
		vcqe = (struct vrdma_cqe *)vqp->sq_vcq->cqe_buff + cqe_idx;
		vcqe->imm_data = 0;
		vcqe->length = 0;
		vcqe->req_id = wqe_idx;
		vcqe->local_qpn = vqp->qp_idx;
		//vcqe->ts = (uint32_t)cqe->timestamp;
		vcqe->ts = 0;
		vcqe->opcode = IBV_WC_RETRY_EXC_ERR;
		vcqe->owner = !!((vcq->pi++) & (vcq->cqe_entry_num));
	}
write_err_cqe:
	ret = vrdma_write_back_sq_cqe(vqp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to write cq CQE entry, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam send err scqe done: vcq new pi %d, pre_pi %d\n",
					vcq->pi, vcq->pre_pi);
	vcq->pre_pi = vcq->pi;
	return false;
}

#define POLL_CQ_NUM 1024
static bool vrdma_qp_sm_gen_completion(struct spdk_vrdma_qp *vqp,
									   enum vrdma_qp_sm_op_status status)
{
	struct snap_hw_cq *mcq;
	struct spdk_vrdma_cq *vcq;
	struct mlx5_cqe64 *cqe;
	struct vrdma_cqe *vcqe;
	uint32_t wqe_idx;
	uint32_t cqe_idx;
	int ret;
	struct timeval tv; 
	uint32_t i;

	vqp->sm_state = VRDMA_QP_STATE_POLL_PI;
	if (spdk_unlikely(vqp->flags & VRDMA_SEND_ERR_CQE)) {
		return vrdma_vqp_send_err_cqe(vqp);
	}
	if (spdk_unlikely(!vqp->bk_qp)) {
		return true;
	}
#ifdef POLL_PI_DBG
	SPDK_NOTICELOG("vrdam gen sq cqe start: vcq pi %d, pre_pi %d, ci %d\n",
					vcq->pi, vcq->pre_pi, vcq->pici->ci);
#endif
	gettimeofday(&tv, NULL);
	mcq = &vqp->bk_qp->bk_qp.sq_hw_cq;
	vcq = vqp->sq_vcq;
	for (i = 0; i < POLL_CQ_NUM; i++) {
		cqe = vrdma_poll_mqp_scq(mcq, SNAP_VRDMA_BACKEND_CQE_SIZE);	
		if (cqe == NULL) {
			/* if no available cqe, need to write prepared vcqes*/
#ifdef POLL_PI_DBG
			SPDK_NOTICELOG("get null MCQE: vcq new pi %d, pre_pi %d, ci %d\n",
							vcq->pi, vcq->pre_pi, vcq->pici->ci);
#endif
			goto write_vcq;
		}

		if (vcq->pi - vcq->pici->ci == vcq->cqe_entry_num) {
			SPDK_ERRLOG("vcq is full: vcq new pi %d, pre_pi %d, ci %d\n",
					vcq->pi, vcq->pre_pi, vcq->pici->ci);
			goto write_vcq;
		}
		
		wqe_idx = vrdma_get_wqe_id(vqp, cqe->wqe_counter);
		cqe_idx = vcq->pi & (vcq->cqe_entry_num - 1);

		SPDK_NOTICELOG("vrdam sq get new mcqe: put vcqe index %d\n", cqe_idx);
		
		vcqe = (struct vrdma_cqe *)vqp->sq_vcq->cqe_buff + cqe_idx;
		vcqe->imm_data = cqe->imm_inval_pkey;
		vcqe->length = cqe->byte_cnt;
		vcqe->req_id = wqe_idx;
		vcqe->local_qpn = vqp->qp_idx;
		//vcqe->ts = (uint32_t)cqe->timestamp;
		vcqe->ts = (uint32_t)tv.tv_usec;

		if (spdk_unlikely(mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_REQ)) {
			vrdma_mcqe_err(cqe);
			vcqe->opcode = cqe->sop_drop_qpn >> 24;
		} else {
			vcqe->opcode = cqe->op_own >> 4;
		}
		vcqe->owner = !!((vcq->pi++) & (vcq->cqe_entry_num));
	}

write_vcq:
	if (vcq->pi == vcq->pre_pi) {
#ifdef POLL_PI_DBG
		SPDK_NOTICELOG("no cqe to generate, jump to poll sq PI\n");
#endif
		return true;
	}
	vrdma_ring_mcq_db(mcq);
	vqp->stats.mcq_dbred_ci = mcq->ci;
	ret = vrdma_write_back_sq_cqe(vqp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to write cq CQE entry, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}
	SPDK_NOTICELOG("vrdam gen sq cqe done: vcq new pi %d, pre_pi %d\n",
					vcq->pi, vcq->pre_pi);
	vcq->pre_pi = vcq->pi;
	
	return false;
}

static bool vrdma_qp_sm_fatal_error(struct spdk_vrdma_qp *vqp,
									   enum vrdma_qp_sm_op_status status)
{
	/*
	 * TODO: maybe need to add more handling
	 */

	return false;
}

static struct vrdma_qp_sm_state vrdma_qp_sm_arr[] = {
/*VRDMA_QP_STATE_IDLE						  */ {vrdma_qp_sm_idle},
/*VRDMA_QP_STATE_POLL_PI					  */ {vrdma_qp_sm_poll_pi},
/*VRDMA_QP_STATE_HANDLE_PI					  */ {vrdma_qp_sm_handle_pi},
/*VRDMA_QP_STATE_WQE_READ					  */ {vrdma_qp_wqe_sm_read},
/*VRDMA_QP_STATE_WQE_PARSE					  */ {vrdma_qp_wqe_sm_parse},
/*VRDMA_QP_STATE_WQE_MAP_BACKEND			  */ {vrdma_qp_wqe_sm_map_backend},
/*VRDMA_QP_STATE_WQE_SUBMIT					  */ {vrdma_qp_wqe_sm_submit},
/*VRDMA_QP_STATE_POLL_CQ_CI					  */ {vrdma_qp_sm_poll_cq_ci},
/*VRDMA_QP_STATE_GEN_COMP					  */ {vrdma_qp_sm_gen_completion},
/*VRDMA_QP_STATE_FATAL_ERR					  */ {vrdma_qp_sm_fatal_error},
};

struct vrdma_qp_state_machine vrdma_sq_sm  = { vrdma_qp_sm_arr,
											sizeof(vrdma_qp_sm_arr) / sizeof(struct vrdma_qp_sm_state) };

/**
 * vrdma_qp_cmd_progress() - admq command state machine progress handle
 * @sq: admq to be processed
 * @status: status of calling function (can be a callback)
 *
 * Return: 0 (Currently no option to fail)
 */
static int vrdma_qp_wqe_progress(struct spdk_vrdma_qp *vqp,
								enum vrdma_qp_sm_op_status status)
{
	struct vrdma_qp_state_machine *sm;
	bool repeat = true;

	while (repeat) {
		repeat = false;
		//SPDK_NOTICELOG("vrdma vq sm state: %d\n", vqp->sm_state);
		sm = vqp->custom_sm;
		if (spdk_likely(vqp->sm_state < VRDMA_QP_NUM_OF_STATES))
			repeat = sm->sm_array[vqp->sm_state].sm_handler(vqp, status);
		else
			SPDK_ERRLOG("reached invalid state %d\n", vqp->sm_state);
	}

	return 0;
}
							
void vrdma_qp_sm_dma_cb(struct snap_dma_completion *self, int status)
{
	enum vrdma_qp_sm_op_status op_status = VRDMA_QP_SM_OP_OK;
	struct spdk_vrdma_qp *vqp = container_of(self, struct spdk_vrdma_qp, q_comp);

	if (status != IBV_WC_SUCCESS) {
		SPDK_ERRLOG("error in dma for vrdma sq state %d\n", vqp->sm_state);
		op_status = VRDMA_QP_SM_OP_ERR;
	}
	vrdma_qp_wqe_progress(vqp, op_status);
}

void vrdma_qp_sm_init(struct spdk_vrdma_qp *vqp)
{
	vqp->q_comp.func = vrdma_qp_sm_dma_cb;
	vqp->q_comp.count = 1;
	vqp->sm_state = VRDMA_QP_STATE_IDLE;
	vqp->custom_sm = &vrdma_sq_sm;
}

void vrdma_qp_sm_start(struct spdk_vrdma_qp *vqp)
{
	vrdma_qp_sm_poll_pi(vqp, VRDMA_QP_SM_OP_OK);
}

void vrdma_dump_vqp_stats(struct spdk_vrdma_qp *vqp)
{
	printf("\n========= vrdma qp debug counter =========\n");
	printf("sq pi  %6d       sq pre pi  %6d\n", vqp->qp_pi->pi.sq_pi, vqp->sq.comm.pre_pi);
	printf("scq pi %6d       scq pre pi %6d     scq ci %10d\n", 
			vqp->sq_vcq->pi, vqp->sq_vcq->pre_pi, vqp->sq_vcq->pici->ci);
	if (vqp->bk_qp) {
		printf("msq pi  %6d     msq dbred pi  %6d\n",
				vqp->bk_qp->bk_qp.hw_qp.sq.pi, vqp->stats.msq_dbred_pi);
		printf("mscq ci %6d     mscq dbred ci %6d\n",
				vqp->bk_qp->bk_qp.sq_hw_cq.ci, vqp->stats.mcq_dbred_ci);
	} else {
		printf("!!!no backend qp info \n");
	}
	printf("sq wqe fetched %15lu\n",vqp->stats.sq_wqe_fetched);
	printf("sq wqe submitted %15lu\n", vqp->stats.sq_wqe_submitted);
	printf("sq wqe wr submitted %15lu\n", vqp->stats.sq_wqe_wr);
	printf("sq wqe atomic submitted %15lu\n", vqp->stats.sq_wqe_atomic);
	printf("sq wqe ud submitted %15lu\n", vqp->stats.sq_wqe_ud);

}

