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

#include "spdk/env.h"
#include "spdk/log.h"
#include "iscsi/conn.h"
#include "iscsi/task.h"

static void
iscsi_task_free(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task);

	if (task->parent) {
		if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
			assert(task->conn->data_in_cnt > 0);
			task->conn->data_in_cnt--;
		}

		spdk_scsi_task_put(&task->parent->scsi);
		task->parent = NULL;
	}

	iscsi_task_disassociate_pdu(task);
	assert(task->conn->pending_task_cnt > 0);
	task->conn->pending_task_cnt--;
	spdk_mempool_put(g_iscsi.task_pool, (void *)task);
}

struct spdk_iscsi_task *
iscsi_task_get(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *parent,
	       spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	task = spdk_mempool_get(g_iscsi.task_pool);
	if (!task) {
		SPDK_ERRLOG("Unable to get task\n");
		abort();
	}

	assert(conn != NULL);
	memset(task, 0, sizeof(*task));
	task->conn = conn;
	assert(conn->pending_task_cnt < UINT32_MAX);
	conn->pending_task_cnt++;
	spdk_scsi_task_construct(&task->scsi,
				 cpl_fn,
				 iscsi_task_free);
	if (parent) {
		parent->scsi.ref++;
		task->parent = parent;
		task->tag = parent->tag;
		task->lun_id = parent->lun_id;
		task->scsi.dxfer_dir = parent->scsi.dxfer_dir;
		task->scsi.transfer_len = parent->scsi.transfer_len;
		task->scsi.lun = parent->scsi.lun;
		task->scsi.cdb = parent->scsi.cdb;
		task->scsi.target_port = parent->scsi.target_port;
		task->scsi.initiator_port = parent->scsi.initiator_port;
		if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
			conn->data_in_cnt++;
		}
	}

	return task;
}
