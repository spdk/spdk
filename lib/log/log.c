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

struct spdk_trace_flag_head g_trace_flags = TAILQ_HEAD_INITIALIZER(g_trace_flags);
struct spdk_log_env g_log_env;

unsigned int spdk_g_notice_stderr_flag = 1;

SPDK_LOG_REGISTER_TRACE_FLAG("debug", SPDK_TRACE_DEBUG)

void
spdk_noticelog(const char *file, const int line, const char *func,
	       const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	g_log_env.log_fn(SPDK_LOG_NOTICE, NULL, file, line, func, format, ap);
	va_end(ap);
}

void
spdk_warnlog(const char *file, const int line, const char *func,
	     const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	g_log_env.log_fn(SPDK_LOG_WARN, NULL, file, line, func, format, ap);
	va_end(ap);
}

void
spdk_tracelog(const char *flag, const char *file, const int line, const char *func,
	      const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	g_log_env.log_fn(SPDK_LOG_NOTICE, flag, file, line, func, format, ap);
	va_end(ap);
}

void
spdk_errlog(const char *file, const int line, const char *func,
	    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	g_log_env.log_fn(SPDK_LOG_ERR, NULL, file, line, func, format, ap);
	va_end(ap);
}

void
spdk_trace_dump(const char *label, const uint8_t *buf, size_t len)
{
	g_log_env.trace_dump(label, buf, len);
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
	g_log_env.open_log();
}

void
spdk_close_log(void)
{
	g_log_env.close_log();
}

void spdk_log_configure_env(spdk_open_log_fn open_fn, spdk_close_log_fn close_fn,
			    spdk_log_fn log_fn, spdk_trace_dump_fn dump_fn)
{
	g_log_env.open_log = open_fn;
	g_log_env.close_log = close_fn;
	g_log_env.log_fn = log_fn;
	g_log_env.trace_dump = dump_fn;
}
