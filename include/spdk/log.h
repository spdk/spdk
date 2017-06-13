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

/**
 * Initialize the logging module. Messages prior
 * to this call will be dropped.
 */
void spdk_log_open(void);

/**
 * Close the currently active log. Messages after this call
 * will be dropped.
 */
void spdk_log_close(void);

enum spdk_log_level {
	SPDK_LOG_ERROR,
	SPDK_LOG_WARN,
	SPDK_LOG_NOTICE,
	SPDK_LOG_INFO,
	SPDK_LOG_DEBUG,
};

/**
 * Set the threshold to log messages. Messages with a higher
 * level than this are ignored.
 */
void spdk_log_set_level(enum spdk_log_level level);

/**
 * Get the current log threshold.
 */
enum spdk_log_level spdk_log_get_level(void);

/**
 * Set the current log level threshold for printing to stderr.
 * Messages with a level less than or equal to this level
 * are also printed to stderr.
 */
void spdk_log_set_print_level(enum spdk_log_level level);

/**
 * Get the current log level print threshold.
 */
enum spdk_log_level spdk_log_get_print_level(void);

#define SPDK_NOTICELOG(...) \
	spdk_log(SPDK_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_WARNLOG(...) \
	spdk_log(SPDK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_ERRLOG(...) \
	spdk_log(SPDK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

void spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	      const char *format, ...) __attribute__((__format__(__printf__, 5, 6)));

void spdk_trace_dump(const char *label, const uint8_t *buf, size_t len);

bool spdk_log_get_trace_flag(const char *flag);
int spdk_log_set_trace_flag(const char *flag);
int spdk_log_clear_trace_flag(const char *flag);

void spdk_tracelog_usage(FILE *f, const char *trace_arg);

#endif /* SPDK_LOG_H */
