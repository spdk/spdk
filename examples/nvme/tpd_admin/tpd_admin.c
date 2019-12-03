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

#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

static struct spdk_nvme_transport_id g_trid;
static uint32_t g_nsid = SPDK_NVME_GLOBAL_NS_TAG;
static bool g_do_identify = false;
static bool g_do_vendor = false; 
static char *g_do_firmware = NULL;
static int32_t g_do_format = -1;

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
print_hex_be(const void *v, size_t size)
{
	const uint8_t *buf = v;

	while (size--) {
		printf("%02X", *buf++);
	}
}

static void
completion_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	printf("echo_cb: sct=%02x, sc=%02x, cdw0=%04x\n", cpl->status.sct, cpl->status.sc, cpl->cdw0);
}

static void
vendor_cmd_no_buffer(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = 0xc0;
	cmd.cdw10 = 0xbeef;
	cmd.nsid = nsid;

	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, completion_cb, NULL) != 0) {
		printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
		return;
	}

	while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);
}

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

static void
vendor_cmd_host2controller(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = 0xc1;
	cmd.cdw10 = 0xdead;
	cmd.nsid = nsid;

	ssize_t buf_size = 512;
	uint8_t *buf = (uint8_t *)spdk_zmalloc(buf_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("*** Unable to allocate send buffer\n");
		return;
	}
	memset(buf, 0xaa, buf_size);

	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, buf_size, completion_cb, NULL) != 0) {
		printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
		return;
	}

	while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);
}

static void
vendor_cmd_controller2host(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = 0xc2;
	cmd.cdw10 = 0xccccc;
	cmd.nsid = nsid;

	size_t buf_size = 512;
	uint8_t *buf = (uint8_t *)spdk_zmalloc(buf_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("*** Unable to allocate send buffer\n");
		return;
	}
	memset(buf, 0xaa, buf_size);

	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, buf_size, completion_cb, NULL) != 0) {
		printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
		return;
	}

	while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);

	for (size_t i=0; i<buf_size; i++) printf("%02x ", buf[i]);
	printf("\n");
}

