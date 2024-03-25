/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation
 */

#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/nvmf_transport.h"

#include "nvmf_internal.h"

#ifndef SPDK_CONFIG_HAVE_EVP_MAC
int
nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair)
{
	return -ENOTSUP;
}

void
nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair)
{
	assert(qpair->auth == NULL);
}

int
nvmf_auth_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *cpl = &req->rsp->nvme_cpl;

	cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	cpl->status.sc = SPDK_NVME_SC_INVALID_OPCODE;

	spdk_nvmf_request_complete(req);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

bool
nvmf_auth_is_supported(void)
{
	return false;
}

SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)
#endif /* !SPDK_CONFIG_HAVE_EVP_MAC */
