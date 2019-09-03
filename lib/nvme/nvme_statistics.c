/*-
 *	 BSD LICENSE
 *
 *	 Copyright (c) Intel Corporation.
 *	 All rights reserved.
 *
 *	 Redistribution and use in source and binary forms, with or without
 *	 modification, are permitted provided that the following conditions
 *	 are met:
 *
 *	   * Redistributions of source code must retain the above copyright
 *		 notice, this list of conditions and the following disclaimer.
 *	   * Redistributions in binary form must reproduce the above copyright
 *		 notice, this list of conditions and the following disclaimer in
 *		 the documentation and/or other materials provided with the
 *		 distribution.
 *	   * Neither the name of Intel Corporation nor the names of its
 *		 contributors may be used to endorse or promote products derived
 *		 from this software without specific prior written permission.
 *
 *	 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *	 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *	 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *	 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *	 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *	 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *	 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *	 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *	 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *	 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *	 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"
#include "spdk/base64.h"

#include "nvme_internal.h"


#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"

#include <endian.h>

#include <sys/ioctl.h>

#if defined(__linux__)
#include <linux/types.h>
#elif defined(__FreeBSD__)
#include <sys/compat/linuxkpi/common/include/linux/types.h>
#endif




#define nvme_admin_cmd nvme_passthru_cmd
#define NVME_IOCTL_ADMIN_CMD	_IOWR('N', 0x41, struct nvme_admin_cmd)
#define NVME_NSID_ALL		0xffffffff


#ifdef __CHECKER__
#define __force       __attribute__((force))
#else
#define __force
#endif

#define cpu_to_le16(x) \
	((__force __le16)htole16(x))
#define cpu_to_le32(x) \
	((__force __le32)htole32(x))
#define cpu_to_le64(x) \
	((__force __le64)htole64(x))

#define le16_to_cpu(x) \
	le16toh((__force __u16)(x))
#define le32_to_cpu(x) \
	le32toh((__force __u32)(x))
#define le64_to_cpu(x) \
	le64toh((__force __u64)(x))

static int outstanding_commands;

struct nvme_smart_log {
	__u8			critical_warning;
	__u8			temperature[2];
	__u8			avail_spare;
	__u8			spare_thresh;
	__u8			percent_used;
	__u8			rsvd6[26];
	__u8			data_units_read[16];
	__u8			data_units_written[16];
	__u8			host_reads[16];
	__u8			host_writes[16];
	__u8			ctrl_busy_time[16];
	__u8			power_cycles[16];
	__u8			power_on_hours[16];
	__u8			unsafe_shutdowns[16];
	__u8			media_errors[16];
	__u8			num_err_log_entries[16];
	__le32			warning_temp_time;
	__le32			critical_comp_time;
	__le16			temp_sensor[8];
	__le32			thm_temp1_trans_count;
	__le32			thm_temp2_trans_count;
	__le32			thm_temp1_total_time;
	__le32			thm_temp2_total_time;
	__u8			rsvd232[280];
};

enum {
	NVME_QUEUE_PHYS_CONTIG	= (1 << 0),
	NVME_CQ_IRQ_ENABLED	= (1 << 1),
	NVME_SQ_PRIO_URGENT	= (0 << 1),
	NVME_SQ_PRIO_HIGH	= (1 << 1),
	NVME_SQ_PRIO_MEDIUM	= (2 << 1),
	NVME_SQ_PRIO_LOW	= (3 << 1),
	NVME_FEAT_ARBITRATION	= 0x01,
	NVME_FEAT_POWER_MGMT	= 0x02,
	NVME_FEAT_LBA_RANGE	= 0x03,
	NVME_FEAT_TEMP_THRESH	= 0x04,
	NVME_FEAT_ERR_RECOVERY	= 0x05,
	NVME_FEAT_VOLATILE_WC	= 0x06,
	NVME_FEAT_NUM_QUEUES	= 0x07,
	NVME_FEAT_IRQ_COALESCE	= 0x08,
	NVME_FEAT_IRQ_CONFIG	= 0x09,
	NVME_FEAT_WRITE_ATOMIC	= 0x0a,
	NVME_FEAT_ASYNC_EVENT	= 0x0b,
	NVME_FEAT_AUTO_PST	= 0x0c,
	NVME_FEAT_HOST_MEM_BUF	= 0x0d,
	NVME_FEAT_TIMESTAMP	= 0x0e,
	NVME_FEAT_KATO		= 0x0f,
	NVME_FEAT_HCTM		= 0X10,
	NVME_FEAT_NOPSC		= 0X11,
	NVME_FEAT_RRL		= 0x12,
	NVME_FEAT_PLM_CONFIG	= 0x13,
	NVME_FEAT_PLM_WINDOW	= 0x14,
	NVME_FEAT_SW_PROGRESS	= 0x80,
	NVME_FEAT_HOST_ID	= 0x81,
	NVME_FEAT_RESV_MASK	= 0x82,
	NVME_FEAT_RESV_PERSIST	= 0x83,
	NVME_LOG_ERROR		= 0x01,
	NVME_LOG_SMART		= 0x02,
	NVME_LOG_FW_SLOT	= 0x03,
	NVME_LOG_CHANGED_NS	= 0x04,
	NVME_LOG_CMD_EFFECTS	= 0x05,
	NVME_LOG_DEVICE_SELF_TEST = 0x06,
	NVME_LOG_TELEMETRY_HOST = 0x07,
	NVME_LOG_TELEMETRY_CTRL = 0x08,
	NVME_LOG_ENDURANCE_GROUP = 0x09,
	NVME_LOG_DISC		= 0x70,
	NVME_LOG_RESERVATION	= 0x80,
	NVME_LOG_SANITIZE	= 0x81,
	NVME_FWACT_REPL		= (0 << 3),
	NVME_FWACT_REPL_ACTV	= (1 << 3),
	NVME_FWACT_ACTV		= (2 << 3),
};



enum {
	NVME_NO_LOG_LSP       = 0x0,
	NVME_NO_LOG_LPO       = 0x0,
	NVME_LOG_ANA_LSP_RGO  = 0x1,
	NVME_TELEM_LSP_CREATE = 0x1,
};


enum nvme_admin_opcode {
	nvme_admin_delete_sq		= 0x00,
	nvme_admin_create_sq		= 0x01,
	nvme_admin_get_log_page		= 0x02,
	nvme_admin_delete_cq		= 0x04,
	nvme_admin_create_cq		= 0x05,
	nvme_admin_identify		= 0x06,
	nvme_admin_abort_cmd		= 0x08,
	nvme_admin_set_features		= 0x09,
	nvme_admin_get_features		= 0x0a,
	nvme_admin_async_event		= 0x0c,
	nvme_admin_ns_mgmt		= 0x0d,
	nvme_admin_activate_fw		= 0x10,
	nvme_admin_download_fw		= 0x11,
	nvme_admin_dev_self_test	= 0x14,
	nvme_admin_ns_attach		= 0x15,
	nvme_admin_keep_alive		= 0x18,
	nvme_admin_directive_send	= 0x19,
	nvme_admin_directive_recv	= 0x1a,
	nvme_admin_virtual_mgmt		= 0x1c,
	nvme_admin_nvme_mi_send		= 0x1d,
	nvme_admin_nvme_mi_recv		= 0x1e,
	nvme_admin_dbbuf		= 0x7C,
	nvme_admin_format_nvm		= 0x80,
	nvme_admin_security_send	= 0x81,
	nvme_admin_security_recv	= 0x82,
	nvme_admin_sanitize_nvm		= 0x84,
};


struct nvme_passthru_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32	result;
};

struct spdk_nvme_passthru_cmd {
	struct nvme_passthru_cmd	*cmd;
	bool				failed;
};

#pragma pack(push,1)
struct nvme_additional_smart_log_item {
	__u8			key;
	__u8			_kp[2];
	__u8			norm;
	__u8			_np;
	union {
		__u8		raw[6];
		struct wear_level {
			__le16	min;
			__le16	max;
			__le16	avg;
		} wear_level ;
		struct thermal_throttle {
			__u8	pct;
			__u32	count;
		} thermal_throttle;
	};
	__u8			_rp;
};
#pragma pack(pop)

struct nvme_additional_smart_log {
	struct nvme_additional_smart_log_item	program_fail_cnt;
	struct nvme_additional_smart_log_item	erase_fail_cnt;
	struct nvme_additional_smart_log_item	wear_leveling_cnt;
	struct nvme_additional_smart_log_item	e2e_err_cnt;
	struct nvme_additional_smart_log_item	crc_err_cnt;
	struct nvme_additional_smart_log_item	timed_workload_media_wear;
	struct nvme_additional_smart_log_item	timed_workload_host_reads;
	struct nvme_additional_smart_log_item	timed_workload_timer;
	struct nvme_additional_smart_log_item	thermal_throttle_status;
	struct nvme_additional_smart_log_item	retry_buffer_overflow_cnt;
	struct nvme_additional_smart_log_item	pll_lock_loss_cnt;
	struct nvme_additional_smart_log_item	nand_bytes_written;
	struct nvme_additional_smart_log_item	host_bytes_written;
};




enum {
	TOTAL_WRITE,
	TOTAL_READ,
	THERMAL_THROTTLE,
	TEMPT_SINCE_RESET,
	POWER_CONSUMPTION,
	TEMPT_SINCE_BOOTUP,
	POWER_LOSS_PROTECTION,
	WEARLEVELING_COUNT,
	HOST_WRITE,
	THERMAL_THROTTLE_CNT,
	NR_SMART_ITEMS,
};

#pragma pack(push, 1)
struct nvme_memblaze_smart_log_item {
	__u8 id[3];
	union {
		__u8	__nmval[2];
		__le16  nmval;
	};
	union {
		__u8 rawval[6];
		struct temperature {
			__le16 max;
			__le16 min;
			__le16 curr;
		} temperature;
		struct power {
			__le16 max;
			__le16 min;
			__le16 curr;
		} power;
		struct thermal_throttle_mb {
			__u8 on;
			__u32 count;
		} thermal_throttle;
		struct temperature_p {
			__le16 max;
			__le16 min;
		} temperature_p;
		struct power_loss_protection {
			__u8 curr;
		} power_loss_protection;
		struct wearleveling_count {
			__le16 min;
			__le16 max;
			__le16 avg;
		} wearleveling_count;
		struct thermal_throttle_cnt {
			__u8 active;
			__le32 cnt;
		} thermal_throttle_cnt;
	};
	__u8 resv;
};
#pragma pack(pop)


struct nvme_memblaze_smart_log {
	struct nvme_memblaze_smart_log_item items[NR_SMART_ITEMS];
	__u8 resv[512 - sizeof(struct nvme_memblaze_smart_log_item) * NR_SMART_ITEMS];
};

uint64_t int48_to_long(__u8 *data);
static long double int128_to_double(__u8 *data);
static void nvme_spdk_get_cmd_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl);
static void json_intel_smart_log(struct nvme_additional_smart_log *smart, const char *devname);
void json_smart_log(struct nvme_smart_log *smart, const char *devname);
int nvme_submit_admin_passthru(struct nvme_passthru_cmd *cmd, struct spdk_nvme_ctrlr *ctrlr);


int nvme_get_log13(__u32 nsid, __u8 log_id, __u8 lsp, __u64 lpo, __u16 lsi, bool rae,
		   __u32 data_len, void *data, struct spdk_nvme_ctrlr *ctrlr);
int nvme_get_log(__u32 nsid, __u8 log_id, __u32 data_len, void *data,
		 struct spdk_nvme_ctrlr *ctrlr);
int nvme_smart_log(__u32 nsid, struct nvme_smart_log *smart_log, struct spdk_nvme_ctrlr *ctrlr);
void bdev_nvme_print_smart_log(struct spdk_nvme_ctrlr *ctrlr);
void bdev_nvme_print_intel_smart_log(struct spdk_nvme_ctrlr *ctrlr);



uint64_t int48_to_long(__u8 *data)
{
	int i;
	uint64_t result = 0;

	for (i = 0; i < 6; i++) {
		result *= 256;
		result += data[5 - i];
	}
	return result;
}


static long double int128_to_double(__u8 *data)
{
	int i;
	long double result = 0;

	for (i = 0; i < 16; i++) {
		result *= 256;
		result += data[15 - i];
	}
	return result;
}



static inline int
nvme_spdk_get_error_code(const struct spdk_nvme_cpl *cpl)
{
	return (cpl->status.sct << 8) | cpl->status.sc;
}


static void
nvme_spdk_get_cmd_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_passthru_cmd *spdk_cmd = (struct spdk_nvme_passthru_cmd *)cb_arg;

	if (spdk_cmd->cmd == NULL || spdk_cmd->failed == true) {
		outstanding_commands--;
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "command error: SC %x SCT %x\n", cpl->status.sc, cpl->status.sct);
		spdk_cmd->cmd->result = nvme_spdk_get_error_code(cpl);
		spdk_cmd->failed = true;
	} else {
		spdk_cmd->cmd->result = cpl->cdw0;
	}

	outstanding_commands--;
}


static void json_intel_smart_log(struct nvme_additional_smart_log *smart,
				 const char *devname)
{
	printf("Additional Smart Log for NVME device:%s\n",
	       devname);
	printf("key                               normalized raw\n");
	printf("program_fail_count              : %3d%%       %"PRIu64"\n",
	       smart->program_fail_cnt.norm,
	       int48_to_long(smart->program_fail_cnt.raw));
	printf("erase_fail_count                : %3d%%       %"PRIu64"\n",
	       smart->erase_fail_cnt.norm,
	       int48_to_long(smart->erase_fail_cnt.raw));
	printf("wear_leveling                   : %3d%%       min: %u, max: %u, avg: %u\n",
	       smart->wear_leveling_cnt.norm,
	       le16_to_cpu(smart->wear_leveling_cnt.wear_level.min),
	       le16_to_cpu(smart->wear_leveling_cnt.wear_level.max),
	       le16_to_cpu(smart->wear_leveling_cnt.wear_level.avg));
	printf("end_to_end_error_detection_count: %3d%%       %"PRIu64"\n",
	       smart->e2e_err_cnt.norm,
	       int48_to_long(smart->e2e_err_cnt.raw));
	printf("crc_error_count                 : %3d%%       %"PRIu64"\n",
	       smart->crc_err_cnt.norm,
	       int48_to_long(smart->crc_err_cnt.raw));
	printf("timed_workload_media_wear       : %3d%%       %.3f%%\n",
	       smart->timed_workload_media_wear.norm,
	       ((float)int48_to_long(smart->timed_workload_media_wear.raw)) / 1024);
	printf("timed_workload_host_reads       : %3d%%       %"PRIu64"%%\n",
	       smart->timed_workload_host_reads.norm,
	       int48_to_long(smart->timed_workload_host_reads.raw));
	printf("timed_workload_timer            : %3d%%       %"PRIu64" min\n",
	       smart->timed_workload_timer.norm,
	       int48_to_long(smart->timed_workload_timer.raw));
	printf("thermal_throttle_status         : %3d%%       %u%%, cnt: %u\n",
	       smart->thermal_throttle_status.norm,
	       smart->thermal_throttle_status.thermal_throttle.pct,
	       smart->thermal_throttle_status.thermal_throttle.count);
	printf("retry_buffer_overflow_count     : %3d%%       %"PRIu64"\n",
	       smart->retry_buffer_overflow_cnt.norm,
	       int48_to_long(smart->retry_buffer_overflow_cnt.raw));
	printf("pll_lock_loss_count             : %3d%%       %"PRIu64"\n",
	       smart->pll_lock_loss_cnt.norm,
	       int48_to_long(smart->pll_lock_loss_cnt.raw));
	printf("nand_bytes_written              : %3d%%       sectors: %"PRIu64"\n",
	       smart->nand_bytes_written.norm,
	       int48_to_long(smart->nand_bytes_written.raw));
	printf("host_bytes_written              : %3d%%       sectors: %"PRIu64"\n",
	       smart->host_bytes_written.norm,
	       int48_to_long(smart->host_bytes_written.raw));
}

void json_smart_log(struct nvme_smart_log *smart, const char *devname)
{
	/* convert temperature from Kelvin to Celsius */
	int temperature = ((smart->temperature[1] << 8) |
			   smart->temperature[0]) - 273;
	int i;

	printf("Smart Log for NVME device:%s\n", devname);
	printf("critical_warning                    : %#x\n", smart->critical_warning);
	printf("temperature                         : %d C\n", temperature);
	printf("available_spare                     : %u%%\n", smart->avail_spare);
	printf("available_spare_threshold           : %u%%\n", smart->spare_thresh);
	printf("percentage_used                     : %u%%\n", smart->percent_used);
	printf("data_units_read                     : %'.0Lf\n",
	       int128_to_double(smart->data_units_read));
	printf("data_units_written                  : %'.0Lf\n",
	       int128_to_double(smart->data_units_written));
	printf("host_read_commands                  : %'.0Lf\n",
	       int128_to_double(smart->host_reads));
	printf("host_write_commands                 : %'.0Lf\n",
	       int128_to_double(smart->host_writes));
	printf("controller_busy_time                : %'.0Lf\n",
	       int128_to_double(smart->ctrl_busy_time));
	printf("power_cycles                        : %'.0Lf\n",
	       int128_to_double(smart->power_cycles));
	printf("power_on_hours                      : %'.0Lf\n",
	       int128_to_double(smart->power_on_hours));
	printf("unsafe_shutdowns                    : %'.0Lf\n",
	       int128_to_double(smart->unsafe_shutdowns));
	printf("media_errors                        : %'.0Lf\n",
	       int128_to_double(smart->media_errors));
	printf("num_err_log_entries                 : %'.0Lf\n",
	       int128_to_double(smart->num_err_log_entries));
	printf("Warning Temperature Time            : %u\n", le32_to_cpu(smart->warning_temp_time));
	printf("Critical Composite Temperature Time : %u\n", le32_to_cpu(smart->critical_comp_time));
	for (i = 0; i < 8; i++) {
		__s32 temp = le16_to_cpu(smart->temp_sensor[i]);
		if (temp == 0) {
			continue;
		}
		printf("Temperature Sensor %d                : %d C\n", i + 1,
		       temp - 273);
	}
	printf("Thermal Management T1 Trans Count   : %u\n", le32_to_cpu(smart->thm_temp1_trans_count));
	printf("Thermal Management T2 Trans Count   : %u\n", le32_to_cpu(smart->thm_temp2_trans_count));
	printf("Thermal Management T1 Total Time    : %u\n", le32_to_cpu(smart->thm_temp1_total_time));
	printf("Thermal Management T2 Total Time    : %u\n", le32_to_cpu(smart->thm_temp2_total_time));

}


