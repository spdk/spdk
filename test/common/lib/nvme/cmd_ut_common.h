/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_NVME_CMD_UT_COMMON_H
#define SPDK_NVME_CMD_UT_COMMON_H

#include "spdk_internal/cunit.h"
#include "nvme/nvme_internal.h"

#define UT_NUM_REQUESTS 32

static inline void
ut_qpair_init(struct spdk_nvme_qpair *qpair, struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;

	memset(qpair, 0, sizeof(*qpair));
	qpair->ctrlr = ctrlr;
	qpair->req_buf = calloc(UT_NUM_REQUESTS, sizeof(struct nvme_request));
	SPDK_CU_ASSERT_FATAL(qpair->req_buf != NULL);

	for (i = 0; i < UT_NUM_REQUESTS; i++) {
		struct nvme_request *req = qpair->req_buf + i * sizeof(struct nvme_request);

		req->qpair = qpair;
		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
	}
}

static inline void
ut_qpair_cleanup(struct spdk_nvme_qpair *qpair)
{
	free(qpair->req_buf);
}

#endif /* SPDK_NVME_CMD_UT_COMMON_H */
