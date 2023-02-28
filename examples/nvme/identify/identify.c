/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

#define MAX_DISCOVERY_LOG_ENTRIES	((uint64_t)1000)

#define NUM_CHUNK_INFO_ENTRIES		8
#define MAX_OCSSD_PU			128
#define MAX_ZONE_DESC_ENTRIES		8

#define FDP_LOG_PAGE_SIZE		4096

static int outstanding_commands;

struct feature {
	uint32_t result;
	bool valid;
};

static struct feature features[256] = {};

static struct spdk_nvme_error_information_entry error_page[256];

static struct spdk_nvme_health_information_page health_page;

static struct spdk_nvme_firmware_page firmware_page;

static struct spdk_nvme_ana_page *g_ana_log_page;

static struct spdk_nvme_ana_group_descriptor *g_copied_ana_desc;

static size_t g_ana_log_page_size;

static uint8_t g_fdp_cfg_log_page_buf[FDP_LOG_PAGE_SIZE];

static uint8_t g_fdp_ruhu_log_page_buf[FDP_LOG_PAGE_SIZE];

static uint8_t g_fdp_events_log_page_buf[FDP_LOG_PAGE_SIZE];

static struct spdk_nvme_fdp_stats_log_page g_fdp_stats_log_page;

static struct spdk_nvme_fdp_cfg_log_page *g_fdp_cfg_log_page = (void *)g_fdp_cfg_log_page_buf;

static struct spdk_nvme_fdp_ruhu_log_page *g_fdp_ruhu_log_page = (void *)g_fdp_ruhu_log_page_buf;

static struct spdk_nvme_fdp_events_log_page *g_fdp_events_log_page = (void *)
		g_fdp_events_log_page_buf;

static struct spdk_nvme_cmds_and_effect_log_page cmd_effects_log_page;

static struct spdk_nvme_intel_smart_information_page intel_smart_page;

static struct spdk_nvme_intel_temperature_page intel_temperature_page;

static struct spdk_nvme_intel_marketing_description_page intel_md_page;

static struct spdk_nvmf_discovery_log_page *g_discovery_page;
static size_t g_discovery_page_size;
static uint64_t g_discovery_page_numrec;

static struct spdk_ocssd_geometry_data geometry_data;

static struct spdk_ocssd_chunk_information_entry *g_ocssd_chunk_info_page;

static int64_t g_zone_report_limit = 8;

static bool g_hex_dump = false;

static int g_shm_id = -1;

static int g_dpdk_mem = 0;

static bool g_dpdk_mem_single_seg = false;

static int g_main_core = 0;

static char g_core_mask[20] = "0x1";

static struct spdk_nvme_transport_id g_trid;
static char g_hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

static int g_controllers_found = 0;

static bool g_vmd = false;

static bool g_ocssd_verbose = false;

static struct spdk_nvme_detach_ctx *g_detach_ctx = NULL;

static void
hex_dump(const void *data, size_t size)
{
	size_t offset = 0, i;
	const uint8_t *bytes = data;

	while (size) {
		printf("%08zX:", offset);

		for (i = 0; i < 16; i++) {
			if (i == 8) {
				printf("-");
			} else {
				printf(" ");
			}

			if (i < size) {
				printf("%02X", bytes[offset + i]);
			} else {
				printf("  ");
			}
		}

		printf("  ");

		for (i = 0; i < 16; i++) {
			if (i < size) {
				if (bytes[offset + i] > 0x20 && bytes[offset + i] < 0x7F) {
					printf("%c", bytes[offset + i]);
				} else {
					printf(".");
				}
			}
		}

		printf("\n");

		offset += 16;
		if (size > 16) {
			size -= 16;
		} else {
			break;
		}
	}
}

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get_feature(0x%02X) failed\n", fid);
	} else {
		feature->result = cpl->cdw0;
		feature->valid = true;
	}
	outstanding_commands--;
}

static void
get_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get log page failed\n");
	}
	outstanding_commands--;
}

static void
get_ocssd_geometry_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get ocssd geometry failed\n");
	}
	outstanding_commands--;
}

static void
get_zns_zone_report_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get zns zone report failed\n");
	}

	outstanding_commands--;
}

static int
get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t fid, uint32_t cdw11, uint32_t nsid)
{
	struct spdk_nvme_cmd cmd = {};
	struct feature *feature = &features[fid];

	feature->valid = false;

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = fid;
	cmd.cdw11 = cdw11;
	cmd.nsid = nsid;

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, get_feature_completion, feature);
}

static void
get_features(struct spdk_nvme_ctrlr *ctrlr, uint8_t *features_to_get, size_t num_features,
	     uint32_t nsid)
{
	size_t i;
	uint32_t cdw11;

	/* Submit only one GET FEATURES at a time. There is a known issue #1799
	 * with Google Cloud Platform NVMe SSDs that do not handle overlapped
	 * GET FEATURES commands correctly.
	 */
	outstanding_commands = 0;
	for (i = 0; i < num_features; i++) {
		cdw11 = 0;
		if (!spdk_nvme_ctrlr_is_ocssd_supported(ctrlr) &&
		    features_to_get[i] == SPDK_OCSSD_FEAT_MEDIA_FEEDBACK) {
			continue;
		}
		if (features_to_get[i] == SPDK_NVME_FEAT_FDP) {
			const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
			struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

			if (!cdata->ctratt.fdps) {
				continue;
			} else {
				cdw11 = nsdata->endgid;
				/* Endurance group scope */
				nsid = 0;
			}
		}
		if (get_feature(ctrlr, features_to_get[i], cdw11, nsid) == 0) {
			outstanding_commands++;
		} else {
			printf("get_feature(0x%02X) failed to submit command\n", features_to_get[i]);
		}

		while (outstanding_commands) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		}
	}

}

static void
get_ctrlr_features(struct spdk_nvme_ctrlr *ctrlr)
{
	uint8_t features_to_get[] = {
		SPDK_NVME_FEAT_ARBITRATION,
		SPDK_NVME_FEAT_POWER_MANAGEMENT,
		SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD,
		SPDK_NVME_FEAT_NUMBER_OF_QUEUES,
		SPDK_OCSSD_FEAT_MEDIA_FEEDBACK,
	};

	get_features(ctrlr, features_to_get, SPDK_COUNTOF(features_to_get), 0);
}

static void
get_ns_features(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	uint8_t features_to_get[] = {
		SPDK_NVME_FEAT_ERROR_RECOVERY,
		SPDK_NVME_FEAT_FDP,
	};

	get_features(ctrlr, features_to_get, SPDK_COUNTOF(features_to_get), nsid);
}

static int
get_error_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_ERROR,
					     SPDK_NVME_GLOBAL_NS_TAG, error_page,
					     sizeof(*error_page) * (cdata->elpe + 1),
					     0,
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_health_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					     SPDK_NVME_GLOBAL_NS_TAG, &health_page, sizeof(health_page), 0, get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_firmware_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_FIRMWARE_SLOT,
					     SPDK_NVME_GLOBAL_NS_TAG, &firmware_page, sizeof(firmware_page), 0, get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_ana_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
					     SPDK_NVME_GLOBAL_NS_TAG, g_ana_log_page, g_ana_log_page_size, 0,
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_cmd_effects_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_COMMAND_EFFECTS_LOG,
					     SPDK_NVME_GLOBAL_NS_TAG, &cmd_effects_log_page, sizeof(cmd_effects_log_page), 0,
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_intel_smart_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_SMART, SPDK_NVME_GLOBAL_NS_TAG,
					     &intel_smart_page, sizeof(intel_smart_page), 0, get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_intel_temperature_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE,
					     SPDK_NVME_GLOBAL_NS_TAG, &intel_temperature_page, sizeof(intel_temperature_page), 0,
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}
	return 0;
}

static int
get_intel_md_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_MARKETING_DESCRIPTION,
					     SPDK_NVME_GLOBAL_NS_TAG, &intel_md_page, sizeof(intel_md_page), 0,
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}
	return 0;
}

static void
get_discovery_log_page_cb(void *ctx, int rc, const struct spdk_nvme_cpl *cpl,
			  struct spdk_nvmf_discovery_log_page *log_page)
{
	if (rc || spdk_nvme_cpl_is_error(cpl)) {
		printf("get discovery log page failed\n");
		exit(1);
	}

	g_discovery_page = log_page;
	g_discovery_page_numrec = from_le64(&log_page->numrec);
	g_discovery_page_size = sizeof(struct spdk_nvmf_discovery_log_page);
	g_discovery_page_size += g_discovery_page_numrec *
				 sizeof(struct spdk_nvmf_discovery_log_page_entry);
	outstanding_commands--;
}

static int
get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	return spdk_nvme_ctrlr_get_discovery_log_page(ctrlr, get_discovery_log_page_cb, NULL);
}

static void
get_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	outstanding_commands = 0;
	bool is_discovery = spdk_nvme_ctrlr_is_discovery(ctrlr);
	uint32_t nsid, active_ns_count = 0;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!is_discovery) {
		/*
		 * Only attempt to retrieve the following log pages
		 * when the NVM subsystem that's being targeted is
		 * NOT the Discovery Controller which only fields
		 * a Discovery Log Page.
		 */
		if (get_error_log_page(ctrlr) == 0) {
			outstanding_commands++;
		} else {
			printf("Get Error Log Page failed\n");
		}

		if (get_health_log_page(ctrlr) == 0) {
			outstanding_commands++;
		} else {
			printf("Get Log Page (SMART/health) failed\n");
		}

		if (get_firmware_log_page(ctrlr) == 0) {
			outstanding_commands++;
		} else {
			printf("Get Log Page (Firmware Slot Information) failed\n");
		}
	}

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS)) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			active_ns_count++;
		}

		/* We always set RGO (Return Groups Only) to 0 in this tool, an ANA group
		 * descriptor is returned only if that ANA group contains namespaces
		 * that are attached to the controller processing the command, and
		 * namespaces attached to the controller shall be members of an ANA group.
		 * Hence the following size should be enough.
		 */
		g_ana_log_page_size = sizeof(struct spdk_nvme_ana_page) + cdata->nanagrpid *
				      sizeof(struct spdk_nvme_ana_group_descriptor) + active_ns_count *
				      sizeof(uint32_t);
		g_ana_log_page = calloc(1, g_ana_log_page_size);
		if (g_ana_log_page == NULL) {
			exit(1);
		}
		g_copied_ana_desc = calloc(1, g_ana_log_page_size);
		if (g_copied_ana_desc == NULL) {
			exit(1);
		}
		if (get_ana_log_page(ctrlr) == 0) {
			outstanding_commands++;
		} else {
			printf("Get Log Page (Asymmetric Namespace Access) failed\n");
		}
	}
	if (cdata->lpa.celp) {
		if (get_cmd_effects_log_page(ctrlr) == 0) {
			outstanding_commands++;
		} else {
			printf("Get Log Page (Commands Supported and Effects) failed\n");
		}
	}

	if (cdata->vid == SPDK_PCI_VID_INTEL) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_LOG_SMART)) {
			if (get_intel_smart_log_page(ctrlr) == 0) {
				outstanding_commands++;
			} else {
				printf("Get Log Page (Intel SMART/health) failed\n");
			}
		}
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE)) {
			if (get_intel_temperature_log_page(ctrlr) == 0) {
				outstanding_commands++;
			} else {
				printf("Get Log Page (Intel temperature) failed\n");
			}
		}
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_MARKETING_DESCRIPTION)) {
			if (get_intel_md_log_page(ctrlr) == 0) {
				outstanding_commands++;
			} else {
				printf("Get Log Page (Intel Marketing Description) failed\n");
			}
		}

	}

	if (is_discovery && (get_discovery_log_page(ctrlr) == 0)) {
		outstanding_commands++;
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static int
get_fdp_cfg_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	outstanding_commands = 0;

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_LOG_FDP_CONFIGURATIONS)) {
		/* Fetch the FDP configurations log page for only 4096 bytes */
		if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_CONFIGURATIONS, 0,
				g_fdp_cfg_log_page, FDP_LOG_PAGE_SIZE, 0, 0, (nsdata->endgid << 16),
				0, get_log_page_completion, NULL) == 0) {
			outstanding_commands++;
		} else {
			printf("spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP config) failed\n");
			return -1;
		}
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return 0;
}

