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
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/likely.h"

static void scsi_lun_execute_tasks(struct spdk_scsi_lun *lun);
static void _scsi_lun_execute_mgmt_task(struct spdk_scsi_lun *lun);

void
scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (lun) {
		TAILQ_REMOVE(&lun->tasks, task, scsi_link);
		spdk_trace_record(TRACE_SCSI_TASK_DONE, lun->dev->id, 0, (uintptr_t)task);
	}
	task->cpl_fn(task);
}

static void
scsi_lun_complete_mgmt_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	TAILQ_REMOVE(&lun->mgmt_tasks, task, scsi_link);

	task->cpl_fn(task);

	/* Try to execute the first pending mgmt task if it exists. */
	_scsi_lun_execute_mgmt_task(lun);
}

static bool
_scsi_lun_has_pending_mgmt_tasks(const struct spdk_scsi_lun *lun)
{
	return !TAILQ_EMPTY(&lun->pending_mgmt_tasks);
}

static bool
scsi_lun_has_outstanding_mgmt_tasks(const struct spdk_scsi_lun *lun)
{
	return !TAILQ_EMPTY(&lun->mgmt_tasks);
}

static bool
_scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun)
{
	return !TAILQ_EMPTY(&lun->pending_tasks);
}

static bool
scsi_lun_has_outstanding_tasks(const struct spdk_scsi_lun *lun)
{
	return !TAILQ_EMPTY(&lun->tasks);
}

/* Reset task have to wait until all prior outstanding tasks complete. */
static int
scsi_lun_reset_check_outstanding_tasks(void *arg)
{
	struct spdk_scsi_task *task = (struct spdk_scsi_task *)arg;
	struct spdk_scsi_lun *lun = task->lun;

	if (scsi_lun_has_outstanding_tasks(lun)) {
		return SPDK_POLLER_BUSY;
	}
	spdk_poller_unregister(&lun->reset_poller);

	scsi_lun_complete_mgmt_task(lun, task);
	return SPDK_POLLER_BUSY;
}

void
scsi_lun_complete_reset_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (task->status == SPDK_SCSI_STATUS_GOOD) {
		if (scsi_lun_has_outstanding_tasks(lun)) {
			lun->reset_poller =
				SPDK_POLLER_REGISTER(scsi_lun_reset_check_outstanding_tasks,
						     task, 10);
			return;
		}
	}

	scsi_lun_complete_mgmt_task(lun, task);
}

static void
scsi_lun_append_mgmt_task(struct spdk_scsi_lun *lun,
			  struct spdk_scsi_task *task)
{
	TAILQ_INSERT_TAIL(&lun->pending_mgmt_tasks, task, scsi_link);
}

static bool
_scsi_lun_handle_unit_attention(struct spdk_scsi_task *task)
{
	uint8_t *cdb = task->cdb;

	assert(task->cdb);

	switch (cdb[0]) {
	case SPDK_SPC_INQUIRY:
	case SPDK_SPC_REPORT_LUNS:
	case SPDK_SPC_REQUEST_SENSE:
		return false;
	default:
		return true;
	}
}

