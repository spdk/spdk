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

#include "spdk_internal/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <ctype.h>
#include <errno.h>

static TAILQ_HEAD(, spdk_trace_flag) g_trace_flags = TAILQ_HEAD_INITIALIZER(g_trace_flags);

unsigned int spdk_g_notice_stderr_flag = 1;
unsigned int spdk_g_log_facility = LOG_DAEMON;
unsigned int spdk_g_log_priority = LOG_NOTICE;

SPDK_LOG_REGISTER_TRACE_FLAG("debug", SPDK_TRACE_DEBUG)

#define MAX_TMPBUF 1024

int
spdk_set_log_facility(const char *facility)
{
	if (strcasecmp(facility, "daemon") == 0) {
		spdk_g_log_facility = LOG_DAEMON;
	} else if (strcasecmp(facility, "auth") == 0) {
		spdk_g_log_facility = LOG_AUTH;
	} else if (strcasecmp(facility, "authpriv") == 0) {
		spdk_g_log_facility = LOG_AUTHPRIV;
	} else if (strcasecmp(facility, "local1") == 0) {
		spdk_g_log_facility = LOG_LOCAL1;
	} else if (strcasecmp(facility, "local2") == 0) {
		spdk_g_log_facility = LOG_LOCAL2;
	} else if (strcasecmp(facility, "local3") == 0) {
		spdk_g_log_facility = LOG_LOCAL3;
	} else if (strcasecmp(facility, "local4") == 0) {
		spdk_g_log_facility = LOG_LOCAL4;
	} else if (strcasecmp(facility, "local5") == 0) {
		spdk_g_log_facility = LOG_LOCAL5;
	} else if (strcasecmp(facility, "local6") == 0) {
		spdk_g_log_facility = LOG_LOCAL6;
	} else if (strcasecmp(facility, "local7") == 0) {
		spdk_g_log_facility = LOG_LOCAL7;
	} else {
		spdk_g_log_facility = LOG_DAEMON;
		return -1;
	}
	return 0;
}

int
spdk_set_log_priority(const char *priority)
{
	if (strcasecmp(priority, "emerg") == 0) {
		spdk_g_log_priority = LOG_EMERG;
	} else if (strcasecmp(priority, "alert") == 0) {
		spdk_g_log_priority = LOG_ALERT;
	} else if (strcasecmp(priority, "crit") == 0) {
		spdk_g_log_priority = LOG_CRIT;
	} else if (strcasecmp(priority, "err") == 0) {
		spdk_g_log_priority = LOG_ERR;
	} else if (strcasecmp(priority, "warning") == 0) {
		spdk_g_log_priority = LOG_WARNING;
	} else if (strcasecmp(priority, "notice") == 0) {
		spdk_g_log_priority = LOG_NOTICE;
	} else if (strcasecmp(priority, "info") == 0) {
		spdk_g_log_priority = LOG_INFO;
	} else if (strcasecmp(priority, "debug") == 0) {
		spdk_g_log_priority = LOG_DEBUG;
	} else {
		spdk_g_log_priority = LOG_NOTICE;
		return -1;
	}
	return 0;
}

void
spdk_noticelog(const char *file, const int line, const char *func,
	       const char *format, ...)
{
	char buf[MAX_TMPBUF];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	if (file != NULL) {
		if (func != NULL) {
			if (spdk_g_notice_stderr_flag) {
				fprintf(stderr, "%s:%4d:%s: %s", file, line, func, buf);
			}
			syslog(LOG_NOTICE, "%s:%4d:%s: %s", file, line, func, buf);
		} else {
			if (spdk_g_notice_stderr_flag) {
				fprintf(stderr, "%s:%4d: %s", file, line, buf);
			}
			syslog(LOG_NOTICE, "%s:%4d: %s", file, line, buf);
		}
	} else {
		if (spdk_g_notice_stderr_flag) {
			fprintf(stderr, "%s", buf);
		}
		syslog(LOG_NOTICE, "%s", buf);
	}
	va_end(ap);
}

void
spdk_warnlog(const char *file, const int line, const char *func,
	     const char *format, ...)
{
	char buf[MAX_TMPBUF];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	if (file != NULL) {
		if (func != NULL) {
			fprintf(stderr, "%s:%4d:%s: %s", file, line, func, buf);
			syslog(LOG_WARNING, "%s:%4d:%s: %s",
			       file, line, func, buf);
		} else {
			fprintf(stderr, "%s:%4d: %s", file, line, buf);
			syslog(LOG_WARNING, "%s:%4d: %s", file, line, buf);
		}
	} else {
		fprintf(stderr, "%s", buf);
		syslog(LOG_WARNING, "%s", buf);
	}

	va_end(ap);
}

void
spdk_tracelog(const char *flag, const char *file, const int line, const char *func,
	      const char *format, ...)
{
	char buf[MAX_TMPBUF];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	if (func != NULL) {
		fprintf(stderr, "[%s] %s:%4d:%s: %s", flag, file, line, func, buf);
		//syslog(LOG_INFO, "[%s] %s:%4d:%s: %s", flag, file, line, func, buf);
	} else {
		fprintf(stderr, "[%s] %s:%4d: %s", flag, file, line, buf);
		//syslog(LOG_INFO, "[%s] %s:%4d: %s", flag, file, line, buf);
	}
	va_end(ap);
}

