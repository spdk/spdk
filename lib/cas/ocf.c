
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
#include <ocf/ocf.h>
#include <ocf/ocf_types.h>
#include <ocf/ocf_mngt.h>
#include <execinfo.h>

#include "spdk/env.h"
#include "spdk/ocf.h"
#include "spdk_internal/log.h"

#include "ctx.h"
#include "ocf_env.h"
#include "data.h"

extern ocf_ctx_t opencas_ctx;

static uint32_t opencas_refcnt = 0;

int spdk_ocf_mngt_cache_start(struct spdk_ocf_ctx *ctx)
{
	return ocf_mngt_cache_start(opencas_ctx, &ctx->dev_cache, &ctx->cfg_cache);
}

int spdk_ocf_mngt_cache_stop(struct spdk_ocf_ctx *ctx)
{
	return ocf_mngt_cache_stop(ctx->dev_cache);
}

bool spdk_ocf_cache_is_running(struct spdk_ocf_ctx *ctx)
{
	return ocf_cache_is_running(ctx->dev_cache);
}

bool spdk_ocf_cache_dev_attached(struct spdk_ocf_ctx *ctx)
{
	return (ctx->dev_cache != NULL);
}

int spdk_ocf_mngt_cache_add_core(struct spdk_ocf_ctx *ctx)
{
	return ocf_mngt_cache_add_core(ctx->dev_cache, &ctx->dev_core, &ctx->cfg_core);
}

int spdk_ocf_mngt_cache_remove_core(struct spdk_ocf_ctx *ctx, int id)
{
	return ocf_mngt_cache_remove_core(ctx->dev_cache, id, false);
}

void *spdk_ocf_queue_get_priv(struct ocf_queue *q)
{
	return ocf_queue_get_priv(q);
}

void spdk_ocf_io_put(struct ocf_io *io)
{
	return ocf_io_put(io);
}

void spdk_ocf_io_configure(struct ocf_io *io, uint64_t addr, uint32_t bytes, uint32_t dir,
			   uint32_t class, uint64_t flags)
{
	return ocf_io_configure(io, addr, bytes, dir,
				class, flags);
}

int spdk_ocf_cache_get_queue(struct spdk_ocf_ctx *ctx, unsigned id, struct ocf_queue *q)
{
	return ocf_cache_get_queue(ctx->dev_cache, id, &q);
}

