/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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
