/*-
 *   BSD LICENSE
 *
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
	void *cntx;
	const struct ftl_mngt_step_desc *desc;
	struct ftl_mngt_step_status action;
	struct ftl_mngt_step_status rollback;
};

struct ftl_mngt_process {
	void *cntx;
	uint64_t tcs_start;
	uint64_t tcs_stop;
	const struct ftl_mngt_process_desc *desc;
	TAILQ_HEAD(, ftl_mngt_step) action_queue_todo;
	TAILQ_HEAD(, ftl_mngt_step) action_queue_done;
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_todo;
	TAILQ_HEAD(, ftl_mngt_step) rollback_queue_done;
	struct {
		struct ftl_mngt_step step;
		struct ftl_mngt_step_desc desc;
	} cleanup;
	int status;
};

struct ftl_mngt {
	struct spdk_ftl_dev *dev;
	int status;
	int silent;
	bool rollback;
	bool continuing;
	struct  {
		ftl_mngt_fn cb;
		void *cb_cntx;
		struct spdk_thread *thread;
	} caller;
	struct ftl_mngt_process process;
	struct ftl_mng_tracer *tracer;
};

static void action_next(struct ftl_mngt *mngt);
static void action_msg(void *cntx);
static void action_execute(struct ftl_mngt *mngt);
static void action_done(struct ftl_mngt *mngt, int status);
static void rollback_next(struct ftl_mngt *mngt);
static void rollback_msg(void *cntx);
static void rollback_execute(struct ftl_mngt *mngt);
static void rollback_done(struct ftl_mngt *mngt, int status);

static inline struct ftl_mngt_step *get_current_step(struct ftl_mngt *mngt)
{
	if (false == mngt->rollback) {
		return TAILQ_FIRST(&mngt->process.action_queue_todo);
	} else {
		return TAILQ_FIRST(&mngt->process.rollback_queue_todo);
	}
}

static int init_step(struct ftl_mngt_process *p,
		     const struct ftl_mngt_step_desc *desc)
{
	struct ftl_mngt_step *step;

	step = calloc(1, sizeof(*step));
	if (!step) {
		return -ENOMEM;
	}

	/* Initialize the step's argument */
	if (desc->arg_size) {
		step->cntx = calloc(1, desc->arg_size);
		if (!step->cntx) {
			free(step);
			return  -ENOMEM;
		}
	}
	step->desc = desc;
	TAILQ_INSERT_TAIL(&p->action_queue_todo, step, action.entry);

	return 0;
}

static void free_mngt(struct ftl_mngt *mngt)
{
	TAILQ_HEAD(, ftl_mngt_step) steps;

	if (!mngt) {
		return;
	}

	TAILQ_INIT(&steps);
	TAILQ_CONCAT(&steps, &mngt->process.action_queue_todo, action.entry);
	TAILQ_CONCAT(&steps, &mngt->process.action_queue_done, action.entry);

	while (!TAILQ_EMPTY(&steps)) {
		struct ftl_mngt_step *step = TAILQ_FIRST(&steps);
		TAILQ_REMOVE(&steps, step, action.entry);

		free(step->cntx);
		free(step);
	}

	free(mngt->process.cntx);
	free(mngt);
}

static struct ftl_mngt *allocate_mngt(struct spdk_ftl_dev *dev,
				      const struct ftl_mngt_process_desc *pdesc,
				      ftl_mngt_fn cb, void *cb_cntx)
{
	struct ftl_mngt *mngt;

	/* Initialize management process */
	mngt = calloc(1, sizeof(*mngt));
	if (!mngt) {
		goto error;
	}
	mngt->dev = dev;
	mngt->caller.cb = cb;
	mngt->caller.cb_cntx = cb_cntx;
	mngt->caller.thread = spdk_get_thread();

	/* Initialize process context */
	struct ftl_mngt_process *process = &mngt->process;
	if (pdesc->arg_size) {
		process->cntx = calloc(1, pdesc->arg_size);
		if (!process->cntx) {
			goto error;
		}
	}
	process->tcs_start = spdk_get_ticks();
	process->desc = pdesc;
	TAILQ_INIT(&process->action_queue_todo);
	TAILQ_INIT(&process->action_queue_done);
	TAILQ_INIT(&process->rollback_queue_todo);
	TAILQ_INIT(&process->rollback_queue_done);

	return mngt;
error:
	free_mngt(mngt);
	return NULL;
}

