/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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

/**
 * Update Namespace flags based on Identify Controller
 * and Identify Namespace.  This can be also used for
 * Namespace Attribute Notice events and Namespace
 * operations such as Attach/Detach.
 */
void
nvme_ns_set_identify_data(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ns_data	*nsdata;

	nsdata = _nvme_ns_get_data(ns);

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
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "ns %u optimal IO boundary %" PRIu32 " blocks\n",
			      ns->id, ns->sectors_per_stripe);
	} else if (ns->ctrlr->quirks & NVME_INTEL_QUIRK_STRIPING &&
		   ns->ctrlr->cdata.vs[3] != 0) {
		ns->sectors_per_stripe = (1ULL << ns->ctrlr->cdata.vs[3]) * ns->ctrlr->min_page_size /
					 ns->sector_size;
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "ns %u stripe size quirk %" PRIu32 " blocks\n",
			      ns->id, ns->sectors_per_stripe);
	} else {
		ns->sectors_per_stripe = 0;
	}

	if (ns->ctrlr->cdata.oncs.dsm) {
		ns->flags |= SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	}

	if (ns->ctrlr->cdata.oncs.compare) {
		ns->flags |= SPDK_NVME_NS_COMPARE_SUPPORTED;
	}

	if (ns->ctrlr->cdata.vwc.present) {
		ns->flags |= SPDK_NVME_NS_FLUSH_SUPPORTED;
	}

	if (ns->ctrlr->cdata.oncs.write_zeroes) {
		ns->flags |= SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED;
	}

	if (ns->ctrlr->cdata.oncs.write_unc) {
		ns->flags |= SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED;
	}

	if (nsdata->nsrescap.raw) {
		ns->flags |= SPDK_NVME_NS_RESERVATION_SUPPORTED;
	}

	ns->pi_type = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
	if (nsdata->lbaf[nsdata->flbas.format].ms && nsdata->dps.pit) {
		ns->flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;
		ns->pi_type = nsdata->dps.pit;
	}
}

static int
nvme_ctrlr_identify_ns(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ns_data		*nsdata;
	int					rc;

	status = malloc(sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	nsdata = _nvme_ns_get_data(ns);
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id,
				     nsdata, sizeof(*nsdata),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	if (spdk_nvme_wait_for_completion_robust_lock(ns->ctrlr->adminq, status,
			&ns->ctrlr->ctrlr_lock)) {
		if (!status->timed_out) {
			free(status);
		}
		/* This can occur if the namespace is not active. Simply zero the
		 * namespace data and continue. */
		nvme_ns_destruct(ns);
		return 0;
	}
	free(status);

	nvme_ns_set_identify_data(ns);

	return 0;
}

static int
nvme_ctrlr_identify_id_desc(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status      *status;
	int                                     rc;

	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	if (ns->ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) ||
	    (ns->ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		return 0;
	}

	status = malloc(sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Attempting to retrieve NS ID Descriptor List\n");
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST, 0, ns->id,
				     ns->id_desc_list, sizeof(ns->id_desc_list),
				     nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return rc;
	}

	rc = spdk_nvme_wait_for_completion_robust_lock(ns->ctrlr->adminq, status, &ns->ctrlr->ctrlr_lock);
	if (rc != 0) {
		SPDK_WARNLOG("Failed to retrieve NS ID Descriptor List\n");
		memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));
	}

	if (!status->timed_out) {
		free(status);
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
	const struct spdk_nvme_ns_data *nsdata = NULL;

	/*
	 * According to the spec, valid NS has non-zero id.
	 */
	if (ns->id == 0) {
		return false;
	}

	nsdata = _nvme_ns_get_data(ns);

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

uint32_t
spdk_nvme_ns_get_extended_sector_size(struct spdk_nvme_ns *ns)
{
	return ns->extended_lba_size;
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

bool
spdk_nvme_ns_supports_compare(struct spdk_nvme_ns *ns)
{
	return (ns->flags & SPDK_NVME_NS_COMPARE_SUPPORTED) ? true : false;
}

uint32_t
spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns)
{
	return ns->md_size;
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
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

static const void *
_spdk_nvme_ns_find_id_desc(const struct spdk_nvme_ns *ns, enum spdk_nvme_nidt type, size_t *length)
{
	const struct spdk_nvme_ns_id_desc *desc;
	size_t offset;

	offset = 0;
	while (offset + 4 < sizeof(ns->id_desc_list)) {
		desc = (const struct spdk_nvme_ns_id_desc *)&ns->id_desc_list[offset];

		if (desc->nidl == 0) {
			/* End of list */
			return NULL;
		}

		/*
		 * Check if this descriptor fits within the list.
		 * 4 is the fixed-size descriptor header (not counted in NIDL).
		 */
		if (offset + desc->nidl + 4 > sizeof(ns->id_desc_list)) {
			/* Descriptor longer than remaining space in list (invalid) */
			return NULL;
		}

		if (desc->nidt == type) {
			*length = desc->nidl;
			return &desc->nid[0];
		}

		offset += 4 + desc->nidl;
	}

	return NULL;
}

const struct spdk_uuid *
spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns)
{
	const struct spdk_uuid *uuid;
	size_t uuid_size;

	uuid = _spdk_nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_UUID, &uuid_size);
	if (uuid == NULL || uuid_size != sizeof(*uuid)) {
		return NULL;
	}

	return uuid;
}

int nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
		      struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	assert(id > 0);

	ns->ctrlr = ctrlr;
	ns->id = id;

	rc = nvme_ctrlr_identify_ns(ns);
	if (rc != 0) {
		return rc;
	}

	return nvme_ctrlr_identify_id_desc(ns);
}

void nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ns_data *nsdata;

	if (!ns->id) {
		return;
	}

	nsdata = _nvme_ns_get_data(ns);
	memset(nsdata, 0, sizeof(*nsdata));
	ns->sector_size = 0;
	ns->extended_lba_size = 0;
	ns->md_size = 0;
	ns->pi_type = 0;
	ns->sectors_per_max_io = 0;
	ns->sectors_per_stripe = 0;
	ns->flags = 0;
}
