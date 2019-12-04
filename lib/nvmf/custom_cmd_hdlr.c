/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

/*
 * This is an example handler for modifying parts of the NVMF identify
 * ctlr/namespace response based on the underlying NVMe device.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk_internal/nvmf.h"
#include "spdk_internal/log.h"

static bool
filter_bdev_by_product_name_matches(struct spdk_nvmf_request *req, const char *product_name)
{
	struct spdk_nvmf_subsystem *subsys;
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;

	subsys = spdk_nvmf_request_get_subsystem(req);
	ns = spdk_nvmf_subsystem_get_first_ns(subsys);
	if (!ns) {
		return 0;
	}

	bdev = spdk_nvmf_ns_get_bdev(ns);
	if (!bdev) {
		return false;
	}

	/* This is a non-performant check and should be replaced with a better approach */
	if (strcmp(spdk_bdev_get_product_name(bdev), product_name) != 0) {
		return false;
	}

	return true;
}

static void
fixup_identify_ctrlr(struct spdk_nvmf_request *req)
{
	uint32_t length;
	int rc;
	struct spdk_nvme_ctrlr_data *nvme_cdata;
	struct spdk_nvme_ctrlr_data nvmf_cdata = {};
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);

	/* This is the identify data from the NVMe drive */
	spdk_nvmf_request_get_data(req, (void **)&nvme_cdata, &length);

	/* Get the NVMF identify data */
	rc = spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, &nvmf_cdata);
	if (rc != SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return;
	}

	/* Fixup NVMF identify data with NVMe identify data */

	/* PCI Vendor ID (VID) */
	nvmf_cdata.vid = nvme_cdata->vid;
	/* PCI Subsystem Vendor ID (SSVID) */
	nvmf_cdata.ssvid = nvme_cdata->ssvid;
	/* Serial Number (SN) */
	memcpy(&nvmf_cdata.sn[0], &nvme_cdata->sn[0], sizeof(nvmf_cdata.sn));
	/* Model Number (MN) */
	memcpy(&nvmf_cdata.mn[0], &nvme_cdata->mn[0], sizeof(nvmf_cdata.mn));
	/* Firmware Revision (FR) */
	memcpy(&nvmf_cdata.fr[0], &nvme_cdata->fr[0], sizeof(nvmf_cdata.fr));
	/* IEEE OUI Identifier (IEEE) */
	memcpy(&nvmf_cdata.ieee[0], &nvme_cdata->ieee[0], sizeof(nvmf_cdata.ieee));
	/* FRU Globally Unique Identifier (FGUID) */
	memcpy(&nvmf_cdata.fguid[0], &nvme_cdata->fguid[0], sizeof(nvmf_cdata.fguid));
	/* Optional Admin Command Support (OACS) */
	memcpy(&nvmf_cdata.oacs, &nvme_cdata->oacs, sizeof(nvmf_cdata.oacs));
	/* Firmware Updates (FRMW) */
	nvmf_cdata.frmw = nvme_cdata->frmw;
	/* Maximum Time for Firmware Activation (MTFA) */
	nvmf_cdata.mtfa = nvme_cdata->mtfa;
	/* Firmware Update Granularity (FWUG) */
	nvmf_cdata.fwug = nvme_cdata->fwug;
	/* Optional NVM Command Support (ONCS) */
	nvmf_cdata.oncs = nvme_cdata->oncs;
	/* Format NVM Attributes (FNA) */
	nvmf_cdata.fna = nvme_cdata->fna;

	/* Copy the fixed up data back to the response */
	memcpy(nvme_cdata, &nvmf_cdata, length);
}

static void
fixup_identify_ns(struct spdk_nvmf_request *req)
{
	uint32_t length;
	struct spdk_nvme_ns_data nvmf_nsdata = {};
	struct spdk_nvme_ns_data *nvme_nsdata;
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);

	spdk_nvmf_request_get_data(req, (void **)&nvme_nsdata, &length);

	int rc = spdk_nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nvmf_nsdata);
	if (rc != SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return;
	};

	/* fixup */
	nvmf_nsdata.flbas = nvme_nsdata->flbas;
	nvmf_nsdata.nlbaf = nvme_nsdata->nlbaf;
	memcpy(&nvmf_nsdata.lbaf[0], nvme_nsdata->lbaf, sizeof(nvmf_nsdata.lbaf));

	memcpy(nvme_nsdata, &nvmf_nsdata, length);
}

int
spdk_nvmf_custom_identify_hdlr(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);

	uint8_t cns = cmd->cdw10 & 0xFF;
	if (cns != SPDK_NVME_IDENTIFY_CTRLR && cns != SPDK_NVME_IDENTIFY_NS) {
		return -1; /* continue */
	}

	/* We only do a special identify for NVMe disk devices */
	if (!filter_bdev_by_product_name_matches(req, "NVMe disk")) {
		return -1; /* continue */
	}

	/* Forward to first namespace */
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	int rc = spdk_nvmf_request_get_bdev(1, req, &bdev, &desc, &ch);
	if (rc) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return spdk_nvmf_bdev_nvme_passthru_admin(bdev, desc, ch, req,
			cns == SPDK_NVME_IDENTIFY_CTRLR ? fixup_identify_ctrlr : fixup_identify_ns);
}
