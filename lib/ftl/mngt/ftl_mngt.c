/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/queue.h"
#include "spdk/assert.h"
#include "spdk/env.h"

#include "ftl_mngt.h"
#include "ftl_core.h"

struct ftl_mngt_step_status {
	uint64_t start;
	uint64_t stop;
	int status;
	int silent;
	TAILQ_ENTRY(ftl_mngt_step) entry;
};

struct ftl_mngt_step {
	void *ctx;
	const struct ftl_mngt_step_desc *desc;
	struct ftl_mngt_step_status action;
	struct ftl_mngt_step_status rollback;
};

struct ftl_mngt_process {
	struct spdk_ftl_dev *dev;
	int status;
	bool silent;
	bool rollback;
	bool continuing;
	struct  {
		ftl_mngt_completion cb;
		void *cb_ctx;
		struct spdk_thread *thread;
	} caller;
	void *ctx;
	uint64_t tsc_start;
	uint64_t tsc_stop;
	const struct ftl_mngt_process_desc *desc;
	TAILQ_HEAD(, ftl_mngt_step) action_queue_todo;
	TAILQ_HEAD(, ftl_mngt_step) action_queue_done;
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_todo;
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_done;
	struct {
		struct ftl_mngt_step step;
		struct ftl_mngt_step_desc desc;
	} cleanup;
	struct ftl_mng_tracer *tracer;
};

static void action_next(struct ftl_mngt_process *mngt);
static void action_msg(void *ctx);
static void action_execute(struct ftl_mngt_process *mngt);
static void action_done(struct ftl_mngt_process *mngt, int status);
static void rollback_next(struct ftl_mngt_process *mngt);
static void rollback_msg(void *ctx);
static void rollback_execute(struct ftl_mngt_process *mngt);
static void rollback_done(struct ftl_mngt_process *mngt, int status);

static inline struct ftl_mngt_step *
get_current_step(struct ftl_mngt_process *mngt)
{
	if (!mngt->rollback) {
		return TAILQ_FIRST(&mngt->action_queue_todo);
	} else {
		return TAILQ_FIRST(&mngt->rollback_queue_todo);
	}
}

static int
init_step(struct ftl_mngt_process *mngt,
	  const struct ftl_mngt_step_desc *desc)
{
	struct ftl_mngt_step *step;

	step = calloc(1, sizeof(*step));
	if (!step) {
		return -ENOMEM;
	}

	/* Initialize the step's argument */
	if (desc->ctx_size) {
		step->ctx = calloc(1, desc->ctx_size);
		if (!step->ctx) {
			free(step);
			return -ENOMEM;
		}
	}
	step->desc = desc;
	TAILQ_INSERT_TAIL(&mngt->action_queue_todo, step, action.entry);

	return 0;
}

static void
free_mngt(struct ftl_mngt_process *mngt)
{
	TAILQ_HEAD(, ftl_mngt_step) steps;

	if (!mngt) {
		return;
	}

	TAILQ_INIT(&steps);
	TAILQ_CONCAT(&steps, &mngt->action_queue_todo, action.entry);
	TAILQ_CONCAT(&steps, &mngt->action_queue_done, action.entry);

	while (!TAILQ_EMPTY(&steps)) {
		struct ftl_mngt_step *step = TAILQ_FIRST(&steps);
		TAILQ_REMOVE(&steps, step, action.entry);

		free(step->ctx);
		free(step);
	}

	free(mngt->ctx);
	free(mngt);
}

static struct ftl_mngt_process *
allocate_mngt(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
	      ftl_mngt_completion cb, void *cb_ctx, bool silent)
{
	struct ftl_mngt_process *mngt;

	/* Initialize management process */
	mngt = calloc(1, sizeof(*mngt));
	if (!mngt) {
		goto error;
	}
	mngt->dev = dev;
	mngt->silent = silent;
	mngt->caller.cb = cb;
	mngt->caller.cb_ctx = cb_ctx;
	mngt->caller.thread = spdk_get_thread();

	/* Initialize process context */
	if (pdesc->ctx_size) {
		mngt->ctx = calloc(1, pdesc->ctx_size);
		if (!mngt->ctx) {
			goto error;
		}
	}
	mngt->tsc_start = spdk_get_ticks();
	mngt->desc = pdesc;
	TAILQ_INIT(&mngt->action_queue_todo);
	TAILQ_INIT(&mngt->action_queue_done);
	TAILQ_INIT(&mngt->rollback_queue_todo);
	TAILQ_INIT(&mngt->rollback_queue_done);

	return mngt;
error:
	free_mngt(mngt);
	return NULL;
}

