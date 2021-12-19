/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include "spdk/bdev_zone.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"
#include "spdk/thread.h"
#include "spdk/tree.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Block device module */
struct spdk_bdev_module {
	/**
	 * Initialization function for the module. Called by the bdev library
	 * during startup.
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
	 * Optional callback for modules that require notification of when
	 * the bdev subsystem is starting the fini process. Called by
	 * the bdev library before starting to unregister the bdevs.
	 *
	 * If a module claimed a bdev without presenting virtual bdevs on top of it,
	 * it has to release that claim during this call.
	 *
	 * Modules are not required to define this function.
	 */
	void (*fini_start)(void);

	/**
	 * Finish function for the module. Called by the bdev library
	 * after all bdevs for all modules have been unregistered.  This allows
	 * the module to do any final cleanup before the bdev library finishes operation.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);

	/**
	 * Function called to return a text string representing the module-level
	 * JSON RPCs required to regenerate the current configuration.  This will
	 * include module-level configuration options, or methods to construct
	 * bdevs when one RPC may generate multiple bdevs (for example, an NVMe
	 * controller with multiple namespaces).
	 *
	 * Per-bdev JSON RPCs (where one "construct" RPC always creates one bdev)
	 * may be implemented here, or by the bdev's write_config_json function -
	 * but not both.  Bdev module implementers may choose which mechanism to
	 * use based on the module's design.
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
	 * First notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs, but no I/O to device can be send to bdev at this point.
	 * Only vbdevs based on config files can be created here. This callback must make
	 * its decision to claim the module synchronously.
	 * It must also call spdk_bdev_module_examine_done() before returning. If the module
	 * needs to perform asynchronous operations such as I/O after claiming the bdev,
	 * it may define an examine_disk callback.  The examine_disk callback will then
	 * be called immediately after the examine_config callback returns.
	 */
	void (*examine_config)(struct spdk_bdev *bdev);

	/**
	 * Second notification that a bdev should be examined by a virtual bdev module.
	 * Virtual bdev modules may use this to examine newly-added bdevs and automatically
	 * create their own vbdevs. This callback may use I/O operations and finish asynchronously.
	 * Once complete spdk_bdev_module_examine_done() must be called.
	 */
	void (*examine_disk)(struct spdk_bdev *bdev);

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

	/**
	 * Denotes if the fini_start function may complete asynchronously.
	 * If set to true finishing has to be explicitly completed by calling
	 * spdk_bdev_module_fini_start_done().
	 */
	bool async_fini_start;

	/**
	 * Fields that are used by the internal bdev subsystem. Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_module_internal_fields {
		/**
		 * Count of bdev inits/examinations in progress. Used by generic bdev
		 * layer and must not be modified by bdev modules.
		 *
		 * \note Used internally by bdev subsystem, don't change this value in bdev module.
		 */
		uint32_t action_in_progress;

		TAILQ_ENTRY(spdk_bdev_module) tailq;
	} internal;
};

/**
 * Called by a bdev module to lay exclusive write claim to a bdev.
 *
 * Also upgrades that bdev's descriptor to have write access.
 *
 * \param bdev Block device to be claimed.
 * \param desc Descriptor for the above block device.
 * \param module Bdev module attempting to claim bdev.
 *
 * \return 0 on success
 * \return -EPERM if the bdev is already claimed by another module.
 */
int spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				struct spdk_bdev_module *module);

/**
 * Called to release a write claim on a block device.
 *
 * \param bdev Block device to be released.
 */
void spdk_bdev_module_release_bdev(struct spdk_bdev *bdev);

/* Libraries may define __SPDK_BDEV_MODULE_ONLY so that they include
 * only the struct spdk_bdev_module definition, and the relevant APIs
 * to claim/release a bdev. This may be useful in some cases to avoid
 * abidiff errors related to including the struct spdk_bdev structure
 * unnecessarily.
 */
