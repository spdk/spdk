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

#include "nvme_internal.h"

static inline struct spdk_nvme_ns_data *
_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return &ns->ctrlr->nsdata[ns->id - 1];
}

uint32_t
spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns)
{
	return ns->id;
}

uint32_t
spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr->max_xfer_size;
}

uint32_t
spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns)
{
	return ns->sector_size;
}

uint64_t
spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns)
{
	return _nvme_ns_get_data(ns)->nsze;
}

uint64_t
spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns)
{
	return spdk_nvme_ns_get_num_sectors(ns) * spdk_nvme_ns_get_sector_size(ns);
}

uint32_t
spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns)
{
	return ns->flags;
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return _nvme_ns_get_data(ns);
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_completion_poll_status	status;
	struct spdk_nvme_ns_data		*nsdata;
	uint32_t				pci_devid;

	nvme_assert(id > 0, ("invalid namespace id %d", id));

	ns->ctrlr = ctrlr;
	ns->id = id;
	ns->stripe_size = 0;

	nvme_pcicfg_read32(ctrlr->devhandle, &pci_devid, 0);
	if (pci_devid == INTEL_DC_P3X00_DEVID && ctrlr->cdata.vs[3] != 0) {
		ns->stripe_size = (1 << ctrlr->cdata.vs[3]) * ctrlr->min_page_size;
	}

	nsdata = _nvme_ns_get_data(ns);

	status.done = false;
	nvme_ctrlr_cmd_identify_namespace(ctrlr, id, nsdata,
					  nvme_completion_poll_cb, &status);
	while (status.done == false) {
		nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_identify_namespace failed\n");
		return ENXIO;
	}

	ns->sector_size = 1 << nsdata->lbaf[nsdata->flbas.format].lbads;

	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	ns->sectors_per_stripe = ns->stripe_size / ns->sector_size;

	if (ctrlr->cdata.oncs.dsm) {
		ns->flags |= SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	}

	if (ctrlr->cdata.vwc.present) {
		ns->flags |= SPDK_NVME_NS_FLUSH_SUPPORTED;
	}

	if (ctrlr->cdata.oncs.write_zeroes) {
		ns->flags |= SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED;
	}

	if (nsdata->nsrescap.raw) {
		ns->flags |= SPDK_NVME_NS_RESERVATION_SUPPORTED;
	}

	return 0;
}

void nvme_ns_destruct(struct spdk_nvme_ns *ns)
{

}