int ftl_mngt_execute(struct spdk_ftl_dev *dev,
		     const struct ftl_mngt_process_desc *pdesc,
		     ftl_mngt_fn cb, void *cb_cntx)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt *mngt;

	mngt = allocate_mngt(dev, pdesc, cb, cb_cntx);
	if (!mngt) {
		goto error;
	}

	struct ftl_mngt_process *proc = &mngt->process;

	if (pdesc->error_handler) {
		/* Initialize a step for error handler */
		proc->cleanup.step.desc = &proc->cleanup.desc;
		proc->cleanup.desc.name = "Handle ERROR";
		proc->cleanup.desc.cleanup = pdesc->error_handler;

		/* Queue error handler to the rollback queue, it will be executed at the end */
		TAILQ_INSERT_HEAD(&proc->rollback_queue_todo, &proc->cleanup.step,
				  rollback.entry);
	}

	/* Initialize steps */
	sdesc = proc->desc->steps;
	while (sdesc->action) {
		if (init_step(proc, sdesc)) {
			goto error;
		}
		sdesc++;
	}

	action_execute(mngt);
	return 0;
error:
	free_mngt(mngt);
	return -ENOMEM;
}

int ftl_mngt_rollback(struct spdk_ftl_dev *dev,
		      const struct ftl_mngt_process_desc *pdesc,
		      ftl_mngt_fn cb, void *cb_cntx)
{
	const struct ftl_mngt_step_desc *sdesc;
	struct ftl_mngt *mngt;

	mngt = allocate_mngt(dev, pdesc, cb, cb_cntx);
	if (!mngt) {
		goto error;
	}

	struct ftl_mngt_process *proc = &mngt->process;

	/* Initialize steps for rollback */
	sdesc = proc->desc->steps;
	while (sdesc->action) {
		if (!sdesc->cleanup) {
			sdesc++;
			continue;
		}

		if (init_step(proc, sdesc)) {
			goto error;
		}
		sdesc++;
	}

	/* Build rollback list */
	struct ftl_mngt_step *step;
	TAILQ_FOREACH(step, &proc->action_queue_todo, action.entry) {
		step->action.silent = true;
		TAILQ_INSERT_HEAD(&proc->rollback_queue_todo, step,
				  rollback.entry);
	}

	mngt->rollback = true;
	rollback_execute(mngt);
	return 0;
error:
	free_mngt(mngt);
	return -ENOMEM;
}

struct spdk_ftl_dev *ftl_mngt_get_dev(struct ftl_mngt *mngt)
{
	return mngt->dev;
}

void ftl_mngt_clear_dev(struct ftl_mngt *mngt)
{
	mngt->dev = NULL;
}

int ftl_mngt_alloc_step_cntx(struct ftl_mngt *mngt, size_t size)
{
	struct ftl_mngt_step *step = get_current_step(mngt);
	void *arg = calloc(1, size);

	if (!arg) {
		return -ENOMEM;
	}

	free(step->cntx);
	step->cntx = arg;

	return 0;
}

void *ftl_mngt_get_step_cntx(struct ftl_mngt *mngt)
{
	return get_current_step(mngt)->cntx;
}

void *ftl_mngt_get_process_cntx(struct ftl_mngt *mngt)
{
	return mngt->process.cntx;
}

void *ftl_mngt_get_caller_context(struct ftl_mngt *mngt)
{
	return mngt->caller.cb_cntx;
}

int ftl_mngt_get_status(struct ftl_mngt *mngt)
{
	return mngt->status;
}

void ftl_mngt_next_step(struct ftl_mngt *mngt)
{
	if (false == mngt->rollback) {
		action_next(mngt);
	} else {
		rollback_next(mngt);
	}
}

void ftl_mngt_skip_step(struct ftl_mngt *mngt)
{
	if (mngt->rollback) {
		get_current_step(mngt)->rollback.silent = true;
	} else {
		get_current_step(mngt)->action.silent = true;
	}
	ftl_mngt_next_step(mngt);
}

void ftl_mngt_continue_step(struct ftl_mngt *mngt)
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

static void child_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *child)
{
	int status = ftl_mngt_get_status(child);
	struct ftl_mngt *parent = ftl_mngt_get_caller_context(child);

	child->silent = true;

	if (status) {
		ftl_mngt_fail_step(parent);
	} else {
		ftl_mngt_next_step(parent);
	}
}

