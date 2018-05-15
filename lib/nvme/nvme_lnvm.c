/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "nvme_internal.h"

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/nvme_lnvm.h"

bool
spdk_nvme_ctrlr_is_lightnvm_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->quirks & NVME_QUIRK_LIGHTNVM) {
		// TODO: There isn't a standardized way to identify Open-Channel SSD
		// different verdors may have different conditions.

		/*
		 * Current QEMU LightNVM Device needs to check nsdata->vs[0].
		 * Here check nsdata->vs[0] of the first namespace.
		 */
		if (ctrlr->num_ns && ctrlr->nsdata[0].vendor_specific[0] == 0x1) {
			return true;
		}
	}
	return false;
}

int
spdk_nvme_lnvm_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			    void *payload, uint32_t payload_size,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size,
					      cb_fn, cb_arg, false);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GEOMETRY;
	cmd->nsid = nsid;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}
