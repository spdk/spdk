/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Vector async io ops
 */

#ifndef SPDK_AIO_MGR_H
#define SPDK_AIO_MGR_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

struct spdk_aio_mgr_io;
struct spdk_aio_mgr;

typedef void (*fsdev_aio_done_cb)(void *ctx, uint32_t data_size, int error);

struct spdk_aio_mgr *spdk_aio_mgr_create(uint32_t max_aios);
struct spdk_aio_mgr_io *spdk_aio_mgr_read(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb,
		void *ctx, int fd, uint64_t offs, uint32_t size, struct iovec *iovs, uint32_t iovcnt);
struct spdk_aio_mgr_io *spdk_aio_mgr_write(struct spdk_aio_mgr *mgr, fsdev_aio_done_cb clb,
		void *ctx, int fd, uint64_t offs, uint32_t size, const struct iovec *iovs, uint32_t iovcnt);
void spdk_aio_mgr_cancel(struct spdk_aio_mgr *mgr, struct spdk_aio_mgr_io *aio);
void spdk_aio_mgr_poll(struct spdk_aio_mgr *mgr);
void spdk_aio_mgr_delete(struct spdk_aio_mgr *mgr);

#endif /* SPDK_AIO_MGR_H */
