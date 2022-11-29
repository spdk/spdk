/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/**
 * \file
 * Logging interfaces
 */

#ifndef SPDK_LOG_H
#define SPDK_LOG_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * for passing user-provided log call
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source file line.
 * \param func Current source function name.
 * \param format Format string to the message.
 * \param args Additional arguments for format string.
 */
typedef void logfunc(int level, const char *file, const int line,
		     const char *func, const char *format, va_list args);

/**
 * Initialize the logging module. Messages prior
 * to this call will be dropped.
 */
void spdk_log_open(logfunc *logf);

/**
 * Close the currently active log. Messages after this call
 * will be dropped.
 */
void spdk_log_close(void);

/**
 * Enable or disable timestamps
 */
void spdk_log_enable_timestamps(bool value);

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
 * Get syslog level based on SPDK current log level threshold.
 *
 * \param level Log level threshold
 * \return -1 for disable log print, otherwise is syslog level.
 */
int spdk_log_to_syslog_level(enum spdk_log_level level);

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

#ifdef DEBUG
#define SPDK_DEBUGLOG_FLAG_ENABLED(name) spdk_log_get_flag(name)
#else
#define SPDK_DEBUGLOG_FLAG_ENABLED(name) false
#endif

#define SPDK_NOTICELOG(...) \
	spdk_log(SPDK_LOG_NOTICE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_WARNLOG(...) \
	spdk_log(SPDK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_ERRLOG(...) \
	spdk_log(SPDK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define SPDK_PRINTF(...) \
	spdk_log(SPDK_LOG_NOTICE, NULL, -1, NULL, __VA_ARGS__)
#define SPDK_INFOLOG(FLAG, ...)									\
	do {											\
		extern struct spdk_log_flag SPDK_LOG_##FLAG;					\
		if (SPDK_LOG_##FLAG.enabled) {							\
			spdk_log(SPDK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)

#ifdef DEBUG
#define SPDK_DEBUGLOG(FLAG, ...)								\
	do {											\
		extern struct spdk_log_flag SPDK_LOG_##FLAG;					\
		if (SPDK_LOG_##FLAG.enabled) {							\
			spdk_log(SPDK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)

#define SPDK_LOGDUMP(FLAG, LABEL, BUF, LEN)				\
	do {								\
		extern struct spdk_log_flag SPDK_LOG_##FLAG;		\
		if (SPDK_LOG_##FLAG.enabled) {				\
			spdk_log_dump(stderr, (LABEL), (BUF), (LEN));	\
		}							\
	} while (0)

#else
#define SPDK_DEBUGLOG(...) do { } while (0)
#define SPDK_LOGDUMP(...) do { } while (0)
#endif

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
 * Same as spdk_log except that instead of being called with variable number of
 * arguments it is called with an argument list as defined in stdarg.h
 *
 * \param level Log level threshold.
 * \param file Name of the current source file.
 * \param line Current source line number.
 * \param func Current source function name.
 * \param format Format string to the message.
 * \param ap printf arguments
 */
void spdk_vlog(enum spdk_log_level level, const char *file, const int line, const char *func,
	       const char *format, va_list ap);

/**
 * Log the contents of a raw buffer to a file.
 *
 * \param fp File to hold the log.
 * \param label Label to print to the file.
 * \param buf Buffer that holds the log information.
 * \param len Length of buffer to dump.
 */
void spdk_log_dump(FILE *fp, const char *label, const void *buf, size_t len);

struct spdk_log_flag {
	TAILQ_ENTRY(spdk_log_flag) tailq;
	const char *name;
	bool enabled;
};

/**
 * Register a log flag.
 *
 * \param name Name of the log flag.
 * \param flag Log flag to be added.
 */
void spdk_log_register_flag(const char *name, struct spdk_log_flag *flag);

#define SPDK_LOG_REGISTER_COMPONENT(FLAG) \
struct spdk_log_flag SPDK_LOG_##FLAG = { \
	.name = #FLAG, \
	.enabled = false, \
}; \
__attribute__((constructor)) static void register_flag_##FLAG(void) \
{ \
	spdk_log_register_flag(#FLAG, &SPDK_LOG_##FLAG); \
}

/**
 * Get the first registered log flag.
 *
 * \return The first registered log flag.
 */
struct spdk_log_flag *spdk_log_get_first_flag(void);

/**
 * Get the next registered log flag.
 *
 * \param flag The current log flag.
 *
 * \return The next registered log flag.
 */
struct spdk_log_flag *spdk_log_get_next_flag(struct spdk_log_flag *flag);

/**
 * Check whether the log flag exists and is enabled.
 *
 * \return true if enabled, or false otherwise.
 */
bool spdk_log_get_flag(const char *flag);

/**
 * Enable the log flag.
 *
 * \param flag Log flag to be enabled.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_log_set_flag(const char *flag);

/**
 * Clear a log flag.
 *
 * \param flag Log flag to clear.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_log_clear_flag(const char *flag);

/**
 * Show all the log flags and their usage.
 *
 * \param f File to hold all the flags' information.
 * \param log_arg Command line option to set/enable the log flag.
 */
void spdk_log_usage(FILE *f, const char *log_arg);

struct spdk_deprecation;

/**
 * Register a deprecation. Most consumers will use SPDK_LOG_DEPRECATION_REGISTER() instead.
 *
 * \param tag A unique string that will appear in each log message and should appear in
 * documentation.
 * \param description A descriptive string that will also be logged.
 * \param rate_limit_seconds If non-zero, log messages related to this deprecation will appear no
 * more frequently than this interval.
 * \param remove_release The release when the deprecated support will be removed.
 * \param reg Pointer to storage for newly allocated deprecation handle.
 * \return 0 on success or negative errno on failure.
 */
int spdk_log_deprecation_register(const char *tag, const char *description,
				  const char *remove_release, uint32_t rate_limit_seconds,
				  struct spdk_deprecation **reg);

#define SPDK_LOG_DEPRECATION_REGISTER(tag, desc, release, rate) \
	static struct spdk_deprecation *_deprecated_##tag; \
	static void __attribute__((constructor)) _spdk_deprecation_register_##tag(void) \
	{ \
		int rc; \
		rc = spdk_log_deprecation_register(#tag, desc, release, rate, &_deprecated_##tag); \
		(void)rc; \
		assert(rc == 0); \
	}

/**
 * Indicate that a deprecated feature was used. Most consumers will use SPDK_LOG_DEPRECATED()
 * instead.
 *
 * \param deprecation The deprecated feature that was used.
 * \param file The name of the source file where the deprecated feature was used.
 * \param line The line in file where where the deprecated feature was used.
 * \param func The name of the function where where the deprecated feature was used.
 */
void spdk_log_deprecated(struct spdk_deprecation *deprecation, const char *file, uint32_t line,
			 const char *func);

#define SPDK_LOG_DEPRECATED(tag) \
	spdk_log_deprecated(_deprecated_##tag, __FILE__, __LINE__, __func__)

/**
 * Callback function for spdk_log_for_each_deprecation().
 *
 * \param ctx Context passed via spdk_log_for_each_deprecation().
 * \param deprecation Pointer to a deprecation structure.
 * \return 0 to continue iteration or non-zero to stop iteration.
 */
typedef int (*spdk_log_for_each_deprecation_fn)(void *ctx, struct spdk_deprecation *deprecation);

/**
 * Iterate over all deprecations, calling a callback on each of them.
 *
 * Iteration will stop early if the callback function returns non-zero.
 *
 * \param ctx Context to pass to the callback.
 * \param fn Callback function
 * \return The value from the last callback called or 0 if there are no deprecations.
 */
int spdk_log_for_each_deprecation(void *ctx, spdk_log_for_each_deprecation_fn fn);

/**
 * Get a deprecation's tag.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's tag.
 */
const char *spdk_deprecation_get_tag(const struct spdk_deprecation *deprecation);

/**
 * Get a deprecation's description.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's description.
 */
const char *spdk_deprecation_get_description(const struct spdk_deprecation *deprecation);

/**
 * Get a deprecation's planned removal release.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's planned removal release.
 */
const char *spdk_deprecation_get_remove_release(const struct spdk_deprecation *deprecation);

/**
 * Get the number of times that a deprecation's code has been executed.
 *
 * \param deprecation A pointer to an spdk_deprecation.
 * \return The deprecation's planned removal release.
 */
uint64_t spdk_deprecation_get_hits(const struct spdk_deprecation *deprecation);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_LOG_H */
