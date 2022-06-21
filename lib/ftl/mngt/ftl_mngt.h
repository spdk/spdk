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

#ifndef FTL_MNGT_H
#define FTL_MNGT_H

#include "spdk/stdinc.h"
#include "spdk/ftl.h"

struct spdk_ftl_dev;
struct ftl_mngt;

/**
 * The FTL management callback function
 *
 * @param dev FTL device
 * @param mngt FTL management handle
 */
typedef void (*ftl_mngt_fn)(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

/**
 * The FTL management step descriptior
 */
struct ftl_mngt_step_desc {
	/**
	 * Name of the step
	 */
	const char *name;

	/**
	 * Size of the step argument (context)
	 *
	 * The step context will be allocated before execution of step's
	 * callback.
	 *
	 * @note The context can be reallocated (freed and newly allocated
	 * when calling ftl_mngt_alloc_step_cntx)
	 * @note It doesn't work like realloc
	 * @note The context can be retrieved within callback when calling
	 * ftl_mngt_get_step_cntx
	 */
	size_t arg_size;

	/**
	 * Step callback function
	 */
	ftl_mngt_fn action;

	/**
	 * It the step requires cleanup this is right place to put your handler.
	 * When a FTL management process fails cleanup callbacks are executed
	 * in rollback procedure. Cleanup functions are executed in reverse
	 * order to actions already called.
	 */
	ftl_mngt_fn cleanup;
};

/**
 * The FTL management process descriptor
 */
struct ftl_mngt_process_desc {
	/**
	 * The name of the process
	 */
	const char *name;

	/**
	 * Size of the process argument (context)
	 *
	 * The process context will be allocated before execution of the first
	 * step
	 *
	 * @note To get context of the process within FTL management callback,
	 * execute ftl_mngt_get_process_cntx
	 */
	size_t arg_size;

	/**
	 * Pointer to the additional error handler when the process fails
	 */
	ftl_mngt_fn error_handler;

	/**
	 * The FTL process steps
	 *
	 * The process context will be allocated before execution of the first
	 * step
	 *
	 * @note The step array terminator shall end with action equals NULL
	 */
	struct ftl_mngt_step_desc steps[];
};

/**
 * @brief Executes the FTL management process defined by the process descriptor
 *
 * @param dev FTL device
 * @param process The descriptor of process to be executed
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Result of invoking the operation
 * @retval 0 - The FTL management process has been started
 * @retval Non-zero An error occurred when starting The FTL management process
 */
int ftl_mngt_execute(struct spdk_ftl_dev *dev,
		     const struct ftl_mngt_process_desc *process,
		     ftl_mngt_fn cb, void *cb_cntx);

/**
 * @brief Executes rollback on the FTL management process defined by the process
 * descriptor
 *
 * All cleanup function from steps will be executed in reversed order
 *
 * @param dev FTL device
 * @param process The descriptor of process to be rollback
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Result of invoking the rollback operation
 * @retval 0 - Rollback of the FTL management process has been started
 * @retval Non-zero An error occurred when starting the rollback
 */
int ftl_mngt_rollback(struct spdk_ftl_dev *dev,
		      const struct ftl_mngt_process_desc *process,
		      ftl_mngt_fn cb, void *cb_cntx);

/*
 * FTL management API for steps
 */

/**
 * @brief Gets FTL device
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within step handler only
 *
 * @return FTL device
 */
struct spdk_ftl_dev *ftl_mngt_get_dev(struct ftl_mngt *mngt);

/*
 * @brief Set FTL device to NULL
 *
 * @param mngt FTL management handle
 */
void ftl_mngt_clear_dev(struct ftl_mngt *mngt);

/**
 * @brief Allocates a context for the management step
 *
 * @param mngt FTL management handle
 * @param size Size of the step context
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Operation result
 * @retval 0 Operation successful
 * @retval Non-zero Operation failure
 */
int ftl_mngt_alloc_step_cntx(struct ftl_mngt *mngt, size_t size);

/**
 * @brief Gets the management step context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Context of the step containing pointer to buffer and its size
 */
void *ftl_mngt_get_step_cntx(struct ftl_mngt *mngt);

/**
 * @brief Gets the management process context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Context of the process containing pointer to buffer and its size
 */
void *ftl_mngt_get_process_cntx(struct ftl_mngt *mngt);

/**
 * @brief Gets the caller context
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return Pointer to the caller context
 */
void *ftl_mngt_get_caller_context(struct ftl_mngt *mngt);

/**
 * @brief Gets the status of executed management process
 *
 * @param mngt FTL management handle
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @return The operation result of the management process
 * @retval 0 Operation successful
 * @retval Non-zero Operation failure
 */
int ftl_mngt_get_status(struct ftl_mngt *mngt);

/**
 * @brief Finishes the management process immediately
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle of process to be finished
 */
void ftl_mngt_finish(struct ftl_mngt *mngt);

/**
 * @brief Completes the step currently in progress and jump to a next one
 *
 * If no more steps to be executed then the management process is finished and
 * caller callback is invoked
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
void ftl_mngt_next_step(struct ftl_mngt *mngt);

/**
 * @brief Skips the step currently in progress and jump to a next one
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
void ftl_mngt_skip_step(struct ftl_mngt *mngt);

/**
 * @brief Continue the step currently in progress
 *
 * This causes invoking the same step handler in next iteration of the
 * management process. This mechanism can be used by a job when polling for
 * something.
 *
 * @note This function can be invoked within ftl_mngt_fn callback only
 *
 * @param mngt FTL management handle
 */
void ftl_mngt_continue_step(struct ftl_mngt *mngt);

/**
 * @brief Fail the step currently in progress.
 *
 * It stops executing all steps and starts the rollback procedure (calling
 * the cleanup functions of all already executed steps).
 *
 *
 * @param mngt FTL management handle
 */
void ftl_mngt_fail_step(struct ftl_mngt *mngt);

/**
 * @brief Calls another management process
 *
 * Ends the current step and executes specified process and finally continues
 * executing the the remaining steps
 *
 * @param mngt The management handle
 * @param process The management process to be called
 */
void ftl_mngt_call(struct ftl_mngt *mngt,
		   const struct ftl_mngt_process_desc *process);

/**
 * @brief Calls rollback steps of another management process
 *
 * Ends the current step and executes rollback steps of specified process
 * and finally continues executing the remaining steps
 *
 * @param mngt The management handle
 * @param process The management process to be called to execute rollback
 */
void ftl_mngt_call_rollback(struct ftl_mngt *mngt,
			    const struct ftl_mngt_process_desc *process);

/*
 * The specific management functions
 */
/**
 * @brief Starts up a FTL instance
 *
 * @param dev FTL device
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Operation result
 * @retval 0 The operation successful has started
 * @retval Non-zero Startup failure
 */
int ftl_mngt_startup(struct spdk_ftl_dev *dev,
		     ftl_mngt_fn cb, void *cb_cntx);

/**
 * @brief Shuts down a FTL instance
 *
 * @param dev FTL device
 * @param cb Caller callback
 * @param cb_cntx Caller context
 *
 * @return Operation result
 * @retval 0 The operation successful has started
 * @retval Non-zero Shutdown failure
 */
int ftl_mngt_shutdown(struct spdk_ftl_dev *dev,
		      ftl_mngt_fn cb, void *cb_cntx);

#endif /* LIB_FTL_FTL_MNGT_H */
