/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

static inline struct nvme_namespace_data *
_nvme_ns_get_data(struct nvme_namespace *ns)
{
	return &ns->ctrlr->nsdata[ns->id - 1];
}

uint32_t
nvme_ns_get_id(struct nvme_namespace *ns)
{
	return ns->id;
}

uint32_t
nvme_ns_get_max_io_xfer_size(struct nvme_namespace *ns)
{
	return ns->ctrlr->max_xfer_size;
}

uint32_t
nvme_ns_get_sector_size(struct nvme_namespace *ns)
{
	return ns->sector_size;
}

uint64_t
nvme_ns_get_num_sectors(struct nvme_namespace *ns)
{
	return _nvme_ns_get_data(ns)->nsze;
}

uint64_t
nvme_ns_get_size(struct nvme_namespace *ns)
{
	return nvme_ns_get_num_sectors(ns) * nvme_ns_get_sector_size(ns);
}

uint32_t
nvme_ns_get_flags(struct nvme_namespace *ns)
{
	return ns->flags;
}

const struct nvme_namespace_data *
nvme_ns_get_data(struct nvme_namespace *ns)
{
	return _nvme_ns_get_data(ns);
}

int
nvme_ns_construct(struct nvme_namespace *ns, uint16_t id,
		  struct nvme_controller *ctrlr)
{
	struct nvme_completion_poll_status	status;
	struct nvme_namespace_data		*nsdata;
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
	if (nvme_completion_is_error(&status.cpl)) {
		nvme_printf(ctrlr, "nvme_identify_namespace failed\n");
		return ENXIO;
	}

	ns->sector_size = 1 << nsdata->lbaf[nsdata->flbas.format].lbads;

	ns->sectors_per_max_io = nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	ns->sectors_per_stripe = ns->stripe_size / ns->sector_size;

	if (ctrlr->cdata.oncs.dsm) {
		ns->flags |= NVME_NS_DEALLOCATE_SUPPORTED;
	}

	if (ctrlr->cdata.vwc.present) {
		ns->flags |= NVME_NS_FLUSH_SUPPORTED;
	}

	return 0;
}

void nvme_ns_destruct(struct nvme_namespace *ns)
{

}
