/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Filesystem device abstraction layer
 */

#ifndef SPDK_FSDEV_H
#define SPDK_FSDEV_H

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/assert.h"
#include "spdk/dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief SPDK filesystem device.
 *
 * This is a virtual representation of a filesystem device that is exported by the backend.
 */
struct spdk_fsdev;

/** Asynchronous event type */
enum spdk_fsdev_event_type {
	SPDK_FSDEV_EVENT_REMOVE,
};

/**
 * Filesystem device event callback.
 *
 * \param type Event type.
 * \param fsdev Filesystem device that triggered event.
 * \param event_ctx Context for the filesystem device event.
 */
typedef void (*spdk_fsdev_event_cb_t)(enum spdk_fsdev_event_type type,
				      struct spdk_fsdev *fsdev,
				      void *event_ctx);

struct spdk_fsdev_fn_table;
struct spdk_io_channel;

/** fsdev status */
enum spdk_fsdev_status {
	SPDK_FSDEV_STATUS_INVALID,
	SPDK_FSDEV_STATUS_READY,
	SPDK_FSDEV_STATUS_UNREGISTERING,
	SPDK_FSDEV_STATUS_REMOVING,
};

/** fsdev library options */
struct spdk_fsdev_opts {
	/**
	 * The size of spdk_fsdev_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	uint32_t opts_size;
	/**
	 * Size of fsdev IO objects pool
	 */
	uint32_t fsdev_io_pool_size;
	/**
	 * Size of fsdev IO objects cache per thread
	 */
	uint32_t fsdev_io_cache_size;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_opts) == 12, "Incorrect size");

/** fsdev mount options */
struct spdk_fsdev_mount_opts {
	/**
	 * The size of spdk_fsdev_mount_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	uint32_t opts_size;

	/**
	 * OUT Maximum size of the write buffer
	 */
	uint32_t max_write;

	/**
	 * IN/OUT Indicates whether the writeback caching should be enabled.
	 *
	 * See FUSE I/O ([1]) doc for more info.
	 *
	 * [1] https://www.kernel.org/doc/Documentation/filesystems/fuse-io.txt
	 *
	 * This feature is disabled by default.
	 */
	uint8_t writeback_cache_enabled;

} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_mount_opts) == 9, "Incorrect size");

/**
 * Structure with optional fsdev IO parameters
 * The content of this structure must be valid until the IO is completed
 */
struct spdk_fsdev_io_opts {
	/** Size of this structure in bytes */
	size_t size;
	/** Memory domain which describes payload in this IO. fsdev must support DMA device type that
	 * can access this memory domain, refer to \ref spdk_fsdev_get_memory_domains and
	 * \ref spdk_memory_domain_get_dma_device_type
	 * If set, that means that data buffers can't be accessed directly and the memory domain must
	 * be used to fetch data to local buffers or to translate data to another memory domain */
	struct spdk_memory_domain *memory_domain;
	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_io_opts) == 24, "Incorrect size");

/**
 * \brief Handle to an opened SPDK filesystem device.
 */
struct spdk_fsdev_desc;

/**
 * Filesystem device initialization callback.
 *
 * \param cb_arg Callback argument.
 * \param rc 0 if filesystem device initialized successfully or negative errno if it failed.
 */
typedef void (*spdk_fsdev_init_cb)(void *cb_arg, int rc);

/**
 * Filesystem device finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_fsdev_fini_cb)(void *cb_arg);

/**
 * Initialize filesystem device modules.
 *
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fsdev_initialize(spdk_fsdev_init_cb cb_fn, void *cb_arg);

/**
 * Perform cleanup work to remove the registered filesystem device modules.
 *
 * \param cb_fn Called when the removal is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fsdev_finish(spdk_fsdev_fini_cb cb_fn, void *cb_arg);

/**
 * Get the full configuration options for the registered filesystem device modules and created fsdevs.
 *
 * \param w pointer to a JSON write context where the configuration will be written.
 */
void spdk_fsdev_subsystem_config_json(struct spdk_json_write_ctx *w);

/**
 * Get filesystem device module name.
 *
 * \param fsdev Filesystem device to query.
 * \return Name of fsdev module as a null-terminated string.
 */
