/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_ACCEL_DPDK_CRYPTODEV_H
#define SPDK_ACCEL_DPDK_CRYPTODEV_H

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_accel_dpdk_cryptodev_driver {
	SPDK_ACCEL_DPDK_CRYPTODEV_DRIVER_AESNI_MB = 0,
	SPDK_ACCEL_DPDK_CRYPTODEV_DRIVER_QAT,
	SPDK_ACCEL_DPDK_CRYPTODEV_DRIVER_MLX5_PCI,
	SPDK_ACCEL_DPDK_CRYPTODEV_DRIVER_UADK,
	SPDK_ACCEL_DPDK_CRYPTODEV_DRIVER_LAST
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_ACCEL_DPDK_CRYPTODEV_H */
