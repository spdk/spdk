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
#ifndef OPENCAS_STATS_H
#define OPENCAS_STATS_H

#include <ocf/ocf_types.h>
#include <ocf/ocf_stats_builder.h>
#include <ocf/ocf_stats.h>

struct cache_stats {
	struct ocf_stats_usage usage;
	struct ocf_stats_requests reqs;
	struct ocf_stats_blocks blocks;
	struct ocf_stats_errors errors;
};

typedef void (*cache_get_stats_callback_t)(const char *stats_text, void *context);
typedef void (*cache_get_stats_fn_t)(void *stat_structure,
				     cache_get_stats_callback_t callback, void *ctx);

void cache_stats_write_reqs(struct ocf_stats_requests *, cache_get_stats_callback_t,
			    void  *context);
void cache_stats_write_usage(struct ocf_stats_usage *, cache_get_stats_callback_t,
			     void  *context);
void cache_stats_write_blocks(struct ocf_stats_blocks *, cache_get_stats_callback_t,
			      void  *context);
void cache_stats_write_errors(struct ocf_stats_errors *, cache_get_stats_callback_t,
			      void  *context);

int cache_get_stats(int cache_id, int core_id, struct cache_stats *stats);

#endif
