/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */
/** \file
 * Userspace block device layer
 */
#ifndef SPDK_UBLK_INTERNAL_H
#define SPDK_UBLK_INTERNAL_H

#include <linux/ublk_cmd.h>

#include "spdk/ublk.h"

#ifndef UBLK_F_CMD_IOCTL_ENCODE
#define UBLK_F_CMD_IOCTL_ENCODE	(1UL << 6)
#endif

#ifndef UBLK_F_USER_COPY
#define UBLK_F_USER_COPY	(1UL << 7)
#endif

#ifndef UBLK_U_CMD_GET_FEATURES
#define UBLK_U_CMD_GET_FEATURES	_IOR('u', 0x13, struct ublksrv_ctrl_cmd)
#endif

#ifndef UBLKSRV_IO_BUF_OFFSET
#define UBLKSRV_IO_BUF_OFFSET	0x80000000
#endif

#ifndef UBLK_IO_BUF_BITS
#define UBLK_IO_BUF_BITS	25
#endif

#ifndef UBLK_TAG_OFF
#define UBLK_TAG_OFF		UBLK_IO_BUF_BITS
#endif

#ifndef UBLK_TAG_BITS
#define UBLK_TAG_BITS		16
#endif

#ifndef UBLK_QID_OFF
#define UBLK_QID_OFF		(UBLK_TAG_OFF + UBLK_TAG_BITS)
#endif


#define UBLK_DEV_QUEUE_DEPTH	128
#define UBLK_DEV_NUM_QUEUE	1

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ublk_ctrl_cb)(void *cb_arg, int result);

int ublk_create_target(const char *cpumask_str);
int ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg);
int ublk_start_disk(const char *bdev_name, uint32_t ublk_id,
		    uint32_t num_queues, uint32_t queue_depth,
		    ublk_ctrl_cb ctrl_cb, void *cb_arg);
int ublk_stop_disk(uint32_t ublk_id, ublk_ctrl_cb ctrl_cb, void *cb_arg);
int
ublk_start_disk_recovery(const char *bdev_name, uint32_t ublk_id, ublk_ctrl_cb ctrl_cb,
			 void *cb_arg);
struct spdk_ublk_dev *ublk_dev_find_by_id(uint32_t ublk_id);
uint32_t ublk_dev_get_id(struct spdk_ublk_dev *ublk);
const char *ublk_dev_get_bdev_name(struct spdk_ublk_dev *ublk);
struct spdk_ublk_dev *ublk_dev_first(void);
struct spdk_ublk_dev *ublk_dev_next(struct spdk_ublk_dev *prev);
uint32_t ublk_dev_get_queue_depth(struct spdk_ublk_dev *ublk);
uint32_t ublk_dev_get_num_queues(struct spdk_ublk_dev *ublk);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_UBLK_INTERNAL_H */
