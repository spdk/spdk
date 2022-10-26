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
#include "spdk/env.h"
#include "spdk/cpuset.h"
#include "spdk/thread.h"
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"

#include "snap_dma.h"
#include "snap_vrdma_ctrl.h"

#define SPDK_IO_MGR_THREAD_NAME_PREFIX "VrdmaSnapThread"
#define SPDK_IO_MGR_THREAD_NAME_LEN 32

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


static bool vrdma_sq_sm_idle(struct vrdma_sq *sq,
                                    enum vrdma_vq_sm_op_status status)
{
	SPDK_ERRLOG("vrdma sq in invalid state %d\n",
					   VRDMA_SQ_STATE_IDLE);
	return false;
}

static bool vrdma_sq_sm_poll_pi(struct vrdma_sq *sq,
                                   enum vrdma_vq_sm_op_status status)
{
	int ret;
	uint64_t pi_addr = sq->comm.doorbell_pa;

	if (status != VRDMA_VQ_SM_OP_OK) {
		SPDK_ERRLOG("failed to update admq CI, status %d\n", status);
		sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam poll sq pi: doorbell pa 0x%lx\n", pi_addr);

	sq->sm_state = VRDMA_SQ_STATE_HANDLE_PI;
	sq->q_comp.func = vrdma_sq_sm_dma_cb;
	sq->q_comp.count = 1;

	ret = snap_dma_q_read(sq->snap_queue->dma_q, &sq->comm.pi, sizeof(uint16_t),
			          sq->mr->lkey, pi_addr,
			          sq->snap_queue->ctrl->xmkey->mkey, &sq->q_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read sq PI, ret %d\n", ret);
		sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
	}

	return false;
}

static bool vrdma_sq_sm_handle_pi(struct vrdma_sq *sq,
                                    enum vrdma_vq_sm_op_status status)
{

	if (status != VRDMA_VQ_SM_OP_OK) {
		SPDK_ERRLOG("failed to get vq PI, status %d\n", status);
		sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
		return true;
	}

	if (sq->comm.pi > sq->comm.pre_pi) {
		sq->sm_state = VRDMA_SQ_STATE_WQE_READ;
	} else {
		sq->sm_state = VRDMA_SQ_STATE_POLL_PI;
	}

	return true;
}

static bool vrdma_sq_sm_read_wqe(struct vrdma_sq *sq,
                                    enum vrdma_vq_sm_op_status status)
{
	uint16_t pi = sq->comm.pi;
	uint32_t sq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = sq->comm.wqebb_cnt;
	int ret;

	local_ring_addr = (uint8_t *)sq->sq_buff;
	SPDK_NOTICELOG("vrdam poll sq wqe: sq pa 0x%lx\n", sq->comm.wqe_buff_pa);

	sq->sm_state = VRDMA_SQ_STATE_WQE_PARSE;
	sq->comm.num_to_parse = pi - sq->comm.pre_pi;

	//fetch the delta PI number entry in one time
	if (!vrdma_vq_rollback(sq->comm.pre_pi, pi, q_size)) {
		sq->q_comp.count = 1;
		sq->q_comp.func = vrdma_sq_sm_dma_cb;
		num = sq->comm.num_to_parse;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (sq->comm.pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
	    host_ring_addr = sq->comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(sq->snap_queue->dma_q, sq->sq_buff, sq_poll_size,
				              sq->mr->lkey, host_ring_addr,
				              sq->snap_queue->ctrl->xmkey->mkey, &sq->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read sq WQE entry, ret %d\n", ret);
		    sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
		    return true;
		}
	} else {
		/* vq roll back case, first part */
		sq->q_comp.count = 1;
		sq->q_comp.func = vrdma_sq_sm_dma_cb;
		num = q_size - (sq->comm.pre_pi % q_size);
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		offset = (sq->comm.pre_pi % q_size) * sizeof(struct vrdma_send_wqe);
	    host_ring_addr = sq->comm.wqe_buff_pa + offset;
		ret = snap_dma_q_read(sq->snap_queue->dma_q, sq->sq_buff, sq_poll_size,
				              sq->mr->lkey, host_ring_addr,
				              sq->snap_queue->ctrl->xmkey->mkey, &sq->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read sq WQE entry, ret %d\n", ret);
		    sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
		    return true;
		}
		
		/* calculate second poll size */
		local_ring_addr = (uint8_t *)sq->sq_buff + num * sizeof(struct vrdma_send_wqe);
		sq->q_comp.count++;
		sq->q_comp.func = vrdma_sq_sm_dma_cb;
		num = pi % q_size;
		sq_poll_size = num * sizeof(struct vrdma_send_wqe);
		ret = snap_dma_q_read(sq->snap_queue->dma_q, local_ring_addr, sq_poll_size,
				              sq->mr->lkey, sq->comm.wqe_buff_pa,
				              sq->snap_queue->ctrl->xmkey->mkey, &sq->q_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second read sq WQE entry, ret %d\n", ret);
		    	sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
			return true;
		}
	}

	return false;
}

static bool vrdma_sq_sm_parse_wqe(struct vrdma_sq *sq,
                                   enum vrdma_vq_sm_op_status status)
{
	if (status != VRDMA_VQ_SM_OP_OK) {
		SPDK_ERRLOG("failed to read vq wqe, status %d\n", status);
		sq->sm_state = VRDMA_SQ_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam parse sq wqe: vq pi %d, pre_pi %d\n", sq->comm.pi, sq->comm.pre_pi);
	sq->sm_state = VRDMA_SQ_STATE_WQE_MAP_BACKEND;

	//TODO: parse wqe handling
	return true;
}

static inline uint32_t vrdma_vq_get_mqpn(struct vrdma_sq *sq)
{
	// TODO: currently, only one-to-one map
	return sq->comm.mqpn[0];
}

static bool vrdma_sq_sm_map_wqe(struct vrdma_sq *sq,
                                    enum vrdma_vq_sm_op_status status)
{
	sq->comm.mqpn[0] = vrdma_vq_get_mqpn(sq);
	SPDK_NOTICELOG("vrdam map sq wqe: vq pi %d, mqpn %d\n", sq->comm.qpn, sq->comm.mqpn[0]);
	sq->sm_state = VRDMA_SQ_STATE_WQE_SUBMIT;
	
