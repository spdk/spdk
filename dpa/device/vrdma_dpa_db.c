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

#include <libflexio-libc/string.h>
#include <libflexio-libc/stdio.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include "../vrdma_dpa_common.h"
#include "vrdma_dpa_dev_com.h"
#include "vrdma_dpa_cq.h"

//#define DPA_LATENCY_TEST
//#define VRDMA_DPA_DEBUG_DETAIL
// #define DPA_COUNT

static int get_next_qp_swqe_index(uint32_t pi, uint32_t depth)
{
	return (pi % depth);
}

static void swqe_seg_ctrl_set_rdmaw(union flexio_dev_sqe_seg *swqe, uint32_t sq_pi,
						 uint32_t sq_number, uint32_t ce,
						 uint32_t imm, bool imm_flag)
{
	uint32_t ds_count;
	uint32_t opcode;
	uint32_t mod;

	/* default for common case */
	mod = 0;
	if (imm_flag) {
		opcode = MLX5_CTRL_SEG_OPCODE_RDMA_WRITE_WITH_IMMEDIATE;
		swqe->ctrl.general_id = cpu_to_be32(imm);
	} else {
		opcode = MLX5_CTRL_SEG_OPCODE_RDMA_WRITE;
		swqe->ctrl.general_id = 0;
	}
	ds_count = 3;
	/* Fill out 1-st segment (Control) */
	swqe->ctrl.idx_opcode = cpu_to_be32((mod << 24) | ((sq_pi & 0xffff) << 8) | opcode);
	swqe->ctrl.qpn_ds = cpu_to_be32((sq_number << 8) | ds_count);
	swqe->ctrl.signature_fm_ce_se = cpu_to_be32(ce << 2);
}

static void vrdma_dpa_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint32_t remote_key,
					uint64_t remote_addr,
					uint32_t local_key,
					uint64_t local_addr,
					uint32_t size,
					uint16_t wr_pi,
					bool imm_flag)
{
	int swqe_index;
	union flexio_dev_sqe_seg *swqe;

	swqe_index = get_next_qp_swqe_index(ehctx->dma_qp.hw_qp_sq_pi,
					    ehctx->dma_qp.hw_qp_depth);
	swqe = (union flexio_dev_sqe_seg *)
		(ehctx->dma_qp.qp_sq_buff + (swqe_index * 64));

	/* Fill out 1-st segment (Control) rdma write/rdma write immediately*/
	swqe_seg_ctrl_set_rdmaw(swqe, ehctx->dma_qp.hw_qp_sq_pi,
			ehctx->dma_qp.qp_num,
			MLX5_CTRL_SEG_CE_CQE_ALWAYS,
			ehctx->vq_index<<16 | wr_pi, imm_flag);
	/* Fill out 2-nd segment (RDMA) */
	swqe++;
	flexio_dev_swqe_seg_rdma_set(swqe, remote_key,
					remote_addr);

	/* Fill out 3-rd segment (local Data) */
	swqe++;
	flexio_dev_swqe_seg_data_set(swqe, size,
					local_key,
					local_addr);
	ehctx->dma_qp.hw_qp_sq_pi++; /*pi is for each wqebb*/
}
// #endif

static void vrdma_dpa_rq_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint16_t rq_start_idx, uint16_t size,
					uint16_t imm_data_pi, bool imm_flag)
{
	uint32_t remote_key, local_key;
	uint64_t remote_addr, local_addr;
	uint16_t wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	//index = rq_start_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
	wqebb_size = ehctx->dma_qp.host_vq_ctx.rq_wqebb_size;

	local_key  = ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey;
	local_addr = ehctx->dma_qp.host_vq_ctx.rq_wqe_buff_pa +
		      wqebb_size * rq_start_idx;

	remote_key   = ehctx->dma_qp.arm_vq_ctx.rq_lkey;
	remote_addr  = ehctx->dma_qp.arm_vq_ctx.rq_buff_addr +
		      wqebb_size * rq_start_idx;

	vrdma_dpa_wr_pi_fetch(ehctx, remote_key, remote_addr, local_key,
				local_addr, size * wqebb_size, imm_data_pi, imm_flag);
#ifdef VRDMA_DPA_DEBUG_DETAIL
	printf("---naliu rq: index %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n imm_data_pi %#x\n",
			rq_start_idx, wqebb_size, size, remote_key, remote_addr, local_key, local_addr,
			imm_data_pi);
#endif
}

