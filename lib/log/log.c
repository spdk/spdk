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

static TAILQ_HEAD(, spdk_trace_flag) g_trace_flags = TAILQ_HEAD_INITIALIZER(g_trace_flags);

static enum spdk_log_level g_spdk_log_level = SPDK_LOG_NOTICE;
static enum spdk_log_level g_spdk_log_print_level = SPDK_LOG_NOTICE;

SPDK_LOG_REGISTER_TRACE_FLAG("log", SPDK_TRACE_LOG)

#define MAX_TMPBUF 1024

static const char *const spdk_level_names[] = {
	[SPDK_LOG_ERROR]	= "ERROR",
	[SPDK_LOG_WARN]		= "WARNING",
	[SPDK_LOG_NOTICE]	= "NOTICE",
	[SPDK_LOG_INFO]		= "INFO",
	[SPDK_LOG_DEBUG]	= "DEBUG",
};

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

void
spdk_log_set_level(enum spdk_log_level level)
{
	g_spdk_log_level = level;
}

enum spdk_log_level
spdk_log_get_level(void) {
	return g_spdk_log_level;
}

void
spdk_log_set_print_level(enum spdk_log_level level)
{
	g_spdk_log_print_level = level;
}

enum spdk_log_level
spdk_log_get_print_level(void) {
	return g_spdk_log_print_level;
}

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
	}

	va_start(ap, format);

	vsnprintf(buf, sizeof(buf), format, ap);

	if (level <= g_spdk_log_print_level) {
		fprintf(stderr, "%s:%4d:%s: *%s*: %s", file, line, func, spdk_level_names[level], buf);
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
		SPDK_ERRLOG("missing spdk_trace_flag parameters\n");
		abort();
	}

	if (get_trace_flag(name)) {
		SPDK_ERRLOG("duplicate spdk_trace_flag '%s'\n", name);
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
  int ret = 0;
  char *tok = NULL;
  char *copy_name = strdup(name);

  if (copy_name == NULL)
    return -1;

  tok = strtok(copy_name, ",");
  while (tok != NULL) {
    ret = set_trace_flag(tok, true);
    if (ret < 0) {
      free(copy_name);
      return ret;
    }
    tok = strtok(NULL, ",");
  }
  free(copy_name);
  return ret;
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
