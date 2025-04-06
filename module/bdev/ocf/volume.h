/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_VOLUME_H
#define VBDEV_OCF_VOLUME_H

struct vbdev_ocf_base {
	bool				is_cache;
	bool				attached;
	struct spdk_bdev *		bdev;
	struct spdk_bdev_desc *		desc;
	struct spdk_io_channel *	mngt_ch;
	struct spdk_thread *		thread;
};

void vbdev_ocf_base_detach(struct vbdev_ocf_base *base);
int vbdev_ocf_volume_init(void);
void vbdev_ocf_volume_cleanup(void);

#endif
