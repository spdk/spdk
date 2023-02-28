/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MD_H
#define FTL_MD_H

#include "spdk/stdinc.h"

#include "ftl_layout.h"

struct ftl_md;
struct spdk_ftl_dev;

typedef void (*ftl_md_cb)(struct spdk_ftl_dev *dev, struct ftl_md *md, int status);

enum ftl_md_ops {
	FTL_MD_OP_RESTORE,
	FTL_MD_OP_PERSIST,
	FTL_MD_OP_CLEAR,
};

typedef int (*shm_open_t)(const char *, int, mode_t);
typedef int (*shm_unlink_t)(const char *);

/* FTL metadata container which allows to store/restore/recover */
struct ftl_md {
	/* Context of owner (Caller of restore/persist/clear operation) */
	struct {
		/* Private context of the metadata's owner */
		void *private;

		/* Additional context of the owner */
		void *cb_ctx;
	} owner;

	/* Callback for signaling end of procedures like restore, persist, or clear */
	ftl_md_cb cb;

	/* Pointer to the FTL device */
	struct spdk_ftl_dev *dev;

	/* Region of device on which store/restore the metadata */
	const struct ftl_layout_region  *region;

	/* Pointer to data */
	void *data;

	/* Size of buffer in FTL block size unit */
	uint64_t data_blocks;

	/* Pointer to VSS metadata data */
	void *vss_data;

	/* Default DMA buffer for VSS of a single entry. Used by ftl_md_persist_entry(). */
	void *entry_vss_dma_buf;

	/* Fields for doing IO */
	struct {
		void *data;
		void *md;
		uint64_t address;
		uint64_t remaining;
		uint64_t data_offset;
		int status;
		enum ftl_md_ops op;
		struct spdk_bdev_io_wait_entry bdev_io_wait;
	} io;

	/* SHM object file descriptor or -1 if heap alloc */
	int shm_fd;

	/* Object name */
	char name[NAME_MAX + 1];

	/* mmap flags for the SHM object */
	int shm_mmap_flags;

	/* Total size of SHM object (data + md) */
	size_t shm_sz;

	/* open() for the SHM object */
	shm_open_t shm_open;

	/* unlink() for the SHM object */
	shm_unlink_t shm_unlink;

	/* Memory was registered to SPDK */
	bool mem_reg;

	/* Metadata primary object */
	struct ftl_md *mirror;

	/* This flag is used by the primary to disable mirror temporarily */
	bool mirror_enabled;
};

typedef void (*ftl_md_io_entry_cb)(int status, void *cb_arg);

struct ftl_md_io_entry_ctx {
	uint32_t remaining;
	int status;
	ftl_md_io_entry_cb cb;
	void *cb_arg;
	struct ftl_md *md;
	uint64_t start_entry;
	void *buffer;
	void *vss_buffer;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

#define FTL_MD_VSS_SZ	64
union ftl_md_vss {
	struct {
		uint8_t		unused[FTL_MD_VSS_SZ - sizeof(uint64_t)];
		uint64_t	md_version;
	} version;

	struct {
		uint64_t	start_lba;
		uint64_t	num_blocks;
		uint64_t	seq_id;
	} unmap;

	struct {
		uint64_t	seq_id;
		uint32_t	p2l_checksum;
	} p2l_ckpt;

	struct {
		uint64_t	lba;
		uint64_t	seq_id;
	} nv_cache;
};

SPDK_STATIC_ASSERT(sizeof(union ftl_md_vss) == FTL_MD_VSS_SZ, "Invalid md vss size");

/**
 *  FTL metadata creation flags
 */
enum ftl_md_create_flags {
	/** FTL metadata will be created without memory allocation */
	FTL_MD_CREATE_NO_MEM =		0x0,

	/** FTL metadata data buf will be allocated in SHM */
	FTL_MD_CREATE_SHM =		0x1,

