/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * in the I/O path.  This is primarily done by only allowing most
 * functions to be called on the metadata thread.  The metadata thread is
 * the thread which called spdk_bs_init() or spdk_bs_load().
 *
 * Functions starting with the prefix "spdk_blob_io" are passed a channel
 * as an argument, and channels may only be used from the thread they were
 * created on. See \ref spdk_bs_alloc_io_channel.  These are the only
 * functions that may be called from a thread other than the metadata
 * thread.
 *
 * The blobstore returns errors using negated POSIX errno values, either
 * returned in the callback or as a return value. An errno value of 0 means
 * success.
 */

#ifndef SPDK_BLOB_H
#define SPDK_BLOB_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t spdk_blob_id;
#define SPDK_BLOBID_INVALID		(uint64_t)-1
#define SPDK_BLOBID_EXTERNAL_SNAPSHOT	(uint64_t)-2
#define SPDK_BLOBSTORE_TYPE_LENGTH 16

enum blob_clear_method {
	BLOB_CLEAR_WITH_DEFAULT,
	BLOB_CLEAR_WITH_NONE,
	BLOB_CLEAR_WITH_UNMAP,
	BLOB_CLEAR_WITH_WRITE_ZEROES,
};

enum bs_clear_method {
	BS_CLEAR_WITH_UNMAP,
	BS_CLEAR_WITH_WRITE_ZEROES,
	BS_CLEAR_WITH_NONE,
};

struct spdk_blob_store;
struct spdk_bs_dev;
struct spdk_io_channel;
struct spdk_blob;
struct spdk_xattr_names;

/**
 * Blobstore operation completion callback.
 *
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_bs_op_complete)(void *cb_arg, int bserrno);

/**
 * Blobstore operation completion callback with handle.
 *
 * \param cb_arg Callback argument.
 * \param bs Handle to a blobstore.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_bs_op_with_handle_complete)(void *cb_arg, struct spdk_blob_store *bs,
		int bserrno);

/**
 * Blob operation completion callback.
 *
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blob_op_complete)(void *cb_arg, int bserrno);

/**
 * Blob operation completion callback with blob ID.
 *
 * \param cb_arg Callback argument.
 * \param blobid Blob ID.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blob_op_with_id_complete)(void *cb_arg, spdk_blob_id blobid, int bserrno);

/**
 * Blob operation completion callback with handle.
 *
 * \param cb_arg Callback argument.
 * \param blb Handle to a blob.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blob_op_with_handle_complete)(void *cb_arg, struct spdk_blob *blb, int bserrno);

/**
 * Blobstore device completion callback.
 *
 * \param channel I/O channel the operation was initiated on.
 * \param cb_arg Callback argument.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_bs_dev_cpl)(struct spdk_io_channel *channel,
				void *cb_arg, int bserrno);

/**
 * Blob device open completion callback with blobstore device.
 *
 * \param cb_arg Callback argument.
 * \param bs_dev Blobstore device.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blob_op_with_bs_dev)(void *cb_arg, struct spdk_bs_dev *bs_dev, int bserrno);

/**
 * External snapshot device open callback. As an esnap clone blob is loading, it uses this
 * callback registered with the blobstore to create the external snapshot device. The blobstore
 * consumer must set this while loading the blobstore if it intends to support external snapshots.
 *
 * \param bs_ctx Context provided by the blobstore consumer via esnap_ctx member of struct
 * spdk_bs_opts.
 * \param blob_ctx Context provided to spdk_bs_open_ext() via esnap_ctx member of struct
 * spdk_bs_open_opts.
 * \param blob The blob that needs its external snapshot device.
 * \param esnap_id A copy of the esnap_id passed via blob_opts when creating the esnap clone.
 * \param id_size The size in bytes of the data referenced by esnap_id.
 * \param bs_dev When 0 is returned, the newly created blobstore device is returned by reference.
 *
 * \return 0 on success, else a negative errno.
 */
typedef int (*spdk_bs_esnap_dev_create)(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
					const void *esnap_id, uint32_t id_size,
					struct spdk_bs_dev **bs_dev);

struct spdk_bs_dev_cb_args {
	spdk_bs_dev_cpl		cb_fn;
	struct spdk_io_channel	*channel;
	void			*cb_arg;
};

/**
 * Structure with optional IO request parameters
 * The content of this structure must be valid until the IO request is completed
 */
