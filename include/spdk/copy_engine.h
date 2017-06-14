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

typedef void (*spdk_copy_completion_cb)(void *ref, int status);

struct spdk_io_channel;

struct spdk_copy_task;

int spdk_copy_engine_initialize(void);
int spdk_copy_engine_finish(void);

struct spdk_io_channel *spdk_copy_engine_get_io_channel(void);
int64_t spdk_copy_submit(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch, void *dst,
			 void *src, uint64_t nbytes, spdk_copy_completion_cb cb);
int64_t spdk_copy_submit_fill(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch,
			      void *dst, uint8_t fill, uint64_t nbytes, spdk_copy_completion_cb cb);
size_t spdk_copy_task_size(void);

#endif
