/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

enum compress_pmd {
	COMPRESS_PMD_AUTO = 0,
	COMPRESS_PMD_QAT_ONLY,
	COMPRESS_PMD_MLX5_PCI_ONLY,
	COMPRESS_PMD_MAX,
};

void accel_dpdk_compressdev_enable(void);
int accel_compressdev_enable_probe(enum compress_pmd *opts);
