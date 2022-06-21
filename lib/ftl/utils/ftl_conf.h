/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_CONF_H
#define FTL_CONF_H

#include "spdk/ftl.h"

int ftl_conf_cpy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src);

void ftl_conf_deinit(struct spdk_ftl_conf *conf);

#endif /* FTL_DEFS_H */
