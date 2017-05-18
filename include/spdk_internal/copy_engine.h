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

#ifndef SPDK_INTERNAL_COPY_ENGINE_H
#define SPDK_INTERNAL_COPY_ENGINE_H

#include "spdk/stdinc.h"

#include "spdk/copy_engine.h"
#include "spdk/queue.h"

struct spdk_copy_task {
	spdk_copy_completion_cb	cb;
	uint8_t			offload_ctx[0];
};

struct spdk_copy_engine {
	int64_t	(*copy)(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src,
			uint64_t nbytes, spdk_copy_completion_cb cb);
	int64_t	(*fill)(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill,
			uint64_t nbytes, spdk_copy_completion_cb cb);
	struct spdk_io_channel *(*get_io_channel)(void);
};

struct spdk_copy_module_if {
	/** Initialization function for the module.  Called by the spdk
	 *   application during startup.
	 *
	 *  Modules are required to define this function.
	 */
	int	(*module_init)(void);

	/** Finish function for the module.  Called by the spdk application
	 *   before the spdk application exits to perform any necessary cleanup.
	 *
	 *  Modules are not required to define this function.
	 */
	void	(*module_fini)(void);

	/** Function called to return a text string representing the
	 *   module's configuration options for inclusion in an
	 *   spdk configuration file.
	 */
	void	(*config_text)(FILE *fp);

	size_t	(*get_ctx_size)(void);
	TAILQ_ENTRY(spdk_copy_module_if)	tailq;
};

void spdk_copy_engine_register(struct spdk_copy_engine *copy_engine);
void spdk_copy_module_list_add(struct spdk_copy_module_if *copy_module);

#define SPDK_COPY_MODULE_REGISTER(init_fn, fini_fn, config_fn, ctx_size_fn)				\
	static struct spdk_copy_module_if init_fn ## _if = {						\
	.module_init 	= init_fn,									\
	.module_fini	= fini_fn,									\
	.config_text	= config_fn,									\
	.get_ctx_size	= ctx_size_fn,                                					\
	};  												\
	__attribute__((constructor)) static void init_fn ## _init(void)  				\
	{                                                           					\
	    spdk_copy_module_list_add(&init_fn ## _if);                  				\
	}

#endif