	/** Always create a new SHM obj, i.e. issue shm_unlink() before shm_open(), only valid with FTL_MD_CREATE_SHM */
	FTL_MD_CREATE_SHM_NEW =		0x2,

	/** FTL metadata will be created on heap */
	FTL_MD_CREATE_HEAP =		0x4,
};

/**
 * @brief Creates FTL metadata
 *
 * @param dev The FTL device
 * @param blocks Size of buffer in FTL block size unit
 * @param vss_blksz Size of VSS MD
 * @param name Name of the object being created
 * @param flags Bit flags of ftl_md_create_flags type
 * @param region Region associated with FTL metadata
 *
 * @note if buffer is NULL, the buffer will be allocated internally by the object
 *
 * @return FTL metadata
 */
struct ftl_md *ftl_md_create(struct spdk_ftl_dev *dev, uint64_t blocks,
			     uint64_t vss_blksz, const char *name, int flags,
			     const struct ftl_layout_region *region);

/**
 * @brief Unlinks metadata object from FS
 * @param dev The FTL device
 * @param name Name of the object being unlinked
 * @param flags Bit flag describing the MD object
 *
 * @note Unlink is possible only for objects created with FTL_MD_CREATE_SHM flag
 *
 * @return Operation result
 */
int ftl_md_unlink(struct spdk_ftl_dev *dev, const char *name, int flags);

/**
 *  FTL metadata destroy flags
 */
enum ftl_md_destroy_flags {
	/** FTL metadata data buf will be kept in SHM */
	FTL_MD_DESTROY_SHM_KEEP = 0x1,
};

/**
 * @brief Destroys metadata
 *
 * @param md Metadata to be destroyed
 * @param flags Bit flags of type ftl_md_destroy_flags
 */
void ftl_md_destroy(struct ftl_md *md, int flags);

/**
 * @brief Free the data buf associated with the metadata
 *
 * @param md Metadata object
 * @param flags Bit flags of type ftl_md_destroy_flags
 */
void ftl_md_free_buf(struct ftl_md *md, int flags);

/**
 * @brief Sets the region of a device on which to perform IO when persisting,
 * restoring, or clearing.
 *
 * @param md The FTL metadata
 * @param region The device region to be set
 *
 * @return Operation status
 */
int ftl_md_set_region(struct ftl_md *md,
		      const struct ftl_layout_region *region);

/**
 * @brief Gets layout region on which ongoing an IO procedure is executed
 *
 * @param md Metadata object
 *
 * @return Layout region
 */
const struct ftl_layout_region *ftl_md_get_region(struct ftl_md *md);

/**
 * @brief Gets metadata's data buffer
 *
 * @param md The FTL metadata
 *
 * @result FTL metadata data buffer
 */
void *ftl_md_get_buffer(struct ftl_md *md);

/**
 * @brief Gets metadata object corresponding buffer size
 *
 * @param md The FTL metadata
 *
 * @return Buffer size
 */
uint64_t ftl_md_get_buffer_size(struct ftl_md *md);

/**
 * @brief Heap allocate and initialize a vss buffer for MD region.
 *
 * The buffer is aligned to FTL_BLOCK_SIZE.
 * The buffer is zeroed.
 * The VSS version is inherited from the MD region.
 *
 * @param region The MD region
 * @param count Number of VSS items to allocate
 *
 * @return VSS buffer
 */
union ftl_md_vss *ftl_md_vss_buf_alloc(struct ftl_layout_region *region, uint32_t count);

/**
 * @brief Get the VSS metadata data buffer
 *
 * @param md The FTL metadata
 *
 * @return VSS metadata data buffer
 */
union ftl_md_vss *ftl_md_get_vss_buffer(struct ftl_md *md);

/**
 * Restores metadata from the region which is set
 *
 * @param md Metadata to be restored
 */
void ftl_md_restore(struct ftl_md *md);

/**
 * Persists all metadata to the region which is set
 *
 * @param md Metadata to be persisted
 */
void ftl_md_persist(struct ftl_md *md);

/**
 * Persists given entries in metadata to the region which is set
 *
 * @param md Metadata to be persisted
 * @param start_entry Starting index of entry to be persisted
 * @param buffer DMA buffer for writing the entry to the device
 * @param vss_buffer DMA buffer for writing the entry VSS to the device
 * @param cb Completion called on persist entry end
 * @param cb_arg Context returned on completion
 * @param ctx Operation context structure
 */
void ftl_md_persist_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
			  ftl_md_io_entry_cb cb, void *cb_arg,
			  struct ftl_md_io_entry_ctx *ctx);

