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

extern enum spdk_log_level g_spdk_log_level;
extern enum spdk_log_level g_spdk_log_print_level;
extern enum spdk_log_level g_spdk_log_backtrace_level;

struct spdk_trace_flag {
	TAILQ_ENTRY(spdk_trace_flag) tailq;
	const char *name;
	bool enabled;
};

void spdk_log_register_trace_flag(const char *name, struct spdk_trace_flag *flag);

struct spdk_trace_flag *spdk_log_get_first_trace_flag(void);
struct spdk_trace_flag *spdk_log_get_next_trace_flag(struct spdk_trace_flag *flag);

#define SPDK_LOG_REGISTER_COMPONENT(str, flag) \
struct spdk_trace_flag flag = { \
	.enabled = false, \
	.name = str, \
}; \
__attribute__((constructor)) static void register_trace_flag_##flag(void) \
{ \
	spdk_log_register_trace_flag(str, &flag); \
}

#define SPDK_INFOLOG(FLAG, ...)									\
	do {											\
		extern struct spdk_trace_flag FLAG;						\
		if (FLAG.enabled) {								\
			spdk_log(SPDK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)

#ifdef DEBUG

#define SPDK_DEBUGLOG(FLAG, ...)								\
	do {											\
		extern struct spdk_trace_flag FLAG;						\
		if (FLAG.enabled) {								\
			spdk_log(SPDK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__);	\
		}										\
	} while (0)

#define SPDK_TRACEDUMP(FLAG, LABEL, BUF, LEN)						\
	do {										\
		extern struct spdk_trace_flag FLAG;					\
		if ((FLAG.enabled) && (LEN)) {						\
			spdk_trace_dump(stderr, (LABEL), (BUF), (LEN));			\
		}									\
	} while (0)

#else
#define SPDK_DEBUGLOG(...) do { } while (0)
#define SPDK_TRACEDUMP(...) do { } while (0)
#endif

#endif /* SPDK_INTERNAL_LOG_H */