struct spdk_blob_ext_io_opts {
	/** Size of this structure in bytes */
	size_t size;
	/** Memory domain which describes payload in this IO request. */
	struct spdk_memory_domain *memory_domain;
	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
	/** Optional user context */
	void *user_ctx;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_ext_io_opts) == 32, "Incorrect size");

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

	void (*readv_ext)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			  struct iovec *iov, int iovcnt,
			  uint64_t lba, uint32_t lba_count,
			  struct spdk_bs_dev_cb_args *cb_args,
			  struct spdk_blob_ext_io_opts *ext_io_opts);

	void (*writev_ext)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			   struct iovec *iov, int iovcnt,
			   uint64_t lba, uint32_t lba_count,
			   struct spdk_bs_dev_cb_args *cb_args,
			   struct spdk_blob_ext_io_opts *ext_io_opts);

	void (*flush)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct spdk_bs_dev_cb_args *cb_args);

	void (*write_zeroes)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			     uint64_t lba, uint64_t lba_count,
			     struct spdk_bs_dev_cb_args *cb_args);

	void (*unmap)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      uint64_t lba, uint64_t lba_count,
		      struct spdk_bs_dev_cb_args *cb_args);

	struct spdk_bdev *(*get_base_bdev)(struct spdk_bs_dev *dev);

	bool (*is_zeroes)(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count);

	/* Translate blob lba to lba on the underlying bdev.
	 * This operation recurses down the whole chain of bs_dev's.
	 * Returns true and initializes value of base_lba on success.
	 * Returns false on failure.
	 * The function may fail when blob lba is not backed by the bdev lba.
	 * For example, when we eventually hit zeroes device in the chain.
	 */
	bool (*translate_lba)(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba);

	void (*copy)(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args);

	uint64_t	blockcnt;
	uint32_t	blocklen; /* In bytes */
};

struct spdk_bs_type {
	char bstype[SPDK_BLOBSTORE_TYPE_LENGTH];
};

struct spdk_bs_opts {
	/** Size of cluster in bytes. Must be multiple of 4KiB page size. */
	uint32_t cluster_sz;

	/** Count of the number of pages reserved for metadata */
	uint32_t num_md_pages;

	/** Maximum simultaneous metadata operations */
	uint32_t max_md_ops;

	/** Maximum simultaneous operations per channel */
	uint32_t max_channel_ops;

	/** Clear method */
	enum bs_clear_method  clear_method;

	/** Blobstore type */
	struct spdk_bs_type bstype;

	/* Hole at bytes 36-39. */
	uint8_t reserved36[4];

	/** Callback function to invoke for each blob. */
	spdk_blob_op_with_handle_complete iter_cb_fn;

	/** Argument passed to iter_cb_fn for each blob. */
	void *iter_cb_arg;

	/**
	 * The size of spdk_bs_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * After that, new added fields should be put in the end of the struct.
	 */
	size_t opts_size;

	/** Force recovery during import. This is a uint64_t for padding reasons, treated as a bool. */
	uint64_t force_recover;

	/**
	 * External snapshot creation callback to register with the blobstore.
	 */
	spdk_bs_esnap_dev_create esnap_bs_dev_create;

	/**
	 * Context to pass with esnap_bs_dev_create.
	 */
	void *esnap_ctx;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_opts) == 88, "Incorrect size");

/**
 * Initialize a spdk_bs_opts structure to the default blobstore option values.
 *
 * \param opts The spdk_bs_opts structure to be initialized.
 * \param opts_size The opts_size must be the size of spdk_bs_opts structure.
 */
void spdk_bs_opts_init(struct spdk_bs_opts *opts, size_t opts_size);

