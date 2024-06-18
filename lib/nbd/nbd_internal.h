/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_NBD_INTERNAL_H
#define SPDK_NBD_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/nbd.h"

struct spdk_nbd_disk *nbd_disk_find_by_nbd_path(const char *nbd_path);

struct spdk_nbd_disk *nbd_disk_first(void);

struct spdk_nbd_disk *nbd_disk_next(struct spdk_nbd_disk *prev);

const char *nbd_disk_get_nbd_path(struct spdk_nbd_disk *nbd);

const char *nbd_disk_get_bdev_name(struct spdk_nbd_disk *nbd);

void nbd_disconnect(struct spdk_nbd_disk *nbd);

#endif /* SPDK_NBD_INTERNAL_H */
