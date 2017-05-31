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

#ifndef SPDK_INTERNAL_LOG_H
#define SPDK_INTERNAL_LOG_H

#include "spdk/log.h"
#include "spdk/queue.h"

struct spdk_trace_flag {
	TAILQ_ENTRY(spdk_trace_flag) tailq;
	const char *name;
	bool enabled;
};

static const char *const spdk_level_names[] = {
	"ERROR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG",
};

void spdk_log_register_trace_flag(const char *name, struct spdk_trace_flag *flag);

struct spdk_trace_flag *spdk_log_get_first_trace_flag(void);
struct spdk_trace_flag *spdk_log_get_next_trace_flag(struct spdk_trace_flag *flag);

#ifdef DEBUG
#define SPDK_LOG_REGISTER_TRACE_FLAG(str, flag) \
struct spdk_trace_flag flag = { \
	.enabled = false, \
	.name = str, \
}; \
__attribute__((constructor)) static void register_trace_flag_##flag(void) \
{ \
	spdk_log_register_trace_flag(str, &flag); \
}

#define SPDK_TRACELOG(FLAG, ...)								\
	do {											\
		extern struct spdk_trace_flag FLAG;						\
		if (FLAG.enabled) {								\
			spdk_log(SPDK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)

#define SPDK_TRACEDUMP(FLAG, LABEL, BUF, LEN)						\
	do {										\
		extern struct spdk_trace_flag FLAG;					\
		if ((FLAG.enabled) && (LEN)) {						\
			spdk_trace_dump((LABEL), (BUF), (LEN));				\
		}									\
	} while (0)

#else
#define SPDK_LOG_REGISTER_TRACE_FLAG(str, flag)
#define SPDK_TRACELOG(...) do { } while (0)
#define SPDK_TRACEDUMP(...) do { } while (0)
#endif

typedef void (*spdk_log_open_fn)(void);
typedef void (*spdk_log_close_fn)(void);
typedef void (*spdk_log_write_fn)(enum spdk_log_level level, const char *file, const int line,
				  const char *func, const char *buf);
typedef void (*spdk_log_trace_dump_fn)(const char *label, const uint8_t *buf, size_t len);

struct spdk_log_module {
	const char		*name;
	spdk_log_open_fn	open_log;
	spdk_log_close_fn	close_log;
	spdk_log_write_fn	write_log;
	spdk_log_trace_dump_fn	trace_dump;

	TAILQ_ENTRY(spdk_log_module) link;
};

void spdk_log_module_register(struct spdk_log_module *mod);

#define SPDK_LOG_MODULE_REGISTER(name_str, open_fn, close_fn, write_fn, trace_dump_fn)		\
	static struct spdk_log_module open_fn ## _if = {					\
	.name		= name_str,									\
	.open_log 	= open_fn,								\
	.close_log	= close_fn,								\
	.write_log	= write_fn,								\
	.trace_dump	= trace_dump_fn,                                			\
	};  											\
	__attribute__((constructor)) static void open_fn ## _init(void)  			\
	{                                                           				\
	    spdk_log_module_register(&open_fn ## _if);                  			\
	}


#endif /* SPDK_INTERNAL_LOG_H */
