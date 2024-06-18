/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/env_dpdk.h"
#include <rte_config.h>
#include <rte_eal.h>

#define MAX_DEVS 64

struct dev {
	struct spdk_nvme_ctrlr				*ctrlr;
	struct spdk_nvme_ns				*ns;
	struct spdk_nvme_qpair				*qpair;
	char						name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static struct dev g_nvme_devs[MAX_DEVS];
static int g_num_devs = 0;
static int g_failed = 0;

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;
	uint32_t nsid;

	/* add to dev list */
	dev = &g_nvme_devs[g_num_devs++];
	if (g_num_devs >= MAX_DEVS) {
		return;
	}

	dev->ctrlr = ctrlr;
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (dev->qpair == NULL) {
		g_failed = 1;
		return;
	}

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

int
main(int argc, char **argv)
{
	int ret;
	int i;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	printf("Starting DPDK initialization...\n");
	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DPDK\n");
		return -1;
	}

	printf("Starting SPDK post initialization...\n");
	ret = spdk_env_dpdk_post_init(false);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize SPDK\n");
		return -1;
	}

	printf("SPDK NVMe probe\n");
	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	printf("Cleaning up...\n");
	for (i = 0; i < g_num_devs; i++) {
		struct dev *dev = &g_nvme_devs[i];
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	return g_failed;
}
