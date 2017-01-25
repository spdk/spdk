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
 * Block device abstraction layer
 */

#ifndef SPDK_BDEV_H_
#define SPDK_BDEV_H_

#include <inttypes.h>
#include <stddef.h>  /* for offsetof */
#include <sys/uio.h> /* for struct iovec */
#include <stdbool.h>
#include <pthread.h>

#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/scsi_spec.h"

#define SPDK_BDEV_SMALL_RBUF_MAX_SIZE 8192
#define SPDK_BDEV_LARGE_RBUF_MAX_SIZE (64 * 1024)

#define SPDK_BDEV_MAX_NAME_LENGTH		16
#define SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH	50

typedef void (*spdk_bdev_remove_cb_t)(void *remove_ctx);

struct spdk_bdev_io;
struct spdk_bdev_fn_table;
struct spdk_json_write_ctx;

/** Blockdev status */
enum spdk_bdev_status {
	SPDK_BDEV_STATUS_INVALID,
	SPDK_BDEV_STATUS_UNCLAIMED,
	SPDK_BDEV_STATUS_CLAIMED,
	SPDK_BDEV_STATUS_REMOVING,
};

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */
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

	/** thin provisioning, not used at the moment */
	int thin_provisioning;

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

/** Blockdev I/O type */
enum spdk_bdev_io_type {
	SPDK_BDEV_IO_TYPE_INVALID,
	SPDK_BDEV_IO_TYPE_READ,
	SPDK_BDEV_IO_TYPE_WRITE,
	SPDK_BDEV_IO_TYPE_UNMAP,
	SPDK_BDEV_IO_TYPE_FLUSH,
	SPDK_BDEV_IO_TYPE_RESET,
};

/** Blockdev I/O completion status */
enum spdk_bdev_io_status {
	SPDK_BDEV_IO_STATUS_SCSI_ERROR = -3,
	SPDK_BDEV_IO_STATUS_NVME_ERROR = -2,
	SPDK_BDEV_IO_STATUS_FAILED = -1,
	SPDK_BDEV_IO_STATUS_PENDING = 0,
	SPDK_BDEV_IO_STATUS_SUCCESS = 1,
};

/** Blockdev reset operation type */
enum spdk_bdev_reset_type {
	/**
	 * A hard reset indicates that the blockdev layer should not
	 *  invoke the completion callback for I/Os issued before the
	 *  reset is issued but completed after the reset is complete.
	 */
	SPDK_BDEV_RESET_HARD,

	/**
	 * A soft reset indicates that the blockdev layer should still
	 *  invoke the completion callback for I/Os issued before the
	 *  reset is issued but completed after the reset is complete.
	 */
	SPDK_BDEV_RESET_SOFT,
};

typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *bdev_io,
		enum spdk_bdev_io_status status,
		void *cb_arg);
typedef void (*spdk_bdev_io_get_rbuf_cb)(struct spdk_bdev_io *bdev_io);

/**
 * Block device I/O
 *
 * This is an I/O that is passed to an spdk_bdev.
 */
struct spdk_bdev_io {
	/** Pointer to scratch area reserved for use by the driver consuming this spdk_bdev_io. */
	void *ctx;

	/** The block device that this I/O belongs to. */
	struct spdk_bdev *bdev;

	/** The I/O channel to submit this I/O on. */
	struct spdk_io_channel *ch;

	/** Generation value for each I/O. */
	uint32_t gencnt;

	/** Enumerated value representing the I/O type. */
	enum spdk_bdev_io_type type;

	union {
		struct {

			/** The unaligned rbuf originally allocated. */
			void *buf_unaligned;

			/** For basic read case, use our own iovec element. */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** For SG buffer cases, total size of data to be transferred. */
			size_t len;

			/** Starting offset (in bytes) of the blockdev for this I/O. */
			uint64_t offset;

			/** Indicate whether the blockdev layer to put rbuf or not. */
			bool put_rbuf;
		} read;
		struct {
			/** For basic write case, use our own iovec element */
			struct iovec iov;