const char *spdk_fsdev_get_module_name(const struct spdk_fsdev *fsdev);

/**
 * Open a filesystem device for I/O operations.
 *
 * \param fsdev_name Filesystem device name to open.
 * \param event_cb notification callback to be called when the fsdev triggers
 * asynchronous event such as fsdev removal. This will always be called on the
 * same thread that spdk_fsdev_open() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the fsdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_fsdev_open(const char *fsdev_name, spdk_fsdev_event_cb_t event_cb,
		    void *event_ctx, struct spdk_fsdev_desc **desc);

/**
 * Close a previously opened filesystem device.
 *
 * Must be called on the same thread that the spdk_fsdev_open()
 * was performed on.
 *
 * \param desc Filesystem device descriptor to close.
 */
void spdk_fsdev_close(struct spdk_fsdev_desc *desc);

/**
 * Get filesystem device name.
 *
 * \param fsdev filesystem device to query.
 * \return Name of fsdev as a null-terminated string.
 */
const char *spdk_fsdev_get_name(const struct spdk_fsdev *fsdev);

/**
 * Get the fsdev associated with a fsdev descriptor.
 *
 * \param desc Open filesystem device descriptor
 * \return fsdev associated with the descriptor
 */
struct spdk_fsdev *spdk_fsdev_desc_get_fsdev(struct spdk_fsdev_desc *desc);

/**
 * Obtain an I/O channel for the filesystem device opened by the specified
 * descriptor. I/O channels are bound to threads, so the resulting I/O
 * channel may only be used from the thread it was originally obtained
 * from.
 *
 * \param desc Filesystem device descriptor.
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_fsdev_get_io_channel(struct spdk_fsdev_desc *desc);

/**
 * Set the options for the fsdev library.
 *
 * \param opts options to set
 * \return 0 on success.
 * \return -EINVAL if the options are invalid.
 */
int spdk_fsdev_set_opts(const struct spdk_fsdev_opts *opts);

/**
 * Get the options for the fsdev library.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
int spdk_fsdev_get_opts(struct spdk_fsdev_opts *opts, size_t opts_size);

/**
 * Get SPDK memory domains used by the given fsdev. If fsdev reports that it uses memory domains
 * that means that it can work with data buffers located in those memory domains.
 *
 * The user can call this function with \b domains set to NULL and \b array_size set to 0 to get the
 * number of memory domains used by fsdev
 *
 * \param fsdev filesystem device
 * \param domains pointer to an array of memory domains to be filled by this function. The user should allocate big enough
 * array to keep all memory domains used by fsdev and all underlying fsdevs
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
 * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
 * the content of \b domains array is valid in that case.
 *         -EINVAL if input parameters were invalid
 */
int spdk_fsdev_get_memory_domains(struct spdk_fsdev *fsdev, struct spdk_memory_domain **domains,
				  int array_size);


/* 'to_set' flags in spdk_fsdev_setattr */
#define FSDEV_SET_ATTR_MODE	(1 << 0)
#define FSDEV_SET_ATTR_UID	(1 << 1)
#define FSDEV_SET_ATTR_GID	(1 << 2)
#define FSDEV_SET_ATTR_SIZE	(1 << 3)
#define FSDEV_SET_ATTR_ATIME	(1 << 4)
#define FSDEV_SET_ATTR_MTIME	(1 << 5)
#define FSDEV_SET_ATTR_ATIME_NOW	(1 << 6)
#define FSDEV_SET_ATTR_MTIME_NOW	(1 << 7)
#define FSDEV_SET_ATTR_CTIME	(1 << 8)

struct spdk_fsdev_file_object;
struct spdk_fsdev_file_handle;

struct spdk_fsdev_file_attr {
	uint64_t ino;
	uint64_t size;
	uint64_t blocks;
	uint64_t atime;
	uint64_t mtime;
	uint64_t ctime;
	uint32_t atimensec;
	uint32_t mtimensec;
	uint32_t ctimensec;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint32_t blksize;
	uint32_t valid_ms;
};

struct spdk_fsdev_file_statfs {
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint32_t bsize;
	uint32_t namelen;
	uint32_t frsize;
};

