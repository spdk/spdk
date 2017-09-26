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

#ifndef SPDK_BDEV_VIRTIO_H
#define SPDK_BDEV_VIRTIO_H

#include <spdk/event.h>
#include <spdk/bdev.h>

enum virtio_scsi_io_type
{
	VIRTIO_SCSI_IO_TYPE_INQUIRY = SPDK_BDEV_IO_TYPE_USER,
	VIRTIO_SCSI_IO_TYPE_READ_CAP_10,
	VIRTIO_SCSI_IO_TYPE_READ_CAP_16,
};

/**
 *
 * \param path
 *   Path to socket.
 * \param max_queue
 *   Max number of queues
 * \param vq_size
 *   Max queue size.
 * \param done_cb
 *   Callback called just after adding new bdev from controller.
 *   First parameter is cb_ctx, second is new bdev.
 *   Last call will have second parameter set to NULL to marking that process is complete.
 * \return
 *   Zero or negative error code on error.
 *   In case of error before establishing valid connection \c done_cb is not called.
 */
int spdk_virtio_user_scsi_connect(const char *path, uint32_t max_queue, uint32_t vq_size, spdk_event_fn done_cb, void *cb_ctx);

#endif /* SPDK_BDEV_VIRTIO_H */