#ifndef __SPDK_BDEV_MODULE_ONLY

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
	 * Output bdev-specific RPC configuration to a JSON stream. Optional - may be NULL.
	 *
	 * This function should only be implemented for bdevs which can be configured
	 * independently of other bdevs.  For example, RPCs to create a bdev for an NVMe
	 * namespace may not be generated by this function, since enumerating an NVMe
	 * namespace requires attaching to an NVMe controller, and that controller may
	 * contain multiple namespaces.  The spdk_bdev_module's config_json function should
	 * be used instead for these cases.
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

	/** Get bdev module context. */
	void *(*get_module_ctx)(void *ctx);

	/** Get memory domains used by bdev. Optional - may be NULL.
	 * Vbdev module implementation should call \ref spdk_bdev_get_memory_domains for underlying bdev.
	 * Vbdev module must inspect types of memory domains returned by base bdev and report only those
	 * memory domains that it can work with. */
	int (*get_memory_domains)(void *ctx, struct spdk_memory_domain **domains, int array_size);
};

/** bdev I/O completion status */
enum spdk_bdev_io_status {
	SPDK_BDEV_IO_STATUS_AIO_ERROR = -8,
	SPDK_BDEV_IO_STATUS_ABORTED = -7,
	SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED = -6,
	SPDK_BDEV_IO_STATUS_MISCOMPARE = -5,
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

struct spdk_bdev_name {
	char *name;
	struct spdk_bdev *bdev;
	RB_ENTRY(spdk_bdev_name) node;
};

struct spdk_bdev_alias {
	struct spdk_bdev_name alias;
	TAILQ_ENTRY(spdk_bdev_alias) tailq;
};

typedef TAILQ_HEAD(, spdk_bdev_io) bdev_io_tailq_t;
typedef STAILQ_HEAD(, spdk_bdev_io) bdev_io_stailq_t;
typedef TAILQ_HEAD(, lba_range) lba_range_tailq_t;

struct spdk_bdev {
	/** User context passed in by the backend */
	void *ctxt;

	/** Unique name for this block device. */
	char *name;

	/** Unique aliases for this block device. */
	TAILQ_HEAD(spdk_bdev_aliases_list, spdk_bdev_alias) aliases;

	/** Unique product name for this kind of block device. */
	char *product_name;

	/** write cache enabled, not used at the moment */
	int write_cache;

	/** Size in bytes of a logical block for the backend */
	uint32_t blocklen;

	/** Size in bytes of a physical block for the backend */
	uint32_t phys_blocklen;

	/** Number of blocks */
	uint64_t blockcnt;

	/** Number of blocks required for write */
	uint32_t write_unit_size;

	/** Atomic compare & write unit */
	uint16_t acwu;

	/**
	 * Specifies an alignment requirement for data buffers associated with an spdk_bdev_io.
	 * 0 = no alignment requirement
	 * >0 = alignment requirement is 2 ^ required_alignment.
	 * bdev layer will automatically double buffer any spdk_bdev_io that violates this
	 * alignment, before the spdk_bdev_io is submitted to the bdev module.
	 */
	uint8_t required_alignment;

	/**
	 * Specifies whether the optimal_io_boundary is mandatory or
	 * only advisory.  If set to true, the bdev layer will split
	 * READ and WRITE I/O that span the optimal_io_boundary before
	 * submitting them to the bdev module.
	 *
	 * Note that this field cannot be used to force splitting of
	 * UNMAP, WRITE_ZEROES or FLUSH I/O.
	 */
	bool split_on_optimal_io_boundary;

	/**
	 * Optimal I/O boundary in blocks, or 0 for no value reported.
	 */
	uint32_t optimal_io_boundary;

	/**
	 * Max io size in bytes of a single segment
	 *
	 * Note: both max_segment_size and max_num_segments
	 * should be zero or non-zero.
	 */
	uint32_t max_segment_size;

	/* Maximum number of segments in a I/O */
	uint32_t max_num_segments;

	/* Maximum unmap in unit of logical block */
	uint32_t max_unmap;

