/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

#ifndef SPDK_THREAD_INTERNAL_H_
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/tree.h"

/**
 * \brief Represents a per-thread channel for accessing an I/O device.
 *
 * An I/O device may be a physical entity (i.e. NVMe controller) or a software
 *  entity (i.e. a blobstore).
 *
 * This structure is not part of the API - all accesses should be done through
 *  spdk_io_channel function calls.
 */
struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct io_device		*dev;
	uint32_t			ref;
	uint32_t			destroy_ref;
	RB_ENTRY(spdk_io_channel)	node;
	spdk_io_channel_destroy_cb	destroy_cb;

	uint8_t				_padding[40];
	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_io_channel) == SPDK_IO_CHANNEL_STRUCT_SIZE, "incorrect size");

#endif /* SPDK_THREAD_INTERNAL_H_ */
