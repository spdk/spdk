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

#ifndef _VHOST_H
#define _VHOST_H

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

struct virtio_user_backend_ops;

struct virtio_user_dev {
	int		vhostfd;

	int		callfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	int		kickfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
	uint32_t	queue_size;

	uint8_t		status;
	char		path[PATH_MAX];
	uint64_t	protocol_features;
	struct vring	vrings[SPDK_VIRTIO_MAX_VIRTQUEUES];
	struct virtio_user_backend_ops *ops;
	struct spdk_mem_map *mem_map;
};

struct virtio_user_backend_ops {
	int (*setup)(struct virtio_user_dev *dev);
	int (*send_request)(struct virtio_user_dev *dev,
			    enum vhost_user_request req,
			    void *arg);
};

extern struct virtio_user_backend_ops ops_user;

#endif
