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
#include "spdk/endian.h"
#include "spdk/io_channel.h"

void
spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (lun) {
		spdk_trace_record(TRACE_SCSI_TASK_DONE, lun->dev->id, 0, (uintptr_t)task, 0);
	}
	spdk_event_call(task->cb_event);

	if (lun && !TAILQ_EMPTY(&lun->pending_tasks)) {
		spdk_scsi_lun_execute_tasks(lun);
	}
}

void
spdk_scsi_lun_clear_all(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_task *task, *task_tmp;

	/*
	 * This function is called from one location, after the backend LUN
	 * device was reset. Can assume are no active tasks in the
	 * backend that need to be terminated.  Just need to queue all tasks
	 * back to frontend for any final processing and cleanup.
	 *
	 * Queue the tasks back roughly in the order they were received
	 * ('cleanup' = oldest, 'tasks' = current, and 'pending' = newest)
	 */

	TAILQ_FOREACH_SAFE(task, &lun->tasks, scsi_link, task_tmp) {
		TAILQ_REMOVE(&lun->tasks, task, scsi_link);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ABORTED_COMMAND,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		spdk_scsi_lun_complete_task(lun, task);
	}

	TAILQ_FOREACH_SAFE(task, &lun->pending_tasks, scsi_link, task_tmp) {
		TAILQ_REMOVE(&lun->pending_tasks, task, scsi_link);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ABORTED_COMMAND,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		spdk_scsi_lun_complete_task(lun, task);
	}
}

static int
spdk_scsi_lun_abort_all(struct spdk_scsi_task *mtask,
			struct spdk_scsi_lun *lun,
			struct spdk_scsi_port *initiator_port)
{
	if (!lun) {
		mtask->response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		return -1;
	}

	mtask->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
	return -1;
}

static int
spdk_scsi_lun_abort_task(struct spdk_scsi_task *mtask,
			 struct spdk_scsi_lun *lun,
			 struct spdk_scsi_port *initiator_port,
			 uint32_t task_tag)
{
	if (!lun) {
		/* LUN does not exist */
		mtask->response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		return -1;
	}

	mtask->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
	return -1;
}

static int
spdk_scsi_lun_reset(struct spdk_scsi_task *mtask, struct spdk_scsi_lun *lun)
{
	if (!lun) {
		/* LUN does not exist */
		mtask->response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		spdk_scsi_lun_complete_task(NULL, mtask);
		return -1;
	}

	spdk_bdev_scsi_reset(lun->bdev, mtask);
	return 0;
}

int
spdk_scsi_lun_task_mgmt_execute(struct spdk_scsi_task *task)
{
	int rc;

	if (!task) {
		return -1;
	}

	switch (task->function) {
	case SPDK_SCSI_TASK_FUNC_ABORT_TASK:
		rc = spdk_scsi_lun_abort_task(task, task->lun,
					      task->initiator_port,
					      task->abort_id);
		if (rc < 0) {
			SPDK_ERRLOG("ABORT_TASK failed\n");
		}

		break;

	case SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET:
		rc = spdk_scsi_lun_abort_all(task, task->lun,
					     task->initiator_port);
		if (rc < 0) {
			SPDK_ERRLOG("ABORT_TASK_SET failed\n");
		}

		break;

	case SPDK_SCSI_TASK_FUNC_LUN_RESET:
		rc = spdk_scsi_lun_reset(task, task->lun);
		if (rc < 0) {
			SPDK_ERRLOG("LUN_RESET failed\n");
		}
		return rc;

	default:
		SPDK_ERRLOG("Unknown Task Management Function!\n");
		/*
		 * Task management functions other than those above should never
		 * reach this point having been filtered by the frontend. Reject
		 * the task as being unsupported.
		 */
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		rc = -1;
		break;
	}

	spdk_scsi_lun_complete_task(task->lun, task);

	return rc;
}

static void
complete_task_with_no_lun(struct spdk_scsi_task *task)
{
	uint8_t buffer[36];
	uint32_t allocation_len;
	uint32_t data_len;

	if (task->cdb[0] == SPDK_SPC_INQUIRY) {
		/*
		 * SPC-4 states that INQUIRY commands to an unsupported LUN
		 *  must be served with PERIPHERAL QUALIFIER = 0x3 and
		 *  PERIPHERAL DEVICE TYPE = 0x1F.
		 */
		data_len = sizeof(buffer);

		memset(buffer, 0, data_len);
		/* PERIPHERAL QUALIFIER(7-5) PERIPHERAL DEVICE TYPE(4-0) */
		buffer[0] = 0x03 << 5 | 0x1f;
		/* ADDITIONAL LENGTH */
		buffer[4] = data_len - 5;

		allocation_len = from_be16(&task->cdb[3]);
		if (spdk_scsi_task_scatter_data(task, buffer, SPDK_MIN(allocation_len, data_len)) >= 0) {
			task->data_transferred = data_len;
			task->status = SPDK_SCSI_STATUS_GOOD;
		}
	} else {
		/* LOGICAL UNIT NOT SUPPORTED */
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_SUPPORTED,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		task->data_transferred = 0;
	}
	spdk_scsi_lun_complete_task(NULL, task);
}