	/* Maximum unmap block segments */
	uint32_t max_unmap_segments;

	/* Maximum write zeroes in unit of logical block */
	uint32_t max_write_zeroes;

	/**
	 * UUID for this bdev.
	 *
	 * Fill with zeroes if no uuid is available. The bdev layer
	 * will automatically populate this if necessary.
	 */
	struct spdk_uuid uuid;

	/** Size in bytes of a metadata for the backend */
	uint32_t md_len;

	/**
	 * Specify metadata location and set to true if metadata is interleaved
	 * with block data or false if metadata is separated with block data.
	 *
	 * Note that this field is valid only if there is metadata.
	 */
	bool md_interleave;

	/**
	 * DIF type for this bdev.
	 *
	 * Note that this field is valid only if there is metadata.
	 */
	enum spdk_dif_type dif_type;

	/*
	 * DIF location.
	 *
	 * Set to true if DIF is set in the first 8 bytes of metadata or false
	 * if DIF is set in the last 8 bytes of metadata.
	 *
	 * Note that this field is valid only if DIF is enabled.
	 */
	bool dif_is_head_of_md;

	/**
	 * Specify whether each DIF check type is enabled.
	 */
	uint32_t dif_check_flags;

	/**
	 * Specify whether bdev is zoned device.
	 */
	bool zoned;

	/**
	 * Default size of each zone (in blocks).
	 */
	uint64_t zone_size;

	/**
	 * Maximum zone append data transfer size (in blocks).
	 */
	uint32_t max_zone_append_size;

	/**
	 * Maximum number of open zones.
	 */
	uint32_t max_open_zones;

	/**
	 * Maximum number of active zones.
	 */
	uint32_t max_active_zones;

	/**
	 * Optimal number of open zones.
	 */
	uint32_t optimal_open_zones;

	/**
	 * Specifies whether bdev supports media management events.
	 */
	bool media_events;

	/**
	 * Pointer to the bdev module that registered this bdev.
	 */
	struct spdk_bdev_module *module;

	/** function table for all LUN ops */
	const struct spdk_bdev_fn_table *fn_table;

	/** Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_internal_fields {
		/** Quality of service parameters */
		struct spdk_bdev_qos *qos;

		/** True if the state of the QoS is being modified */
		bool qos_mod_in_progress;

		/** Mutex protecting claimed */
		pthread_mutex_t mutex;

		/** The bdev status */
		enum spdk_bdev_status status;

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

		/** poller for tracking the queue_depth of a device, NULL if not tracking */
		struct spdk_poller *qd_poller;

		/** period at which we poll for queue depth information */
		uint64_t period;

		/** used to aggregate queue depth while iterating across the bdev's open channels */
		uint64_t temporary_queue_depth;

		/** queue depth as calculated the last time the telemetry poller checked. */
		uint64_t measured_queue_depth;

		/** most recent value of ticks spent performing I/O. Used to calculate the weighted time doing I/O */
		uint64_t io_time;

		/** weighted time performing I/O. Equal to measured_queue_depth * period */
		uint64_t weighted_io_time;

		/** accumulated I/O statistics for previously deleted channels of this bdev */
		struct spdk_bdev_io_stat stat;

		/** histogram enabled on this bdev */
		bool	histogram_enabled;
		bool	histogram_in_progress;

		/** Currently locked ranges for this bdev.  Used to populate new channels. */
		lba_range_tailq_t locked_ranges;

		/** Pending locked ranges for this bdev.  These ranges are not currently
		 *  locked due to overlapping with another locked range.
		 */
		lba_range_tailq_t pending_locked_ranges;

		/** Bdev name used for quick lookup */
		struct spdk_bdev_name bdev_name;
	} internal;
};

/**
 * Callback when buffer is allocated for the bdev I/O.
 *
 * \param ch The I/O channel the bdev I/O was handled on.
 * \param bdev_io The bdev I/O
 * \param success True if buffer is allocated successfully or the bdev I/O has an SGL
 * assigned already, or false if it failed. The possible reason of failure is the size
 * of the buffer to allocate is greater than the permitted maximum.
 */