static int
get_fdp_ruhu_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	outstanding_commands = 0;

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE)) {
		/* Fetch the reclaim unit handle usage log page for only 4096 bytes */
		if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE, 0,
				g_fdp_ruhu_log_page, FDP_LOG_PAGE_SIZE, 0, 0, (nsdata->endgid << 16),
				0, get_log_page_completion, NULL) == 0) {
			outstanding_commands++;
		} else {
			printf("spdk_nvme_ctrlr_cmd_get_log_page_ext(RUH usage) failed\n");
			return -1;
		}
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return 0;
}

static int
get_fdp_stats_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	outstanding_commands = 0;

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_LOG_FDP_STATISTICS)) {
		if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_STATISTICS, 0,
				&g_fdp_stats_log_page, 64, 0, 0, (nsdata->endgid << 16), 0,
				get_log_page_completion, NULL) == 0) {
			outstanding_commands++;
		} else {
			printf("spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP stats) failed\n");
			return -1;
		}
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return 0;
}

static int
get_fdp_events_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	outstanding_commands = 0;

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_LOG_FDP_EVENTS)) {
		/* Only fetch FDP host events here */
		if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_EVENTS, 0,
				g_fdp_events_log_page, FDP_LOG_PAGE_SIZE, 0,
				(SPDK_NVME_FDP_REPORT_HOST_EVENTS << 8), (nsdata->endgid << 16),
				0, get_log_page_completion, NULL) == 0) {
			outstanding_commands++;
		} else {
			printf("spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP events) failed\n");
			return -1;
		}
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return 0;
}

static int
get_ocssd_chunk_info_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	int nsid = spdk_nvme_ns_get_id(ns);
	uint32_t num_entry = geometry_data.num_grp * geometry_data.num_pu * geometry_data.num_chk;
	uint32_t xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	uint32_t buf_size = 0;
	uint64_t buf_offset = 0;
	outstanding_commands = 0;

	assert(num_entry != 0);
	if (!g_ocssd_verbose) {
		num_entry = spdk_min(num_entry, NUM_CHUNK_INFO_ENTRIES);
	}

	g_ocssd_chunk_info_page = calloc(num_entry, sizeof(struct spdk_ocssd_chunk_information_entry));
	assert(g_ocssd_chunk_info_page != NULL);

	buf_size = num_entry * sizeof(struct spdk_ocssd_chunk_information_entry);
	while (buf_size > 0) {
		xfer_size = spdk_min(buf_size, xfer_size);
		if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_OCSSD_LOG_CHUNK_INFO,
						     nsid, (void *) g_ocssd_chunk_info_page + buf_offset,
						     xfer_size, buf_offset, get_log_page_completion, NULL) == 0) {
			outstanding_commands++;
		} else {
			printf("get_ocssd_chunk_info_log_page() failed\n");
			return -1;
		}

		buf_size -= xfer_size;
		buf_offset += xfer_size;
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return 0;
}

static void
get_ocssd_geometry(struct spdk_nvme_ns *ns, struct spdk_ocssd_geometry_data *geometry_data)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	int nsid = spdk_nvme_ns_get_id(ns);
	outstanding_commands = 0;

	if (spdk_nvme_ocssd_ctrlr_cmd_geometry(ctrlr, nsid, geometry_data,
					       sizeof(*geometry_data), get_ocssd_geometry_completion, NULL)) {
		printf("Get OpenChannel SSD geometry failed\n");
		exit(1);
	} else {
		outstanding_commands++;
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
print_hex_be(const void *v, size_t size)
{
	const uint8_t *buf = v;

	while (size--) {
		printf("%02X", *buf++);
	}
}

static void
print_uint128_hex(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		printf("0x%llX%016llX", hi, lo);
	} else {
		printf("0x%llX", lo);
	}
}

static void
print_uint128_dec(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		/* can't handle large (>64-bit) decimal values for now, so fall back to hex */
		print_uint128_hex(v);
	} else {
		printf("%llu", (unsigned long long)lo);
	}
}

/* The len should be <= 8. */
static void
print_uint_var_dec(uint8_t *array, unsigned int len)
{
	uint64_t result = 0;
	int i = len;

	while (i > 0) {
		result += (uint64_t)array[i - 1] << (8 * (i - 1));
		i--;
	}
	printf("%" PRIu64, result);
}

/* Print ASCII string as defined by the NVMe spec */
static void
print_ascii_string(const void *buf, size_t size)
{
	const uint8_t *str = buf;

	/* Trim trailing spaces */
	while (size > 0 && str[size - 1] == ' ') {
		size--;
	}

	while (size--) {
		if (*str >= 0x20 && *str <= 0x7E) {
			printf("%c", *str);
		} else {
			printf(".");
		}
		str++;
	}
}

/* Underline a "line" with the given marker, e.g. print_uline("=", printf(...)); */
static void
print_uline(char marker, int line_len)
{
	for (int i = 1; i < line_len; ++i) {
		putchar(marker);
	}
	putchar('\n');
}

static void
print_fdp_cfg_log_page(void)
{
	uint32_t i, j;
	struct spdk_nvme_fdp_cfg_descriptor *cfg_desc;
	void *log = g_fdp_cfg_log_page->cfg_desc;

	printf("FDP configurations log page\n");
	printf("===========================\n");
	if (g_hex_dump) {
		hex_dump(g_fdp_cfg_log_page, FDP_LOG_PAGE_SIZE);
		printf("\n");
	}

	printf("Number of FDP configurations:         %u\n", g_fdp_cfg_log_page->ncfg + 1);
	printf("Version:                              %u\n", g_fdp_cfg_log_page->version);
	printf("Size:                                 %u\n", g_fdp_cfg_log_page->size);

	for (i = 0; i <= g_fdp_cfg_log_page->ncfg; i++) {
		cfg_desc = log;
		printf("FDP Configuration Descriptor:         %u\n", i);
		printf("  Descriptor Size:                    %u\n", cfg_desc->ds);
		printf("  Reclaim Group Identifier format:    %u\n", cfg_desc->fdpa.bits.rgif);
		printf("  FDP Volatile Write Cache:           %s\n",
		       cfg_desc->fdpa.bits.fdpvwc ? "Present" : "Not Present");
		printf("  FDP Configuration:                  %s\n",
		       cfg_desc->fdpa.bits.fdpcv ? "Valid" : "Invalid");
		printf("  Vendor Specific Size:               %u\n", cfg_desc->vss);
		printf("  Number of Reclaim Groups:           %u\n", cfg_desc->nrg);
		printf("  Number of Recalim Unit Handles:     %u\n", cfg_desc->nruh);
		printf("  Max Placement Identifiers:          %u\n", cfg_desc->maxpids + 1);
		printf("  Number of Namespaces Suppprted:     %u\n", cfg_desc->nns);
		printf("  Reclaim unit Nominal Size:          %" PRIx64 " bytes\n", cfg_desc->runs);
		printf("  Estimated Reclaim Unit Time Limit:  ");
		if (cfg_desc->erutl) {
			printf("%u seconds\n", cfg_desc->erutl);
		} else {
			printf("Not Reported\n");
		}
		for (j = 0; j < cfg_desc->nruh; j++) {
			printf("    RUH Desc #%03d:          RUH Type: %s\n", j,
			       cfg_desc->ruh_desc[j].ruht == SPDK_NVME_FDP_RUHT_INITIALLY_ISOLATED ? "Initially Isolated" :
			       cfg_desc->ruh_desc[j].ruht == SPDK_NVME_FDP_RUHT_PERSISTENTLY_ISOLATED ? "Persistently Isolated" :
			       "Reserved");
		}
		log += cfg_desc->ds;
	}

	printf("\n");

}

static void
print_fdp_ruhu_log_page(void)
{
	uint32_t i;
	struct spdk_nvme_fdp_ruhu_descriptor *ruhu_desc;

	printf("FDP reclaim unit handle usage log page\n");
	printf("======================================\n");
	if (g_hex_dump) {
		hex_dump(g_fdp_ruhu_log_page, FDP_LOG_PAGE_SIZE);
		printf("\n");
	}

	printf("Number of Reclaim Unit Handles:       %u\n", g_fdp_ruhu_log_page->nruh);

	for (i = 0; i < g_fdp_ruhu_log_page->nruh; i++) {
		ruhu_desc = &g_fdp_ruhu_log_page->ruhu_desc[i];

		printf("  RUH Usage Desc #%03d:   RUH Attributes: %s\n", i,
		       ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_UNUSED ? "Unused" :
		       ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_HOST_SPECIFIED ? "Host Specified" :
		       ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_CTRLR_SPECIFIED ? "Controller Specified" :
		       "Reserved");
	}

	printf("\n");
}

static void
print_fdp_stats_log_page(void)
{
	printf("FDP statistics log page\n");
	printf("=======================\n");
	if (g_hex_dump) {
		hex_dump(&g_fdp_stats_log_page, 64);
		printf("\n");
	}

	printf("Host bytes with metadata written:  ");
	print_uint128_dec(g_fdp_stats_log_page.hbmw);
	printf("\n");
	printf("Media bytes with metadata written: ");
	print_uint128_dec(g_fdp_stats_log_page.mbmw);
	printf("\n");
	printf("Media bytes erased:                ");
	print_uint128_dec(g_fdp_stats_log_page.mbe);
	printf("\n\n");
}

static void
print_fdp_events_log_page(void)
{
	uint32_t i;
	struct spdk_nvme_fdp_event *event;
	struct spdk_nvme_fdp_event_media_reallocated *media_reallocated;

	printf("FDP events log page\n");
	printf("===================\n");
	if (g_hex_dump) {
		hex_dump(g_fdp_events_log_page, FDP_LOG_PAGE_SIZE);
		printf("\n");
	}

	printf("Number of FDP events:              %u\n", g_fdp_events_log_page->nevents);

	for (i = 0; i < g_fdp_events_log_page->nevents; i++) {
		event = &g_fdp_events_log_page->event[i];

		printf("FDP Event #%u:\n", i);
		printf("  Event Type:                      %s\n",
		       event->etype == SPDK_NVME_FDP_EVENT_RU_NOT_WRITTEN_CAPACITY ? "RU Not Written to Capacity" :
		       event->etype == SPDK_NVME_FDP_EVENT_RU_TIME_LIMIT_EXCEEDED ? "RU Time Limit Exceeded" :
		       event->etype == SPDK_NVME_FDP_EVENT_CTRLR_RESET_MODIFY_RUH ? "Ctrlr Reset Modified RUH's" :
		       event->etype == SPDK_NVME_FDP_EVENT_INVALID_PLACEMENT_ID ? "Invalid Placement Identifier" :
		       event->etype == SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED ? "Media Reallocated" :
		       event->etype == SPDK_NVME_FDP_EVENT_IMPLICIT_MODIFIED_RUH ? "Implicitly modified RUH" :
		       "Reserved");
		printf("  Placement Identifier:            %s\n",
		       event->fdpef.bits.piv ? "Valid" : "Invalid");
		printf("  NSID:                            %s\n",
		       event->fdpef.bits.nsidv ? "Valid" : "Invalid");
		printf("  Location:                        %s\n",
		       event->fdpef.bits.lv ? "Valid" : "Invalid");
		if (event->fdpef.bits.piv) {
			printf("  Placement Identifier:            %u\n", event->pid);
		} else {
			printf("  Placement Identifier:            Reserved\n");
		}
		printf("  Event Timestamp:                 %" PRIx64 "\n", event->timestamp);
		if (event->fdpef.bits.nsidv) {
			printf("  Namespace Identifier:            %u\n", event->nsid);
		} else {
			printf("  Namespace Identifier:            Ignore\n");
		}

		if (event->etype == SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED) {
			media_reallocated = (struct spdk_nvme_fdp_event_media_reallocated *)&event->event_type_specific;

			printf("  LBA:                             %s\n",
			       media_reallocated->sef.bits.lbav ? "Valid" : "Invalid");
			printf("  Number of LBA's Moved:           %u\n", media_reallocated->nlbam);
			if (media_reallocated->sef.bits.lbav) {
				printf("  Logical Block Address:           %u\n", event->nsid);
			} else {
				printf("  Logical Block Address:           Ignore\n");
			}
		}

		if (event->fdpef.bits.lv) {
			printf("  Reclaim Group Identifier:        %u\n", event->rgid);
		} else {
			printf("  Reclaim Group Identifier:        Ignore\n");
		}
		if (event->fdpef.bits.lv) {
			printf("  Reclaim Unit Handle Identifier:  %u\n", event->ruhid);
		} else {
			printf("  Reclaim Unit Handle Identifier:  Ignore\n");
		}
	}

	printf("\n");

}