/**
 * Mount operation completion callback.
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation status, 0 on success or error code otherwise.
 * \param opts Result options.
 * \param root_fobject Root file object
 */
typedef void (spdk_fsdev_mount_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       const struct spdk_fsdev_mount_opts *opts,
				       struct spdk_fsdev_file_object *root_fobject);

/**
 * Mount the filesystem.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param opts Requested options.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *
 * Note: the \p opts are the subject of negotiation. An API user provides a desired \p opts here
 * and gets a result \p opts in the \p cb_fn. The result \p opts are filled by the underlying
 * fsdev module which may agree or reduce (but not expand) the desired features set.
 */
int spdk_fsdev_mount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		     uint64_t unique, const struct spdk_fsdev_mount_opts *opts,
		     spdk_fsdev_mount_cpl_cb cb_fn, void *cb_arg);

/**
 * Umount operation completion callback.
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 */
typedef void (spdk_fsdev_umount_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch);

/**
 * Unmount the filesystem.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *
 * NOTE: on unmount the lookup count for all fobjects implicitly drops to zero.
 */
int spdk_fsdev_umount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t unique, spdk_fsdev_umount_cpl_cb cb_fn, void *cb_arg);

/**
 * Lookup file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
typedef void (spdk_fsdev_lookup_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Look up a directory entry by name and get its attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory. NULL for the root directory.
 * \param name The name to look up. Ignored if parent_fobject is NULL.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_lookup(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      spdk_fsdev_lookup_cpl_cb cb_fn, void *cb_arg);

/**
 * Look up file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_forget_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove file object from internal cache
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param nlookup Number of lookups to forget.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_forget(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *fobject, uint64_t nlookup,
		      spdk_fsdev_forget_cpl_cb cb_fn, void *cb_arg);

/**
 * Read symbolic link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param linkname symbolic link contents
 */
typedef void (spdk_fsdev_readlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const char *linkname);

/**
 * Read symbolic link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_readlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject,
			spdk_fsdev_readlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a symbolic link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
typedef void (spdk_fsdev_symlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a symbolic link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param target symbolic link's content
 * \param linkpath symbolic link's name
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_symlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *parent_fobject, const char *target,
		       const char *linkpath, uid_t euid, gid_t egid,
		       spdk_fsdev_symlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Create file node operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
typedef void (spdk_fsdev_mknod_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create file node
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name File name to create.
 * \param mode File type and mode with which to create the new file.
 * \param rdev The device number (only valid if created file is a device)
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_mknod(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, dev_t rdev,
		     uid_t euid, gid_t egid, spdk_fsdev_mknod_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
typedef void (spdk_fsdev_mkdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Directory name to create.
 * \param mode Directory type and mode with which to create the new directory.
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_mkdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode,
		     uid_t euid, gid_t egid, spdk_fsdev_mkdir_cpl_cb cb_fn, void *cb_arg);


/**
 * Remove a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_unlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to remove.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_unlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      spdk_fsdev_unlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Remove a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_rmdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to remove.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_rmdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name,
		     spdk_fsdev_rmdir_cpl_cb cb_fn, void *cb_arg);

/**
 * Rename a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_rename_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Rename a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory.
 * \param name Old rename.
 * \param new_parent_fobject New parent directory.
 * \param new_name New name.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_rename(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      struct spdk_fsdev_file_object *new_parent_fobject, const char *new_name,
		      uint32_t flags, spdk_fsdev_rename_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a hard link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
typedef void (spdk_fsdev_link_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				      struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a hard link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param new_parent_fobject New parent directory.
 * \param name Link name.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_link(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_object *new_parent_fobject,
		    const char *name, spdk_fsdev_link_cpl_cb cb_fn, void *cb_arg);

/**
 * Get file system statistic operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param statfs filesystem statistics
 */
typedef void (spdk_fsdev_statfs_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					const struct spdk_fsdev_file_statfs *statfs);

/**
 * Get file system statistics
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_statfs(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *fobject, spdk_fsdev_statfs_cpl_cb cb_fn, void *cb_arg);

/**
 * Set an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_setxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Set an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param value Buffer that contains value of an extended attribute.
 * \param size Size of an extended attribute.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_setxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name, const char *value,
			size_t size, uint32_t flags, spdk_fsdev_setxattr_cpl_cb cb_fn, void *cb_arg);
/**
 * Get an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param value_size Size of an data copied to the value buffer.
 */
