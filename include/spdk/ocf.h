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

/** \file
 * SPDK Open CAS Framework
 */

#ifndef SPDK_OCF_H
#define SPDK_OCF_H

#include <ocf/ocf.h>
#include <ocf/ocf_types.h>
#include <ocf/ocf_mngt.h>

#include "ctx.h"
#include "data.h"
#include "dobj.h"
#include "utils.h"

/*
 * OCF cache configuration options
 */
struct spdk_ocf_ctx {
	/* Initial cache configuration  */
	struct ocf_mngt_cache_config        cfg_cache;

	/* Cache device config */
	struct ocf_mngt_cache_device_config cfg_device;

	/* Core initial config */
	struct ocf_mngt_core_config         cfg_core;

	/* Base bdevs OCF objects */
	ocf_cache_t                  dev_cache;
	ocf_core_t                   dev_core;
};


/**
 * \brief Start management engine
 * \param data Pointer to ocf data
 */
int spdk_ocf_mngt_cache_start(struct spdk_ocf_ctx *ctx);

/**
 * \brief Stop management engine
 * \param data Pointer to ocf data
 */
int spdk_ocf_mngt_cache_stop(struct spdk_ocf_ctx *ctx);

/**
 * \brief Check if cache device exists
 * \param data Pointer to ocf data
 */
bool spdk_ocf_cache_dev_attached(struct spdk_ocf_ctx *ctx);

/**
 * \brief Check if ocf engine is running
 * \param data Pointer to ocf data
 */
bool spdk_ocf_cache_is_running(struct spdk_ocf_ctx *ctx);

/**
 * \brief Add core to ocf engine
 * \param data Pointer to ocf data
 */
int spdk_ocf_mngt_cache_add_core(struct spdk_ocf_ctx *ctx);

/**
 * \brief Remove core from ocf engine
 * \param data Pointer to ocf data
 */
int spdk_ocf_mngt_cache_remove_core(struct spdk_ocf_ctx *ctx, int id);

void *spdk_ocf_queue_get_priv(struct ocf_queue *q);

void spdk_ocf_io_put(struct ocf_io *io);

void spdk_ocf_io_configure(struct ocf_io *io, uint64_t addr, uint32_t bytes, uint32_t dir,
			   uint32_t class, uint64_t flags);

int spdk_ocf_cache_get_queue(struct spdk_ocf_ctx *ctx, unsigned id, struct ocf_queue *q);

#endif
