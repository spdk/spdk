/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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
 * Block Device Module Interface
 */

#ifndef SPDK_INTERNAL_BDEV_H
#define SPDK_INTERNAL_BDEV_H

#include <inttypes.h>
#include <unistd.h>
#include <stddef.h>  /* for offsetof */
#include <sys/uio.h> /* for struct iovec */
#include <stdbool.h>

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"

/** \page block_backend_modules Block Device Backend Modules

To implement a backend block device driver, a number of functions
dictated by struct spdk_bdev_fn_table must be provided.

The module should register itself using SPDK_BDEV_MODULE_REGISTER or
SPDK_VBDEV_MODULE_REGISTER to define the parameters for the module.

Use SPDK_BDEV_MODULE_REGISTER for all block backends that are real disks.
Any virtual backends such as RAID, partitioning, etc. should use
SPDK_VBDEV_MODULE_REGISTER.

<hr>

In the module initialization code, the config file sections can be parsed to
acquire custom configuration parameters. For example, if the config file has
a section such as below:
<blockquote><pre>
[MyBE]
  MyParam 1234
</pre></blockquote>

The value can be extracted as the example below:
<blockquote><pre>
struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "MyBe");
int my_param = spdk_conf_section_get_intval(sp, "MyParam");
</pre></blockquote>

The backend initialization routine also need to create "disks". A virtual
representation of each LUN must be constructed. Mainly a struct spdk_bdev
must be passed to the bdev database via spdk_bdev_register().

*/

/** Block device module */
struct spdk_bdev_module_if {
	/**
	 * Initialization function for the module.  Called by the spdk
	 * application during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);

	/**
	 * Finish function for the module.  Called by the spdk application
	 * before the spdk application exits to perform any necessary cleanup.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);

	/**
	 * Function called to return a text string representing the
	 * module's configuration options for inclusion in a configuration file.
	 */
	void (*config_text)(FILE *fp);

	/** Name for the modules being defined. */
	const char *module_name;

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);

	TAILQ_ENTRY(spdk_bdev_module_if) tailq;
};

/**
 * Function table for a block device backend.
 *
 * The backend block device function table provides a set of APIs to allow
 * communication with a backend. The main commands are read/write API
 * calls for I/O via submit_request.
 */
struct spdk_bdev_fn_table {
	/** Destroy the backend block device object */
	int (*destruct)(void *ctx);

	/** Process the IO. */
	void (*submit_request)(struct spdk_bdev_io *);

	/** Check if the block device supports a specific I/O type. */
	bool (*io_type_supported)(void *ctx, enum spdk_bdev_io_type);

	/** Get an I/O channel for the specific bdev for the calling thread. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx, uint32_t priority);

	/**
	 * Output driver-specific configuration to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write a name (based on the driver name) followed by a JSON value
	 * (most likely another nested object).
	 */
	int (*dump_config_json)(void *ctx, struct spdk_json_write_ctx *w);
};

void spdk_bdev_register(struct spdk_bdev *bdev);
void spdk_bdev_io_get_rbuf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_rbuf_cb cb);
struct spdk_bdev_io *spdk_bdev_get_io(void);
struct spdk_bdev_io *spdk_bdev_get_child_io(struct spdk_bdev_io *parent,
		struct spdk_bdev *bdev,
		spdk_bdev_io_completion_cb cb,
		void *cb_arg);
void spdk_bdev_io_resubmit(struct spdk_bdev_io *bdev_io, struct spdk_bdev *new_bdev);
void spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io,
			   enum spdk_bdev_io_status status);

/**
 * Complete a bdev_io with an NVMe status code.
 *
 * \param bdev_io I/O to complete.
 * \param sct NVMe Status Code Type.
 * \param sc NVMe Status Code.
 */
void spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc);

void spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module);
void spdk_vbdev_module_list_add(struct spdk_bdev_module_if *vbdev_module);

static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return (struct spdk_bdev_io *)
	       ((uintptr_t)ctx - offsetof(struct spdk_bdev_io, driver_ctx));
}

#define SPDK_BDEV_MODULE_REGISTER(init_fn, fini_fn, config_fn, ctx_size_fn)			\
	static struct spdk_bdev_module_if init_fn ## _if = {					\
	.module_init 	= init_fn,								\
	.module_fini	= fini_fn,								\
	.config_text	= config_fn,								\
	.get_ctx_size	= ctx_size_fn,                                				\
	};  											\
	__attribute__((constructor)) static void init_fn ## _init(void)  			\
	{                                                           				\
	    spdk_bdev_module_list_add(&init_fn ## _if);                  			\
	}

#define SPDK_VBDEV_MODULE_REGISTER(init_fn, fini_fn, config_fn, ctx_size_fn)			\
	static struct spdk_bdev_module_if init_fn ## _if = {					\
	.module_init 	= init_fn,								\
	.module_fini	= fini_fn,								\
	.config_text	= config_fn,								\
	.get_ctx_size	= ctx_size_fn,                                				\
	};  											\
	__attribute__((constructor)) static void init_fn ## _init(void)  			\
	{                                                           				\
	    spdk_vbdev_module_list_add(&init_fn ## _if);                  			\
	}

#endif /* SPDK_INTERNAL_BDEV_H */