static void
print_ocssd_chunk_info(struct spdk_ocssd_chunk_information_entry *chk_info, int chk_num)
{
	int i;
	char *cs_str, *ct_str;

	printf("OCSSD Chunk Info Glance\n");
	printf("======================\n");

	for (i = 0; i < chk_num; i++) {
		cs_str = chk_info[i].cs.free ? "Free" :
			 chk_info[i].cs.closed ? "Closed" :
			 chk_info[i].cs.open ? "Open" :
			 chk_info[i].cs.offline ? "Offline" : "Unknown";
		ct_str = chk_info[i].ct.seq_write ? "Sequential Write" :
			 chk_info[i].ct.rnd_write ? "Random Write" : "Unknown";

		printf("------------\n");
		printf("Chunk index:                    %d\n", i);
		printf("Chunk state:                    %s(0x%x)\n", cs_str, *(uint8_t *) & (chk_info[i].cs));
		printf("Chunk type (write mode):        %s\n", ct_str);
		printf("Chunk type (size_deviate):      %s\n", chk_info[i].ct.size_deviate ? "Yes" : "No");
		printf("Wear-level Index:               %d\n", chk_info[i].wli);
		printf("Starting LBA:                   %" PRIu64 "\n", chk_info[i].slba);
		printf("Number of blocks in chunk:      %" PRIu64 "\n", chk_info[i].cnlb);
		printf("Write Pointer:                  %" PRIu64 "\n", chk_info[i].wp);
	}
}

static void
print_ocssd_chunk_info_verbose(struct spdk_ocssd_chunk_information_entry *chk_info)
{
	uint32_t pu, chk, i;
	uint32_t cnt_free, cnt_closed, cnt_open, cnt_offline;
	uint32_t max_pu = spdk_min(MAX_OCSSD_PU, (geometry_data.num_grp * geometry_data.num_pu));
	char cs_str[MAX_OCSSD_PU + 1], cs;

	assert(chk_info != NULL);
	printf("OCSSD Chunk Info Verbose\n");
	printf("======================\n");

	printf("%4s %-*s %3s %3s %3s %3s\n", "band", max_pu, "chunk state", "fr", "cl", "op", "of");
	for (chk = 0; chk < geometry_data.num_chk; chk++) {
		cnt_free = cnt_closed = cnt_open = cnt_offline = 0;
		for (pu = 0; pu < max_pu; pu++) {
			i = (pu * geometry_data.num_chk) + chk;
			if (chk_info[i].cs.free) {
				cnt_free++;
				cs = 'f';
			} else if (chk_info[i].cs.closed) {
				cnt_closed++;
				cs = 'c';
			} else if (chk_info[i].cs.open) {
				cnt_open++;
				cs = 'o';
			} else if (chk_info[i].cs.offline) {
				cnt_offline++;
				cs = 'l';
			} else {
				cs = '.';
			}
			cs_str[pu] = cs;
		}
		cs_str[pu] = 0;
		printf("%4d %s %3d %3d %3d %3d\n", chk, cs_str, cnt_free, cnt_closed, cnt_open, cnt_offline);
	}
}

static void
print_ocssd_geometry(struct spdk_ocssd_geometry_data *geometry_data)
{
	printf("Namespace OCSSD Geometry\n");
	printf("=======================\n");

	if (geometry_data->mjr < 2) {
		printf("Open-Channel Spec version is less than 2.0\n");
		printf("OC version:             maj:%d\n", geometry_data->mjr);
		return;
	}

	printf("OC version:                     maj:%d min:%d\n", geometry_data->mjr, geometry_data->mnr);
	printf("LBA format:\n");
	printf("  Group bits:                   %d\n", geometry_data->lbaf.grp_len);
	printf("  PU bits:                      %d\n", geometry_data->lbaf.pu_len);
	printf("  Chunk bits:                   %d\n", geometry_data->lbaf.chk_len);
	printf("  Logical block bits:           %d\n", geometry_data->lbaf.lbk_len);

	printf("Media and Controller Capabilities:\n");
	printf("  Namespace supports Vector Chunk Copy:                 %s\n",
	       geometry_data->mccap.vec_chk_cpy ? "Supported" : "Not Supported");
	printf("  Namespace supports multiple resets a free chunk:      %s\n",
	       geometry_data->mccap.multi_reset ? "Supported" : "Not Supported");

	printf("Wear-level Index Delta Threshold:                       %d\n", geometry_data->wit);
	printf("Groups (channels):              %d\n", geometry_data->num_grp);
	printf("PUs (LUNs) per group:           %d\n", geometry_data->num_pu);
	printf("Chunks per LUN:                 %d\n", geometry_data->num_chk);
	printf("Logical blks per chunk:         %d\n", geometry_data->clba);
	printf("MIN write size:                 %d\n", geometry_data->ws_min);
	printf("OPT write size:                 %d\n", geometry_data->ws_opt);
	printf("Cache min write size:           %d\n", geometry_data->mw_cunits);
	printf("Max open chunks:                %d\n", geometry_data->maxoc);
	printf("Max open chunks per PU:         %d\n", geometry_data->maxocpu);
	printf("\n");
}

static void
print_zns_zone(uint8_t *report, uint32_t index, uint32_t zdes)
{
	struct spdk_nvme_zns_zone_desc *desc;
	uint32_t i, zds, zrs, zd_index;

	zrs = sizeof(struct spdk_nvme_zns_zone_report);
	zds = sizeof(struct spdk_nvme_zns_zone_desc);
	zd_index = zrs + index * (zds + zdes);

	desc = (struct spdk_nvme_zns_zone_desc *)(report + zd_index);

	printf("ZSLBA: 0x%016"PRIx64" ZCAP: 0x%016"PRIx64" WP: 0x%016"PRIx64" ZS: ", desc->zslba,
	       desc->zcap, desc->wp);
	switch (desc->zs) {
	case SPDK_NVME_ZONE_STATE_EMPTY:
		printf("Empty");
		break;
	case SPDK_NVME_ZONE_STATE_IOPEN:
		printf("Implicit open");
		break;
	case SPDK_NVME_ZONE_STATE_EOPEN:
		printf("Explicit open");
		break;
	case SPDK_NVME_ZONE_STATE_CLOSED:
		printf("Closed");
		break;
	case SPDK_NVME_ZONE_STATE_RONLY:
		printf("Read only");
		break;
	case SPDK_NVME_ZONE_STATE_FULL:
		printf("Full");
		break;
	case SPDK_NVME_ZONE_STATE_OFFLINE:
		printf("Offline");
		break;
	default:
		printf("Reserved");
	}
	printf(" ZT: %s ZA: %x\n", (desc->zt == SPDK_NVME_ZONE_TYPE_SEQWR) ? "SWR" : "Reserved",
	       desc->za.raw);

	if (!desc->za.bits.zdev) {
		return;
	}

	for (i = 0; i < zdes; i += 8) {
		printf("zone_desc_ext[%d] : 0x%"PRIx64"\n", i,
		       *(uint64_t *)(report + zd_index + zds + i));
	}
}

static void
get_and_print_zns_zone_report(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_ns_data *nsdata;
	const struct spdk_nvme_zns_ns_data *nsdata_zns;
	uint8_t *report_buf;
	size_t report_bufsize;
	uint64_t zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
	uint64_t total_zones = spdk_nvme_zns_ns_get_num_zones(ns);
	uint64_t max_zones_per_buf, zones_to_print, i;
	uint64_t nr_zones = 0;
	uint64_t handled_zones = 0;
	uint64_t slba = 0;
	size_t zdes = 0;
	uint32_t zds, zrs, format_index;
	int rc = 0;

	outstanding_commands = 0;

	nsdata = spdk_nvme_ns_get_data(ns);
	nsdata_zns = spdk_nvme_zns_ns_get_data(ns);

	zrs = sizeof(struct spdk_nvme_zns_zone_report);
	zds = sizeof(struct spdk_nvme_zns_zone_desc);

	format_index = spdk_nvme_ns_get_format_index(nsdata);
	zdes = nsdata_zns->lbafe[format_index].zdes * 64;

	report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns);
	report_buf = calloc(1, report_bufsize);
	if (!report_buf) {
		printf("Zone report allocation failed!\n");
		exit(1);
	}

	zones_to_print = g_zone_report_limit ? spdk_min(total_zones, (uint64_t)g_zone_report_limit) : \
			 total_zones;

	print_uline('=', printf("NVMe ZNS Zone Report (first %zu of %zu)\n", zones_to_print, total_zones));

	while (handled_zones < zones_to_print) {
		memset(report_buf, 0, report_bufsize);

		if (zdes) {
			max_zones_per_buf = (report_bufsize - zrs) / (zds + zdes);
			rc = spdk_nvme_zns_ext_report_zones(ns, qpair, report_buf, report_bufsize,
							    slba, SPDK_NVME_ZRA_LIST_ALL, true,
							    get_zns_zone_report_completion, NULL);
		} else {
			max_zones_per_buf = (report_bufsize - zrs) / zds;
			rc = spdk_nvme_zns_report_zones(ns, qpair, report_buf, report_bufsize,
							slba, SPDK_NVME_ZRA_LIST_ALL, true,
							get_zns_zone_report_completion, NULL);
		}

		if (rc) {
			fprintf(stderr, "Report zones failed\n");
			exit(1);
		} else {
			outstanding_commands++;
		}

		while (outstanding_commands) {
			spdk_nvme_qpair_process_completions(qpair, 0);
		}

		nr_zones = report_buf[0];
		if (nr_zones > max_zones_per_buf) {
			fprintf(stderr, "nr_zones too big\n");
			exit(1);
		}

		if (!nr_zones) {
			break;
		}

		for (i = 0; i < nr_zones && handled_zones < zones_to_print; i++) {
			print_zns_zone(report_buf, i, zdes);
			slba += zone_size_lba;
			handled_zones++;
		}
		printf("\n");
	}

	free(report_buf);
}

static void
print_zns_ns_data(const struct spdk_nvme_zns_ns_data *nsdata_zns)
{
	printf("ZNS Specific Namespace Data\n");
	printf("===========================\n");
	printf("Variable Zone Capacity:                %s\n",
	       nsdata_zns->zoc.variable_zone_capacity ? "Yes" : "No");
	printf("Zone Active Excursions:                %s\n",
	       nsdata_zns->zoc.zone_active_excursions ? "Yes" : "No");
	printf("Read Across Zone Boundaries:           %s\n",
	       nsdata_zns->ozcs.read_across_zone_boundaries ? "Yes" : "No");
	if (nsdata_zns->mar == 0xffffffff) {
		printf("Max Active Resources:                  No Limit\n");
	} else {
		printf("Max Active Resources:                  %"PRIu32"\n",
		       nsdata_zns->mar + 1);
	}
	if (nsdata_zns->mor == 0xffffffff) {
		printf("Max Open Resources:                    No Limit\n");
	} else {
		printf("Max Open Resources:                    %"PRIu32"\n",
		       nsdata_zns->mor + 1);
	}
	if (nsdata_zns->rrl == 0) {
		printf("Reset Recommended Limit:               Not Reported\n");
	} else {
		printf("Reset Recommended Limit:               %"PRIu32" seconds\n",
		       nsdata_zns->rrl);
	}
	if (nsdata_zns->rrl1 == 0) {
		printf("Reset Recommended Limit 1:             Not Reported\n");
	} else {
		printf("Reset Recommended Limit 1:             %"PRIu32" seconds\n",
		       nsdata_zns->rrl1);
	}
	if (nsdata_zns->rrl2 == 0) {
		printf("Reset Recommended Limit 2:             Not Reported\n");
	} else {
		printf("Reset Recommended Limit 2:             %"PRIu32" seconds\n",
		       nsdata_zns->rrl2);
	}
	if (nsdata_zns->rrl3 == 0) {
		printf("Reset Recommended Limit 3:             Not Reported\n");
	} else {
		printf("Reset Recommended Limit 3:             %"PRIu32" seconds\n",
		       nsdata_zns->rrl3);
	}
	if (nsdata_zns->frl == 0) {
		printf("Finish Recommended Limit:              Not Reported\n");
	} else {
		printf("Finish Recommended Limit:              %"PRIu32" seconds\n",
		       nsdata_zns->frl);
	}
	if (nsdata_zns->frl1 == 0) {
		printf("Finish Recommended Limit 1:            Not Reported\n");
	} else {
		printf("Finish Recommended Limit 1:            %"PRIu32" seconds\n",
		       nsdata_zns->frl1);
	}
	if (nsdata_zns->frl2 == 0) {
		printf("Finish Recommended Limit 2:            Not Reported\n");
	} else {
		printf("Finish Recommended Limit 2:            %"PRIu32" seconds\n",
		       nsdata_zns->frl2);
	}
	if (nsdata_zns->frl3 == 0) {
		printf("Finish Recommended Limit 3:            Not Reported\n");
	} else {
		printf("Finish Recommended Limit 3:            %"PRIu32" seconds\n",
		       nsdata_zns->frl3);
	}
	printf("\n");
}

