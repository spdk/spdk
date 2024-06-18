/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_LOG_H
#define FTL_LOG_H

#include "spdk/stdinc.h"
#include "spdk/log.h"

#define FTL_LOG_COMMON(type, dev, format, ...) \
	spdk_log(SPDK_LOG_##type, __FILE__, __LINE__, __func__, "[FTL][%s] "format, \
		 (dev) != NULL ? (dev)->conf.name : "N/A", ## __VA_ARGS__)

#define FTL_ERRLOG(dev, format, ...) \
	FTL_LOG_COMMON(ERROR, dev, format, ## __VA_ARGS__)

#define FTL_WARNLOG(dev, format, ...) \
	FTL_LOG_COMMON(WARN, dev, format, ## __VA_ARGS__)

#define FTL_NOTICELOG(dev, format, ...) \
	FTL_LOG_COMMON(NOTICE, dev, format, ## __VA_ARGS__)

#define FTL_INFOLOG(dev, format, ...) \
	FTL_LOG_COMMON(INFO, dev, format, ## __VA_ARGS__)

#define FTL_DEBUGLOG(dev, format, ...) \
	FTL_LOG_COMMON(DEBUG, dev, format, ## __VA_ARGS__)

#endif /* FTL_LOG_H */