static void
_scsi_lun_execute_mgmt_task(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_task *task;

	if (!TAILQ_EMPTY(&lun->mgmt_tasks)) {
		return;
	}

	task = TAILQ_FIRST(&lun->pending_mgmt_tasks);
	if (spdk_likely(task == NULL)) {
		/* Try to execute all pending tasks */
		scsi_lun_execute_tasks(lun);
		return;
	}
	TAILQ_REMOVE(&lun->pending_mgmt_tasks, task, scsi_link);

	TAILQ_INSERT_TAIL(&lun->mgmt_tasks, task, scsi_link);

	if (lun->removed) {
		task->response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		scsi_lun_complete_mgmt_task(lun, task);
		return;
	}

	switch (task->function) {
	case SPDK_SCSI_TASK_FUNC_ABORT_TASK:
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_ERRLOG("ABORT_TASK failed\n");
		break;

	case SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET:
		task->response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_ERRLOG("ABORT_TASK_SET failed\n");
		break;

	case SPDK_SCSI_TASK_FUNC_LUN_RESET:
		bdev_scsi_reset(task);
		return;

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

	scsi_lun_complete_mgmt_task(lun, task);
}

void
scsi_lun_execute_mgmt_task(struct spdk_scsi_lun *lun,
			   struct spdk_scsi_task *task)
{
	scsi_lun_append_mgmt_task(lun, task);
	_scsi_lun_execute_mgmt_task(lun);
}

static void
_scsi_lun_execute_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	int rc;

	task->status = SPDK_SCSI_STATUS_GOOD;
	spdk_trace_record(TRACE_SCSI_TASK_START, lun->dev->id, task->length, (uintptr_t)task);
	TAILQ_INSERT_TAIL(&lun->tasks, task, scsi_link);
	if (spdk_unlikely(lun->removed)) {
		spdk_scsi_task_process_abort(task);
		rc = SPDK_SCSI_TASK_COMPLETE;
	} else if (spdk_unlikely(lun->resizing) && _scsi_lun_handle_unit_attention(task)) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_UNIT_ATTENTION,
					  SPDK_SCSI_ASC_CAPACITY_DATA_HAS_CHANGED,
					  SPDK_SCSI_ASCQ_CAPACITY_DATA_HAS_CHANGED);
		lun->resizing = false;
		rc = SPDK_SCSI_TASK_COMPLETE;
	} else {
		/* Check the command is allowed or not when reservation is exist */
		if (spdk_unlikely(lun->reservation.flags & SCSI_SPC2_RESERVE)) {
			rc = scsi2_reserve_check(task);
		} else {
			rc = scsi_pr_check(task);
		}
		if (spdk_unlikely(rc < 0)) {
			/* Reservation Conflict */
			rc = SPDK_SCSI_TASK_COMPLETE;
		} else {
			rc = bdev_scsi_execute(task);
		}
	}

	switch (rc) {
	case SPDK_SCSI_TASK_PENDING:
		break;

	case SPDK_SCSI_TASK_COMPLETE:
		scsi_lun_complete_task(lun, task);
		break;

	default:
		abort();
	}
}

static void
scsi_lun_append_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	TAILQ_INSERT_TAIL(&lun->pending_tasks, task, scsi_link);
}

static void
scsi_lun_execute_tasks(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_task *task, *task_tmp;

	TAILQ_FOREACH_SAFE(task, &lun->pending_tasks, scsi_link, task_tmp) {
		TAILQ_REMOVE(&lun->pending_tasks, task, scsi_link);
		_scsi_lun_execute_task(lun, task);
	}
}

void
scsi_lun_execute_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task)
{
	if (spdk_unlikely(_scsi_lun_has_pending_mgmt_tasks(lun))) {
		/* Add the IO task to pending list and wait for completion of
		 * existing mgmt tasks.
		 */
		scsi_lun_append_task(lun, task);
	} else if (spdk_unlikely(_scsi_lun_has_pending_tasks(lun))) {
		/* If there is any pending IO task, append the IO task to the
		 * tail of the pending list, and then execute all pending IO tasks
		 * from the head to submit IO tasks in order.
		 */
		scsi_lun_append_task(lun, task);
		scsi_lun_execute_tasks(lun);
	} else {
		/* Execute the IO task directly. */
		_scsi_lun_execute_task(lun, task);
	}
}

static void
_scsi_lun_remove(void *arg)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)arg;

	spdk_bdev_close(lun->bdev_desc);
	spdk_scsi_dev_delete_lun(lun->dev, lun);
	free(lun);
}

static void
scsi_lun_remove(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_pr_registrant *reg, *tmp;
	struct spdk_thread *thread;

	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		TAILQ_REMOVE(&lun->reg_head, reg, link);
		free(reg);
	}

	thread = spdk_get_thread();
	if (thread != lun->thread) {
		spdk_thread_send_msg(lun->thread, _scsi_lun_remove, lun);
	} else {
		_scsi_lun_remove(lun);
	}
}

static int
scsi_lun_check_io_channel(void *arg)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)arg;

	if (lun->io_channel) {
		return SPDK_POLLER_BUSY;
	}
	spdk_poller_unregister(&lun->hotremove_poller);

	scsi_lun_remove(lun);
	return SPDK_POLLER_BUSY;
}

static void
scsi_lun_notify_hot_remove(struct spdk_scsi_lun *lun)
{
	struct spdk_scsi_lun_desc *desc, *tmp;

	if (lun->hotremove_cb) {
		lun->hotremove_cb(lun, lun->hotremove_ctx);
	}

	TAILQ_FOREACH_SAFE(desc, &lun->open_descs, link, tmp) {
		if (desc->hotremove_cb) {
			desc->hotremove_cb(lun, desc->hotremove_ctx);
		} else {
			spdk_scsi_lun_close(desc);
		}
	}

	if (lun->io_channel) {
		lun->hotremove_poller = SPDK_POLLER_REGISTER(scsi_lun_check_io_channel,
					lun, 10);
	} else {
		scsi_lun_remove(lun);
	}
}