int
spdk_scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (lun == NULL) {
		complete_task_with_no_lun(task);
		return -1;
	}

	TAILQ_INSERT_TAIL(&lun->pending_tasks, task, scsi_link);
	return 0;
}

void
spdk_scsi_lun_execute_tasks(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_task *task, *task_tmp;
	int rc;

	TAILQ_FOREACH_SAFE(task, &lun->pending_tasks, scsi_link, task_tmp) {
		task->status = SPDK_SCSI_STATUS_GOOD;
		task->ch = lun->io_channel;
		spdk_trace_record(TRACE_SCSI_TASK_START, lun->dev->id, task->length, (uintptr_t)task, 0);
		rc = spdk_bdev_scsi_execute(lun->bdev, task);

		/* Task is removed from the pending list if it gets the slot. */
		if (task->status == SPDK_SCSI_STATUS_TASK_SET_FULL) {
			break;
		}

		TAILQ_REMOVE(&lun->pending_tasks, task, scsi_link);

		switch (rc) {
		case SPDK_SCSI_TASK_PENDING:
			TAILQ_INSERT_TAIL(&lun->tasks, task, scsi_link);
			break;

		case SPDK_SCSI_TASK_COMPLETE:
			spdk_scsi_lun_complete_task(lun, task);
			break;

		default:
			abort();
		}
	}
}

/*!

\brief Constructs a new spdk_scsi_lun object based on the provided parameters.

\param name Name for the SCSI LUN.
\param blockdev  Blockdev associated with this LUN

\return NULL if blockdev == NULL
\return pointer to the new spdk_scsi_lun object otherwise

*/
_spdk_scsi_lun *
spdk_scsi_lun_construct(const char *name, struct spdk_bdev *bdev)
{
	struct spdk_scsi_lun *lun;
	int rc;

	if (bdev == NULL) {
		SPDK_ERRLOG("blockdev must be non-NULL\n");
		return NULL;
	}

	lun = spdk_lun_db_get_lun(name, 0);
	if (lun) {
		return lun;
	}

	lun = calloc(1, sizeof(*lun));
	if (lun == NULL) {
		SPDK_ERRLOG("could not allocate lun\n");
		return NULL;
	}

	TAILQ_INIT(&lun->tasks);
	TAILQ_INIT(&lun->pending_tasks);

	lun->bdev = bdev;
	strncpy(lun->name, name, sizeof(lun->name));

	rc = spdk_scsi_lun_db_add(lun);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to add LUN %s to DB\n", lun->name);
		free(lun);
		return NULL;
	}

	return lun;
}

static int
spdk_scsi_lun_destruct(struct spdk_scsi_lun *lun)
{
	spdk_scsi_lun_db_delete(lun);

	free(lun);

	return 0;
}

int
spdk_scsi_lun_claim(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_lun *tmp = spdk_lun_db_get_lun(lun->name, 1);

	if (tmp == NULL) {
		return -1;
	}

	return 0;
}

int
spdk_scsi_lun_unclaim(struct spdk_scsi_lun *lun)
{
	spdk_lun_db_put_lun(lun->name);
	lun->dev = NULL;

	return 0;
}

int
spdk_scsi_lun_deletable(const char *name)
{
	int ret = 0;
	struct spdk_scsi_lun *lun;

	pthread_mutex_lock(&g_spdk_scsi.mutex);
	lun = spdk_lun_db_get_lun(name, 0);
	if (lun == NULL) {
		ret = -1;
		goto out;
	}

out:
	pthread_mutex_unlock(&g_spdk_scsi.mutex);
	return ret;
}

void
spdk_scsi_lun_delete(const char *lun_name)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_dev *dev;
	struct spdk_lun_db_entry *current;

	pthread_mutex_lock(&g_spdk_scsi.mutex);
	current = spdk_scsi_lun_list_head;
	while (current != NULL) {
		lun = current->lun;
		if (strncmp(lun->name, lun_name, sizeof(lun->name)) == 0) {
			break;
		}
		current = current->next;
	}

	if (current == NULL) {
		pthread_mutex_unlock(&g_spdk_scsi.mutex);
		return;
	}

	dev = lun->dev;

	/* Remove the LUN from the device */
	if (dev != NULL) {
		spdk_scsi_dev_delete_lun(dev, lun);
	}

	/* LUNs are always created in a pair with a blockdev.
	 * Delete the blockdev associated with this lun.
	 */
	spdk_bdev_unregister(lun->bdev);

	/* Destroy this lun */
	spdk_scsi_lun_destruct(lun);
	pthread_mutex_unlock(&g_spdk_scsi.mutex);
}

int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel != NULL) {
		if (pthread_self() == lun->thread_id) {
			return 0;
		}
		SPDK_ERRLOG("io_channel already allocated for lun %s\n", lun->name);
		return -1;
	}

	lun->io_channel = spdk_bdev_get_io_channel(lun->bdev, SPDK_IO_PRIORITY_DEFAULT);
	if (lun->io_channel == NULL) {
		return -1;
	}
	lun->thread_id = pthread_self();
	return 0;
}

void spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel != NULL) {
		spdk_put_io_channel(lun->io_channel);
		lun->io_channel = NULL;
	}
}
