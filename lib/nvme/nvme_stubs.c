/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/stdinc.h"
#include "nvme_internal.h"

#ifndef SPDK_CONFIG_NVME_CUSE
int
spdk_nvme_cuse_get_ctrlr_name(struct spdk_nvme_ctrlr *ctrlr, char *name, size_t *size)
{
	SPDK_ERRLOG("spdk_nvme_cuse_get_ctrlr_name() is unsupported\n");
	return -ENOTSUP;
}

int
spdk_nvme_cuse_get_ns_name(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, char *name, size_t *size)
{
	SPDK_ERRLOG("spdk_nvme_cuse_get_ns_name() is unsupported\n");
	return -ENOTSUP;
}

int
spdk_nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_register() is unsupported\n");
	return -ENOTSUP;
}

int
spdk_nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_unregister() is unsupported\n");
	return -ENOTSUP;
}

void
spdk_nvme_cuse_update_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	SPDK_ERRLOG("spdk_nvme_cuse_update_namespaces() is unsupported\n");
}
#endif /* !SPDK_CONFIG_NVME_CUSE */

#ifndef SPDK_CONFIG_RDMA
void
spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	SPDK_ERRLOG("spdk_nvme_rdma_init_hooks() is unsupported: RDMA transport is not available\n");
	abort();
}
#endif /* !SPDK_CONFIG_RDMA */

#ifndef SPDK_CONFIG_HAVE_EVP_MAC
int
nvme_fabric_qpair_authenticate_async(struct spdk_nvme_qpair *qpair)
{
	SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
	return -ENOTSUP;
}

int
nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair)
{
	return -ENOTSUP;
}
#endif /* !SPDK_CONFIG_HAVE_EVP_MAC */
