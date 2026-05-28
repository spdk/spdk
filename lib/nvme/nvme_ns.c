/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "nvme_internal.h"

/**
 * Update Namespace flags based on Identify Controller
 * and Identify Namespace.  This can be also used for
 * Namespace Attribute Notice events and Namespace
 * operations such as Attach/Detach.
 */
void
nvme_ns_set_identify_data(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr		*ctrlr = ns->ctrlr;
	struct spdk_nvme_ns_data	*nsdata = nvme_ns_get_data(ns);
	struct spdk_nvme_nvm_ns_data	*nsdata_nvm;
	struct spdk_nvme_ns_data_lbaf	lbaf;
	uint32_t			format_index;

	ns->identify_pending = false;
	ns->active = spdk_nvme_ns_is_active(ns);
	if (!ns->active) {
		nvme_ns_clear(ns);
		return;
	}

	nsdata_nvm = ns->nsdata_nvm;

	ns->flags = 0x0000;
	format_index = spdk_nvme_ns_get_active_format_index(ns);
	spdk_nvme_ns_get_format(ns, format_index, &lbaf);

	ns->sector_size = 1 << lbaf.lbads;
	ns->extended_lba_size = ns->sector_size;

	ns->md_size = lbaf.ms;
	if (nsdata->flbas.extended) {
		ns->flags |= SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED;
		ns->extended_lba_size += ns->md_size;
	}

	ns->sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->extended_lba_size;
	ns->sectors_per_max_io_no_md = spdk_nvme_ns_get_max_io_xfer_size(ns) / ns->sector_size;
	if (ctrlr->quirks & NVME_QUIRK_MDTS_EXCLUDE_MD) {
		ns->sectors_per_max_io = ns->sectors_per_max_io_no_md;
	}

	if (nsdata->noiob) {
		ns->sectors_per_stripe = nsdata->noiob;
		NVME_CTRLR_DEBUGLOG(ctrlr, "ns %u optimal IO boundary %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
	} else if (ctrlr->quirks & NVME_INTEL_QUIRK_STRIPING && ctrlr->cdata.vs[3] != 0) {
		ns->sectors_per_stripe = (1ULL << ctrlr->cdata.vs[3]) * ctrlr->min_page_size / ns->sector_size;
		NVME_CTRLR_DEBUGLOG(ctrlr, "ns %u stripe size quirk %" PRIu32 " blocks\n", ns->id,
				    ns->sectors_per_stripe);
	} else {
		ns->sectors_per_stripe = 0;
	}

	if (ctrlr->cdata.oncs.nvmdsmsv) {
		ns->flags |= SPDK_NVME_NS_DEALLOCATE_SUPPORTED;
	}

	if (ctrlr->cdata.oncs.nvmcmps) {
		ns->flags |= SPDK_NVME_NS_COMPARE_SUPPORTED;
	}

	if (ctrlr->cdata.vwc.present) {
		ns->flags |= SPDK_NVME_NS_FLUSH_SUPPORTED;
	}

	if (ctrlr->cdata.oncs.nvmwzsv) {
		ns->flags |= SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED;
	}

	if (ctrlr->cdata.oncs.nvmwusv) {
		ns->flags |= SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED;
	}

	if (nsdata->nsrescap.raw) {
		ns->flags |= SPDK_NVME_NS_RESERVATION_SUPPORTED;
	}

	ns->pi_type = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
	if (lbaf.ms && nsdata->dps.pit) {
		ns->flags |= SPDK_NVME_NS_DPS_PI_SUPPORTED;
		ns->pi_type = nsdata->dps.pit;
		if (nsdata_nvm != NULL && ctrlr->cdata.ctratt.elbas) {
			/* We may have nsdata_nvm for other purposes but
			 * the elbaf array is only valid when elbas is 1.
			 */
			ns->pi_format = nsdata_nvm->elbaf[format_index].pif;
		} else {
			ns->pi_format = SPDK_NVME_16B_GUARD_PI;
		}
	}
}

static int
nvme_ctrlr_identify_ns(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status	*status;
	struct spdk_nvme_ctrlr			*ctrlr = ns->ctrlr;
	struct spdk_nvme_ns_data		*nsdata = nvme_ns_get_data(ns);
	int					rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS, 0, ns->id, 0,
				     nsdata, sizeof(*nsdata),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		/* This can occur if the namespace is not active. */
		NVME_CTRLR_WARNLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: rc=%s\n",
				   spdk_strerror(abs(rc)));
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

	nvme_ns_free_nvm_specific_data(ns);

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
nvme_ctrlr_identify_ns_kv_specific(struct spdk_nvme_ns *ns)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	struct spdk_nvme_kv_ns_data *nsdata_kv;
	int rc;

	nvme_ns_free_kv_specific_data(ns);

	nsdata_kv = spdk_zmalloc(sizeof(*nsdata_kv), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				 SPDK_MALLOC_SHARE);
	if (!nsdata_kv) {
		return -ENOMEM;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		spdk_free(nsdata_kv);
		return -ENOMEM;
	}

	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_IOCS, 0, ns->id, ns->csi,
				     nsdata_kv, sizeof(*nsdata_kv),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		spdk_free(nsdata_kv);
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_ctrlr_cmd_identify failed: %s\n", spdk_strerror(abs(rc)));
		spdk_free(nsdata_kv);
		return -ENXIO;
	}

	ns->nsdata_kv = nsdata_kv;
	return 0;
}

