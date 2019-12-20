/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FTL_DEBUG_H
#define FTL_DEBUG_H

#include "ftl_addr.h"
#include "ftl_band.h"
#include "ftl_core.h"
#include "ftl_rwb.h"

#if defined(DEBUG)
/* Debug flags - enabled when defined */
#define FTL_META_DEBUG	1
#define FTL_DUMP_STATS	1

#define ftl_debug(msg, ...) \
	fprintf(stderr, msg, ## __VA_ARGS__)
#else
#define ftl_debug(msg, ...)
#endif

static inline const char *
ftl_addr2str(struct ftl_addr addr, char *buf, size_t size)
{
	snprintf(buf, size, "(%"PRIu64")", addr.offset);
	return buf;
}

#if defined(FTL_META_DEBUG)
bool ftl_band_validate_md(struct ftl_band *band);
void ftl_dev_dump_bands(struct spdk_ftl_dev *dev);
#else
#define ftl_band_validate_md(band)
#define ftl_dev_dump_bands(dev)
#endif

#if defined(FTL_DUMP_STATS)
void ftl_dev_dump_stats(const struct spdk_ftl_dev *dev);
#else
#define ftl_dev_dump_stats(dev)
#endif

#endif /* FTL_DEBUG_H */