static void vrdma_dpa_sq_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint16_t sq_start_idx, uint16_t size,
					uint16_t imm_data_pi, bool imm_flag)
{
	uint32_t remote_key, local_key;
	uint64_t remote_addr, local_addr;
	uint16_t wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	wqebb_size = ehctx->dma_qp.host_vq_ctx.sq_wqebb_size;

	local_key  = ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey;
	local_addr = ehctx->dma_qp.host_vq_ctx.sq_wqe_buff_pa +
		      wqebb_size * sq_start_idx;

	remote_key   = ehctx->dma_qp.arm_vq_ctx.sq_lkey;
	remote_addr  = ehctx->dma_qp.arm_vq_ctx.sq_buff_addr +
		      wqebb_size * sq_start_idx;

	vrdma_dpa_wr_pi_fetch(ehctx, remote_key, remote_addr, local_key,
				local_addr, size * wqebb_size, imm_data_pi, imm_flag);
#ifdef VRDMA_DPA_DEBUG_DETAIL
	printf("---naliu sq: index %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n imm_data_pi %#x\n",
			sq_start_idx, wqebb_size, size, remote_key, remote_addr, local_key, local_addr,
			imm_data_pi);
#endif
}

#define VRDMA_COUNT_BATCH(batch, total_batch, times, start_end, pi) \
batch = start_end; \
total_batch += start_end; \
times++;

static inline int vrdma_vq_dpa_rollback(uint16_t pre_pi, uint16_t pi,
				        uint16_t q_size)
{
		if (pi % q_size == 0) {
			return 0;
		}
		return !(pi % q_size > pre_pi % q_size);
}


__FLEXIO_ENTRY_POINT_START
flexio_dev_event_handler_t vrdma_db_handler;
void vrdma_db_handler(flexio_uintptr_t thread_arg)
{
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct flexio_dev_thread_ctx *dtctx;
	uint16_t rq_pi = 0 , sq_pi = 0;
	uint16_t rq_pi_last = 0 , sq_pi_last = 0;
	uint32_t print = print;
	uint16_t rq_wqebb_cnt, sq_wqebb_cnt;
	uint16_t fetch_size;
	// uint32_t i, cnt[5];
	bool has_wqe = false;
	uint32_t empty_count = 0, change_count = 0;

	flexio_dev_get_thread_ctx(&dtctx);
	ehctx = (struct vrdma_dpa_event_handler_ctx *)thread_arg;
	printf("%s: --------virtq status %d.\n", __func__, ehctx->dma_qp.state);
	if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
#ifdef VRDMA_DPA_DEBUG
		printf("%s: ------virtq status %d is not READY.\n", __func__, ehctx->dma_qp.state);
#endif
		goto err_state;
	}
	flexio_dev_outbox_config(dtctx, ehctx->emu_outbox);
	flexio_dev_window_mkey_config(dtctx,
				      ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey);
	flexio_dev_window_ptr_acquire(dtctx, 0,
		(flexio_uintptr_t *)&ehctx->window_base_addr);

#ifdef VRDMA_DPA_DEBUG
	printf("---naliu vq_idx %d, emu_outbox %d, emu_crossing_mkey %d\n",
		ehctx->vq_index, ehctx->emu_outbox, ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey);
	printf("---naliu window_base_addr %#x\n", ehctx->window_base_addr);
	printf("---naliu rq_wqe_buff_pa %#lx, rq_pi_paddr %#lx, rq_wqebb_cnt %#x,"
			"rq_wqebb_size %#x, sq_wqe_buff_pa %#lx, sq_pi_paddr %#lx,"
			"sq_wqebb_cnt %#x, sq_wqebb_size %#lx, emu_crossing_mkey %#x,"
			"sf_crossing_mkey %#x\n",
			ehctx->dma_qp.host_vq_ctx.rq_wqe_buff_pa, ehctx->dma_qp.host_vq_ctx.rq_pi_paddr,
			ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt, ehctx->dma_qp.host_vq_ctx.rq_wqebb_size,
			ehctx->dma_qp.host_vq_ctx.sq_wqe_buff_pa, ehctx->dma_qp.host_vq_ctx.sq_pi_paddr,
			ehctx->dma_qp.host_vq_ctx.sq_wqebb_cnt, ehctx->dma_qp.host_vq_ctx.sq_wqebb_size,
			ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey, ehctx->dma_qp.host_vq_ctx.sf_crossing_mkey);
