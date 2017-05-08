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

#ifndef SPDK_BLOCKDEV_AIO_H
#define SPDK_BLOCKDEV_AIO_H

#include "spdk/stdinc.h"

#include <libaio.h>

#include "spdk/queue.h"
#include "spdk/bdev.h"

#include "spdk_internal/bdev.h"

struct blockdev_aio_task {
	struct iocb			iocb;
	uint64_t			len;
	TAILQ_ENTRY(blockdev_aio_task)	link;
};

struct blockdev_aio_io_channel {
	io_context_t		io_ctx;
	long			queue_depth;
	struct io_event		*events;
	struct spdk_poller	*poller;
};

struct file_disk {
	struct spdk_bdev	disk;
	const char		*file;
	int			fd;
	char			disk_name[SPDK_BDEV_MAX_NAME_LENGTH];
	uint64_t		size;

	/**
	 * For storing I/O that were completed synchronously, and will be
	 *   completed during next check_io call.
	 */
	TAILQ_HEAD(, blockdev_aio_task) sync_completion_list;
};

struct spdk_bdev *create_aio_disk(const char *name, const char *fname);

#endif // SPDK_BLOCKDEV_AIO_H