static int
scsi_lun_check_outstanding_tasks(void *arg)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)arg;

	if (scsi_lun_has_outstanding_tasks(lun) ||
	    scsi_lun_has_outstanding_mgmt_tasks(lun)) {
		return SPDK_POLLER_BUSY;
	}
	spdk_poller_unregister(&lun->hotremove_poller);

	scsi_lun_notify_hot_remove(lun);
	return SPDK_POLLER_BUSY;
}

static void
_scsi_lun_hot_remove(void *arg1)
{
	struct spdk_scsi_lun *lun = arg1;

	/* If lun->removed is set, no new task can be submitted to the LUN.
	 * Execute previously queued tasks, which will be immediately aborted.
	 */
	scsi_lun_execute_tasks(lun);

	/* Then we only need to wait for all outstanding tasks to be completed
	 * before notifying the upper layer about the removal.
	 */
	if (scsi_lun_has_outstanding_tasks(lun) ||
	    scsi_lun_has_outstanding_mgmt_tasks(lun)) {
		lun->hotremove_poller = SPDK_POLLER_REGISTER(scsi_lun_check_outstanding_tasks,
					lun, 10);
	} else {
		scsi_lun_notify_hot_remove(lun);
	}
}

static void
scsi_lun_hot_remove(void *remove_ctx)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)remove_ctx;
	struct spdk_thread *thread;

	if (lun->removed) {
		return;
	}

	lun->removed = true;
	if (lun->io_channel == NULL) {
		_scsi_lun_hot_remove(lun);
		return;
	}

	thread = spdk_io_channel_get_thread(lun->io_channel);
	if (thread != spdk_get_thread()) {
		spdk_thread_send_msg(thread, _scsi_lun_hot_remove, lun);
	} else {
		_scsi_lun_hot_remove(lun);
	}
}

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	      void *event_ctx)
{
	struct spdk_scsi_lun *lun = (struct spdk_scsi_lun *)event_ctx;
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_REMOVE)\n", spdk_bdev_get_name(bdev));
		scsi_lun_hot_remove(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_RESIZE)\n", spdk_bdev_get_name(bdev));
		lun->resizing = true;
		if (lun->resize_cb) {
			lun->resize_cb(lun, lun->resize_ctx);
		}
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/**
 * \brief Constructs a new spdk_scsi_lun object based on the provided parameters.
 *
 * \param bdev_name Bdev name to open and associate with this LUN
 *
 * \return NULL if bdev whose name matches is not found
 * \return pointer to the new spdk_scsi_lun object otherwise
 */
struct spdk_scsi_lun *scsi_lun_construct(const char *bdev_name,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;
	int rc;

	if (bdev_name == NULL) {
		SPDK_ERRLOG("bdev_name must be non-NULL\n");
		return NULL;
	}

	lun = calloc(1, sizeof(*lun));
	if (lun == NULL) {
		SPDK_ERRLOG("could not allocate lun\n");
		return NULL;
	}

	rc = spdk_bdev_open_ext(bdev_name, true, bdev_event_cb, lun, &lun->bdev_desc);

	if (rc != 0) {
		SPDK_ERRLOG("bdev %s cannot be opened, error=%d\n", bdev_name, rc);
		free(lun);
		return NULL;
	}

	lun->thread = spdk_get_thread();

	TAILQ_INIT(&lun->tasks);
	TAILQ_INIT(&lun->pending_tasks);
	TAILQ_INIT(&lun->mgmt_tasks);
	TAILQ_INIT(&lun->pending_mgmt_tasks);

	/* Bdev is not removed while it is opened. */
	lun->bdev = spdk_bdev_desc_get_bdev(lun->bdev_desc);
	lun->io_channel = NULL;
	lun->hotremove_cb = hotremove_cb;
	lun->hotremove_ctx = hotremove_ctx;

	lun->resize_cb = resize_cb;
	lun->resize_ctx = resize_ctx;
	lun->resizing = false;

	TAILQ_INIT(&lun->open_descs);
	TAILQ_INIT(&lun->reg_head);

	return lun;
}

void
scsi_lun_destruct(struct spdk_scsi_lun *lun)
{
	scsi_lun_hot_remove(lun);
}