static void
print_namespace(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data		*nsdata;
	const struct spdk_uuid			*uuid;
	uint32_t				i;
	uint32_t				flags;
	char					uuid_str[SPDK_UUID_STRING_LEN];

	nsdata = spdk_nvme_ns_get_data(ns);
	flags  = spdk_nvme_ns_get_flags(ns);

	printf("Namespace ID:%d\n", spdk_nvme_ns_get_id(ns));

	/* This function is only called for active namespaces. */
	assert(spdk_nvme_ns_is_active(ns));

	printf("Deallocate:                            %s\n",
	       (flags & SPDK_NVME_NS_DEALLOCATE_SUPPORTED) ? "Supported" : "Not Supported");
	printf("Deallocated/Unwritten Error:           %s\n",
	       nsdata->nsfeat.dealloc_or_unwritten_error ? "Supported" : "Not Supported");
	printf("Deallocated Read Value:                %s\n",
	       nsdata->dlfeat.bits.read_value == SPDK_NVME_DEALLOC_READ_00 ? "All 0x00" :
	       nsdata->dlfeat.bits.read_value == SPDK_NVME_DEALLOC_READ_FF ? "All 0xFF" :
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
	if (nsdata->lbaf[nsdata->flbas.format].ms > 0) {
		printf("Metadata Transferred as:               %s\n",
		       nsdata->flbas.extended ? "Extended Data LBA" : "Separate Metadata Buffer");
	}
	printf("Namespace Sharing Capabilities:        %s\n",
	       nsdata->nmic.can_share ? "Multiple Controllers" : "Private");
	printf("Size (in LBAs):                        %lld (%lldM)\n",
	       (long long)nsdata->nsze,
	       (long long)nsdata->nsze / 1024 / 1024);
	printf("Capacity (in LBAs):                    %lld (%lldM)\n",
	       (long long)nsdata->ncap,
	       (long long)nsdata->ncap / 1024 / 1024);
	printf("Utilization (in LBAs):                 %lld (%lldM)\n",
	       (long long)nsdata->nuse,
	       (long long)nsdata->nuse / 1024 / 1024);
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

		if (nsdata->nacwu) {
			printf("  Atomic Compare & Write Unit:         %d\n", nsdata->nacwu + 1);
		}

		printf("  Atomic Boundary Size (Normal):       %d\n", nsdata->nabsn);
		printf("  Atomic Boundary Size (PFail):        %d\n", nsdata->nabspf);
		printf("  Atomic Boundary Offset:              %d\n", nsdata->nabo);
	}

	printf("NGUID/EUI64 Never Reused:              %s\n",
	       nsdata->nsfeat.guid_never_reused ? "Yes" : "No");
	printf("Number of LBA Formats:                 %d\n", nsdata->nlbaf + 1);
	printf("Current LBA Format:                    LBA Format #%02d\n",
	       nsdata->flbas.format);
	for (i = 0; i <= nsdata->nlbaf; i++)
		printf("LBA Format #%02d: Data Size: %5d  Metadata Size: %5d\n",
		       i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	printf("\n");
}

static void 
print_identify(struct spdk_nvme_ctrlr *ctrlr) 
{
	const struct spdk_nvme_ctrlr_data	*cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	hex_dump(cdata, sizeof(*cdata));
	printf("\n");

	printf("Vendor ID [VID]:                       %04x\n", cdata->vid);
	printf("Subsystem Vendor ID [SSVID]:           %04x\n", cdata->ssvid);
	printf("Serial Number [SN]:                    ");
	print_ascii_string(cdata->sn, sizeof(cdata->sn));
	printf("\n");
	printf("Model Number [MN]:                     ");
	print_ascii_string(cdata->mn, sizeof(cdata->mn));
	printf("\n");
	printf("Firmware Version [FR]:                 ");
	print_ascii_string(cdata->fr, sizeof(cdata->fr));
	printf("\n");
	printf("Recommended Arb Burst:                 %d\n", cdata->rab);
	printf("IEEE OUI Identifier:                   %02x %02x %02x\n",
	       cdata->ieee[0], cdata->ieee[1], cdata->ieee[2]);
	printf("Multi-path I/O\n");
	printf("  May have multiple subsystem ports:   %s\n", cdata->cmic.multi_port ? "Yes" : "No");
	printf("  May be connected to multiple hosts:  %s\n", cdata->cmic.multi_host ? "Yes" : "No");
	printf("  Associated with SR-IOV VF:           %s\n", cdata->cmic.sr_iov ? "Yes" : "No");
	printf("Max Data Transfer Size [MTDTS]:        ");
	if (cdata->mdts == 0) {
		printf("Unlimited\n");
	} else {
		printf("%" PRIu8 "* CAPS.MPSMIN\n", cdata->mdts);
	}
	printf("Controller ID [CNTLID]:                %u\n", cdata->cntlid);
	
	if (cdata->ver.raw != 0) {
		printf("NVMe Specification Version (Identify): %u.%u", cdata->ver.bits.mjr, cdata->ver.bits.mnr);
		if (cdata->ver.bits.ter) {
			printf(".%u", cdata->ver.bits.ter);
		}
		printf("\n");
	} else {
		printf("NVMe Specification Version            : is 0\n");
	}

	printf("RTD3 Resume Latency (RTD3R):           %u\n", cdata->rtd3r);
	printf("RTD3 Entry Latency (RTD3E):            %u\n", cdata->rtd3e);
	printf("Optional Asynchronous Events Supported (OAES): fw=%s, ns=%s\n", 
		cdata->oaes.fw_activation_notices ? "Yes" : "No",
		cdata->oaes.ns_attribute_notices ? "Yes" : "No");

	// printf("Controller Attributes (CTRATT):        %u\n", cdata->ctratt);
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

	// Number of Power States Support (NPSS)
	// Admin Vendor Specific Command Configuration (AVSCC):
	// Autonomous Power State Transition Attributes (APSTA):
	// Warning Composite Temperature Threshold (WCTEMP):
	// Critical Composite Temperature Threshold (CCTEMP):
	// Maximum Time for Firmware Activation (MTFA):
	// Host Memory Buffer Preferred Size (HMPRE):
	// Host Memory Buffer Minimum Size (HMMIN):

	printf("Total NVM Capacity (TNVMCAP):            %" PRIu64 ", %" PRIu64 "\n", cdata->tnvmcap[0], cdata->tnvmcap[1]);
	printf("Unallocated NVM Capacity (UNVMCAP):      %" PRIu64 ", %" PRIu64 "\n", cdata->unvmcap[0], cdata->unvmcap[1]);

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
	printf("\n");

	// Extended Device Self-test Time (EDSTT):
	// Device Self-test Options (DSTO):

	if (cdata->kas == 0) {
		printf("Keep Alive:                            Not Supported\n");
	} else {
		printf("Keep Alive:                            Supported\n");
		printf("Keep Alive Granularity:                %u ms\n",
		       cdata->kas * 100);
	}
	printf("\n");

	if (cdata->hctma.bits.supported) {
		printf("Host Controlled Thermal Management\n");
		printf("==================================\n");
		printf("Minimum Thermal Management Temperature:  ");
		if (cdata->mntmt) {
			printf("%u Kelvin (%d Celsius)\n", cdata->mntmt, (int)cdata->mntmt - 273);
		} else {
			printf("Not Reported\n");
		}
		printf("Maximum Thermal Managment Temperature:   ");
		if (cdata->mxtmt) {
			printf("%u Kelvin (%d Celsius)\n", cdata->mxtmt, (int)cdata->mxtmt - 273);
		} else {
			printf("Not Reported\n");
		}
		printf("\n");
	}

	printf("Sanitize Capabilities (SANICAP):           0x%0x4\n", cdata->sanicap.raw);
	printf("\n");

	printf("NVM Command Set Attributes\n");
	printf("==========================\n");
	printf("Submission Queue Entry Size\n");
	printf("  Max:                       %d\n", 1 << cdata->sqes.max);
	printf("  Min:                       %d\n", 1 << cdata->sqes.min);
	printf("Completion Queue Entry Size\n");
	printf("  Max:                       %d\n", 1 << cdata->cqes.max);
	printf("  Min:                       %d\n", 1 << cdata->cqes.min);
	printf("Maximum Outstanding Commands (MAXCMD): %u\n", cdata->maxcmd);
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

	// Fused Operation Support (FUSES):
	// Format NVM Attributes (FNA):
	
	printf("Volatile Write Cache:        %s\n",
	       cdata->vwc.present ? "Present" : "Not Present");
	printf("Atomic Write Unit (Normal):  %d\n", cdata->awun + 1);
	printf("Atomic Write Unit (PFail):   %d\n", cdata->awupf + 1);
	printf("Atomic Compare & Write Unit: %d\n", cdata->acwu + 1);
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
	
	printf("\n");
	
	printf("NVM Subsystem NVMe Qualified Name (SUBNQN):");
	print_ascii_string(cdata->subnqn, sizeof(cdata->subnqn));
	printf("\n");

	// Power State 0 Descriptor (PSD0):
	// ....
	// Power State 0 Descriptor (PSD31):

	if (cdata->lpa.celp) {
		printf("Commands Supported and Effects\n");
		printf("==============================\n");
		printf("?????\n");
	}

	printf("Error Log Page Entries (ELPE): %u\n", cdata->elpe);

	printf("Active Namespaces\n");
	printf("=================\n");
	for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		print_namespace(spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}
}

static void 
upload_firmware(struct spdk_nvme_ctrlr *ctrlr, const char *filename)
{
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf("Unable to open %s (errno=%d)\n", filename, errno);
		return;
	}

	struct stat fw_stat;
	if (fstat(fd, &fw_stat) < 0) {
		printf("Unable to get size for %s\n", filename);
		close(fd);
		return;
	}

	if (fw_stat.st_size % 4) {
		close(fd);
		printf("Firmware size must be a multiple of 4 bytes\n");
		close(fd);
		return;
	}

	struct spdk_nvme_cmd cmd;

	ssize_t buf_size = 512;
	uint8_t *buf = (uint8_t *)spdk_zmalloc(buf_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("*** Unable to allocate send buffer\n");
		return;
	}

	// download
	ssize_t br;
	uint32_t offset = 0;
	while ((br = read(fd, buf, buf_size)) > 0) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;
		cmd.cdw10 = (br >> 2) - 1; // 0 based value
		cmd.cdw11 = offset >> 2;
		printf("sending: %3ld bytes, cdw10=%u, cdw11=%u\n", br, cmd.cdw10, cmd.cdw11);

		if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, buf_size, completion_cb, NULL) != 0) {
			printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
			close(fd);
			return;
		}

		while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);
		offset += br;
	}
	close(fd);

	// commit
	struct spdk_nvme_fw_commit fw_commit;
	memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
	fw_commit.fs = 0;
	fw_commit.ca = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_FIRMWARE_COMMIT;
	memcpy(&cmd.cdw10, &fw_commit, sizeof(uint32_t));
	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, completion_cb, NULL) != 0) {
		printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
		return;
	}

	while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);
}