int
nvme_submit_admin_passthru(struct nvme_passthru_cmd *cmd, struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;
	struct spdk_nvme_cmd *spdk_cmd = (struct spdk_nvme_cmd *)cmd;
	struct spdk_nvme_passthru_cmd spdk_nvme_cmd = {};
	void *contig_buffer = NULL;
	enum spdk_nvme_data_transfer xfer;

	xfer = spdk_nvme_opc_get_data_transfer(cmd->opcode);

	if (cmd->data_len != 0) {
		contig_buffer = spdk_dma_zmalloc(cmd->data_len, 128, NULL);
		if (!contig_buffer) {
			return 1;
		}
	}

	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		if (contig_buffer) {
			memcpy(contig_buffer, (void *)cmd->addr, cmd->data_len);
		}
	}

	spdk_nvme_cmd.cmd = cmd;
	spdk_nvme_cmd.failed = false;

	outstanding_commands = 0;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, spdk_cmd, contig_buffer, cmd->data_len,
					   nvme_spdk_get_cmd_completion, &spdk_nvme_cmd);


	if (rc != 0) {
		fprintf(stderr, "send command failed 0x%x\n", rc);
		return rc;
	}

	outstanding_commands++;

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (spdk_nvme_cmd.failed == true) {
		rc = cmd->result;
	} else if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (contig_buffer) {
			memcpy((void *)cmd->addr, contig_buffer, cmd->data_len);
		}
	}

	spdk_dma_free(contig_buffer);

	return rc;
}