typedef void (*spdk_bdev_io_get_buf_cb)(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
					bool success);

/**
 * Callback when an auxiliary buffer is allocated for the bdev I/O.
 *
 * \param ch The I/O channel the bdev I/O was handled on.
 * \param bdev_io The bdev I/O
 * \param aux_buf Pointer to the allocated buffer.  NULL if there was a failure such as
 * the size of the buffer to allocate is greater than the permitted maximum.
 */
typedef void (*spdk_bdev_io_get_aux_buf_cb)(struct spdk_io_channel *ch,
		struct spdk_bdev_io *bdev_io, void *aux_buf);

#define BDEV_IO_NUM_CHILD_IOV 32

struct spdk_bdev_io {
	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;

	/** Enumerated value representing the I/O type. */
	uint8_t type;

	/** Number of IO submission retries */
	uint16_t num_retries;

	/** A single iovec element for use by this bdev_io. */
	struct iovec iov;

	/** Array of iovecs used for I/O splitting. */
	struct iovec child_iov[BDEV_IO_NUM_CHILD_IOV];

	union {
		struct {
			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** For fused operations such as COMPARE_AND_WRITE, array of iovecs
			 *  for the second operation.
			 */
			struct iovec *fused_iovs;

			/** Number of iovecs in fused_iovs. */
			int fused_iovcnt;

			/* Metadata buffer */
			void *md_buf;

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

			/** count of outstanding batched split I/Os */
			uint32_t split_outstanding;

			struct {
				/** Whether the buffer should be populated with the real data */
				uint8_t populate : 1;

				/** Whether the buffer should be committed back to disk */
				uint8_t commit : 1;

				/** True if this request is in the 'start' phase of zcopy. False if in 'end'. */
				uint8_t start : 1;
			} zcopy;

			struct {
				/** The callback argument for the outstanding request which this abort
				 *  attempts to cancel.
				 */
				void *bio_cb_arg;
			} abort;
		} bdev;
		struct {
			/** Channel reference held while messages for this reset are in progress. */
			struct spdk_io_channel *ch_ref;
		} reset;
		struct {
			/** The outstanding request matching bio_cb_arg which this abort attempts to cancel. */
			struct spdk_bdev_io *bio_to_abort;
		} abort;
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
		struct {
			/* First logical block of a zone */
			uint64_t zone_id;

			/* Number of zones */
			uint32_t num_zones;

			/* Used to change zoned device zone state */
			enum spdk_bdev_zone_action zone_action;

			/* The data buffer */
			void *buf;
		} zone_mgmt;
	} u;

	/** It may be used by modules to put the bdev_io into its own list. */
	TAILQ_ENTRY(spdk_bdev_io) module_link;

	/**
	 *  Fields that are used internally by the bdev subsystem.  Bdev modules
	 *  must not read or write to these fields.
	 */
	struct __bdev_io_internal_fields {
		/** The bdev I/O channel that this was handled on. */
		struct spdk_bdev_channel *ch;

		/** The bdev I/O channel that this was submitted on. */
		struct spdk_bdev_channel *io_submit_ch;

		/** The bdev descriptor that was used when submitting this I/O. */
		struct spdk_bdev_desc *desc;

		/** User function that will be called when this completes */
		spdk_bdev_io_completion_cb cb;

		/** Context that will be passed to the completion callback */
		void *caller_ctx;

		/** Current tsc at submit time. Used to calculate latency at completion. */
		uint64_t submit_tsc;

		/** Error information from a device */
		union {
			struct {
				/** NVMe completion queue entry DW0 */
				uint32_t cdw0;
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
			/** Only valid when status is SPDK_BDEV_IO_STATUS_AIO_ERROR */
			int aio_result;
		} error;

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

		/** if the request is double buffered, store original request iovs here */
		struct iovec  bounce_iov;
		struct iovec *orig_iovs;
		int           orig_iovcnt;
		void	     *orig_md_buf;

