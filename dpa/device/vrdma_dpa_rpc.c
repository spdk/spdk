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
// #include "vrdma_dpa_txrx.h"
// #include "vrdma_dpa_tx.h"

__FLEXIO_ENTRY_POINT_START

flexio_dev_arg_unpack_func_t vrdma_dpa_rpc_unpack_func;
uint64_t vrdma_dpa_rpc_unpack_func(void *arg_buf, void *func)
{
	uint64_t arg1 = *(uint64_t *)arg_buf;
	flexio_dev_rpc_handler_t *vrdma_rpc_handler = func;
	(*vrdma_rpc_handler)(arg1);
	return 0;
}

#if 0
flexio_dev_rpc_handler_t test_dpa_flexio_work;
uint64_t test_dpa_flexio_work(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	uint64_t res;
	TRACEVAL(arg1);
	TRACEVAL(arg2);
	TRACEVAL(arg3);

	res = arg1 + arg2 + arg3;

	// flexio_dev_print("------naliu DPA says %lu + %lu + %lu is %lu\n", arg1, arg2, arg3, res);
	printf("------naliu DPA says %lu + %lu + %lu is %lu\n", arg1, arg2, arg3, res);
	return res;
}
#endif

flexio_dev_rpc_handler_t vrdma_qp_rpc_handler;
uint64_t vrdma_qp_rpc_handler(uint64_t arg1)
{

	struct  vrdma_dpa_event_handler_ctx *ectx;
	struct flexio_dev_thread_ctx *dtctx;
#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n------naliu vrdma_qp_rpc_handler start\n");
#endif
	flexio_dev_get_thread_ctx(&dtctx);
	ectx = (struct vrdma_dpa_event_handler_ctx *)arg1;
	vrdma_debug_count_set(ectx, 0);
	flexio_dev_outbox_config(dtctx, ectx->emu_outbox);
#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n------naliu vrdma_qp_rpc_handler cqn: %#x, emu_db_to_cq_id %d,"
		"guest_db_cq_ctx.ci %d\n", ectx->guest_db_cq_ctx.cqn,
		ectx->emu_db_to_cq_id, ectx->guest_db_cq_ctx.ci);
#endif
	flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
			      ectx->emu_db_to_cq_id);
	flexio_dev_cq_arm(dtctx, ectx->guest_db_cq_ctx.ci,
			  ectx->guest_db_cq_ctx.cqn);
	flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
					ectx->emu_db_to_cq_id);
	vrdma_debug_count_set(ectx, 1);
#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n------naliu vrdma_qp_rpc_handler end\n");
#endif
	return 0;
}

flexio_dev_rpc_handler_t vrdma_dev2host_copy_handler;
uint64_t vrdma_dev2host_copy_handler(uint64_t arg1)
{
	struct vrdma_window_dev_config *window_cfg;
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct vrdma_dpa_vq_data *host_data;
	struct flexio_dev_thread_ctx *dtctx;

	window_cfg = (struct vrdma_window_dev_config *)arg1;
	ehctx = (struct vrdma_dpa_event_handler_ctx *)window_cfg->heap_memory;
	flexio_dev_get_thread_ctx(&dtctx);

	/* get window mkey*/
	flexio_dev_window_mkey_config(dtctx, window_cfg->mkey);

	/* acquire dev ptr to host memory */
	flexio_dev_window_ptr_acquire(dtctx, (flexio_uintptr_t)window_cfg->haddr,
				      (flexio_uintptr_t *)&host_data);

	memcpy(&host_data->ehctx, ehctx, sizeof(host_data->ehctx));
	return 0;
}

__FLEXIO_ENTRY_POINT_END
