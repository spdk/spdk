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

static
int nvme_ns_identify_update(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status	status;
	struct spdk_nvme_ns_data		*nsdata;
	int					rc;

	nsdata = _nvme_ns_get_data(ns);
	status.done = false;
	rc = nvme_ctrlr_cmd_identify_namespace(ns->ctrlr, ns->id, nsdata,
					       nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		nvme_robust_mutex_lock(&ns->ctrlr->ctrlr_lock);
		spdk_nvme_qpair_process_completions(ns->ctrlr->adminq, 0);
		nvme_robust_mutex_unlock(&ns->ctrlr->ctrlr_lock);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		/* This can occur if the namespace is not active. Simply zero the
		 * namespace data and continue. */
		memset(nsdata, 0, sizeof(*nsdata));
		ns->sector_size = 0;
		ns->extended_lba_size = 0;
		ns->md_size = 0;
		ns->pi_type = 0;
		ns->sectors_per_max_io = 0;
		ns->sectors_per_stripe = 0;
		ns->flags = 0;
		return 0;
	}

	ns->flags = 0x0000;

	ns->sector_size = 1 << nsdata->lbaf[nsdata->flbas.format].lbads;
	ns->extended_lba_size = ns->sector_size;

	ns->md_size = nsdata->lbaf[nsdata->flbas.format].ms;
	if (nsdata->flbas.extended) {
		ns->flags |= SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED;
		ns->extended_lba_size += ns->md_size;
	}

	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->extended_lba_size;

	if (nsdata->noiob) {
		ns->sectors_per_stripe = nsdata->noiob;
		SPDK_DEBUGLOG(SPDK_TRACE_NVME, "ns %u optimal IO boundary %" PRIu32 " blocks\n",
			      ns->id, ns->sectors_per_stripe);
	} else if (ns->ctrlr->quirks & NVME_INTEL_QUIRK_STRIPING &&
		   ns->ctrlr->cdata.vs[3] != 0) {
		ns->sectors_per_stripe = (1ULL << ns->ctrlr->cdata.vs[3]) * ns->ctrlr->min_page_size /
					 ns->sector_size;
		SPDK_DEBUGLOG(SPDK_TRACE_NVME, "ns %u stripe size quirk %" PRIu32 " blocks\n",
			      ns->id, ns->sectors_per_stripe);
	} else {
		ns->sectors_per_stripe = 0;
	}

	if (ns->ctrlr->cdata.oncs.dsm) {
		ns->flags |= SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	}

	if (ns->ctrlr->cdata.vwc.present) {
		ns->flags |= SPDK_NVME_NS_FLUSH_SUPPORTED;
	}

	if (ns->ctrlr->cdata.oncs.write_zeroes) {
		ns->flags |= SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED;
	}

	if (nsdata->nsrescap.raw) {
		ns->flags |= SPDK_NVME_NS_RESERVATION_SUPPORTED;
	}

	ns->pi_type = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
	if (nsdata->lbaf[nsdata->flbas.format].ms && nsdata->dps.pit) {
		ns->flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;
		ns->pi_type = nsdata->dps.pit;
	}
	return rc;
}

uint32_t
spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns)
{
	return ns->id;
}

bool
spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data *nsdata = _nvme_ns_get_data(ns);

	/*
	 * According to the spec, Identify Namespace will return a zero-filled structure for
	 *  inactive namespace IDs.
	 * Check NCAP since it must be nonzero for an active namespace.
	 */
	return nsdata->ncap != 0;
}

struct spdk_nvme_ctrlr *
spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr;
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

enum spdk_nvme_pi_type
spdk_nvme_ns_get_pi_type(struct spdk_nvme_ns *ns) {
	return ns->pi_type;
}

bool
spdk_nvme_ns_supports_extended_lba(struct spdk_nvme_ns *ns)
{
	return (ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) ? true : false;
}

uint32_t
spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns)
{
	return ns->md_size;
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	nvme_ns_identify_update(ns);
	return _nvme_ns_get_data(ns);
}

enum spdk_nvme_dealloc_logical_block_read_value spdk_nvme_ns_get_dealloc_logical_block_read_value(
	struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	const struct spdk_nvme_ns_data *data = spdk_nvme_ns_get_data(ns);

	if (ctrlr->quirks & NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE) {
		return SPDK_NVME_DEALLOC_READ_00;
	} else {
		return data->dlfeat.bits.read_value;
	}
}

uint32_t
spdk_nvme_ns_get_optimal_io_boundary(struct spdk_nvme_ns *ns)
{
	return ns->sectors_per_stripe;
}

int nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
		      struct spdk_nvme_ctrlr *ctrlr)
{
	assert(id > 0);

	ns->ctrlr = ctrlr;
	ns->id = id;

	return nvme_ns_identify_update(ns);
}

void nvme_ns_destruct(struct spdk_nvme_ns *ns)
{

}