		/** Callback for when the aux buf is allocated */
		spdk_bdev_io_get_aux_buf_cb get_aux_buf_cb;

		/** Callback for when buf is allocated */
		spdk_bdev_io_get_buf_cb get_buf_cb;

		/** Member used for linking child I/Os together. */
		TAILQ_ENTRY(spdk_bdev_io) link;

		/** Entry to the list need_buf of struct spdk_bdev. */
		STAILQ_ENTRY(spdk_bdev_io) buf_link;

		/** Entry to the list io_submitted of struct spdk_bdev_channel */
		TAILQ_ENTRY(spdk_bdev_io) ch_link;

		/** Enables queuing parent I/O when no bdev_ios available for split children. */
		struct spdk_bdev_io_wait_entry waitq_entry;

		/** Pointer to a structure passed by the user in ext API */
		struct spdk_bdev_ext_io_opts *ext_opts;
	} internal;

	/**
	 * Per I/O context for use by the bdev module.
	 */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

/**
 * Register a new bdev.
 *
 * \param bdev Block device to register.
 *
 * \return 0 on success.
 * \return -EINVAL if the bdev name is NULL.
 * \return -EEXIST if a bdev or bdev alias with the same name already exists.
 */
int spdk_bdev_register(struct spdk_bdev *bdev);

/**
 * Start unregistering a bdev. This will notify each currently open descriptor
 * on this bdev about the hotremoval in hopes that the upper layers will stop
 * using this bdev and manually close all the descriptors with spdk_bdev_close().
 * The actual bdev unregistration may be deferred until all descriptors are closed.
 *
 * \param bdev Block device to unregister.
 * \param cb_fn Callback function to be called when the unregister is complete.
 * \param cb_arg Argument to be supplied to cb_fn
 */
void spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Invokes the unregister callback of a bdev backing a virtual bdev.
 *
 * A Bdev with an asynchronous destruct path should return 1 from its
 * destruct function and call this function at the conclusion of that path.
 * Bdevs with synchronous destruct paths should return 0 from their destruct
 * path.
 *
 * \param bdev Block device that was destroyed.
 * \param bdeverrno Error code returned from bdev's destruct callback.
 */
void spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno);

/**
 * Indicate to the bdev layer that the module is done examining a bdev.
 *
 * To be called during examine_config function or asynchronously in response to the
 * module's examine_disk function being called.
 *
 * \param module Pointer to the module completing the examination.
 */
void spdk_bdev_module_examine_done(struct spdk_bdev_module *module);

/**
 * Indicate to the bdev layer that the module is done initializing.
 *
 * To be called once during module_init or asynchronously after
 * an asynchronous operation required for module initialization is completed.
 *
 * \param module Pointer to the module completing the initialization.
 */
void spdk_bdev_module_init_done(struct spdk_bdev_module *module);

/**
 * Indicate that the module finish has completed.
 *
 * To be called in response to the module_fini, only if async_fini is set.
 *
 */
void spdk_bdev_module_fini_done(void);
void spdk_bdev_module_finish_done(void); /* Deprecated */

/**
 * Indicate that the module fini start has completed.
 *
 * To be called in response to the fini_start, only if async_fini_start is set.
 * May be called during fini_start or asynchronously.
 *
 */
void spdk_bdev_module_fini_start_done(void);

/**
 * Add alias to block device names list.
 * Aliases can be add only to registered bdev.
 *
 * \param bdev Block device to query.
 * \param alias Alias to be added to list.
 *
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
 * Removes all alias from block device alias list.
 *
 * \param bdev Block device to operate.
 */
void spdk_bdev_alias_del_all(struct spdk_bdev *bdev);

/**
 * Get pointer to block device aliases list.
 *
 * \param bdev Block device to query.
 * \return Pointer to bdev aliases list.
 */
const struct spdk_bdev_aliases_list *spdk_bdev_get_aliases(const struct spdk_bdev *bdev);

/**
 * Allocate a buffer for given bdev_io.  Allocation will happen
 * only if the bdev_io has no assigned SGL yet or SGL is not
 * aligned to \c bdev->required_alignment.  If SGL is not aligned,
 * this call will cause copy from SGL to bounce buffer on write
 * path or copy from bounce buffer to SGL before completion
 * callback on read path.  The buffer will be freed automatically
 * on \c spdk_bdev_free_io() call. This call will never fail.
 * In case of lack of memory given callback \c cb will be deferred
 * until enough memory is freed.
 *
 * \param bdev_io I/O to allocate buffer for.
 * \param cb callback to be called when the buffer is allocated
 * or the bdev_io has an SGL assigned already.
 * \param len size of the buffer to allocate. In case the bdev_io
 * doesn't have an SGL assigned this field must be no bigger than
 * \c SPDK_BDEV_LARGE_BUF_MAX_SIZE.
 */
void spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len);

