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
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"

/** \page block_backend_modules Block Device Backend Modules
 *
 * To implement a backend block device driver, a number of functions
 * dictated by struct spdk_bdev_fn_table must be provided.
 *
 * The module should register itself using SPDK_BDEV_MODULE_REGISTER to
 * define the parameters for the module.
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
	const char *name;

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);

	/**
	 * Notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs.
	 */
	void (*examine)(struct spdk_bdev *bdev);

	/**
	 * Count of bdev inits/examinations in progress. Used by generic bdev
	 * layer and must not be modified by bdev modules.
	 */
	uint32_t action_in_progress;

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
	struct spdk_io_channel *(*get_io_channel)(void *ctx);

	/**
	 * Output driver-specific configuration to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write a name (based on the driver name) followed by a JSON value
	 * (most likely another nested object).
	 */
	int (*dump_config_json)(void *ctx, struct spdk_json_write_ctx *w);

	/** Get spin-time per I/O channel in microseconds.
	 *  Optional - may be NULL.
	 */
	uint64_t (*get_spin_time)(struct spdk_io_channel *ch);
};

/** bdev I/O completion status */
enum spdk_bdev_io_status {
	SPDK_BDEV_IO_STATUS_SCSI_ERROR = -3,
	SPDK_BDEV_IO_STATUS_NVME_ERROR = -2,
	SPDK_BDEV_IO_STATUS_FAILED = -1,
	SPDK_BDEV_IO_STATUS_PENDING = 0,
	SPDK_BDEV_IO_STATUS_SUCCESS = 1,
};

struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char *name;

	/** Unique product name for this kind of block device. */
	char *product_name;

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

	/**
	 * Optimal I/O boundary in blocks, or 0 for no value reported.
	 */
	uint32_t optimal_io_boundary;

	/**
	 * Pointer to the bdev module that registered this bdev.
	 */
	struct spdk_bdev_module_if *module;

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;

	/** Mutex protecting claimed */
	pthread_mutex_t mutex;

	/** The bdev status */
	enum spdk_bdev_status status;

	/** The list of block devices that this block device is built on top of (if any). */
	TAILQ_HEAD(, spdk_bdev) base_bdevs;

	TAILQ_ENTRY(spdk_bdev) base_bdev_link;

	/** The list of virtual block devices built on top of this block device. */
	TAILQ_HEAD(, spdk_bdev) vbdevs;

	TAILQ_ENTRY(spdk_bdev) vbdev_link;

	bool bdev_opened_for_write;

	/**
	 * Pointer to the module that has claimed this bdev for purposes of creating virtual
	 *  bdevs on top of it.  Set to NULL if the bdev has not been claimed.
	 */
	struct spdk_bdev_module_if *claim_module;

	/** List of open descriptors for this block device. */
	TAILQ_HEAD(, spdk_bdev_desc) open_descs;

	TAILQ_ENTRY(spdk_bdev) link;

	/** denotes if a reset is currently in progress on this bdev */
	bool reset_in_progress;

	TAILQ_HEAD(, spdk_bdev_io) queued_resets;
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

			/** Starting offset (in bytes) of the bdev for this I/O. */
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

			/** Starting offset (in bytes) of the bdev for this I/O. */
			uint64_t offset;
		} write;
		struct {
			/** Total size of region to be unmapped. */
			uint64_t len;

			/** Starting offset (in bytes) of the bdev for this I/O. */
			uint64_t offset;
		} unmap;
		struct {
			/** Represents starting offset in bytes of the range to be flushed. */
			uint64_t offset;

			/** Represents the number of bytes to be flushed, starting at offset. */
			uint64_t len;
		} flush;
		struct {
			/* The NVMe command to execute */
			struct spdk_nvme_cmd cmd;

			/* The data buffer to transfer */
			void *buf;

			/* The number of bytes to transfer */
			size_t nbytes;
		} nvme_passthru;
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

	/** Member used for linking child I/Os together. */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** It may be used by modules to put the bdev_io into its own list. */
	TAILQ_ENTRY(spdk_bdev_io) module_link;

	/**
	 * Per I/O context for use by the bdev module.
	 *
	 * Note that vbdev modules may not use this field if modifying a bdev_io and resubmitting
	 * to the next lower bdev.
	 */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

void spdk_bdev_register(struct spdk_bdev *bdev);
void spdk_bdev_unregister(struct spdk_bdev *bdev);

void spdk_vbdev_register(struct spdk_bdev *vbdev, struct spdk_bdev **base_bdevs,
			 int base_bdev_count);
void spdk_vbdev_unregister(struct spdk_bdev *vbdev);

void spdk_bdev_module_examine_done(struct spdk_bdev_module_if *module);
void spdk_bdev_module_init_done(struct spdk_bdev_module_if *module);
int spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module_if *module);
void spdk_bdev_module_release_bdev(struct spdk_bdev *bdev);

void spdk_bdev_poller_start(struct spdk_bdev_poller **ppoller,
			    spdk_bdev_poller_fn fn,
			    void *arg,
			    uint32_t lcore,
			    uint64_t period_microseconds);

void spdk_bdev_poller_stop(struct spdk_bdev_poller **ppoller);

void spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb);
struct spdk_bdev_io *spdk_bdev_get_io(void);
void spdk_bdev_io_resubmit(struct spdk_bdev_io *bdev_io, struct spdk_bdev_desc *new_bdev_desc);
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

static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return (struct spdk_bdev_io *)
	       ((uintptr_t)ctx - offsetof(struct spdk_bdev_io, driver_ctx));
}

#define SPDK_BDEV_MODULE_REGISTER(_name, init_fn, fini_fn, config_fn, ctx_size_fn, examine_fn)	\
	static struct spdk_bdev_module_if _name ## _if = {					\
	.name		= #_name,								\
	.module_init 	= init_fn,								\
	.module_fini	= fini_fn,								\
	.config_text	= config_fn,								\
	.get_ctx_size	= ctx_size_fn,                                				\
	.examine	= examine_fn,								\
	};  											\
	__attribute__((constructor)) static void _name ## _init(void) 				\
	{                                                           				\
	    spdk_bdev_module_list_add(&_name ## _if);                  				\
	}

#define SPDK_GET_BDEV_MODULE(name) (&name ## _if)

/*
 * Set module initialization to be asynchronous. After using this macro, the module
 * initialization has to be explicitly finished by calling spdk_bdev_module_init_done().
 */
#define SPDK_BDEV_MODULE_ASYNC_INIT(name)							\
	__attribute__((constructor)) static void name ## _async_init(void)			\
	{											\
		SPDK_GET_BDEV_MODULE(name)->action_in_progress = 1;				\
	}

/*
 * Modules are not required to use this macro.  It allows modules to reference the module with
 * SPDK_GET_BDEV_MODULE() before it is defined by SPDK_BDEV_MODULE_REGISTER.
 */
#define SPDK_DECLARE_BDEV_MODULE(name)								\
	static struct spdk_bdev_module_if name ## _if;

#endif /* SPDK_INTERNAL_BDEV_H */