/**
 * Load a blobstore from the given device.
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Grow a blobstore to fill the underlying device
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_grow(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Initialize a blobstore on the given device.
 *
 * \param dev Blobstore block device.
 * \param opts The structure which contains the option values for the blobstore.
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg);

typedef void (*spdk_bs_dump_print_xattr)(FILE *fp, const char *bstype, const char *name,
		const void *value, size_t value_length);

/**
 * Dump a blobstore's metadata to a given FILE in human-readable format.
 *
 * \param dev Blobstore block device.
 * \param fp FILE pointer to dump the metadata contents.
 * \param print_xattr_fn Callback function to interpret external xattrs.
 * \param cb_fn Called when the dump is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_dump(struct spdk_bs_dev *dev, FILE *fp, spdk_bs_dump_print_xattr print_xattr_fn,
		  spdk_bs_op_complete cb_fn, void *cb_arg);
/**
 * Destroy the blobstore.
 *
 * It will destroy the blobstore by zeroing the super block.
 *
 * \param bs blobstore to destroy.
 * \param cb_fn Called when the destruction is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		     void *cb_arg);

/**
 * Unload the blobstore.
 *
 * It will flush all volatile data to disk.
 *
 * \param bs blobstore to unload.
 * \param cb_fn Called when the unloading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Set a super blob on the given blobstore.
 *
 * This will be retrievable immediately after spdk_bs_load() on the next initialization.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob which will be set as the super blob.
 * \param cb_fn Called when the setting is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_bs_op_complete cb_fn, void *cb_arg);

/**
 * Get the super blob. The obtained blob id will be passed to the callback function.
 *
 * \param bs blobstore.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_get_super(struct spdk_blob_store *bs,
		       spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Get the cluster size in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return cluster size.
 */
uint64_t spdk_bs_get_cluster_size(struct spdk_blob_store *bs);

/**
 * Get the page size in bytes. This is the write and read granularity of blobs.
 *
 * \param bs blobstore to query.
 *
 * \return page size.
 */
uint64_t spdk_bs_get_page_size(struct spdk_blob_store *bs);

/**
 * Get the io unit size in bytes.
 *
 * \param bs blobstore to query.
 *
 * \return io unit size.
 */
uint64_t spdk_bs_get_io_unit_size(struct spdk_blob_store *bs);

/**
 * Get the number of free clusters.
 *
 * \param bs blobstore to query.
 *
 * \return the number of free clusters.
 */
uint64_t spdk_bs_free_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the total number of clusters accessible by user.
 *
 * \param bs blobstore to query.
 *
 * \return the total number of clusters accessible by user.
 */
uint64_t spdk_bs_total_data_cluster_count(struct spdk_blob_store *bs);

/**
 * Get the blob id.
 *
 * \param blob Blob struct to query.
 *
 * \return blob id.
 */
spdk_blob_id spdk_blob_get_id(struct spdk_blob *blob);

/**
 * Get the number of pages allocated to the blob.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of pages.
 */
uint64_t spdk_blob_get_num_pages(struct spdk_blob *blob);

/**
 * Get the number of io_units allocated to the blob.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of io_units.
 */
uint64_t spdk_blob_get_num_io_units(struct spdk_blob *blob);

/**
 * Get the number of clusters allocated to the blob.
 *
 * \param blob Blob struct to query.
 *
 * \return the number of clusters.
 */
uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob);

/**
 * Get next allocated io_unit
 *
 * Starting at 'offset' io_units into the blob, returns the offset of
 * the first allocated io unit found.
 * If 'offset' points to an allocated io_unit, same offset is returned.
 *
 * \param blob Blob struct to query.
 * \param offset Offset is in io units from the beginning of the blob.
 *
 * \return offset in io_units or UINT64_MAX if no allocated io_unit found
 */
uint64_t spdk_blob_get_next_allocated_io_unit(struct spdk_blob *blob, uint64_t offset);

/**
 * Get next unallocated io_unit
 *
 * Starting at 'offset' io_units into the blob, returns the offset of
 * the first unallocated io unit found.
 * If 'offset' points to an unallocated io_unit, same offset is returned.
 *
 * \param blob Blob struct to query.
 * \param offset Offset is in io units from the beginning of the blob.
 *
 * \return offset in io_units or UINT64_MAX if only allocated io_unit found
 */
uint64_t spdk_blob_get_next_unallocated_io_unit(struct spdk_blob *blob, uint64_t offset);

struct spdk_blob_xattr_opts {
	/* Number of attributes */
	size_t	count;
	/* Array of attribute names. Caller should free this array after use. */
	char	**names;
	/* User context passed to get_xattr_value function */
	void	*ctx;
	/* Callback that will return value for each attribute name. */
	void	(*get_value)(void *xattr_ctx, const char *name,
			     const void **value, size_t *value_len);
};

struct spdk_blob_opts {
	uint64_t  num_clusters;
	bool	thin_provision;
	enum blob_clear_method clear_method;
	struct spdk_blob_xattr_opts xattrs;

	/** Enable separate extent pages in metadata */
	bool use_extent_table;