/**
 * Allocate an auxillary buffer for given bdev_io. The length of the
 * buffer will be the same size as the bdev_io primary buffer. The buffer
 * must be freed using \c spdk_bdev_io_put_aux_buf() before completing
 * the associated bdev_io.  This call will never fail. In case of lack of
 * memory given callback \c cb will be deferred until enough memory is freed.
 *
 * \param bdev_io I/O to allocate buffer for.
 * \param cb callback to be called when the buffer is allocated
 */
void spdk_bdev_io_get_aux_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_aux_buf_cb cb);

/**
 * Free an auxiliary buffer previously allocated by \c spdk_bdev_io_get_aux_buf().
 *
 * \param bdev_io bdev_io specified when the aux_buf was allocated.
 * \param aux_buf auxiliary buffer to free
 */
void spdk_bdev_io_put_aux_buf(struct spdk_bdev_io *bdev_io, void *aux_buf);

/**
 * Set the given buffer as the data buffer described by this bdev_io.
 *
 * The portion of the buffer used may be adjusted for memory alignement
 * purposes.
 *
 * \param bdev_io I/O to set the buffer on.
 * \param buf The buffer to set as the active data buffer.
 * \param len The length of the buffer.
 *
 */
void spdk_bdev_io_set_buf(struct spdk_bdev_io *bdev_io, void *buf, size_t len);

/**
 * Set the given buffer as metadata buffer described by this bdev_io.
 *
 * \param bdev_io I/O to set the buffer on.
 * \param md_buf The buffer to set as the active metadata buffer.
 * \param len The length of the metadata buffer.
 */
void spdk_bdev_io_set_md_buf(struct spdk_bdev_io *bdev_io, void *md_buf, size_t len);

/**
 * Complete a bdev_io
 *
 * \param bdev_io I/O to complete.
 * \param status The I/O completion status.
 */
void spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io,
			   enum spdk_bdev_io_status status);

/**
 * Complete a bdev_io with an NVMe status code and DW0 completion queue entry
 *
 * \param bdev_io I/O to complete.
 * \param cdw0 NVMe Completion Queue DW0 value (set to 0 if not applicable)
 * \param sct NVMe Status Code Type.
 * \param sc NVMe Status Code.
 */
void spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct,
				       int sc);

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
 * Complete a bdev_io with AIO errno.
 *
 * \param bdev_io I/O to complete.
 * \param aio_result Negative errno returned from AIO.
 */
void spdk_bdev_io_complete_aio_status(struct spdk_bdev_io *bdev_io, int aio_result);

/**
 * Get a thread that given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return thread that submitted the I/O
 */
struct spdk_thread *spdk_bdev_io_get_thread(struct spdk_bdev_io *bdev_io);

/**
 * Get the bdev module's I/O channel that the given bdev_io was submitted on.
 *
 * \param bdev_io I/O
 * \return the bdev module's I/O channel that the given bdev_io was submitted on.
 */
struct spdk_io_channel *spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io);

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

