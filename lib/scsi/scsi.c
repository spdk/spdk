/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "scsi_internal.h"

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

SPDK_LOG_REGISTER_COMPONENT(scsi)