static const char *
csi_name(enum spdk_nvme_csi csi)
{
	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		return "NVM";
	case SPDK_NVME_CSI_KV:
		return "KV";
	case SPDK_NVME_CSI_ZNS:
		return "ZNS";
	default:
		if (csi >= 0x30 && csi <= 0x3f) {
			return "Vendor specific";
		}
		return "Unknown";
	}
}

static void
print_namespace(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	const struct spdk_nvme_ns_data		*nsdata;
	const struct spdk_nvme_zns_ns_data	*nsdata_zns;
	const struct spdk_uuid			*uuid;
	uint32_t				i;
	uint32_t				flags;
	char					uuid_str[SPDK_UUID_STRING_LEN];
	uint32_t				blocksize, format_index;
	enum spdk_nvme_dealloc_logical_block_read_value	dlfeat_read_value;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	nsdata = spdk_nvme_ns_get_data(ns);
	nsdata_zns = spdk_nvme_zns_ns_get_data(ns);
	flags  = spdk_nvme_ns_get_flags(ns);

	printf("Namespace ID:%d\n", spdk_nvme_ns_get_id(ns));

	if (g_hex_dump) {
		hex_dump(nsdata, sizeof(*nsdata));
		printf("\n");
	}

	/* This function is only called for active namespaces. */
	assert(spdk_nvme_ns_is_active(ns));

	if (features[SPDK_NVME_FEAT_ERROR_RECOVERY].valid) {
		unsigned tler = features[SPDK_NVME_FEAT_ERROR_RECOVERY].result & 0xFFFF;
		printf("Error Recovery Timeout:                ");
		if (tler == 0) {
			printf("Unlimited\n");
		} else {
			printf("%u milliseconds\n", tler * 100);
		}
	}

	printf("Command Set Identifier:                %s (%02Xh)\n",
	       csi_name(spdk_nvme_ns_get_csi(ns)), spdk_nvme_ns_get_csi(ns));
	printf("Deallocate:                            %s\n",
	       (flags & SPDK_NVME_NS_DEALLOCATE_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Deallocated/Unwritten Error:           %s\n",
	       nsdata->nsfeat.dealloc_or_unwritten_error ? "Supported" : "Not Supported");
	dlfeat_read_value = spdk_nvme_ns_get_dealloc_logical_block_read_value(ns);
	printf("Deallocated Read Value:                %s\n",
	       dlfeat_read_value == SPDK_NVME_DEALLOC_READ_00 ? "All 0x00" :
	       dlfeat_read_value == SPDK_NVME_DEALLOC_READ_FF ? "All 0xFF" :
	       "Unknown");
	printf("Deallocate in Write Zeroes:            %s\n",
	       nsdata->dlfeat.bits.write_zero_deallocate ? "Supported" : "Not Supported");
	printf("Deallocated Guard Field:               %s\n",
	       nsdata->dlfeat.bits.guard_value ? "CRC for Read Value" : "0xFFFF");
	printf("Flush:                                 %s\n",
	       (flags & SPDK_NVME_NS_FLUSH_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Reservation:                           %s\n",
	       (flags & SPDK_NVME_NS_RESERVATION_SUPPORTED) ? "Supported" : "Not Supported");
	if (flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		printf("End-to-End Data Protection:            Supported\n");
		printf("Protection Type:                       Type%d\n", nsdata->dps.pit);
		printf("Protection Information Transferred as: %s\n",
		       nsdata->dps.md_start ? "First 8 Bytes" : "Last 8 Bytes");
	}
	format_index = spdk_nvme_ns_get_format_index(nsdata);
	if (nsdata->lbaf[format_index].ms > 0) {
		printf("Metadata Transferred as:               %s\n",
		       nsdata->flbas.extended ? "Extended Data LBA" : "Separate Metadata Buffer");
	}
	printf("Namespace Sharing Capabilities:        %s\n",
	       nsdata->nmic.can_share ? "Multiple Controllers" : "Private");
	blocksize = 1 << nsdata->lbaf[format_index].lbads;
	printf("Size (in LBAs):                        %lld (%lldGiB)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze * blocksize / 1024 / 1024 / 1024);
	printf("Capacity (in LBAs):                    %lld (%lldGiB)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap * blocksize / 1024 / 1024 / 1024);
	printf("Utilization (in LBAs):                 %lld (%lldGiB)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse * blocksize / 1024 / 1024 / 1024);
	if (nsdata->noiob) {
		printf("Optimal I/O Boundary:                  %u blocks\n", nsdata->noiob);
	}
	if (!spdk_mem_all_zero(nsdata->nguid, sizeof(nsdata->nguid))) {
		printf("NGUID:                                 ");
		print_hex_be(nsdata->nguid, sizeof(nsdata->nguid));
		printf("\n");
	}
	if (!spdk_mem_all_zero(&nsdata->eui64, sizeof(nsdata->eui64))) {
		printf("EUI64:                                 ");
		print_hex_be(&nsdata->eui64, sizeof(nsdata->eui64));
		printf("\n");
	}
	uuid = spdk_nvme_ns_get_uuid(ns);
	if (uuid) {
		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), uuid);
		printf("UUID:                                  %s\n", uuid_str);
	}
	printf("Thin Provisioning:                     %s\n",
	       nsdata->nsfeat.thin_prov ? "Supported" : "Not Supported");
	printf("Per-NS Atomic Units:                   %s\n",
	       nsdata->nsfeat.ns_atomic_write_unit ? "Yes" : "No");
	if (nsdata->nsfeat.ns_atomic_write_unit) {
		if (nsdata->nawun) {
			printf("  Atomic Write Unit (Normal):          %d\n", nsdata->nawun + 1);
		}

		if (nsdata->nawupf) {
			printf("  Atomic Write Unit (PFail):           %d\n", nsdata->nawupf + 1);
		}

		if (nsdata->npwg) {
			printf("  Preferred Write Granularity:         %d\n", nsdata->npwg + 1);
		}

		if (nsdata->nacwu) {
			printf("  Atomic Compare & Write Unit:         %d\n", nsdata->nacwu + 1);
		}

		printf("  Atomic Boundary Size (Normal):       %d\n", nsdata->nabsn);
		printf("  Atomic Boundary Size (PFail):        %d\n", nsdata->nabspf);
		printf("  Atomic Boundary Offset:              %d\n", nsdata->nabo);
	}

	if (cdata->oncs.copy) {
		printf("Maximum Single Source Range Length:    %d\n", nsdata->mssrl);
		printf("Maximum Copy Length:                   %d\n", nsdata->mcl);
		printf("Maximum Source Range Count:            %d\n", nsdata->msrc + 1);
	}

	printf("NGUID/EUI64 Never Reused:              %s\n",
	       nsdata->nsfeat.guid_never_reused ? "Yes" : "No");

	if (cdata->cmic.ana_reporting) {
		printf("ANA group ID:                          %u\n", nsdata->anagrpid);
	}

	printf("Namespace Write Protected:             %s\n",
	       nsdata->nsattr.write_protected ? "Yes" : "No");

	if (cdata->ctratt.nvm_sets) {
		printf("NVM set ID:                            %u\n", nsdata->nvmsetid);
	}

	if (cdata->ctratt.endurance_groups) {
		printf("Endurance group ID:                    %u\n", nsdata->endgid);
	}

	printf("Number of LBA Formats:                 %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:                    LBA Format #%02d\n",
	       format_index);
	for (i = 0; i <= nsdata->nlbaf; i++) {
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
		if (spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS) {
			printf("LBA Format Extension #%02d: Zone Size (in LBAs): 0x%"PRIx64" Zone Descriptor Extension Size: %d bytes\n",
			       i, nsdata_zns->lbafe[i].zsze, nsdata_zns->lbafe[i].zdes << 6);
		}

	}
	printf("\n");

	if (cdata->ctratt.fdps) {
		union spdk_nvme_feat_fdp_cdw12 fdp_res;

		if (features[SPDK_NVME_FEAT_FDP].valid) {
			fdp_res.raw = features[SPDK_NVME_FEAT_FDP].result;

			printf("Get Feature FDP:\n");
			printf("================\n");
			printf("  Enabled:                 %s\n",
			       fdp_res.bits.fdpe ? "Yes" : "No");
			printf("  FDP configuration index: %u\n\n", fdp_res.bits.fdpci);

			if (fdp_res.bits.fdpe && !get_fdp_cfg_log_page(ns)) {
				print_fdp_cfg_log_page();
			}
			if (fdp_res.bits.fdpe && !get_fdp_ruhu_log_page(ns)) {
				print_fdp_ruhu_log_page();
			}
			if (fdp_res.bits.fdpe && !get_fdp_stats_log_page(ns)) {
				print_fdp_stats_log_page();
			}
			if (fdp_res.bits.fdpe && !get_fdp_events_log_page(ns)) {
				print_fdp_events_log_page();
			}
		}
	}

	if (spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
		get_ocssd_geometry(ns, &geometry_data);
		print_ocssd_geometry(&geometry_data);
		get_ocssd_chunk_info_log_page(ns);
		if (g_ocssd_verbose) {
			print_ocssd_chunk_info_verbose(g_ocssd_chunk_info_page);
		} else {
			print_ocssd_chunk_info(g_ocssd_chunk_info_page, NUM_CHUNK_INFO_ENTRIES);
		}
	} else if (spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS) {
		struct spdk_nvme_qpair *qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
		if (qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			exit(1);
		}
		print_zns_ns_data(nsdata_zns);
		get_and_print_zns_zone_report(ns, qpair);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}
}

static const char *
admin_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_DELETE_IO_SQ:
		return "Delete I/O Submission Queue";
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return "Create I/O Submission Queue";
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return "Get Log Page";
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return "Delete I/O Completion Queue";
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		return "Create I/O Completion Queue";
	case SPDK_NVME_OPC_IDENTIFY:
		return "Identify";
	case SPDK_NVME_OPC_ABORT:
		return "Abort";
	case SPDK_NVME_OPC_SET_FEATURES:
		return "Set Features";
	case SPDK_NVME_OPC_GET_FEATURES:
		return "Get Features";
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return "Asynchronous Event Request";
	case SPDK_NVME_OPC_NS_MANAGEMENT:
		return "Namespace Management";
	case SPDK_NVME_OPC_FIRMWARE_COMMIT:
		return "Firmware Commit";
	case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD:
		return "Firmware Image Download";
	case SPDK_NVME_OPC_DEVICE_SELF_TEST:
		return "Device Self-test";
	case SPDK_NVME_OPC_NS_ATTACHMENT:
		return "Namespace Attachment";
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return "Keep Alive";
	case SPDK_NVME_OPC_DIRECTIVE_SEND:
		return "Directive Send";
	case SPDK_NVME_OPC_DIRECTIVE_RECEIVE:
		return "Directive Receive";
	case SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT:
		return "Virtualization Management";
	case SPDK_NVME_OPC_NVME_MI_SEND:
		return "NVMe-MI Send";
	case SPDK_NVME_OPC_NVME_MI_RECEIVE:
		return "NVMe-MI Receive";
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		return "Doorbell Buffer Config";
	case SPDK_NVME_OPC_FORMAT_NVM:
		return "Format NVM";
	case SPDK_NVME_OPC_SECURITY_SEND:
		return "Security Send";
	case SPDK_NVME_OPC_SECURITY_RECEIVE:
		return "Security Receive";
	case SPDK_NVME_OPC_SANITIZE:
		return "Sanitize";
	default:
		if (opc >= 0xC0) {
			return "Vendor specific";
		}
		return "Unknown";
	}
}

static const char *
io_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_FLUSH:
		return "Flush";
	case SPDK_NVME_OPC_WRITE:
		return "Write";
	case SPDK_NVME_OPC_READ:
		return "Read";
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
		return "Write Uncorrectable";
	case SPDK_NVME_OPC_COMPARE:
		return "Compare";
	case SPDK_NVME_OPC_WRITE_ZEROES:
		return "Write Zeroes";
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return "Dataset Management";
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		return "Reservation Register";
	case SPDK_NVME_OPC_RESERVATION_REPORT:
		return "Reservation Report";
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		return "Reservation Acquire";
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		return "Reservation Release";
	default:
		if (opc >= 0x80) {
			return "Vendor specific";
		}
		return "Unknown";
	}
}

