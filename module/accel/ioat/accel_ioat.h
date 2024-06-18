/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_MODULE_IOAT_H
#define SPDK_ACCEL_MODULE_IOAT_H

#include "spdk/stdinc.h"

#define IOAT_MAX_CHANNELS	64

void accel_ioat_enable_probe(void);

#endif /* SPDK_ACCEL_MODULE_IOAT_H */
