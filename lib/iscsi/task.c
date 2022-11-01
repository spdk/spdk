/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
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

	if (iscsi_task_get_mobj(task)) {
		iscsi_datapool_put(iscsi_task_get_mobj(task));
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
