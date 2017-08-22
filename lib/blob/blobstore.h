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

#ifndef SPDK_BLOBSTORE_H
#define SPDK_BLOBSTORE_H

#include "spdk/assert.h"
#include "spdk/blob.h"
#include "spdk/queue.h"
#include "spdk/util.h"

/* In Memory Data Structures
 *
 * The following data structures exist only in memory.
 */

#define SPDK_BLOB_OPTS_CLUSTER_SZ (1024 * 1024)
#define SPDK_BLOB_OPTS_NUM_MD_PAGES UINT32_MAX
#define SPDK_BLOB_OPTS_MAX_MD_OPS 32
#define SPDK_BLOB_OPTS_MAX_CHANNEL_OPS 512

struct spdk_xattr {
	/* TODO: reorder for best packing */
	uint32_t	index;
	char		*name;
	void		*value;
	uint16_t	value_len;
	TAILQ_ENTRY(spdk_xattr)	link;
};

/* The mutable part of the blob data that is sync'd to
 * disk. The data in here is both mutable and persistent.
 */
struct spdk_blob_mut_data {
	/* Number of data clusters in the blob */
	uint64_t	num_clusters;

	/* Array LBAs that are the beginning of a cluster, in
	 * the order they appear in the blob.
	 */
	uint64_t	*clusters;

	/* The size of the clusters array. This is greater than or
	 * equal to 'num_clusters'.
	 */
	size_t		cluster_array_size;

	/* Number of metadata pages */
	uint32_t	num_pages;

	/* Array of page offsets into the metadata region, in
	 * the order of the metadata page sequence.
	 */
	uint32_t	*pages;
};

enum spdk_blob_state {
	/* The blob in-memory version does not match the on-disk
	 * version.
	 */
	SPDK_BLOB_STATE_DIRTY,

	/* The blob in memory version of the blob matches the on disk
	 * version.
	 */
	SPDK_BLOB_STATE_CLEAN,

	/* The in-memory state being synchronized with the on-disk
	 * blob state. */
	SPDK_BLOB_STATE_LOADING,

	/* The disk state is being synchronized with the current
	 * blob state.
	 */
	SPDK_BLOB_STATE_SYNCING,
};

struct spdk_blob {
	struct spdk_blob_store *bs;

	uint32_t	open_ref;

	spdk_blob_id	id;

	enum spdk_blob_state		state;

	/* Two copies of the mutable data. One is a version
	 * that matches the last known data on disk (clean).
	 * The other (active) is the current data. Syncing
	 * a blob makes the clean match the active.
	 */
	struct spdk_blob_mut_data	clean;
	struct spdk_blob_mut_data	active;

	/* TODO: The xattrs are mutable, but we don't want to be
	 * copying them unecessarily. Figure this out.
	 */
	TAILQ_HEAD(, spdk_xattr) xattrs;

	TAILQ_ENTRY(spdk_blob) link;
};

struct spdk_blob_store {
	uint64_t			md_start; /* Offset from beginning of disk, in pages */
	uint32_t			md_len; /* Count, in pages */

	struct {
		uint32_t		max_md_ops;
		struct spdk_io_channel	*md_channel;
	} md_target;

	struct {
		uint32_t		max_channel_ops;
	} io_target;


	struct spdk_bs_dev		*dev;

	struct spdk_bit_array		*used_md_pages;
	struct spdk_bit_array		*used_clusters;

	uint32_t			cluster_sz;
	uint64_t			total_clusters;
	uint64_t			num_free_clusters;
	uint32_t			pages_per_cluster;

	spdk_blob_id			super_blob;

	TAILQ_HEAD(, spdk_blob) 	blobs;
};

struct spdk_bs_channel {
	struct spdk_bs_request_set	*req_mem;
	TAILQ_HEAD(, spdk_bs_request_set) reqs;

	struct spdk_blob_store		*bs;

	struct spdk_bs_dev		*dev;
	struct spdk_io_channel		*dev_channel;
};

/* On-Disk Data Structures
 *
 * The following data structures exist on disk.
 */
#define SPDK_BS_VERSION 1

#pragma pack(push, 1)

#define SPDK_MD_MASK_TYPE_USED_PAGES 0
#define SPDK_MD_MASK_TYPE_USED_CLUSTERS 1

struct spdk_bs_md_mask {
	uint8_t		type;
	uint32_t	length; /* In bits */
	uint8_t		mask[0];
};

#define SPDK_MD_DESCRIPTOR_TYPE_PADDING 0
#define SPDK_MD_DESCRIPTOR_TYPE_EXTENT 1
#define SPDK_MD_DESCRIPTOR_TYPE_XATTR 2

struct spdk_blob_md_descriptor_xattr {
	uint8_t		type;
	uint32_t	length;

	uint16_t	name_length;
	uint16_t	value_length;

	char		name[0];
	/* String name immediately followed by string value. */
};

struct spdk_blob_md_descriptor_extent {
	uint8_t		type;
	uint32_t	length;

	struct {
		uint32_t        cluster_idx;
		uint32_t        length; /* In units of clusters */
	} extents[0];
};