int
spdk_scsi_lun_open(struct spdk_scsi_lun *lun, spdk_scsi_lun_remove_cb_t hotremove_cb,
		   void *hotremove_ctx, struct spdk_scsi_lun_desc **_desc)
{
	struct spdk_scsi_lun_desc *desc;

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL) {
		SPDK_ERRLOG("calloc() failed for LUN descriptor.\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&lun->open_descs, desc, link);

	desc->lun = lun;
	desc->hotremove_cb = hotremove_cb;
	desc->hotremove_ctx = hotremove_ctx;
	*_desc = desc;

	return 0;
}

void
spdk_scsi_lun_close(struct spdk_scsi_lun_desc *desc)
{
	struct spdk_scsi_lun *lun = desc->lun;

	TAILQ_REMOVE(&lun->open_descs, desc, link);
	free(desc);

	assert(!TAILQ_EMPTY(&lun->open_descs) || lun->io_channel == NULL);
}

int
scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel != NULL) {
		if (spdk_get_thread() == spdk_io_channel_get_thread(lun->io_channel)) {
			lun->ref++;
			return 0;
		}
		SPDK_ERRLOG("io_channel already allocated for lun %s\n",
			    spdk_bdev_get_name(lun->bdev));
		return -1;
	}

	lun->io_channel = spdk_bdev_get_io_channel(lun->bdev_desc);
	if (lun->io_channel == NULL) {
		return -1;
	}
	lun->ref = 1;
	return 0;
}

void
scsi_lun_free_io_channel(struct spdk_scsi_lun *lun)
{
	if (lun->io_channel == NULL) {
		return;
	}

	if (spdk_get_thread() != spdk_io_channel_get_thread(lun->io_channel)) {
		SPDK_ERRLOG("io_channel was freed by different thread\n");
		return;
	}

	lun->ref--;
	if (lun->ref == 0) {
		spdk_put_io_channel(lun->io_channel);
		lun->io_channel = NULL;
	}
}

int
spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_lun_desc *desc)
{
	struct spdk_scsi_lun *lun = desc->lun;

	return scsi_lun_allocate_io_channel(lun);
}

void
spdk_scsi_lun_free_io_channel(struct spdk_scsi_lun_desc *desc)
{
	struct spdk_scsi_lun *lun = desc->lun;

	scsi_lun_free_io_channel(lun);
}

int
spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun)
{
	return lun->id;
}

const char *
spdk_scsi_lun_get_bdev_name(const struct spdk_scsi_lun *lun)
{
	return spdk_bdev_get_name(lun->bdev);
}

const struct spdk_scsi_dev *
spdk_scsi_lun_get_dev(const struct spdk_scsi_lun *lun)
{
	return lun->dev;
}

bool
scsi_lun_has_pending_mgmt_tasks(const struct spdk_scsi_lun *lun,
				const struct spdk_scsi_port *initiator_port)
{
	struct spdk_scsi_task *task;

	if (initiator_port == NULL) {
		return _scsi_lun_has_pending_mgmt_tasks(lun) ||
		       scsi_lun_has_outstanding_mgmt_tasks(lun);
	}

	TAILQ_FOREACH(task, &lun->pending_mgmt_tasks, scsi_link) {
		if (task->initiator_port == initiator_port) {
			return true;
		}
	}

	TAILQ_FOREACH(task, &lun->mgmt_tasks, scsi_link) {
		if (task->initiator_port == initiator_port) {
			return true;
		}
	}

	return false;
}
/* This check includes both pending and submitted (outstanding) tasks. */
bool
scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun,
			   const struct spdk_scsi_port *initiator_port)
{
	struct spdk_scsi_task *task;

	if (initiator_port == NULL) {
		return _scsi_lun_has_pending_tasks(lun) ||
		       scsi_lun_has_outstanding_tasks(lun);
	}

	TAILQ_FOREACH(task, &lun->pending_tasks, scsi_link) {
		if (task->initiator_port == initiator_port) {
			return true;
		}
	}

	TAILQ_FOREACH(task, &lun->tasks, scsi_link) {
		if (task->initiator_port == initiator_port) {
			return true;
		}
	}

	return false;
}

bool
spdk_scsi_lun_is_removing(const struct spdk_scsi_lun *lun)
{
	return lun->removed;
}

bool
spdk_scsi_lun_get_dif_ctx(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task,
			  struct spdk_dif_ctx *dif_ctx)
{
	return bdev_scsi_get_dif_ctx(lun->bdev, task, dif_ctx);
}