int nvme_get_log13(__u32 nsid, __u8 log_id, __u8 lsp, __u64 lpo,
		   __u16 lsi, bool rae, __u32 data_len, void *data, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_admin_cmd cmd = {
		.opcode		= nvme_admin_get_log_page,
		.nsid		= nsid,
		.addr		= (__u64)(uintptr_t) data,
		.data_len	= data_len,
	};
	__u32 numd = (data_len >> 2) - 1;
	__u16 numdu = numd >> 16, numdl = numd & 0xffff;

	cmd.cdw10 = log_id | (numdl << 16) | (rae ? 1 << 15 : 0);
	if (lsp) { cmd.cdw10 |= lsp << 8; }

	cmd.cdw11 = numdu | (lsi << 16);
	cmd.cdw12 = lpo;
	cmd.cdw13 = (lpo >> 32);

	return nvme_submit_admin_passthru(&cmd, ctrlr);
}



int nvme_get_log(__u32 nsid, __u8 log_id, __u32 data_len, void *data, struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_get_log13(nsid, log_id, NVME_NO_LOG_LSP, NVME_NO_LOG_LPO,
			      0, 0, data_len, data, ctrlr);
}


int nvme_smart_log(__u32 nsid, struct nvme_smart_log *smart_log, struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_get_log(nsid, NVME_LOG_SMART, sizeof(*smart_log), smart_log, ctrlr);
}

void
bdev_nvme_print_smart_log(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_smart_log smart_log;

	int err = nvme_smart_log(NVME_NSID_ALL, &smart_log, ctrlr);
	if (!err) {
		json_smart_log(&smart_log, ctrlr->trid.traddr);
	}
}

void
bdev_nvme_print_intel_smart_log(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_additional_smart_log smart_log;

	int err = nvme_get_log(NVME_NSID_ALL, 0xca, sizeof(smart_log), &smart_log, ctrlr);
	if (!err) {
		json_intel_smart_log(&smart_log, ctrlr->trid.traddr);
	}
}

void
bdev_nvme_print_log(struct spdk_nvme_ctrlr *ctrlr)
{
	fflush(stdout);
	printf("\n");
	printf("----------------------\n");
	bdev_nvme_print_smart_log(ctrlr);
	printf("----------------------\n");
	bdev_nvme_print_intel_smart_log(ctrlr);
	printf("----------------------\n");
	fflush(stdout);
}
