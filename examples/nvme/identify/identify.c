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

#include <stdbool.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"
#include "spdk/nvme_intel.h"
#include "spdk/pci_ids.h"

struct rte_mempool *request_mempool;

static int outstanding_commands;

struct feature {
	uint32_t result;
	bool valid;
};

static struct feature features[256];

static struct spdk_nvme_health_information_page *health_page;

static struct spdk_nvme_intel_smart_information_page *intel_smart_page;

static struct spdk_nvme_intel_temperature_page *intel_temperature_page;

static bool g_hex_dump = false;

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
	for (i = 0; i < sizeof(features_to_get) / sizeof(*features_to_get); i++) {
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
get_health_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (health_page == NULL) {
		health_page = rte_zmalloc("nvme health", sizeof(*health_page), 4096);
	}
	if (health_page == NULL) {
		printf("Allocation error (health page)\n");
		exit(1);
	}

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					     SPDK_NVME_GLOBAL_NS_TAG, health_page, sizeof(*health_page), get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_intel_smart_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (intel_smart_page == NULL) {
		intel_smart_page = rte_zmalloc("nvme intel smart", sizeof(*intel_smart_page), 4096);
	}
	if (intel_smart_page == NULL) {
		printf("Allocation error (intel smart page)\n");
		exit(1);
	}

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_SMART, SPDK_NVME_GLOBAL_NS_TAG,
					     intel_smart_page, sizeof(*intel_smart_page), get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}

	return 0;
}