#endif
	rq_wqebb_cnt = ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
	sq_wqebb_cnt = ehctx->dma_qp.host_vq_ctx.sq_wqebb_cnt;
	/*fetch rq_pi*/
	rq_pi_last = ehctx->rq_last_fetch_start;
	sq_pi_last = ehctx->sq_last_fetch_start;

	rq_pi = *(uint16_t*)(ehctx->window_base_addr +
				ehctx->dma_qp.host_vq_ctx.rq_pi_paddr);
	sq_pi = *(uint16_t*)(ehctx->window_base_addr +
				ehctx->dma_qp.host_vq_ctx.sq_pi_paddr);
	// ehctx->pi_count++;

#ifdef DPA_LATENCY_TEST
	while (1)
#else
	while ((rq_pi_last != rq_pi) ||
	 	(sq_pi_last != sq_pi))
#endif
	{
		if (rq_pi_last != rq_pi) {
			if (!vrdma_vq_dpa_rollback(rq_pi_last, rq_pi, rq_wqebb_cnt)) {
				vrdma_dpa_rq_wr_pi_fetch(ehctx, rq_pi_last % rq_wqebb_cnt,
						rq_pi - rq_pi_last, rq_pi, true);
			} else {
				fetch_size = rq_wqebb_cnt - rq_pi_last % rq_wqebb_cnt;
				vrdma_dpa_rq_wr_pi_fetch(ehctx,
						rq_pi_last % rq_wqebb_cnt, fetch_size, 0, false);
				fetch_size = rq_pi % rq_wqebb_cnt;
				vrdma_dpa_rq_wr_pi_fetch(ehctx, 0, fetch_size, rq_pi, true);
			}
			VRDMA_COUNT_BATCH(ehctx->batch_stats.rq_batch,
					ehctx->batch_stats.rq_total_batchess,
					ehctx->batch_stats.rq_times,
					rq_pi - rq_pi_last,
					rq_pi);
			has_wqe = true;
		}

		if (sq_pi_last != sq_pi) {
			if (!vrdma_vq_dpa_rollback(sq_pi_last, sq_pi, sq_wqebb_cnt)) {
				vrdma_dpa_sq_wr_pi_fetch(ehctx, sq_pi_last % sq_wqebb_cnt,
						sq_pi - sq_pi_last, sq_pi, true);
			} else {
				fetch_size = sq_wqebb_cnt - sq_pi_last % sq_wqebb_cnt;
				vrdma_dpa_sq_wr_pi_fetch(ehctx,
						sq_pi_last % sq_wqebb_cnt, fetch_size, 0, false);
				fetch_size = sq_pi % sq_wqebb_cnt;
				vrdma_dpa_sq_wr_pi_fetch(ehctx, 0, fetch_size, sq_pi, true);
			}
			VRDMA_COUNT_BATCH(ehctx->batch_stats.sq_batch,
					ehctx->batch_stats.sq_total_batchess,
					ehctx->batch_stats.sq_times,
					sq_pi - sq_pi_last,
					sq_pi);
			has_wqe = true;
		}

		if (has_wqe) {
			flexio_dev_dbr_sq_set_pi((uint32_t *)
					ehctx->dma_qp.dbr_daddr + 1,
					ehctx->dma_qp.hw_qp_sq_pi);
			flexio_dev_qp_sq_ring_db(dtctx, ehctx->dma_qp.hw_qp_sq_pi,
					ehctx->dma_qp.qp_num);
			ehctx->wqe_send_count++;
			has_wqe = false;
			change_count++;
		} else {
			empty_count++;
		}
		if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
			printf("%s: Now virtq status is not READY.\n", __func__);
			goto out;
		}
		rq_pi_last = rq_pi;
		sq_pi_last = sq_pi;

#if 0
		if((empty_count == 100000) || (change_count == 10))
			goto out;
