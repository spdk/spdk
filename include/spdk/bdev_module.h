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
 *
 * For information on how to write a bdev module, see @ref bdev_module.
 */

#ifndef SPDK_BDEV_MODULE_H
#define SPDK_BDEV_MODULE_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"
#include "spdk/thread.h"
#include "spdk/uuid.h"

/** Block device module */
struct spdk_bdev_module {
	/**
	 * Initialization function for the module.  Called by the spdk
	 * application during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);

	/**
	 * Optional callback for modules that require notification of when
	 * the bdev subsystem has completed initialization.
	 *
	 * Modules are not required to define this function.
	 */
	void (*init_complete)(void);

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

	/**
	 * Function called to return a text string representing the
	 * module's configuration options for inclusion in a configuration file.
	 *
	 * \return 0 on success or Bdev specific negative error code.
	 */
	int (*config_json)(struct spdk_json_write_ctx *w);

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
	 *
	 * \note Used internally by bdev subsystem, don't change this value in bdev module.
	 */
	uint32_t action_in_progress;

	/**
	 * Denotes if the module_init function may complete asynchronously. If set to true,
	 * the module initialization has to be explicitly completed by calling
	 * spdk_bdev_module_init_done().
	 */
	bool async_init;

	/**
	 * Denotes if the module_fini function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_done().
	 */
	bool async_fini;

	TAILQ_ENTRY(spdk_bdev_module) tailq;
};

typedef void (*spdk_bdev_unregister_cb)(void *cb_arg, int rc);

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
	 * Output driver-specific information to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write a name (based on the driver name) followed by a JSON value
	 * (most likely another nested object).
	 */
	int (*dump_info_json)(void *ctx, struct spdk_json_write_ctx *w);

	/**
	 * Output bdev-specific configuration to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the bdev
	 * driver should write all data necessary to recreate this bdev by invoking
	 * constructor method. No other data should be written.
	 */
	void (*write_config_json)(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);

	/** Get spin-time per I/O channel in microseconds.
	 *  Optional - may be NULL.
	 */
	uint64_t (*get_spin_time)(struct spdk_io_channel *ch);
};

/** bdev I/O completion status */
enum spdk_bdev_io_status {
	/*
	 * NOMEM should be returned when a bdev module cannot start an I/O because of
	 *  some lack of resources.  It may not be returned for RESET I/O.  I/O completed
	 *  with NOMEM status will be retried after some I/O from the same channel have
	 *  completed.
	 */
	SPDK_BDEV_IO_STATUS_NOMEM = -4,
	SPDK_BDEV_IO_STATUS_SCSI_ERROR = -3,
	SPDK_BDEV_IO_STATUS_NVME_ERROR = -2,
	SPDK_BDEV_IO_STATUS_FAILED = -1,
	SPDK_BDEV_IO_STATUS_PENDING = 0,
	SPDK_BDEV_IO_STATUS_SUCCESS = 1,
};

struct spdk_bdev_alias {
	char *alias;
	TAILQ_ENTRY(spdk_bdev_alias) tailq;
};

typedef TAILQ_HEAD(, spdk_bdev_io) bdev_io_tailq_t;
typedef STAILQ_HEAD(, spdk_bdev_io) bdev_io_stailq_t;

struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char *name;

	/** Unique aliases for this block device. */
	TAILQ_HEAD(spdk_bdev_aliases_list, spdk_bdev_alias) aliases;

	/** Unique product name for this kind of block device. */
	char *product_name;

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;

	/** Number of blocks */
	uint64_t blockcnt;

	/** Number of active channels on this bdev except the QoS bdev channel */
	uint32_t channel_count;

	/** Quality of service parameters */
	struct spdk_bdev_qos *qos;

	/** True if the state of the QoS is being modified */
	bool qos_mod_in_progress;

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
	 * UUID for this bdev.
	 *
	 * Fill with zeroes if no uuid is available.
	 */
	struct spdk_uuid uuid;

	/**
	 * Pointer to the bdev module that registered this bdev.
	 */
	struct spdk_bdev_module *module;

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;

	/** Mutex protecting claimed */
	pthread_mutex_t mutex;

	/** The bdev status */
	enum spdk_bdev_status status;

	/** The array of block devices that this block device is built on top of (if any). */
	struct spdk_bdev **base_bdevs;
	size_t base_bdevs_cnt;


	/** The array of virtual block devices built on top of this block device. */
	struct spdk_bdev **vbdevs;
	size_t vbdevs_cnt;

	/**
	 * Pointer to the module that has claimed this bdev for purposes of creating virtual
	 *  bdevs on top of it.  Set to NULL if the bdev has not been claimed.
	 */
	struct spdk_bdev_module *claim_module;

	/** Callback function that will be called after bdev destruct is completed. */
	spdk_bdev_unregister_cb	unregister_cb;

	/** Unregister call context */
	void *unregister_ctx;

	/** List of open descriptors for this block device. */
	TAILQ_HEAD(, spdk_bdev_desc) open_descs;

	TAILQ_ENTRY(spdk_bdev) link;

	/** points to a reset bdev_io if one is in progress. */
	struct spdk_bdev_io *reset_in_progress;
};

