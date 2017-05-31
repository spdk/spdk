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

#define MAX_TMPBUF 1024

static void
spdk_syslog_open(void)
{
	openlog("spdk", LOG_PID, LOG_LOCAL7);
}

static void
spdk_syslog_close(void)
{
	closelog();
}

static void
spdk_syslog_write(enum spdk_log_level level, const char *file, const int line,
		  const char *func, const char *buf)
{
	int severity;

	switch (level) {
	case SPDK_LOG_NONE:
		return;
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
	}

	syslog(severity, "%s:%4d:%s: *%s*: %s", file, line, func, spdk_level_names[level], buf);
}

static void
spdk_syslog_trace_dump(const char *label, const uint8_t *buf, size_t len)
{
	char tmpbuf[MAX_TMPBUF];
	char buf16[16 + 1];
	size_t total;
	unsigned int idx;

	fprintf(stderr, "%s\n", label);

	memset(buf16, 0, sizeof(buf16));
	total = 0;
	for (idx = 0; idx < len; idx++) {
		if (idx != 0 && idx % 16 == 0) {
			snprintf(tmpbuf + total, sizeof(tmpbuf) - total,
				 " %s", buf16);
			fprintf(stderr, "%s\n", tmpbuf);
			total = 0;
		}
		if (idx % 16 == 0) {
			total += snprintf(tmpbuf + total, sizeof(tmpbuf) - total,
					  "%08x ", idx);
		}
		if (idx % 8 == 0) {
			total += snprintf(tmpbuf + total, sizeof(tmpbuf) - total,
					  "%s", " ");
		}
		total += snprintf(tmpbuf + total, sizeof(tmpbuf) - total,
				  "%2.2x ", buf[idx] & 0xff);
		buf16[idx % 16] = isprint(buf[idx]) ? buf[idx] : '.';
	}
	for (; idx % 16 != 0; idx++) {
		total += snprintf(tmpbuf + total, sizeof(tmpbuf) - total, "   ");
		buf16[idx % 16] = ' ';
	}
	snprintf(tmpbuf + total, sizeof(tmpbuf) - total, "  %s", buf16);
	fprintf(stderr, "%s\n", tmpbuf);
	fflush(stderr);
}

SPDK_LOG_MODULE_REGISTER("syslog", spdk_syslog_open, spdk_syslog_close,
			 spdk_syslog_write, spdk_syslog_trace_dump);
