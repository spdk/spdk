/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "scsi_internal.h"
#include "spdk/util.h"

int
spdk_scsi_init(void)
{
	return 0;
}

void
spdk_scsi_fini(void)
{
}

SPDK_TRACE_REGISTER_FN(scsi_trace, "scsi", TRACE_GROUP_SCSI)
{
	spdk_trace_register_owner(OWNER_SCSI_DEV, 'd');
	spdk_trace_register_object(OBJECT_SCSI_TASK, 't');
	spdk_trace_register_description("SCSI_TASK_DONE", TRACE_SCSI_TASK_DONE,
					OWNER_SCSI_DEV, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("SCSI_TASK_START", TRACE_SCSI_TASK_START,
					OWNER_SCSI_DEV, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
}

uint64_t
spdk_scsi_lun_id_int_to_fmt(int lun_id)
{
	uint64_t fmt_lun, method;

	if (lun_id < 0x0100) {
		/* below 256 */
		method = 0x00U;
		fmt_lun = (method & 0x03U) << 62;
		fmt_lun |= ((uint64_t)lun_id & 0x00ffU) << 48;
	} else if (lun_id < 0x4000) {
		/* below 16384 */
		method = 0x01U;
		fmt_lun = (method & 0x03U) << 62;
		fmt_lun |= ((uint64_t)lun_id & 0x3fffU) << 48;
	} else {
		/* XXX */
		fmt_lun = 0;
	}

	return fmt_lun;
}

int
spdk_scsi_lun_id_fmt_to_int(uint64_t fmt_lun)
{
	uint64_t method;
	int lun_i;

	method = (fmt_lun >> 62) & 0x03U;
	fmt_lun = fmt_lun >> 48;
	if (method == 0x00U) {
		lun_i = (int)(fmt_lun & 0x00ffU);
	} else if (method == 0x01U) {
		lun_i = (int)(fmt_lun & 0x3fffU);
	} else {
		lun_i = 0xffffU;
	}
	return lun_i;
}

struct scsi_sbc_opcode_string {
	enum spdk_sbc_opcode opc;
	const char *str;
};

static const struct scsi_sbc_opcode_string scsi_sbc_opcode_strings[] = {
	{ SPDK_SBC_COMPARE_AND_WRITE, "COMPARE AND WRITE" },
	{ SPDK_SBC_FORMAT_UNIT, "FORMAT UNIT" },
	{ SPDK_SBC_GET_LBA_STATUS, "GET LBA STATUS" },
	{ SPDK_SBC_ORWRITE_16, "ORWRITE 16" },
	{ SPDK_SBC_PRE_FETCH_10, "PRE FETCH 10" },
	{ SPDK_SBC_PRE_FETCH_16, "PRE FETCH 16" },
	{ SPDK_SBC_READ_6, "READ 6" },
	{ SPDK_SBC_READ_10, "READ 10" },
	{ SPDK_SBC_READ_12, "READ 12" },
	{ SPDK_SBC_READ_16, "READ 16" },
	{ SPDK_SBC_READ_ATTRIBUTE, "READ ATTRIBUTE" },
	{ SPDK_SBC_READ_BUFFER, "READ BUFFER" },
	{ SPDK_SBC_READ_CAPACITY_10, "READ CAPACITY 10" },
	{ SPDK_SBC_READ_DEFECT_DATA_10, "READ DEFECT DATA 10" },
	{ SPDK_SBC_READ_DEFECT_DATA_12, "READ DEFECT DATA 12" },
	{ SPDK_SBC_READ_LONG_10, "READ LONG 10" },
	{ SPDK_SBC_REASSIGN_BLOCKS, "REASSIGN BLOCKS" },
	{ SPDK_SBC_SANITIZE, "SANITIZE" },
	{ SPDK_SBC_START_STOP_UNIT, "START STOP UNIT" },
	{ SPDK_SBC_SYNCHRONIZE_CACHE_10, "SYNCHRONIZE CACHE 10" },
	{ SPDK_SBC_SYNCHRONIZE_CACHE_16, "SYNCHRONIZE CACHE 16" },
	{ SPDK_SBC_UNMAP, "UNMAP" },
	{ SPDK_SBC_VERIFY_10, "VERIFY 10" },
	{ SPDK_SBC_VERIFY_12, "VERIFY 12" },
	{ SPDK_SBC_VERIFY_16, "VERIFY 16" },
	{ SPDK_SBC_WRITE_6, "WRITE 6" },
	{ SPDK_SBC_WRITE_10, "WRITE 10" },
	{ SPDK_SBC_WRITE_12, "WRITE 12" },
	{ SPDK_SBC_WRITE_16, "WRITE 16" },
	{ SPDK_SBC_WRITE_AND_VERIFY_10, "WRITE AND VERIFY 10" },
	{ SPDK_SBC_WRITE_AND_VERIFY_12, "WRITE AND VERIFY 12" },
	{ SPDK_SBC_WRITE_AND_VERIFY_16, "WRITE AND VERIFY 16" },
	{ SPDK_SBC_WRITE_LONG_10, "WRITE LONG 10" },
	{ SPDK_SBC_WRITE_SAME_10, "WRITE SAME 10" },
	{ SPDK_SBC_WRITE_SAME_16, "WRITE SAME 16" },
	{ SPDK_SBC_XDREAD_10, "XDREAD 10" },
	{ SPDK_SBC_XDWRITE_10, "XDWRITE 10" },
	{ SPDK_SBC_XDWRITEREAD_10, "XDWRITEREAD 10" },
	{ SPDK_SBC_XPWRITE_10, "XPWRITE 10" }
};

const char *
spdk_scsi_sbc_opcode_string(uint8_t opcode, uint16_t sa)
{
	uint8_t i;

	/* FIXME: sa is unsupported currently, support variable length CDBs if necessary */
	for (i = 0; i < SPDK_COUNTOF(scsi_sbc_opcode_strings); i++) {
		if (scsi_sbc_opcode_strings[i].opc == opcode) {
			return scsi_sbc_opcode_strings[i].str;
		}
	}

	return "UNKNOWN";
}

SPDK_LOG_REGISTER_COMPONENT(scsi)
