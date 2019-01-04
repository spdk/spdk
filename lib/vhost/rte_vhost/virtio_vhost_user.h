/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018 Red Hat, Inc.
 */

#ifndef _LINUX_VIRTIO_VHOST_USER_H
#define _LINUX_VIRTIO_VHOST_USER_H

#include <stdint.h>

struct virtio_vhost_user_config {
    uint32_t status;
#define VIRTIO_VHOST_USER_STATUS_SLAVE_UP 0
#define VIRTIO_VHOST_USER_STATUS_MASTER_UP 1
    uint32_t max_vhost_queues;
    uint8_t uuid[16];
};

#endif /* _LINUX_VIRTIO_VHOST_USER_H */
