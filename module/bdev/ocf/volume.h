/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_DOBJ_H
#define VBDEV_OCF_DOBJ_H

#include <ocf/ocf.h>

#include "ctx.h"
#include "data.h"

/* ocf_io context
 * It is initialized from io size and offset */
struct ocf_io_ctx {
	struct bdev_ocf_data *data;
	struct spdk_io_channel *ch;
	uint32_t offset;
	int ref;
	int rq_cnt;
	int error;
	bool iovs_allocated;
};

int vbdev_ocf_volume_init(void);
void vbdev_ocf_volume_cleanup(void);

static inline struct ocf_io_ctx *
ocf_get_io_ctx(struct ocf_io *io)
{
	return ocf_io_get_priv(io);
}

#endif