typedef void (spdk_fsdev_getxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		size_t value_size);

/**
 * Get an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param buffer Buffer to put the extended attribute's value.
 * \param size Size of value's buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_getxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name, void *buffer,
			size_t size, spdk_fsdev_getxattr_cpl_cb cb_fn, void *cb_arg);

/**
 * List extended attribute names operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param size Size of an extended attribute list.
 * \param size_only true if buffer was NULL or size was 0 upon the \ref spdk_fsdev_listxattr call
 */
typedef void (spdk_fsdev_listxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		size_t size, bool size_only);

/**
 * List extended attribute names
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param buffer Buffer to to be used for the attribute names.
 * \param size Size of the \b buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_listxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			 uint64_t unique, struct spdk_fsdev_file_object *fobject, char *buffer, size_t size,
			 spdk_fsdev_listxattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Remove an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_removexattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status);

/**
 * Remove an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_removexattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name,
			   spdk_fsdev_removexattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Open a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fhandle File handle
 */
typedef void (spdk_fsdev_fopen_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_handle *fhandle);

/**
 * Open a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_fopen(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, uint32_t flags, spdk_fsdev_fopen_cpl_cb cb_fn,
		     void *cb_arg);


/**
 * Create and open a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param fobject File object.
 * \param attr File attributes.
 * \param fhandle File handle.
 */
typedef void (spdk_fsdev_create_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
					struct spdk_fsdev_file_handle *fhandle);

/**
 * Create and open a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to create.
 * \param mode File type and mode with which to create the new file.
 * \param flags Operation flags.
 * \param umask Umask of the calling process.
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
int spdk_fsdev_create(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, uint32_t flags,
		      mode_t umask, uid_t euid, gid_t egid,
		      spdk_fsdev_create_cpl_cb cb_fn, void *cb_arg);

/**
 * Release an open file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_release_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Release an open file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_release(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       spdk_fsdev_release_cpl_cb cb_fn, void *cb_arg);

/**
 * Get file attributes operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param attr file attributes.
 */
typedef void (spdk_fsdev_getattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const struct spdk_fsdev_file_attr *attr);

/**
 * Get file attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_getattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       spdk_fsdev_getattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Set file attributes operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param attr file attributes.
 */
typedef void (spdk_fsdev_setattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const struct spdk_fsdev_file_attr *attr);

/**
 * Set file attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param attr file attributes to set.
 * \param to_set Bit mask of attributes which should be set.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_setattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       const struct spdk_fsdev_file_attr *attr, uint32_t to_set,
		       spdk_fsdev_setattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Read data operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes read.
 */
typedef void (spdk_fsdev_read_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				      uint32_t data_size);

/**
 * Read data
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param size Number of bytes to read.
 * \param offs Offset to read from.
 * \param flags Operation flags.
 * \param iov Array of iovec to be used for the data.
 * \param iovcnt Size of the \b iov array.
 * \param opts Optional structure with extended File Operation options. If set, this structure must be
 * valid until the operation is completed. `size` member of this structure is used for ABI compatibility and
 * must be set to sizeof(struct spdk_fsdev_io_opts).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_read(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		    size_t size, uint64_t offs, uint32_t flags,
		    struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		    spdk_fsdev_read_cpl_cb cb_fn, void *cb_arg);

/**
 * Write data operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes written.
 */
typedef void (spdk_fsdev_write_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       uint32_t data_size);

