/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_DPDK_CRYPTODEV_MODULE_H
#define SPDK_ACCEL_DPDK_CRYPTODEV_MODULE_H

#include "spdk/stdinc.h"
#include "spdk/module/accel/dpdk_cryptodev.h"

void accel_dpdk_cryptodev_enable(void);
int accel_dpdk_cryptodev_set_driver(enum spdk_accel_dpdk_cryptodev_driver driver);
enum spdk_accel_dpdk_cryptodev_driver accel_dpdk_cryptodev_get_driver(void);
const char *accel_dpdk_cryptodev_driver_to_str(enum spdk_accel_dpdk_cryptodev_driver driver);

#endif /* SPDK_ACCEL_DPDK_CRYPTODEV_MODULE_H */
