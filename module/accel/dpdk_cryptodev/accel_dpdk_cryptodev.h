/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_DPDK_CRYPTODEV_H
#define SPDK_ACCEL_DPDK_CRYPTODEV_H

#include "spdk/stdinc.h"

void accel_dpdk_cryptodev_enable(void);
int accel_dpdk_cryptodev_set_driver(const char *driver_name);
const char *accel_dpdk_cryptodev_get_driver(void);

#endif /* SPDK_ACCEL_DPDK_CRYPTODEV_H */
