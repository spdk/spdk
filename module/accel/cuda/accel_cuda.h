/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 StarWind Software, Inc.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_MODULE_CUDA_H
#define SPDK_ACCEL_MODULE_CUDA_H

#include "spdk/stdinc.h"

#define ACCEL_CUDA_XOR_MIN_BUF_LEN	    4096

#define ACCEL_CUDA_STREAMS_PER_CHANNEL	4

void accel_cuda_enable_probe(void);

#endif /* SPDK_ACCEL_MODULE_CUDA_H */
