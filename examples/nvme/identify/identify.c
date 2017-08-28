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

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/util.h"

static int outstanding_commands;

struct feature {
	uint32_t result;
	bool valid;
};

static struct feature features[256];

static struct spdk_nvme_error_information_entry error_page[256];

static struct spdk_nvme_health_information_page health_page;

static struct spdk_nvme_intel_smart_information_page intel_smart_page;

static struct spdk_nvme_intel_temperature_page intel_temperature_page;

static struct spdk_nvme_intel_marketing_description_page intel_md_page;

static bool g_hex_dump = false;

static int g_shm_id = -1;

static int g_dpdk_mem = 64;

static int g_master_core = 0;

static char g_core_mask[16] = "0x1";

static struct spdk_nvme_transport_id g_trid;

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

static int
get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t fid)
{
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = fid;

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, get_feature_completion, &features[fid]);
}

static void
get_features(struct spdk_nvme_ctrlr *ctrlr)
{
	size_t i;

	uint8_t features_to_get[] = {
		SPDK_NVME_FEAT_ARBITRATION,
		SPDK_NVME_FEAT_POWER_MANAGEMENT,
		SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD,
		SPDK_NVME_FEAT_ERROR_RECOVERY,
	};

	/* Submit several GET FEATURES commands and wait for them to complete */
	outstanding_commands = 0;
	for (i = 0; i < SPDK_COUNTOF(features_to_get); i++) {
		if (get_feature(ctrlr, features_to_get[i]) == 0) {
			outstanding_commands++;
		} else {
			printf("get_feature(0x%02X) failed to submit command\n", features_to_get[i]);
		}
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
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
get_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	outstanding_commands = 0;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

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
	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
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
	printf("%lu", result);
}

static void
print_namespace(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data		*nsdata;
	uint32_t				i;
	uint32_t				flags;

	nsdata = spdk_nvme_ns_get_data(ns);
	flags  = spdk_nvme_ns_get_flags(ns);

	printf("Namespace ID:%d\n", spdk_nvme_ns_get_id(ns));

	if (g_hex_dump) {
		hex_dump(nsdata, sizeof(*nsdata));
		printf("\n");
	}

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Inactive namespace ID\n\n");
		return;
	}

	printf("Deallocate:                  %s\n",
	       (flags & SPDK_NVME_NS_DEALLOCATE_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Deallocated/Unwritten Error: %s\n",
	       nsdata->nsfeat.dealloc_or_unwritten_error ? "Supported" : "Not Supported");
	printf("Deallocated Read Value:      %s\n",
	       nsdata->dlfeat.bits.read_value == SPDK_NVME_DEALLOC_READ_00 ? "All 0x00" :
	       nsdata->dlfeat.bits.read_value == SPDK_NVME_DEALLOC_READ_FF ? "All 0xFF" :
	       "Unknown");
	printf("Deallocate in Write Zeroes:  %s\n",
	       nsdata->dlfeat.bits.write_zero_deallocate ? "Supported" : "Not Supported");
	printf("Deallocated Guard Field:     %s\n",
	       nsdata->dlfeat.bits.guard_value ? "CRC for Read Value" : "0xFFFF");
	printf("Flush:                       %s\n",
	       (flags & SPDK_NVME_NS_FLUSH_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Reservation:                 %s\n",
	       (flags & SPDK_NVME_NS_RESERVATION_SUPPORTED) ? "Supported" : "Not Supported");
	if (flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		printf("End-to-End Data Protection:  Supported\n");
		printf("Protection Type:             Type%d\n", nsdata->dps.pit);
		printf("Metadata Transfered as:      %s\n",
		       nsdata->flbas.extended ? "Extended Data LBA" : "Separate Metadata Buffer");
		printf("Metadata Location:           %s\n",
		       nsdata->dps.md_start ? "First 8 Bytes" : "Last 8 Bytes");
	}
	printf("Size (in LBAs):              %lld (%lldM)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze / 1024 / 1024);
	printf("Capacity (in LBAs):          %lld (%lldM)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap / 1024 / 1024);
	printf("Utilization (in LBAs):       %lld (%lldM)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse / 1024 / 1024);
	if (nsdata->noiob) {
		printf("Optimal I/O Boundary:        %u blocks\n", nsdata->noiob);
	}
	printf("Thin Provisioning:           %s\n",
	       nsdata->nsfeat.thin_prov ? "Supported" : "Not Supported");
	printf("Per-NS Atomic Units:         %s\n",
	       nsdata->nsfeat.ns_atomic_write_unit ? "Yes" : "No");
	if (nsdata->nawun) {
		printf("Atomic Write Unit (Normal):  %d\n", nsdata->nawun + 1);
	}
	if (nsdata->nawupf) {
		printf("Atomic Write Unit (PFail):   %d\n", nsdata->nawupf + 1);
	}

	printf("NGUID/EUI64 Never Reused:    %s\n",
	       nsdata->nsfeat.guid_never_reused ? "Yes" : "No");
	printf("Number of LBA Formats:       %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:          LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("\n");
}

static void
print_controller(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	union spdk_nvme_cap_register		cap;
	union spdk_nvme_vs_register		vs;
	uint8_t					str[512];
	uint32_t				i;
	struct spdk_nvme_error_information_entry *error_entry;
	struct spdk_pci_addr 			pci_addr;
	struct spdk_pci_device			*pci_dev;
	struct spdk_pci_id			pci_id;

	cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(ctrlr);

	get_features(ctrlr);
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

		pci_dev = spdk_pci_get_device(&pci_addr);
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
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial Number:                         %s\n", str);
	snprintf(str, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	printf("Model Number:                          %s\n", str);
	snprintf(str, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	printf("Firmware Version:                      %s\n", str);
	printf("Recommended Arb Burst:                 %d\n", cdata->rab);
	printf("IEEE OUI Identifier:                   %02x %02x %02x\n",
	       cdata->ieee[0], cdata->ieee[1], cdata->ieee[2]);
	printf("Multi-path I/O\n");
	printf("  May have multiple subsystem ports:   %s\n", cdata->cmic.multi_port ? "Yes" : "No");
	printf("  May be connected to multiple hosts:  %s\n", cdata->cmic.multi_host ? "Yes" : "No");
	printf("  Associated with SR-IOV VF:           %s\n", cdata->cmic.sr_iov ? "Yes" : "No");
	printf("Max Data Transfer Size:                ");
	if (cdata->mdts == 0)
		printf("Unlimited\n");
	else
		printf("%" PRIu64 "\n", (uint64_t)1 << (12 + cap.bits.mpsmin + cdata->mdts));
	if (features[SPDK_NVME_FEAT_ERROR_RECOVERY].valid) {
		unsigned tler = features[SPDK_NVME_FEAT_ERROR_RECOVERY].result & 0xFFFF;
		printf("Error Recovery Timeout:                ");
		if (tler == 0) {
			printf("Unlimited\n");
		} else {
			printf("%u milliseconds\n", tler * 100);
		}
	}
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
	       cap.bits.css_nvm ? "Supported" : "Not Supported");
	printf("Memory Page Size Minimum:              %" PRIu64 " bytes\n",
	       (uint64_t)1 << (12 + cap.bits.mpsmin));
	printf("Memory Page Size Maximum:              %" PRIu64 " bytes\n",
	       (uint64_t)1 << (12 + cap.bits.mpsmax));
	printf("Optional Asynchronous Events Supported\n");
	printf("  Namespace Attribute Notices:         %s\n",
	       cdata->oaes.ns_attribute_notices ? "Supported" : "Not Supported");
	printf("  Firmware Activation Notices:         %s\n",
	       cdata->oaes.fw_activation_notices ? "Supported" : "Not Supported");

	printf("128-bit Host Identifier:               %s\n",
	       cdata->ctratt.host_id_exhid_supported ? "Supported" : "Not Supported");
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
	printf("Abort Command Limit:                   %d\n", cdata->acl + 1);
	printf("Async Event Request Limit:             %d\n", cdata->aerl + 1);
	printf("Number of Firmware Slots:              ");
	if (cdata->oacs.firmware != 0)
		printf("%d\n", cdata->frmw.num_slots);
	else
		printf("N/A\n");
	printf("Firmware Slot 1 Read-Only:             ");
	if (cdata->oacs.firmware != 0)
		printf("%s\n", cdata->frmw.slot1_ro ? "Yes" : "No");
	else
		printf("N/A\n");
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
	printf("Command Effects Log Page:              %s\n",
	       cdata->lpa.celp ? "Supported" : "Not Supported");
	printf("Get Log Page Extended Data:            %s\n",
	       cdata->lpa.edlp ? "Supported" : "Not Supported");
	printf("Telemetry Log Pages:                   %s\n",
	       cdata->lpa.telemetry ? "Supported" : "Not Supported");
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
	printf("Volatile Write Cache:        %s\n",
	       cdata->vwc.present ? "Present" : "Not Present");
	printf("Atomic Write Unit (Normal):  %d\n", cdata->awun + 1);
	printf("Atomic Write Unit (PFail):   %d\n", cdata->awupf + 1);
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
	printf("\n");

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
		printf("Low Priority Weight:         %u\n", lpw);
		printf("Medium Priority Weight:      %u\n", mpw);
		printf("High Priority Weight:        %u\n", hpw);
		printf("\n");
	}

	if (features[SPDK_NVME_FEAT_POWER_MANAGEMENT].valid) {
		unsigned ps = features[SPDK_NVME_FEAT_POWER_MANAGEMENT].result & 0x1F;
		printf("Power Management\n");
		printf("================\n");
		printf("Number of Power States:      %u\n", cdata->npss + 1);
		printf("Current Power State:         Power State #%u\n", ps);
		for (i = 0; i <= cdata->npss; i++) {
			const struct spdk_nvme_power_state *psd = &cdata->psd[i];
			printf("Power State #%u:  ", i);
			if (psd->mps) {
				/* MP scale is 0.0001 W */
				printf("Max Power: %u.%04u W\n",
				       psd->mp / 10000,
				       psd->mp % 10000);
			} else {
				/* MP scale is 0.01 W */
				printf("Max Power: %3u.%02u W\n",
				       psd->mp / 100,
				       psd->mp % 100);
			}
			/* TODO: print other power state descriptor fields */
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
		printf("Current Temperature:         %u Kelvin (%u Celsius)\n",
		       health_page.temperature,
		       health_page.temperature - 273);
		printf("Temperature Threshold:       %u Kelvin (%u Celsius)\n",
		       features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result,
		       features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result - 273);
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
				printf("Temperature Sensor %d:        %u Kelvin (%u Celsius)\n",
				       i + 1, health_page.temp_sensor[i],
				       health_page.temp_sensor[i] - 273);
			}
		}
		printf("\n");
	}

	if (cdata->hctma.bits.supported) {
		printf("Host Controlled Thermal Management\n");
		printf("==================================\n");
		printf("Minimum Thermal Management Temperature:  ");
		if (cdata->mntmt) {
			printf("%u Kelvin (%u Celsius)\n", cdata->mntmt, cdata->mntmt - 273);
		} else {
			printf("Not Reported\n");
		}
		printf("Maximum Thermal Managment Temperature:   ");
		if (cdata->mxtmt) {
			printf("%u Kelvin (%u Celsius)\n", cdata->mxtmt, cdata->mxtmt - 273);
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
				printf("  Current Raw Value: \n");
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
				printf("  Current Raw Value: \n");
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
		printf("Current Temperature: %lu\n", intel_temperature_page.current_temperature);
		printf("Overtemp shutdown Flag for last critical component temperature: %lu\n",
		       intel_temperature_page.shutdown_flag_last);
		printf("Overtemp shutdown Flag for life critical component temperature: %lu\n",
		       intel_temperature_page.shutdown_flag_life);
		printf("Highest temperature: %lu\n", intel_temperature_page.highest_temperature);
		printf("Lowest temperature: %lu\n", intel_temperature_page.lowest_temperature);
		printf("Specified Maximum Operating Temperature: %lu\n",
		       intel_temperature_page.specified_max_op_temperature);
		printf("Specified Minimum Operating Temperature: %lu\n",
		       intel_temperature_page.specified_min_op_temperature);
		printf("Estimated offset: %ld\n", intel_temperature_page.estimated_offset);
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

	for (i = 1; i <= spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		print_namespace(spdk_nvme_ctrlr_get_ns(ctrlr, i));
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
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");

	spdk_tracelog_usage(stdout, "-t");

	printf(" -i         shared memory group ID\n");
	printf(" -p         core number in decimal to run this application which started from 0\n");
	printf(" -d         DPDK huge memory size in MB\n");
	printf(" -x         print hex dump of raw data\n");
	printf(" -v         verbose (enable warnings)\n");
	printf(" -H         show this usage\n");
}

static int
parse_args(int argc, char **argv)
{
	int op, rc;

	g_trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:i:p:r:t:xH")) != -1) {
		switch (op) {
		case 'd':
			g_dpdk_mem = atoi(optarg);
			break;
		case 'i':
			g_shm_id = atoi(optarg);
			break;
		case 'p':
			g_master_core = atoi(optarg);
			if (g_master_core < 0) {
				fprintf(stderr, "Invalid core number\n");
				return 1;
			}
			snprintf(g_core_mask, sizeof(g_core_mask), "0x%llx", 1ULL << g_master_core);
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage(argv[0]);
			return 0;
#endif
			break;
		case 'x':
			g_hex_dump = true;
			break;
		case 'H':
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
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	print_controller(ctrlr, trid);
	spdk_nvme_detach(ctrlr);
}

int main(int argc, char **argv)
{
	int				rc;
	struct spdk_env_opts		opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "identify";
	opts.shm_id = g_shm_id;
	opts.mem_size = g_dpdk_mem;
	opts.mem_channel = 1;
	opts.master_core = g_master_core;
	opts.core_mask = g_core_mask;
	if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		opts.no_pci = true;
	}
	spdk_env_init(&opts);

	rc = 0;
	if (spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
	}

	return rc;
}
