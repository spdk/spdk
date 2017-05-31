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

#ifndef _SPDK_INTERNAL_TRACE_SHM_H_
#define _SPDK_INTERNAL_TRACE_SHM_H_

#include "spdk/trace.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_trace_histories {
	uint64_t			tsc_rate;
	struct spdk_trace_history	per_lcore_history[SPDK_TRACE_MAX_LCORE];
	struct spdk_trace_owner		owner[UCHAR_MAX + 1];
	struct spdk_trace_object	object[UCHAR_MAX + 1];
	struct spdk_trace_tpoint	tpoint[SPDK_TRACE_MAX_TPOINT_ID];
};

void spdk_trace_shm_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
			   uint64_t object_id, uint64_t arg1);

void spdk_trace_shm_init(const char *shm_name);
void spdk_trace_shm_cleanup(void);
void spdk_trace_shm_register_owner(uint8_t type, char id_prefix);
void spdk_trace_shm_register_object(uint8_t type, char id_prefix);
void spdk_trace_shm_register_description(const char *name, const char *short_name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_is_ptr, uint8_t arg1_is_alias,
		const char *arg1_name);

#ifdef __cplusplus
}
#endif

#endif /* _SPDK_INTERNAL_TRACE_SHM_H_ */