static int
nvme_ctrlr_identify_ns_iocs_specific(struct spdk_nvme_ns *ns)
{
	switch (ns->csi) {
	case SPDK_NVME_CSI_ZNS:
		return nvme_ctrlr_identify_ns_zns_specific(ns);
	case SPDK_NVME_CSI_KV:
		return nvme_ctrlr_identify_ns_kv_specific(ns);
	case SPDK_NVME_CSI_NVM:
		if (ns->ctrlr->cdata.ctratt.elbas) {
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
	struct spdk_nvme_ctrlr                  *ctrlr = ns->ctrlr;
	int                                     rc;

	memset(ns->id_desc_list, 0, sizeof(ns->id_desc_list));

	if ((ctrlr->vs.raw < SPDK_NVME_VERSION(1, 3, 0) &&
	     !(ctrlr->cap.bits.css & SPDK_NVME_CAP_CSS_IOCS)) ||
	    (ctrlr->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "Version < 1.3; not attempting to retrieve NS ID Descriptor List\n");
		return 0;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	NVME_CTRLR_DEBUGLOG(ctrlr, "Attempting to retrieve NS ID Descriptor List\n");
	rc = nvme_ctrlr_cmd_identify(ctrlr, SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST, 0, ns->id,
				     0, ns->id_desc_list, sizeof(ns->id_desc_list),
				     nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return rc;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_WARNLOG(ctrlr, "Failed to retrieve NS ID Descriptor List\n");
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
	const struct spdk_nvme_ns_data *nsdata = nvme_ns_get_data(ns);

	/*
	 * According to the spec, valid NS has non-zero id.
	 */
	if (ns->id == 0) {
		return false;
	}

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
	return nvme_ns_get_data(ns)->nsze;
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
spdk_nvme_ns_supports_write_uncorrectable(struct spdk_nvme_ns *ns)
{
	return (ns->flags & SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED) ? true : false;
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

static inline uint32_t
nvme_ns_get_active_format_index(const struct spdk_nvme_ns_data *nsdata)
{
	if (nsdata->nlbaf < 16) {
		return nsdata->flbas.format;
	}

	return (nsdata->flbas.msb_format << 4) + nsdata->flbas.format;
}

uint32_t
spdk_nvme_ns_get_active_format_index(const struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data *nsdata = nvme_ns_get_data((struct spdk_nvme_ns *)ns);

	return nvme_ns_get_active_format_index(nsdata);
}

SPDK_LOG_DEPRECATION_REGISTER(nvme_ns_get_format_index,
			      "use spdk_nvme_ns_get_active_format_index instead",
			      "v27.01", SPDK_LOG_DEPRECATION_EVERY_24H);

uint32_t
spdk_nvme_ns_get_format_index(const struct spdk_nvme_ns_data *nsdata)
{
	SPDK_LOG_DEPRECATED(nvme_ns_get_format_index);

	return nvme_ns_get_active_format_index(nsdata);
}

int
spdk_nvme_ns_get_format(struct spdk_nvme_ns *ns, uint8_t format_index,
			struct spdk_nvme_ns_data_lbaf *lbaf)
{
	const struct spdk_nvme_ns_data *nsdata = nvme_ns_get_data(ns);

	if (!lbaf) {
		return -EINVAL;
	}

	memset(lbaf, 0, sizeof(*lbaf));

	if (format_index > spdk_min(nsdata->nlbaf, SPDK_NVME_NS_MAX_LBA_FORMATS - 1)) {
		return -EINVAL;
	}

	*lbaf = nsdata->lbaf[format_index];
	return 0;
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return nvme_ns_get_data(ns);
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
	const struct spdk_nvme_ns_data *nsdata = nvme_ns_get_data(ns);

	if (ctrlr->quirks & NVME_QUIRK_READ_ZERO_AFTER_DEALLOCATE) {
		return SPDK_NVME_DEALLOC_READ_00;
	} else {
		return nsdata->dlfeat.bits.read_value;
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
	const struct spdk_nvme_ns_data *nsdata = &ns->nsdata;

	if (spdk_mem_all_zero(nsdata->nguid, sizeof(nsdata->nguid))) {
		return NULL;
	}

	return nsdata->nguid;
}

static bool
nvme_ns_check_desc_len(const void *val, size_t val_size, size_t expected_size, const char *label,
		       struct spdk_nvme_ctrlr *ctrlr)
{
	if (val_size != expected_size) {
		NVME_CTRLR_WARNLOG(ctrlr, "Invalid %s descriptor length reported: %zu (expected: %zu)\n", label,
				   val_size, expected_size);
		return false;
	}

	return true;
}

static void
nvme_ns_backfill_nsdata(const void *val, void *dst, size_t dst_size, const char *label,
			struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_mem_all_zero(dst, dst_size)) {
		NVME_CTRLR_DEBUGLOG(ctrlr, "%s not in Identify NS data; using descriptor list value\n", label);
		memcpy(dst, val, dst_size);
	} else if (memcmp(val, dst, dst_size) != 0) {
		NVME_CTRLR_WARNLOG(ctrlr,
				   "%s descriptor differs from Identify NS data; using Identify NS value\n", label);
	}
}

const struct spdk_uuid *
spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns)
{
	const struct spdk_uuid *uuid;
	size_t uuid_size;

	uuid = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_UUID, &uuid_size);
	if (uuid && !nvme_ns_check_desc_len(uuid, uuid_size, sizeof(*uuid), "UUID", ns->ctrlr)) {
		return NULL;
	}

	return uuid;
}

static enum spdk_nvme_csi
nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	const uint8_t *csi;
	size_t csi_size;

	csi = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_CSI, &csi_size);
	if (csi && !nvme_ns_check_desc_len(csi, csi_size, sizeof(*csi), "CSI", ns->ctrlr))
	{
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
	struct spdk_nvme_ns_data *nsdata = &ns->nsdata;
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	const void *val;
	size_t val_size;

	ns->csi = nvme_ns_get_csi(ns);

	val = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_NGUID, &val_size);
	if (val && nvme_ns_check_desc_len(val, val_size, sizeof(nsdata->nguid), "NGUID", ctrlr)) {
		nvme_ns_backfill_nsdata(val, nsdata->nguid, sizeof(nsdata->nguid), "NGUID", ctrlr);
	}

	val = nvme_ns_find_id_desc(ns, SPDK_NVME_NIDT_EUI64, &val_size);
	if (val && nvme_ns_check_desc_len(val, val_size, sizeof(nsdata->eui64), "EUI64", ctrlr)) {
		nvme_ns_backfill_nsdata(val, &nsdata->eui64, sizeof(nsdata->eui64), "EUI64", ctrlr);
	}
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
nvme_ns_free_kv_specific_data(struct spdk_nvme_ns *ns)
{
	if (!ns->id) {
		return;
	}

	if (ns->nsdata_kv) {
		spdk_free(ns->nsdata_kv);
		ns->nsdata_kv = NULL;
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
	if (!ns->id) {
		return;
	}

	if (ns->nsdata_iocs) {
		spdk_free(ns->nsdata_iocs);
		ns->nsdata_iocs = NULL;
	}
}

bool
nvme_ns_has_supported_iocs_specific_data(struct spdk_nvme_ns *ns)
{
	switch (ns->csi) {
	case SPDK_NVME_CSI_NVM:
		if (ns->ctrlr->cdata.ctratt.elbas) {
			return true;
		}

		return false;
	case SPDK_NVME_CSI_ZNS:
		return true;
	case SPDK_NVME_CSI_KV:
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
nvme_ns_identify(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = ns->ctrlr;
	int	rc;

	assert(ns->id > 0);

	rc = nvme_ctrlr_identify_ns(ns);
	if (rc != 0) {
		return rc;
	}

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
nvme_ns_clear(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ns_data *nsdata = nvme_ns_get_data(ns);

	if (!ns->id) {
		return;
	}

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
	ns->active = false;
	ns->identify_pending = false;
}
