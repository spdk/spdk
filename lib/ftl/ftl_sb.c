/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb.h"
#include "ftl_core.h"
#include "ftl_layout.h"

bool
ftl_superblock_check_magic(struct ftl_superblock *sb)
{
	return sb->header.magic == FTL_SUPERBLOCK_MAGIC;
}
