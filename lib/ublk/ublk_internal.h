/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */
/** \file
 * Userspace block device layer
 */
#ifndef SPDK_UBLK_INTERNAL_H
#define SPDK_UBLK_INTERNAL_H

#include "spdk/ublk.h"

#define UBLK_DEV_QUEUE_DEPTH	128
#define UBLK_DEV_NUM_QUEUE	1

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ublk_start_cb)(void *cb_arg, int result);
typedef void (*ublk_del_cb)(void *cb_arg);

int ublk_create_target(const char *cpumask_str);
int ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg);
int ublk_start_disk(const char *bdev_name, uint32_t ublk_id,
		    uint32_t num_queues, uint32_t queue_depth,
		    ublk_start_cb start_cb, void *cb_arg);
int ublk_stop_disk(uint32_t ublk_id, ublk_del_cb del_cb, void *cb_arg);
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
