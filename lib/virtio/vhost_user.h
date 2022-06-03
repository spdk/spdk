/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
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
