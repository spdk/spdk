/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/time.h>

#include "spdk/env.h"
#include "spdk/cpuset.h"
#include "spdk/thread.h"
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_srv.h"

#include "snap_dma.h"
#include "snap_vrdma_ctrl.h"

#define SPDK_IO_MGR_THREAD_NAME_PREFIX "VrdmaSnapThread"
#define SPDK_IO_MGR_THREAD_NAME_LEN 32

#define MLX5_ATOMIC_SIZE 8

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
	return (pi % q_size < pre_pi % q_size);
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
		SPDK_ERRLOG("failed to update admq CI, status %d\n", status);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam poll sq pi: doorbell pa 0x%lx\n", pi_addr);

	vqp->sm_state = VRDMA_QP_STATE_HANDLE_PI;
	vqp->q_comp.func = vrdma_qp_sm_dma_cb;
	vqp->q_comp.count = 1;

	ret = snap_dma_q_read(vqp->snap_queue->dma_q, &vqp->sq.comm.pi, sizeof(uint16_t),
			          vqp->sq.comm.mr->lkey, pi_addr,
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

	if (vqp->sq.comm.pi > vqp->sq.comm.pre_pi) {
		vqp->sm_state = VRDMA_QP_STATE_WQE_READ;
	} else {
		vqp->sm_state = VRDMA_QP_STATE_POLL_PI;
	}

	return true;
}

static bool vrdma_qp_wqe_sm_read(struct spdk_vrdma_qp *vqp,
                                    enum vrdma_qp_sm_op_status status)
{
	uint16_t pi = vqp->sq.comm.pi;
	uint32_t sq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = vqp->sq.comm.wqebb_cnt;
	int ret;

	local_ring_addr = (uint8_t *)vqp->sq.sq_buff;
	SPDK_NOTICELOG("vrdam poll sq wqe: sq pa 0x%lx\n", vqp->sq.comm.wqe_buff_pa);

	vqp->sm_state = VRDMA_QP_STATE_WQE_PARSE;
	vqp->sq.comm.num_to_parse = pi - vqp->sq.comm.pre_pi;