static void 
format(struct spdk_nvme_ctrlr *ctrlr, uint32_t lbaf)
{
	printf("Formatting ...\n");

	struct spdk_nvme_format fmt;
	fmt.lbaf = lbaf;

	struct spdk_nvme_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_FORMAT_NVM;
	cmd.nsid = 0xFFFFFFFF; // all namespaces
	memcpy(&cmd.cdw10, &fmt, sizeof(uint32_t));

	if (spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, completion_cb, NULL) != 0) {
		printf("*** spdk_nvme_ctrlr_cmd_admin_raw failed\n");
		return;
	}

	while (spdk_nvme_ctrlr_process_admin_completions(ctrlr) == 0);
	
}


static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -i\t\tSend identify");
	printf(" -V\t\tSend vendor commands 0xC1 and 0xC2\n");
	printf(" -d filename\t\tFirmware download and commit\n");
	printf(" -f LBAF\t\tFormat with specified LBAF\n");
	printf(" -n nsid\t\tSet namespace, default=%08x\n", g_nsid);
	printf(" -r trid\t\t\remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	while ((op = getopt(argc, argv, "r:n:iVd:f:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'i':
			g_do_identify = true;
			break;
		case 'V':
			g_do_vendor = true;
			break;
		case 'd':
			g_do_firmware = strdup(optarg);
			break;
		case 'f':
			g_do_format = atoi(optarg);
			break;
		case 'n':
			g_nsid = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (strlen(g_trid.traddr) == 0) {
		usage(argv[0]);
	}

	return 0;
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct spdk_nvme_ctrlr		*ctrlr;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "tpd_admin";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	if (!ctrlr) {
		fprintf(stderr, "spdk_nvme_connect() failed\n");
		return 1;
	}

	if (g_do_identify) {
		print_identify(ctrlr);
	}

	if (g_do_vendor) {
		// vendor_cmd_no_buffer(ctrlr, g_nsid);
		vendor_cmd_host2controller(ctrlr, g_nsid);
		vendor_cmd_controller2host(ctrlr, g_nsid);
	}

	if (g_do_firmware) {
		upload_firmware(ctrlr, g_do_firmware);
	}

	if (g_do_format >= 0) {
		format(ctrlr, g_do_format);
	}

	// Wait a little bit to make sure we get all completions
	for (int i=0; i<100; i++) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		usleep(1000);
	}

	spdk_nvme_detach(ctrlr);
	
	return 0;
}
