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

#include <libflexio-dev/flexio_dev_queue_access.h>

flexio_dev_rpc_handler_t vrdma_dpa_msix_send_rpc_handler;

__FLEXIO_ENTRY_POINT_START
uint64_t vrdma_dpa_msix_send_rpc_handler(uint64_t arg1, uint64_t arg2,
					   uint64_t __unused arg3)
{
	struct flexio_dev_thread_ctx *dtctx;
	uint32_t outbox_id = arg2;
	uint32_t cqn = arg1;

	flexio_dev_get_thread_ctx(&dtctx);
	flexio_dev_outbox_config(dtctx, outbox_id);
	flexio_dev_msix_send(dtctx, cqn);
	return 0;
}

__FLEXIO_ENTRY_POINT_END