typedef void (*spdk_bdev_io_get_buf_cb)(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

struct spdk_bdev_io {
	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;

	/** Error information from a device */
	union {
		/** Only valid when status is SPDK_BDEV_IO_STATUS_NVME_ERROR */
		struct {
			/** NVMe status code type */
			uint8_t sct;
			/** NVMe status code */
			uint8_t sc;
		} nvme;
		/** Only valid when status is SPDK_BDEV_IO_STATUS_SCSI_ERROR */
		struct {
			/** SCSI status code */
			uint8_t sc;
			/** SCSI sense key */
			uint8_t sk;
			/** SCSI additional sense code */
			uint8_t asc;
			/** SCSI additional sense code qualifier */
			uint8_t ascq;
		} scsi;
	} error;

	/** Enumerated value representing the I/O type. */
	uint8_t type;

	union {
		struct {
			/** For basic IO case, use our own iovec element. */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** Total size of data to be transferred. */
			uint64_t num_blocks;

			/** Starting offset (in blocks) of the bdev for this I/O. */
			uint64_t offset_blocks;

			/** stored user callback in case we split the I/O and use a temporary callback */
			spdk_bdev_io_completion_cb stored_user_cb;

			/** number of blocks remaining in a split i/o */
			uint64_t split_remaining_num_blocks;

			/** current offset of the split I/O in the bdev */
			uint64_t split_current_offset_blocks;
		} bdev;
		struct {
			/** Channel reference held while messages for this reset are in progress. */
			struct spdk_io_channel *ch_ref;
		} reset;
		struct {
			/* The NVMe command to execute */
			struct spdk_nvme_cmd cmd;

			/* The data buffer to transfer */
			void *buf;

			/* The number of bytes to transfer */
			size_t nbytes;

			/* The meta data buffer to transfer */
			void *md_buf;

			/* meta data buffer size to transfer */
			size_t md_len;
		} nvme_passthru;
	} u;

	/** Member used for linking child I/Os together. */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** It may be used by modules to put the bdev_io into its own list. */
	TAILQ_ENTRY(spdk_bdev_io) module_link;

	/** Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_io_internal_fields {
		/** The bdev I/O channel that this was handled on. */
		struct spdk_bdev_channel *ch;

		/** The bdev I/O channel that this was submitted on. */
		struct spdk_bdev_channel *io_submit_ch;

		/** User function that will be called when this completes */
		spdk_bdev_io_completion_cb cb;

		/** Context that will be passed to the completion callback */
		void *caller_ctx;

		/** Current tsc at submit time. Used to calculate latency at completion. */
		uint64_t submit_tsc;

		/**
		 * Set to true while the bdev module submit_request function is in progress.
		 *
		 * This is used to decide whether spdk_bdev_io_complete() can complete the I/O directly
		 * or if completion must be deferred via an event.
		 */
		bool in_submit_request;

		/** Status for the IO */
		int8_t status;

		/** bdev allocated memory associated with this request */
		void *buf;

		/** requested size of the buffer associated with this I/O */
		uint64_t buf_len;

		/** Callback for when buf is allocated */
		spdk_bdev_io_get_buf_cb get_buf_cb;

		/** Entry to the list need_buf of struct spdk_bdev. */
		STAILQ_ENTRY(spdk_bdev_io) buf_link;
	} internal;

	/**
	 * Per I/O context for use by the bdev module.
	 */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

int spdk_bdev_register(struct spdk_bdev *bdev);
void spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg);
void spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno);
int spdk_vbdev_register(struct spdk_bdev *vbdev, struct spdk_bdev **base_bdevs,
			int base_bdev_count);

void spdk_bdev_module_examine_done(struct spdk_bdev_module *module);
void spdk_bdev_module_init_done(struct spdk_bdev_module *module);
void spdk_bdev_module_finish_done(void);
int spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module *module);
void spdk_bdev_module_release_bdev(struct spdk_bdev *bdev);

/**
 * Add alias to block device names list.
 * Aliases can be add only to registered bdev.
 *
 * \param bdev Block device to query.
 * \param alias Alias to be added to list.
 *
 * Return codes
 * \return 0 on success
 * \return -EEXIST if alias already exists as name or alias on any bdev
 * \return -ENOMEM if memory cannot be allocated to store alias
 * \return -EINVAL if passed alias is empty
 */
int spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias);

/**
 * Removes name from block device names list.
 *
 * \param bdev Block device to query.
 * \param alias Alias to be deleted from list.
 * \return 0 on success
 * \return -ENOENT if alias does not exists
 */
int spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias);

/**
 * Get pointer to block device aliases list.
 *
 * \param bdev Block device to query.
 * \return Pointer to bdev aliases list.
 */
const struct spdk_bdev_aliases_list *spdk_bdev_get_aliases(const struct spdk_bdev *bdev);

/**
 * Allocate a buffer for given bdev_io.  Allocation will happen
 * only if the bdev_io has no assigned SGL yet. The buffer will be
 * freed automatically on \c spdk_bdev_free_io() call. This call
 * will never fail - in case of lack of memory given callback \c cb
 * will be deferred until enough memory is freed.
 *
 * \param bdev_io I/O to allocate buffer for.
 * \param cb callback to be called when the buffer is allocated
 * or the bdev_io has an SGL assigned already.
 * \param len size of the buffer to allocate. In case the bdev_io
 * doesn't have an SGL assigned this field must be no bigger than
 * \c SPDK_BDEV_LARGE_BUF_MAX_SIZE.
 */
void spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len);

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

/**
 * Get a thread that given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return thread that submitted the I/O
 */
struct spdk_thread *spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io);

/**
 * Resize for a bdev.
 *
 * Change number of blocks for provided block device.
 * It can only be called on a registered bdev.
 *
 * \param bdev Block device to change.
 * \param size New size of bdev.
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size);

void spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			      int *sc, int *sk, int *asc, int *ascq);

void spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module);

/**
 * Find registered module with name pointed by \c name.
 *
 * \param name name of module to be searched for.
 * \return pointer to module or NULL if no module with \c name exist
 */
struct spdk_bdev_module *spdk_bdev_module_list_find(const char *name);

static inline struct spdk_bdev_io *
spdk_bdev_io_from_ctx(void *ctx)
{
	return (struct spdk_bdev_io *)
	       ((uintptr_t)ctx - offsetof(struct spdk_bdev_io, driver_ctx));
}

struct spdk_bdev_part_base;

typedef void (*spdk_bdev_part_base_free_fn)(struct spdk_bdev_part_base *base);

struct spdk_bdev_part_base {
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*desc;
	uint32_t			ref;
	uint32_t			channel_size;
	spdk_bdev_part_base_free_fn	base_free_fn;
	bool				claimed;
	struct spdk_bdev_module		*module;
	struct spdk_bdev_fn_table	*fn_table;
	struct bdev_part_tailq		*tailq;
	spdk_io_channel_create_cb	ch_create_cb;
	spdk_io_channel_destroy_cb	ch_destroy_cb;
};

struct spdk_bdev_part {
	struct spdk_bdev		bdev;
	struct spdk_bdev_part_base	*base;
	uint64_t			offset_blocks;
	TAILQ_ENTRY(spdk_bdev_part)	tailq;
};

struct spdk_bdev_part_channel {
	struct spdk_bdev_part		*part;
	struct spdk_io_channel		*base_ch;
};

typedef TAILQ_HEAD(bdev_part_tailq, spdk_bdev_part)	SPDK_BDEV_PART_TAILQ;

void spdk_bdev_part_base_free(struct spdk_bdev_part_base *base);
int spdk_bdev_part_free(struct spdk_bdev_part *part);
void spdk_bdev_part_base_hotremove(struct spdk_bdev *base_bdev, struct bdev_part_tailq *tailq);
int spdk_bdev_part_base_construct(struct spdk_bdev_part_base *base, struct spdk_bdev *bdev,
				  spdk_bdev_remove_cb_t remove_cb,
				  struct spdk_bdev_module *module,
				  struct spdk_bdev_fn_table *fn_table,
				  struct bdev_part_tailq *tailq,
				  spdk_bdev_part_base_free_fn free_fn,
				  uint32_t channel_size,
				  spdk_io_channel_create_cb ch_create_cb,
				  spdk_io_channel_destroy_cb ch_destroy_cb);
int spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			     char *name, uint64_t offset_blocks, uint64_t num_blocks,
			     char *product_name);
void spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io);


/*
 * Macro used to register module for later initialization.
 */
#define SPDK_BDEV_MODULE_REGISTER(_module)							\
	__attribute__((constructor)) static void						\
	SPDK_BDEV_MODULE_REGISTER_FN_NAME(__LINE__)  (void)					\
	{											\
	    spdk_bdev_module_list_add(_module);							\
	}

/*
 * This is helper macro for automatic function generation.
 *
 */
#define SPDK_BDEV_MODULE_REGISTER_FN_NAME(line) SPDK_BDEV_MODULE_REGISTER_FN_NAME_(line)

/*
 *  Second helper macro for "stringize" trick to work.
 */
#define SPDK_BDEV_MODULE_REGISTER_FN_NAME_(line) spdk_bdev_module_register_ ## line

#endif /* SPDK_BDEV_MODULE_H */