	//fetch the delta PI number entry in one time
	if (!vrdma_vq_rollback(vqp->sq.comm.pre_pi, pi, q_size)) {
		vqp->q_comp.count = 1;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = vqp->sq.comm.num_to_parse;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (vqp->sq.comm.pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
	    host_ring_addr = vqp->sq.comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, vqp->sq.sq_buff, sq_poll_size,
				              vqp->sq.comm.mr->lkey, host_ring_addr,
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
		num = q_size - (vqp->sq.comm.pre_pi % q_size);
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (vqp->sq.comm.pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
	    host_ring_addr = vqp->sq.comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, vqp->sq.sq_buff, sq_poll_size,
				              vqp->sq.comm.mr->lkey, host_ring_addr,
				              vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read sq WQE entry, ret %d\n", ret);
		    vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		    return true;
		}
		
		/* calculate second poll size */
		local_ring_addr = (uint8_t *)vqp->sq.sq_buff + num * sizeof(struct vrdma_send_wqe);
		vqp->q_comp.count++;
		vqp->q_comp.func = vrdma_qp_sm_dma_cb;
		num = pi % q_size;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		ret = snap_dma_q_read(vqp->snap_queue->dma_q, local_ring_addr, sq_poll_size,
				              vqp->sq.comm.mr->lkey, vqp->sq.comm.wqe_buff_pa,
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

	SPDK_NOTICELOG("vrdam parse sq wqe: vq pi %d, pre_pi %d\n", vqp->sq.comm.pi, vqp->sq.comm.pre_pi);
	vqp->sm_state = VRDMA_QP_STATE_WQE_MAP_BACKEND;

	//TODO: parse wqe handling
	return true;
}

static inline struct vrdma_backend_qp *vrdma_vq_get_mqp(struct spdk_vrdma_qp *vqp)
{
	//TODO: currently, only one-to-one map
	return vqp->bk_qp[0];
}

static bool vrdma_qp_wqe_sm_map_backend(struct spdk_vrdma_qp *vqp,
                                    		enum vrdma_qp_sm_op_status status)
{
	vqp->bk_qp[0] = vrdma_vq_get_mqp(vqp);
	SPDK_NOTICELOG("vrdam map sq wqe: vq pi %d, mqp %p\n", vqp->sq.comm.pi, vqp->bk_qp[0]);
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
	rseg->rkey     = htobe32(rkey);
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
#if !__DPA
	/* 8.9.3.1  Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 *    WQE (on WQEBB granularity)
	 *
	 * 2. Update Doorbell Record associated with that queue by writing
	 *    the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 **/
	vrdma_update_tx_db(bk_qp);

	/* Make sure that doorbell record is written before ringing the doorbell
	 **/
	snap_memory_bus_store_fence();

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *    Register field in the UAR associated with that queue
	 */
	vrdma_flush_tx_db(bk_qp, ctrl);

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	//if (!vrdma_flush_tx_db->hw_qp.sq.tx_db_nc)
	//	snap_memory_bus_store_fence();
#endif

#else
	/* Based on review with Eliav:
	 * - only need a store fence to ensure that the wqe is committed to the
	 *   memory
	 * - there is no need to update dbr on DPA
	 * - The flow works only if there is no doorbell recovery. Unfortunately
	 *   doorbell recovery can happen on any QP in the same gvmi. Then it
	 *   will trigger recovery on the dpa qp with wrong pi.
	 * - Disable optimization for now.
	 * - At the moment optimization has no measurable effect on single
	 *   queue virtio performance
	 */
#define HAVE_DOORBELL_RECOVERY 1
#if SIMX_BUILD || HAVE_DOORBELL_RECOVERY
	vrdma_update_tx_db(bk_qp);
#endif
	snap_memory_bus_store_fence();
	dpa_dma_q_ring_tx_db(bk_qp->hw_qp.qp_num, bk_qp->hw_qp.sq.pi);
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
	bk_qp->hw_qp.sq.pi++;
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

static int vrdma_rw_wqe_submit(struct vrdma_send_wqe *wqe,
										struct snap_vrdma_backend_qp *bk_qp, 
										uint8_t opcode)
{
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	struct vrdma_buf_desc sge;
	void *seg;
	uint8_t fm_ce_se = 0;
	uint8_t ds = 0;
	uint8_t sig = 0;
	uint32_t imm = 0;
	uint8_t i = 0;
	uint8_t sge_num = wqe->meta.sge_num;
	uint64_t sge_addr;

	fm_ce_se = vrdma_get_send_flags(wqe);

	ctrl = seg = (struct mlx5_wqe_ctrl_seg *)vrdma_get_wqe_bb(bk_qp);

	seg += sizeof(*ctrl); 
	ds += sizeof(*ctrl) / 16;

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	vrdma_set_raddr_seg(rseg, (uintptr_t)wqe->rdma_rw.remote_addr, wqe->rdma_rw.rkey);

	seg  += sizeof(*rseg);
	ds += sizeof(*rseg) / 16;
	
	dseg = seg;
	for (i = 0; i < sge_num; i++) {
		sge = wqe->sgl[i];
		if (spdk_likely(sge.buf_length)) {
			sge_addr = ((uint64_t)sge.buf_addr_hi << 32) + sge.buf_addr_lo;
			mlx5dv_set_data_seg(dseg, sge.buf_length, sge.lkey, (intptr_t)sge_addr);
			++dseg;
			ds += sizeof(*dseg) / 16;		
		}
	}

	vrdma_set_ctrl_seg(ctrl, bk_qp->hw_qp.sq.pi, opcode, 0, bk_qp->hw_qp.qp_num,
			    	fm_ce_se, ds, sig, imm);	
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
	struct mlx5_wqe_data_seg *dseg;
	struct vrdma_buf_desc sge;
	void *seg;
	uint8_t fm_ce_se = 0;
	uint8_t ds = 0;
	uint8_t sig = 0;
	uint32_t imm = 0;
	uint8_t i = 0;
	uint8_t sge_num = wqe->meta.sge_num;
	uint64_t sge_addr;

	fm_ce_se = vrdma_get_send_flags(wqe);

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
	
	dseg = seg;
	for (i = 0; i < sge_num; i++) {
		sge = wqe->sgl[i];
		if (spdk_likely(sge.buf_length)) {
			sge_addr = ((uint64_t)sge.buf_addr_hi << 32) + sge.buf_addr_lo;
			mlx5dv_set_data_seg(dseg, MLX5_ATOMIC_SIZE, sge.lkey, (intptr_t)sge_addr);
			++dseg;
			ds += sizeof(*dseg) / 16;		
		}
	}

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
	struct snap_vrdma_backend_qp *backend_qp = &vqp->bk_qp[0]->bk_qp;
	uint16_t i;
	struct vrdma_send_wqe *wqe;
	uint8_t opcode = 0;
	uint16_t q_size = vqp->sq.comm.wqebb_cnt;

	SPDK_NOTICELOG("vrdam submit sq wqe: vq pi %d, pre_pi %d\n",
					vqp->sq.comm.pi, vqp->sq.comm.pre_pi);
	vqp->sm_state = VRDMA_QP_STATE_POLL_CQ_CI;

	for (i = 0; i < num_to_parse; i++) {
		wqe = vqp->sq.sq_buff + ((vqp->sq.comm.pre_pi + i) % q_size);
		opcode = wqe->meta.opcode;
		switch (opcode) {
			case IBV_WR_RDMA_READ:
			case IBV_WR_RDMA_WRITE:
			case IBV_WR_RDMA_WRITE_WITH_IMM:
				vrdma_rw_wqe_submit(wqe, backend_qp, opcode);
				break;
			case IBV_WR_ATOMIC_CMP_AND_SWP:
			case IBV_WR_ATOMIC_FETCH_AND_ADD:
				vrdma_atomic_wqe_submit(wqe, backend_qp, opcode);
				break;
			default:
				// place holder, will be replaced in future
				vrdma_ud_wqe_submit(wqe, backend_qp, opcode);
				vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
				return false;
		}
	}

	vrdma_tx_complete(backend_qp);
	return true;
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

static inline struct mlx5_cqe64 *vrdma_poll_mqp_cq(struct snap_hw_cq *dv_cq,
															int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	cqe = vrdma_get_mqp_cqe(dv_cq, cqe_size);

	/* cqe is hw owned */
	if (mlx5dv_get_cqe_owner(cqe) == !(dv_cq->ci & dv_cq->cqe_cnt))
		return NULL;

	/* and must have valid opcode */
	if (mlx5dv_get_cqe_opcode(cqe) == MLX5_CQE_INVALID)
		return NULL;

	dv_cq->ci++;

	snap_debug("cq: 0x%x ci: %d CQ opcode %d size %d wqe_counter %d scatter32 %d scatter64 %d\n",
		   dv_cq->cq_num, dv_cq->ci,
		   mlx5dv_get_cqe_opcode(cqe),
		   be32toh(cqe->byte_cnt),
		   be16toh(cqe->wqe_counter),
		   cqe->op_own & MLX5_INLINE_SCATTER_32,
		   cqe->op_own & MLX5_INLINE_SCATTER_64);
	return cqe;
}

static uint32_t vrdma_get_wqe_id(struct spdk_vrdma_qp *vqp, uint32_t mwqe_idx)
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

	SPDK_NOTICELOG("vrdam poll sq vcq ci: doorbell pa 0x%lx\n", ci_addr);

	vqp->sm_state = VRDMA_QP_STATE_GEN_COMP;
	vqp->q_comp.func = vrdma_qp_sm_dma_cb;
	vqp->q_comp.count = 1;

	// TODO, here lkey need to be changed to vcq
	ret = snap_dma_q_read(vqp->snap_queue->dma_q, &vqp->sq_vcq->ci, sizeof(uint32_t),
			          vqp->sq.comm.mr->lkey, ci_addr,
			          vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read sq vcq CI, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}

	return false;
}

static bool vrdma_qp_sm_gen_completion(struct spdk_vrdma_qp *vqp,
                                           enum vrdma_qp_sm_op_status status)
{
	struct snap_hw_cq *mcq = &vqp->bk_qp[0]->bk_qp.sq_hw_cq;
	struct spdk_vrdma_cq *vcq = vqp->sq_vcq;
	struct mlx5_cqe64 *cqe;
	struct vrdma_cqe *vcqe;
	uint32_t wqe_idx;
	uint32_t cqe_idx;
	uint64_t vcq_host_addr = vcq->host_pa + vcq->pre_pi * vcq->cqebb_size;
	uint8_t *vcq_local_addr = (uint8_t *)vqp->sq.cqe_buff + vcq->pre_pi * vcq->cqebb_size;
	uint32_t size;
	int ret;
	struct timeval tv; 

	SPDK_NOTICELOG("vrdam gen sq cqe: vcq pi %d, pre_pi %d\n",
					vqp->sq.comm.pi, vqp->sq.comm.pre_pi);
	vqp->sm_state = VRDMA_QP_STATE_POLL_PI;
	gettimeofday(&tv, NULL);

	while(1) {
		cqe = vrdma_poll_mqp_cq(mcq, 64);
		if (cqe == NULL || vcq->pi - vcq->ci == vcq->cqe_entry_num) {
			/* if no available cqe or vcq is full
			   need to write prepared vcqes*/
			goto write_vcq;
		}
		wqe_idx = vrdma_get_wqe_id(vqp, cqe->wqe_counter);
		cqe_idx = vcq->pi & (vcq->cqe_entry_num - 1);
		
		vcqe = vqp->sq.cqe_buff + cqe_idx;
		vcqe->imm_data = cqe->imm_inval_pkey;
		vcqe->length = cqe->byte_cnt;
		vcqe->opcode = cqe->sop_drop_qpn >> 24;
		vcqe->req_id = wqe_idx;
		vcqe->local_qpn = vqp->qp_idx;
		vcqe->owner = ((vcq->pi++) & (vcq->cqe_entry_num - 1)) & 1;
		//vcqe->ts = (uint32_t)cqe->timestamp;
		vcqe->ts = (uint32_t)tv.tv_usec;
	}

write_vcq:
	vqp->q_comp.count = 1;
	size = (vcq->pi - vcq->pre_pi) * vcq->cqebb_size;
	ret = snap_dma_q_write(vqp->snap_queue->dma_q, vcq_local_addr, size,
				              vqp->sq.comm.mr->lkey, vcq_host_addr,
				              vqp->snap_queue->ctrl->xmkey->mkey, &vqp->q_comp);

	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to write cq CQE entry, ret %d\n", ret);
		vqp->sm_state = VRDMA_QP_STATE_FATAL_ERR;
		return true;
	}
	
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
/*VRDMA_QP_STATE_IDLE                         */ {vrdma_qp_sm_idle},
/*VRDMA_QP_STATE_POLL_PI                      */ {vrdma_qp_sm_poll_pi},
/*VRDMA_QP_STATE_HANDLE_PI                    */ {vrdma_qp_sm_handle_pi},
/*VRDMA_QP_STATE_WQE_READ                     */ {vrdma_qp_wqe_sm_read},
/*VRDMA_QP_STATE_WQE_PARSE                    */ {vrdma_qp_wqe_sm_parse},
/*VRDMA_QP_STATE_WQE_MAP_BACKEND              */ {vrdma_qp_wqe_sm_map_backend},
/*VRDMA_QP_STATE_WQE_SUBMIT                   */ {vrdma_qp_wqe_sm_submit},
/*VRDMA_QP_STATE_POLL_CQ_CI                   */ {vrdma_qp_sm_poll_cq_ci},
/*VRDMA_QP_STATE_GEN_COMP                  	  */ {vrdma_qp_sm_gen_completion},
/*VRDMA_QP_STATE_FATAL_ERR                    */ {vrdma_qp_sm_fatal_error},
};

struct vrdma_qp_state_machine vrdma_sq_sm  = { vrdma_qp_sm_arr,
											sizeof(vrdma_qp_sm_arr) / sizeof(struct vrdma_qp_sm_state) };

/**
 * vrdma_qp_cmd_progress() - admq command state machine progress handle
 * @sq:	admq to be processed
 * @status:	status of calling function (can be a callback)
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
		SPDK_NOTICELOG("vrdma vq sm state: %d\n", vqp->sm_state);
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

