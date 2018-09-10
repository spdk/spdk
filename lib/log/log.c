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

#include "spdk/stdinc.h"

#include "spdk_internal/log.h"

#ifdef SPDK_LOG_BACKTRACE_LVL
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

static const char *const spdk_level_names[] = {
	[SPDK_LOG_ERROR]	= "ERROR",
	[SPDK_LOG_WARN]		= "WARNING",
	[SPDK_LOG_NOTICE]	= "NOTICE",
	[SPDK_LOG_INFO]		= "INFO",
	[SPDK_LOG_DEBUG]	= "DEBUG",
};

#define MAX_TMPBUF 1024

void
spdk_log_open(void)
{
	openlog("spdk", LOG_PID, LOG_LOCAL7);
}

void
spdk_log_close(void)
{
	closelog();
}

#ifdef SPDK_LOG_BACKTRACE_LVL
static void
spdk_log_unwind_stack(FILE *fp, enum spdk_log_level level)
{
	unw_error_t err;
	unw_cursor_t cursor;
	unw_context_t uc;
	unw_word_t ip;
	unw_word_t offp;
	char f_name[64];
	int frame;

	if (level > g_spdk_log_backtrace_level) {
		return;
	}

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
	fprintf(fp, "*%s*: === BACKTRACE START ===\n", spdk_level_names[level]);

	unw_step(&cursor);
	for (frame = 1; unw_step(&cursor) > 0; frame++) {
		unw_get_reg(&cursor, UNW_REG_IP, &ip);
		err = unw_get_proc_name(&cursor, f_name, sizeof(f_name), &offp);
		if (err || strcmp(f_name, "main") == 0) {
			break;
		}

		fprintf(fp, "*%s*: %3d: %*s%s() at %#lx\n", spdk_level_names[level], frame, frame - 1, "", f_name,
			(unsigned long)ip);
	}
	fprintf(fp, "*%s*: === BACKTRACE END ===\n", spdk_level_names[level]);
}

#else
#define spdk_log_unwind_stack(fp, lvl)
#endif

void
spdk_log(enum spdk_log_level level, const char *file, const int line, const char *func,
	 const char *format, ...)
{
	int severity = LOG_INFO;
	char buf[MAX_TMPBUF];
	va_list ap;

	switch (level) {
	case SPDK_LOG_ERROR:
		severity = LOG_ERR;
		break;
	case SPDK_LOG_WARN:
		severity = LOG_WARNING;
		break;
	case SPDK_LOG_NOTICE:
		severity = LOG_NOTICE;
		break;
	case SPDK_LOG_INFO:
	case SPDK_LOG_DEBUG:
		severity = LOG_INFO;
		break;
	case SPDK_LOG_DISABLED:
		return;
	}

	va_start(ap, format);

	vsnprintf(buf, sizeof(buf), format, ap);

	if (level <= g_spdk_log_print_level) {
		fprintf(stderr, "%s:%4d:%s: *%s*: %s", file, line, func, spdk_level_names[level], buf);
		spdk_log_unwind_stack(stderr, level);
	}

	if (level <= g_spdk_log_level) {
		syslog(severity, "%s:%4d:%s: *%s*: %s", file, line, func, spdk_level_names[level], buf);
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
spdk_trace_dump(FILE *fp, const char *label, const void *buf, size_t len)
{
	fdump(fp, label, buf, len);
}
