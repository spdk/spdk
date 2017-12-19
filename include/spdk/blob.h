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
 * The metadata thread is the thread which called spdk_bs_init() or
 * spdk_bs_load().
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

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t spdk_blob_id;
#define SPDK_BLOBID_INVALID	(uint64_t)-1
#define SPDK_BLOBSTORE_TYPE_LENGTH 16

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

	void (*readv)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);

	void (*writev)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       struct spdk_bs_dev_cb_args *cb_args);

	void (*flush)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct spdk_bs_dev_cb_args *cb_args);

	void (*write_zeroes)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			     uint64_t lba, uint32_t lba_count,
			     struct spdk_bs_dev_cb_args *cb_args);

	void (*unmap)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      uint64_t lba, uint32_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);

	uint64_t	blockcnt;
	uint32_t	blocklen; /* In bytes */
};

struct spdk_bs_type {
	char bstype[SPDK_BLOBSTORE_TYPE_LENGTH];
};

struct spdk_bs_opts {
	uint32_t cluster_sz; /* In bytes. Must be multiple of page size. */
	uint32_t num_md_pages; /* Count of the number of pages reserved for metadata */
	uint32_t max_md_ops; /* Maximum simultaneous metadata operations */
	uint32_t max_channel_ops; /* Maximum simultaneous operations per channel */
	struct spdk_bs_type bstype; /* Blobstore type */
};

/**
 * Initialize an spdk_bs_opts structure to the default blobstore option values.
 *
 * \param opts The spdk_bs_opts structure to be initialized.
 */
void spdk_bs_opts_init(struct spdk_bs_opts *opts);

/**
 * Load a blob store from the given device.
 *
 * \param dev Blobstore device.
 * \param opts The structure which contains the option values for the blob store.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Initialize a blob store on the given device.
 *
 * \param dev Blobstore device.
 * \param opts The structure which contains the option values for the blob store.
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Destroy the blob store.
 *
 * It will destroy the blob store by zeroing the super block and freeing
 * in-memory structures.
 *
 * \param bs Blob store to destroy.
 * \param cb_fn Called when the destruction is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		     void *cb_arg);

/**
 * Unload the blob store.
 *
 * It will flush all volatile data to disk and free in-memory structures.
 *
 * \param bs Blob store to unload.
 * \param cb_fn Called when the unloading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Set super blob on the gien blob store.
 *
 * This will be retrievable immediately after spdk_bs_load on the next initializaiton.
 *
 * \param bs Blob store.
 * \param blobid The id of the blob which will be set as the super blob..
 * \param cb_fn Called when the setting is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Get super blob. The obtained blob id will be passed to the callback function.
 *
 * \param bs Blob store.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_get_super(struct spdk_blob_store *bs,
		       spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Get the cluster size in bytes. Used in the extend operation.
 *
 * \param bs Blob store to query.
 * \return Cluster size.
 */
uint64_t spdk_bs_get_cluster_size(struct spdk_blob_store *bs);

/**
 * Get the page size in bytes. This is the write and read granularity of blobs.
 *
 * \param bs Blob store to query.
 * \return Page size.
 */
uint64_t spdk_bs_get_page_size(struct spdk_blob_store *bs);

/**
 * Get the number of free clusters.
 *
 * \param bs Blob store to query.
 * \return Amount of free clusters.
 */
uint64_t spdk_bs_free_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the total number of clusters accessible by user.
 *
 * \param bs Blob store to query.
 * \return Amount of clusters accessible by user.
 */
uint64_t spdk_bs_total_data_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the blob id.
 *
 * \param blob Blob struct to query.
 * \return Blob id.
 */
spdk_blob_id spdk_blob_get_id(struct spdk_blob *blob);

/**
 * Get the number of pages allocated to the blob.
 *
 * \param blob Blob struct to query.
 * \return Number of pages.
 */
uint64_t spdk_blob_get_num_pages(struct spdk_blob *blob);

/**
 * Get the number of clusters allocated to the blob.
 *
 * \param blob Blob struct to query.
 * \return Number of clusters.
 */
uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob);

