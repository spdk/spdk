/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_XNVME_H
#define SPDK_BDEV_XNVME_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/bdev.h"

#include "spdk/bdev_module.h"

struct spdk_bdev *create_xnvme_bdev(const char *name, const char *filename,
				    const char *io_mechanism, bool conserve_cpu);

void delete_xnvme_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_XNVME_H */
