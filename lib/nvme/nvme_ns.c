/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "nvme_internal.h"

static inline struct spdk_nvme_ns_data *
_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return &ns->nsdata;
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
	struct spdk_nvme_nvm_ns_data	*nsdata_nvm;
	uint32_t			format_index;

	nsdata = _nvme_ns_get_data(ns);
	nsdata_nvm = ns->nsdata_nvm;

	ns->flags = 0x0000;
	format_index = spdk_nvme_ns_get_format_index(nsdata);

	ns->sector_size = 1 << nsdata->lbaf[format_index].lbads;
	ns->extended_lba_size = ns->sector_size;

	ns->md_size = nsdata->lbaf[format_index].ms;
	if (nsdata->flbas.extended) {
		ns->flags |= SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED;
		ns->extended_lba_size += ns->md_size;
	}

	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->extended_lba_size;
	ns->sectors_per_max_io_no_md = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	if (ns->ctrlr->quirks & NVME_QUIRK_MDTS_EXCLUDE_MD) {
		ns->sectors_per_max_io = ns->sectors_per_max_io_no_md;
	}

	if (nsdata->noiob) {
		ns->sectors_per_stripe = nsdata->noiob;
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "ns %u optimal IO boundary %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
	} else if (ns->ctrlr->quirks & NVME_INTEL_QUIRK_STRIPING &&
		   ns->ctrlr->cdata.vs[3] != 0) {
		ns->sectors_per_stripe = (1ULL << ns->ctrlr->cdata.vs[3]) * ns->ctrlr->min_page_size /
					 ns->sector_size;
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "ns %u stripe size quirk %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
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
	if (nsdata->lbaf[format_index].ms && nsdata->dps.pit) {
		ns->flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;
		ns->pi_type = nsdata->dps.pit;
		if (nsdata_nvm != NULL && ns->ctrlr->cdata.ctratt.bits.elbas) {
			/* We may have nsdata_nvm for other purposes but
			 * the elbaf array is only valid when elbas is 1.
			 */
			ns->pi_format = nsdata_nvm->elbaf[format_index].pif;
		} else {
			ns->pi_format = SPDK_NVME_16B_GUARD_PI;
		}
	}

	ns->active = spdk_nvme_ns_is_active(ns);
}

static int
nvme_ctrlr_identify_ns(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ns_data		*nsdata;
	int					rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ns->ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	nsdata = _nvme_ns_get_data(ns);
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				     nsdata, sizeof(*nsdata),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ns->ctrlr, status, true);
	if (rc) {
		/* This can occur if the namespace is not active. Simply zero the
		 * namespace data and continue. */
		SPDK_ERRLOG("wait for nvme_ctrlr_cmd_identify failed: rc=%s\n", spdk_strerror(abs(rc)));
		nvme_ns_destruct(ns);
		return 0;
	}

	nvme_ns_set_identify_data(ns);
	return 0;
}

static int
nvme_ctrlr_identify_ns_zns_specific(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_zns_ns_data *nsdata_zns;
	int rc;

	nvme_ns_free_zns_specific_data(ns);

	nsdata_zns = spdk_zmalloc(sizeof(*nsdata_zns), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				  SPDK_MALLOC_SHARE);
	if (!nsdata_zns) {
		return -ENOMEM;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		spdk_free(nsdata_zns);
		return -ENOMEM;
	}

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     nsdata_zns, sizeof(*nsdata_zns),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		spdk_free(nsdata_zns);
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: %s\n", spdk_strerror(abs(rc)));
		spdk_free(nsdata_zns);
		return -ENXIO;
	}

	ns->nsdata_zns = nsdata_zns;
	return 0;
}

