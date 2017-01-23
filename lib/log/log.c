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
int spdk_g_log_facility = LOG_DAEMON;
unsigned int spdk_g_log_priority = LOG_NOTICE;

SPDK_LOG_REGISTER_TRACE_FLAG("debug", SPDK_TRACE_DEBUG)

#define MAX_TMPBUF 1024

struct syslog_code {
	const char *c_name;
	int c_val;
};

static const struct syslog_code facilitynames[] = {
	{ "auth",	LOG_AUTH,	},
	{ "authpriv",	LOG_AUTHPRIV,	},
	{ "cron",	LOG_CRON,	},
	{ "daemon",	LOG_DAEMON,	},
	{ "ftp",	LOG_FTP,	},
	{ "kern",	LOG_KERN,	},
	{ "lpr",	LOG_LPR,	},
	{ "mail",	LOG_MAIL,	},
	{ "news",	LOG_NEWS,	},
	{ "syslog",	LOG_SYSLOG,	},
	{ "user",	LOG_USER,	},
	{ "uucp",	LOG_UUCP,	},
	{ "local0",	LOG_LOCAL0,	},
	{ "local1",	LOG_LOCAL1,	},
	{ "local2",	LOG_LOCAL2,	},
	{ "local3",	LOG_LOCAL3,	},
	{ "local4",	LOG_LOCAL4,	},
	{ "local5",	LOG_LOCAL5,	},
	{ "local6",	LOG_LOCAL6,	},
	{ "local7",	LOG_LOCAL7,	},
#ifdef __FreeBSD__
	{ "console",	LOG_CONSOLE,	},
	{ "ntp",	LOG_NTP,	},
	{ "security",	LOG_SECURITY,	},
#endif
	{ NULL,		-1,		}
};

static const struct syslog_code prioritynames[] = {
	{ "alert",	LOG_ALERT,	},
	{ "crit",	LOG_CRIT,	},
	{ "debug",	LOG_DEBUG,	},
	{ "emerg",	LOG_EMERG,	},
	{ "err",	LOG_ERR,	},
	{ "info",	LOG_INFO,	},
	{ "notice",	LOG_NOTICE,	},
	{ "warning",	LOG_WARNING,	},
	{ NULL,		-1,		}
};

int
spdk_set_log_facility(const char *facility)
{
	int i;

	for (i = 0; facilitynames[i].c_name != NULL; i++) {
		if (strcasecmp(facilitynames[i].c_name, facility) == 0) {
			spdk_g_log_facility = facilitynames[i].c_val;
			return 0;
		}
	}

	spdk_g_log_facility = LOG_DAEMON;
	return -1;
}

const char *
spdk_get_log_facility(void)
{
	const char *def_name = NULL;
	int i;

	for (i = 0; facilitynames[i].c_name != NULL; i++) {
		if (facilitynames[i].c_val == spdk_g_log_facility) {
			return facilitynames[i].c_name;
		} else if (facilitynames[i].c_val == LOG_DAEMON) {
			def_name = facilitynames[i].c_name;
		}
	}

	return def_name;
}

int
spdk_set_log_priority(const char *priority)
{
	int i;

	for (i = 0; prioritynames[i].c_name != NULL; i++) {
		if (strcasecmp(prioritynames[i].c_name, priority) == 0) {
			spdk_g_log_priority = prioritynames[i].c_val;
			return 0;
		}
	}

	spdk_g_log_priority = LOG_NOTICE;
	return -1;
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