/**
 * Retries a persist operation performed by ftl_md_persist_entry.
 *
 * @param ctx Operation context structure.
 */
void ftl_md_persist_entry_retry(struct ftl_md_io_entry_ctx *ctx);

/**
 * Reads given entries from metadata region
 *
 * @param md Metadata to be read
 * @param start_entry Starting index of entry to be read
 * @param buffer DMA buffer for reading the entry from the device
 * @param vss_buffer DMA buffer for reading the entry VSS from the device
 * @param cb Completion called on read entry end
 * @param cb_arg Context returned on completion
 * @param ctx Operation context structure
 */
void ftl_md_read_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
		       ftl_md_io_entry_cb cb, void *cb_arg, struct ftl_md_io_entry_ctx *ctx);

/**
 * @brief Clears metadata on the region which is set
 *
 * @param md Metadata to be cleared
 * @param pattern Pattern used to initialize metadata
 * @param vss_pattern Pattern used to initialize metadata VSS
 *
 * @note size of pattern needs to be aligned to FTL device transfer size
 */
void ftl_md_clear(struct ftl_md *md, int pattern, union ftl_md_vss *vss_pattern);

/**
 * @brief Gets the number of blocks that are transferred in a single IO operation
 *
 * @param dev The FTL device
 *
 * @return Number of blocks
 */
uint64_t ftl_md_xfer_blocks(struct spdk_ftl_dev *dev);

/**
 * @brief Return the md creation flags for a given md region type
 *
 * Controls MD regions backed up on SHM via FTL_MD_CREATE_SHM.
 * FTL_MD_CREATE_SHM_NEW is added for:
 * 1. superblock upon SPDK_FTL_MODE_CREATE flag set,
 * 2. other regions if not in a fast startup mode.
 *
 * @param dev		The FTL device
 * @param region_type	MD region type
 *
 * @return MD creation flags
 */
int ftl_md_create_region_flags(struct spdk_ftl_dev *dev, int region_type);

/**
 * @brief Return the md destroy flags for a given md region type
 *
 * In a fast shutdown mode, returns FTL_MD_DESTROY_SHM_KEEP.
 * Otherwise the SHM is unlinked.
 *
 * @param dev		The FTL device
 * @param region_type	MD region type
 *
 * #return MD destroy flags
 */
int ftl_md_destroy_region_flags(struct spdk_ftl_dev *dev, int region_type);

/**
 * @brief Return the SHM-backed md creation flags
 *
 * FTL_MD_CREATE_SHM is always set.
 * FTL_MD_CREATE_SHM_NEW is added if not in a fast startup mode.
 *
 * @param dev	The FTL device
 *
 * @return MD creation flags
 */
int ftl_md_create_shm_flags(struct spdk_ftl_dev *dev);

/**
 * @brief Return the md destroy flags
 *
 * In a fast shutdown mode, returns FTL_MD_DESTROY_SHM_KEEP.
 * Otherwise the SHM is unlinked.
 *
 * @param dev			The FTL device
 *
 * @return MD destroy flags
 */
int ftl_md_destroy_shm_flags(struct spdk_ftl_dev *dev);
#endif /* FTL_MD_H */