static int
nvme_ctrlr_identify_ns_nvm_specific(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_nvm_ns_data *nsdata_nvm;
	int rc;

	nvme_ns_free_zns_specific_data(ns);

	nsdata_nvm = spdk_zmalloc(sizeof(*nsdata_nvm), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				  SPDK_MALLOC_SHARE);
	if (!nsdata_nvm) {
		return -ENOMEM;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		spdk_free(nsdata_nvm);
		return -ENOMEM;
	}

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     nsdata_nvm, sizeof(*nsdata_nvm),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		spdk_free(nsdata_nvm);
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		spdk_free(nsdata_nvm);
		return -ENXIO;
	}

	ns->nsdata_nvm = nsdata_nvm;
	return 0;
}

static int
nvme_ctrlr_identify_ns_iocs_specific(struct spdk_nvme_ns *ns)
{
	switch (ns->csi) {
	case SPDK_NVME_CSI_ZNS:
		return nvme_ctrlr_identify_ns_zns_specific(ns);
	case SPDK_NVME_CSI_NVM:
		if (ns->ctrlr->cdata.ctratt.bits.elbas) {
			return nvme_ctrlr_identify_ns_nvm_specific(ns);
		}
	/* fallthrough */
	default:
		/*
		 * This switch must handle all cases for which
		 * nvme_ns_has_supported_iocs_specific_data() returns true,
		 * other cases should never happen.
		 */
		assert(0);
	}

	return -EINVAL;
}

static int
nvme_ctrlr_identify_id_desc(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status      *status;
	int                                     rc;

	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	if ((ns->ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) &&
	     !(ns->ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS)) ||
	    (ns->ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		NVME_CTRLR_DEBUGLOG(ns->ctrlr, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		return 0;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ns->ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	NVME_CTRLR_DEBUGLOG(ns->ctrlr, "Attempting to retrieve NS ID Descriptor List\n");
	rc = nvme_ctrlr_cmd_identify(ns->ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST, 0, ns->id,
				     0, ns->id_desc_list, sizeof(ns->id_desc_list),
				     nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ns->ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Failed to retrieve NS ID Descriptor List\n");
		memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));
	}

	nvme_ns_set_id_desc_list_data(ns);
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

enum spdk_nvme_pi_format
spdk_nvme_ns_get_pi_format(struct spdk_nvme_ns *ns) {
	return ns->pi_format;
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

uint32_t
spdk_nvme_ns_get_format_index(const struct spdk_nvme_ns_data *nsdata)
{
	if (nsdata->nlbaf < 16) {
		return nsdata->flbas.format;
	} else {
		return ((nsdata->flbas.msb_format << 4) + nsdata->flbas.format);
	}
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return _nvme_ns_get_data(ns);
}

const struct spdk_nvme_nvm_ns_data *
spdk_nvme_nvm_ns_get_data(struct spdk_nvme_ns *ns)
{
	return ns->nsdata_nvm;
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvme_dealloc_logical_block_read_value
spdk_nvme_dealloc_logical_block_read_value_t;

spdk_nvme_dealloc_logical_block_read_value_t
spdk_nvme_ns_get_dealloc_logical_block_read_value(
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
nvme_ns_find_id_desc(const struct spdk_nvme_ns *ns, enum spdk_nvme_nidt type, size_t *length)
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

const uint8_t *
spdk_nvme_ns_get_nguid(const struct spdk_nvme_ns *ns)
{
	const uint8_t *nguid;
	size_t size;

	nguid = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_NGUID, &size);
	if (nguid && size != SPDK_SIZEOF_MEMBER(struct spdk_nvme_ns_data, nguid)) {
		NVME_CTRLR_WARNLOG(ns->ctrlr,
				   "Invalid NIDT_NGUID descriptor length reported: %zu (expected: %zu)\n",
				   size, SPDK_SIZEOF_MEMBER(struct spdk_nvme_ns_data, nguid));
		return NULL;
	}

	return nguid;
}

const struct spdk_uuid *
spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns)
{
	const struct spdk_uuid *uuid;
	size_t uuid_size;

	uuid = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_UUID, &uuid_size);
	if (uuid && uuid_size != sizeof(*uuid)) {
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Invalid NIDT_UUID descriptor length reported: %zu (expected: %zu)\n",
				   uuid_size, sizeof(*uuid));
		return NULL;
	}

	return uuid;
}

