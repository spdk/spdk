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

void
nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w)
{
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

#ifndef SPDK_CONFIG_RDMA
void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	SPDK_ERRLOG("spdk_nvmf_rdma_init_hooks() is unsupported: RDMA transport is not available\n");
	abort();
}
#endif /* !SPDK_CONFIG_RDMA */

#ifndef SPDK_CONFIG_AVAHI
int
nvmf_publish_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	SPDK_ERRLOG("nvmf_publish_mdns_prr is supported when built with the --with-avahi option\n");

	return -ENOTSUP;
}

void
nvmf_tgt_stop_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
}

int
nvmf_tgt_update_mdns_prr(struct spdk_nvmf_tgt *tgt)
{
	return 0;
}
#endif /* !SPDK_CONFIG_AVAHI */