#endif

		/*fetch rq_pi*/
		asm volatile("fence iorw, iorw" ::: "memory");
		rq_pi = *(uint16_t*)(ehctx->window_base_addr + 
					ehctx->dma_qp.host_vq_ctx.rq_pi_paddr);
		sq_pi = *(uint16_t*)(ehctx->window_base_addr + 
					ehctx->dma_qp.host_vq_ctx.sq_pi_paddr);
		ehctx->pi_count++;
#ifdef DPA_COUNT
		if ((print != ehctx->wqe_send_count) && (ehctx->wqe_send_count % 512 == 1)) {
			print = ehctx->wqe_send_count;
			printf(//"\n------naliu latest_rq_batch %d, avg_rq_batch %d,  rq_total_batches %llu, rq_times %d\n"
				// "\n-----naliu latest_sq_batch %d, avg_sq_batch %d, sq_total_batchess %llu, sq_times %d\n" 
				// "\n-----naliu latest_sq_batch %d, sq_total_batchess %llu, sq_times %d\n"
				// "\n-----naliu pi_query %d, wqe_send_count %d\n",
				"\n-----naliu latest_sq_batch %d, avg_sq_batch %d, sq_total_batchess %llu, sq_times %d\n",
			// ehctx->batch_stats.rq_batch,
			// ehctx->batch_stats.rq_times?ehctx->batch_stats.rq_total_batchess/ehctx->batch_stats.rq_times:0,
			// ehctx->batch_stats.rq_total_batchess,
			// ehctx->batch_stats.rq_times,
			ehctx->batch_stats.sq_batch,
			ehctx->batch_stats.sq_times?ehctx->batch_stats.sq_total_batchess/ehctx->batch_stats.sq_times:0,
			ehctx->batch_stats.sq_total_batchess,
			// ehctx->batch_stats.sq_times,
			// ehctx->pi_count, ehctx->wqe_send_count);
			ehctx->batch_stats.sq_times);
			
			// for (i = 0 ; i < 5; i++) {
			// 	printf("count[%d] = %d,", i, cnt[i]);
			// }
			printf("\n");
		}
#endif
	}
out:
	ehctx->rq_last_fetch_start = rq_pi;
	ehctx->sq_last_fetch_start = sq_pi;

	flexio_dev_db_ctx_arm(dtctx, ehctx->guest_db_cq_ctx.cqn,
			      ehctx->emu_db_to_cq_id);
#if 0
	/*fetch rq_pi*/
	asm volatile("fence iorw, iorw" ::: "memory");
	rq_pi = *(uint16_t*)(ehctx->window_base_addr +
			ehctx->dma_qp.host_vq_ctx.rq_pi_paddr);
	sq_pi = *(uint16_t*)(ehctx->window_base_addr +
			ehctx->dma_qp.host_vq_ctx.sq_pi_paddr);

	if ((rq_pi_last != rq_pi) || (sq_pi_last != sq_pi)) {
		flexio_dev_db_ctx_force_trigger(dtctx,
				ehctx->guest_db_cq_ctx.cqn,
				ehctx->emu_db_to_cq_id);
	}
#endif
	vrdma_dpa_db_cq_incr(&ehctx->guest_db_cq_ctx);
	flexio_dev_dbr_cq_set_ci(ehctx->guest_db_cq_ctx.dbr,
				 ehctx->guest_db_cq_ctx.ci);
	flexio_dev_cq_arm(dtctx, ehctx->guest_db_cq_ctx.ci,
			  ehctx->guest_db_cq_ctx.cqn);
#ifdef VRDMA_DPA_DEBUG
	//printf("\n------naliu count[0] %d, count[1] %d, count[2] %d, count[3] %d, count[4] %d",
	//	ehctx->count[0], ehctx->count[1], ehctx->count[2], ehctx->count[3], ehctx->count[4]);
	printf("\n------naliu rq_pi %d, sq_pi %d\n", rq_pi, sq_pi);
	printf("\n------naliu dma_qp.hw_qp_sq_pi %d\n", ehctx->dma_qp.hw_qp_sq_pi);
	printf("\n------naliu vrdma_db_handler done. cqn: %#x, emu_db_to_cq_id %d, guest_db_cq_ctx.ci %d\n",
		ehctx->guest_db_cq_ctx.cqn, ehctx->emu_db_to_cq_id, ehctx->guest_db_cq_ctx.ci);
#endif
err_state:
	flexio_dev_reschedule();
}
__FLEXIO_ENTRY_POINT_END