struct spdk_blob_md_descriptor {
	uint8_t		type;
	uint32_t	length;
};

#define SPDK_INVALID_MD_PAGE UINT32_MAX

struct spdk_blob_md_page {
	spdk_blob_id     id;

	uint32_t        sequence_num;
	uint32_t	reserved0;

	/* Descriptors here */
	uint8_t		descriptors[4072];

	uint32_t	next;
	uint32_t	crc;
};
#define SPDK_BS_PAGE_SIZE 0x1000
SPDK_STATIC_ASSERT(SPDK_BS_PAGE_SIZE == sizeof(struct spdk_blob_md_page), "Invalid md page size");

#define SPDK_BS_SUPER_BLOCK_SIG "SPDKBLOB"

struct spdk_bs_super_block {
	uint8_t		signature[8];
	uint32_t        version;
	uint32_t        length;
	uint32_t	clean; /* If there was a clean shutdown, this is 1. */
	spdk_blob_id	super_blob;

	uint32_t	cluster_size; /* In bytes */

	uint32_t	used_page_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_page_mask_len; /* Count, in pages */

	uint32_t	used_cluster_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_cluster_mask_len; /* Count, in pages */

	uint32_t	md_start; /* Offset from beginning of disk, in pages */
	uint32_t	md_len; /* Count, in pages */

	uint8_t		reserved[4040];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_super_block) == 0x1000, "Invalid super block size");

#pragma pack(pop)

/* Unit Conversions
 *
 * The blobstore works with several different units:
 * - Byte: Self explanatory
 * - LBA: The logical blocks on the backing storage device.
 * - Page: The read/write units of blobs and metadata. This is
 *         an offset into a blob in units of 4KiB.
 * - Cluster Index: The disk is broken into a sequential list of
 *		    clusters. This is the offset from the beginning.
 *
 * NOTE: These conversions all act on simple magnitudes, not with any sort
 *        of knowledge about the blobs themselves. For instance, converting
 *        a page to an lba with the conversion function below simply converts
 *        a number of pages to an equivalent number of lbas, but that
 *        lba certainly isn't the right lba that corresponds to a page offset
 *        for a particular blob.
 */
static inline uint64_t
_spdk_bs_byte_to_lba(struct spdk_blob_store *bs, uint64_t length)
{
	assert(length % bs->dev->blocklen == 0);

	return length / bs->dev->blocklen;
}

static inline uint64_t
_spdk_bs_lba_to_byte(struct spdk_blob_store *bs, uint64_t lba)
{
	return lba * bs->dev->blocklen;
}

static inline uint64_t
_spdk_bs_page_to_lba(struct spdk_blob_store *bs, uint64_t page)
{
	return page * SPDK_BS_PAGE_SIZE / bs->dev->blocklen;
}

static inline uint32_t
_spdk_bs_lba_to_page(struct spdk_blob_store *bs, uint64_t lba)
{
	uint64_t	lbas_per_page;

	lbas_per_page = SPDK_BS_PAGE_SIZE / bs->dev->blocklen;

	assert(lba % lbas_per_page == 0);

	return lba / lbas_per_page;
}

static inline uint64_t
_spdk_bs_cluster_to_page(struct spdk_blob_store *bs, uint32_t cluster)
{
	return cluster * bs->pages_per_cluster;
}

static inline uint32_t
_spdk_bs_page_to_cluster(struct spdk_blob_store *bs, uint64_t page)
{
	assert(page % bs->pages_per_cluster == 0);

	return page / bs->pages_per_cluster;
}

static inline uint64_t
_spdk_bs_cluster_to_lba(struct spdk_blob_store *bs, uint32_t cluster)
{
	return cluster * (bs->cluster_sz / bs->dev->blocklen);
}

static inline uint32_t
_spdk_bs_lba_to_cluster(struct spdk_blob_store *bs, uint64_t lba)
{
	assert(lba % (bs->cluster_sz / bs->dev->blocklen) == 0);

	return lba / (bs->cluster_sz / bs->dev->blocklen);
}

/* End basic conversions */

static inline uint32_t
_spdk_bs_blobid_to_page(spdk_blob_id id)
{
	return id & 0xFFFFFFFF;
}

/* Given a page offset into a blob, look up the LBA for the
 * start of that page.
 */
static inline uint64_t
_spdk_bs_blob_page_to_lba(struct spdk_blob *blob, uint32_t page)
{
	uint64_t	lba;
	uint32_t	pages_per_cluster;

	pages_per_cluster = blob->bs->pages_per_cluster;

	assert(page < blob->active.num_clusters * pages_per_cluster);

	lba = blob->active.clusters[page / pages_per_cluster];
	lba += _spdk_bs_page_to_lba(blob->bs, page % pages_per_cluster);

	return lba;
}

/* Given a page offset into a blob, look up the number of pages until the
 * next cluster boundary.
 */
static inline uint32_t
_spdk_bs_num_pages_to_cluster_boundary(struct spdk_blob *blob, uint32_t page)
{
	uint32_t	pages_per_cluster;

	pages_per_cluster = blob->bs->pages_per_cluster;

	return pages_per_cluster - (page % pages_per_cluster);
}

#endif