/**
 * Translates NVMe status codes to SCSI status information.
 *
 * The codes are stored in the user supplied integers.
 *
 * \param bdev_io I/O containing status codes to translate.
 * \param sc SCSI Status Code will be stored here.
 * \param sk SCSI Sense Key will be stored here.
 * \param asc SCSI Additional Sense Code will be stored here.
 * \param ascq SCSI Additional Sense Code Qualifier will be stored here.
 */
void spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
			      int *sc, int *sk, int *asc, int *ascq);

/**
 * Add the given module to the list of registered modules.
 * This function should be invoked by referencing the macro
 * SPDK_BDEV_MODULE_REGISTER in the module c file.
 *
 * \param bdev_module Module to be added.
 */
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
	return SPDK_CONTAINEROF(ctx, struct spdk_bdev_io, driver_ctx);
}

struct spdk_bdev_part_base;

/**
 * Returns a pointer to the spdk_bdev associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev struct.
 */
struct spdk_bdev *spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base);

/**
 * Returns a spdk_bdev name of the corresponding spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A text string representing the name of the base bdev.
 */
const char *spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the spdk_bdev_descriptor associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the base's spdk_bdev_desc struct.
 */
struct spdk_bdev_desc *spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the tailq associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return The head of a tailq of spdk_bdev_part structs registered to the base's module.
 */
struct bdev_part_tailq *spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base);

/**
 * Returns a pointer to the module level context associated with an spdk_bdev_part_base
 *
 * \param part_base A pointer to an spdk_bdev_part_base object.
 *
 * \return A pointer to the module level context registered with the base in spdk_bdev_part_base_construct.
 */
void *spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base);

typedef void (*spdk_bdev_part_base_free_fn)(void *ctx);

struct spdk_bdev_part {
	/* Entry into the module's global list of bdev parts */
	TAILQ_ENTRY(spdk_bdev_part)	tailq;

	/**
	 * Fields that are used internally by part.c These fields should only
	 * be accessed from a module using any pertinent get and set methods.
	 */
	struct bdev_part_internal_fields {

		/* This part's corresponding bdev object. Not to be confused with the base bdev */
		struct spdk_bdev		bdev;

		/* The base to which this part belongs */
		struct spdk_bdev_part_base	*base;

		/* number of blocks from the start of the base bdev to the start of this part */
		uint64_t			offset_blocks;
	} internal;
};

struct spdk_bdev_part_channel {
	struct spdk_bdev_part		*part;
	struct spdk_io_channel		*base_ch;
};

typedef TAILQ_HEAD(bdev_part_tailq, spdk_bdev_part)	SPDK_BDEV_PART_TAILQ;

/**
 * Free the base corresponding to one or more spdk_bdev_part.
 *
 * \param base The base to free.
 */
void spdk_bdev_part_base_free(struct spdk_bdev_part_base *base);

/**
 * Free an spdk_bdev_part context.
 *
 * \param part The part to free.
 *
 * \return 1 always. To indicate that the operation is asynchronous.
 */
int spdk_bdev_part_free(struct spdk_bdev_part *part);

/**
 * Calls spdk_bdev_unregister on the bdev for each part associated with base_bdev.
 *
 * \param part_base The part base object built on top of an spdk_bdev
 * \param tailq The list of spdk_bdev_part bdevs associated with this base bdev.
 */
void spdk_bdev_part_base_hotremove(struct spdk_bdev_part_base *part_base,
				   struct bdev_part_tailq *tailq);