	/**
	 * The size of spdk_blob_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/**
	 * If set, create an esnap clone. The memory referenced by esnap_id will be copied into the
	 * blob's metadata and can be retrieved with spdk_blob_get_esnap_id(), typically from an
	 * esnap_bs_dev_create() callback.
	 * See struct_bs_opts.
	 *
	 * When esnap_id is specified, num_clusters should be specified. If it is not, the blob will
	 * have no capacity until spdk_blob_resize() is called.
	 */
	const void *esnap_id;

	/**
	 * The size of data referenced by esnap_id, in bytes.
	 */
	uint64_t esnap_id_len;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_opts) == 80, "Incorrect size");

/**
 * Initialize a spdk_blob_opts structure to the default blob option values.
 *
 * \param opts spdk_blob_opts structure to initialize.
 * \param opts_size It must be the size of spdk_blob_opts structure.
 */
void spdk_blob_opts_init(struct spdk_blob_opts *opts, size_t opts_size);

/**
 * Create a new blob with options on the given blobstore. The new blob id will
 * be passed to the callback function.
 *
 * \param bs blobstore.
 * \param opts The structure which contains the option values for the new blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_blob_ext(struct spdk_blob_store *bs, const struct spdk_blob_opts *opts,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a new blob with default option values on the given blobstore.
 * The new blob id will be passed to the callback function.
 *
 * \param bs blobstore.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_blob(struct spdk_blob_store *bs,
			 spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a read-only snapshot of specified blob with provided options.
 * This will automatically sync specified blob.
 *
 * When operation is done, original blob is converted to the thin-provisioned
 * blob with a newly created read-only snapshot set as a backing blob.
 * Structure snapshot_xattrs as well as anything it references (like e.g. names
 * array) must be valid until the completion is called.
 *
 * \param bs blobstore.
 * \param blobid Id of the source blob used to create a snapshot.
 * \param snapshot_xattrs xattrs specified for snapshot.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid,
			     const struct spdk_blob_xattr_opts *snapshot_xattrs,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Create a clone of specified read-only blob.
 *
 * Structure clone_xattrs as well as anything it references (like e.g. names
 * array) must be valid until the completion is called.
 *
 * \param bs blobstore.
 * \param blobid Id of the read only blob used as a snapshot for new clone.
 * \param clone_xattrs xattrs specified for clone.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_clone(struct spdk_blob_store *bs, spdk_blob_id blobid,
			  const struct spdk_blob_xattr_opts *clone_xattrs,
			  spdk_blob_op_with_id_complete cb_fn, void *cb_arg);

/**
 * Provide table with blob id's of clones are dependent on specified snapshot.
 *
 * Ids array should be allocated and the count parameter set to the number of
 * id's it can store, before calling this function.
 *
 * If ids is NULL or count parameter is not sufficient to handle ids of all
 * clones, -ENOMEM error is returned and count parameter is updated to the
 * total number of clones.
 *
 * \param bs blobstore.
 * \param blobid Snapshots blob id.
 * \param ids Array of the clone ids or NULL to get required size in count.
 * \param count Size of ids. After call it is updated to the number of clones.
 *
 * \return -ENOMEM if count is not sufficient to store all clones.
 */
int spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
			 size_t *count);

/**
 * Get the blob id for the parent snapshot of this blob.
 *
 * \param bs blobstore.
 * \param blobid Blob id.
 *
 * \return blob id of parent blob or SPDK_BLOBID_INVALID if have no parent
 */
spdk_blob_id spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid);

/**
 * Get the id used to access the esnap clone's parent.
 *
 * \param blob The clone's blob.
 * \param id On successful return, *id will reference memory that has the same life as blob.
 * \param len On successful return *len will be the size of id in bytes.
 *
 * \return 0 on success
 * \return -EINVAL if blob is not an esnap clone.
 */
int spdk_blob_get_esnap_id(struct spdk_blob *blob, const void **id, size_t *len);

/**
 * Check if blob is read only.
 *
 * \param blob Blob.
 *
 * \return true if blob is read only.
 */
bool spdk_blob_is_read_only(struct spdk_blob *blob);

/**
 * Check if blob is a snapshot.
 *
 * \param blob Blob.
 *
 * \return true if blob is a snapshot.
 */
bool spdk_blob_is_snapshot(struct spdk_blob *blob);

/**
 * Check if blob is a clone.
 *
 * \param blob Blob.
 *
 * \return true if blob is a clone.
 */
