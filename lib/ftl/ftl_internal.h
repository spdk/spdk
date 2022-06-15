/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_INTERNAL_H
#define FTL_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

/* Marks address as invalid */
#define FTL_ADDR_INVALID	((ftl_addr)-1)
/* Marks LBA as invalid */
#define FTL_LBA_INVALID		((uint64_t)-1)
/* Smallest data unit size */
#define FTL_BLOCK_SIZE		4096ULL

/*
 * This type represents address in the ftl address space. Values from 0 to based bdev size are
 * mapped directly to base device lbas. Values above that represent nv cache lbas.
 */
typedef uint64_t ftl_addr;

/* Number of LBAs that could be stored in a single block */
#define FTL_NUM_LBA_IN_BLOCK	(FTL_BLOCK_SIZE / sizeof(uint64_t))

#endif /* FTL_INTERNAL_H */
