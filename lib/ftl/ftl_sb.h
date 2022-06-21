/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_H
#define FTL_SB_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"
#include "ftl_sb_current.h"

struct spdk_ftl_dev;

bool ftl_superblock_check_magic(struct ftl_superblock *sb);

#endif /* FTL_SB_H */
