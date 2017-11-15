/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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

#ifndef _VIRTIO_USER_DEV_H
#define _VIRTIO_USER_DEV_H

#include "spdk/stdinc.h"

#include <linux/virtio_ring.h>

#include "vhost.h"

#include "../virtio_dev.h"

#define VIRTIO_MAX_VIRTQUEUES 0x100

struct virtio_user_dev {
	struct virtio_dev vdev;

	/* for vhost_user backend */
	int		vhostfd;

	/* for both vhost_user and vhost_kernel */
	int		callfds[VIRTIO_MAX_VIRTQUEUES];
	int		kickfds[VIRTIO_MAX_VIRTQUEUES];
	uint32_t	queue_size;

	uint8_t		status;
	char		path[PATH_MAX];
	struct vring	vrings[VIRTIO_MAX_VIRTQUEUES];
	struct virtio_user_backend_ops *ops;
};

int virtio_user_start_device(struct virtio_user_dev *dev);
int virtio_user_stop_device(struct virtio_user_dev *dev);

/**
 * Connect to a vhost-user device and create corresponding virtio_dev.
 *
 * \param name name of this virtio device
 * \param path path to the Unix domain socket of the vhost-user device
 * \param requested_queues maximum number of request queues that this
 * device will support
 * \param queue_size size of each of the queues
 * \param fixed_queue_num number of queues preceeding the first
 * request queue. For Virtio-SCSI this is equal to 2, as there are
 * additional event and control queues.
 * \return virtio device
 */
struct virtio_dev *virtio_user_dev_init(const char *name, const char *path,
					uint16_t requested_queues,
					uint32_t queue_size, uint16_t fixed_queue_num);

#endif