static int
_ftl_mngt_process_execute(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			  ftl_mngt_completion cb, void *cb_ctx, bool silent)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt_process *mngt;
	int rc = 0;

	mngt = allocate_mngt(dev, pdesc, cb, cb_ctx, silent);
	if (!mngt) {
		rc = -ENOMEM;
		goto error;
	}

	if (pdesc->error_handler) {
		/* Initialize a step for error handler */
		mngt->cleanup.step.desc = &mngt->cleanup.desc;
		mngt->cleanup.desc.name = "Handle ERROR";
		mngt->cleanup.desc.cleanup = pdesc->error_handler;

		/* Queue error handler to the rollback queue, it will be executed at the end */
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, &mngt->cleanup.step,
				  rollback.entry);
	}

	/* Initialize steps */
	sdesc = mngt->desc->steps;
	while (sdesc->action) {
		rc = init_step(mngt, sdesc);
		if (rc) {
			goto error;
		}
		sdesc++;
	}

	action_execute(mngt);
	return 0;
error:
	free_mngt(mngt);
	return rc;
}

int
ftl_mngt_process_execute(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			 ftl_mngt_completion cb, void *cb_ctx)
{
	return _ftl_mngt_process_execute(dev, pdesc, cb, cb_ctx, false);
}

int
ftl_mngt_process_rollback(struct spdk_ftl_dev *dev, const struct ftl_mngt_process_desc *pdesc,
			  ftl_mngt_completion cb, void *cb_ctx)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt_process *mngt;
	int rc = 0;

	mngt = allocate_mngt(dev, pdesc, cb, cb_ctx, true);
	if (!mngt) {
		rc = -ENOMEM;
		goto error;
	}

	/* Initialize steps for rollback */
	sdesc = mngt->desc->steps;
	while (sdesc->action) {
		if (!sdesc->cleanup) {
			sdesc++;
			continue;
		}
		rc = init_step(mngt, sdesc);
		if (rc) {
			goto error;
		}
		sdesc++;
	}

	/* Build rollback list */
	struct ftl_mngt_step *step;
	TAILQ_FOREACH(step, &mngt->action_queue_todo, action.entry) {
		step->action.silent = true;
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, step,
				  rollback.entry);
	}

	mngt->rollback = true;
	rollback_execute(mngt);
	return 0;
error:
	free_mngt(mngt);
	return rc;
}

struct spdk_ftl_dev *
ftl_mngt_get_dev(struct ftl_mngt_process *mngt)
{
	return mngt->dev;
}

int
ftl_mngt_alloc_step_ctx(struct ftl_mngt_process *mngt, size_t size)
{
	struct ftl_mngt_step *step = get_current_step(mngt);
	void *arg = calloc(1, size);

	if (!arg) {
		return -ENOMEM;
	}

	free(step->ctx);
	step->ctx = arg;

	return 0;
}

void *
ftl_mngt_get_step_ctx(struct ftl_mngt_process *mngt)
{
	return get_current_step(mngt)->ctx;
}

void *
ftl_mngt_get_process_ctx(struct ftl_mngt_process *mngt)
{
	return mngt->ctx;
}

void *
ftl_mngt_get_caller_ctx(struct ftl_mngt_process *mngt)
{
	return mngt->caller.cb_ctx;
}

void
ftl_mngt_next_step(struct ftl_mngt_process *mngt)
{
	if (false == mngt->rollback) {
		action_next(mngt);
	} else {
		rollback_next(mngt);
	}
}

void
ftl_mngt_skip_step(struct ftl_mngt_process *mngt)
{
	if (mngt->rollback) {
		get_current_step(mngt)->rollback.silent = true;
	} else {
		get_current_step(mngt)->action.silent = true;
	}
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_continue_step(struct ftl_mngt_process *mngt)
{

	if (!mngt->continuing) {
		if (false == mngt->rollback) {
			action_execute(mngt);
		} else {
			rollback_execute(mngt);
		}
	}

	mngt->continuing = true;
}

static void
child_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_mngt_process *parent = ctx;

	if (status) {
		ftl_mngt_fail_step(parent);
	} else {
		ftl_mngt_next_step(parent);
	}
}