	return true;
}

static bool vrdma_sq_sm_submit_wqe(struct vrdma_sq *sq,
                                    enum vrdma_vq_sm_op_status status)
{
	SPDK_NOTICELOG("vrdam submit sq wqe: vq pi %d, pre_pi %d\n", sq->comm.pi, sq->comm.pre_pi);
	sq->sm_state = VRDMA_SQ_STATE_POLL_PI;

	//TODO: translate wqe and submit
	return true;
}

static bool vrdma_sq_sm_gen_completion(struct vrdma_sq *sq,
                                           enum vrdma_vq_sm_op_status status)
{
	SPDK_NOTICELOG("vrdam gen sq cqe: vq pi %d, pre_pi %d\n", sq->comm.pi, sq->comm.pre_pi);
	sq->sm_state = VRDMA_SQ_STATE_WQE_MAP_BACKEND;

	//TODO: poll mqp cq handling
	return true;
}

static bool vrdma_sq_sm_fatal_error(struct vrdma_sq *sq,
                                       enum vrdma_vq_sm_op_status status)
{
	/*
	 * TODO: maybe need to add more handling
	 */

	return false;
}

static struct vrdma_sq_sm_state vrdma_sq_sm_arr[] = {
/*VRDMA_SQ_STATE_IDLE                         */ {vrdma_sq_sm_idle},
/*VRDMA_SQ_STATE_POLL_PI                      */ {vrdma_sq_sm_poll_pi},
/*VRDMA_SQ_STATE_HANDLE_PI                    */ {vrdma_sq_sm_handle_pi},
/*VRDMA_SQ_STATE_WQE_READ                     */ {vrdma_sq_sm_read_wqe},
/*VRDMA_SQ_STATE_WQE_PARSE                    */ {vrdma_sq_sm_parse_wqe},
/*VRDMA_SQ_STATE_WQE_MAP_BACKEND              */ {vrdma_sq_sm_map_wqe},
/*VRDMA_SQ_STATE_WQE_SUBMIT                  */ {vrdma_sq_sm_submit_wqe},
///*VRDMA_SQ_STATE_GEN_COMP                  */ {vrdma_sq_sm_gen_completion},
/*VRDMA_SQ_STATE_FATAL_ERR                   */ {vrdma_sq_sm_fatal_error},
};

struct vrdma_sq_state_machine vrdma_sq_sm  = { vrdma_sq_sm_arr, sizeof(vrdma_sq_sm_arr) / sizeof(struct vrdma_sq_sm_state) };

/**
 * vrdma_sq_cmd_progress() - admq command state machine progress handle
 * @sq:	admq to be processed
 * @status:	status of calling function (can be a callback)
 *
 * Return: 0 (Currently no option to fail)
 */
static int vrdma_sq_wqe_progress(struct vrdma_sq *sq,
		          	enum vrdma_vq_sm_op_status status)
{
	struct vrdma_sq_state_machine *sm;
	bool repeat = true;

	while (repeat) {
		repeat = false;
		SPDK_NOTICELOG("vrdma vq sm state: %d\n", sq->sm_state);
		sm = sq->custom_sm;
		if (spdk_likely(sq->sm_state < VRDMA_SQ_NUM_OF_STATES))
			repeat = sm->sm_array[sq->sm_state].sm_handler(sq, status);
		else
			SPDK_ERRLOG("reached invalid state %d\n", sq->sm_state);
	}

	return 0;
}

void vrdma_sq_sm_dma_cb(struct snap_dma_completion *self, int status)
{
	enum vrdma_vq_sm_op_status op_status = VRDMA_VQ_SM_OP_OK;
	struct vrdma_sq *sq = container_of(self, struct vrdma_sq, q_comp);

	if (status != IBV_WC_SUCCESS) {
		SPDK_ERRLOG("error in dma for vrdma sq state %d\n", sq->sm_state);
		op_status = VRDMA_VQ_SM_OP_ERR;
	}
	vrdma_sq_wqe_progress(sq, op_status);
}

