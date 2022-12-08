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
// #include "vrdma_dpa_txrx.h"
// #include "vrdma_dpa_tx.h"

__FLEXIO_ENTRY_POINT_START

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

flexio_dev_rpc_handler_t vrdma_qp_rpc_handler;
uint64_t vrdma_qp_rpc_handler(uint64_t arg1, uint64_t __unused arg2,
				     uint64_t __unused arg3)
{
	struct  vrdma_dpa_event_handler_ctx *ectx;
	struct flexio_dev_thread_ctx *dtctx;

	ectx = (struct vrdma_dpa_event_handler_ctx *)arg1;
	flexio_dev_get_thread_ctx(&dtctx);
	printf("\n------naliu vrdma_qp_rpc_handler cqn: %d, emu_db_to_cq_id %d,"
		"guest_db_cq_ctx.ci %d\n", ectx->guest_db_cq_ctx.cqn,
		ectx->emu_db_to_cq_id, ectx->guest_db_cq_ctx.ci);

	flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
			      ectx->emu_db_to_cq_id);
	flexio_dev_cq_arm(dtctx, ectx->guest_db_cq_ctx.ci,
			  ectx->guest_db_cq_ctx.cqn);
	flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
					ectx->emu_db_to_cq_id);
	return 0;
}

__FLEXIO_ENTRY_POINT_END
