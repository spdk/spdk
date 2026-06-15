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

static inline struct spdk_nvme_ns *
ut_ns_alloc(uint32_t id, struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns;

	ns = calloc(1, sizeof(*ns) + nvme_ctrlr_get_nsdata_size(ctrlr));
	SPDK_CU_ASSERT_FATAL(ns != NULL);
	ns->id = id;
	ns->ctrlr = ctrlr;
	return ns;
}

static inline void
ut_ns_free(struct spdk_nvme_ns *ns)
{
	free(ns);
}

static inline void
ut_req_cleanup(struct nvme_request **req)
{
	if (*req != NULL) {
		nvme_free_request(*req);
		*req = NULL;
	}
}

static inline void
ut_user_req_cleanup(struct nvme_request **req)
{
	if (*req != NULL) {
		nvme_cleanup_user_req(*req);
	}
	ut_req_cleanup(req);
}

#endif /* SPDK_NVME_CMD_UT_COMMON_H */