void
ftl_mngt_call_process(struct ftl_mngt_process *mngt,
		      const struct ftl_mngt_process_desc *pdesc)
{
	if (_ftl_mngt_process_execute(mngt->dev, pdesc, child_cb, mngt, true)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

void
ftl_mngt_call_process_rollback(struct ftl_mngt_process *mngt,
			       const struct ftl_mngt_process_desc *pdesc)
{
	if (ftl_mngt_process_rollback(mngt->dev, pdesc, child_cb, mngt)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

void
ftl_mngt_fail_step(struct ftl_mngt_process *mngt)
{
	mngt->status = -1;

	if (false == mngt->rollback) {
		action_done(mngt, -1);
	} else {
		rollback_done(mngt, -1);
	}

	mngt->rollback = true;
	rollback_execute(mngt);
}

static inline float
tsc_to_ms(uint64_t tsc)
{
	float ms = tsc;
	ms /= (float)spdk_get_ticks_hz();
	ms *= 1000.0;
	return ms;
}

static void
trace_step(struct spdk_ftl_dev *dev, struct ftl_mngt_step *step, bool rollback)
{
	uint64_t duration;
	const char *what = rollback ? "Rollback" : "Action";
	int silent = rollback ? step->rollback.silent : step->action.silent;

	if (silent) {
		return;
	}

	FTL_NOTICELOG(dev, "%s\n", what);
	FTL_NOTICELOG(dev, "\t name:     %s\n", step->desc->name);
	duration = step->action.stop - step->action.start;
	FTL_NOTICELOG(dev, "\t duration: %.3f ms\n", tsc_to_ms(duration));
	FTL_NOTICELOG(dev, "\t status:   %d\n", step->action.status);
}

static void
finish_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	char *devname = NULL;

	if (!mngt->silent && mngt->dev->conf.name) {
		/* the callback below can free the device so make a temp copy of the name */
		devname = strdup(mngt->dev->conf.name);
	}

	mngt->caller.cb(mngt->dev, mngt->caller.cb_ctx, mngt->status);

	if (!mngt->silent) {
		/* TODO: refactor the logging macros to pass just the name instead of device */
		struct spdk_ftl_dev tmpdev = {
			.conf = {
				.name = devname
			}
		};

		FTL_NOTICELOG(&tmpdev, "Management process finished, name '%s', duration = %.3f ms, result %d\n",
			      mngt->desc->name,
			      tsc_to_ms(mngt->tsc_stop - mngt->tsc_start),
			      mngt->status);
	}
	free_mngt(mngt);
	free(devname);
}

void
ftl_mngt_finish(struct ftl_mngt_process *mngt)
{
	mngt->tsc_stop = spdk_get_ticks();
	spdk_thread_send_msg(mngt->caller.thread, finish_msg, mngt);
}

/*
 * Actions
 */
static void
action_next(struct ftl_mngt_process *mngt)
{
	if (TAILQ_EMPTY(&mngt->action_queue_todo)) {
		/* Nothing to do, finish the management process */
		ftl_mngt_finish(mngt);
		return;
	} else {
		action_done(mngt, 0);
		action_execute(mngt);
	}
}

static void
action_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&mngt->action_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&mngt->action_queue_todo);
	if (!step->action.start) {
		step->action.start = spdk_get_ticks();
	}
	step->desc->action(mngt->dev, mngt);
}

static void
action_execute(struct ftl_mngt_process *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, action_msg, mngt);
}

static void
action_done(struct ftl_mngt_process *mngt, int status)
{
	struct ftl_mngt_step *step;

	assert(!TAILQ_EMPTY(&mngt->action_queue_todo));
	step = TAILQ_FIRST(&mngt->action_queue_todo);
	TAILQ_REMOVE(&mngt->action_queue_todo, step, action.entry);

	TAILQ_INSERT_TAIL(&mngt->action_queue_done, step, action.entry);
	if (step->desc->cleanup) {
		TAILQ_INSERT_HEAD(&mngt->rollback_queue_todo, step,
				  rollback.entry);
	}

	step->action.stop = spdk_get_ticks();
	step->action.status = status;

	trace_step(mngt->dev, step, false);
}

/*
 * Rollback
 */
static void
rollback_next(struct ftl_mngt_process *mngt)
{
	if (TAILQ_EMPTY(&mngt->rollback_queue_todo)) {
		/* Nothing to do, finish the management process */
		ftl_mngt_finish(mngt);
		return;
	} else {
		rollback_done(mngt, 0);
		rollback_execute(mngt);
	}
}

static void
rollback_msg(void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&mngt->rollback_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&mngt->rollback_queue_todo);
	if (!step->rollback.start) {
		step->rollback.start = spdk_get_ticks();
	}
	step->desc->cleanup(mngt->dev, mngt);
}

static void
rollback_execute(struct ftl_mngt_process *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, rollback_msg, mngt);
}

void
rollback_done(struct ftl_mngt_process *mngt, int status)
{
	struct ftl_mngt_step *step;

	assert(!TAILQ_EMPTY(&mngt->rollback_queue_todo));
	step = TAILQ_FIRST(&mngt->rollback_queue_todo);
	TAILQ_REMOVE(&mngt->rollback_queue_todo, step, rollback.entry);
	TAILQ_INSERT_TAIL(&mngt->rollback_queue_done, step, rollback.entry);

	step->rollback.stop = spdk_get_ticks();
	step->rollback.status = status;

	trace_step(mngt->dev, step,  true);
}
