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
 * Memory copy offload engine abstraction layer
 */

#ifndef SPDK_COPY_ENGINE_H
#define SPDK_COPY_ENGINE_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_copy_completion_cb)(void *ref, int status);
typedef void (*spdk_copy_fini_cb)(void *cb_arg);

struct spdk_io_channel;

struct spdk_copy_task;

/**
 * Initialize the copy engine.
 *
 * \return 0 on success.
 */
int spdk_copy_engine_initialize(void);

/**
 * Close the copy engine.
 *
 * \param cb_fn Called when the close operation completes.
 * \param cb_arg Argument passed to the callback function.
 */
void spdk_copy_engine_finish(spdk_copy_fini_cb cb_fn, void *cb_arg);

/**
 * Get the configuration for the copy engine.
 *
 * \param fp The pointer to a file that will be written to the configuration.
 */
void spdk_copy_engine_config_text(FILE *fp);

/**
 * Close the copy engine module and perform any necessary cleanup.
 */
void spdk_copy_engine_module_finish(void);

/**
 * Get the I/O channel registered on the copy engine.
 *
 * This I/O channel is used to submit copy request.
 *
 * \return a pointer to the I/O channel on success, or NULL on failure.
 */
struct spdk_io_channel *spdk_copy_engine_get_io_channel(void);

/**
 * Submit a copy request.
 *
 * \param copy_req Copy request task.
 * \param ch I/O channel to submit request to the copy engine. This channel can
 * be obtained by the function spdk_copy_engine_get_io_channel().
 * \param dst Destination to copy to.
 * \param src Source to copy from.
 * \param nbytes Length in bytes to copy.
 * \param cb Called when this copy operation completes.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_copy_submit(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch, void *dst,
		     void *src, uint64_t nbytes, spdk_copy_completion_cb cb);

/**
 * Submit a fill request.
 *
 * This operation will fill the destination buffer with the specified value.
 *
 * \param copy_req Copy request task.
 * \param ch I/O channel to submit request to the copy engine. This channel can
 * be obtained by the function spdk_copy_engine_get_io_channel().
 * \param dst Destination to fill.
 * \param fill Constant byte to fill to the destination.
 * \param nbytes Length in bytes to fill.
 * \param cb Called when this copy operation completes.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_copy_submit_fill(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch,
			  void *dst, uint8_t fill, uint64_t nbytes, spdk_copy_completion_cb cb);

/**
 * Get the size of copy task.
 *
 * \return the size of copy task.
 */
size_t spdk_copy_task_size(void);

#ifdef __cplusplus
}
#endif

#endif
