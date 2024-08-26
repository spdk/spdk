/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/nvme_ocssd.h"
#include "nvme_internal.h"

bool
spdk_nvme_ctrlr_is_ocssd_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->quirks & NVME_QUIRK_OCSSD) {
		/* TODO: There isn't a standardized way to identify Open-Channel SSD
		 * different verdors may have different conditions.
		 */

		/*
		 * Current QEMU OpenChannel Device needs to check nsdata->vs[0].
		 * Here check nsdata->vs[0] of the first namespace.
		 */
		if (ctrlr->cdata.vid == SPDK_PCI_VID_CNEXLABS) {
			uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
			struct spdk_nvme_ns *ns;

			if (nsid == 0) {
				return false;
			}

			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

			if (ns && ns->nsdata.vendor_specific[0] == 0x1) {
				return true;
			}
		}
	}
	return false;
}


int
spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   void *payload, uint32_t payload_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	if (!payload || (payload_size != sizeof(struct spdk_ocssd_geometry_data))) {
		return -EINVAL;
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size, cb_fn, cb_arg, false);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_OCSSD_OPC_GEOMETRY;
	cmd->nsid = nsid;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}
