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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"

/** \page block_backend_modules Block Device Backend Modules
 *
 * To implement a backend block device driver, a number of functions
 * dictated by struct spdk_bdev_fn_table must be provided.
 *
 * The module should register itself using SPDK_BDEV_MODULE_REGISTER or
 * SPDK_VBDEV_MODULE_REGISTER to define the parameters for the module.
 *
 * Use SPDK_BDEV_MODULE_REGISTER for all block backends that are real disks.
 * Any virtual backends such as RAID, partitioning, etc. should use
 * SPDK_VBDEV_MODULE_REGISTER.
 *
 * <hr>
 *
 * In the module initialization code, the config file sections can be parsed to
 * acquire custom configuration parameters. For example, if the config file has
 * a section such as below:
 * <blockquote><pre>
 * [MyBE]
 * MyParam 1234
 * </pre></blockquote>
 *
 * The value can be extracted as the example below:
 * <blockquote><pre>
 * struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "MyBe");
 * int my_param = spdk_conf_section_get_intval(sp, "MyParam");
 * </pre></blockquote>
 *
 * The backend initialization routine also need to create "disks". A virtual
 * representation of each LUN must be constructed. Mainly a struct spdk_bdev
 * must be passed to the bdev database via spdk_bdev_register().
 */

#define SPDK_BDEV_MAX_NAME_LENGTH		16
#define SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH	50

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
	void (*submit_request)(struct spdk_io_channel *ch, struct spdk_bdev_io *);

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

struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char name[SPDK_BDEV_MAX_NAME_LENGTH];

	/** Unique product name for this kind of block device. */
	char product_name[SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH];

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;

	/** Number of blocks */
	uint64_t blockcnt;

	/** write cache enabled, not used at the moment */
	int write_cache;

	/**
	 * This is used to make sure buffers are sector aligned.
	 * This causes double buffering on writes.
	 */
	int need_aligned_buffer;

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;

	/** Represents maximum unmap block descriptor count */
	uint32_t max_unmap_bdesc_count;

	/** generation value used by block device reset */
	uint32_t gencnt;

	/** Mutex protecting claimed */
	pthread_mutex_t mutex;

	/** The bdev status */
	enum spdk_bdev_status status;

	/** Remove callback function pointer to upper level stack */
	spdk_bdev_remove_cb_t remove_cb;

	/** Callback context for hot remove the device */
	void *remove_ctx;

	TAILQ_ENTRY(spdk_bdev) link;
};

typedef void (*spdk_bdev_io_get_buf_cb)(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

struct spdk_bdev_io {
	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;

	/** The bdev I/O channel that this was submitted on. */
	struct spdk_bdev_channel *ch;

	/** bdev allocated memory associated with this request */
	void *buf;

	/** Callback for when buf is allocated */
	spdk_bdev_io_get_buf_cb get_buf_cb;

	/** Entry to the list need_buf of struct spdk_bdev. */
	TAILQ_ENTRY(spdk_bdev_io) buf_link;

	/** Generation value for each I/O. */
	uint32_t gencnt;

	/** Enumerated value representing the I/O type. */
	enum spdk_bdev_io_type type;

	union {
		struct {
			/** For basic read case, use our own iovec element. */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** Total size of data to be transferred. */
			size_t len;

			/** Starting offset (in bytes) of the blockdev for this I/O. */
			uint64_t offset;
		} read;
		struct {
			/** For basic write case, use our own iovec element */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** Total size of data to be transferred. */
			size_t len;

			/** Starting offset (in bytes) of the blockdev for this I/O. */
			uint64_t offset;
		} write;
		struct {
			/** Represents the unmap block descriptors. */
			struct spdk_scsi_unmap_bdesc *unmap_bdesc;

			/** Count of unmap block descriptors. */
			uint16_t bdesc_count;
		} unmap;
		struct {
			/** Represents starting offset in bytes of the range to be flushed. */
			uint64_t offset;

			/** Represents the number of bytes to be flushed, starting at offset. */
			uint64_t length;
		} flush;
		struct {
			enum spdk_bdev_reset_type type;
		} reset;
	} u;

	/** Status for the IO */
	enum spdk_bdev_io_status status;

	/** Error information from a device */
	union {
		/** Only valid when status is SPDK_BDEV_IO_STATUS_NVME_ERROR */
		struct {
			/** NVMe status code type */
			int sct;
			/** NVMe status code */
			int sc;
		} nvme;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_SCSI_ERROR */
		struct {
			/** SCSI status code */
			enum spdk_scsi_status sc;
			/** SCSI sense key */
			enum spdk_scsi_sense sk;
			/** SCSI additional sense code */
			uint8_t asc;
			/** SCSI additional sense code qualifier */
			uint8_t ascq;
		} scsi;
	} error;

	/** User function that will be called when this completes */
	spdk_bdev_io_completion_cb cb;

	/** Context that will be passed to the completion callback */
	void *caller_ctx;

	/**
	 * Set to true while the bdev module submit_request function is in progress.
	 *
	 * This is used to decide whether spdk_bdev_io_complete() can complete the I/O directly
	 * or if completion must be deferred via an event.
	 */
	bool in_submit_request;

	/** Used in virtual device (e.g., RAID), indicates its parent spdk_bdev_io */
	struct spdk_bdev_io *parent;

	/** Used in virtual device (e.g., RAID) for storing multiple child device I/Os */
	TAILQ_HEAD(child_io, spdk_bdev_io) child_io;

	/** Member used for linking child I/Os together. */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** Per I/O context for use by the blockdev module */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

void spdk_bdev_register(struct spdk_bdev *bdev);
void spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb);
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

/**
 * Complete a bdev_io with a SCSI status code.
 *
 * \param bdev_io I/O to complete.
 * \param sc SCSI Status Code.
 * \param sk SCSI Sense Key.
 * \param asc SCSI Additional Sense Code.
 * \param ascq SCSI Additional Sense Code Qualifier.
 */
void spdk_bdev_io_complete_scsi_status(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				       enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq);


void spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			      int *sc, int *sk, int *asc, int *ascq);

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