static enum spdk_nvme_csi
nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	const uint8_t *csi;
	size_t csi_size;

	csi = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_CSI, &csi_size);
	if (csi && csi_size != sizeof(*csi))
	{
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Invalid NIDT_CSI descriptor length reported: %zu (expected: %zu)\n",
				   csi_size, sizeof(*csi));
		return SPDK_NVME_CSI_NVM;
	}
	if (!csi)
	{
		if (ns->ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS) {
			NVME_CTRLR_WARNLOG(ns->ctrlr, "CSI not reported for NSID: %" PRIu32 "\n", ns->id);
		}
		return SPDK_NVME_CSI_NVM;
	}

	return *csi;
}

void
nvme_ns_set_id_desc_list_data(struct spdk_nvme_ns *ns)
{
	ns->csi = nvme_ns_get_csi(ns);
}

enum spdk_nvme_csi
spdk_nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	return ns->csi;
}

void
nvme_ns_free_zns_specific_data(struct spdk_nvme_ns *ns)
{
	if (!ns->id) {
		return;
	}

	if (ns->nsdata_zns) {
		spdk_free(ns->nsdata_zns);
		ns->nsdata_zns = NULL;
	}
}

void
nvme_ns_free_nvm_specific_data(struct spdk_nvme_ns *ns)
{
	if (!ns->id) {
		return;
	}

	if (ns->nsdata_nvm) {
		spdk_free(ns->nsdata_nvm);
		ns->nsdata_nvm = NULL;
	}
}

void
nvme_ns_free_iocs_specific_data(struct spdk_nvme_ns *ns)
{
	nvme_ns_free_zns_specific_data(ns);
	nvme_ns_free_nvm_specific_data(ns);
}

bool
nvme_ns_has_supported_iocs_specific_data(struct spdk_nvme_ns *ns)
{
	switch (ns->csi) {
	case SPDK_NVME_CSI_NVM:
		if (ns->ctrlr->cdata.ctratt.bits.elbas) {
			return true;
		}

		return false;
	case SPDK_NVME_CSI_ZNS:
		return true;
	default:
		NVME_CTRLR_WARNLOG(ns->ctrlr, "Unsupported CSI: %u for NSID: %u\n", ns->csi, ns->id);
		return false;
	}
}

uint32_t
spdk_nvme_ns_get_ana_group_id(const struct spdk_nvme_ns *ns)
{
	return ns->ana_group_id;
}

enum spdk_nvme_ana_state
spdk_nvme_ns_get_ana_state(const struct spdk_nvme_ns *ns) {
	return ns->ana_state;
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	int	rc;

	assert(id > 0);

	ns->ctrlr = ctrlr;
	ns->id = id;
	/* This will be overwritten when reading ANA log page. */
	ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	rc = nvme_ctrlr_identify_ns(ns);
	if (rc != 0) {
		return rc;
	}

	/* skip Identify NS ID Descriptor List for inactive NS */
	if (!spdk_nvme_ns_is_active(ns)) {
		return 0;
	}

	rc = nvme_ctrlr_identify_id_desc(ns);
	if (rc != 0) {
		return rc;
	}

	if (nvme_ctrlr_multi_iocs_enabled(ctrlr) &&
	    nvme_ns_has_supported_iocs_specific_data(ns)) {
		rc = nvme_ctrlr_identify_ns_iocs_specific(ns);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ns_data *nsdata;

	if (!ns->id) {
		return;
	}

	nsdata = _nvme_ns_get_data(ns);
	memset(nsdata, 0, sizeof(*nsdata));
	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));
	nvme_ns_free_iocs_specific_data(ns);
	ns->sector_size = 0;
	ns->extended_lba_size = 0;
	ns->md_size = 0;
	ns->pi_type = 0;
	ns->sectors_per_max_io = 0;
	ns->sectors_per_max_io_no_md = 0;
	ns->sectors_per_stripe = 0;
	ns->flags = 0;
	ns->csi = SPDK_NVME_CSI_NVM;
}
