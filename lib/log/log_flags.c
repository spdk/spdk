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

enum spdk_log_level g_spdk_log_level = SPDK_LOG_NOTICE;
enum spdk_log_level g_spdk_log_print_level = SPDK_LOG_NOTICE;
enum spdk_log_level g_spdk_log_backtrace_level = SPDK_LOG_DISABLED;

SPDK_LOG_REGISTER_COMPONENT("log", SPDK_LOG_LOG)

#define MAX_TMPBUF 1024

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
spdk_log_set_backtrace_level(enum spdk_log_level level)
{
	g_spdk_log_backtrace_level = level;
}

enum spdk_log_level
spdk_log_get_backtrace_level(void) {
	return g_spdk_log_backtrace_level;
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
		assert(false);
		return;
	}

	if (get_trace_flag(name)) {
		SPDK_ERRLOG("duplicate spdk_trace_flag '%s'\n", name);
		assert(false);
		return;
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
spdk_tracelog_usage(FILE *f, const char *trace_arg)
{
#ifdef DEBUG
	struct spdk_trace_flag *flag;
	fprintf(f, " %s, --traceflag <flag>    enable debug log flag (all", trace_arg);

	TAILQ_FOREACH(flag, &g_trace_flags, tailq) {
		fprintf(f, ", %s", flag->name);
	}

	fprintf(f, ")\n");
#else
	fprintf(f, " %s, --traceflag <flag>    enable debug log flag (not supported"
		" - must rebuild with --enable-debug)\n", trace_arg);
#endif
}