void ftl_mngt_call(struct ftl_mngt *mngt,
		   const struct ftl_mngt_process_desc *pdesc)
{
	if (ftl_mngt_execute(mngt->dev, pdesc, child_cb, mngt)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

void ftl_mngt_call_rollback(struct ftl_mngt *mngt,
			    const struct ftl_mngt_process_desc *pdesc)
{
	if (ftl_mngt_rollback(mngt->dev, pdesc, child_cb, mngt)) {
		ftl_mngt_fail_step(mngt);
	} else {
		if (mngt->rollback) {
			get_current_step(mngt)->rollback.silent = true;
		} else {
			get_current_step(mngt)->action.silent = true;
		}
	}
}

void ftl_mngt_fail_step(struct ftl_mngt *mngt)
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

static inline float tcs_to_ms(uint64_t tcs)
{
	float ms = tcs;
	ms /= (float)spdk_get_ticks_hz();
	ms *= 1000.0;
	return ms;
}

static void trace_step(struct spdk_ftl_dev *dev, struct ftl_mngt_step *step, bool rollback)
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
	FTL_NOTICELOG(dev, "\t duration: %.3f ms\n", tcs_to_ms(duration));
	FTL_NOTICELOG(dev, "\t status:   %d\n", step->action.status);
}

static void process_summary(struct ftl_mngt *mngt)
{
	uint64_t duration;

	if (mngt->silent) {
		return;
	}

	duration = mngt->process.tcs_stop - mngt->process.tcs_start;
	FTL_NOTICELOG(mngt->dev, "Management process finished, "
		      "name '%s', duration = %.3f ms, result %d\n",
		      mngt->process.desc->name,
		      tcs_to_ms(duration),
		      mngt->status);
}

static void finish_msg(void *cntx)
{
	struct ftl_mngt *mngt = cntx;

	mngt->caller.cb(mngt->dev, mngt);
	process_summary(mngt);
	free_mngt(mngt);
}

void ftl_mngt_finish(struct ftl_mngt *mngt)
{
	mngt->process.tcs_stop = spdk_get_ticks();
	spdk_thread_send_msg(mngt->caller.thread, finish_msg, mngt);
}

/*
 * Actions
 */
static void action_next(struct ftl_mngt *mngt)
{
	if (TAILQ_EMPTY(&mngt->process.action_queue_todo)) {
		/* Nothing to do, finish the management process */
		ftl_mngt_finish(mngt);
		return;
	} else {
		action_done(mngt, 0);
		action_execute(mngt);
	}
}

static void action_msg(void *cntx)
{
	struct ftl_mngt *mngt = cntx;
	struct ftl_mngt_process *process = &mngt->process;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&process->action_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&process->action_queue_todo);
	if (!step->action.start) {
		step->action.start = spdk_get_ticks();
	}
	step->desc->action(mngt->dev, mngt);
}

static void action_execute(struct ftl_mngt *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, action_msg, mngt);
}

static void action_done(struct ftl_mngt *mngt, int status)
{
	struct ftl_mngt_step *step;
	struct ftl_mngt_process *process = &mngt->process;

	assert(!TAILQ_EMPTY(&process->action_queue_todo));
	step = TAILQ_FIRST(&process->action_queue_todo);
	TAILQ_REMOVE(&process->action_queue_todo, step, action.entry);

	TAILQ_INSERT_TAIL(&process->action_queue_done, step, action.entry);
	if (step->desc->cleanup) {
		TAILQ_INSERT_HEAD(&process->rollback_queue_todo, step,
				  rollback.entry);
	}

	step->action.stop = spdk_get_ticks();
	step->action.status = status;

	trace_step(mngt->dev, step, false);
}

/*
 * Rollback
 */
static void rollback_next(struct ftl_mngt *mngt)
{
	if (TAILQ_EMPTY(&mngt->process.rollback_queue_todo)) {
		/* Nothing to do, finish the management process */
		ftl_mngt_finish(mngt);
		return;
	} else {
		rollback_done(mngt, 0);
		rollback_execute(mngt);
	}
}

static void rollback_msg(void *cntx)
{
	struct ftl_mngt *mngt = cntx;
	struct ftl_mngt_process *process = &mngt->process;
	struct ftl_mngt_step *step;

	mngt->continuing = false;

	if (TAILQ_EMPTY(&process->rollback_queue_todo)) {
		ftl_mngt_finish(mngt);
		return;
	}

	step = TAILQ_FIRST(&process->rollback_queue_todo);
	if (!step->rollback.start) {
		step->rollback.start = spdk_get_ticks();
	}
	step->desc->cleanup(mngt->dev, mngt);
}

static void rollback_execute(struct ftl_mngt *mngt)
{
	spdk_thread_send_msg(mngt->dev->core_thread, rollback_msg, mngt);
}

void rollback_done(struct ftl_mngt *mngt, int status)
{
	struct ftl_mngt_step *step;
	struct ftl_mngt_process *process = &mngt->process;

	assert(!TAILQ_EMPTY(&process->rollback_queue_todo));
	step = TAILQ_FIRST(&process->rollback_queue_todo);
	TAILQ_REMOVE(&process->rollback_queue_todo, step, rollback.entry);
	TAILQ_INSERT_TAIL(&process->rollback_queue_done, step, rollback.entry);

	step->rollback.stop = spdk_get_ticks();
	step->rollback.status = status;

	trace_step(mngt->dev, step,  true);
}
