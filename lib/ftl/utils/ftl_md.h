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
#ifndef FTL_OBJ_H
#define FTL_OBJ_H

#include "spdk/stdinc.h"

#include "ftl_layout.h"

struct ftl_md;
struct spdk_ftl_dev;

typedef void (*ftl_md_cb)(struct spdk_ftl_dev *dev, struct ftl_md *md, int status);

/* FTL metadata container which allows to store/restore/recover */
struct ftl_md {
	/* Context of owner */
	struct {
		/* Private context of the metadata's owner */
		void                    *private;

		/* Additional context of the owner */
		void                    *cb_ctx;

		/* Additional callback of the owner */
		void (*cb)(struct spdk_ftl_dev *dev, int status, void *ctx);
	} owner;

	/* Callback for signaling end of procedures like restore, persist, or clear */
	ftl_md_cb                      cb;
};

typedef void (*ftl_md_io_entry_cb)(int status, void *cb_arg);

struct ftl_md_impl;

struct ftl_md_io_entry_ctx {
	uint32_t remaining;
	int status;
	ftl_md_io_entry_cb cb;
	void *cb_arg;
	struct ftl_md_impl *md;
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
	} unmap;

	struct {
		uint64_t	lba;
	} nv_cache;
};

SPDK_STATIC_ASSERT(sizeof(union ftl_md_vss) == FTL_MD_VSS_SZ, "Invalid md vss size");

/**
 * @brief Creates FTL metadata
 *
 * @param dev The FTL device
 * @param blocks Size of buffer in FTL block size unit
 * @param vss_blksz Size of VSS MD
 * @param name Name of the object being created
 * @param no_mem If true metadata will be created without memory allocation
 *
 * @note if buffer is NULL, the buffer will be allocated internally by the object
 *
 * @return FTL metadata
 */
struct ftl_md *ftl_md_create(struct spdk_ftl_dev *dev, uint64_t blocks,
			     uint64_t vss_blksz, const char *name, bool no_mem);

/**
 * @brief Destroys metadata
 *
 * @param md Metadata to be destroyed
 */
void ftl_md_destroy(struct ftl_md *md);

/**
 * @brief Free the data buf associated with the metadata
 *
 * @param md Metadata object
 */
void ftl_md_free_buf(struct ftl_md *md);

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
 * @param size Size of pattern in bytes
 * @param vss_pattern Pattern used to initialize metadata VSS
 *
 * @note size of pattern needs to be aligned to FTL device transfer size
 */
void ftl_md_clear(struct ftl_md *md, const void *pattern, uint64_t size,
			  union ftl_md_vss *vss_pattern);

/**
 * @brief Gets the number of blocks that are transfered in a single IO operation
 *
 * @param dev The FTL device
 *
 * @return Number of blocks
 */
uint64_t ftl_md_xfer_blocks(struct spdk_ftl_dev *dev);

#endif /* FTL_OBJ_H */
