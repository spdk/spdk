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
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/event.h"
#include "spdk/util.h"

void
spdk_scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (lun) {
		TAILQ_REMOVE(&lun->tasks, task, scsi_link);
		spdk_trace_record(TRACE_SCSI_TASK_DONE, lun->dev->id, 0, (uintptr_t)task, 0);
	}
	task->cpl_fn(task);
}

void
spdk_scsi_lun_complete_mgmt_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (task->function == SPDK_SCSI_TASK_FUNC_LUN_RESET &&
	    task->status == SPDK_SCSI_STATUS_GOOD) {
		spdk_scsi_lun_clear_all(task->lun);
	}
	task->cpl_fn(task);
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
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ABORTED_COMMAND,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		spdk_scsi_lun_complete_task(lun, task);
	}

	TAILQ_FOREACH_SAFE(task, &lun->pending_tasks, scsi_link, task_tmp) {
		TAILQ_REMOVE(&lun->pending_tasks, task, scsi_link);
		TAILQ_INSERT_TAIL(&lun->tasks, task, scsi_link);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ABORTED_COMMAND,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		spdk_scsi_lun_complete_task(lun, task);
	}
}

int
spdk_scsi_lun_task_mgmt_execute(struct spdk_scsi_task *task,
				enum spdk_scsi_task_func func)
{
	if (!task) {
		return -1;
	}

	if (!task->lun) {
		/* LUN does not exist */
		task->response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		spdk_scsi_lun_complete_mgmt_task(NULL, task);
		return -1;
	}

	task->ch = task->lun->io_channel;
	task->desc = task->lun->bdev_desc;

	switch (func) {
	case SPDK_SCSI_TASK_FUNC_ABORT_TASK:
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_ERRLOG("ABORT_TASK failed\n");
		break;

	case SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET:
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_ERRLOG("ABORT_TASK_SET failed\n");
		break;

	case SPDK_SCSI_TASK_FUNC_LUN_RESET:
		spdk_bdev_scsi_reset(task->lun->bdev, task);
		return 0;

	default:
		SPDK_ERRLOG("Unknown Task Management Function!\n");
		/*
		 * Task management functions other than those above should never
		 * reach this point having been filtered by the frontend. Reject
		 * the task as being unsupported.
		 */
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		break;
	}

	spdk_scsi_lun_complete_mgmt_task(task->lun, task);

	return -1;
}

void
spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task)
{
	uint8_t buffer[36];
	uint32_t allocation_len;
	uint32_t data_len;

	task->length = task->transfer_len;
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
		if (spdk_scsi_task_scatter_data(task, buffer, spdk_min(allocation_len, data_len)) >= 0) {
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
}

int
spdk_scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
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
		task->desc = lun->bdev_desc;
		spdk_trace_record(TRACE_SCSI_TASK_START, lun->dev->id, task->length, (uintptr_t)task, 0);
		TAILQ_REMOVE(&lun->pending_tasks, task, scsi_link);
		TAILQ_INSERT_TAIL(&lun->tasks, task, scsi_link);
		if (!lun->removed) {
			rc = spdk_bdev_scsi_execute(lun->bdev, task);
		} else {
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ABORTED_COMMAND,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			rc = SPDK_SCSI_TASK_COMPLETE;
		}

		switch (rc) {
		case SPDK_SCSI_TASK_PENDING:
			break;

		case SPDK_SCSI_TASK_COMPLETE:
			spdk_scsi_lun_complete_task(lun, task);
			break;

		default:
			abort();
		}
	}
}

static void
spdk_scsi_lun_hotplug(void *arg)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)arg;

	if (!spdk_scsi_lun_has_pending_tasks(lun)) {
		spdk_scsi_lun_free_io_channel(lun);
		spdk_scsi_lun_delete(lun->name);
	}
}

static void
spdk_scsi_lun_hot_remove(void *remove_ctx)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)remove_ctx;

	lun->removed = true;
	if (lun->hotremove_cb) {
		lun->hotremove_cb(lun, lun->hotremove_ctx);
	}

	spdk_poller_register(&lun->hotplug_poller, spdk_scsi_lun_hotplug, lun, lun->lcore, 0);
}