/**
 * Write data
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param size Number of bytes to write.
 * \param offs Offset to write to.
 * \param flags Operation flags.
 * \param iov Array of iovec to where the data is stored.
 * \param iovcnt Size of the \b iov array.
 * \param opts Optional structure with extended File Operation options. If set, this structure must be
 * valid until the operation is completed. `size` member of this structure is used for ABI compatibility and
 * must be set to sizeof(struct spdk_fsdev_io_opts).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_write(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, size_t size,
		     uint64_t offs, uint64_t flags,
		     const struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		     spdk_fsdev_write_cpl_cb cb_fn, void *cb_arg);

/**
 * Synchronize file contents operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_fsync_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Synchronize file contents
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param datasync Flag indicating if only data should be flushed.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_fsync(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, bool datasync,
		     spdk_fsdev_fsync_cpl_cb cb_fn, void *cb_arg);

/**
 * Flush operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_flush_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Flush
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_flush(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		     spdk_fsdev_flush_cpl_cb cb_fn,
		     void *cb_arg);

/**
 * Open a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fhandle File handle
 */
typedef void (spdk_fsdev_opendir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_handle *fhandle);

/**
 * Open a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_opendir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, uint32_t flags,
		       spdk_fsdev_opendir_cpl_cb cb_fn, void *cb_arg);

/**
 * Read directory per-entry callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param name Name of the entry
 * \param fobject File object. NULL for "." and "..".
 * \param attr File attributes.
 * \param offset Offset of the next entry
 *
 * \return 0 to continue the enumeration, an error code otherwise.
 */
typedef int (spdk_fsdev_readdir_entry_cb)(void *cb_arg, struct spdk_io_channel *ch,
		const char *name, struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
		off_t offset);

/**
 * Read directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_readdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Read directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param offset Offset to continue reading the directory stream
 * \param entry_cb_fn Per-entry callback.
 * \param cpl_cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_readdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       uint64_t offset,
		       spdk_fsdev_readdir_entry_cb entry_cb_fn, spdk_fsdev_readdir_cpl_cb cpl_cb_fn, void *cb_arg);

/**
 * Open a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_releasedir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status);

/**
 * Open a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_releasedir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			  spdk_fsdev_releasedir_cpl_cb cb_fn, void *cb_arg);

/**
 * Synchronize directory contents operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_fsyncdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Synchronize directory contents
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param datasync Flag indicating if only data should be flushed.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_fsyncdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			bool datasync,
			spdk_fsdev_fsyncdir_cpl_cb cb_fn, void *cb_arg);

/**
 * Acquire, modify or release a BSD file lock operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_flock_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Acquire, modify or release a BSD file lock
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object..
 * \param fhandle File handle.
 * \param operation Lock operation (see man flock, LOCK_NB will always be added).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_flock(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		     int operation, spdk_fsdev_flock_cpl_cb cb_fn, void *cb_arg);

/**
 * Allocate requested space operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_fallocate_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Allocate requested space.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object..
 * \param fhandle File handle.
 * \param mode determines the operation to be performed on the given range, see fallocate(2)
 * \param offset starting point for allocated region.
 * \param length size of allocated region.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_fallocate(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			 uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			 int mode, off_t offset, off_t length,
			 spdk_fsdev_fallocate_cpl_cb cb_fn, void *cb_arg);

/**
 * Copy a range of data from one file to another operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes written.
 */
typedef void (spdk_fsdev_copy_file_range_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status, uint32_t data_size);

/**
 * Copy a range of data from one file to another.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject_in IN File object.
 * \param fhandle_in IN File handle.
 * \param off_in Starting point from were the data should be read.
 * \param fobject_out OUT File object.
 * \param fhandle_out OUT File handle.
 * \param off_out Starting point from were the data should be written.
 * \param len Maximum size of the data to copy.
 * \param flags Operation flags, see the copy_file_range()
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_copy_file_range(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			       uint64_t unique,
			       struct spdk_fsdev_file_object *fobject_in, struct spdk_fsdev_file_handle *fhandle_in, off_t off_in,
			       struct spdk_fsdev_file_object *fobject_out, struct spdk_fsdev_file_handle *fhandle_out,
			       off_t off_out, size_t len, uint32_t flags,
			       spdk_fsdev_copy_file_range_cpl_cb cb_fn, void *cb_arg);


/**
 * I/O operation abortion completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
typedef void (spdk_fsdev_abort_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Abort an I/O
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique_to_abort Unique I/O id of the IO to abort.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
int spdk_fsdev_abort(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		     uint64_t unique_to_abort, spdk_fsdev_abort_cpl_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FSDEV_H */
