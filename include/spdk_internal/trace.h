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
 * Tracepoint library
 */

#ifndef _SPDK_INTERNAL_TRACE_H_
#define _SPDK_INTERNAL_TRACE_H_

#include <spdk/stdinc.h>

typedef void (*spdk_trace_init_fn)(const char *shm_name);
typedef void (*spdk_trace_cleanup_fn)(void);
typedef void (*spdk_trace_record_fn)(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
				     uint64_t object_id, uint64_t arg1);
typedef void (*spdk_trace_register_owner_fn)(uint8_t type, char id_prefix);
typedef void (*spdk_trace_register_object_fn)(uint8_t type, char id_prefix);
typedef void (*spdk_trace_register_description_fn)(const char *name, const char *short_name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_is_ptr, uint8_t arg1_is_alias,
		const char *arg1_name);


struct spdk_trace_env {
	spdk_trace_init_fn init_trace;
	spdk_trace_cleanup_fn cleanup_trace;
	spdk_trace_record_fn record_trace;
	spdk_trace_register_owner_fn register_owner;
	spdk_trace_register_object_fn register_object;
	spdk_trace_register_description_fn register_description;
};

extern struct spdk_trace_env g_trace_env;
void spdk_trace_configure_env(struct spdk_trace_env *env);

#define SPDK_TRACE_MODULE_REGISTER(init_fn, cleanup_fn, record_fn, register_owner_fn,	\
				    register_object_fn, register_description_fn)	\
	static struct spdk_trace_env init_fn ## _if = {					\
		.init_trace 	= init_fn,						\
		.cleanup_trace	= cleanup_fn,						\
		.record_trace	= record_fn,						\
		.register_owner	= register_owner_fn,                                	\
		.register_object	= register_object_fn,				\
		.register_description	= register_description_fn,			\
};  											\
__attribute__((constructor)) static void init_fn ## _env(void)				\
{											\
    spdk_trace_configure_env(&init_fn ## _if);						\
}


#endif /* _SPDK_INTERNAL_TRACE_H_ */