static void
print_controller(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
		 const struct spdk_nvme_ctrlr_opts *opts)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	union spdk_nvme_cap_register		cap;
	union spdk_nvme_vs_register		vs;
	union spdk_nvme_cmbsz_register		cmbsz;
	union spdk_nvme_pmrcap_register		pmrcap;
	uint8_t					str[512];
	uint32_t				i, j;
	struct spdk_nvme_error_information_entry *error_entry;
	struct spdk_pci_addr			pci_addr;
	struct spdk_pci_device			*pci_dev;
	struct spdk_pci_id			pci_id;
	uint32_t				nsid;
	uint64_t				pmrsz;
	uint8_t					*orig_desc;
	struct spdk_nvme_ana_group_descriptor	*copied_desc;
	uint32_t				desc_size, copy_len;


	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(ctrlr);
	cmbsz = spdk_nvme_ctrlr_get_regs_cmbsz(ctrlr);
	pmrcap = spdk_nvme_ctrlr_get_regs_pmrcap(ctrlr);
	pmrsz = spdk_nvme_ctrlr_get_pmrsz(ctrlr);

	if (!spdk_nvme_ctrlr_is_discovery(ctrlr)) {
		/*
		 * Discovery Controller only supports the
		 * IDENTIFY and GET_LOG_PAGE cmd set, so only
		 * attempt GET_FEATURES when NOT targeting a
		 * Discovery Controller.
		 */
		get_ctrlr_features(ctrlr);
	}
	get_log_pages(ctrlr);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid, trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr) != 0) {
			return;
		}

		pci_dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("NVMe Controller at %04x:%02x:%02x.%x [%04x:%04x]\n",
		       pci_addr.domain, pci_addr.bus,
		       pci_addr.dev, pci_addr.func,
		       pci_id.vendor_id, pci_id.device_id);
	}
	printf("=====================================================\n");

	if (g_hex_dump) {
		hex_dump(cdata, sizeof(*cdata));
		printf("\n");
	}

	printf("Controller Capabilities/Features\n");
	printf("================================\n");
	printf("Vendor ID:                             %04x\n", cdata->vid);
	printf("Subsystem Vendor ID:                   %04x\n", cdata->ssvid);
	printf("Serial Number:                         ");
	print_ascii_string(cdata->sn, sizeof(cdata->sn));
	printf("\n");
	printf("Model Number:                          ");
	print_ascii_string(cdata->mn, sizeof(cdata->mn));
	printf("\n");
	printf("Firmware Version:                      ");
	print_ascii_string(cdata->fr, sizeof(cdata->fr));
	printf("\n");
	printf("Recommended Arb Burst:                 %d\n", cdata->rab);
	printf("IEEE OUI Identifier:                   %02x %02x %02x\n",
	       cdata->ieee[0], cdata->ieee[1], cdata->ieee[2]);
	printf("Multi-path I/O\n");
	printf("  May have multiple subsystem ports:   %s\n", cdata->cmic.multi_port ? "Yes" : "No");
	printf("  May have multiple controllers:       %s\n", cdata->cmic.multi_ctrlr ? "Yes" : "No");
	printf("  Associated with SR-IOV VF:           %s\n", cdata->cmic.sr_iov ? "Yes" : "No");
	printf("Max Data Transfer Size:                ");
	if (cdata->mdts == 0) {
		printf("Unlimited\n");
	} else {
		printf("%" PRIu64 "\n", (uint64_t)1 << (12 + cap.bits.mpsmin + cdata->mdts));
	}
	printf("Max Number of Namespaces:              %d\n", cdata->nn);
	printf("Max Number of I/O Queues:              %d\n", opts->num_io_queues);
	printf("NVMe Specification Version (VS):       %u.%u", vs.bits.mjr, vs.bits.mnr);
	if (vs.bits.ter) {
		printf(".%u", vs.bits.ter);
	}
	printf("\n");
	if (cdata->ver.raw != 0) {
		printf("NVMe Specification Version (Identify): %u.%u", cdata->ver.bits.mjr, cdata->ver.bits.mnr);
		if (cdata->ver.bits.ter) {
			printf(".%u", cdata->ver.bits.ter);
		}
		printf("\n");
	}

	printf("Maximum Queue Entries:                 %u\n", cap.bits.mqes + 1);
	printf("Contiguous Queues Required:            %s\n", cap.bits.cqr ? "Yes" : "No");
	printf("Arbitration Mechanisms Supported\n");
	printf("  Weighted Round Robin:                %s\n",
	       cap.bits.ams & SPDK_NVME_CAP_AMS_WRR ? "Supported" : "Not Supported");
	printf("  Vendor Specific:                     %s\n",
	       cap.bits.ams & SPDK_NVME_CAP_AMS_VS ? "Supported" : "Not Supported");
	printf("Reset Timeout:                         %" PRIu64 " ms\n", (uint64_t)500 * cap.bits.to);
	printf("Doorbell Stride:                       %" PRIu64 " bytes\n",
	       (uint64_t)1 << (2 + cap.bits.dstrd));
	printf("NVM Subsystem Reset:                   %s\n",
	       cap.bits.nssrs ? "Supported" : "Not Supported");
	printf("Command Sets Supported\n");
	printf("  NVM Command Set:                     %s\n",
	       cap.bits.css & SPDK_NVME_CAP_CSS_NVM ? "Supported" : "Not Supported");
	printf("Boot Partition:                        %s\n",
	       cap.bits.bps ? "Supported" : "Not Supported");
	printf("Memory Page Size Minimum:              %" PRIu64 " bytes\n",
	       (uint64_t)1 << (12 + cap.bits.mpsmin));
	printf("Memory Page Size Maximum:              %" PRIu64 " bytes\n",
	       (uint64_t)1 << (12 + cap.bits.mpsmax));
	printf("Persistent Memory Region:              %s\n",
	       cap.bits.pmrs ? "Supported" : "Not Supported");

	printf("Optional Asynchronous Events Supported\n");
	printf("  Namespace Attribute Notices:         %s\n",
	       cdata->oaes.ns_attribute_notices ? "Supported" : "Not Supported");
	printf("  Firmware Activation Notices:         %s\n",
	       cdata->oaes.fw_activation_notices ? "Supported" : "Not Supported");
	printf("  ANA Change Notices:                  %s\n",
	       cdata->oaes.ana_change_notices ? "Supported" : "Not Supported");
	printf("  PLE Aggregate Log Change Notices:    %s\n",
	       cdata->oaes.pleal_change_notices ? "Supported" : "Not Supported");
	printf("  LBA Status Info Alert Notices:       %s\n",
	       cdata->oaes.lba_sia_notices ? "Supported" : "Not Supported");
	printf("  EGE Aggregate Log Change Notices:    %s\n",
	       cdata->oaes.egealp_change_notices ? "Supported" : "Not Supported");
	printf("  Normal NVM Subsystem Shutdown event: %s\n",
	       cdata->oaes.nnvm_sse ? "Supported" : "Not Supported");
	printf("  Zone Descriptor Change Notices:      %s\n",
	       cdata->oaes.zdes_change_notices ? "Supported" : "Not Supported");
	printf("  Discovery Log Change Notices:        %s\n",
	       cdata->oaes.discovery_log_change_notices ? "Supported" : "Not Supported");

	printf("Controller Attributes\n");
	printf("  128-bit Host Identifier:             %s\n",
	       cdata->ctratt.host_id_exhid_supported ? "Supported" : "Not Supported");
	printf("  Non-Operational Permissive Mode:     %s\n",
	       cdata->ctratt.non_operational_power_state_permissive_mode ? "Supported" : "Not Supported");
	printf("  NVM Sets:                            %s\n",
	       cdata->ctratt.nvm_sets ? "Supported" : "Not Supported");
	printf("  Read Recovery Levels:                %s\n",
	       cdata->ctratt.read_recovery_levels ? "Supported" : "Not Supported");
	printf("  Endurance Groups:                    %s\n",
	       cdata->ctratt.endurance_groups ? "Supported" : "Not Supported");
	printf("  Predictable Latency Mode:            %s\n",
	       cdata->ctratt.predictable_latency_mode ? "Supported" : "Not Supported");
	printf("  Traffic Based Keep ALive:            %s\n",
	       cdata->ctratt.tbkas ? "Supported" : "Not Supported");
	printf("  Namespace Granularity:               %s\n",
	       cdata->ctratt.namespace_granularity ? "Supported" : "Not Supported");
	printf("  SQ Associations:                     %s\n",
	       cdata->ctratt.sq_associations ? "Supported" : "Not Supported");
	printf("  UUID List:                           %s\n",
	       cdata->ctratt.uuid_list ? "Supported" : "Not Supported");
	printf("  Multi-Domain Subsystem:              %s\n",
	       cdata->ctratt.mds ? "Supported" : "Not Supported");
	printf("  Fixed Capacity Management:           %s\n",
	       cdata->ctratt.fixed_capacity_management ? "Supported" : "Not Supported");
	printf("  Variable Capacity Management:        %s\n",
	       cdata->ctratt.variable_capacity_management ? "Supported" : "Not Supported");
	printf("  Delete Endurance Group:              %s\n",
	       cdata->ctratt.delete_endurance_group ? "Supported" : "Not Supported");
	printf("  Delete NVM Set:                      %s\n",
	       cdata->ctratt.delete_nvm_set ? "Supported" : "Not Supported");
	printf("  Extended LBA Formats Supported:      %s\n",
	       cdata->ctratt.elbas ? "Supported" : "Not Supported");
	printf("  Flexible Data Placement Supported:   %s\n",
	       cdata->ctratt.fdps ? "Supported" : "Not Supported");
	printf("\n");

	printf("Controller Memory Buffer Support\n");
	printf("================================\n");
	if (cmbsz.raw != 0) {
		uint64_t size = cmbsz.bits.sz;

		/* Convert the size to bytes by multiplying by the granularity.
		   By spec, szu is at most 6 and sz is 20 bits, so size requires
		   at most 56 bits. */
		size *= (0x1000 << (cmbsz.bits.szu * 4));

		printf("Supported:                             Yes\n");
		printf("Total Size:                            %" PRIu64 " bytes\n", size);
		printf("Submission Queues in CMB:              %s\n",
		       cmbsz.bits.sqs ? "Supported" : "Not Supported");
		printf("Completion Queues in CMB:              %s\n",
		       cmbsz.bits.cqs ? "Supported" : "Not Supported");
		printf("Read data and metadata in CMB          %s\n",
		       cmbsz.bits.rds ? "Supported" : "Not Supported");
		printf("Write data and metadata in CMB:        %s\n",
		       cmbsz.bits.wds ? "Supported" : "Not Supported");
	} else {
		printf("Supported:                             No\n");
	}
	printf("\n");

	printf("Persistent Memory Region Support\n");
	printf("================================\n");
	if (cap.bits.pmrs != 0) {
		printf("Supported:                             Yes\n");
		printf("Total Size:                            %" PRIu64 " bytes\n", pmrsz);
		printf("Read data and metadata in PMR          %s\n",
		       pmrcap.bits.rds ? "Supported" : "Not Supported");
		printf("Write data and metadata in PMR:        %s\n",
		       pmrcap.bits.wds ? "Supported" : "Not Supported");
	} else {
		printf("Supported:                             No\n");
	}
	printf("\n");

	printf("Admin Command Set Attributes\n");
	printf("============================\n");
	printf("Security Send/Receive:                 %s\n",
	       cdata->oacs.security ? "Supported" : "Not Supported");
	printf("Format NVM:                            %s\n",
	       cdata->oacs.format ? "Supported" : "Not Supported");
	printf("Firmware Activate/Download:            %s\n",
	       cdata->oacs.firmware ? "Supported" : "Not Supported");
	printf("Namespace Management:                  %s\n",
	       cdata->oacs.ns_manage ? "Supported" : "Not Supported");
	printf("Device Self-Test:                      %s\n",
	       cdata->oacs.device_self_test ? "Supported" : "Not Supported");
	printf("Directives:                            %s\n",
	       cdata->oacs.directives ? "Supported" : "Not Supported");
	printf("NVMe-MI:                               %s\n",
	       cdata->oacs.nvme_mi ? "Supported" : "Not Supported");
	printf("Virtualization Management:             %s\n",
	       cdata->oacs.virtualization_management ? "Supported" : "Not Supported");
	printf("Doorbell Buffer Config:                %s\n",
	       cdata->oacs.doorbell_buffer_config ? "Supported" : "Not Supported");
	printf("Get LBA Status Capability:             %s\n",
	       cdata->oacs.get_lba_status ? "Supported" : "Not Supported");
	printf("Command & Feature Lockdown Capability: %s\n",
	       cdata->oacs.doorbell_buffer_config ? "Supported" : "Not Supported");
	printf("Abort Command Limit:                   %d\n", cdata->acl + 1);
	printf("Async Event Request Limit:             %d\n", cdata->aerl + 1);
	printf("Number of Firmware Slots:              ");
	if (cdata->oacs.firmware != 0) {
		printf("%d\n", cdata->frmw.num_slots);
	} else {
		printf("N/A\n");
	}
	printf("Firmware Slot 1 Read-Only:             ");
	if (cdata->oacs.firmware != 0) {
		printf("%s\n", cdata->frmw.slot1_ro ? "Yes" : "No");
	} else {
		printf("N/A\n");
	}
	printf("Firmware Activation Without Reset:     ");
	if (cdata->oacs.firmware != 0) {
		printf("%s\n", cdata->frmw.activation_without_reset ? "Yes" : "No");
	} else {
		printf("N/A\n");
	}
	printf("Multiple Update Detection Support:     ");
	if (cdata->oacs.firmware != 0) {
		printf("%s\n", cdata->frmw.multiple_update_detection ? "Yes" : "No");
	} else {
		printf("N/A\n");
	}
	if (cdata->fwug == 0x00) {
		printf("Firmware Update Granularity:           No Information Provided\n");
	} else if (cdata->fwug == 0xFF) {
		printf("Firmware Update Granularity:           No Restriction\n");
	} else {
		printf("Firmware Update Granularity:           %u KiB\n",
		       cdata->fwug * 4);
	}
	printf("Per-Namespace SMART Log:               %s\n",
	       cdata->lpa.ns_smart ? "Yes" : "No");
	if (cdata->cmic.ana_reporting == 0) {
		printf("Asymmetric Namespace Access Log Page:  Not Supported\n");
	} else {
		printf("Asymmetric Namespace Access Log Page:  Supported\n");
		printf("ANA Transition Time                 :  %u sec\n", cdata->anatt);
		printf("\n");
		printf("Asymmetric Namespace Access Capabilities\n");
		printf("  ANA Optimized State               : %s\n",
		       cdata->anacap.ana_optimized_state ? "Supported" : "Not Supported");
		printf("  ANA Non-Optimized State           : %s\n",
		       cdata->anacap.ana_non_optimized_state ? "Supported" : "Not Supported");
		printf("  ANA Inaccessible State            : %s\n",
		       cdata->anacap.ana_inaccessible_state ? "Supported" : "Not Supported");
		printf("  ANA Persistent Loss State         : %s\n",
		       cdata->anacap.ana_persistent_loss_state ? "Supported" : "Not Supported");
		printf("  ANA Change State                  : %s\n",
		       cdata->anacap.ana_change_state ? "Supported" : "Not Supported");
		printf("  ANAGRPID is not changed           : %s\n",
		       cdata->anacap.no_change_anagrpid ? "Yes" : "No");
		printf("  Non-Zero ANAGRPID for NS Mgmt Cmd : %s\n",
		       cdata->anacap.non_zero_anagrpid ? "Supported" : "Not Supported");
		printf("\n");
		printf("ANA Group Identifier Maximum        : %u\n", cdata->anagrpmax);
		printf("Number of ANA Group Identifiers     : %u\n", cdata->nanagrpid);
		printf("Max Number of Allowed Namespaces    : %u\n", cdata->mnan);
	}
	printf("Command Effects Log Page:              %s\n",
	       cdata->lpa.celp ? "Supported" : "Not Supported");
	printf("Get Log Page Extended Data:            %s\n",
	       cdata->lpa.edlp ? "Supported" : "Not Supported");
	printf("Telemetry Log Pages:                   %s\n",
	       cdata->lpa.telemetry ? "Supported" : "Not Supported");
	printf("Persistent Event Log Pages:            %s\n",
	       cdata->lpa.pelp ? "Supported" : "Not Supported");
	printf("Supported Log Pages Log Page:          %s\n",
	       cdata->lpa.lplp ? "Supported" : "May Support");
	printf("Commands Supported & Effects Log Page: %s\n",
	       cdata->lpa.lplp ? "Supported" : "Not Supported");
	printf("Feature Identifiers & Effects Log Page:%s\n",
	       cdata->lpa.lplp ? "Supported" : "May Support");
	printf("NVMe-MI Commands & Effects Log Page:   %s\n",
	       cdata->lpa.lplp ? "Supported" : "May Support");
	printf("Data Area 4 for Telemetry Log:         %s\n",
	       cdata->lpa.da4_telemetry ? "Supported" : "Not Supported");
	printf("Error Log Page Entries Supported:      %d\n", cdata->elpe + 1);
	if (cdata->kas == 0) {
		printf("Keep Alive:                            Not Supported\n");
	} else {
		printf("Keep Alive:                            Supported\n");
		printf("Keep Alive Granularity:                %u ms\n",
		       cdata->kas * 100);
	}
	printf("\n");

	printf("NVM Command Set Attributes\n");
	printf("==========================\n");
	printf("Submission Queue Entry Size\n");
	printf("  Max:                       %d\n", 1 << cdata->sqes.max);
	printf("  Min:                       %d\n", 1 << cdata->sqes.min);
	printf("Completion Queue Entry Size\n");
	printf("  Max:                       %d\n", 1 << cdata->cqes.max);
	printf("  Min:                       %d\n", 1 << cdata->cqes.min);
	printf("Number of Namespaces:        %d\n", cdata->nn);
	printf("Compare Command:             %s\n",
	       cdata->oncs.compare ? "Supported" : "Not Supported");
	printf("Write Uncorrectable Command: %s\n",
	       cdata->oncs.write_unc ? "Supported" : "Not Supported");
	printf("Dataset Management Command:  %s\n",
	       cdata->oncs.dsm ? "Supported" : "Not Supported");
	printf("Write Zeroes Command:        %s\n",
	       cdata->oncs.write_zeroes ? "Supported" : "Not Supported");
	printf("Set Features Save Field:     %s\n",
	       cdata->oncs.set_features_save ? "Supported" : "Not Supported");
	printf("Reservations:                %s\n",
	       cdata->oncs.reservations ? "Supported" : "Not Supported");
	printf("Timestamp:                   %s\n",
	       cdata->oncs.timestamp ? "Supported" : "Not Supported");
	printf("Copy:                        %s\n",
	       cdata->oncs.copy ? "Supported" : "Not Supported");
	printf("Volatile Write Cache:        %s\n",
	       cdata->vwc.present ? "Present" : "Not Present");
	printf("Atomic Write Unit (Normal):  %d\n", cdata->awun + 1);
	printf("Atomic Write Unit (PFail):   %d\n", cdata->awupf + 1);
	printf("Atomic Compare & Write Unit: %d\n", cdata->acwu + 1);
	printf("Fused Compare & Write:       %s\n",
	       cdata->fuses.compare_and_write ? "Supported" : "Not Supported");
	printf("Scatter-Gather List\n");
	printf("  SGL Command Set:           %s\n",
	       cdata->sgls.supported == SPDK_NVME_SGLS_SUPPORTED ? "Supported" :
	       cdata->sgls.supported == SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED ? "Supported (Dword aligned)" :
	       "Not Supported");
	printf("  SGL Keyed:                 %s\n",
	       cdata->sgls.keyed_sgl ? "Supported" : "Not Supported");
	printf("  SGL Bit Bucket Descriptor: %s\n",
	       cdata->sgls.bit_bucket_descriptor ? "Supported" : "Not Supported");
	printf("  SGL Metadata Pointer:      %s\n",
	       cdata->sgls.metadata_pointer ? "Supported" : "Not Supported");
	printf("  Oversized SGL:             %s\n",
	       cdata->sgls.oversized_sgl ? "Supported" : "Not Supported");
	printf("  SGL Metadata Address:      %s\n",
	       cdata->sgls.metadata_address ? "Supported" : "Not Supported");
	printf("  SGL Offset:                %s\n",
	       cdata->sgls.sgl_offset ? "Supported" : "Not Supported");
	printf("  Transport SGL Data Block:  %s\n",
	       cdata->sgls.transport_sgl ? "Supported" : "Not Supported");
	printf("Replay Protected Memory Block:");
	if (cdata->rpmbs.num_rpmb_units > 0) {
		printf("  Supported\n");
		printf("  Number of RPMB Units:  %d\n", cdata->rpmbs.num_rpmb_units);
		printf("  Authentication Method: %s\n", cdata->rpmbs.auth_method == 0 ? "HMAC SHA-256" : "Unknown");
		printf("  Total Size (in 128KB units) = %d\n", cdata->rpmbs.total_size + 1);
		printf("  Access Size (in 512B units) = %d\n", cdata->rpmbs.access_size + 1);
	} else {
		printf("  Not Supported\n");
	}
	if (cdata->crdt[0]) {
		printf("Command Retry Delay Time 1:  %u milliseconds\n", cdata->crdt[0] * 100);
	}
	if (cdata->crdt[1]) {
		printf("Command Retry Delay Time 2:  %u milliseconds\n", cdata->crdt[1] * 100);
	}
	if (cdata->crdt[2]) {
		printf("Command Retry Delay Time 3:  %u milliseconds\n", cdata->crdt[2] * 100);
	}
	printf("\n");

	printf("Firmware Slot Information\n");
	printf("=========================\n");
	if (g_hex_dump) {
		hex_dump(&firmware_page, sizeof(firmware_page));
		printf("\n");
	}
	printf("Active slot:                 %u\n", firmware_page.afi.active_slot);
	if (firmware_page.afi.next_reset_slot) {
		printf("Next controller reset slot:  %u\n", firmware_page.afi.next_reset_slot);
	}
	for (i = 0; i < 7; i++) {
		if (!spdk_mem_all_zero(firmware_page.revision[i], sizeof(firmware_page.revision[i]))) {
			printf("Slot %u Firmware Revision:    ", i + 1);
			print_ascii_string(firmware_page.revision[i], sizeof(firmware_page.revision[i]));
			printf("\n");
		}
	}
	printf("\n");

	if (g_ana_log_page) {
		printf("Asymmetric Namespace Access\n");
		printf("===========================\n");
		if (g_hex_dump) {
			hex_dump(g_ana_log_page, g_ana_log_page_size);
			printf("\n");
		}

		printf("Change Count                    : %" PRIx64 "\n", g_ana_log_page->change_count);
		printf("Number of ANA Group Descriptors : %u\n", g_ana_log_page->num_ana_group_desc);

		copied_desc = g_copied_ana_desc;
		orig_desc = (uint8_t *)g_ana_log_page + sizeof(struct spdk_nvme_ana_page);
		copy_len = g_ana_log_page_size - sizeof(struct spdk_nvme_ana_page);

		for (i = 0; i < g_ana_log_page->num_ana_group_desc; i++) {
			memcpy(copied_desc, orig_desc, copy_len);

			printf("ANA Group Descriptor            : %u\n", i);
			printf("  ANA Group ID                  : %u\n", copied_desc->ana_group_id);
			printf("  Number of NSID Values         : %u\n", copied_desc->num_of_nsid);
			printf("  Change Count                  : %" PRIx64 "\n", copied_desc->change_count);
			printf("  ANA State                     : %u\n", copied_desc->ana_state);
			for (j = 0; j < copied_desc->num_of_nsid; j++) {
				printf("  Namespace Identifier          : %u\n", copied_desc->nsid[j]);
			}

			desc_size = sizeof(struct spdk_nvme_ana_group_descriptor) +
				    copied_desc->num_of_nsid * sizeof(uint32_t);
			orig_desc += desc_size;
			copy_len -= desc_size;
		}
		free(g_ana_log_page);
		free(g_copied_ana_desc);
	}

	printf("\n");

	if (cdata->lpa.celp) {
		printf("Commands Supported and Effects\n");
		printf("==============================\n");

		if (g_hex_dump) {
			hex_dump(&cmd_effects_log_page, sizeof(cmd_effects_log_page));
			printf("\n");
		}

		printf("Admin Commands\n");
		printf("--------------\n");
		for (i = 0; i < SPDK_COUNTOF(cmd_effects_log_page.admin_cmds_supported); i++) {
			struct spdk_nvme_cmds_and_effect_entry *cmd = &cmd_effects_log_page.admin_cmds_supported[i];
			if (cmd->csupp) {
				printf("%30s (%02Xh): Supported %s%s%s%s%s\n",
				       admin_opc_name(i), i,
				       cmd->lbcc ? "LBA-Change " : "",
				       cmd->ncc ? "NS-Cap-Change " : "",
				       cmd->nic ? "NS-Inventory-Change " : "",
				       cmd->ccc ? "Ctrlr-Cap-Change " : "",
				       cmd->cse == 0 ? "" : cmd->cse == 1 ? "Per-NS-Exclusive" : cmd->cse == 2 ? "All-NS-Exclusive" : "");
			}
		}

		printf("I/O Commands\n");
		printf("------------\n");
		for (i = 0; i < SPDK_COUNTOF(cmd_effects_log_page.io_cmds_supported); i++) {
			struct spdk_nvme_cmds_and_effect_entry *cmd = &cmd_effects_log_page.io_cmds_supported[i];
			if (cmd->csupp) {
				printf("%30s (%02Xh): Supported %s%s%s%s%s\n",
				       io_opc_name(i), i,
				       cmd->lbcc ? "LBA-Change " : "",
				       cmd->ncc ? "NS-Cap-Change " : "",
				       cmd->nic ? "NS-Inventory-Change " : "",
				       cmd->ccc ? "Ctrlr-Cap-Change " : "",
				       cmd->cse == 0 ? "" : cmd->cse == 1 ? "Per-NS-Exclusive" : cmd->cse == 2 ? "All-NS-Exclusive" : "");
			}
		}
		printf("\n");
	}

	printf("Error Log\n");
	printf("=========\n");
	for (i = 0; i <= cdata->elpe; i++) {
		error_entry = &error_page[i];
		if (error_entry->error_count == 0) {
			continue;
		}
		if (i != 0) {
			printf("-----------\n");
		}

		printf("Entry: %u\n", i);
		printf("Error Count:            0x%"PRIx64"\n", error_entry->error_count);
		printf("Submission Queue Id:    0x%x\n", error_entry->sqid);
		printf("Command Id:             0x%x\n", error_entry->cid);
		printf("Phase Bit:              %x\n", error_entry->status.p);
		printf("Status Code:            0x%x\n", error_entry->status.sc);
		printf("Status Code Type:       0x%x\n", error_entry->status.sct);
		printf("Do Not Retry:           %x\n", error_entry->status.dnr);
		printf("Error Location:         0x%x\n", error_entry->error_location);
		printf("LBA:                    0x%"PRIx64"\n", error_entry->lba);
		printf("Namespace:              0x%x\n", error_entry->nsid);
		printf("Vendor Log Page:        0x%x\n", error_entry->vendor_specific);

	}
	printf("\n");

	if (features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		uint32_t arb = features[SPDK_NVME_FEAT_ARBITRATION].result;
		unsigned ab, lpw, mpw, hpw;

		ab = arb & 0x7;
		lpw = ((arb >> 8) & 0xFF) + 1;
		mpw = ((arb >> 16) & 0xFF) + 1;
		hpw = ((arb >> 24) & 0xFF) + 1;

		printf("Arbitration\n");
		printf("===========\n");
		printf("Arbitration Burst:           ");
		if (ab == 0x7) {
			printf("no limit\n");
		} else {
			printf("%u\n", 1u << ab);
		}

		if (cap.bits.ams & SPDK_NVME_CAP_AMS_WRR) {
			printf("Low Priority Weight:         %u\n", lpw);
			printf("Medium Priority Weight:      %u\n", mpw);
			printf("High Priority Weight:        %u\n", hpw);
		}
		printf("\n");
	}

	if (features[SPDK_NVME_FEAT_POWER_MANAGEMENT].valid) {
		unsigned ps = features[SPDK_NVME_FEAT_POWER_MANAGEMENT].result & 0x1F;
		printf("Power Management\n");
		printf("================\n");
		printf("Number of Power States:          %u\n", cdata->npss + 1);
		printf("Current Power State:             Power State #%u\n", ps);
		for (i = 0; i <= cdata->npss; i++) {
			const struct spdk_nvme_power_state psd = cdata->psd[i];
			printf("Power State #%u:\n", i);
			if (psd.mps) {
				/* MP scale is 0.0001 W */
				printf("  Max Power:                    %u.%04u W\n",
				       psd.mp / 10000,
				       psd.mp % 10000);
			} else {
				/* MP scale is 0.01 W */
				printf("  Max Power:                    %3u.%02u W\n",
				       psd.mp / 100,
				       psd.mp % 100);
			}
			printf("  Non-Operational State:         %s\n",
			       psd.nops ? "Non-Operation" : "Operational");
			printf("  Entry Latency:                 ");
			if (psd.enlat) {
				printf("%u microseconds\n", psd.enlat);
			} else {
				printf("Not Reported\n");
			}
			printf("  Exit Latency:                  ");
			if (psd.exlat) {
				printf("%u microseconds\n", psd.exlat);
			} else {
				printf("Not Reported\n");
			}
			printf("  Relative Read Throughput:      %u\n", psd.rrt);
			printf("  Relative Read Latency:         %u\n", psd.rrl);
			printf("  Relative Write Throughput:     %u\n", psd.rwt);
			printf("  Relative Write Latency:        %u\n", psd.rwl);
			printf("  Idle Power:                    ");
			switch (psd.ips) {
			case 1:
				/* Idle Power scale is 0.0001 W */
				printf("%u.%04u W\n", psd.idlp / 10000, psd.idlp % 10000);
				break;
			case 2:
				/* Idle Power scale is 0.01 W */
				printf("%u.%02u W\n", psd.idlp / 100, psd.idlp % 100);
				break;
			default:
				printf(" Not Reported\n");
			}
			printf("  Active Power:                  ");
			switch (psd.aps) {
			case 1:
				/* Active Power scale is 0.0001 W */
				printf("%u.%04u W\n", psd.actp / 10000, psd.actp % 10000);
				break;
			case 2:
				/* Active Power scale is 0.01 W */
				printf("%u.%02u W\n", psd.actp / 100, psd.actp % 100);
				break;
			default:
				printf(" Not Reported\n");
			}
		}
		printf("Non-Operational Permissive Mode: %s\n",
		       cdata->ctratt.non_operational_power_state_permissive_mode ? "Supported" : "Not Supported");
		printf("\n");
	}

	if (features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].valid) {
		printf("Health Information\n");
		printf("==================\n");

		if (g_hex_dump) {
			hex_dump(&health_page, sizeof(health_page));
			printf("\n");
		}

		printf("Critical Warnings:\n");
		printf("  Available Spare Space:     %s\n",
		       health_page.critical_warning.bits.available_spare ? "WARNING" : "OK");
		printf("  Temperature:               %s\n",
		       health_page.critical_warning.bits.temperature ? "WARNING" : "OK");
		printf("  Device Reliability:        %s\n",
		       health_page.critical_warning.bits.device_reliability ? "WARNING" : "OK");
		printf("  Read Only:                 %s\n",
		       health_page.critical_warning.bits.read_only ? "Yes" : "No");
		printf("  Volatile Memory Backup:    %s\n",
		       health_page.critical_warning.bits.volatile_memory_backup ? "WARNING" : "OK");
		printf("Current Temperature:         %u Kelvin (%d Celsius)\n",
		       health_page.temperature,
		       (int)health_page.temperature - 273);
		printf("Temperature Threshold:       %u Kelvin (%d Celsius)\n",
		       features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result,
		       (int)features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result - 273);
		printf("Available Spare:             %u%%\n", health_page.available_spare);
		printf("Available Spare Threshold:   %u%%\n", health_page.available_spare_threshold);
		printf("Life Percentage Used:        %u%%\n", health_page.percentage_used);
		printf("Data Units Read:             ");
		print_uint128_dec(health_page.data_units_read);
		printf("\n");
		printf("Data Units Written:          ");
		print_uint128_dec(health_page.data_units_written);
		printf("\n");
		printf("Host Read Commands:          ");
		print_uint128_dec(health_page.host_read_commands);
		printf("\n");
		printf("Host Write Commands:         ");
		print_uint128_dec(health_page.host_write_commands);
		printf("\n");
		printf("Controller Busy Time:        ");
		print_uint128_dec(health_page.controller_busy_time);
		printf(" minutes\n");
		printf("Power Cycles:                ");
		print_uint128_dec(health_page.power_cycles);
		printf("\n");
		printf("Power On Hours:              ");
		print_uint128_dec(health_page.power_on_hours);
		printf(" hours\n");
		printf("Unsafe Shutdowns:            ");
		print_uint128_dec(health_page.unsafe_shutdowns);
		printf("\n");
		printf("Unrecoverable Media Errors:  ");
		print_uint128_dec(health_page.media_errors);
		printf("\n");
		printf("Lifetime Error Log Entries:  ");
		print_uint128_dec(health_page.num_error_info_log_entries);
		printf("\n");
		printf("Warning Temperature Time:    %u minutes\n", health_page.warning_temp_time);
		printf("Critical Temperature Time:   %u minutes\n", health_page.critical_temp_time);
		for (i = 0; i < 8; i++) {
			if (health_page.temp_sensor[i] != 0) {
				printf("Temperature Sensor %d:        %u Kelvin (%d Celsius)\n",
				       i + 1, health_page.temp_sensor[i],
				       (int)health_page.temp_sensor[i] - 273);
			}
		}
		printf("\n");
	}

	if (features[SPDK_NVME_FEAT_NUMBER_OF_QUEUES].valid) {
		uint32_t result = features[SPDK_NVME_FEAT_NUMBER_OF_QUEUES].result;

		printf("Number of Queues\n");
		printf("================\n");
		printf("Number of I/O Submission Queues:      %u\n", (result & 0xFFFF) + 1);
		printf("Number of I/O Completion Queues:      %u\n", (result & 0xFFFF0000 >> 16) + 1);
		printf("\n");
	}

	if (features[SPDK_OCSSD_FEAT_MEDIA_FEEDBACK].valid) {
		uint32_t result = features[SPDK_OCSSD_FEAT_MEDIA_FEEDBACK].result;

		printf("OCSSD Media Feedback\n");
		printf("=======================\n");
		printf("High ECC status:                %u\n", (result & 0x1));
		printf("Vector High ECC status:         %u\n", (result & 0x2 >> 1));
		printf("\n");
	}

	if (cdata->hctma.bits.supported) {
		printf("Host Controlled Thermal Management\n");
		printf("==================================\n");
		printf("Minimum Thermal Management Temperature:  ");
		if (cdata->mntmt) {
			printf("%u Kelvin (%d Celsius)\n", cdata->mntmt, (int)cdata->mntmt - 273);
		} else {
			printf("Not Reported\n");
		}
		printf("Maximum Thermal Management Temperature:   ");
		if (cdata->mxtmt) {
			printf("%u Kelvin (%d Celsius)\n", cdata->mxtmt, (int)cdata->mxtmt - 273);
		} else {
			printf("Not Reported\n");
		}
		printf("\n");
	}

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_LOG_SMART)) {
		size_t i = 0;

		printf("Intel Health Information\n");
		printf("==================\n");
		for (i = 0;
		     i < SPDK_COUNTOF(intel_smart_page.attributes); i++) {
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_PROGRAM_FAIL_COUNT) {
				printf("Program Fail Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_ERASE_FAIL_COUNT) {
				printf("Erase Fail Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_WEAR_LEVELING_COUNT) {
				printf("Wear Leveling Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value:\n");
				printf("  Min: ");
				print_uint_var_dec(&intel_smart_page.attributes[i].raw_value[0], 2);
				printf("\n");
				printf("  Max: ");
				print_uint_var_dec(&intel_smart_page.attributes[i].raw_value[2], 2);
				printf("\n");
				printf("  Avg: ");
				print_uint_var_dec(&intel_smart_page.attributes[i].raw_value[4], 2);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_E2E_ERROR_COUNT) {
				printf("End to End Error Detection Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_CRC_ERROR_COUNT) {
				printf("CRC Error Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_MEDIA_WEAR) {
				printf("Timed Workload, Media Wear:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_HOST_READ_PERCENTAGE) {
				printf("Timed Workload, Host Read/Write Ratio:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("%%");
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_TIMER) {
				printf("Timed Workload, Timer:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_THERMAL_THROTTLE_STATUS) {
				printf("Thermal Throttle Status:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value:\n");
				printf("  Percentage: %d%%\n", intel_smart_page.attributes[i].raw_value[0]);
				printf("  Throttling Event Count: ");
				print_uint_var_dec(&intel_smart_page.attributes[i].raw_value[1], 4);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_RETRY_BUFFER_OVERFLOW_COUNTER) {
				printf("Retry Buffer Overflow Counter:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_PLL_LOCK_LOSS_COUNT) {
				printf("PLL Lock Loss Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_NAND_BYTES_WRITTEN) {
				printf("NAND Bytes Written:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page.attributes[i].code == SPDK_NVME_INTEL_SMART_HOST_BYTES_WRITTEN) {
				printf("Host Bytes Written:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page.attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page.attributes[i].raw_value, 6);
				printf("\n");
			}
		}
		printf("\n");
	}

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE)) {
		printf("Intel Temperature Information\n");
		printf("==================\n");
		printf("Current Temperature: %" PRIu64 "\n", intel_temperature_page.current_temperature);
		printf("Overtemp shutdown Flag for last critical component temperature: %" PRIu64 "\n",
		       intel_temperature_page.shutdown_flag_last);
		printf("Overtemp shutdown Flag for life critical component temperature: %" PRIu64 "\n",
		       intel_temperature_page.shutdown_flag_life);
		printf("Highest temperature: %" PRIu64 "\n", intel_temperature_page.highest_temperature);
		printf("Lowest temperature: %" PRIu64 "\n", intel_temperature_page.lowest_temperature);
		printf("Specified Maximum Operating Temperature: %" PRIu64 "\n",
		       intel_temperature_page.specified_max_op_temperature);
		printf("Specified Minimum Operating Temperature: %" PRIu64 "\n",
		       intel_temperature_page.specified_min_op_temperature);
		printf("Estimated offset: %" PRId64 "\n", (int64_t)intel_temperature_page.estimated_offset);
		printf("\n");
		printf("\n");

	}

	if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr, SPDK_NVME_INTEL_MARKETING_DESCRIPTION)) {
		printf("Intel Marketing Information\n");
		printf("==================\n");
		snprintf(str, sizeof(intel_md_page.marketing_product), "%s", intel_md_page.marketing_product);
		printf("Marketing Product Information:		%s\n", str);
		printf("\n");
		printf("\n");
	}

	if (spdk_nvme_zns_ctrlr_get_data(ctrlr)) {
		printf("ZNS Specific Controller Data\n");
		printf("============================\n");
		printf("Zone Append Size Limit:      %u\n",
		       spdk_nvme_zns_ctrlr_get_data(ctrlr)->zasl);
		printf("\n");
		printf("\n");
	}

	printf("Active Namespaces\n");
	printf("=================\n");
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		get_ns_features(ctrlr, nsid);
		print_namespace(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}

	if (g_discovery_page) {
		printf("Discovery Log Page\n");
		printf("==================\n");

		if (g_hex_dump) {
			hex_dump(g_discovery_page, g_discovery_page_size);
			printf("\n");
		}

		printf("Generation Counter:                    %" PRIu64 "\n",
		       from_le64(&g_discovery_page->genctr));
		printf("Number of Records:                     %" PRIu64 "\n",
		       from_le64(&g_discovery_page->numrec));
		printf("Record Format:                         %" PRIu16 "\n",
		       from_le16(&g_discovery_page->recfmt));
		printf("\n");

		for (i = 0; i < g_discovery_page_numrec; i++) {
			struct spdk_nvmf_discovery_log_page_entry *entry = &g_discovery_page->entries[i];

			printf("Discovery Log Entry %u\n", i);
			printf("----------------------\n");
			printf("Transport Type:                        %u (%s)\n",
			       entry->trtype, spdk_nvme_transport_id_trtype_str(entry->trtype));
			printf("Address Family:                        %u (%s)\n",
			       entry->adrfam, spdk_nvme_transport_id_adrfam_str(entry->adrfam));
			printf("Subsystem Type:                        %u (%s)\n",
			       entry->subtype,
			       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY ? "Discovery Service" :
			       entry->subtype == SPDK_NVMF_SUBTYPE_NVME ? "NVM Subsystem" :
			       "Unknown");
			printf("Transport Requirements:\n");
			printf("  Secure Channel:                      %s\n",
			       entry->treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED ? "Not Specified" :
			       entry->treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED ? "Required" :
			       entry->treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED ? "Not Required" :
			       "Reserved");
			printf("Port ID:                               %" PRIu16 " (0x%04" PRIx16 ")\n",
			       from_le16(&entry->portid), from_le16(&entry->portid));
			printf("Controller ID:                         %" PRIu16 " (0x%04" PRIx16 ")\n",
			       from_le16(&entry->cntlid), from_le16(&entry->cntlid));
			printf("Admin Max SQ Size:                     %" PRIu16 "\n",
			       from_le16(&entry->asqsz));
			snprintf(str, sizeof(entry->trsvcid) + 1, "%s", entry->trsvcid);
			printf("Transport Service Identifier:          %s\n", str);
			snprintf(str, sizeof(entry->subnqn) + 1, "%s", entry->subnqn);
			printf("NVM Subsystem Qualified Name:          %s\n", str);
			snprintf(str, sizeof(entry->traddr) + 1, "%s", entry->traddr);
			printf("Transport Address:                     %s\n", str);

			if (entry->trtype == SPDK_NVMF_TRTYPE_RDMA) {
				printf("Transport Specific Address Subtype - RDMA\n");
				printf("  RDMA QP Service Type:                %u (%s)\n",
				       entry->tsas.rdma.rdma_qptype,
				       entry->tsas.rdma.rdma_qptype == SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED ? "Reliable Connected" :
				       entry->tsas.rdma.rdma_qptype == SPDK_NVMF_RDMA_QPTYPE_RELIABLE_DATAGRAM ? "Reliable Datagram" :
				       "Unknown");
				printf("  RDMA Provider Type:                  %u (%s)\n",
				       entry->tsas.rdma.rdma_prtype,
				       entry->tsas.rdma.rdma_prtype == SPDK_NVMF_RDMA_PRTYPE_NONE ? "No provider specified" :
				       entry->tsas.rdma.rdma_prtype == SPDK_NVMF_RDMA_PRTYPE_IB ? "InfiniBand" :
				       entry->tsas.rdma.rdma_prtype == SPDK_NVMF_RDMA_PRTYPE_ROCE ? "InfiniBand RoCE" :
				       entry->tsas.rdma.rdma_prtype == SPDK_NVMF_RDMA_PRTYPE_ROCE2 ? "InfiniBand RoCE v2" :
				       entry->tsas.rdma.rdma_prtype == SPDK_NVMF_RDMA_PRTYPE_IWARP ? "iWARP" :
				       "Unknown");
				printf("  RDMA CM Service:                     %u (%s)\n",
				       entry->tsas.rdma.rdma_cms,
				       entry->tsas.rdma.rdma_cms == SPDK_NVMF_RDMA_CMS_RDMA_CM ? "RDMA_CM" :
				       "Unknown");
				if (entry->adrfam == SPDK_NVMF_ADRFAM_IB) {
					printf("  RDMA Partition Key:                  %" PRIu32 "\n",
					       from_le32(&entry->tsas.rdma.rdma_pkey));
				}
			}
		}
		free(g_discovery_page);
		g_discovery_page = NULL;
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r trid    remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("     hostnqn     Host NQN\n");
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");

	spdk_log_usage(stdout, "-L");

	printf(" -i         shared memory group ID\n");
	printf(" -p         core number in decimal to run this application which started from 0\n");
	printf(" -d         DPDK huge memory size in MB\n");
	printf(" -g         use single file descriptor for DPDK memory segments\n");
	printf(" -x         print hex dump of raw data\n");
	printf(" -z         For NVMe Zoned Namespaces, dump the full zone report (-z) or the first N entries (-z N)\n");
	printf(" -V         enumerate VMD\n");
	printf(" -H         show this usage\n");
}

static int
parse_args(int argc, char **argv)
{
	int op, rc;
	char *hostnqn;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:gi:op:r:xz::HL:V")) != -1) {
		switch (op) {
		case 'd':
			g_dpdk_mem = spdk_strtol(optarg, 10);
			if (g_dpdk_mem < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return g_dpdk_mem;
			}
			break;
		case 'g':
			g_dpdk_mem_single_seg = true;
			break;
		case 'i':
			g_shm_id = spdk_strtol(optarg, 10);
			if (g_shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return g_shm_id;
			}
			break;
		case 'o':
			g_ocssd_verbose = true;
			break;
		case 'p':
			g_main_core = spdk_strtol(optarg, 10);
			if (g_main_core < 0) {
				fprintf(stderr, "Invalid core number\n");
				return g_main_core;
			}
			snprintf(g_core_mask, sizeof(g_core_mask), "0x%llx", 1ULL << g_main_core);
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}

			assert(optarg != NULL);
			hostnqn = strcasestr(optarg, "hostnqn:");
			if (hostnqn) {
				size_t len;

				hostnqn += strlen("hostnqn:");

				len = strcspn(hostnqn, " \t\n");
				if (len > (sizeof(g_hostnqn) - 1)) {
					fprintf(stderr, "Host NQN is too long\n");
					return 1;
				}

				memcpy(g_hostnqn, hostnqn, len);
				g_hostnqn[len] = '\0';
			}
			break;
		case 'x':
			g_hex_dump = true;
			break;
		case 'z':
			if (optarg == NULL && argv[optind] != NULL && argv[optind][0] != '-') {
				g_zone_report_limit = spdk_strtol(argv[optind], 10);
				++optind;
			} else if (optarg) {
				g_zone_report_limit = spdk_strtol(optarg, 10);
			} else {
				g_zone_report_limit = 0;
			}
			if (g_zone_report_limit < 0) {
				fprintf(stderr, "Invalid Zone Report limit\n");
				return g_zone_report_limit;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'H':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'V':
			g_vmd = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	memcpy(opts->hostnqn, g_hostnqn, sizeof(opts->hostnqn));
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	g_controllers_found++;
	print_controller(ctrlr, trid, opts);
	spdk_nvme_detach_async(ctrlr, &g_detach_ctx);
}

int
main(int argc, char **argv)
{
	int				rc;
	struct spdk_env_opts		opts;
	struct spdk_nvme_ctrlr		*ctrlr;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "identify";
	opts.shm_id = g_shm_id;
	opts.mem_size = g_dpdk_mem;
	opts.mem_channel = 1;
	opts.main_core = g_main_core;
	opts.core_mask = g_core_mask;
	opts.hugepage_single_segments = g_dpdk_mem_single_seg;
	if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		opts.no_pci = true;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	/* A specific trid is required. */
	if (strlen(g_trid.traddr) != 0) {
		struct spdk_nvme_ctrlr_opts opts;

		spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
		memcpy(opts.hostnqn, g_hostnqn, sizeof(opts.hostnqn));
		ctrlr = spdk_nvme_connect(&g_trid, &opts, sizeof(opts));
		if (!ctrlr) {
			fprintf(stderr, "spdk_nvme_connect() failed\n");
			rc = 1;
			goto exit;
		}

		g_controllers_found++;
		print_controller(ctrlr, &g_trid, spdk_nvme_ctrlr_get_opts(ctrlr));
		spdk_nvme_detach_async(ctrlr, &g_detach_ctx);
	} else if (spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}

	if (g_detach_ctx) {
		spdk_nvme_detach_poll(g_detach_ctx);
	}

	if (g_controllers_found == 0) {
		fprintf(stderr, "No NVMe controllers found.\n");
	}

exit:
	if (g_vmd) {
		spdk_vmd_fini();
	}

	spdk_env_fini();

	return rc;
}