/**
 * Create a new blob on the given blob store. The new blob id will be passed to
 * the callback function.
 *
 * \param bs Blob store.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_blob(struct spdk_blob_store *bs,
			 spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Delete an existing blob from the given blob store.
 *
 * \param bs Blob store.
 * \param blobid The id of the blob to delete.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			 spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Open a blob from the given blob store.
 *
 * \param bs Blob store.
 * \param blobid The id of the blob to open.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Resize a blob to 'sz' clusters.
 *
 * These changes are not persisted to disk until spdk_bs_md_sync_blob() is called.
 *
 * \param blob Blob to resize.
 * \param sz The new size of clusters.
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_resize(struct spdk_blob *blob, size_t sz);

/**
 * Sync a blob.
 *
 * Make a blob persistent. This applies to open, resize, set xattr,
 * and remove xattr. These operations will not be persistent until
 * the blob has been synced.
 *
 * I/O operations (read/write) are synced independently. See
 * spdk_bs_io_flush_channel().
 *
 * \param blob Blob to sync.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Close a blob. This will automatically sync.
 *
 * \param blob Blob to close.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_close(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Allocate an I/O channel for the given blob store.
 *
 * \param bs Blob store.
 * \return The allocated I/O channel.
 */
struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs);

/**
 * Free the I/O channel.
 *
 * \param channel I/O channel to free.
 */
void spdk_bs_free_io_channel(struct spdk_io_channel *channel);

/**
 * Write data to a blob.
 *
 * \param blob Blob to write.
 * \param channel The I/O channel used to submit requests.
 * \param payload The specified buffer which should contain the data to be transmitted.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of data to write.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_write_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   void *payload, uint64_t offset, uint64_t length,
			   spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read data from a blob.
 *
 * \param blob Blob to read.
 * \param channel The I/O channel used to submit requests.
 * \param payload The specified buffer which will store the obtained data.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of data to write.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_read_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  void *payload, uint64_t offset, uint64_t length,
			  spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write iovcnt buffers of data described by iov to the given blob.
 *
 * \param blib Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of data to write.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_writev_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			    spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read iovcnt buffers from the given blob into the buffers described by iov.
 *
 * \param blob Blob to read.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of data to read.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_readv_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			   spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Unmap area of a blob.
 *
 * \param blob Blob to unmap.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of unmap area.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_unmap_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write zeros into area of a blob.
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in pages from the beginning of the blob.
 * \param length The size of data to write.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_io_write_zeroes_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
				  uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Get the first blob of the blob store. The obtained blob will be passed to
 * the callback function.
 *
 * \param bs Blob store to traverse.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_iter_first(struct spdk_blob_store *bs,
			spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Get the next blob by using the current blob. The obtained blob will be passed
 * to the callback function.
 *
 * \param bs Blob store to traverse.
 * \param blob The current blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *blob,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Set an extend attribute for the given blob. The extend attribute is usually
 * limited in size to a value.
 *
 * \param blob Blob to set attribute.
 * \param name Name of the extend attribute.
 * \param value Value of the extend attribute.
 * \param value_len Limited size of the value.
 * \return 0 on success, -1 on failure.
 */
int spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
			uint16_t value_len);

/**
 * Remove the extend attribute from the given blob.
 *
 * \param blob Blob to remove attribute.
 * \param name Name of the extend attribute.
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name);

/**
 * Get the value of the specified extend attribute. The obtained value and its
 * size will be stored in value and value_len.
 *
 * \param blob Blob to query.
 * \param name Name of the extend attribute.
 * \param value Parameter as output.
 * \param value_len Parameter as output.
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			      const void **value, size_t *value_len);

/**
 * Iterate through all extend attributes of the blob. Get the names of all extend
 * attributes that will be stored in names.
 *
 * \param blob Blob to query.
 * \param names Parameter as output.
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_get_xattr_names(struct spdk_blob *blob, struct spdk_xattr_names **names);

/**
 * Get the amount of extend attributes. The names as input is obtaind by calling
 * spdk_bs_md_get_xattr_names().
 *
 * \param names Names of total extend attributes of the blob.
 * \return Amount of extend attributes.
 */
uint32_t spdk_xattr_names_get_count(struct spdk_xattr_names *names);

/**
 * Get the attribute name specified by the index. The names as input is obtained
 * by calling spdk_bs_md_get_xattr_names().
 *
 * \param names Names of total extend attributes of the blob.
 * \param index Index position of the specified attribute.
 * \return The obtained attribute name.
 */
const char *spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index);

/**
 * Free the structure. The names as input is obtained by calling
 * spdk_bs_md_get_xattr_names().
 *
 * \param names Names of total extend attributes of the blob.
 */
void spdk_xattr_names_free(struct spdk_xattr_names *names);

/**
 * Get blob store type of the given device.
 *
 * \param bs Blob store to query.
 * \return Blob store type.
 */
struct spdk_bs_type spdk_bs_get_bstype(struct spdk_blob_store *bs);

/**
 * Set blob store type to the given device.
 *
 * \param bs Blob store to set to.
 * \param bstype Type label to set.
 */
void spdk_bs_set_bstype(struct spdk_blob_store *bs, struct spdk_bs_type bstype);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BLOB_H_ */
