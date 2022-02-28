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

#ifndef FTL_LOG_H
#define FTL_LOG_H

#include "spdk/stdinc.h"
#include "spdk/log.h"

#define FTL_LOG_COMMON(TYPE, DEV, FORMAT, ...) \
	if ((DEV) == NULL) \
	{ \
		spdk_log(SPDK_LOG_##TYPE, __FILE__, __LINE__, __func__, "[FTL] "FORMAT, ## __VA_ARGS__); \
	} else { \
		spdk_log(SPDK_LOG_##TYPE, __FILE__, __LINE__, __func__, "[FTL][%s] "FORMAT, DEV->name, ## __VA_ARGS__); \
	} \

#define FTL_ERRLOG(DEV, FORMAT, ...) \
	FTL_LOG_COMMON(ERROR, DEV, FORMAT, ## __VA_ARGS__)

#define FTL_WARNLOG(DEV, FORMAT, ...) \
	FTL_LOG_COMMON(WARN, DEV, FORMAT, ## __VA_ARGS__)

#define FTL_NOTICELOG(DEV, FORMAT, ...) \
	FTL_LOG_COMMON(NOTICE, DEV, FORMAT, ## __VA_ARGS__)

#define FTL_INFOLOG(DEV, FORMAT, ...) \
	FTL_LOG_COMMON(INFO, DEV, FORMAT, ## __VA_ARGS__)

#define FTL_DEBUGLOG(DEV, FORMAT, ...) \
	FTL_LOG_COMMON(DEBUG, DEV, FORMAT, ## __VA_ARGS__)

#endif /* FTL_LOG_H */
