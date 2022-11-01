/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_OPAL_H
#define SPDK_VBDEV_OPAL_H

#include "spdk/bdev_module.h"
#include "bdev_nvme.h"

int vbdev_opal_create(const char *nvme_ctrlr_name, uint32_t nsid, uint8_t locking_range_id,
		      uint64_t range_start, uint64_t range_length, const char *password);

struct spdk_opal_locking_range_info *vbdev_opal_get_info_from_bdev(const char *opal_bdev_name,
		const char *password);

int vbdev_opal_destruct(const char *bdev_name, const char *password);

int vbdev_opal_enable_new_user(const char *bdev_name, const char *admin_password,
			       uint16_t user_id, const char *user_password);

int vbdev_opal_set_lock_state(const char *bdev_name, uint16_t user_id, const char *password,
			      const char *lock_state);

#endif