bool spdk_blob_is_clone(struct spdk_blob *blob);

/**
 * Check if blob is thin-provisioned.
 *
 * \param blob Blob.
 *
 * \return true if blob is thin-provisioned.
 */
bool spdk_blob_is_thin_provisioned(struct spdk_blob *blob);

/**
 * Check if blob is a clone of an external bdev.
 *
 * \param blob Blob.
 *
 * \return true if blob is a clone of an external bdev.
 */
bool spdk_blob_is_esnap_clone(const struct spdk_blob *blob);

/**
 * Delete an existing blob from the given blobstore.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to delete.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			 spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Allocate all clusters in this blob. Data for allocated clusters is copied
 * from backing blob(s) if they exist.
 *
 * This call removes all dependencies on any backing blobs.
 *
 * \param bs blobstore.
 * \param channel IO channel used to inflate blob.
 * \param blobid The id of the blob to inflate.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			  spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Remove dependency on parent blob.
 *
 * This call allocates and copies data for any clusters that are allocated in
 * the parent blob, and decouples parent updating dependencies of blob to
 * its ancestor.
 *
 * If blob have no parent -EINVAL error is reported.
 *
 * \param bs blobstore.
 * \param channel IO channel used to inflate blob.
 * \param blobid The id of the blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_blob_decouple_parent(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
				  spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg);

struct spdk_blob_open_opts {
	enum blob_clear_method  clear_method;

	/**
	 * The size of spdk_blob_open_opts according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	/**
	 * Blob context to be passed to any call of bs->external_bs_dev_create() that is triggered
	 * by this open call.
	 */
	void *esnap_ctx;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_blob_open_opts) == 24, "Incorrect size");

/**
 * Initialize a spdk_blob_open_opts structure to the default blob option values.
 *
 * \param opts spdk_blob_open_opts structure to initialize.
 * \param opts_size It mus be the size of struct spdk_blob_open_opts.
 */
void spdk_blob_open_opts_init(struct spdk_blob_open_opts *opts, size_t opts_size);

/**
 * Open a blob from the given blobstore.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to open.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Open a blob from the given blobstore with additional options.
 *
 * \param bs blobstore.
 * \param blobid The id of the blob to open.
 * \param opts The structure which contains the option values for the blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_open_blob_ext(struct spdk_blob_store *bs, spdk_blob_id blobid,
			   struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Resize a blob to 'sz' clusters. These changes are not persisted to disk until
 * spdk_bs_md_sync_blob() is called.
 * If called before previous resize finish, it will fail with errno -EBUSY
 *
 * \param blob Blob to resize.
 * \param sz The new number of clusters.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 *
 */
void spdk_blob_resize(struct spdk_blob *blob, uint64_t sz, spdk_blob_op_complete cb_fn,
		      void *cb_arg);

/**
 * Set blob as read only.
 *
 * These changes do not take effect until spdk_blob_sync_md() is called.
 *
 * \param blob Blob to set.
 */
int spdk_blob_set_read_only(struct spdk_blob *blob);

/**
 * Sync a blob.
 *
 * Make a blob persistent. This applies to open, resize, set xattr, and remove
 * xattr. These operations will not be persistent until the blob has been synced.
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
 * Allocate an I/O channel for the given blobstore.
 *
 * \param bs blobstore.
 * \return a pointer to the allocated I/O channel.
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
 * \param payload The specified buffer which should contain the data to be written.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
			void *payload, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read data from a blob.
 *
 * \param blob Blob to read.
 * \param channel The I/O channel used to submit requests.
 * \param payload The specified buffer which will store the obtained data.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       void *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write the data described by 'iov' to 'length' io_units beginning at 'offset' io_units
 * into the blob.
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
			 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			 spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Read 'length' io_units starting at 'offset' io_units into the blob into the memory
 * described by 'iov'.
 *
 * \param blob Blob to read.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write the data described by 'iov' to 'length' io_units beginning at 'offset' io_units
 * into the blob. Accepts extended IO request options
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 * \param io_opts Optional extended IO request options
 */
void spdk_blob_io_writev_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			     struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg,
			     struct spdk_blob_ext_io_opts *io_opts);

/**
 * Read 'length' io_units starting at 'offset' io_units into the blob into the memory
 * described by 'iov'. Accepts extended IO request options
 *
 * \param blob Blob to read.
 * \param channel I/O channel used to submit requests.
 * \param iov The pointer points to an array of iovec structures.
 * \param iovcnt The number of buffers.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 * \param io_opts Optional extended IO request options
 */