void
spdk_errlog(const char *file, const int line, const char *func,
	    const char *format, ...)
{
	char buf[MAX_TMPBUF];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	if (func != NULL) {
		fprintf(stderr, "%s:%4d:%s: ***ERROR*** %s", file, line, func, buf);
		syslog(LOG_ERR, "%s:%4d:%s: ***ERROR*** %s", file, line, func, buf);
	} else {
		fprintf(stderr, "%s:%4d: ***ERROR*** %s", file, line, buf);
		syslog(LOG_ERR, "%s:%4d: ***ERROR*** %s", file, line, buf);
	}
	va_end(ap);
}

static void
fdump(FILE *fp, const char *label, const uint8_t *buf, size_t len)
{
	char tmpbuf[MAX_TMPBUF];
	char buf16[16 + 1];
	size_t total;
	unsigned int idx;

	fprintf(fp, "%s\n", label);

	memset(buf16, 0, sizeof buf16);
	total = 0;
	for (idx = 0; idx < len; idx++) {
		if (idx != 0 && idx % 16 == 0) {
			snprintf(tmpbuf + total, sizeof tmpbuf - total,
				 " %s", buf16);
			fprintf(fp, "%s\n", tmpbuf);
			total = 0;
		}
		if (idx % 16 == 0) {
			total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
					  "%08x ", idx);
		}
		if (idx % 8 == 0) {
			total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
					  "%s", " ");
		}
		total += snprintf(tmpbuf + total, sizeof tmpbuf - total,
				  "%2.2x ", buf[idx] & 0xff);
		buf16[idx % 16] = isprint(buf[idx]) ? buf[idx] : '.';
	}
	for (; idx % 16 != 0; idx++) {
		total += snprintf(tmpbuf + total, sizeof tmpbuf - total, "   ");
		buf16[idx % 16] = ' ';
	}
	snprintf(tmpbuf + total, sizeof tmpbuf - total, "  %s", buf16);
	fprintf(fp, "%s\n", tmpbuf);
	fflush(fp);
}

void
spdk_trace_dump(const char *label, const uint8_t *buf, size_t len)
{
	fdump(stderr, label, buf, len);
}

static struct spdk_trace_flag *
get_trace_flag(const char *name)
{
	struct spdk_trace_flag *flag;

	TAILQ_FOREACH(flag, &g_trace_flags, tailq) {
		if (strcasecmp(name, flag->name) == 0) {
			return flag;
		}
	}

	return NULL;
}

void
spdk_log_register_trace_flag(const char *name, struct spdk_trace_flag *flag)
{
	struct spdk_trace_flag *iter;

	if (name == NULL || flag == NULL) {
		fprintf(stderr, "missing spdk_trace_flag parameters\n");
		abort();
	}

	if (get_trace_flag(name)) {
		fprintf(stderr, "duplicate spdk_trace_flag '%s'\n", name);
		abort();
	}

	TAILQ_FOREACH(iter, &g_trace_flags, tailq) {
		if (strcasecmp(iter->name, flag->name) > 0) {
			TAILQ_INSERT_BEFORE(iter, flag, tailq);
			return;
		}
	}

	TAILQ_INSERT_TAIL(&g_trace_flags, flag, tailq);
}

bool
spdk_log_get_trace_flag(const char *name)
{
	struct spdk_trace_flag *flag = get_trace_flag(name);

	if (flag && flag->enabled) {
		return true;
	}

	return false;
}

static int
set_trace_flag(const char *name, bool value)
{
	struct spdk_trace_flag *flag;

	if (strcasecmp(name, "all") == 0) {
		TAILQ_FOREACH(flag, &g_trace_flags, tailq) {
			flag->enabled = value;
		}
		return 0;
	}

	flag = get_trace_flag(name);
	if (flag == NULL) {
		return -1;
	}

	flag->enabled = value;

	return 0;
}

int
spdk_log_set_trace_flag(const char *name)
{
	return set_trace_flag(name, true);
}

int
spdk_log_clear_trace_flag(const char *name)
{
	return set_trace_flag(name, false);
}

struct spdk_trace_flag *
spdk_log_get_first_trace_flag(void)
{
	return TAILQ_FIRST(&g_trace_flags);
}

struct spdk_trace_flag *
spdk_log_get_next_trace_flag(struct spdk_trace_flag *flag)
{
	return TAILQ_NEXT(flag, tailq);
}

void
spdk_open_log(void)
{
	if (spdk_g_log_facility != 0) {
		openlog("spdk", LOG_PID, spdk_g_log_facility);
	} else {
		openlog("spdk", LOG_PID, LOG_DAEMON);
	}
}

void
spdk_close_log(void)
{
	closelog();
}

void
spdk_tracelog_usage(FILE *f, const char *trace_arg)
{
#ifdef DEBUG
	struct spdk_trace_flag *flag;

	fprintf(f, " %s flag    enable trace flag (all", trace_arg);

	TAILQ_FOREACH(flag, &g_trace_flags, tailq) {
		fprintf(f, ", %s", flag->name);
	}

	fprintf(f, ")\n");
#else
	fprintf(f, " %s flag    enable trace flag (not supported - must rebuild with CONFIG_DEBUG=y)\n",
		trace_arg);
#endif
}
