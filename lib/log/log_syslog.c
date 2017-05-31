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
#include "spdk_internal/log_syslog.h"

static int spdk_g_log_facility = LOG_DAEMON;
static unsigned int spdk_g_log_priority = LOG_NOTICE;

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

static void
spdk_log_syslog(enum spdk_log_priority sev, const char *flag, const char *file, const int line,
		const char *func, const char *format, va_list ap)
{
	char buf[MAX_TMPBUF];
	vsnprintf(buf, sizeof buf, format, ap);

	switch (sev) {
	case SPDK_LOG_NOTICE:
	case SPDK_LOG_WARN:		/* warning conditions */
	case SPDK_LOG_INFO: {	/* informational */
		int syslog_sev;
		switch (sev) {
		case SPDK_LOG_NOTICE:
			syslog_sev = LOG_NOTICE;
			break;
		case SPDK_LOG_WARN:		/* warning conditions */
			syslog_sev = LOG_WARNING;
			break;
		case SPDK_LOG_INFO:		/* informational */
		default:
			syslog_sev = LOG_INFO;
			break;

		}
		if (file != NULL) {
			if (func != NULL) {
				if (spdk_g_notice_stderr_flag || syslog_sev != LOG_NOTICE) {
					fprintf(stderr, "%s:%4d:%s: %s", file, line, func, buf);
				}
				if (syslog_sev != LOG_DEBUG) {
					syslog(syslog_sev, "%s:%4d:%s: %s", file, line, func, buf);
				}
			} else {
				if (spdk_g_notice_stderr_flag || syslog_sev != LOG_NOTICE) {
					fprintf(stderr, "%s:%4d: %s", file, line, buf);
				}
				if (syslog_sev != LOG_DEBUG) {
					syslog(syslog_sev, "%s:%4d: %s", file, line, buf);
				}
			}
		} else {
			if (spdk_g_notice_stderr_flag || syslog_sev != LOG_NOTICE) {
				fprintf(stderr, "%s", buf);
			}
			if (syslog_sev != LOG_DEBUG) {
				syslog(syslog_sev, "%s", buf);
			}
		}

	}
	break;
	case SPDK_LOG_ERR:		/* informational */
		if (func != NULL) {
			fprintf(stderr, "%s:%4d:%s: ***ERROR*** %s", file, line, func, buf);
			syslog(LOG_ERR, "%s:%4d:%s: ***ERROR*** %s", file, line, func, buf);
		} else {
			fprintf(stderr, "%s:%4d: ***ERROR*** %s", file, line, buf);
			syslog(LOG_ERR, "%s:%4d: ***ERROR*** %s", file, line, buf);
		}
		break;
	case SPDK_LOG_TRACE:		/* debug-level messages */
		if (func != NULL) {
			fprintf(stderr, "[%s] %s:%4d:%s: %s", flag, file, line, func, buf);
			//syslog(LOG_INFO, "[%s] %s:%4d:%s: %s", flag, file, line, func, buf);
		} else {
			fprintf(stderr, "[%s] %s:%4d: %s", flag, file, line, buf);
			//syslog(LOG_INFO, "[%s] %s:%4d: %s", flag, file, line, buf);
		}
		break;
	}

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

static void
spdk_trace_dump_syslog(const char *label, const uint8_t *buf, size_t len)
{
	fdump(stderr, label, buf, len);
}

static void
spdk_open_log_syslog(void)
{
	if (spdk_g_log_facility != 0) {
		openlog("spdk", LOG_PID, spdk_g_log_facility);
	} else {
		openlog("spdk", LOG_PID, LOG_DAEMON);
	}
}

static void
spdk_close_log_syslog(void)
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

SPDK_LOG_MODULE_REGISTER(spdk_open_log_syslog, spdk_close_log_syslog, spdk_log_syslog,
			 spdk_trace_dump_syslog);