void spdk_blob_io_readv_ext(struct spdk_blob *blob, struct spdk_io_channel *channel,
			    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			    spdk_blob_op_complete cb_fn, void *cb_arg,
			    struct spdk_blob_ext_io_opts *io_opts);

/**
 * Unmap 'length' io_units beginning at 'offset' io_units on the blob as unused. Unmapped
 * io_units may allow the underlying storage media to behave more efficiently.
 *
 * \param blob Blob to unmap.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of unmap area in io_units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
			uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Write zeros into area of a blob.
 *
 * \param blob Blob to write.
 * \param channel I/O channel used to submit requests.
 * \param offset Offset is in io units from the beginning of the blob.
 * \param length Size of data in io units.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			       uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg);

/**
 * Get the first blob of the blobstore. The obtained blob will be passed to
 * the callback function.
 *
 * The user's cb_fn will be called with rc == -ENOENT when the iteration is
 * complete.
 *
 * When the user's cb_fn is called with rc == 0, the associated blob is open.
 * This means that the cb_fn may not attempt to unload the blobstore.  It
 * must complete the iteration before attempting to unload.
 *
 * \param bs blobstore to traverse.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_iter_first(struct spdk_blob_store *bs,
			spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Get the next blob by using the current blob. The obtained blob will be passed
 * to the callback function.
 *
 * The user's cb_fn will be called with rc == -ENOENT when the iteration is
 * complete.
 *
 * When the user's cb_fn is called with rc == 0, the associated blob is open.
 * This means that the cb_fn may not attempt to unload the blobstore.  It
 * must complete the iteration before attempting to unload.
 *
 * \param bs blobstore to traverse.
 * \param blob The current blob.
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *blob,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Set an extended attribute for the given blob.
 *
 * \param blob Blob to set attribute.
 * \param name Name of the extended attribute.
 * \param value Value of the extended attribute.
 * \param value_len Length of the value.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
			uint16_t value_len);

/**
 * Remove the extended attribute from the given blob.
 *
 * \param blob Blob to remove attribute.
 * \param name Name of the extended attribute.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name);

/**
 * Get the value of the specified extended attribute. The obtained value and its
 * size will be stored in value and value_len.
 *
 * \param blob Blob to query.
 * \param name Name of the extended attribute.
 * \param value Parameter as output.
 * \param value_len Parameter as output.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			      const void **value, size_t *value_len);

/**
 * Iterate through all extended attributes of the blob. Get the names of all extended
 * attributes that will be stored in names.
 *
 * \param blob Blob to query.
 * \param names Parameter as output.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_blob_get_xattr_names(struct spdk_blob *blob, struct spdk_xattr_names **names);

/**
 * Get the number of extended attributes.
 *
 * \param names Names of total extended attributes of the blob.
 *
 * \return the number of extended attributes.
 */
uint32_t spdk_xattr_names_get_count(struct spdk_xattr_names *names);

/**
 * Get the attribute name specified by the index.
 *
 * \param names Names of total extended attributes of the blob.
 * \param index Index position of the specified attribute.
 *
 * \return attribute name.
 */
const char *spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index);

/**
 * Free the attribute names.
 *
 * \param names Names of total extended attributes of the blob.
 */
void spdk_xattr_names_free(struct spdk_xattr_names *names);

/**
 * Get blobstore type of the given device.
 *
 * \param bs blobstore to query.
 *
 * \return blobstore type.
 */
struct spdk_bs_type spdk_bs_get_bstype(struct spdk_blob_store *bs);

/**
 * Set blobstore type to the given device.
 *
 * \param bs blobstore to set to.
 * \param bstype Type label to set.
 */
void spdk_bs_set_bstype(struct spdk_blob_store *bs, struct spdk_bs_type bstype);

/**
 * Replace the existing external snapshot device.
 *
 * \param blob The blob that is getting a new external snapshot device.
 * \param back_bs_dev The new blobstore device to use as an external snapshot.
 * \param cb_fn Callback to be called when complete.
 * \param cb_arg Callback argument used with cb_fn.
 */
void spdk_blob_set_esnap_bs_dev(struct spdk_blob *blob, struct spdk_bs_dev *back_bs_dev,
				spdk_blob_op_complete cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BLOB_H_ */