/**
 * Construct a new spdk_bdev_part_base on top of the provided bdev.
 *
 * \param bdev_name Name of the bdev upon which this base will be built.
 * \param remove_cb Function to be called upon hotremove of the bdev.
 * \param module The module to which this bdev base belongs.
 * \param fn_table Function table for communicating with the bdev backend.
 * \param tailq The head of the list of all spdk_bdev_part structures registered to this base's module.
 * \param free_fn User provided function to free base related context upon bdev removal or shutdown.
 * \param ctx Module specific context for this bdev part base.
 * \param channel_size Channel size in bytes.
 * \param ch_create_cb Called after a new channel is allocated.
 * \param ch_destroy_cb Called upon channel deletion.
 * \param base output parameter for the part object when operation is successful.
 *
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_part_base_construct_ext(const char *bdev_name,
				      spdk_bdev_remove_cb_t remove_cb,
				      struct spdk_bdev_module *module,
				      struct spdk_bdev_fn_table *fn_table,
				      struct bdev_part_tailq *tailq,
				      spdk_bdev_part_base_free_fn free_fn,
				      void *ctx,
				      uint32_t channel_size,
				      spdk_io_channel_create_cb ch_create_cb,
				      spdk_io_channel_destroy_cb ch_destroy_cb,
				      struct spdk_bdev_part_base **base);

/**
 * Create a logical spdk_bdev_part on top of a base.
 *
 * \param part The part object allocated by the user.
 * \param base The base from which to create the part.
 * \param name The name of the new spdk_bdev_part.
 * \param offset_blocks The offset into the base bdev at which this part begins.
 * \param num_blocks The number of blocks that this part will span.
 * \param product_name Unique name for this type of block device.
 *
 * \return 0 on success.
 * \return -1 if the bases underlying bdev cannot be claimed by the current module.
 */
int spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			     char *name, uint64_t offset_blocks, uint64_t num_blocks,
			     char *product_name);

/**
 * Forwards I/O from an spdk_bdev_part to the underlying base bdev.
 *
 * This function will apply the offset_blocks the user provided to
 * spdk_bdev_part_construct to the I/O. The user should not manually
 * apply this offset before submitting any I/O through this function.
 *
 * \param ch The I/O channel associated with the spdk_bdev_part.
 * \param bdev_io The I/O to be submitted to the underlying bdev.
 * \return 0 on success or non-zero if submit request failed.
 */
int spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io);

/**
 * Return a pointer to this part's spdk_bdev.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to this part's spdk_bdev object.
 */
struct spdk_bdev *spdk_bdev_part_get_bdev(struct spdk_bdev_part *part);

/**
 * Return a pointer to this part's base.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to this part's spdk_bdev_part_base object.
 */
struct spdk_bdev_part_base *spdk_bdev_part_get_base(struct spdk_bdev_part *part);

/**
 * Return a pointer to this part's base bdev.
 *
 * The return value of this function is equivalent to calling
 * spdk_bdev_part_base_get_bdev on this part's base.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return A pointer to the bdev belonging to this part's base.
 */
struct spdk_bdev *spdk_bdev_part_get_base_bdev(struct spdk_bdev_part *part);

/**
 * Return this part's offset from the beginning of the base bdev.
 *
 * This function should not be called in the I/O path. Any block
 * translations to I/O will be handled in spdk_bdev_part_submit_request.
 *
 * \param part An spdk_bdev_part object.
 *
 * \return the block offset of this part from it's underlying bdev.
 */
uint64_t spdk_bdev_part_get_offset_blocks(struct spdk_bdev_part *part);

/**
 * Push media management events.  To send the notification that new events are
 * available, spdk_bdev_notify_media_management needs to be called.
 *
 * \param bdev Block device
 * \param events Array of media events
 * \param num_events Size of the events array
 *
 * \return number of events pushed or negative errno in case of failure
 */
int spdk_bdev_push_media_events(struct spdk_bdev *bdev, const struct spdk_bdev_media_event *events,
				size_t num_events);

/**
 * Send SPDK_BDEV_EVENT_MEDIA_MANAGEMENT to all open descriptors that have
 * pending media events.
 *
 * \param bdev Block device
 */
void spdk_bdev_notify_media_management(struct spdk_bdev *bdev);

/*
 *  Macro used to register module for later initialization.
 */
#define SPDK_BDEV_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_bdev_module_register_##name(void) \
{ \
	spdk_bdev_module_list_add(module); \
} \

#endif /* __SPDK_BDEV_MODULE_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_MODULE_H */
