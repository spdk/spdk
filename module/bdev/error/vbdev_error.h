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

#ifndef SPDK_VBDEV_ERROR_H
#define SPDK_VBDEV_ERROR_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

enum vbdev_error_type {
	VBDEV_IO_FAILURE = 1,
	VBDEV_IO_PENDING,
};

typedef void (*spdk_delete_error_complete)(void *cb_arg, int bdeverrno);

/**
 * Create a vbdev on the base bdev to inject error into it.
 *
 * \param base_bdev_name Name of the base bdev.
 * \return 0 on success or negative on failure.
 */
int vbdev_error_create(const char *base_bdev_name);

/**
 * Delete vbdev used to inject errors.
 *
 * \param bdev Pointer to error vbdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Arguments to pass to cb_fn.
 */
void vbdev_error_delete(struct spdk_bdev *vbdev, spdk_delete_error_complete cb_fn,
			void *cb_arg);

/**
 * Inject error to the base bdev. Users can specify which IO type error is injected,
 * what type of error is injected, and how many errors are injected.
 *
 * \param name Name of the base bdev into which error is injected.
 * \param io_type IO type into which error is injected.
 * \param error_num Count of injected errors
 */
int vbdev_error_inject_error(char *name, uint32_t io_type, uint32_t error_type,
			     uint32_t error_num);

#endif /* SPDK_VBDEV_ERROR_H */
