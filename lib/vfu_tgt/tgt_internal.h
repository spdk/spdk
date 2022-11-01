/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef _TGT_INTERNAL_H
#define _TGT_INTERNAL_H

#include "spdk/vfu_target.h"

struct spdk_vfu_endpoint {
	char				name[SPDK_VFU_MAX_NAME_LEN];
	char				uuid[PATH_MAX];

	struct spdk_vfu_endpoint_ops	ops;

	vfu_ctx_t			*vfu_ctx;
	void				*endpoint_ctx;

	struct spdk_poller		*accept_poller;
	struct spdk_poller		*vfu_ctx_poller;
	bool				is_attached;

	struct msixcap			*msix;
	vfu_pci_config_space_t		*pci_config_space;

	struct spdk_thread		*thread;

	TAILQ_ENTRY(spdk_vfu_endpoint)	link;
};

#endif
