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

/** \file
 * Acceleration engine abstraction layer
 */

#ifndef SPDK_ACCEL_ENGINE_H
#define SPDK_ACCEL_ENGINE_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Acceleration operation callback.
 *
 * \param ref 'accel_req' passed to the corresponding spdk_accel_submit* call.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_accel_completion_cb)(void *ref, int status);

/**
 * Acceleration engine finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_accel_fini_cb)(void *cb_arg);

struct spdk_io_channel;

struct spdk_accel_task;

/**
 * Initialize the acceleration engine.
 *
 * \return 0 on success.
 */
int spdk_accel_engine_initialize(void);

/**
 * Close the acceleration engine.
 *
 * \param cb_fn Called when the close operation completes.
 * \param cb_arg Argument passed to the callback function.
 */
void spdk_accel_engine_finish(spdk_accel_fini_cb cb_fn, void *cb_arg);

/**
 * Get the configuration for the acceleration engine.
 *
 * \param fp The pointer to a file that will be written to the configuration.
 */
void spdk_accel_engine_config_text(FILE *fp);

/**
 * Close the acceleration engine module and perform any necessary cleanup.
 */
void spdk_accel_engine_module_finish(void);

/**
 * Get the I/O channel registered on the acceleration engine.
 *
 * This I/O channel is used to submit copy request.
 *
 * \return a pointer to the I/O channel on success, or NULL on failure.
 */
struct spdk_io_channel *spdk_accel_engine_get_io_channel(void);

/**
 * Submit a copy request.
 *
 * \param accel_req Accel request task.
 * \param ch I/O channel to submit request to the accel engine. This channel can
 * be obtained by the function spdk_accel_engine_get_io_channel().
 * \param dst Destination to copy to.
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb Called when this copy operation completes.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_copy(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch, void *dst,
			   void *src, uint64_t nbytes, spdk_accel_completion_cb cb);

/**
 * Submit a fill request.
 *
 * This operation will fill the destination buffer with the specified value.
 *
 * \param accel_req Accel request task.
 * \param ch I/O channel to submit request to the accel engine. This channel can
 * be obtained by the function spdk_accel_engine_get_io_channel().
 * \param dst Destination to fill.
 * \param fill Constant byte to fill to the destination.
 * \param nbytes Length in bytes to fill.
 * \param cb Called when this fill operation completes.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_accel_submit_fill(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			   void *dst, uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb);

/**
 * Get the size of an acceleration task.
 *
 * \return the size of acceleration task.
 */
size_t spdk_accel_task_size(void);

struct spdk_json_write_ctx;

/**
 * Write Acceleration subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_accel_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif
