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
 * Blob Storage System
 *
 * The blob storage system, or the blobstore for short, is a low level
 * library for placing opaque blobs of data onto a storage device such
 * that scattered physical blocks on the storage device appear as a
 * single, contiguous storage region. These blobs are also persistent,
 * which means they are rediscoverable after reboot or power loss.
 *
 * The blobstore is designed to be very high performance, and thus has
 * a few general rules regarding thread safety to avoid taking locks
 * in the I/O path. Functions starting with the prefix "spdk_bs_md" must only
 * be called from the metadata thread, of which there is only one at a time.
 * The user application can declare which thread is the metadata thread by
 * calling \ref spdk_bs_register_md_thread, but by default it is the thread
 * that was used to create the blobstore initially. The metadata thread can
 * be changed at run time by first unregistering
 * (\ref spdk_bs_unregister_md_thread) and then re-registering. Registering
 * a thread as the metadata thread is expensive and should be avoided.
 *
 * Functions starting with the prefix "spdk_bs_io" are passed a channel
 * as an argument, and channels may only be used from the thread they were
 * created on. See \ref spdk_bs_alloc_io_channel.
 *
 * Functions not starting with one of those two prefixes are thread safe
 * and may be called from any thread at any time.
 *
 * The blob store returns errors using negated POSIX errno values, either
 * returned in the callback or as a return value. An errno value of 0 means
 * success.
 */

#ifndef SPDK_BLOB_H
#define SPDK_BLOB_H

#include "spdk/stdinc.h"

typedef uint64_t spdk_blob_id;
#define SPDK_BLOBID_INVALID	(uint64_t)-1

struct spdk_blob_store;
struct spdk_io_channel;
struct spdk_blob;
struct spdk_xattr_names;

typedef void (*spdk_bs_op_complete)(void *cb_arg, int bserrno);
typedef void (*spdk_bs_op_with_handle_complete)(void *cb_arg, struct spdk_blob_store *bs,
		int bserrno);
typedef void (*spdk_blob_op_complete)(void *cb_arg, int bserrno);
typedef void (*spdk_blob_op_with_id_complete)(void *cb_arg, spdk_blob_id blobid, int bserrno);
typedef void (*spdk_blob_op_with_handle_complete)(void *cb_arg, struct spdk_blob *blb, int bserrno);


/* Calls to function pointers of this type must obey all of the normal
   rules for channels. The channel passed to this completion must match
   the channel the operation was initiated on. */
typedef void (*spdk_bs_dev_cpl)(struct spdk_io_channel *channel,
				void *cb_arg, int bserrno);

struct spdk_bs_dev_cb_args {
	spdk_bs_dev_cpl		cb_fn;
	struct spdk_io_channel	*channel;
	void			*cb_arg;
	/*
	 * Blobstore device implementations can use this for scratch space for any data
	 *  structures needed to translate the function arguments to the required format
	 *  for the backing store.
	 */
	uint8_t			scratch[32];
};

struct spdk_bs_dev {
	/* Create a new channel which is a software construct that is used
	 * to submit I/O. */
	struct spdk_io_channel *(*create_channel)(struct spdk_bs_dev *dev);

	/* Destroy a previously created channel */
	void (*destroy_channel)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel);

	/* Destroy this blobstore device.  Applications must not destroy the blobstore device,
	 *  rather the blobstore will destroy it using this function pointer once all
	 *  references to it during unload callback context have been completed.
	 */
	void (*destroy)(struct spdk_bs_dev *dev);

	void (*read)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		     uint64_t lba, uint32_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args);

	void (*write)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);

	void (*flush)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct spdk_bs_dev_cb_args *cb_args);

	void (*unmap)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);

	uint64_t	blockcnt;
	uint32_t	blocklen; /* In bytes */
};

struct spdk_bs_opts {
	uint32_t cluster_sz; /* In bytes. Must be multiple of page size. */
	uint32_t num_md_pages; /* Count of the number of pages reserved for metadata */
	uint32_t max_md_ops; /* Maximum simultaneous metadata operations */
};

/* Initialize an spdk_bs_opts structure to the default blobstore option values. */
void spdk_bs_opts_init(struct spdk_bs_opts *opts);

/* Load a blob store from the given device. This will fail (return NULL) if no blob store is present. */
void spdk_bs_load(struct spdk_bs_dev *dev,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/* Initialize a blob store on the given disk. Destroys all data present on the device. */
void spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/* Flush all volatile data to disk and destroy in-memory structures. */
void spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg);

/* Set the given blob as the super blob. This will be retrievable immediately after an
 * spdk_bs_load on the next initialization.
 */
void spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_bs_op_complete cb_fn, void *cb_arg);

/* Open the super blob. */
void spdk_bs_get_super(struct spdk_blob_store *bs,
		       spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/* Get the cluster size in bytes. Used in the extend operation. */
uint64_t spdk_bs_get_cluster_size(struct spdk_blob_store *bs);

/* Get the page size in bytes. This is the write and read granularity of blobs. */
uint64_t spdk_bs_get_page_size(struct spdk_blob_store *bs);

/* Get the number of free clusters. */
uint64_t spdk_bs_free_cluster_count(struct spdk_blob_store *bs);

/* Register the current thread as the metadata thread. All functions beginning with
 * the prefix "spdk_bs_md" must be called only from this thread.
 */
int spdk_bs_register_md_thread(struct spdk_blob_store *bs);

/* Unregister the current thread as the metadata thread. This allows a different
 * thread to be registered.
 */
int spdk_bs_unregister_md_thread(struct spdk_blob_store *bs);

/* Return the blobid */
spdk_blob_id spdk_blob_get_id(struct spdk_blob *blob);

/* Return the number of pages allocated to the blob */
uint64_t spdk_blob_get_num_pages(struct spdk_blob *blob);

/* Return the number of clusters allocated to the blob */
uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob);

/* Create a new blob with initial size of 'sz' clusters. */
void spdk_bs_md_create_blob(struct spdk_blob_store *bs,
			    spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/* Delete an existing blob. */
void spdk_bs_md_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			    spdk_blob_op_complete cb_fn, void *cb_arg);

/* Open a blob */
void spdk_bs_md_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/* Resize a blob to 'sz' clusters.
 *
 * These changes are not persisted to disk until
 * spdk_bs_md_sync_blob() is called. */
int spdk_bs_md_resize_blob(struct spdk_blob *blob, size_t sz);

/* Sync a blob */
/* Make a blob persistent. This applies to open, resize, set xattr,
 * and remove xattr. These operations will not be persistent until
 * the blob has been synced.
 *
 * I/O operations (read/write) are synced independently. See
 * spdk_bs_io_flush_channel().
 */
void spdk_bs_md_sync_blob(struct spdk_blob *blob,
			  spdk_blob_op_complete cb_fn, void *cb_arg);

/* Close a blob. This will automatically sync. */
void spdk_bs_md_close_blob(struct spdk_blob **blob, spdk_blob_op_complete cb_fn, void *cb_arg);

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs,
		uint32_t priority, uint32_t max_ops);

void spdk_bs_free_io_channel(struct spdk_io_channel *channel);

/* Force all previously completed operations on this channel to be persistent. */
void spdk_bs_io_flush_channel(struct spdk_io_channel *channel,
			      spdk_blob_op_complete cb_fn, void *cb_arg);

/* Write data to a blob. Offset is in pages from the beginning of the blob. */
void spdk_bs_io_write_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   void *payload, uint64_t offset, uint64_t length,
			   spdk_blob_op_complete cb_fn, void *cb_arg);


/* Read data from a blob. Offset is in pages from the beginning of the blob. */
void spdk_bs_io_read_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  void *payload, uint64_t offset, uint64_t length,
			  spdk_blob_op_complete cb_fn, void *cb_arg);

/* Iterate through all blobs */
void spdk_bs_md_iter_first(struct spdk_blob_store *bs,
			   spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);
void spdk_bs_md_iter_next(struct spdk_blob_store *bs, struct spdk_blob **blob,
			  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

int spdk_blob_md_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
			   uint16_t value_len);
int spdk_blob_md_remove_xattr(struct spdk_blob *blob, const char *name);
int spdk_bs_md_get_xattr_value(struct spdk_blob *blob, const char *name,
			       const void **value, size_t *value_len);
int spdk_bs_md_get_xattr_names(struct spdk_blob *blob,
			       struct spdk_xattr_names **names);

uint32_t spdk_xattr_names_get_count(struct spdk_xattr_names *names);
const char *spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index);
void spdk_xattr_names_free(struct spdk_xattr_names *names);

#endif /* SPDK_BLOB_H_ */