/**
 * \brief Constructs a new spdk_scsi_lun object based on the provided parameters.
 *
 * \param name Name for the SCSI LUN.
 * \param bdev  bdev associated with this LUN
 *
 * \return NULL if bdev == NULL
 * \return pointer to the new spdk_scsi_lun object otherwise
 */
_spdk_scsi_lun *
spdk_scsi_lun_construct(const char *name, struct spdk_bdev *bdev,
			void (*hotremove_cb)(const struct spdk_scsi_lun *, void *), void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;
	int rc;

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev must be non-NULL\n");
		return NULL;
	}

	lun = spdk_lun_db_get_lun(name);
	if (lun) {
		SPDK_ERRLOG("LUN %s already created\n", lun->name);
		return NULL;
	}

	lun = calloc(1, sizeof(*lun));
	if (lun == NULL) {
		SPDK_ERRLOG("could not allocate lun\n");
		return NULL;
	}

	rc = spdk_bdev_open(bdev, true, spdk_scsi_lun_hot_remove, lun, &lun->bdev_desc);

	if (rc != 0) {
		SPDK_ERRLOG("LUN %s: bdev %s cannot be opened, error=%d\n", name, spdk_bdev_get_name(bdev), rc);
		free(lun);
		return NULL;
	}

	TAILQ_INIT(&lun->tasks);
	TAILQ_INIT(&lun->pending_tasks);

	lun->bdev = bdev;
	snprintf(lun->name, sizeof(lun->name), "%s", name);
	lun->hotremove_cb = hotremove_cb;
	lun->hotremove_ctx = hotremove_ctx;

	rc = spdk_scsi_lun_db_add(lun);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to add LUN %s to DB\n", lun->name);
		spdk_bdev_close(lun->bdev_desc);
		free(lun);
		return NULL;
	}

	return lun;
}

int
spdk_scsi_lun_destruct(struct spdk_scsi_lun *lun)
{
	spdk_bdev_close(lun->bdev_desc);
	spdk_poller_unregister(&lun->hotplug_poller, NULL);
	spdk_scsi_lun_db_delete(lun);

	free(lun);

	return 0;
}

int
spdk_scsi_lun_claim(struct spdk_scsi_lun *lun)
{
	assert(spdk_lun_db_get_lun(lun->name) != NULL);

	if (lun->claimed != false) {
		return -1;
	}

	lun->claimed = true;
	return 0;
}

int
spdk_scsi_lun_unclaim(struct spdk_scsi_lun *lun)
{
	assert(spdk_lun_db_get_lun(lun->name) != NULL);
	assert(lun->claimed == true);
	lun->claimed = false;
	lun->dev = NULL;

	return 0;
}

int
spdk_scsi_lun_delete(const char *lun_name)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_dev *dev;

	pthread_mutex_lock(&g_spdk_scsi.mutex);
	lun = spdk_lun_db_get_lun(lun_name);
	if (lun == NULL) {
		SPDK_ERRLOG("LUN '%s' not found\n", lun_name);
		pthread_mutex_unlock(&g_spdk_scsi.mutex);
		return -1;
	}

	dev = lun->dev;

	/* Remove the LUN from the device */
	if (dev != NULL) {
		spdk_scsi_dev_delete_lun(dev, lun);
	}

	/* Destroy this lun */
	spdk_scsi_lun_destruct(lun);
	pthread_mutex_unlock(&g_spdk_scsi.mutex);
	return 0;
}

int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel != NULL) {
		assert(spdk_io_channel_get_thread(lun->io_channel) == spdk_get_thread());
		return 0;
	}

	lun->lcore = spdk_env_get_current_core();

	lun->io_channel = spdk_bdev_get_io_channel(lun->bdev_desc);
	if (lun->io_channel == NULL) {
		return -1;
	}
	return 0;
}

void spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel != NULL) {
		spdk_put_io_channel(lun->io_channel);
		lun->io_channel = NULL;
	}
}

int
spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun)
{
	return lun->id;
}

const char *
spdk_scsi_lun_get_name(const struct spdk_scsi_lun *lun)
{
	return lun->name;
}

const struct spdk_scsi_dev *
spdk_scsi_lun_get_dev(const struct spdk_scsi_lun *lun)
{
	return lun->dev;
}

bool
spdk_scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun)
{
	return !TAILQ_EMPTY(&lun->pending_tasks) || !TAILQ_EMPTY(&lun->tasks);
}