static int
get_intel_temperature_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (intel_temperature_page == NULL) {
		intel_temperature_page = rte_zmalloc("nvme intel temperature", sizeof(*intel_temperature_page),
						     4096);
	}
	if (intel_temperature_page == NULL) {
		printf("Allocation error (nvme intel temperature page)\n");
		exit(1);
	}

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE,
					     SPDK_NVME_GLOBAL_NS_TAG, intel_temperature_page, sizeof(*intel_temperature_page),
					     get_log_page_completion, NULL)) {
		printf("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}
	return 0;
}

static void
get_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *ctrlr_data;
	outstanding_commands = 0;

	if (get_health_log_page(ctrlr) == 0) {
		outstanding_commands++;
	} else {
		printf("Get Log Page (SMART/health) failed\n");
	}

	ctrlr_data = spdk_nvme_ctrlr_get_data(ctrlr);
	if (ctrlr_data->vid == SPDK_PCI_VID_INTEL) {
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
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
cleanup(void)
{
	if (health_page) {
		rte_free(health_page);
		health_page = NULL;
	}
	if (intel_smart_page) {
		rte_free(intel_smart_page);
		intel_smart_page = NULL;
	}
	if (intel_temperature_page) {
		rte_free(intel_temperature_page);
		intel_temperature_page = NULL;
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

/* The len should be <= 8.*/
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

	printf("Deallocate:                  %s\n",
	       (flags & SPDK_NVME_NS_DEALLOCATE_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Flush:                       %s\n",
	       (flags & SPDK_NVME_NS_FLUSH_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Reservation:                 %s\n",
	       (flags & SPDK_NVME_NS_RESERVATION_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Size (in LBAs):              %lld (%lldM)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze / 1024 / 1024);
	printf("Capacity (in LBAs):          %lld (%lldM)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap / 1024 / 1024);
	printf("Utilization (in LBAs):       %lld (%lldM)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse / 1024 / 1024);
	printf("Thin Provisioning:           %s\n",
	       nsdata->nsfeat.thin_prov ? "Supported" : "Not Supported");
	printf("Number of LBA Formats:       %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:          LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("\n");
}

static void
print_controller(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_device *pci_dev)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	uint8_t					str[128];
	uint32_t				i;

	get_features(ctrlr);
	get_log_pages(ctrlr);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller at PCI bus %d, device %d, function %d\n",
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev));
	printf("=====================================================\n");

	if (g_hex_dump) {
		hex_dump(cdata, sizeof(*cdata));
		printf("\n");
	}

	printf("Controller Capabilities/Features\n");
	printf("================================\n");
	printf("Vendor ID:                  %04x\n", cdata->vid);
	printf("Subsystem Vendor ID:        %04x\n", cdata->ssvid);
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial Number:              %s\n", str);
	snprintf(str, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	printf("Model Number:               %s\n", str);
	snprintf(str, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	printf("Firmware Version:           %s\n", str);
	printf("Recommended Arb Burst:      %d\n", cdata->rab);
	printf("IEEE OUI Identifier:        %02x %02x %02x\n",
	       cdata->ieee[0], cdata->ieee[1], cdata->ieee[2]);
	printf("Multi-Interface Cap:        %02x\n", cdata->mic);
	/* TODO: Use CAP.MPSMIN to determine true memory page size. */
	printf("Max Data Transfer Size:     ");
	if (cdata->mdts == 0)
		printf("Unlimited\n");
	else
		printf("%d\n", 4096 * (1 << cdata->mdts));
	if (features[SPDK_NVME_FEAT_ERROR_RECOVERY].valid) {
		unsigned tler = features[SPDK_NVME_FEAT_ERROR_RECOVERY].result & 0xFFFF;
		printf("Error Recovery Timeout:     ");
		if (tler == 0) {
			printf("Unlimited\n");
		} else {
			printf("%u milliseconds\n", tler * 100);
		}
	}
	printf("\n");

	printf("Admin Command Set Attributes\n");
	printf("============================\n");
	printf("Security Send/Receive:       %s\n",
	       cdata->oacs.security ? "Supported" : "Not Supported");
	printf("Format NVM:                  %s\n",
	       cdata->oacs.format ? "Supported" : "Not Supported");
	printf("Firmware Activate/Download:  %s\n",
	       cdata->oacs.firmware ? "Supported" : "Not Supported");
	printf("Abort Command Limit:         %d\n", cdata->acl + 1);
	printf("Async Event Request Limit:   %d\n", cdata->aerl + 1);
	printf("Number of Firmware Slots:    ");
	if (cdata->oacs.firmware != 0)
		printf("%d\n", cdata->frmw.num_slots);
	else
		printf("N/A\n");
	printf("Firmware Slot 1 Read-Only:   ");
	if (cdata->oacs.firmware != 0)
		printf("%s\n", cdata->frmw.slot1_ro ? "Yes" : "No");
	else
		printf("N/A\n");
	printf("Per-Namespace SMART Log:     %s\n",
	       cdata->lpa.ns_smart ? "Yes" : "No");
	printf("Error Log Page Entries:      %d\n", cdata->elpe + 1);
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
	printf("Volatile Write Cache:        %s\n",
	       cdata->vwc.present ? "Present" : "Not Present");
	printf("Scatter-Gather List\n");
	printf("  SGL Command Set:           %s\n",
	       cdata->sgls.supported ? "Supported" : "Not Supported");
	printf("  SGL Bit Bucket Descriptor: %s\n",
	       cdata->sgls.bit_bucket_descriptor_supported ? "Supported" : "Not Supported");
	printf("  SGL Metadata Pointer:      %s\n",
	       cdata->sgls.metadata_pointer_supported ? "Supported" : "Not Supported");
	printf("  Oversized SGL:             %s\n",
	       cdata->sgls.oversized_sgl_supported ? "Supported" : "Not Supported");
	printf("\n");

	if (features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		uint32_t arb = features[SPDK_NVME_FEAT_ARBITRATION].result;
		unsigned ab, lpw, mpw, hpw;

		ab = arb & 0x3;
		lpw = ((arb >> 8) & 0xFF) + 1;
		mpw = ((arb >> 16) & 0xFF) + 1;
		hpw = ((arb >> 24) & 0xFF) + 1;

		printf("Arbitration\n");
		printf("===========\n");
		printf("Arbitration Burst:           ");
		if (ab == 7) {
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
		printf("\n");
	}

	if (features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].valid && health_page) {
		printf("Health Information\n");
		printf("==================\n");

		if (g_hex_dump) {
			hex_dump(health_page, sizeof(*health_page));
			printf("\n");
		}

		printf("Critical Warnings:\n");
		printf("  Available Spare Space:     %s\n",
		       health_page->critical_warning.bits.available_spare ? "WARNING" : "OK");
		printf("  Temperature:               %s\n",
		       health_page->critical_warning.bits.temperature ? "WARNING" : "OK");
		printf("  Device Reliability:        %s\n",
		       health_page->critical_warning.bits.device_reliability ? "WARNING" : "OK");
		printf("  Read Only:                 %s\n",
		       health_page->critical_warning.bits.read_only ? "Yes" : "No");
		printf("  Volatile Memory Backup:    %s\n",
		       health_page->critical_warning.bits.volatile_memory_backup ? "WARNING" : "OK");
		printf("Current Temperature:         %u Kelvin (%u Celsius)\n",
		       health_page->temperature,
		       health_page->temperature - 273);
		printf("Temperature Threshold:       %u Kelvin (%u Celsius)\n",
		       features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result,
		       features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].result - 273);
		printf("Available Spare:             %u%%\n", health_page->available_spare);
		printf("Life Percentage Used:        %u%%\n", health_page->percentage_used);
		printf("Data Units Read:             ");
		print_uint128_dec(health_page->data_units_read);
		printf("\n");
		printf("Data Units Written:          ");
		print_uint128_dec(health_page->data_units_written);
		printf("\n");
		printf("Host Read Commands:          ");
		print_uint128_dec(health_page->host_read_commands);
		printf("\n");
		printf("Host Write Commands:         ");
		print_uint128_dec(health_page->host_write_commands);
		printf("\n");
		printf("Controller Busy Time:        ");
		print_uint128_dec(health_page->controller_busy_time);
		printf(" minutes\n");
		printf("Power Cycles:                ");
		print_uint128_dec(health_page->power_cycles);
		printf("\n");
		printf("Power On Hours:              ");
		print_uint128_dec(health_page->power_on_hours);
		printf(" hours\n");
		printf("Unsafe Shutdowns:            ");
		print_uint128_dec(health_page->unsafe_shutdowns);
		printf("\n");
		printf("Unrecoverable Media Errors:  ");
		print_uint128_dec(health_page->media_errors);
		printf("\n");
		printf("Lifetime Error Log Entries:  ");
		print_uint128_dec(health_page->num_error_info_log_entries);
		printf("\n");
		printf("\n");
	}

	if (intel_smart_page) {
		size_t i = 0;

		printf("Intel Health Information\n");
		printf("==================\n");
		for (i = 0;
		     i < sizeof(intel_smart_page->attributes) / sizeof(intel_smart_page->attributes[0]); i++) {
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_PROGRAM_FAIL_COUNT) {
				printf("Program Fail Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_ERASE_FAIL_COUNT) {
				printf("Erase Fail Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_WEAR_LEVELING_COUNT) {
				printf("Wear Leveling Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: \n");
				printf("  Min: ");
				print_uint_var_dec(&intel_smart_page->attributes[i].raw_value[0], 2);
				printf("\n");
				printf("  Max: ");
				print_uint_var_dec(&intel_smart_page->attributes[i].raw_value[2], 2);
				printf("\n");
				printf("  Avg: ");
				print_uint_var_dec(&intel_smart_page->attributes[i].raw_value[4], 2);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_E2E_ERROR_COUNT) {
				printf("End to End Error Detection Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_CRC_ERROR_COUNT) {
				printf("CRC Error Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_MEDIA_WEAR) {
				printf("Timed Workload, Media Wear:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_HOST_READ_PERCENTAGE) {
				printf("Timed Workload, Host Read/Write Ratio:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("%%");
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_TIMER) {
				printf("Timed Workload, Timer:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_THERMAL_THROTTLE_STATUS) {
				printf("Thermal Throttle Status:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: \n");
				printf("  Percentage: %d%%\n", intel_smart_page->attributes[i].raw_value[0]);
				printf("  Throttling Event Count: ");
				print_uint_var_dec(&intel_smart_page->attributes[i].raw_value[1], 4);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_RETRY_BUFFER_OVERFLOW_COUNTER) {
				printf("Retry Buffer Overflow Counter:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_PLL_LOCK_LOSS_COUNT) {
				printf("PLL Lock Loss Count:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_NAND_BYTES_WRITTEN) {
				printf("NAND Bytes Written:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
			if (intel_smart_page->attributes[i].code == SPDK_NVME_INTEL_SMART_HOST_BYTES_WRITTEN) {
				printf("Host Bytes Written:\n");
				printf("  Normalized Value : %d\n",
				       intel_smart_page->attributes[i].normalized_value);
				printf("  Current Raw Value: ");
				print_uint_var_dec(intel_smart_page->attributes[i].raw_value, 6);
				printf("\n");
			}
		}
		printf("\n");
	}

	if (intel_temperature_page) {
		printf("Intel Temperature Information\n");
		printf("==================\n");
		printf("Current Temperature: %lu\n", intel_temperature_page->current_temperature);
		printf("Overtemp shutdown Flag for last critical component temperature: %lu\n",
		       intel_temperature_page->shutdown_flag_last);
		printf("Overtemp shutdown Flag for life critical component temperature: %lu\n",
		       intel_temperature_page->shutdown_flag_life);
		printf("Highest temperature: %lu\n", intel_temperature_page->highest_temperature);
		printf("Lowest temperature: %lu\n", intel_temperature_page->lowest_temperature);
		printf("Specified Maximum Operating Temperature: %lu\n",
		       intel_temperature_page->specified_max_op_temperature);
		printf("Specified Minimum Operating Temperature: %lu\n",
		       intel_temperature_page->specified_min_op_temperature);
		printf("Estimated offset: %ld\n", intel_temperature_page->estimated_offset);
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
	printf("  -x  print hex dump of raw data\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	while ((op = getopt(argc, argv, "x")) != -1) {
		switch (op) {
		case 'x':
			g_hex_dump = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	optind = 1;

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	print_controller(ctrlr, pci_dev);
	spdk_nvme_detach(ctrlr);
}

static const char *ealargs[] = {
	"identify",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	int				rc;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	rc = 0;
	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
	}

	cleanup();

	return rc;
}
