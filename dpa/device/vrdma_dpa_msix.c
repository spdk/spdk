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

#include <common/flexio_common.h>
#include <libflexio-libc/string.h>
#include <libflexio-libc/stdio.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include "../vrdma_dpa_common.h"
#include "vrdma_dpa_cq.h"

__FLEXIO_ENTRY_POINT_START
flexio_dev_event_handler_t vrdma_msix_handler;
void vrdma_msix_handler(flexio_uintptr_t thread_arg)
{
	flexio_dev_return();
}

#if 0
void vrdma_msix_handler(flexio_uintptr_t thread_arg)
{
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct flexio_dev_thread_ctx *dtctx;

	flexio_dev_get_thread_ctx(&dtctx);
	ehctx = (struct vrdma_dpa_event_handler_ctx *)thread_arg;
	if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
		printf("%s: virtq status is not READY.\n", __func__);
		goto out;
	}

	/*Tod: mask need to change to cq_size-1*/
	while (cqe = vrdma_dpa_cqe_get(&ehctx->msix_cq_ctx, mask)) {
		opcode = flexio_dev_cqe_get_opcode(cqe);
		if (unlikely(opcode == MLX5_CQE_RESP_ERR)) {
			/*Todo: we can add an error count in ehctx or ehctx->counters*/
		} else {
			/*get cqn from cqe data*/
			flexio_dev_msix_send(dtctx, cqn);
		}
	}

out:
	flexio_dev_dbr_cq_set_ci(ehctx->msix_cq_ctx.dbr,
				 ehctx->msix_cq_ctx.ci);
	flexio_dev_cq_arm(dtctx, ehctx->msix_cq_ctx.ci,
			  ehctx->msix_cq_ctx.cqn);
	flexio_dev_return();

}

#endif




// {
// 	struct vrdma_dpa_event_handler_ctx *ehctx;
// 	struct flexio_dev_thread_ctx *dtctx;
// 	uint16_t rq_last_fetch_start = 0;
// 	uint16_t sq_last_fetch_start = 0;
// 	uint16_t rq_last_fetch_end = 0;
// 	uint16_t sq_last_fetch_end = 0;
// 	uint16_t rq_pi, sq_pi;
// 	uint16_t rq_wr_num, sq_wr_num;

// 	flexio_dev_get_thread_ctx(&dtctx);
// 	ehctx = (struct vrdma_dpa_event_handler_ctx *)thread_arg;
// 	if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
// 		printf("%s: virtq status is not READY.\n", __func__);
// 		goto out;
// 	}

// 	while (flexio_dev_cqe_get_owner(ehctx.msix_cq_ctx.cqe) != ehctx->msix_cq_ctx.hw_owner_bit)
// 	{
// 		flexio_dev_print("Process packet: %d\n", app_ctx.packets_count++);

// 		TRACE("CQE received, idx ", app_ctx.rq_cq_ctx.cq_idx & CQ_IDX_MASK);
// 		process_packet(dtctx);
// 		com_step_cq(&app_ctx.rq_cq_ctx);
// 	}
	
// out:
// 	// virtnet_dpa_db_cq_incr(&ehctx->msix_cq_ctx);
// 	flexio_dev_dbr_cq_set_ci(ehctx->msix_cq_ctx.dbr,
// 				 ehctx->msix_cq_ctx.ci);
// 	flexio_dev_cq_arm(dtctx, ehctx->msix_cq_ctx.ci,
// 			  ehctx->msix_cq_ctx.cqn);
// 	flexio_dev_return();
// }
__FLEXIO_ENTRY_POINT_END