			/** For SG buffer cases, array of iovecs to transfer. */
			struct iovec *iovs;

			/** For SG buffer cases, number of iovecs in iovec array. */
			int iovcnt;

			/** For SG buffer cases, total size of data to be transferred. */
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

	/** Callback for when rbuf is allocated */
	spdk_bdev_io_get_rbuf_cb get_rbuf_cb;

	/** Status for the IO */
	enum spdk_bdev_io_status status;

	/**
	 * Set to true while the bdev module submit_request function is in progress.
	 *
	 * This is used to decide whether spdk_bdev_io_complete() can complete the I/O directly
	 * or if completion must be deferred via an event.
	 */
	bool in_submit_request;

	/** Used in virtual device (e.g., RAID), indicates its parent spdk_bdev_io **/
	struct spdk_bdev_io *parent;

	/** Used in virtual device (e.g., RAID) for storing multiple child device I/Os **/
	TAILQ_HEAD(child_io, spdk_bdev_io) child_io;

	/** Member used for linking child I/Os together. */
	TAILQ_ENTRY(spdk_bdev_io) link;

	/** Entry to the list need_buf of struct spdk_bdev. */
	TAILQ_ENTRY(spdk_bdev_io) rbuf_link;

	/** Per I/O context for use by the blockdev module */
	uint8_t driver_ctx[0];

	/* No members may be added after driver_ctx! */
};

struct spdk_bdev *spdk_bdev_get_by_name(const char *bdev_name);
void spdk_bdev_unregister(struct spdk_bdev *bdev);

struct spdk_bdev *spdk_bdev_first(void);
struct spdk_bdev *spdk_bdev_next(struct spdk_bdev *prev);

/**
 * Claim ownership of a block device.
 *
 * User applications and virtual blockdevs may use this to mediate access to bdevs.
 *
 * When the ownership of the bdev is no longer needed, the user should call spdk_bdev_unclaim().
 *
 * \param bdev Block device to claim.
 * \param remove_cb callback function for hot remove the device.
 * \param remove_ctx param for hot removal callback function.
 * \return true if the caller claimed the bdev, or false if it was already claimed by another user.
 */
bool spdk_bdev_claim(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb, void *remove_ctx);

/**
 * Release claim of ownership of a block device.
 *
 * When a bdev reference acquired with spdk_bdev_claim() is no longer needed, the user should
 * release the claim using spdk_bdev_unclaim().
 *
 * \param bdev Block device to release.
 */
void spdk_bdev_unclaim(struct spdk_bdev *bdev);

bool spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type);

int spdk_bdev_dump_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);

struct spdk_bdev_io *spdk_bdev_read(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				    void *buf, uint64_t offset, uint64_t nbytes,
				    spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *
spdk_bdev_readv(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_write(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				     void *buf, uint64_t offset, uint64_t nbytes,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_writev(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				      struct iovec *iov, int iovcnt,
				      uint64_t offset, uint64_t len,
				      spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_unmap(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				     struct spdk_scsi_unmap_bdesc *unmap_d,
				     uint16_t bdesc_count,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_bdev_io *spdk_bdev_flush(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
				     uint64_t offset, uint64_t length,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);
int spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);
int spdk_bdev_reset(struct spdk_bdev *bdev, enum spdk_bdev_reset_type,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);
struct spdk_io_channel *spdk_bdev_get_io_channel(struct spdk_bdev *bdev, uint32_t priority);
void spdk_bdev_io_set_scsi_error(struct spdk_bdev_io *bdev_io, enum spdk_scsi_status sc,
				 enum spdk_scsi_sense sk, uint8_t asc, uint8_t ascq);

/**
 * Get the status of bdev_io as an NVMe status code.
 *
 * \param bdev_io I/O to get the status from.
 * \param sct Status Code Type return value, as defined by the NVMe specification.
 * \param sc Status Code return value, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc);

#endif /* SPDK_BDEV_H_ */
