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

#ifdef __cplusplus
extern "C" {
#endif

int ublk_create_target(const char *cpumask_str);
int ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_UBLK_INTERNAL_H */
