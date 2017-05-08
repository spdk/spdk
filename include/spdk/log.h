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

/**
 * \file
 * Logging interfaces
 */

#ifndef SPDK_LOG_H
#define SPDK_LOG_H

#include "spdk/stdinc.h"

/*
 * Default: 1 - noticelog messages will print to stderr and syslog.
 * Can be set to 0 to print noticelog messages to syslog only.
 */
extern unsigned int spdk_g_notice_stderr_flag;

#define SPDK_NOTICELOG(...) \
	spdk_noticelog(NULL, 0, NULL, __VA_ARGS__)
#define SPDK_WARNLOG(...) \
	spdk_warnlog(NULL, 0, NULL, __VA_ARGS__)
#define SPDK_ERRLOG(...) \
	spdk_errlog(__FILE__, __LINE__, __func__, __VA_ARGS__)

int spdk_set_log_facility(const char *facility);
const char *spdk_get_log_facility(void);
int spdk_set_log_priority(const char *priority);
void spdk_noticelog(const char *file, const int line, const char *func,
		    const char *format, ...) __attribute__((__format__(__printf__, 4, 5)));
void spdk_warnlog(const char *file, const int line, const char *func,
		  const char *format, ...) __attribute__((__format__(__printf__, 4, 5)));
void spdk_tracelog(const char *flag, const char *file, const int line,
		   const char *func, const char *format, ...) __attribute__((__format__(__printf__, 5, 6)));
void spdk_errlog(const char *file, const int line, const char *func,
		 const char *format, ...) __attribute__((__format__(__printf__, 4, 5)));
void spdk_trace_dump(const char *label, const uint8_t *buf, size_t len);

bool spdk_log_get_trace_flag(const char *flag);
int spdk_log_set_trace_flag(const char *flag);
int spdk_log_clear_trace_flag(const char *flag);

void spdk_open_log(void);
void spdk_close_log(void);

void spdk_tracelog_usage(FILE *f, const char *trace_arg);

#endif /* SPDK_LOG_H */
