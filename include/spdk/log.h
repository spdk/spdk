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

#ifdef __cplusplus
extern "C" {
#endif

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
	/** All messages will be suppressed. */
	SPDK_LOG_DISABLED = -1,
	SPDK_LOG_ERROR,
	SPDK_LOG_WARN,
	SPDK_LOG_NOTICE,
	SPDK_LOG_INFO,
	SPDK_LOG_DEBUG,
};

/**
 * Set the log level threshold to log messages. Messages with a higher
 * level than this are ignored.
 *
 * \param level Log level threshold to set to log messages.
 */
void spdk_log_set_level(enum spdk_log_level level);

/**
 * Get the current log level threshold.
 *
 * \return the current log level threshold.
 */
enum spdk_log_level spdk_log_get_level(void);

/**
 * Set the log level threshold to include stack trace in log messages.
 * Messages with a higher level than this will not contain stack trace. You
 * can use \c SPDK_LOG_DISABLED to completely disable stack trace printing
 * even if it is supported.
 *
 * \note This function has no effect if SPDK is built without stack trace
 *  printing support.
 *
 * \param level Log level threshold for stacktrace.
 */
void spdk_log_set_backtrace_level(enum spdk_log_level level);

/**
 * Get the current log level threshold for showing stack trace in log message.
 *
 * \return the current log level threshold for stack trace.
 */
enum spdk_log_level spdk_log_get_backtrace_level(void);

/**
 * Set the current log level threshold for printing to stderr.
 * Messages with a level less than or equal to this level
 * are also printed to stderr. You can use \c SPDK_LOG_DISABLED to completely
 * suppress log printing.
 *
 * \param level Log level threshold for printing to stderr.
 */
void spdk_log_set_print_level(enum spdk_log_level level);

/**
 * Get the current log level print threshold.
 *
 * \return the current log level print threshold.
 */
enum spdk_log_level spdk_log_get_print_level(void);

#define SPDK_NOTICELOG(...) \
	spdk_log(SPDK_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_WARNLOG(...) \
	spdk_log(SPDK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_ERRLOG(...) \
	spdk_log(SPDK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

/**
 * Write messages to the log file. If \c level is set to \c SPDK_LOG_DISABLED,
 * this log message won't be written.
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 */
void spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	      const char *format, ...) __attribute__((__format__(__printf__, 5, 6)));

/**
 * Dump the trace to a file.
 *
 * \param fp File to hold the trace.
 * \param label Label to print to the file.
 * \param buf Buffer that holds the trace information.
 * \param len Length of trace to dump.
 */
void spdk_trace_dump(FILE *fp, const char *label, const void *buf, size_t len);

/**
 * Check whether the trace flag exists and is enabled.
 *
 * \return true if enabled, or false otherwise.
 */
bool spdk_log_get_trace_flag(const char *flag);

/**
 * Enable the trace flag.
 *
 * \param flag Trace flag to be enabled.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_log_set_trace_flag(const char *flag);

/**
 * Clear a trace flag.
 *
 * \param flag Trace flag to clear.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_log_clear_trace_flag(const char *flag);

/**
 * Show all the log trace flags and their usage.
 *
 * \param f File to hold all the flags' information.
 * \param trace_arg Command line option to set/enable the trace flag.
 */
void spdk_tracelog_usage(FILE *f, const char *trace_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_LOG_H */
