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

#include "spdk/stdinc.h"

#include "spdk/blob.h"
#include "spdk/crc32.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/bit_array.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

#include "blobstore.h"

#define BLOB_CRC32C_INITIAL    0xffffffffUL

static int spdk_bs_register_md_thread(struct spdk_blob_store *bs);
static int spdk_bs_unregister_md_thread(struct spdk_blob_store *bs);
static void _spdk_blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno);
static void _spdk_blob_insert_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num,
		uint64_t cluster, uint32_t extent, spdk_blob_op_complete cb_fn, void *cb_arg);

static int _spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
				uint16_t value_len, bool internal);
static int _spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
				      const void **value, size_t *value_len, bool internal);
static int _spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name, bool internal);

static void _spdk_blob_insert_extent(struct spdk_blob *blob, uint32_t extent, uint64_t cluster_num,
				     spdk_blob_op_complete cb_fn, void *cb_arg);

static void
_spdk_blob_verify_md_op(struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(spdk_get_thread() == blob->bs->md_thread);
	assert(blob->state != SPDK_BLOB_STATE_LOADING);
}

static struct spdk_blob_list *
_spdk_bs_get_snapshot_entry(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	struct spdk_blob_list *snapshot_entry = NULL;

	TAILQ_FOREACH(snapshot_entry, &bs->snapshots, link) {
		if (snapshot_entry->id == blobid) {
			break;
		}
	}

	return snapshot_entry;
}

static void
_spdk_bs_claim_md_page(struct spdk_blob_store *bs, uint32_t page)
{
	assert(page < spdk_bit_array_capacity(bs->used_md_pages));
	assert(spdk_bit_array_get(bs->used_md_pages, page) == false);

	spdk_bit_array_set(bs->used_md_pages, page);
}

static void
_spdk_bs_release_md_page(struct spdk_blob_store *bs, uint32_t page)
{
	assert(page < spdk_bit_array_capacity(bs->used_md_pages));
	assert(spdk_bit_array_get(bs->used_md_pages, page) == true);

	spdk_bit_array_clear(bs->used_md_pages, page);
}

static void
_spdk_bs_claim_cluster(struct spdk_blob_store *bs, uint32_t cluster_num)
{
	assert(cluster_num < spdk_bit_array_capacity(bs->used_clusters));
	assert(spdk_bit_array_get(bs->used_clusters, cluster_num) == false);
	assert(bs->num_free_clusters > 0);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming cluster %u\n", cluster_num);

	spdk_bit_array_set(bs->used_clusters, cluster_num);
	bs->num_free_clusters--;
}

static int
_spdk_blob_insert_cluster(struct spdk_blob *blob, uint32_t cluster_num, uint64_t cluster)
{
	uint64_t *cluster_lba = &blob->active.clusters[cluster_num];

	_spdk_blob_verify_md_op(blob);

	if (*cluster_lba != 0) {
		return -EEXIST;
	}

	*cluster_lba = _spdk_bs_cluster_to_lba(blob->bs, cluster);
	return 0;
}

static int
_spdk_bs_allocate_cluster(struct spdk_blob *blob, uint32_t cluster_num,
			  uint64_t *lowest_free_cluster, uint32_t *lowest_free_md_page, bool update_map)
{
	uint32_t *extent_page = 0;

	pthread_mutex_lock(&blob->bs->used_clusters_mutex);
	*lowest_free_cluster = spdk_bit_array_find_first_clear(blob->bs->used_clusters,
			       *lowest_free_cluster);
	if (*lowest_free_cluster == UINT32_MAX) {
		/* No more free clusters. Cannot satisfy the request */
		pthread_mutex_unlock(&blob->bs->used_clusters_mutex);
		return -ENOSPC;
	}

	if (blob->use_extent_table) {
		extent_page = _spdk_bs_cluster_to_extent_page(blob, cluster_num);
		if (*extent_page == 0) {
			/* No extent_page is allocated for the cluster */
			*lowest_free_md_page = spdk_bit_array_find_first_clear(blob->bs->used_md_pages,
					       *lowest_free_md_page);
			if (*lowest_free_md_page == UINT32_MAX) {
				/* No more free md pages. Cannot satisfy the request */
				pthread_mutex_unlock(&blob->bs->used_clusters_mutex);
				return -ENOSPC;
			}
			_spdk_bs_claim_md_page(blob->bs, *lowest_free_md_page);
		}
	}

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming cluster %lu for blob %lu\n", *lowest_free_cluster, blob->id);
	_spdk_bs_claim_cluster(blob->bs, *lowest_free_cluster);

	pthread_mutex_unlock(&blob->bs->used_clusters_mutex);

	if (update_map) {
		_spdk_blob_insert_cluster(blob, cluster_num, *lowest_free_cluster);
		if (blob->use_extent_table && *extent_page == 0) {
			*extent_page = *lowest_free_md_page;
		}
	}

	return 0;
}

static void
_spdk_bs_release_cluster(struct spdk_blob_store *bs, uint32_t cluster_num)
{
	assert(cluster_num < spdk_bit_array_capacity(bs->used_clusters));
	assert(spdk_bit_array_get(bs->used_clusters, cluster_num) == true);
	assert(bs->num_free_clusters < bs->total_clusters);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Releasing cluster %u\n", cluster_num);

	pthread_mutex_lock(&bs->used_clusters_mutex);
	spdk_bit_array_clear(bs->used_clusters, cluster_num);
	bs->num_free_clusters++;
	pthread_mutex_unlock(&bs->used_clusters_mutex);
}

static void
_spdk_blob_xattrs_init(struct spdk_blob_xattr_opts *xattrs)
{
	xattrs->count = 0;
	xattrs->names = NULL;
	xattrs->ctx = NULL;
	xattrs->get_value = NULL;
}

void
spdk_blob_opts_init(struct spdk_blob_opts *opts)
{
	opts->num_clusters = 0;
	opts->thin_provision = false;
	opts->clear_method = BLOB_CLEAR_WITH_DEFAULT;
	_spdk_blob_xattrs_init(&opts->xattrs);
	opts->use_extent_table = true;
}

void
spdk_blob_open_opts_init(struct spdk_blob_open_opts *opts)
{
	opts->clear_method = BLOB_CLEAR_WITH_DEFAULT;
}

static struct spdk_blob *
_spdk_blob_alloc(struct spdk_blob_store *bs, spdk_blob_id id)
{
	struct spdk_blob *blob;

	blob = calloc(1, sizeof(*blob));
	if (!blob) {
		return NULL;
	}

	blob->id = id;
	blob->bs = bs;

	blob->parent_id = SPDK_BLOBID_INVALID;

	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->extent_rle_found = false;
	blob->extent_table_found = false;
	blob->active.num_pages = 1;
	blob->active.pages = calloc(1, sizeof(*blob->active.pages));
	if (!blob->active.pages) {
		free(blob);
		return NULL;
	}

	blob->active.pages[0] = _spdk_bs_blobid_to_page(id);

	TAILQ_INIT(&blob->xattrs);
	TAILQ_INIT(&blob->xattrs_internal);
	TAILQ_INIT(&blob->pending_persists);

	return blob;
}

static void
_spdk_xattrs_free(struct spdk_xattr_tailq *xattrs)
{
	struct spdk_xattr	*xattr, *xattr_tmp;

	TAILQ_FOREACH_SAFE(xattr, xattrs, link, xattr_tmp) {
		TAILQ_REMOVE(xattrs, xattr, link);
		free(xattr->name);
		free(xattr->value);
		free(xattr);
	}
}

static void
_spdk_blob_free(struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(TAILQ_EMPTY(&blob->pending_persists));

	free(blob->active.extent_pages);
	free(blob->clean.extent_pages);
	free(blob->active.clusters);
	free(blob->clean.clusters);
	free(blob->active.pages);
	free(blob->clean.pages);

	_spdk_xattrs_free(&blob->xattrs);
	_spdk_xattrs_free(&blob->xattrs_internal);

	if (blob->back_bs_dev) {
		blob->back_bs_dev->destroy(blob->back_bs_dev);
	}

	free(blob);
}

struct freeze_io_ctx {
	struct spdk_bs_cpl cpl;
	struct spdk_blob *blob;
};

static void
_spdk_blob_io_sync(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, 0);
}

static void
_spdk_blob_execute_queued_io(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_bs_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct freeze_io_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bs_request_set	*set;
	struct spdk_bs_user_op_args	*args;
	spdk_bs_user_op_t *op, *tmp;

	TAILQ_FOREACH_SAFE(op, &ch->queued_io, link, tmp) {
		set = (struct spdk_bs_request_set *)op;
		args = &set->u.user_op;

		if (args->blob == ctx->blob) {
			TAILQ_REMOVE(&ch->queued_io, op, link);
			bs_user_op_execute(op);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_spdk_blob_io_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct freeze_io_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->cpl.u.blob_basic.cb_fn(ctx->cpl.u.blob_basic.cb_arg, 0);

	free(ctx);
}

static void
_spdk_blob_freeze_io(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct freeze_io_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	ctx->cpl.u.blob_basic.cb_fn = cb_fn;
	ctx->cpl.u.blob_basic.cb_arg = cb_arg;
	ctx->blob = blob;

	/* Freeze I/O on blob */
	blob->frozen_refcnt++;

	if (blob->frozen_refcnt == 1) {
		spdk_for_each_channel(blob->bs, _spdk_blob_io_sync, ctx, _spdk_blob_io_cpl);
	} else {
		cb_fn(cb_arg, 0);
		free(ctx);
	}
}

static void
_spdk_blob_unfreeze_io(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct freeze_io_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	ctx->cpl.u.blob_basic.cb_fn = cb_fn;
	ctx->cpl.u.blob_basic.cb_arg = cb_arg;
	ctx->blob = blob;

	assert(blob->frozen_refcnt > 0);

	blob->frozen_refcnt--;

	if (blob->frozen_refcnt == 0) {
		spdk_for_each_channel(blob->bs, _spdk_blob_execute_queued_io, ctx, _spdk_blob_io_cpl);
	} else {
		cb_fn(cb_arg, 0);
		free(ctx);
	}
}

static int
_spdk_blob_mark_clean(struct spdk_blob *blob)
{
	uint32_t *extent_pages = NULL;
	uint64_t *clusters = NULL;
	uint32_t *pages = NULL;

	assert(blob != NULL);

	if (blob->active.num_extent_pages) {
		assert(blob->active.extent_pages);
		extent_pages = calloc(blob->active.num_extent_pages, sizeof(*blob->active.extent_pages));
		if (!extent_pages) {
			return -ENOMEM;
		}
		memcpy(extent_pages, blob->active.extent_pages,
		       blob->active.num_extent_pages * sizeof(*extent_pages));
	}

	if (blob->active.num_clusters) {
		assert(blob->active.clusters);
		clusters = calloc(blob->active.num_clusters, sizeof(*blob->active.clusters));
		if (!clusters) {
			free(extent_pages);
			return -ENOMEM;
		}
		memcpy(clusters, blob->active.clusters, blob->active.num_clusters * sizeof(*blob->active.clusters));
	}

	if (blob->active.num_pages) {
		assert(blob->active.pages);
		pages = calloc(blob->active.num_pages, sizeof(*blob->active.pages));
		if (!pages) {
			free(extent_pages);
			free(clusters);
			return -ENOMEM;
		}
		memcpy(pages, blob->active.pages, blob->active.num_pages * sizeof(*blob->active.pages));
	}

	free(blob->clean.extent_pages);
	free(blob->clean.clusters);
	free(blob->clean.pages);

	blob->clean.num_extent_pages = blob->active.num_extent_pages;
	blob->clean.extent_pages = blob->active.extent_pages;
	blob->clean.num_clusters = blob->active.num_clusters;
	blob->clean.clusters = blob->active.clusters;
	blob->clean.num_pages = blob->active.num_pages;
	blob->clean.pages = blob->active.pages;

	blob->active.extent_pages = extent_pages;
	blob->active.clusters = clusters;
	blob->active.pages = pages;

	/* If the metadata was dirtied again while the metadata was being written to disk,
	 *  we do not want to revert the DIRTY state back to CLEAN here.
	 */
	if (blob->state == SPDK_BLOB_STATE_LOADING) {
		blob->state = SPDK_BLOB_STATE_CLEAN;
	}

	return 0;
}

static int
_spdk_blob_deserialize_xattr(struct spdk_blob *blob,
			     struct spdk_blob_md_descriptor_xattr *desc_xattr, bool internal)
{
	struct spdk_xattr                       *xattr;

	if (desc_xattr->length != sizeof(desc_xattr->name_length) +
	    sizeof(desc_xattr->value_length) +
	    desc_xattr->name_length + desc_xattr->value_length) {
		return -EINVAL;
	}

	xattr = calloc(1, sizeof(*xattr));
	if (xattr == NULL) {
		return -ENOMEM;
	}

	xattr->name = malloc(desc_xattr->name_length + 1);
	if (xattr->name == NULL) {
		free(xattr);
		return -ENOMEM;
	}
	memcpy(xattr->name, desc_xattr->name, desc_xattr->name_length);
	xattr->name[desc_xattr->name_length] = '\0';

	xattr->value = malloc(desc_xattr->value_length);
	if (xattr->value == NULL) {
		free(xattr->name);
		free(xattr);
		return -ENOMEM;
	}
	xattr->value_len = desc_xattr->value_length;
	memcpy(xattr->value,
	       (void *)((uintptr_t)desc_xattr->name + desc_xattr->name_length),
	       desc_xattr->value_length);

	TAILQ_INSERT_TAIL(internal ? &blob->xattrs_internal : &blob->xattrs, xattr, link);

	return 0;
}


static int
_spdk_blob_parse_page(const struct spdk_blob_md_page *page, struct spdk_blob *blob)
{
	struct spdk_blob_md_descriptor *desc;
	size_t	cur_desc = 0;
	void *tmp;

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			struct spdk_blob_md_descriptor_flags	*desc_flags;

			desc_flags = (struct spdk_blob_md_descriptor_flags *)desc;

			if (desc_flags->length != sizeof(*desc_flags) - sizeof(*desc)) {
				return -EINVAL;
			}

			if ((desc_flags->invalid_flags | SPDK_BLOB_INVALID_FLAGS_MASK) !=
			    SPDK_BLOB_INVALID_FLAGS_MASK) {
				return -EINVAL;
			}

			if ((desc_flags->data_ro_flags | SPDK_BLOB_DATA_RO_FLAGS_MASK) !=
			    SPDK_BLOB_DATA_RO_FLAGS_MASK) {
				blob->data_ro = true;
				blob->md_ro = true;
			}

			if ((desc_flags->md_ro_flags | SPDK_BLOB_MD_RO_FLAGS_MASK) !=
			    SPDK_BLOB_MD_RO_FLAGS_MASK) {
				blob->md_ro = true;
			}

			if ((desc_flags->data_ro_flags & SPDK_BLOB_READ_ONLY)) {
				blob->data_ro = true;
				blob->md_ro = true;
			}

			blob->invalid_flags = desc_flags->invalid_flags;
			blob->data_ro_flags = desc_flags->data_ro_flags;
			blob->md_ro_flags = desc_flags->md_ro_flags;

		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i, j;
			unsigned int				cluster_count = blob->active.num_clusters;

			if (blob->extent_table_found) {
				/* Extent Table already present in the md,
				 * both descriptors should never be at the same time. */
				return -EINVAL;
			}
			blob->extent_rle_found = true;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			if (desc_extent_rle->length == 0 ||
			    (desc_extent_rle->length % sizeof(desc_extent_rle->extents[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					if (desc_extent_rle->extents[i].cluster_idx != 0) {
						if (!spdk_bit_array_get(blob->bs->used_clusters,
									desc_extent_rle->extents[i].cluster_idx + j)) {
							return -EINVAL;
						}
					}
					cluster_count++;
				}
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}
			tmp = realloc(blob->active.clusters, cluster_count * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = cluster_count;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					if (desc_extent_rle->extents[i].cluster_idx != 0) {
						blob->active.clusters[blob->active.num_clusters++] = _spdk_bs_cluster_to_lba(blob->bs,
								desc_extent_rle->extents[i].cluster_idx + j);
					} else if (spdk_blob_is_thin_provisioned(blob)) {
						blob->active.clusters[blob->active.num_clusters++] = 0;
					} else {
						return -EINVAL;
					}
				}
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE) {
			struct spdk_blob_md_descriptor_extent_table *desc_extent_table;
			uint32_t num_extent_pages = blob->active.num_extent_pages;
			uint32_t i, j;
			size_t extent_pages_length;

			desc_extent_table = (struct spdk_blob_md_descriptor_extent_table *)desc;
			extent_pages_length = desc_extent_table->length - sizeof(desc_extent_table->num_clusters);

			if (blob->extent_rle_found) {
				/* This means that Extent RLE is present in MD,
				 * both should never be at the same time. */
				return -EINVAL;
			} else if (blob->extent_table_found &&
				   desc_extent_table->num_clusters != blob->remaining_clusters_in_et) {
				/* Number of clusters in this ET does not match number
				 * from previously read EXTENT_TABLE. */
				return -EINVAL;
			}

			blob->extent_table_found = true;

			if (desc_extent_table->length == 0 ||
			    (extent_pages_length % sizeof(desc_extent_table->extent_page[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				num_extent_pages += desc_extent_table->extent_page[i].num_pages;
			}

			tmp = realloc(blob->active.extent_pages, num_extent_pages * sizeof(uint32_t));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.extent_pages = tmp;
			blob->active.extent_pages_array_size = num_extent_pages;

			blob->remaining_clusters_in_et = desc_extent_table->num_clusters;

			/* Extent table entries contain md page numbers for extent pages.
			 * Zeroes represent unallocated extent pages, those are run-length-encoded.
			 */
			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				if (desc_extent_table->extent_page[i].page_idx != 0) {
					assert(desc_extent_table->extent_page[i].num_pages == 1);
					blob->active.extent_pages[blob->active.num_extent_pages++] =
						desc_extent_table->extent_page[i].page_idx;
				} else if (spdk_blob_is_thin_provisioned(blob)) {
					for (j = 0; j < desc_extent_table->extent_page[i].num_pages; j++) {
						blob->active.extent_pages[blob->active.num_extent_pages++] = 0;
					}
				} else {
					return -EINVAL;
				}
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			unsigned int					i;
			unsigned int					cluster_count = 0;
			size_t						cluster_idx_length;

			if (blob->extent_rle_found) {
				/* This means that Extent RLE is present in MD,
				 * both should never be at the same time. */
				return -EINVAL;
			}

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;
			cluster_idx_length = desc_extent->length - sizeof(desc_extent->start_cluster_idx);

			if (desc_extent->length <= sizeof(desc_extent->start_cluster_idx) ||
			    (cluster_idx_length % sizeof(desc_extent->cluster_idx[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					if (!spdk_bit_array_get(blob->bs->used_clusters, desc_extent->cluster_idx[i])) {
						return -EINVAL;
					}
				}
				cluster_count++;
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}

			/* When reading extent pages sequentially starting cluster idx should match
			 * current size of a blob.
			 * If changed to batch reading, this check shall be removed. */
			if (desc_extent->start_cluster_idx != blob->active.num_clusters) {
				return -EINVAL;
			}

			tmp = realloc(blob->active.clusters,
				      (cluster_count + blob->active.num_clusters) * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = (cluster_count + blob->active.num_clusters);

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					blob->active.clusters[blob->active.num_clusters++] = _spdk_bs_cluster_to_lba(blob->bs,
							desc_extent->cluster_idx[i]);
				} else if (spdk_blob_is_thin_provisioned(blob)) {
					blob->active.clusters[blob->active.num_clusters++] = 0;
				} else {
					return -EINVAL;
				}
			}
			assert(desc_extent->start_cluster_idx + cluster_count == blob->active.num_clusters);
			assert(blob->remaining_clusters_in_et >= cluster_count);
			blob->remaining_clusters_in_et -= cluster_count;
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			int rc;

			rc = _spdk_blob_deserialize_xattr(blob,
							  (struct spdk_blob_md_descriptor_xattr *) desc, false);
			if (rc != 0) {
				return rc;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			int rc;

			rc = _spdk_blob_deserialize_xattr(blob,
							  (struct spdk_blob_md_descriptor_xattr *) desc, true);
			if (rc != 0) {
				return rc;
			}
		} else {
			/* Unrecognized descriptor type.  Do not fail - just continue to the
			 *  next descriptor.  If this descriptor is associated with some feature
			 *  defined in a newer version of blobstore, that version of blobstore
			 *  should create and set an associated feature flag to specify if this
			 *  blob can be loaded or not.
			 */
		}

		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}

	return 0;
}

static bool _spdk_bs_load_cur_extent_page_valid(struct spdk_blob_md_page *page);

static int
_spdk_blob_parse_extent_page(struct spdk_blob_md_page *extent_page, struct spdk_blob *blob)
{
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);

	if (_spdk_bs_load_cur_extent_page_valid(extent_page) == false) {
		return -ENOENT;
	}

	return _spdk_blob_parse_page(extent_page, blob);
}

static int
_spdk_blob_parse(const struct spdk_blob_md_page *pages, uint32_t page_count,
		 struct spdk_blob *blob)
{
	const struct spdk_blob_md_page *page;
	uint32_t i;
	int rc;

	assert(page_count > 0);
	assert(pages[0].sequence_num == 0);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);
	assert(blob->active.clusters == NULL);

	/* The blobid provided doesn't match what's in the MD, this can
	 * happen for example if a bogus blobid is passed in through open.
	 */
	if (blob->id != pages[0].id) {
		SPDK_ERRLOG("Blobid (%lu) doesn't match what's in metadata (%lu)\n",
			    blob->id, pages[0].id);
		return -ENOENT;
	}

	for (i = 0; i < page_count; i++) {
		page = &pages[i];

		assert(page->id == blob->id);
		assert(page->sequence_num == i);

		rc = _spdk_blob_parse_page(page, blob);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
_spdk_blob_serialize_add_page(const struct spdk_blob *blob,
			      struct spdk_blob_md_page **pages,
			      uint32_t *page_count,
			      struct spdk_blob_md_page **last_page)
{
	struct spdk_blob_md_page *page;

	assert(pages != NULL);
	assert(page_count != NULL);

	if (*page_count == 0) {
		assert(*pages == NULL);
		*page_count = 1;
		*pages = spdk_malloc(SPDK_BS_PAGE_SIZE, SPDK_BS_PAGE_SIZE,
				     NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	} else {
		assert(*pages != NULL);
		(*page_count)++;
		*pages = spdk_realloc(*pages,
				      SPDK_BS_PAGE_SIZE * (*page_count),
				      SPDK_BS_PAGE_SIZE);
	}

	if (*pages == NULL) {
		*page_count = 0;
		*last_page = NULL;
		return -ENOMEM;
	}

	page = &(*pages)[*page_count - 1];
	memset(page, 0, sizeof(*page));
	page->id = blob->id;
	page->sequence_num = *page_count - 1;
	page->next = SPDK_INVALID_MD_PAGE;
	*last_page = page;

	return 0;
}

/* Transform the in-memory representation 'xattr' into an on-disk xattr descriptor.
 * Update required_sz on both success and failure.
 *
 */
static int
_spdk_blob_serialize_xattr(const struct spdk_xattr *xattr,
			   uint8_t *buf, size_t buf_sz,
			   size_t *required_sz, bool internal)
{
	struct spdk_blob_md_descriptor_xattr	*desc;

	*required_sz = sizeof(struct spdk_blob_md_descriptor_xattr) +
		       strlen(xattr->name) +
		       xattr->value_len;

	if (buf_sz < *required_sz) {
		return -1;
	}

	desc = (struct spdk_blob_md_descriptor_xattr *)buf;

	desc->type = internal ? SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL : SPDK_MD_DESCRIPTOR_TYPE_XATTR;
	desc->length = sizeof(desc->name_length) +
		       sizeof(desc->value_length) +
		       strlen(xattr->name) +
		       xattr->value_len;
	desc->name_length = strlen(xattr->name);
	desc->value_length = xattr->value_len;

	memcpy(desc->name, xattr->name, desc->name_length);
	memcpy((void *)((uintptr_t)desc->name + desc->name_length),
	       xattr->value,
	       desc->value_length);

	return 0;
}

static void
_spdk_blob_serialize_extent_table_entry(const struct spdk_blob *blob,
					uint64_t start_ep, uint64_t *next_ep,
					uint8_t **buf, size_t *remaining_sz)
{
	struct spdk_blob_md_descriptor_extent_table *desc;
	size_t cur_sz;
	uint64_t i, et_idx;
	uint32_t extent_page, ep_len;

	/* The buffer must have room for at least num_clusters entry */
	cur_sz = sizeof(struct spdk_blob_md_descriptor) + sizeof(desc->num_clusters);
	if (*remaining_sz < cur_sz) {
		*next_ep = start_ep;
		return;
	}

	desc = (struct spdk_blob_md_descriptor_extent_table *)*buf;
	desc->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE;

	desc->num_clusters = blob->active.num_clusters;

	ep_len = 1;
	et_idx = 0;
	for (i = start_ep; i < blob->active.num_extent_pages; i++) {
		if (*remaining_sz < cur_sz  + sizeof(desc->extent_page[0])) {
			/* If we ran out of buffer space, return */
			break;
		}

		extent_page = blob->active.extent_pages[i];
		/* Verify that next extent_page is unallocated */
		if (extent_page == 0 &&
		    (i + 1 < blob->active.num_extent_pages && blob->active.extent_pages[i + 1] == 0)) {
			ep_len++;
			continue;
		}
		desc->extent_page[et_idx].page_idx = extent_page;
		desc->extent_page[et_idx].num_pages = ep_len;
		et_idx++;

		ep_len = 1;
		cur_sz += sizeof(desc->extent_page[et_idx]);
	}
	*next_ep = i;

	desc->length = sizeof(desc->num_clusters) + sizeof(desc->extent_page[0]) * et_idx;
	*remaining_sz -= sizeof(struct spdk_blob_md_descriptor) + desc->length;
	*buf += sizeof(struct spdk_blob_md_descriptor) + desc->length;
}

static int
_spdk_blob_serialize_extent_table(const struct spdk_blob *blob,
				  struct spdk_blob_md_page **pages,
				  struct spdk_blob_md_page *cur_page,
				  uint32_t *page_count, uint8_t **buf,
				  size_t *remaining_sz)
{
	uint64_t				last_extent_page;
	int					rc;

	last_extent_page = 0;
	/* At least single extent table entry has to be always persisted.
	 * Such case occurs with num_extent_pages == 0. */
	while (last_extent_page <= blob->active.num_extent_pages) {
		_spdk_blob_serialize_extent_table_entry(blob, last_extent_page, &last_extent_page, buf,
							remaining_sz);

		if (last_extent_page == blob->active.num_extent_pages) {
			break;
		}

		rc = _spdk_blob_serialize_add_page(blob, pages, page_count, &cur_page);
		if (rc < 0) {
			return rc;
		}

		*buf = (uint8_t *)cur_page->descriptors;
		*remaining_sz = sizeof(cur_page->descriptors);
	}

	return 0;
}

static void
_spdk_blob_serialize_extent_rle(const struct spdk_blob *blob,
				uint64_t start_cluster, uint64_t *next_cluster,
				uint8_t **buf, size_t *buf_sz)
{
	struct spdk_blob_md_descriptor_extent_rle *desc_extent_rle;
	size_t cur_sz;
	uint64_t i, extent_idx;
	uint64_t lba, lba_per_cluster, lba_count;

	/* The buffer must have room for at least one extent */
	cur_sz = sizeof(struct spdk_blob_md_descriptor) + sizeof(desc_extent_rle->extents[0]);
	if (*buf_sz < cur_sz) {
		*next_cluster = start_cluster;
		return;
	}

	desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)*buf;
	desc_extent_rle->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE;

	lba_per_cluster = _spdk_bs_cluster_to_lba(blob->bs, 1);

	lba = blob->active.clusters[start_cluster];
	lba_count = lba_per_cluster;
	extent_idx = 0;
	for (i = start_cluster + 1; i < blob->active.num_clusters; i++) {
		if ((lba + lba_count) == blob->active.clusters[i] && lba != 0) {
			/* Run-length encode sequential non-zero LBA */
			lba_count += lba_per_cluster;
			continue;
		} else if (lba == 0 && blob->active.clusters[i] == 0) {
			/* Run-length encode unallocated clusters */
			lba_count += lba_per_cluster;
			continue;
		}
		desc_extent_rle->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
		desc_extent_rle->extents[extent_idx].length = lba_count / lba_per_cluster;
		extent_idx++;

		cur_sz += sizeof(desc_extent_rle->extents[extent_idx]);

		if (*buf_sz < cur_sz) {
			/* If we ran out of buffer space, return */
			*next_cluster = i;
			break;
		}

		lba = blob->active.clusters[i];
		lba_count = lba_per_cluster;
	}

	if (*buf_sz >= cur_sz) {
		desc_extent_rle->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
		desc_extent_rle->extents[extent_idx].length = lba_count / lba_per_cluster;
		extent_idx++;

		*next_cluster = blob->active.num_clusters;
	}

	desc_extent_rle->length = sizeof(desc_extent_rle->extents[0]) * extent_idx;
	*buf_sz -= sizeof(struct spdk_blob_md_descriptor) + desc_extent_rle->length;
	*buf += sizeof(struct spdk_blob_md_descriptor) + desc_extent_rle->length;
}

static int
_spdk_blob_serialize_extents_rle(const struct spdk_blob *blob,
				 struct spdk_blob_md_page **pages,
				 struct spdk_blob_md_page *cur_page,
				 uint32_t *page_count, uint8_t **buf,
				 size_t *remaining_sz)
{
	uint64_t				last_cluster;
	int					rc;

	last_cluster = 0;
	while (last_cluster < blob->active.num_clusters) {
		_spdk_blob_serialize_extent_rle(blob, last_cluster, &last_cluster, buf, remaining_sz);

		if (last_cluster == blob->active.num_clusters) {
			break;
		}

		rc = _spdk_blob_serialize_add_page(blob, pages, page_count, &cur_page);
		if (rc < 0) {
			return rc;
		}

		*buf = (uint8_t *)cur_page->descriptors;
		*remaining_sz = sizeof(cur_page->descriptors);
	}

	return 0;
}

static void
_spdk_blob_serialize_extent_page(const struct spdk_blob *blob,
				 uint64_t cluster, struct spdk_blob_md_page *page)
{
	struct spdk_blob_md_descriptor_extent_page *desc_extent;
	uint64_t i, extent_idx;
	uint64_t lba, lba_per_cluster;
	uint64_t start_cluster_idx = (cluster / SPDK_EXTENTS_PER_EP) * SPDK_EXTENTS_PER_EP;

	desc_extent = (struct spdk_blob_md_descriptor_extent_page *) page->descriptors;
	desc_extent->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE;

	lba_per_cluster = _spdk_bs_cluster_to_lba(blob->bs, 1);

	desc_extent->start_cluster_idx = start_cluster_idx;
	extent_idx = 0;
	for (i = start_cluster_idx; i < blob->active.num_clusters; i++) {
		lba = blob->active.clusters[i];
		desc_extent->cluster_idx[extent_idx++] = lba / lba_per_cluster;
		if (extent_idx >= SPDK_EXTENTS_PER_EP) {
			break;
		}
	}
	desc_extent->length = sizeof(desc_extent->start_cluster_idx) +
			      sizeof(desc_extent->cluster_idx[0]) * extent_idx;
}

static void
_spdk_blob_serialize_flags(const struct spdk_blob *blob,
			   uint8_t *buf, size_t *buf_sz)
{
	struct spdk_blob_md_descriptor_flags *desc;

	/*
	 * Flags get serialized first, so we should always have room for the flags
	 *  descriptor.
	 */
	assert(*buf_sz >= sizeof(*desc));

	desc = (struct spdk_blob_md_descriptor_flags *)buf;
	desc->type = SPDK_MD_DESCRIPTOR_TYPE_FLAGS;
	desc->length = sizeof(*desc) - sizeof(struct spdk_blob_md_descriptor);
	desc->invalid_flags = blob->invalid_flags;
	desc->data_ro_flags = blob->data_ro_flags;
	desc->md_ro_flags = blob->md_ro_flags;

	*buf_sz -= sizeof(*desc);
}

static int
_spdk_blob_serialize_xattrs(const struct spdk_blob *blob,
			    const struct spdk_xattr_tailq *xattrs, bool internal,
			    struct spdk_blob_md_page **pages,
			    struct spdk_blob_md_page *cur_page,
			    uint32_t *page_count, uint8_t **buf,
			    size_t *remaining_sz)
{
	const struct spdk_xattr	*xattr;
	int	rc;

	TAILQ_FOREACH(xattr, xattrs, link) {
		size_t required_sz = 0;

		rc = _spdk_blob_serialize_xattr(xattr,
						*buf, *remaining_sz,
						&required_sz, internal);
		if (rc < 0) {
			/* Need to add a new page to the chain */
			rc = _spdk_blob_serialize_add_page(blob, pages, page_count,
							   &cur_page);
			if (rc < 0) {
				spdk_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return rc;
			}

			*buf = (uint8_t *)cur_page->descriptors;
			*remaining_sz = sizeof(cur_page->descriptors);

			/* Try again */
			required_sz = 0;
			rc = _spdk_blob_serialize_xattr(xattr,
							*buf, *remaining_sz,
							&required_sz, internal);

			if (rc < 0) {
				spdk_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return rc;
			}
		}

		*remaining_sz -= required_sz;
		*buf += required_sz;
	}

	return 0;
}

static int
_spdk_blob_serialize(const struct spdk_blob *blob, struct spdk_blob_md_page **pages,
		     uint32_t *page_count)
{
	struct spdk_blob_md_page		*cur_page;
	int					rc;
	uint8_t					*buf;
	size_t					remaining_sz;

	assert(pages != NULL);
	assert(page_count != NULL);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_DIRTY);

	*pages = NULL;
	*page_count = 0;

	/* A blob always has at least 1 page, even if it has no descriptors */
	rc = _spdk_blob_serialize_add_page(blob, pages, page_count, &cur_page);
	if (rc < 0) {
		return rc;
	}

	buf = (uint8_t *)cur_page->descriptors;
	remaining_sz = sizeof(cur_page->descriptors);

	/* Serialize flags */
	_spdk_blob_serialize_flags(blob, buf, &remaining_sz);
	buf += sizeof(struct spdk_blob_md_descriptor_flags);

	/* Serialize xattrs */
	rc = _spdk_blob_serialize_xattrs(blob, &blob->xattrs, false,
					 pages, cur_page, page_count, &buf, &remaining_sz);
	if (rc < 0) {
		return rc;
	}

	/* Serialize internal xattrs */
	rc = _spdk_blob_serialize_xattrs(blob, &blob->xattrs_internal, true,
					 pages, cur_page, page_count, &buf, &remaining_sz);
	if (rc < 0) {
		return rc;
	}

	if (blob->use_extent_table) {
		/* Serialize extent table */
		rc = _spdk_blob_serialize_extent_table(blob, pages, cur_page, page_count, &buf, &remaining_sz);
	} else {
		/* Serialize extents */
		rc = _spdk_blob_serialize_extents_rle(blob, pages, cur_page, page_count, &buf, &remaining_sz);
	}

	return rc;
}

struct spdk_blob_load_ctx {
	struct spdk_blob		*blob;

	struct spdk_blob_md_page	*pages;
	uint32_t			num_pages;
	uint32_t			next_extent_page;
	spdk_bs_sequence_t	        *seq;

	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static uint32_t
_spdk_blob_md_page_calc_crc(void *page)
{
	uint32_t		crc;

	crc = BLOB_CRC32C_INITIAL;
	crc = spdk_crc32c_update(page, SPDK_BS_PAGE_SIZE - 4, crc);
	crc ^= BLOB_CRC32C_INITIAL;

	return crc;

}

static void
_spdk_blob_load_final(void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		_spdk_blob_mark_clean(blob);
	}

	ctx->cb_fn(ctx->seq, ctx->cb_arg, bserrno);

	/* Free the memory */
	spdk_free(ctx->pages);
	free(ctx);
}

static void
_spdk_blob_load_snapshot_cpl(void *cb_arg, struct spdk_blob *snapshot, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		blob->back_bs_dev = bs_create_blob_bs_dev(snapshot);
		if (blob->back_bs_dev == NULL) {
			bserrno = -ENOMEM;
		}
	}
	if (bserrno != 0) {
		SPDK_ERRLOG("Snapshot fail\n");
	}

	_spdk_blob_load_final(ctx, bserrno);
}

static void _spdk_blob_update_clear_method(struct spdk_blob *blob);

static void
_spdk_blob_load_backing_dev(void *cb_arg)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	const void			*value;
	size_t				len;
	int				rc;

	if (spdk_blob_is_thin_provisioned(blob)) {
		rc = _spdk_blob_get_xattr_value(blob, BLOB_SNAPSHOT, &value, &len, true);
		if (rc == 0) {
			if (len != sizeof(spdk_blob_id)) {
				_spdk_blob_load_final(ctx, -EINVAL);
				return;
			}
			/* open snapshot blob and continue in the callback function */
			blob->parent_id = *(spdk_blob_id *)value;
			spdk_bs_open_blob(blob->bs, blob->parent_id,
					  _spdk_blob_load_snapshot_cpl, ctx);
			return;
		} else {
			/* add zeroes_dev for thin provisioned blob */
			blob->back_bs_dev = bs_create_zeroes_dev();
		}
	} else {
		/* standard blob */
		blob->back_bs_dev = NULL;
	}
	_spdk_blob_load_final(ctx, 0);
}

static void
_spdk_blob_load_cpl_extents_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	uint64_t			i;
	uint32_t			crc;
	uint64_t			lba;
	void				*tmp;
	uint64_t			sz;

	if (bserrno) {
		SPDK_ERRLOG("Extent page read failed: %d\n", bserrno);
		_spdk_blob_load_final(ctx, bserrno);
		return;
	}

	if (ctx->pages == NULL) {
		/* First iteration of this function, allocate buffer for single EXTENT_PAGE */
		ctx->pages = spdk_zmalloc(SPDK_BS_PAGE_SIZE, SPDK_BS_PAGE_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY,
					  SPDK_MALLOC_DMA);
		if (!ctx->pages) {
			_spdk_blob_load_final(ctx, -ENOMEM);
			return;
		}
		ctx->num_pages = 1;
		ctx->next_extent_page = 0;
	} else {
		page = &ctx->pages[0];
		crc = _spdk_blob_md_page_calc_crc(page);
		if (crc != page->crc) {
			_spdk_blob_load_final(ctx, -EINVAL);
			return;
		}

		if (page->next != SPDK_INVALID_MD_PAGE) {
			_spdk_blob_load_final(ctx, -EINVAL);
			return;
		}

		bserrno = _spdk_blob_parse_extent_page(page, blob);
		if (bserrno) {
			_spdk_blob_load_final(ctx, bserrno);
			return;
		}
	}

	for (i = ctx->next_extent_page; i < blob->active.num_extent_pages; i++) {
		if (blob->active.extent_pages[i] != 0) {
			/* Extent page was allocated, read and parse it. */
			lba = _spdk_bs_md_page_to_lba(blob->bs, blob->active.extent_pages[i]);
			ctx->next_extent_page = i + 1;

			bs_sequence_read_dev(seq, &ctx->pages[0], lba,
					     _spdk_bs_byte_to_lba(blob->bs, SPDK_BS_PAGE_SIZE),
					     _spdk_blob_load_cpl_extents_cpl, ctx);
			return;
		} else {
			/* Thin provisioned blobs can point to unallocated extent pages.
			 * In this case blob size should be increased by up to the amount left in remaining_clusters_in_et. */

			sz = spdk_min(blob->remaining_clusters_in_et, SPDK_EXTENTS_PER_EP);
			blob->active.num_clusters += sz;
			blob->remaining_clusters_in_et -= sz;

			assert(spdk_blob_is_thin_provisioned(blob));
			assert(i + 1 < blob->active.num_extent_pages || blob->remaining_clusters_in_et == 0);

			tmp = realloc(blob->active.clusters, blob->active.num_clusters * sizeof(*blob->active.clusters));
			if (tmp == NULL) {
				_spdk_blob_load_final(ctx, -ENOMEM);
				return;
			}
			memset(tmp + blob->active.cluster_array_size, 0,
			       sizeof(*blob->active.clusters) * (blob->active.num_clusters - blob->active.cluster_array_size));
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = blob->active.num_clusters;
		}
	}

	_spdk_blob_load_backing_dev(ctx);
}

static void
_spdk_blob_load_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	int				rc;
	uint32_t			crc;

	if (bserrno) {
		SPDK_ERRLOG("Metadata page read failed: %d\n", bserrno);
		_spdk_blob_load_final(ctx, bserrno);
		return;
	}

	page = &ctx->pages[ctx->num_pages - 1];
	crc = _spdk_blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		SPDK_ERRLOG("Metadata page %d crc mismatch\n", ctx->num_pages);
		_spdk_blob_load_final(ctx, -EINVAL);
		return;
	}

	if (page->next != SPDK_INVALID_MD_PAGE) {
		uint32_t next_page = page->next;
		uint64_t next_lba = _spdk_bs_md_page_to_lba(blob->bs, next_page);

		/* Read the next page */
		ctx->num_pages++;
		ctx->pages = spdk_realloc(ctx->pages, (sizeof(*page) * ctx->num_pages),
					  sizeof(*page));
		if (ctx->pages == NULL) {
			_spdk_blob_load_final(ctx, -ENOMEM);
			return;
		}

		bs_sequence_read_dev(seq, &ctx->pages[ctx->num_pages - 1],
				     next_lba,
				     _spdk_bs_byte_to_lba(blob->bs, sizeof(*page)),
				     _spdk_blob_load_cpl, ctx);
		return;
	}

	/* Parse the pages */
	rc = _spdk_blob_parse(ctx->pages, ctx->num_pages, blob);
	if (rc) {
		_spdk_blob_load_final(ctx, rc);
		return;
	}

	if (blob->extent_table_found == true) {
		/* If EXTENT_TABLE was found, that means support for it should be enabled. */
		assert(blob->extent_rle_found == false);
		blob->use_extent_table = true;
	} else {
		/* If EXTENT_RLE or no extent_* descriptor was found disable support
		 * for extent table. No extent_* descriptors means that blob has length of 0
		 * and no extent_rle descriptors were persisted for it.
		 * EXTENT_TABLE if used, is always present in metadata regardless of length. */
		blob->use_extent_table = false;
	}

	/* Check the clear_method stored in metadata vs what may have been passed
	 * via spdk_bs_open_blob_ext() and update accordingly.
	 */
	_spdk_blob_update_clear_method(blob);

	spdk_free(ctx->pages);
	ctx->pages = NULL;

	if (blob->extent_table_found) {
		_spdk_blob_load_cpl_extents_cpl(seq, ctx, 0);
	} else {
		_spdk_blob_load_backing_dev(ctx);
	}
}

/* Load a blob from disk given a blobid */
static void
_spdk_blob_load(spdk_bs_sequence_t *seq, struct spdk_blob *blob,
		spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_blob_load_ctx *ctx;
	struct spdk_blob_store *bs;
	uint32_t page_num;
	uint64_t lba;

	_spdk_blob_verify_md_op(blob);

	bs = blob->bs;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}

	ctx->blob = blob;
	ctx->pages = spdk_realloc(ctx->pages, SPDK_BS_PAGE_SIZE, SPDK_BS_PAGE_SIZE);
	if (!ctx->pages) {
		free(ctx);
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->num_pages = 1;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->seq = seq;

	page_num = _spdk_bs_blobid_to_page(blob->id);
	lba = _spdk_bs_md_page_to_lba(blob->bs, page_num);

	blob->state = SPDK_BLOB_STATE_LOADING;

	bs_sequence_read_dev(seq, &ctx->pages[0], lba,
			     _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE),
			     _spdk_blob_load_cpl, ctx);
}

struct spdk_blob_persist_ctx {
	struct spdk_blob		*blob;

	struct spdk_bs_super_block	*super;

	struct spdk_blob_md_page	*pages;
	uint32_t			next_extent_page;
	struct spdk_blob_md_page	*extent_page;

	spdk_bs_sequence_t		*seq;
	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(spdk_blob_persist_ctx) link;
};

static void
spdk_bs_batch_clear_dev(struct spdk_blob_persist_ctx *ctx, spdk_bs_batch_t *batch, uint64_t lba,
			uint32_t lba_count)
{
	switch (ctx->blob->clear_method) {
	case BLOB_CLEAR_WITH_DEFAULT:
	case BLOB_CLEAR_WITH_UNMAP:
		bs_batch_unmap_dev(batch, lba, lba_count);
		break;
	case BLOB_CLEAR_WITH_WRITE_ZEROES:
		bs_batch_write_zeroes_dev(batch, lba, lba_count);
		break;
	case BLOB_CLEAR_WITH_NONE:
	default:
		break;
	}
}

static void _spdk_blob_persist_check_dirty(struct spdk_blob_persist_ctx *ctx);

static void
_spdk_blob_persist_complete(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob_persist_ctx	*next_persist;
	struct spdk_blob		*blob = ctx->blob;

	if (bserrno == 0) {
		_spdk_blob_mark_clean(blob);
	}

	assert(ctx == TAILQ_FIRST(&blob->pending_persists));
	TAILQ_REMOVE(&blob->pending_persists, ctx, link);

	next_persist = TAILQ_FIRST(&blob->pending_persists);

	/* Call user callback */
	ctx->cb_fn(seq, ctx->cb_arg, bserrno);

	/* Free the memory */
	spdk_free(ctx->pages);
	free(ctx);

	if (next_persist != NULL) {
		_spdk_blob_persist_check_dirty(next_persist);
	}
}

static void
_spdk_blob_persist_clear_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* Release all clusters that were truncated */
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint32_t cluster_num = _spdk_bs_lba_to_cluster(bs, blob->active.clusters[i]);

		/* Nothing to release if it was not allocated */
		if (blob->active.clusters[i] != 0) {
			_spdk_bs_release_cluster(bs, cluster_num);
		}
	}

	if (blob->active.num_clusters == 0) {
		free(blob->active.clusters);
		blob->active.clusters = NULL;
		blob->active.cluster_array_size = 0;
	} else if (blob->active.num_clusters != blob->active.cluster_array_size) {
#ifndef __clang_analyzer__
		void *tmp;

		/* scan-build really can't figure reallocs, workaround it */
		tmp = realloc(blob->active.clusters, sizeof(*blob->active.clusters) * blob->active.num_clusters);
		assert(tmp != NULL);
		blob->active.clusters = tmp;

		tmp = realloc(blob->active.extent_pages, sizeof(uint32_t) * blob->active.num_extent_pages);
		assert(tmp != NULL);
		blob->active.extent_pages = tmp;
#endif
		blob->active.extent_pages_array_size = blob->active.num_extent_pages;
		blob->active.cluster_array_size = blob->active.num_clusters;
	}

	/* TODO: Add path to persist clear extent pages. */
	_spdk_blob_persist_complete(seq, ctx, bserrno);
}

static void
_spdk_blob_persist_clear_clusters(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	spdk_bs_batch_t			*batch;
	size_t				i;
	uint64_t			lba;
	uint32_t			lba_count;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	batch = bs_sequence_to_batch(seq, _spdk_blob_persist_clear_clusters_cpl, ctx);

	/* Clear all clusters that were truncated */
	lba = 0;
	lba_count = 0;
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint64_t next_lba = blob->active.clusters[i];
		uint32_t next_lba_count = _spdk_bs_cluster_to_lba(bs, 1);

		if (next_lba > 0 && (lba + lba_count) == next_lba) {
			/* This cluster is contiguous with the previous one. */
			lba_count += next_lba_count;
			continue;
		}

		/* This cluster is not contiguous with the previous one. */

		/* If a run of LBAs previously existing, clear them now */
		if (lba_count > 0) {
			spdk_bs_batch_clear_dev(ctx, batch, lba, lba_count);
		}

		/* Start building the next batch */
		lba = next_lba;
		if (next_lba > 0) {
			lba_count = next_lba_count;
		} else {
			lba_count = 0;
		}
	}

	/* If we ended with a contiguous set of LBAs, clear them now */
	if (lba_count > 0) {
		spdk_bs_batch_clear_dev(ctx, batch, lba, lba_count);
	}

	bs_batch_close(batch);
}

static void
_spdk_blob_persist_zero_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		_spdk_bs_release_md_page(bs, blob->clean.pages[i]);
	}

	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		page_num = _spdk_bs_blobid_to_page(blob->id);
		_spdk_bs_release_md_page(bs, page_num);
	}

	/* Move on to clearing clusters */
	_spdk_blob_persist_clear_clusters(seq, ctx, 0);
}

static void
_spdk_blob_persist_zero_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	spdk_bs_batch_t			*batch;
	size_t				i;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	batch = bs_sequence_to_batch(seq, _spdk_blob_persist_zero_pages_cpl, ctx);

	lba_count = _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE);

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		lba = _spdk_bs_md_page_to_lba(bs, blob->clean.pages[i]);

		bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	/* The first page will only be zeroed if this is a delete. */
	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		/* The first page in the metadata goes where the blobid indicates */
		page_num = _spdk_bs_blobid_to_page(blob->id);
		lba = _spdk_bs_md_page_to_lba(bs, page_num);

		bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	bs_batch_close(batch);
}

static void
_spdk_blob_persist_write_page_root(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	if (blob->active.num_pages == 0) {
		/* Move on to the next step */
		_spdk_blob_persist_zero_pages(seq, ctx, 0);
		return;
	}

	lba_count = _spdk_bs_byte_to_lba(bs, sizeof(*page));

	page = &ctx->pages[0];
	/* The first page in the metadata goes where the blobid indicates */
	lba = _spdk_bs_md_page_to_lba(bs, _spdk_bs_blobid_to_page(blob->id));

	bs_sequence_write_dev(seq, page, lba, lba_count,
			      _spdk_blob_persist_zero_pages, ctx);
}

static void
_spdk_blob_persist_write_page_chain(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;
	spdk_bs_batch_t			*batch;
	size_t				i;

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	lba_count = _spdk_bs_byte_to_lba(bs, sizeof(*page));

	batch = bs_sequence_to_batch(seq, _spdk_blob_persist_write_page_root, ctx);

	/* This starts at 1. The root page is not written until
	 * all of the others are finished
	 */
	for (i = 1; i < blob->active.num_pages; i++) {
		page = &ctx->pages[i];
		assert(page->sequence_num == i);

		lba = _spdk_bs_md_page_to_lba(bs, blob->active.pages[i]);

		bs_batch_write_dev(batch, page, lba, lba_count);
	}

	bs_batch_close(batch);
}

static int
_spdk_blob_resize(struct spdk_blob *blob, uint64_t sz)
{
	uint64_t	i;
	uint64_t	*tmp;
	uint64_t	lfc; /* lowest free cluster */
	uint32_t	lfmd; /*  lowest free md page */
	uint64_t	num_clusters;
	uint32_t	*ep_tmp;
	uint64_t	new_num_ep = 0, current_num_ep = 0;
	struct spdk_blob_store *bs;

	bs = blob->bs;

	_spdk_blob_verify_md_op(blob);

	if (blob->active.num_clusters == sz) {
		return 0;
	}

	if (blob->active.num_clusters < blob->active.cluster_array_size) {
		/* If this blob was resized to be larger, then smaller, then
		 * larger without syncing, then the cluster array already
		 * contains spare assigned clusters we can use.
		 */
		num_clusters = spdk_min(blob->active.cluster_array_size,
					sz);
	} else {
		num_clusters = blob->active.num_clusters;
	}

	if (blob->use_extent_table) {
		/* Round up since every cluster beyond current Extent Table size,
		 * requires new extent page. */
		new_num_ep = spdk_divide_round_up(sz, SPDK_EXTENTS_PER_EP);
		current_num_ep = spdk_divide_round_up(num_clusters, SPDK_EXTENTS_PER_EP);
	}

	/* Do two passes - one to verify that we can obtain enough clusters
	 * and md pages, another to actually claim them.
	 */

	if (spdk_blob_is_thin_provisioned(blob) == false) {
		lfc = 0;
		for (i = num_clusters; i < sz; i++) {
			lfc = spdk_bit_array_find_first_clear(bs->used_clusters, lfc);
			if (lfc == UINT32_MAX) {
				/* No more free clusters. Cannot satisfy the request */
				return -ENOSPC;
			}
			lfc++;
		}
		lfmd = 0;
		for (i = current_num_ep; i < new_num_ep ; i++) {
			lfmd = spdk_bit_array_find_first_clear(blob->bs->used_md_pages, lfmd);
			if (lfmd == UINT32_MAX) {
				/* No more free md pages. Cannot satisfy the request */
				return -ENOSPC;
			}
		}
	}

	if (sz > num_clusters) {
		/* Expand the cluster array if necessary.
		 * We only shrink the array when persisting.
		 */
		tmp = realloc(blob->active.clusters, sizeof(*blob->active.clusters) * sz);
		if (sz > 0 && tmp == NULL) {
			return -ENOMEM;
		}
		memset(tmp + blob->active.cluster_array_size, 0,
		       sizeof(*blob->active.clusters) * (sz - blob->active.cluster_array_size));
		blob->active.clusters = tmp;
		blob->active.cluster_array_size = sz;

		/* Expand the extents table, only if enough clusters were added */
		if (new_num_ep > current_num_ep && blob->use_extent_table) {
			ep_tmp = realloc(blob->active.extent_pages, sizeof(*blob->active.extent_pages) * new_num_ep);
			if (new_num_ep > 0 && ep_tmp == NULL) {
				return -ENOMEM;
			}
			memset(ep_tmp + blob->active.extent_pages_array_size, 0,
			       sizeof(*blob->active.extent_pages) * (new_num_ep - blob->active.extent_pages_array_size));
			blob->active.extent_pages = ep_tmp;
			blob->active.extent_pages_array_size = new_num_ep;
		}
	}

	blob->state = SPDK_BLOB_STATE_DIRTY;

	if (spdk_blob_is_thin_provisioned(blob) == false) {
		lfc = 0;
		lfmd = 0;
		for (i = num_clusters; i < sz; i++) {
			_spdk_bs_allocate_cluster(blob, i, &lfc, &lfmd, true);
			lfc++;
			lfmd++;
		}
	}

	blob->active.num_clusters = sz;
	blob->active.num_extent_pages = new_num_ep;

	return 0;
}

static void
_spdk_blob_persist_generate_new_md(struct spdk_blob_persist_ctx *ctx)
{
	spdk_bs_sequence_t *seq = ctx->seq;
	struct spdk_blob *blob = ctx->blob;
	struct spdk_blob_store *bs = blob->bs;
	uint64_t i;
	uint32_t page_num;
	void *tmp;
	int rc;

	/* Generate the new metadata */
	rc = _spdk_blob_serialize(blob, &ctx->pages, &blob->active.num_pages);
	if (rc < 0) {
		_spdk_blob_persist_complete(seq, ctx, rc);
		return;
	}

	assert(blob->active.num_pages >= 1);

	/* Resize the cache of page indices */
	tmp = realloc(blob->active.pages, blob->active.num_pages * sizeof(*blob->active.pages));
	if (!tmp) {
		_spdk_blob_persist_complete(seq, ctx, -ENOMEM);
		return;
	}
	blob->active.pages = tmp;

	/* Assign this metadata to pages. This requires two passes -
	 * one to verify that there are enough pages and a second
	 * to actually claim them. */
	page_num = 0;
	/* Note that this loop starts at one. The first page location is fixed by the blobid. */
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		if (page_num == UINT32_MAX) {
			_spdk_blob_persist_complete(seq, ctx, -ENOMEM);
			return;
		}
		page_num++;
	}

	page_num = 0;
	blob->active.pages[0] = _spdk_bs_blobid_to_page(blob->id);
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		ctx->pages[i - 1].next = page_num;
		/* Now that previous metadata page is complete, calculate the crc for it. */
		ctx->pages[i - 1].crc = _spdk_blob_md_page_calc_crc(&ctx->pages[i - 1]);
		blob->active.pages[i] = page_num;
		_spdk_bs_claim_md_page(bs, page_num);
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming page %u for blob %lu\n", page_num, blob->id);
		page_num++;
	}
	ctx->pages[i - 1].crc = _spdk_blob_md_page_calc_crc(&ctx->pages[i - 1]);
	/* Start writing the metadata from last page to first */
	blob->state = SPDK_BLOB_STATE_CLEAN;
	_spdk_blob_persist_write_page_chain(seq, ctx, 0);
}

static void
_spdk_blob_persist_write_extent_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob		*blob = ctx->blob;
	size_t				i;
	uint32_t			extent_page_id;
	uint32_t                        page_count = 0;
	int				rc;

	if (ctx->extent_page != NULL) {
		spdk_free(ctx->extent_page);
		ctx->extent_page = NULL;
	}

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	/* Only write out changed extent pages */
	for (i = ctx->next_extent_page; i < blob->active.num_extent_pages; i++) {
		extent_page_id = blob->active.extent_pages[i];
		if (extent_page_id == 0) {
			/* No Extent Page to persist */
			assert(spdk_blob_is_thin_provisioned(blob));
			continue;
		}
		/* Writing out new extent page for the first time. Either active extent pages is larger
		 * than clean extent pages or there was no extent page assigned due to thin provisioning. */
		if (i >= blob->clean.extent_pages_array_size || blob->clean.extent_pages[i] == 0) {
			blob->state = SPDK_BLOB_STATE_DIRTY;
			assert(spdk_bit_array_get(blob->bs->used_md_pages, extent_page_id));
			ctx->next_extent_page = i + 1;
			rc = _spdk_blob_serialize_add_page(ctx->blob, &ctx->extent_page, &page_count, &ctx->extent_page);
			if (rc < 0) {
				_spdk_blob_persist_complete(seq, ctx, rc);
				return;
			}

			_spdk_blob_serialize_extent_page(blob, i * SPDK_EXTENTS_PER_EP, ctx->extent_page);

			ctx->extent_page->crc = _spdk_blob_md_page_calc_crc(ctx->extent_page);

			bs_sequence_write_dev(seq, ctx->extent_page, _spdk_bs_md_page_to_lba(blob->bs, extent_page_id),
					      _spdk_bs_byte_to_lba(blob->bs, SPDK_BS_PAGE_SIZE),
					      _spdk_blob_persist_write_extent_pages, ctx);
			return;
		}
		assert(blob->clean.extent_pages[i] != 0);
	}

	_spdk_blob_persist_generate_new_md(ctx);
}

static void
_spdk_blob_persist_start(struct spdk_blob_persist_ctx *ctx)
{
	spdk_bs_sequence_t *seq = ctx->seq;
	struct spdk_blob *blob = ctx->blob;

	if (blob->active.num_pages == 0) {
		/* This is the signal that the blob should be deleted.
		 * Immediately jump to the clean up routine. */
		assert(blob->clean.num_pages > 0);
		blob->state = SPDK_BLOB_STATE_CLEAN;
		_spdk_blob_persist_zero_pages(seq, ctx, 0);
		return;

	}

	_spdk_blob_persist_write_extent_pages(seq, ctx, 0);
}

static void
_spdk_blob_persist_dirty_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx *ctx = cb_arg;

	spdk_free(ctx->super);

	if (bserrno != 0) {
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	ctx->blob->bs->clean = 0;

	_spdk_blob_persist_start(ctx);
}

static void
_spdk_bs_write_super(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
		     struct spdk_bs_super_block *super, spdk_bs_sequence_cpl cb_fn, void *cb_arg);


static void
_spdk_blob_persist_dirty(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		spdk_free(ctx->super);
		_spdk_blob_persist_complete(seq, ctx, bserrno);
		return;
	}

	ctx->super->clean = 0;
	if (ctx->super->size == 0) {
		ctx->super->size = ctx->blob->bs->dev->blockcnt * ctx->blob->bs->dev->blocklen;
	}

	_spdk_bs_write_super(seq, ctx->blob->bs, ctx->super, _spdk_blob_persist_dirty_cpl, ctx);
}

static void
_spdk_blob_persist_check_dirty(struct spdk_blob_persist_ctx *ctx)
{
	if (ctx->blob->bs->clean) {
		ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
					  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (!ctx->super) {
			_spdk_blob_persist_complete(ctx->seq, ctx, -ENOMEM);
			return;
		}

		bs_sequence_read_dev(ctx->seq, ctx->super, _spdk_bs_page_to_lba(ctx->blob->bs, 0),
				     _spdk_bs_byte_to_lba(ctx->blob->bs, sizeof(*ctx->super)),
				     _spdk_blob_persist_dirty, ctx);
	} else {
		_spdk_blob_persist_start(ctx);
	}
}

/* Write a blob to disk */
static void
_spdk_blob_persist(spdk_bs_sequence_t *seq, struct spdk_blob *blob,
		   spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_blob_persist_ctx *ctx;

	_spdk_blob_verify_md_op(blob);

	if (blob->state == SPDK_BLOB_STATE_CLEAN && TAILQ_EMPTY(&blob->pending_persists)) {
		cb_fn(seq, cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->blob = blob;
	ctx->seq = seq;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->next_extent_page = 0;

	/* Multiple blob persists can affect one another, via blob->state or
	 * blob mutable data changes. To prevent it, queue up the persists. */
	if (!TAILQ_EMPTY(&blob->pending_persists)) {
		TAILQ_INSERT_TAIL(&blob->pending_persists, ctx, link);
		return;
	}
	TAILQ_INSERT_HEAD(&blob->pending_persists, ctx, link);

	_spdk_blob_persist_check_dirty(ctx);
}

struct spdk_blob_copy_cluster_ctx {
	struct spdk_blob *blob;
	uint8_t *buf;
	uint64_t page;
	uint64_t new_cluster;
	uint32_t new_extent_page;
	spdk_bs_sequence_t *seq;
};

static void
_spdk_blob_allocate_and_copy_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;
	struct spdk_bs_request_set *set = (struct spdk_bs_request_set *)ctx->seq;
	TAILQ_HEAD(, spdk_bs_request_set) requests;
	spdk_bs_user_op_t *op;

	TAILQ_INIT(&requests);
	TAILQ_SWAP(&set->channel->need_cluster_alloc, &requests, spdk_bs_request_set, link);

	while (!TAILQ_EMPTY(&requests)) {
		op = TAILQ_FIRST(&requests);
		TAILQ_REMOVE(&requests, op, link);
		if (bserrno == 0) {
			bs_user_op_execute(op);
		} else {
			bs_user_op_abort(op);
		}
	}

	spdk_free(ctx->buf);
	free(ctx);
}

static void
_spdk_blob_insert_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno) {
		if (bserrno == -EEXIST) {
			/* The metadata insert failed because another thread
			 * allocated the cluster first. Free our cluster
			 * but continue without error. */
			bserrno = 0;
		}
		_spdk_bs_release_cluster(ctx->blob->bs, ctx->new_cluster);
		if (ctx->new_extent_page != 0) {
			_spdk_bs_release_md_page(ctx->blob->bs, ctx->new_extent_page);
		}
	}

	bs_sequence_finish(ctx->seq, bserrno);
}

static void
_spdk_blob_write_copy_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;
	uint32_t cluster_number;

	if (bserrno) {
		/* The write failed, so jump to the final completion handler */
		bs_sequence_finish(seq, bserrno);
		return;
	}

	cluster_number = _spdk_bs_page_to_cluster(ctx->blob->bs, ctx->page);

	_spdk_blob_insert_cluster_on_md_thread(ctx->blob, cluster_number, ctx->new_cluster,
					       ctx->new_extent_page, _spdk_blob_insert_cluster_cpl, ctx);
}

static void
_spdk_blob_write_copy(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		/* The read failed, so jump to the final completion handler */
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/* Write whole cluster */
	bs_sequence_write_dev(seq, ctx->buf,
			      _spdk_bs_cluster_to_lba(ctx->blob->bs, ctx->new_cluster),
			      _spdk_bs_cluster_to_lba(ctx->blob->bs, 1),
			      _spdk_blob_write_copy_cpl, ctx);
}

static void
_spdk_bs_allocate_and_copy_cluster(struct spdk_blob *blob,
				   struct spdk_io_channel *_ch,
				   uint64_t io_unit, spdk_bs_user_op_t *op)
{
	struct spdk_bs_cpl cpl;
	struct spdk_bs_channel *ch;
	struct spdk_blob_copy_cluster_ctx *ctx;
	uint32_t cluster_start_page;
	uint32_t cluster_number;
	int rc;

	ch = spdk_io_channel_get_ctx(_ch);

	if (!TAILQ_EMPTY(&ch->need_cluster_alloc)) {
		/* There are already operations pending. Queue this user op
		 * and return because it will be re-executed when the outstanding
		 * cluster allocation completes. */
		TAILQ_INSERT_TAIL(&ch->need_cluster_alloc, op, link);
		return;
	}

	/* Round the io_unit offset down to the first page in the cluster */
	cluster_start_page = _spdk_bs_io_unit_to_cluster_start(blob, io_unit);

	/* Calculate which index in the metadata cluster array the corresponding
	 * cluster is supposed to be at. */
	cluster_number = _spdk_bs_io_unit_to_cluster_number(blob, io_unit);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		bs_user_op_abort(op);
		return;
	}

	assert(blob->bs->cluster_sz % blob->back_bs_dev->blocklen == 0);

	ctx->blob = blob;
	ctx->page = cluster_start_page;

	if (blob->parent_id != SPDK_BLOBID_INVALID) {
		ctx->buf = spdk_malloc(blob->bs->cluster_sz, blob->back_bs_dev->blocklen,
				       NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (!ctx->buf) {
			SPDK_ERRLOG("DMA allocation for cluster of size = %" PRIu32 " failed.\n",
				    blob->bs->cluster_sz);
			free(ctx);
			bs_user_op_abort(op);
			return;
		}
	}

	rc = _spdk_bs_allocate_cluster(blob, cluster_number, &ctx->new_cluster, &ctx->new_extent_page,
				       false);
	if (rc != 0) {
		spdk_free(ctx->buf);
		free(ctx);
		bs_user_op_abort(op);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = _spdk_blob_allocate_and_copy_cluster_cpl;
	cpl.u.blob_basic.cb_arg = ctx;

	ctx->seq = bs_sequence_start(_ch, &cpl);
	if (!ctx->seq) {
		_spdk_bs_release_cluster(blob->bs, ctx->new_cluster);
		spdk_free(ctx->buf);
		free(ctx);
		bs_user_op_abort(op);
		return;
	}

	/* Queue the user op to block other incoming operations */
	TAILQ_INSERT_TAIL(&ch->need_cluster_alloc, op, link);

	if (blob->parent_id != SPDK_BLOBID_INVALID) {
		/* Read cluster from backing device */
		bs_sequence_read_bs_dev(ctx->seq, blob->back_bs_dev, ctx->buf,
					_spdk_bs_dev_page_to_lba(blob->back_bs_dev, cluster_start_page),
					_spdk_bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz),
					_spdk_blob_write_copy, ctx);
	} else {
		_spdk_blob_insert_cluster_on_md_thread(ctx->blob, cluster_number, ctx->new_cluster,
						       ctx->new_extent_page, _spdk_blob_insert_cluster_cpl, ctx);
	}
}

static inline void
_spdk_blob_calculate_lba_and_lba_count(struct spdk_blob *blob, uint64_t io_unit, uint64_t length,
				       uint64_t *lba,	uint32_t *lba_count)
{
	*lba_count = length;

	if (!_spdk_bs_io_unit_is_allocated(blob, io_unit)) {
		assert(blob->back_bs_dev != NULL);
		*lba = _spdk_bs_io_unit_to_back_dev_lba(blob, io_unit);
		*lba_count = _spdk_bs_io_unit_to_back_dev_lba(blob, *lba_count);
	} else {
		*lba = _spdk_bs_blob_io_unit_to_lba(blob, io_unit);
	}
}

struct op_split_ctx {
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	uint64_t io_unit_offset;
	uint64_t io_units_remaining;
	void *curr_payload;
	enum spdk_blob_op_type op_type;
	spdk_bs_sequence_t *seq;
};

static void
_spdk_blob_request_submit_op_split_next(void *cb_arg, int bserrno)
{
	struct op_split_ctx	*ctx = cb_arg;
	struct spdk_blob	*blob = ctx->blob;
	struct spdk_io_channel	*ch = ctx->channel;
	enum spdk_blob_op_type	op_type = ctx->op_type;
	uint8_t			*buf = ctx->curr_payload;
	uint64_t		offset = ctx->io_unit_offset;
	uint64_t		length = ctx->io_units_remaining;
	uint64_t		op_length;

	if (bserrno != 0 || ctx->io_units_remaining == 0) {
		bs_sequence_finish(ctx->seq, bserrno);
		free(ctx);
		return;
	}

	op_length = spdk_min(length, _spdk_bs_num_io_units_to_cluster_boundary(blob,
			     offset));

	/* Update length and payload for next operation */
	ctx->io_units_remaining -= op_length;
	ctx->io_unit_offset += op_length;
	if (op_type == SPDK_BLOB_WRITE || op_type == SPDK_BLOB_READ) {
		ctx->curr_payload += op_length * blob->bs->io_unit_size;
	}

	switch (op_type) {
	case SPDK_BLOB_READ:
		spdk_blob_io_read(blob, ch, buf, offset, op_length,
				  _spdk_blob_request_submit_op_split_next, ctx);
		break;
	case SPDK_BLOB_WRITE:
		spdk_blob_io_write(blob, ch, buf, offset, op_length,
				   _spdk_blob_request_submit_op_split_next, ctx);
		break;
	case SPDK_BLOB_UNMAP:
		spdk_blob_io_unmap(blob, ch, offset, op_length,
				   _spdk_blob_request_submit_op_split_next, ctx);
		break;
	case SPDK_BLOB_WRITE_ZEROES:
		spdk_blob_io_write_zeroes(blob, ch, offset, op_length,
					  _spdk_blob_request_submit_op_split_next, ctx);
		break;
	case SPDK_BLOB_READV:
	case SPDK_BLOB_WRITEV:
		SPDK_ERRLOG("readv/write not valid\n");
		bs_sequence_finish(ctx->seq, -EINVAL);
		free(ctx);
		break;
	}
}

static void
_spdk_blob_request_submit_op_split(struct spdk_io_channel *ch, struct spdk_blob *blob,
				   void *payload, uint64_t offset, uint64_t length,
				   spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct op_split_ctx *ctx;
	spdk_bs_sequence_t *seq;
	struct spdk_bs_cpl cpl;

	assert(blob != NULL);

	ctx = calloc(1, sizeof(struct op_split_ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(ch, &cpl);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->blob = blob;
	ctx->channel = ch;
	ctx->curr_payload = payload;
	ctx->io_unit_offset = offset;
	ctx->io_units_remaining = length;
	ctx->op_type = op_type;
	ctx->seq = seq;

	_spdk_blob_request_submit_op_split_next(ctx, 0);
}

static void
_spdk_blob_request_submit_op_single(struct spdk_io_channel *_ch, struct spdk_blob *blob,
				    void *payload, uint64_t offset, uint64_t length,
				    spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct spdk_bs_cpl cpl;
	uint64_t lba;
	uint32_t lba_count;

	assert(blob != NULL);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	_spdk_blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

	if (blob->frozen_refcnt) {
		/* This blob I/O is frozen */
		spdk_bs_user_op_t *op;
		struct spdk_bs_channel *bs_channel = spdk_io_channel_get_ctx(_ch);

		op = bs_user_op_alloc(_ch, &cpl, op_type, blob, payload, 0, offset, length);
		if (!op) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		TAILQ_INSERT_TAIL(&bs_channel->queued_io, op, link);

		return;
	}

	switch (op_type) {
	case SPDK_BLOB_READ: {
		spdk_bs_batch_t *batch;

		batch = bs_batch_open(_ch, &cpl);
		if (!batch) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		if (_spdk_bs_io_unit_is_allocated(blob, offset)) {
			/* Read from the blob */
			bs_batch_read_dev(batch, payload, lba, lba_count);
		} else {
			/* Read from the backing block device */
			spdk_bs_batch_read_bs_dev(batch, blob->back_bs_dev, payload, lba, lba_count);
		}

		bs_batch_close(batch);
		break;
	}
	case SPDK_BLOB_WRITE:
	case SPDK_BLOB_WRITE_ZEROES: {
		if (_spdk_bs_io_unit_is_allocated(blob, offset)) {
			/* Write to the blob */
			spdk_bs_batch_t *batch;

			if (lba_count == 0) {
				cb_fn(cb_arg, 0);
				return;
			}

			batch = bs_batch_open(_ch, &cpl);
			if (!batch) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (op_type == SPDK_BLOB_WRITE) {
				bs_batch_write_dev(batch, payload, lba, lba_count);
			} else {
				bs_batch_write_zeroes_dev(batch, lba, lba_count);
			}

			bs_batch_close(batch);
		} else {
			/* Queue this operation and allocate the cluster */
			spdk_bs_user_op_t *op;

			op = bs_user_op_alloc(_ch, &cpl, op_type, blob, payload, 0, offset, length);
			if (!op) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			_spdk_bs_allocate_and_copy_cluster(blob, _ch, offset, op);
		}
		break;
	}
	case SPDK_BLOB_UNMAP: {
		spdk_bs_batch_t *batch;

		batch = bs_batch_open(_ch, &cpl);
		if (!batch) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		if (_spdk_bs_io_unit_is_allocated(blob, offset)) {
			bs_batch_unmap_dev(batch, lba, lba_count);
		}

		bs_batch_close(batch);
		break;
	}
	case SPDK_BLOB_READV:
	case SPDK_BLOB_WRITEV:
		SPDK_ERRLOG("readv/write not valid\n");
		cb_fn(cb_arg, -EINVAL);
		break;
	}
}

static void
_spdk_blob_request_submit_op(struct spdk_blob *blob, struct spdk_io_channel *_channel,
			     void *payload, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	assert(blob != NULL);

	if (blob->data_ro && op_type != SPDK_BLOB_READ) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (offset + length > _spdk_bs_cluster_to_lba(blob->bs, blob->active.num_clusters)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}
	if (length <= _spdk_bs_num_io_units_to_cluster_boundary(blob, offset)) {
		_spdk_blob_request_submit_op_single(_channel, blob, payload, offset, length,
						    cb_fn, cb_arg, op_type);
	} else {
		_spdk_blob_request_submit_op_split(_channel, blob, payload, offset, length,
						   cb_fn, cb_arg, op_type);
	}
}

struct rw_iov_ctx {
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_op_complete cb_fn;
	void *cb_arg;
	bool read;
	int iovcnt;
	struct iovec *orig_iov;
	uint64_t io_unit_offset;
	uint64_t io_units_remaining;
	uint64_t io_units_done;
	struct iovec iov[0];
};

static void
_spdk_rw_iov_done(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	assert(cb_arg == NULL);
	bs_sequence_finish(seq, bserrno);
}

static void
_spdk_rw_iov_split_next(void *cb_arg, int bserrno)
{
	struct rw_iov_ctx *ctx = cb_arg;
	struct spdk_blob *blob = ctx->blob;
	struct iovec *iov, *orig_iov;
	int iovcnt;
	size_t orig_iovoff;
	uint64_t io_units_count, io_units_to_boundary, io_unit_offset;
	uint64_t byte_count;

	if (bserrno != 0 || ctx->io_units_remaining == 0) {
		ctx->cb_fn(ctx->cb_arg, bserrno);
		free(ctx);
		return;
	}

	io_unit_offset = ctx->io_unit_offset;
	io_units_to_boundary = _spdk_bs_num_io_units_to_cluster_boundary(blob, io_unit_offset);
	io_units_count = spdk_min(ctx->io_units_remaining, io_units_to_boundary);
	/*
	 * Get index and offset into the original iov array for our current position in the I/O sequence.
	 *  byte_count will keep track of how many bytes remaining until orig_iov and orig_iovoff will
	 *  point to the current position in the I/O sequence.
	 */
	byte_count = ctx->io_units_done * blob->bs->io_unit_size;
	orig_iov = &ctx->orig_iov[0];
	orig_iovoff = 0;
	while (byte_count > 0) {
		if (byte_count >= orig_iov->iov_len) {
			byte_count -= orig_iov->iov_len;
			orig_iov++;
		} else {
			orig_iovoff = byte_count;
			byte_count = 0;
		}
	}

	/*
	 * Build an iov array for the next I/O in the sequence.  byte_count will keep track of how many
	 *  bytes of this next I/O remain to be accounted for in the new iov array.
	 */
	byte_count = io_units_count * blob->bs->io_unit_size;
	iov = &ctx->iov[0];
	iovcnt = 0;
	while (byte_count > 0) {
		assert(iovcnt < ctx->iovcnt);
		iov->iov_len = spdk_min(byte_count, orig_iov->iov_len - orig_iovoff);
		iov->iov_base = orig_iov->iov_base + orig_iovoff;
		byte_count -= iov->iov_len;
		orig_iovoff = 0;
		orig_iov++;
		iov++;
		iovcnt++;
	}

	ctx->io_unit_offset += io_units_count;
	ctx->io_units_remaining -= io_units_count;
	ctx->io_units_done += io_units_count;
	iov = &ctx->iov[0];

	if (ctx->read) {
		spdk_blob_io_readv(ctx->blob, ctx->channel, iov, iovcnt, io_unit_offset,
				   io_units_count, _spdk_rw_iov_split_next, ctx);
	} else {
		spdk_blob_io_writev(ctx->blob, ctx->channel, iov, iovcnt, io_unit_offset,
				    io_units_count, _spdk_rw_iov_split_next, ctx);
	}
}

static void
_spdk_blob_request_submit_rw_iov(struct spdk_blob *blob, struct spdk_io_channel *_channel,
				 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
				 spdk_blob_op_complete cb_fn, void *cb_arg, bool read)
{
	struct spdk_bs_cpl	cpl;

	assert(blob != NULL);

	if (!read && blob->data_ro) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (offset + length > _spdk_bs_cluster_to_lba(blob->bs, blob->active.num_clusters)) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	/*
	 * For now, we implement readv/writev using a sequence (instead of a batch) to account for having
	 *  to split a request that spans a cluster boundary.  For I/O that do not span a cluster boundary,
	 *  there will be no noticeable difference compared to using a batch.  For I/O that do span a cluster
	 *  boundary, the target LBAs (after blob offset to LBA translation) may not be contiguous, so we need
	 *  to allocate a separate iov array and split the I/O such that none of the resulting
	 *  smaller I/O cross a cluster boundary.  These smaller I/O will be issued in sequence (not in parallel)
	 *  but since this case happens very infrequently, any performance impact will be negligible.
	 *
	 * This could be optimized in the future to allocate a big enough iov array to account for all of the iovs
	 *  for all of the smaller I/Os, pre-build all of the iov arrays for the smaller I/Os, then issue them
	 *  in a batch.  That would also require creating an intermediate spdk_bs_cpl that would get called
	 *  when the batch was completed, to allow for freeing the memory for the iov arrays.
	 */
	if (spdk_likely(length <= _spdk_bs_num_io_units_to_cluster_boundary(blob, offset))) {
		uint32_t lba_count;
		uint64_t lba;

		cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
		cpl.u.blob_basic.cb_fn = cb_fn;
		cpl.u.blob_basic.cb_arg = cb_arg;

		if (blob->frozen_refcnt) {
			/* This blob I/O is frozen */
			enum spdk_blob_op_type op_type;
			spdk_bs_user_op_t *op;
			struct spdk_bs_channel *bs_channel = spdk_io_channel_get_ctx(_channel);

			op_type = read ? SPDK_BLOB_READV : SPDK_BLOB_WRITEV;
			op = bs_user_op_alloc(_channel, &cpl, op_type, blob, iov, iovcnt, offset, length);
			if (!op) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			TAILQ_INSERT_TAIL(&bs_channel->queued_io, op, link);

			return;
		}

		_spdk_blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

		if (read) {
			spdk_bs_sequence_t *seq;

			seq = bs_sequence_start(_channel, &cpl);
			if (!seq) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (_spdk_bs_io_unit_is_allocated(blob, offset)) {
				bs_sequence_readv_dev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
			} else {
				bs_sequence_readv_bs_dev(seq, blob->back_bs_dev, iov, iovcnt, lba, lba_count,
							 _spdk_rw_iov_done, NULL);
			}
		} else {
			if (_spdk_bs_io_unit_is_allocated(blob, offset)) {
				spdk_bs_sequence_t *seq;

				seq = bs_sequence_start(_channel, &cpl);
				if (!seq) {
					cb_fn(cb_arg, -ENOMEM);
					return;
				}

				bs_sequence_writev_dev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
			} else {
				/* Queue this operation and allocate the cluster */
				spdk_bs_user_op_t *op;

				op = bs_user_op_alloc(_channel, &cpl, SPDK_BLOB_WRITEV, blob, iov, iovcnt, offset,
						      length);
				if (!op) {
					cb_fn(cb_arg, -ENOMEM);
					return;
				}

				_spdk_bs_allocate_and_copy_cluster(blob, _channel, offset, op);
			}
		}
	} else {
		struct rw_iov_ctx *ctx;

		ctx = calloc(1, sizeof(struct rw_iov_ctx) + iovcnt * sizeof(struct iovec));
		if (ctx == NULL) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		ctx->blob = blob;
		ctx->channel = _channel;
		ctx->cb_fn = cb_fn;
		ctx->cb_arg = cb_arg;
		ctx->read = read;
		ctx->orig_iov = iov;
		ctx->iovcnt = iovcnt;
		ctx->io_unit_offset = offset;
		ctx->io_units_remaining = length;
		ctx->io_units_done = 0;

		_spdk_rw_iov_split_next(ctx, 0);
	}
}

static struct spdk_blob *
_spdk_blob_lookup(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	struct spdk_blob *blob;

	TAILQ_FOREACH(blob, &bs->blobs, link) {
		if (blob->id == blobid) {
			return blob;
		}
	}

	return NULL;
}

static void
_spdk_blob_get_snapshot_and_clone_entries(struct spdk_blob *blob,
		struct spdk_blob_list **snapshot_entry, struct spdk_blob_list **clone_entry)
{
	assert(blob != NULL);
	*snapshot_entry = NULL;
	*clone_entry = NULL;

	if (blob->parent_id == SPDK_BLOBID_INVALID) {
		return;
	}

	TAILQ_FOREACH(*snapshot_entry, &blob->bs->snapshots, link) {
		if ((*snapshot_entry)->id == blob->parent_id) {
			break;
		}
	}

	if (*snapshot_entry != NULL) {
		TAILQ_FOREACH(*clone_entry, &(*snapshot_entry)->clones, link) {
			if ((*clone_entry)->id == blob->id) {
				break;
			}
		}

		assert(clone_entry != NULL);
	}
}

static int
_spdk_bs_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_blob_store		*bs = io_device;
	struct spdk_bs_channel		*channel = ctx_buf;
	struct spdk_bs_dev		*dev;
	uint32_t			max_ops = bs->max_channel_ops;
	uint32_t			i;

	dev = bs->dev;

	channel->req_mem = calloc(max_ops, sizeof(struct spdk_bs_request_set));
	if (!channel->req_mem) {
		return -1;
	}

	TAILQ_INIT(&channel->reqs);

	for (i = 0; i < max_ops; i++) {
		TAILQ_INSERT_TAIL(&channel->reqs, &channel->req_mem[i], link);
	}

	channel->bs = bs;
	channel->dev = dev;
	channel->dev_channel = dev->create_channel(dev);

	if (!channel->dev_channel) {
		SPDK_ERRLOG("Failed to create device channel.\n");
		free(channel->req_mem);
		return -1;
	}

	TAILQ_INIT(&channel->need_cluster_alloc);
	TAILQ_INIT(&channel->queued_io);

	return 0;
}

static void
_spdk_bs_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bs_channel *channel = ctx_buf;
	spdk_bs_user_op_t *op;

	while (!TAILQ_EMPTY(&channel->need_cluster_alloc)) {
		op = TAILQ_FIRST(&channel->need_cluster_alloc);
		TAILQ_REMOVE(&channel->need_cluster_alloc, op, link);
		bs_user_op_abort(op);
	}

	while (!TAILQ_EMPTY(&channel->queued_io)) {
		op = TAILQ_FIRST(&channel->queued_io);
		TAILQ_REMOVE(&channel->queued_io, op, link);
		bs_user_op_abort(op);
	}

	free(channel->req_mem);
	channel->dev->destroy_channel(channel->dev, channel->dev_channel);
}

static void
_spdk_bs_dev_destroy(void *io_device)
{
	struct spdk_blob_store *bs = io_device;
	struct spdk_blob	*blob, *blob_tmp;

	bs->dev->destroy(bs->dev);

	TAILQ_FOREACH_SAFE(blob, &bs->blobs, link, blob_tmp) {
		TAILQ_REMOVE(&bs->blobs, blob, link);
		_spdk_blob_free(blob);
	}

	pthread_mutex_destroy(&bs->used_clusters_mutex);

	spdk_bit_array_free(&bs->used_blobids);
	spdk_bit_array_free(&bs->used_md_pages);
	spdk_bit_array_free(&bs->used_clusters);
	/*
	 * If this function is called for any reason except a successful unload,
	 * the unload_cpl type will be NONE and this will be a nop.
	 */
	bs_call_cpl(&bs->unload_cpl, bs->unload_err);

	free(bs);
}

static int
_spdk_bs_blob_list_add(struct spdk_blob *blob)
{
	spdk_blob_id snapshot_id;
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	assert(blob != NULL);

	snapshot_id = blob->parent_id;
	if (snapshot_id == SPDK_BLOBID_INVALID) {
		return 0;
	}

	snapshot_entry = _spdk_bs_get_snapshot_entry(blob->bs, snapshot_id);
	if (snapshot_entry == NULL) {
		/* Snapshot not found */
		snapshot_entry = calloc(1, sizeof(struct spdk_blob_list));
		if (snapshot_entry == NULL) {
			return -ENOMEM;
		}
		snapshot_entry->id = snapshot_id;
		TAILQ_INIT(&snapshot_entry->clones);
		TAILQ_INSERT_TAIL(&blob->bs->snapshots, snapshot_entry, link);
	} else {
		TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
			if (clone_entry->id == blob->id) {
				break;
			}
		}
	}

	if (clone_entry == NULL) {
		/* Clone not found */
		clone_entry = calloc(1, sizeof(struct spdk_blob_list));
		if (clone_entry == NULL) {
			return -ENOMEM;
		}
		clone_entry->id = blob->id;
		TAILQ_INIT(&clone_entry->clones);
		TAILQ_INSERT_TAIL(&snapshot_entry->clones, clone_entry, link);
		snapshot_entry->clone_count++;
	}

	return 0;
}

static void
_spdk_bs_blob_list_remove(struct spdk_blob *blob)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	_spdk_blob_get_snapshot_and_clone_entries(blob, &snapshot_entry, &clone_entry);

	if (snapshot_entry == NULL) {
		return;
	}

	blob->parent_id = SPDK_BLOBID_INVALID;
	TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
	free(clone_entry);

	snapshot_entry->clone_count--;
}

static int
_spdk_bs_blob_list_free(struct spdk_blob_store *bs)
{
	struct spdk_blob_list *snapshot_entry;
	struct spdk_blob_list *snapshot_entry_tmp;
	struct spdk_blob_list *clone_entry;
	struct spdk_blob_list *clone_entry_tmp;

	TAILQ_FOREACH_SAFE(snapshot_entry, &bs->snapshots, link, snapshot_entry_tmp) {
		TAILQ_FOREACH_SAFE(clone_entry, &snapshot_entry->clones, link, clone_entry_tmp) {
			TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
			free(clone_entry);
		}
		TAILQ_REMOVE(&bs->snapshots, snapshot_entry, link);
		free(snapshot_entry);
	}

	return 0;
}

static void
_spdk_bs_free(struct spdk_blob_store *bs)
{
	_spdk_bs_blob_list_free(bs);

	spdk_bs_unregister_md_thread(bs);
	spdk_io_device_unregister(bs, _spdk_bs_dev_destroy);
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_DEFAULT_CHANNEL_OPS;
	opts->clear_method = BS_CLEAR_WITH_UNMAP;
	memset(&opts->bstype, 0, sizeof(opts->bstype));
	opts->iter_cb_fn = NULL;
	opts->iter_cb_arg = NULL;
}

static int
_spdk_bs_opts_verify(struct spdk_bs_opts *opts)
{
	if (opts->cluster_sz == 0 || opts->num_md_pages == 0 || opts->max_md_ops == 0 ||
	    opts->max_channel_ops == 0) {
		SPDK_ERRLOG("Blobstore options cannot be set to 0\n");
		return -1;
	}

	return 0;
}

static int
_spdk_bs_alloc(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts, struct spdk_blob_store **_bs)
{
	struct spdk_blob_store	*bs;
	uint64_t dev_size;
	int rc;

	dev_size = dev->blocklen * dev->blockcnt;
	if (dev_size < opts->cluster_sz) {
		/* Device size cannot be smaller than cluster size of blobstore */
		SPDK_INFOLOG(SPDK_LOG_BLOB, "Device size %" PRIu64 " is smaller than cluster size %" PRIu32 "\n",
			     dev_size, opts->cluster_sz);
		return -ENOSPC;
	}
	if (opts->cluster_sz < SPDK_BS_PAGE_SIZE) {
		/* Cluster size cannot be smaller than page size */
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than page size %d\n",
			    opts->cluster_sz, SPDK_BS_PAGE_SIZE);
		return -EINVAL;
	}
	bs = calloc(1, sizeof(struct spdk_blob_store));
	if (!bs) {
		return -ENOMEM;
	}

	TAILQ_INIT(&bs->blobs);
	TAILQ_INIT(&bs->snapshots);
	bs->dev = dev;
	bs->md_thread = spdk_get_thread();
	assert(bs->md_thread != NULL);

	/*
	 * Do not use _spdk_bs_lba_to_cluster() here since blockcnt may not be an
	 *  even multiple of the cluster size.
	 */
	bs->cluster_sz = opts->cluster_sz;
	bs->total_clusters = dev->blockcnt / (bs->cluster_sz / dev->blocklen);
	bs->pages_per_cluster = bs->cluster_sz / SPDK_BS_PAGE_SIZE;
	if (spdk_u32_is_pow2(bs->pages_per_cluster)) {
		bs->pages_per_cluster_shift = spdk_u32log2(bs->pages_per_cluster);
	}
	bs->num_free_clusters = bs->total_clusters;
	bs->used_clusters = spdk_bit_array_create(bs->total_clusters);
	bs->io_unit_size = dev->blocklen;
	if (bs->used_clusters == NULL) {
		free(bs);
		return -ENOMEM;
	}

	bs->max_channel_ops = opts->max_channel_ops;
	bs->super_blob = SPDK_BLOBID_INVALID;
	memcpy(&bs->bstype, &opts->bstype, sizeof(opts->bstype));

	/* The metadata is assumed to be at least 1 page */
	bs->used_md_pages = spdk_bit_array_create(1);
	bs->used_blobids = spdk_bit_array_create(0);

	pthread_mutex_init(&bs->used_clusters_mutex, NULL);

	spdk_io_device_register(bs, _spdk_bs_channel_create, _spdk_bs_channel_destroy,
				sizeof(struct spdk_bs_channel), "blobstore");
	rc = spdk_bs_register_md_thread(bs);
	if (rc == -1) {
		spdk_io_device_unregister(bs, NULL);
		pthread_mutex_destroy(&bs->used_clusters_mutex);
		spdk_bit_array_free(&bs->used_blobids);
		spdk_bit_array_free(&bs->used_md_pages);
		spdk_bit_array_free(&bs->used_clusters);
		free(bs);
		/* FIXME: this is a lie but don't know how to get a proper error code here */
		return -ENOMEM;
	}

	*_bs = bs;
	return 0;
}

/* START spdk_bs_load, spdk_bs_load_ctx will used for both load and unload. */

struct spdk_bs_load_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;

	struct spdk_bs_md_mask		*mask;
	bool				in_page_chain;
	uint32_t			page_index;
	uint32_t			cur_page;
	struct spdk_blob_md_page	*page;

	uint64_t			num_extent_pages;
	uint32_t			*extent_page_num;
	struct spdk_blob_md_page	*extent_pages;

	spdk_bs_sequence_t			*seq;
	spdk_blob_op_with_handle_complete	iter_cb_fn;
	void					*iter_cb_arg;
	struct spdk_blob			*blob;
	spdk_blob_id				blobid;
};

static void
_spdk_bs_load_ctx_fail(struct spdk_bs_load_ctx *ctx, int bserrno)
{
	assert(bserrno != 0);

	spdk_free(ctx->super);
	bs_sequence_finish(ctx->seq, bserrno);
	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void
_spdk_bs_set_mask(struct spdk_bit_array *array, struct spdk_bs_md_mask *mask)
{
	uint32_t i = 0;

	while (true) {
		i = spdk_bit_array_find_first_set(array, i);
		if (i >= mask->length) {
			break;
		}
		mask->mask[i / 8] |= 1U << (i % 8);
		i++;
	}
}

static int
_spdk_bs_load_mask(struct spdk_bit_array **array_ptr, struct spdk_bs_md_mask *mask)
{
	struct spdk_bit_array *array;
	uint32_t i;

	if (spdk_bit_array_resize(array_ptr, mask->length) < 0) {
		return -ENOMEM;
	}

	array = *array_ptr;
	for (i = 0; i < mask->length; i++) {
		if (mask->mask[i / 8] & (1U << (i % 8))) {
			spdk_bit_array_set(array, i);
		}
	}

	return 0;
}

static void
_spdk_bs_write_super(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
		     struct spdk_bs_super_block *super, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	/* Update the values in the super block */
	super->super_blob = bs->super_blob;
	memcpy(&super->bstype, &bs->bstype, sizeof(bs->bstype));
	super->crc = _spdk_blob_md_page_calc_crc(super);
	bs_sequence_write_dev(seq, super, _spdk_bs_page_to_lba(bs, 0),
			      _spdk_bs_byte_to_lba(bs, sizeof(*super)),
			      cb_fn, cb_arg);
}

static void
_spdk_bs_write_used_clusters(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	/* Write out the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_CLUSTERS;
	ctx->mask->length = ctx->bs->total_clusters;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_clusters));

	_spdk_bs_set_mask(ctx->bs->used_clusters, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
_spdk_bs_write_used_md(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_PAGES;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_md_pages));

	_spdk_bs_set_mask(ctx->bs->used_md_pages, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
_spdk_bs_write_used_blobids(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	if (ctx->super->used_blobid_mask_len == 0) {
		/*
		 * This is a pre-v3 on-disk format where the blobid mask does not get
		 *  written to disk.
		 */
		cb_fn(seq, arg, 0);
		return;
	}

	mask_size = ctx->super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_BLOBIDS;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_blobids));

	_spdk_bs_set_mask(ctx->bs->used_blobids, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
_spdk_blob_set_thin_provision(struct spdk_blob *blob)
{
	_spdk_blob_verify_md_op(blob);
	blob->invalid_flags |= SPDK_BLOB_THIN_PROV;
	blob->state = SPDK_BLOB_STATE_DIRTY;
}

static void
_spdk_blob_set_clear_method(struct spdk_blob *blob, enum blob_clear_method clear_method)
{
	_spdk_blob_verify_md_op(blob);
	blob->clear_method = clear_method;
	blob->md_ro_flags |= (clear_method << SPDK_BLOB_CLEAR_METHOD_SHIFT);
	blob->state = SPDK_BLOB_STATE_DIRTY;
}

static void _spdk_bs_load_iter(void *arg, struct spdk_blob *blob, int bserrno);

static void
_spdk_bs_delete_corrupted_blob_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	spdk_blob_id id;
	int64_t page_num;

	/* Iterate to next blob (we can't use spdk_bs_iter_next function as our
	 * last blob has been removed */
	page_num = _spdk_bs_blobid_to_page(ctx->blobid);
	page_num++;
	page_num = spdk_bit_array_find_first_set(ctx->bs->used_blobids, page_num);
	if (page_num >= spdk_bit_array_capacity(ctx->bs->used_blobids)) {
		_spdk_bs_load_iter(ctx, NULL, -ENOENT);
		return;
	}

	id = _spdk_bs_page_to_blobid(page_num);

	spdk_bs_open_blob(ctx->bs, id, _spdk_bs_load_iter, ctx);
}

static void
_spdk_bs_delete_corrupted_close_cb(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, _spdk_bs_load_iter, ctx);
		return;
	}

	spdk_bs_delete_blob(ctx->bs, ctx->blobid, _spdk_bs_delete_corrupted_blob_cpl, ctx);
}

static void
_spdk_bs_delete_corrupted_blob(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t i;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, _spdk_bs_load_iter, ctx);
		return;
	}

	/* Snapshot and clone have the same copy of cluster map and extent pages
	 * at this point. Let's clear both for snpashot now,
	 * so that it won't be cleared for clone later when we remove snapshot.
	 * Also set thin provision to pass data corruption check */
	for (i = 0; i < ctx->blob->active.num_clusters; i++) {
		ctx->blob->active.clusters[i] = 0;
	}
	for (i = 0; i < ctx->blob->active.num_extent_pages; i++) {
		ctx->blob->active.extent_pages[i] = 0;
	}

	ctx->blob->md_ro = false;

	_spdk_blob_set_thin_provision(ctx->blob);

	ctx->blobid = ctx->blob->id;

	spdk_blob_close(ctx->blob, _spdk_bs_delete_corrupted_close_cb, ctx);
}

static void
_spdk_bs_update_corrupted_blob(void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to close clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, _spdk_bs_load_iter, ctx);
		return;
	}

	ctx->blob->md_ro = false;
	_spdk_blob_remove_xattr(ctx->blob, SNAPSHOT_PENDING_REMOVAL, true);
	_spdk_blob_remove_xattr(ctx->blob, SNAPSHOT_IN_PROGRESS, true);
	spdk_blob_set_read_only(ctx->blob);

	if (ctx->iter_cb_fn) {
		ctx->iter_cb_fn(ctx->iter_cb_arg, ctx->blob, 0);
	}
	_spdk_bs_blob_list_add(ctx->blob);

	spdk_bs_iter_next(ctx->bs, ctx->blob, _spdk_bs_load_iter, ctx);
}

static void
_spdk_bs_examine_clone(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Failed to open clone of a corrupted blob\n");
		spdk_bs_iter_next(ctx->bs, ctx->blob, _spdk_bs_load_iter, ctx);
		return;
	}

	if (blob->parent_id == ctx->blob->id) {
		/* Power failure occured before updating clone (snapshot delete case)
		 * or after updating clone (creating snapshot case) - keep snapshot */
		spdk_blob_close(blob, _spdk_bs_update_corrupted_blob, ctx);
	} else {
		/* Power failure occured after updating clone (snapshot delete case)
		 * or before updating clone (creating snapshot case) - remove snapshot */
		spdk_blob_close(blob, _spdk_bs_delete_corrupted_blob, ctx);
	}
}

static void
_spdk_bs_load_iter(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = arg;
	const void *value;
	size_t len;
	int rc = 0;

	if (bserrno == 0) {
		/* Examine blob if it is corrupted after power failure. Fix
		 * the ones that can be fixed and remove any other corrupted
		 * ones. If it is not corrupted just process it */
		rc = _spdk_blob_get_xattr_value(blob, SNAPSHOT_PENDING_REMOVAL, &value, &len, true);
		if (rc != 0) {
			rc = _spdk_blob_get_xattr_value(blob, SNAPSHOT_IN_PROGRESS, &value, &len, true);
			if (rc != 0) {
				/* Not corrupted - process it and continue with iterating through blobs */
				if (ctx->iter_cb_fn) {
					ctx->iter_cb_fn(ctx->iter_cb_arg, blob, 0);
				}
				_spdk_bs_blob_list_add(blob);
				spdk_bs_iter_next(ctx->bs, blob, _spdk_bs_load_iter, ctx);
				return;
			}

		}

		assert(len == sizeof(spdk_blob_id));

		ctx->blob = blob;

		/* Open clone to check if we are able to fix this blob or should we remove it */
		spdk_bs_open_blob(ctx->bs, *(spdk_blob_id *)value, _spdk_bs_examine_clone, ctx);
		return;
	} else if (bserrno == -ENOENT) {
		bserrno = 0;
	} else {
		/*
		 * This case needs to be looked at further.  Same problem
		 *  exists with applications that rely on explicit blob
		 *  iteration.  We should just skip the blob that failed
		 *  to load and continue on to the next one.
		 */
		SPDK_ERRLOG("Error in iterating blobs\n");
	}

	ctx->iter_cb_fn = NULL;

	spdk_free(ctx->super);
	spdk_free(ctx->mask);
	bs_sequence_finish(ctx->seq, bserrno);
	free(ctx);
}

static void
_spdk_bs_load_complete(struct spdk_bs_load_ctx *ctx)
{
	spdk_bs_iter_first(ctx->bs, _spdk_bs_load_iter, ctx);
}

static void
_spdk_bs_load_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int rc;

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_BLOBIDS);

	/* The length of the mask (in bits) must not be greater than
	 * the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE * 8));

	/* The length of the mask must be exactly equal to the size
	 * (in pages) of the metadata region */
	assert(ctx->mask->length == ctx->super->md_len);

	rc = _spdk_bs_load_mask(&ctx->bs->used_blobids, ctx->mask);
	if (rc < 0) {
		spdk_free(ctx->mask);
		_spdk_bs_load_ctx_fail(ctx, rc);
		return;
	}

	_spdk_bs_load_complete(ctx);
}

static void
_spdk_bs_load_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
	int			rc;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_cluster_mask_len * sizeof(
					     struct spdk_blob_md_page) * 8));
	/* The length of the mask must be exactly equal to the total number of clusters */
	assert(ctx->mask->length == ctx->bs->total_clusters);

	rc = _spdk_bs_load_mask(&ctx->bs->used_clusters, ctx->mask);
	if (rc < 0) {
		spdk_free(ctx->mask);
		_spdk_bs_load_ctx_fail(ctx, rc);
		return;
	}

	ctx->bs->num_free_clusters = spdk_bit_array_count_clear(ctx->bs->used_clusters);
	assert(ctx->bs->num_free_clusters <= ctx->bs->total_clusters);

	spdk_free(ctx->mask);

	/* Read the used blobids mask */
	mask_size = ctx->super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
			     _spdk_bs_load_used_blobids_cpl, ctx);
}

static void
_spdk_bs_load_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
	int			rc;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_PAGES);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE *
				     8));
	/* The length of the mask must be exactly equal to the size (in pages) of the metadata region */
	assert(ctx->mask->length == ctx->super->md_len);

	rc = _spdk_bs_load_mask(&ctx->bs->used_md_pages, ctx->mask);
	if (rc < 0) {
		spdk_free(ctx->mask);
		_spdk_bs_load_ctx_fail(ctx, rc);
		return;
	}

	spdk_free(ctx->mask);

	/* Read the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
			     _spdk_bs_load_used_clusters_cpl, ctx);
}

static void
_spdk_bs_load_read_used_pages(struct spdk_bs_load_ctx *ctx)
{
	uint64_t lba, lba_count, mask_size;

	/* Read the used pages mask */
	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_zmalloc(mask_size, 0x1000, NULL,
				 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	bs_sequence_read_dev(ctx->seq, ctx->mask, lba, lba_count,
			     _spdk_bs_load_used_pages_cpl, ctx);
}

static int
_spdk_bs_load_replay_md_parse_page(struct spdk_bs_load_ctx *ctx, struct spdk_blob_md_page *page)
{
	struct spdk_blob_store *bs = ctx->bs;
	struct spdk_blob_md_descriptor *desc;
	size_t	cur_desc = 0;

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i, j;
			unsigned int				cluster_count = 0;
			uint32_t				cluster_idx;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				for (j = 0; j < desc_extent_rle->extents[i].length; j++) {
					cluster_idx = desc_extent_rle->extents[i].cluster_idx;
					/*
					 * cluster_idx = 0 means an unallocated cluster - don't mark that
					 * in the used cluster map.
					 */
					if (cluster_idx != 0) {
						spdk_bit_array_set(bs->used_clusters, cluster_idx + j);
						if (bs->num_free_clusters == 0) {
							return -ENOSPC;
						}
						bs->num_free_clusters--;
					}
					cluster_count++;
				}
			}
			if (cluster_count == 0) {
				return -EINVAL;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			uint32_t					i;
			uint32_t					cluster_count = 0;
			uint32_t					cluster_idx;
			size_t						cluster_idx_length;

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;
			cluster_idx_length = desc_extent->length - sizeof(desc_extent->start_cluster_idx);

			if (desc_extent->length <= sizeof(desc_extent->start_cluster_idx) ||
			    (cluster_idx_length % sizeof(desc_extent->cluster_idx[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < cluster_idx_length / sizeof(desc_extent->cluster_idx[0]); i++) {
				cluster_idx = desc_extent->cluster_idx[i];
				/*
				 * cluster_idx = 0 means an unallocated cluster - don't mark that
				 * in the used cluster map.
				 */
				if (cluster_idx != 0) {
					if (cluster_idx < desc_extent->start_cluster_idx &&
					    cluster_idx >= desc_extent->start_cluster_idx + cluster_count) {
						return -EINVAL;
					}
					spdk_bit_array_set(bs->used_clusters, cluster_idx);
					if (bs->num_free_clusters == 0) {
						return -ENOSPC;
					}
					bs->num_free_clusters--;
				}
				cluster_count++;
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_TABLE) {
			struct spdk_blob_md_descriptor_extent_table *desc_extent_table;
			uint32_t num_extent_pages = ctx->num_extent_pages;
			uint32_t i;
			size_t extent_pages_length;
			void *tmp;

			desc_extent_table = (struct spdk_blob_md_descriptor_extent_table *)desc;
			extent_pages_length = desc_extent_table->length - sizeof(desc_extent_table->num_clusters);

			if (desc_extent_table->length == 0 ||
			    (extent_pages_length % sizeof(desc_extent_table->extent_page[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
				if (desc_extent_table->extent_page[i].page_idx != 0) {
					if (desc_extent_table->extent_page[i].num_pages != 1) {
						return -EINVAL;
					}
					num_extent_pages += 1;
				}
			}

			if (num_extent_pages > 0) {
				tmp = realloc(ctx->extent_page_num, num_extent_pages * sizeof(uint32_t));
				if (tmp == NULL) {
					return -ENOMEM;
				}
				ctx->extent_page_num = tmp;

				/* Extent table entries contain md page numbers for extent pages.
				 * Zeroes represent unallocated extent pages, those are run-length-encoded.
				 */
				for (i = 0; i < extent_pages_length / sizeof(desc_extent_table->extent_page[0]); i++) {
					if (desc_extent_table->extent_page[i].page_idx != 0) {
						ctx->extent_page_num[ctx->num_extent_pages] = desc_extent_table->extent_page[i].page_idx;
						ctx->num_extent_pages += 1;
					}
				}
			}
		} else {
			/* Error */
			return -EINVAL;
		}
		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}
	return 0;
}

static bool _spdk_bs_load_cur_extent_page_valid(struct spdk_blob_md_page *page)
{
	uint32_t crc;
	struct spdk_blob_md_descriptor *desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	size_t desc_len;

	crc = _spdk_blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		return false;
	}

	/* Extent page should always be of sequence num 0. */
	if (page->sequence_num != 0) {
		return false;
	}

	/* Descriptor type must be EXTENT_PAGE. */
	if (desc->type != SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
		return false;
	}

	/* Descriptor length cannot exceed the page. */
	desc_len = sizeof(*desc) + desc->length;
	if (desc_len > sizeof(page->descriptors)) {
		return false;
	}

	/* It has to be the only descriptor in the page. */
	if (desc_len + sizeof(*desc) <= sizeof(page->descriptors)) {
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + desc_len);
		if (desc->length != 0) {
			return false;
		}
	}

	return true;
}

static bool _spdk_bs_load_cur_md_page_valid(struct spdk_bs_load_ctx *ctx)
{
	uint32_t crc;
	struct spdk_blob_md_page *page = ctx->page;

	crc = _spdk_blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		return false;
	}

	/* First page of a sequence should match the blobid. */
	if (page->sequence_num == 0 &&
	    _spdk_bs_page_to_blobid(ctx->cur_page) != page->id) {
		return false;
	}
	assert(_spdk_bs_load_cur_extent_page_valid(page) == false);

	return true;
}

static void
_spdk_bs_load_replay_cur_md_page(struct spdk_bs_load_ctx *ctx);

static void
_spdk_bs_load_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	_spdk_bs_load_complete(ctx);
}

static void
_spdk_bs_load_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	_spdk_bs_write_used_clusters(seq, ctx, _spdk_bs_load_write_used_clusters_cpl);
}

static void
_spdk_bs_load_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	_spdk_bs_write_used_blobids(seq, ctx, _spdk_bs_load_write_used_blobids_cpl);
}

static void
_spdk_bs_load_write_used_md(struct spdk_bs_load_ctx *ctx)
{
	_spdk_bs_write_used_md(ctx->seq, ctx, _spdk_bs_load_write_used_pages_cpl);
}

static void
_spdk_bs_load_replay_md_chain_cpl(struct spdk_bs_load_ctx *ctx)
{
	uint64_t num_md_clusters;
	uint64_t i;

	ctx->in_page_chain = false;

	do {
		ctx->page_index++;
	} while (spdk_bit_array_get(ctx->bs->used_md_pages, ctx->page_index) == true);

	if (ctx->page_index < ctx->super->md_len) {
		ctx->cur_page = ctx->page_index;
		_spdk_bs_load_replay_cur_md_page(ctx);
	} else {
		/* Claim all of the clusters used by the metadata */
		num_md_clusters = spdk_divide_round_up(ctx->super->md_len, ctx->bs->pages_per_cluster);
		for (i = 0; i < num_md_clusters; i++) {
			_spdk_bs_claim_cluster(ctx->bs, i);
		}
		spdk_free(ctx->page);
		_spdk_bs_load_write_used_md(ctx);
	}
}

static void
_spdk_bs_load_replay_extent_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t page_num;
	uint64_t i;

	if (bserrno != 0) {
		spdk_free(ctx->extent_pages);
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	for (i = 0; i < ctx->num_extent_pages; i++) {
		/* Extent pages are only read when present within in chain md.
		 * Integrity of md is not right if that page was not a valid extent page. */
		if (_spdk_bs_load_cur_extent_page_valid(&ctx->extent_pages[i]) != true) {
			spdk_free(ctx->extent_pages);
			_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
			return;
		}

		page_num = ctx->extent_page_num[i];
		spdk_bit_array_set(ctx->bs->used_md_pages, page_num);
		if (_spdk_bs_load_replay_md_parse_page(ctx, &ctx->extent_pages[i])) {
			spdk_free(ctx->extent_pages);
			_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
			return;
		}
	}

	spdk_free(ctx->extent_pages);
	free(ctx->extent_page_num);
	ctx->extent_page_num = NULL;
	ctx->num_extent_pages = 0;

	_spdk_bs_load_replay_md_chain_cpl(ctx);
}

static void
_spdk_bs_load_replay_extent_pages(struct spdk_bs_load_ctx *ctx)
{
	spdk_bs_batch_t *batch;
	uint32_t page;
	uint64_t lba;
	uint64_t i;

	ctx->extent_pages = spdk_zmalloc(SPDK_BS_PAGE_SIZE * ctx->num_extent_pages, SPDK_BS_PAGE_SIZE,
					 NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->extent_pages) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	batch = bs_sequence_to_batch(ctx->seq, _spdk_bs_load_replay_extent_page_cpl, ctx);

	for (i = 0; i < ctx->num_extent_pages; i++) {
		page = ctx->extent_page_num[i];
		assert(page < ctx->super->md_len);
		lba = _spdk_bs_md_page_to_lba(ctx->bs, page);
		bs_batch_read_dev(batch, &ctx->extent_pages[i], lba,
				  _spdk_bs_byte_to_lba(ctx->bs, SPDK_BS_PAGE_SIZE));
	}

	bs_batch_close(batch);
}

static void
_spdk_bs_load_replay_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t page_num;
	struct spdk_blob_md_page *page;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(ctx, bserrno);
		return;
	}

	page_num = ctx->cur_page;
	page = ctx->page;
	if (_spdk_bs_load_cur_md_page_valid(ctx) == true) {
		if (page->sequence_num == 0 || ctx->in_page_chain == true) {
			_spdk_bs_claim_md_page(ctx->bs, page_num);
			if (page->sequence_num == 0) {
				spdk_bit_array_set(ctx->bs->used_blobids, page_num);
			}
			if (_spdk_bs_load_replay_md_parse_page(ctx, page)) {
				_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
				return;
			}
			if (page->next != SPDK_INVALID_MD_PAGE) {
				ctx->in_page_chain = true;
				ctx->cur_page = page->next;
				_spdk_bs_load_replay_cur_md_page(ctx);
				return;
			}
			if (ctx->num_extent_pages != 0) {
				_spdk_bs_load_replay_extent_pages(ctx);
				return;
			}
		}
	}
	_spdk_bs_load_replay_md_chain_cpl(ctx);
}

static void
_spdk_bs_load_replay_cur_md_page(struct spdk_bs_load_ctx *ctx)
{
	uint64_t lba;

	assert(ctx->cur_page < ctx->super->md_len);
	lba = _spdk_bs_md_page_to_lba(ctx->bs, ctx->cur_page);
	bs_sequence_read_dev(ctx->seq, ctx->page, lba,
			     _spdk_bs_byte_to_lba(ctx->bs, SPDK_BS_PAGE_SIZE),
			     _spdk_bs_load_replay_md_cpl, ctx);
}

static void
_spdk_bs_load_replay_md(struct spdk_bs_load_ctx *ctx)
{
	ctx->page_index = 0;
	ctx->cur_page = 0;
	ctx->page = spdk_zmalloc(SPDK_BS_PAGE_SIZE, SPDK_BS_PAGE_SIZE,
				 NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->page) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	_spdk_bs_load_replay_cur_md_page(ctx);
}

static void
_spdk_bs_recover(struct spdk_bs_load_ctx *ctx)
{
	int		rc;

	rc = spdk_bit_array_resize(&ctx->bs->used_md_pages, ctx->super->md_len);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_blobids, ctx->super->md_len);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}

	ctx->bs->num_free_clusters = ctx->bs->total_clusters;
	_spdk_bs_load_replay_md(ctx);
}

static void
_spdk_bs_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t	crc;
	int		rc;
	static const char zeros[SPDK_BLOBSTORE_TYPE_LENGTH];

	if (ctx->super->version > SPDK_BS_VERSION ||
	    ctx->super->version < SPDK_BS_INITIAL_VERSION) {
		_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
		return;
	}

	if (memcmp(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(ctx->super->signature)) != 0) {
		_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
		return;
	}

	crc = _spdk_blob_md_page_calc_crc(ctx->super);
	if (crc != ctx->super->crc) {
		_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
		return;
	}

	if (memcmp(&ctx->bs->bstype, &ctx->super->bstype, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Bstype matched - loading blobstore\n");
	} else if (memcmp(&ctx->bs->bstype, zeros, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Bstype wildcard used - loading blobstore regardless bstype\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Unexpected bstype\n");
		SPDK_LOGDUMP(SPDK_LOG_BLOB, "Expected:", ctx->bs->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		SPDK_LOGDUMP(SPDK_LOG_BLOB, "Found:", ctx->super->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		_spdk_bs_load_ctx_fail(ctx, -ENXIO);
		return;
	}

	if (ctx->super->size > ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen) {
		SPDK_NOTICELOG("Size mismatch, dev size: %lu, blobstore size: %lu\n",
			       ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen, ctx->super->size);
		_spdk_bs_load_ctx_fail(ctx, -EILSEQ);
		return;
	}

	if (ctx->super->size == 0) {
		ctx->super->size = ctx->bs->dev->blockcnt * ctx->bs->dev->blocklen;
	}

	if (ctx->super->io_unit_size == 0) {
		ctx->super->io_unit_size = SPDK_BS_PAGE_SIZE;
	}

	/* Parse the super block */
	ctx->bs->clean = 1;
	ctx->bs->cluster_sz = ctx->super->cluster_size;
	ctx->bs->total_clusters = ctx->super->size / ctx->super->cluster_size;
	ctx->bs->pages_per_cluster = ctx->bs->cluster_sz / SPDK_BS_PAGE_SIZE;
	if (spdk_u32_is_pow2(ctx->bs->pages_per_cluster)) {
		ctx->bs->pages_per_cluster_shift = spdk_u32log2(ctx->bs->pages_per_cluster);
	}
	ctx->bs->io_unit_size = ctx->super->io_unit_size;
	rc = spdk_bit_array_resize(&ctx->bs->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(ctx, -ENOMEM);
		return;
	}
	ctx->bs->md_start = ctx->super->md_start;
	ctx->bs->md_len = ctx->super->md_len;
	ctx->bs->total_data_clusters = ctx->bs->total_clusters - spdk_divide_round_up(
					       ctx->bs->md_start + ctx->bs->md_len, ctx->bs->pages_per_cluster);
	ctx->bs->super_blob = ctx->super->super_blob;
	memcpy(&ctx->bs->bstype, &ctx->super->bstype, sizeof(ctx->super->bstype));

	if (ctx->super->used_blobid_mask_len == 0 || ctx->super->clean == 0) {
		_spdk_bs_recover(ctx);
	} else {
		_spdk_bs_load_read_used_pages(ctx);
	}
}

void
spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_load_ctx *ctx;
	struct spdk_bs_opts	opts = {};
	int err;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Loading blobstore from dev %p\n", dev);

	if ((SPDK_BS_PAGE_SIZE % dev->blocklen) != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "unsupported dev block length of %d\n", dev->blocklen);
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (o) {
		opts = *o;
	} else {
		spdk_bs_opts_init(&opts);
	}

	if (opts.max_md_ops == 0 || opts.max_channel_ops == 0) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	err = _spdk_bs_alloc(dev, &opts, &bs);
	if (err) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, err);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->bs = bs;
	ctx->iter_cb_fn = opts.iter_cb_fn;
	ctx->iter_cb_arg = opts.iter_cb_arg;

	/* Allocate memory for the super block */
	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	ctx->seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* Read the super block */
	bs_sequence_read_dev(ctx->seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			     _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     _spdk_bs_load_super_cpl, ctx);
}

/* END spdk_bs_load */

/* START spdk_bs_dump */

struct spdk_bs_dump_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
	uint32_t			cur_page;
	struct spdk_blob_md_page	*page;
	spdk_bs_sequence_t		*seq;
	FILE				*fp;
	spdk_bs_dump_print_xattr	print_xattr_fn;
	char				xattr_name[4096];
};

static void
_spdk_bs_dump_finish(spdk_bs_sequence_t *seq, struct spdk_bs_dump_ctx *ctx, int bserrno)
{
	spdk_free(ctx->super);

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	ctx->bs->unload_err = bserrno;
	memcpy(&ctx->bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	bs_sequence_finish(seq, 0);
	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void _spdk_bs_dump_read_md_page(spdk_bs_sequence_t *seq, void *cb_arg);

static void
_spdk_bs_dump_print_md_page(struct spdk_bs_dump_ctx *ctx)
{
	uint32_t page_idx = ctx->cur_page;
	struct spdk_blob_md_page *page = ctx->page;
	struct spdk_blob_md_descriptor *desc;
	size_t cur_desc = 0;
	uint32_t crc;

	fprintf(ctx->fp, "=========\n");
	fprintf(ctx->fp, "Metadata Page Index: %" PRIu32 " (0x%" PRIx32 ")\n", page_idx, page_idx);
	fprintf(ctx->fp, "Blob ID: 0x%" PRIx64 "\n", page->id);

	crc = _spdk_blob_md_page_calc_crc(page);
	fprintf(ctx->fp, "CRC: 0x%" PRIx32 " (%s)\n", page->crc, crc == page->crc ? "OK" : "Mismatch");

	desc = (struct spdk_blob_md_descriptor *)page->descriptors;
	while (cur_desc < sizeof(page->descriptors)) {
		if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_PADDING) {
			if (desc->length == 0) {
				/* If padding and length are 0, this terminates the page */
				break;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_RLE) {
			struct spdk_blob_md_descriptor_extent_rle	*desc_extent_rle;
			unsigned int				i;

			desc_extent_rle = (struct spdk_blob_md_descriptor_extent_rle *)desc;

			for (i = 0; i < desc_extent_rle->length / sizeof(desc_extent_rle->extents[0]); i++) {
				if (desc_extent_rle->extents[i].cluster_idx != 0) {
					fprintf(ctx->fp, "Allocated Extent - Start: %" PRIu32,
						desc_extent_rle->extents[i].cluster_idx);
				} else {
					fprintf(ctx->fp, "Unallocated Extent - ");
				}
				fprintf(ctx->fp, " Length: %" PRIu32, desc_extent_rle->extents[i].length);
				fprintf(ctx->fp, "\n");
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT_PAGE) {
			struct spdk_blob_md_descriptor_extent_page	*desc_extent;
			unsigned int					i;

			desc_extent = (struct spdk_blob_md_descriptor_extent_page *)desc;

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->cluster_idx[0]); i++) {
				if (desc_extent->cluster_idx[i] != 0) {
					fprintf(ctx->fp, "Allocated Extent - Start: %" PRIu32,
						desc_extent->cluster_idx[i]);
				} else {
					fprintf(ctx->fp, "Unallocated Extent");
				}
				fprintf(ctx->fp, "\n");
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			struct spdk_blob_md_descriptor_xattr *desc_xattr;
			uint32_t i;

			desc_xattr = (struct spdk_blob_md_descriptor_xattr *)desc;

			if (desc_xattr->length !=
			    sizeof(desc_xattr->name_length) + sizeof(desc_xattr->value_length) +
			    desc_xattr->name_length + desc_xattr->value_length) {
			}

			memcpy(ctx->xattr_name, desc_xattr->name, desc_xattr->name_length);
			ctx->xattr_name[desc_xattr->name_length] = '\0';
			fprintf(ctx->fp, "XATTR: name = \"%s\"\n", ctx->xattr_name);
			fprintf(ctx->fp, "       value = \"");
			ctx->print_xattr_fn(ctx->fp, ctx->super->bstype.bstype, ctx->xattr_name,
					    (void *)((uintptr_t)desc_xattr->name + desc_xattr->name_length),
					    desc_xattr->value_length);
			fprintf(ctx->fp, "\"\n");
			for (i = 0; i < desc_xattr->value_length; i++) {
				if (i % 16 == 0) {
					fprintf(ctx->fp, "               ");
				}
				fprintf(ctx->fp, "%02" PRIx8 " ", *((uint8_t *)desc_xattr->name + desc_xattr->name_length + i));
				if ((i + 1) % 16 == 0) {
					fprintf(ctx->fp, "\n");
				}
			}
			if (i % 16 != 0) {
				fprintf(ctx->fp, "\n");
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			/* TODO */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			/* TODO */
		} else {
			/* Error */
		}
		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}
}

static void
_spdk_bs_dump_read_md_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_dump_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		_spdk_bs_dump_finish(seq, ctx, bserrno);
		return;
	}

	if (ctx->page->id != 0) {
		_spdk_bs_dump_print_md_page(ctx);
	}

	ctx->cur_page++;

	if (ctx->cur_page < ctx->super->md_len) {
		_spdk_bs_dump_read_md_page(seq, ctx);
	} else {
		spdk_free(ctx->page);
		_spdk_bs_dump_finish(seq, ctx, 0);
	}
}

static void
_spdk_bs_dump_read_md_page(spdk_bs_sequence_t *seq, void *cb_arg)
{
	struct spdk_bs_dump_ctx *ctx = cb_arg;
	uint64_t lba;

	assert(ctx->cur_page < ctx->super->md_len);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->md_start + ctx->cur_page);
	bs_sequence_read_dev(seq, ctx->page, lba,
			     _spdk_bs_byte_to_lba(ctx->bs, SPDK_BS_PAGE_SIZE),
			     _spdk_bs_dump_read_md_page_cpl, ctx);
}

static void
_spdk_bs_dump_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_dump_ctx *ctx = cb_arg;

	fprintf(ctx->fp, "Signature: \"%.8s\" ", ctx->super->signature);
	if (memcmp(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(ctx->super->signature)) != 0) {
		fprintf(ctx->fp, "(Mismatch)\n");
		_spdk_bs_dump_finish(seq, ctx, bserrno);
		return;
	} else {
		fprintf(ctx->fp, "(OK)\n");
	}
	fprintf(ctx->fp, "Version: %" PRIu32 "\n", ctx->super->version);
	fprintf(ctx->fp, "CRC: 0x%x (%s)\n", ctx->super->crc,
		(ctx->super->crc == _spdk_blob_md_page_calc_crc(ctx->super)) ? "OK" : "Mismatch");
	fprintf(ctx->fp, "Blobstore Type: %.*s\n", SPDK_BLOBSTORE_TYPE_LENGTH, ctx->super->bstype.bstype);
	fprintf(ctx->fp, "Cluster Size: %" PRIu32 "\n", ctx->super->cluster_size);
	fprintf(ctx->fp, "Super Blob ID: ");
	if (ctx->super->super_blob == SPDK_BLOBID_INVALID) {
		fprintf(ctx->fp, "(None)\n");
	} else {
		fprintf(ctx->fp, "%" PRIu64 "\n", ctx->super->super_blob);
	}
	fprintf(ctx->fp, "Clean: %" PRIu32 "\n", ctx->super->clean);
	fprintf(ctx->fp, "Used Metadata Page Mask Start: %" PRIu32 "\n", ctx->super->used_page_mask_start);
	fprintf(ctx->fp, "Used Metadata Page Mask Length: %" PRIu32 "\n", ctx->super->used_page_mask_len);
	fprintf(ctx->fp, "Used Cluster Mask Start: %" PRIu32 "\n", ctx->super->used_cluster_mask_start);
	fprintf(ctx->fp, "Used Cluster Mask Length: %" PRIu32 "\n", ctx->super->used_cluster_mask_len);
	fprintf(ctx->fp, "Used Blob ID Mask Start: %" PRIu32 "\n", ctx->super->used_blobid_mask_start);
	fprintf(ctx->fp, "Used Blob ID Mask Length: %" PRIu32 "\n", ctx->super->used_blobid_mask_len);
	fprintf(ctx->fp, "Metadata Start: %" PRIu32 "\n", ctx->super->md_start);
	fprintf(ctx->fp, "Metadata Length: %" PRIu32 "\n", ctx->super->md_len);

	ctx->cur_page = 0;
	ctx->page = spdk_zmalloc(SPDK_BS_PAGE_SIZE, SPDK_BS_PAGE_SIZE,
				 NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->page) {
		_spdk_bs_dump_finish(seq, ctx, -ENOMEM);
		return;
	}
	_spdk_bs_dump_read_md_page(seq, ctx);
}

void
spdk_bs_dump(struct spdk_bs_dev *dev, FILE *fp, spdk_bs_dump_print_xattr print_xattr_fn,
	     spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_dump_ctx *ctx;
	struct spdk_bs_opts	opts = {};
	int err;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Dumping blobstore from dev %p\n", dev);

	spdk_bs_opts_init(&opts);

	err = _spdk_bs_alloc(dev, &opts, &bs);
	if (err) {
		dev->destroy(dev);
		cb_fn(cb_arg, err);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;
	ctx->fp = fp;
	ctx->print_xattr_fn = print_xattr_fn;

	/* Allocate memory for the super block */
	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read the super block */
	bs_sequence_read_dev(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			     _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     _spdk_bs_dump_super_cpl, ctx);
}

/* END spdk_bs_dump */

/* START spdk_bs_init */

struct spdk_bs_init_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
};

static void
_spdk_bs_init_persist_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;

	spdk_free(ctx->super);
	free(ctx);

	bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_init_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;

	/* Write super block */
	bs_sequence_write_dev(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
			      _spdk_bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			      _spdk_bs_init_persist_super_cpl, ctx);
}

void
spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_init_ctx *ctx;
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	spdk_bs_batch_t		*batch;
	uint64_t		num_md_lba;
	uint64_t		num_md_pages;
	uint64_t		num_md_clusters;
	uint32_t		i;
	struct spdk_bs_opts	opts = {};
	int			rc;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Initializing blobstore on dev %p\n", dev);

	if ((SPDK_BS_PAGE_SIZE % dev->blocklen) != 0) {
		SPDK_ERRLOG("unsupported dev block length of %d\n",
			    dev->blocklen);
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (o) {
		opts = *o;
	} else {
		spdk_bs_opts_init(&opts);
	}

	if (_spdk_bs_opts_verify(&opts) != 0) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = _spdk_bs_alloc(dev, &opts, &bs);
	if (rc) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	if (opts.num_md_pages == SPDK_BLOB_OPTS_NUM_MD_PAGES) {
		/* By default, allocate 1 page per cluster.
		 * Technically, this over-allocates metadata
		 * because more metadata will reduce the number
		 * of usable clusters. This can be addressed with
		 * more complex math in the future.
		 */
		bs->md_len = bs->total_clusters;
	} else {
		bs->md_len = opts.num_md_pages;
	}
	rc = spdk_bit_array_resize(&bs->used_md_pages, bs->md_len);
	if (rc < 0) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&bs->used_blobids, bs->md_len);
	if (rc < 0) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	/* Allocate memory for the super block */
	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	memcpy(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
	       sizeof(ctx->super->signature));
	ctx->super->version = SPDK_BS_VERSION;
	ctx->super->length = sizeof(*ctx->super);
	ctx->super->super_blob = bs->super_blob;
	ctx->super->clean = 0;
	ctx->super->cluster_size = bs->cluster_sz;
	ctx->super->io_unit_size = bs->io_unit_size;
	memcpy(&ctx->super->bstype, &bs->bstype, sizeof(bs->bstype));

	/* Calculate how many pages the metadata consumes at the front
	 * of the disk.
	 */

	/* The super block uses 1 page */
	num_md_pages = 1;

	/* The used_md_pages mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_page_mask_start = num_md_pages;
	ctx->super->used_page_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					 spdk_divide_round_up(bs->md_len, 8),
					 SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_page_mask_len;

	/* The used_clusters mask requires 1 bit per cluster, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_cluster_mask_start = num_md_pages;
	ctx->super->used_cluster_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					    spdk_divide_round_up(bs->total_clusters, 8),
					    SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_cluster_mask_len;

	/* The used_blobids mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_blobid_mask_start = num_md_pages;
	ctx->super->used_blobid_mask_len = spdk_divide_round_up(sizeof(struct spdk_bs_md_mask) +
					   spdk_divide_round_up(bs->md_len, 8),
					   SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_blobid_mask_len;

	/* The metadata region size was chosen above */
	ctx->super->md_start = bs->md_start = num_md_pages;
	ctx->super->md_len = bs->md_len;
	num_md_pages += bs->md_len;

	num_md_lba = _spdk_bs_page_to_lba(bs, num_md_pages);

	ctx->super->size = dev->blockcnt * dev->blocklen;

	ctx->super->crc = _spdk_blob_md_page_calc_crc(ctx->super);

	num_md_clusters = spdk_divide_round_up(num_md_pages, bs->pages_per_cluster);
	if (num_md_clusters > bs->total_clusters) {
		SPDK_ERRLOG("Blobstore metadata cannot use more clusters than is available, "
			    "please decrease number of pages reserved for metadata "
			    "or increase cluster size.\n");
		spdk_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	/* Claim all of the clusters used by the metadata */
	for (i = 0; i < num_md_clusters; i++) {
		_spdk_bs_claim_cluster(bs, i);
	}

	bs->total_data_clusters = bs->num_free_clusters;

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	batch = bs_sequence_to_batch(seq, _spdk_bs_init_trim_cpl, ctx);

	/* Clear metadata space */
	bs_batch_write_zeroes_dev(batch, 0, num_md_lba);

	switch (opts.clear_method) {
	case BS_CLEAR_WITH_UNMAP:
		/* Trim data clusters */
		bs_batch_unmap_dev(batch, num_md_lba, ctx->bs->dev->blockcnt - num_md_lba);
		break;
	case BS_CLEAR_WITH_WRITE_ZEROES:
		/* Write_zeroes to data clusters */
		bs_batch_write_zeroes_dev(batch, num_md_lba, ctx->bs->dev->blockcnt - num_md_lba);
		break;
	case BS_CLEAR_WITH_NONE:
	default:
		break;
	}

	bs_batch_close(batch);
}

/* END spdk_bs_init */

/* START spdk_bs_destroy */

static void
_spdk_bs_destroy_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	bs->unload_err = bserrno;
	memcpy(&bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	bs_sequence_finish(seq, bserrno);

	_spdk_bs_free(bs);
	free(ctx);
}

void
spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_init_ctx *ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Destroying blobstore\n");

	if (!TAILQ_EMPTY(&bs->blobs)) {
		SPDK_ERRLOG("Blobstore still has open blobs\n");
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Write zeroes to the super block */
	bs_sequence_write_zeroes_dev(seq,
				     _spdk_bs_page_to_lba(bs, 0),
				     _spdk_bs_byte_to_lba(bs, sizeof(struct spdk_bs_super_block)),
				     _spdk_bs_destroy_trim_cpl, ctx);
}

/* END spdk_bs_destroy */

/* START spdk_bs_unload */

static void
_spdk_bs_unload_finish(struct spdk_bs_load_ctx *ctx, int bserrno)
{
	spdk_bs_sequence_t *seq = ctx->seq;

	spdk_free(ctx->super);

	/*
	 * We need to defer calling bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	ctx->bs->unload_err = bserrno;
	memcpy(&ctx->bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	bs_sequence_finish(seq, bserrno);

	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void
_spdk_bs_unload_write_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	_spdk_bs_unload_finish(ctx, bserrno);
}

static void
_spdk_bs_unload_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);

	if (bserrno != 0) {
		_spdk_bs_unload_finish(ctx, bserrno);
		return;
	}

	ctx->super->clean = 1;

	_spdk_bs_write_super(seq, ctx->bs, ctx->super, _spdk_bs_unload_write_super_cpl, ctx);
}

static void
_spdk_bs_unload_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		_spdk_bs_unload_finish(ctx, bserrno);
		return;
	}

	_spdk_bs_write_used_clusters(seq, ctx, _spdk_bs_unload_write_used_clusters_cpl);
}

static void
_spdk_bs_unload_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_free(ctx->mask);
	ctx->mask = NULL;

	if (bserrno != 0) {
		_spdk_bs_unload_finish(ctx, bserrno);
		return;
	}

	_spdk_bs_write_used_blobids(seq, ctx, _spdk_bs_unload_write_used_blobids_cpl);
}

static void
_spdk_bs_unload_read_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		_spdk_bs_unload_finish(ctx, bserrno);
		return;
	}

	_spdk_bs_write_used_md(seq, cb_arg, _spdk_bs_unload_write_used_pages_cpl);
}

void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	struct spdk_bs_load_ctx *ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Syncing blobstore\n");

	if (!TAILQ_EMPTY(&bs->blobs)) {
		SPDK_ERRLOG("Blobstore still has open blobs\n");
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	ctx->seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!ctx->seq) {
		spdk_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read super block */
	bs_sequence_read_dev(ctx->seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			     _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     _spdk_bs_unload_read_super_cpl, ctx);
}

/* END spdk_bs_unload */

/* START spdk_bs_set_super */

struct spdk_bs_set_super_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
};

static void
_spdk_bs_set_super_write_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_set_super_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Unable to write to super block of blobstore\n");
	}

	spdk_free(ctx->super);

	bs_sequence_finish(seq, bserrno);

	free(ctx);
}

static void
_spdk_bs_set_super_read_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_set_super_ctx	*ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Unable to read super block of blobstore\n");
		spdk_free(ctx->super);
		bs_sequence_finish(seq, bserrno);
		free(ctx);
		return;
	}

	_spdk_bs_write_super(seq, ctx->bs, ctx->super, _spdk_bs_set_super_write_cpl, ctx);
}

void
spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl		cpl;
	spdk_bs_sequence_t		*seq;
	struct spdk_bs_set_super_ctx	*ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Setting super blob id on blobstore\n");

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	ctx->super = spdk_zmalloc(sizeof(*ctx->super), 0x1000, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	bs->super_blob = blobid;

	/* Read super block */
	bs_sequence_read_dev(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			     _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			     _spdk_bs_set_super_read_cpl, ctx);
}

/* END spdk_bs_set_super */

void
spdk_bs_get_super(struct spdk_blob_store *bs,
		  spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	if (bs->super_blob == SPDK_BLOBID_INVALID) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOENT);
	} else {
		cb_fn(cb_arg, bs->super_blob, 0);
	}
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return bs->cluster_sz;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

uint64_t
spdk_bs_get_io_unit_size(struct spdk_blob_store *bs)
{
	return bs->io_unit_size;
}

uint64_t
spdk_bs_free_cluster_count(struct spdk_blob_store *bs)
{
	return bs->num_free_clusters;
}

uint64_t
spdk_bs_total_data_cluster_count(struct spdk_blob_store *bs)
{
	return bs->total_data_clusters;
}

static int
spdk_bs_register_md_thread(struct spdk_blob_store *bs)
{
	bs->md_channel = spdk_get_io_channel(bs);
	if (!bs->md_channel) {
		SPDK_ERRLOG("Failed to get IO channel.\n");
		return -1;
	}

	return 0;
}

static int
spdk_bs_unregister_md_thread(struct spdk_blob_store *bs)
{
	spdk_put_io_channel(bs->md_channel);

	return 0;
}

spdk_blob_id spdk_blob_get_id(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->id;
}

uint64_t spdk_blob_get_num_pages(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return _spdk_bs_cluster_to_page(blob->bs, blob->active.num_clusters);
}

uint64_t spdk_blob_get_num_io_units(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return spdk_blob_get_num_pages(blob) * _spdk_bs_io_unit_per_page(blob->bs);
}

uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->active.num_clusters;
}

/* START spdk_bs_create_blob */

static void
_spdk_bs_create_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	_spdk_blob_free(blob);

	bs_sequence_finish(seq, bserrno);
}

static int
_spdk_blob_set_xattrs(struct spdk_blob *blob, const struct spdk_blob_xattr_opts *xattrs,
		      bool internal)
{
	uint64_t i;
	size_t value_len = 0;
	int rc;
	const void *value = NULL;
	if (xattrs->count > 0 && xattrs->get_value == NULL) {
		return -EINVAL;
	}
	for (i = 0; i < xattrs->count; i++) {
		xattrs->get_value(xattrs->ctx, xattrs->names[i], &value, &value_len);
		if (value == NULL || value_len == 0) {
			return -EINVAL;
		}
		rc = _spdk_blob_set_xattr(blob, xattrs->names[i], value, value_len, internal);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static void
_spdk_bs_create_blob(struct spdk_blob_store *bs,
		     const struct spdk_blob_opts *opts,
		     const struct spdk_blob_xattr_opts *internal_xattrs,
		     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_blob	*blob;
	uint32_t		page_idx;
	struct spdk_bs_cpl	cpl;
	struct spdk_blob_opts	opts_default;
	struct spdk_blob_xattr_opts internal_xattrs_default;
	spdk_bs_sequence_t	*seq;
	spdk_blob_id		id;
	int rc;

	assert(spdk_get_thread() == bs->md_thread);

	page_idx = spdk_bit_array_find_first_clear(bs->used_md_pages, 0);
	if (page_idx == UINT32_MAX) {
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}
	spdk_bit_array_set(bs->used_blobids, page_idx);
	_spdk_bs_claim_md_page(bs, page_idx);

	id = _spdk_bs_page_to_blobid(page_idx);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Creating blob with id %lu at page %u\n", id, page_idx);

	blob = _spdk_blob_alloc(bs, id);
	if (!blob) {
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}

	if (!opts) {
		spdk_blob_opts_init(&opts_default);
		opts = &opts_default;
	}

	blob->use_extent_table = opts->use_extent_table;
	if (blob->use_extent_table) {
		blob->invalid_flags |= SPDK_BLOB_EXTENT_TABLE;
	}

	if (!internal_xattrs) {
		_spdk_blob_xattrs_init(&internal_xattrs_default);
		internal_xattrs = &internal_xattrs_default;
	}

	rc = _spdk_blob_set_xattrs(blob, &opts->xattrs, false);
	if (rc < 0) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, rc);
		return;
	}

	rc = _spdk_blob_set_xattrs(blob, internal_xattrs, true);
	if (rc < 0) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, rc);
		return;
	}

	if (opts->thin_provision) {
		_spdk_blob_set_thin_provision(blob);
	}

	_spdk_blob_set_clear_method(blob, opts->clear_method);

	rc = _spdk_blob_resize(blob, opts->num_clusters);
	if (rc < 0) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, rc);
		return;
	}
	cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	cpl.u.blobid.cb_fn = cb_fn;
	cpl.u.blobid.cb_arg = cb_arg;
	cpl.u.blobid.blobid = blob->id;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}

	_spdk_blob_persist(seq, blob, _spdk_bs_create_blob_cpl, blob);
}

void spdk_bs_create_blob(struct spdk_blob_store *bs,
			 spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	_spdk_bs_create_blob(bs, NULL, NULL, cb_fn, cb_arg);
}

void spdk_bs_create_blob_ext(struct spdk_blob_store *bs, const struct spdk_blob_opts *opts,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	_spdk_bs_create_blob(bs, opts, NULL, cb_fn, cb_arg);
}

/* END spdk_bs_create_blob */

/* START blob_cleanup */

struct spdk_clone_snapshot_ctx {
	struct spdk_bs_cpl      cpl;
	int bserrno;
	bool frozen;

	struct spdk_io_channel *channel;

	/* Current cluster for inflate operation */
	uint64_t cluster;

	/* For inflation force allocation of all unallocated clusters and remove
	 * thin-provisioning. Otherwise only decouple parent and keep clone thin. */
	bool allocate_all;

	struct {
		spdk_blob_id id;
		struct spdk_blob *blob;
	} original;
	struct {
		spdk_blob_id id;
		struct spdk_blob *blob;
	} new;

	/* xattrs specified for snapshot/clones only. They have no impact on
	 * the original blobs xattrs. */
	const struct spdk_blob_xattr_opts *xattrs;
};

static void
_spdk_bs_clone_snapshot_cleanup_finish(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = cb_arg;
	struct spdk_bs_cpl *cpl = &ctx->cpl;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	switch (cpl->type) {
	case SPDK_BS_CPL_TYPE_BLOBID:
		cpl->u.blobid.cb_fn(cpl->u.blobid.cb_arg, cpl->u.blobid.blobid, ctx->bserrno);
		break;
	case SPDK_BS_CPL_TYPE_BLOB_BASIC:
		cpl->u.blob_basic.cb_fn(cpl->u.blob_basic.cb_arg, ctx->bserrno);
		break;
	default:
		SPDK_UNREACHABLE();
		break;
	}

	free(ctx);
}

static void
_spdk_bs_snapshot_unfreeze_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Unfreeze error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	ctx->original.id = origblob->id;
	origblob->locked_operation_in_progress = false;

	spdk_blob_close(origblob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
}

static void
_spdk_bs_clone_snapshot_origblob_cleanup(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	if (ctx->frozen) {
		/* Unfreeze any outstanding I/O */
		_spdk_blob_unfreeze_io(origblob, _spdk_bs_snapshot_unfreeze_cpl, ctx);
	} else {
		_spdk_bs_snapshot_unfreeze_cpl(ctx, 0);
	}

}

static void
_spdk_bs_clone_snapshot_newblob_cleanup(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		if (ctx->bserrno != 0) {
			SPDK_ERRLOG("Cleanup error %d\n", bserrno);
		} else {
			ctx->bserrno = bserrno;
		}
	}

	ctx->new.id = newblob->id;
	spdk_blob_close(newblob, _spdk_bs_clone_snapshot_origblob_cleanup, ctx);
}

/* END blob_cleanup */

/* START spdk_bs_create_snapshot */

static void
_spdk_bs_snapshot_swap_cluster_maps(struct spdk_blob *blob1, struct spdk_blob *blob2)
{
	uint64_t *cluster_temp;
	uint32_t *extent_page_temp;

	cluster_temp = blob1->active.clusters;
	blob1->active.clusters = blob2->active.clusters;
	blob2->active.clusters = cluster_temp;

	extent_page_temp = blob1->active.extent_pages;
	blob1->active.extent_pages = blob2->active.extent_pages;
	blob2->active.extent_pages = extent_page_temp;
}

static void
_spdk_bs_snapshot_origblob_sync_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		_spdk_bs_snapshot_swap_cluster_maps(newblob, origblob);
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	/* Remove metadata descriptor SNAPSHOT_IN_PROGRESS */
	bserrno = _spdk_blob_remove_xattr(newblob, SNAPSHOT_IN_PROGRESS, true);
	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	_spdk_bs_blob_list_add(ctx->original.blob);

	spdk_blob_set_read_only(newblob);

	/* sync snapshot metadata */
	spdk_blob_sync_md(newblob, _spdk_bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
_spdk_bs_snapshot_newblob_sync_cpl(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;

	if (bserrno != 0) {
		/* return cluster map back to original */
		_spdk_bs_snapshot_swap_cluster_maps(newblob, origblob);

		/* Newblob md sync failed. Valid clusters are only present in origblob.
		 * Since I/O is frozen on origblob, not changes to zeroed out cluster map should have occured.
		 * Newblob needs to be reverted to thin_provisioned state at creation to properly close. */
		_spdk_blob_set_thin_provision(newblob);
		assert(spdk_mem_all_zero(newblob->active.clusters,
					 newblob->active.num_clusters * sizeof(*newblob->active.clusters)));
		assert(spdk_mem_all_zero(newblob->active.extent_pages,
					 newblob->active.num_extent_pages * sizeof(*newblob->active.extent_pages)));

		_spdk_bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
		return;
	}

	/* Set internal xattr for snapshot id */
	bserrno = _spdk_blob_set_xattr(origblob, BLOB_SNAPSHOT, &newblob->id, sizeof(spdk_blob_id), true);
	if (bserrno != 0) {
		/* return cluster map back to original */
		_spdk_bs_snapshot_swap_cluster_maps(newblob, origblob);
		_spdk_bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
		return;
	}

	_spdk_bs_blob_list_remove(origblob);
	origblob->parent_id = newblob->id;

	/* Create new back_bs_dev for snapshot */
	origblob->back_bs_dev = bs_create_blob_bs_dev(newblob);
	if (origblob->back_bs_dev == NULL) {
		/* return cluster map back to original */
		_spdk_bs_snapshot_swap_cluster_maps(newblob, origblob);
		_spdk_bs_clone_snapshot_newblob_cleanup(ctx, -EINVAL);
		return;
	}

	/* set clone blob as thin provisioned */
	_spdk_blob_set_thin_provision(origblob);

	_spdk_bs_blob_list_add(newblob);

	/* sync clone metadata */
	spdk_blob_sync_md(origblob, _spdk_bs_snapshot_origblob_sync_cpl, ctx);
}

static void
_spdk_bs_snapshot_freeze_cpl(void *cb_arg, int rc)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = ctx->new.blob;
	int bserrno;

	if (rc != 0) {
		_spdk_bs_clone_snapshot_newblob_cleanup(ctx, rc);
		return;
	}

	ctx->frozen = true;

	/* set new back_bs_dev for snapshot */
	newblob->back_bs_dev = origblob->back_bs_dev;
	/* Set invalid flags from origblob */
	newblob->invalid_flags = origblob->invalid_flags;

	/* inherit parent from original blob if set */
	newblob->parent_id = origblob->parent_id;
	if (origblob->parent_id != SPDK_BLOBID_INVALID) {
		/* Set internal xattr for snapshot id */
		bserrno = _spdk_blob_set_xattr(newblob, BLOB_SNAPSHOT,
					       &origblob->parent_id, sizeof(spdk_blob_id), true);
		if (bserrno != 0) {
			_spdk_bs_clone_snapshot_newblob_cleanup(ctx, bserrno);
			return;
		}
	}

	/* swap cluster maps */
	_spdk_bs_snapshot_swap_cluster_maps(newblob, origblob);

	/* Set the clear method on the new blob to match the original. */
	_spdk_blob_set_clear_method(newblob, origblob->clear_method);

	/* sync snapshot metadata */
	spdk_blob_sync_md(newblob, _spdk_bs_snapshot_newblob_sync_cpl, ctx);
}

static void
_spdk_bs_snapshot_newblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;
	struct spdk_blob *newblob = _blob;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	ctx->new.blob = newblob;
	assert(spdk_blob_is_thin_provisioned(newblob));
	assert(spdk_mem_all_zero(newblob->active.clusters,
				 newblob->active.num_clusters * sizeof(*newblob->active.clusters)));
	assert(spdk_mem_all_zero(newblob->active.extent_pages,
				 newblob->active.num_extent_pages * sizeof(*newblob->active.extent_pages)));

	_spdk_blob_freeze_io(origblob, _spdk_bs_snapshot_freeze_cpl, ctx);
}

static void
_spdk_bs_snapshot_newblob_create_cpl(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *origblob = ctx->original.blob;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	ctx->new.id = blobid;
	ctx->cpl.u.blobid.blobid = blobid;

	spdk_bs_open_blob(origblob->bs, ctx->new.id, _spdk_bs_snapshot_newblob_open_cpl, ctx);
}


static void
_spdk_bs_xattr_snapshot(void *arg, const char *name,
			const void **value, size_t *value_len)
{
	assert(strncmp(name, SNAPSHOT_IN_PROGRESS, sizeof(SNAPSHOT_IN_PROGRESS)) == 0);

	struct spdk_blob *blob = (struct spdk_blob *)arg;
	*value = &blob->id;
	*value_len = sizeof(blob->id);
}

static void
_spdk_bs_snapshot_origblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	char *xattrs_names[] = { SNAPSHOT_IN_PROGRESS };

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;

	if (_blob->data_ro || _blob->md_ro) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot create snapshot from read only blob with id %lu\n",
			      _blob->id);
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot create snapshot - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	spdk_blob_opts_init(&opts);
	_spdk_blob_xattrs_init(&internal_xattrs);

	/* Change the size of new blob to the same as in original blob,
	 * but do not allocate clusters */
	opts.thin_provision = true;
	opts.num_clusters = spdk_blob_get_num_clusters(_blob);
	opts.use_extent_table = _blob->use_extent_table;

	/* If there are any xattrs specified for snapshot, set them now */
	if (ctx->xattrs) {
		memcpy(&opts.xattrs, ctx->xattrs, sizeof(*ctx->xattrs));
	}
	/* Set internal xattr SNAPSHOT_IN_PROGRESS */
	internal_xattrs.count = 1;
	internal_xattrs.ctx = _blob;
	internal_xattrs.names = xattrs_names;
	internal_xattrs.get_value = _spdk_bs_xattr_snapshot;

	_spdk_bs_create_blob(_blob->bs, &opts, &internal_xattrs,
			     _spdk_bs_snapshot_newblob_create_cpl, ctx);
}

void spdk_bs_create_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid,
			     const struct spdk_blob_xattr_opts *snapshot_xattrs,
			     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOMEM);
		return;
	}
	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	ctx->cpl.u.blobid.cb_fn = cb_fn;
	ctx->cpl.u.blobid.cb_arg = cb_arg;
	ctx->cpl.u.blobid.blobid = SPDK_BLOBID_INVALID;
	ctx->bserrno = 0;
	ctx->frozen = false;
	ctx->original.id = blobid;
	ctx->xattrs = snapshot_xattrs;

	spdk_bs_open_blob(bs, ctx->original.id, _spdk_bs_snapshot_origblob_open_cpl, ctx);
}
/* END spdk_bs_create_snapshot */

/* START spdk_bs_create_clone */

static void
_spdk_bs_xattr_clone(void *arg, const char *name,
		     const void **value, size_t *value_len)
{
	assert(strncmp(name, BLOB_SNAPSHOT, sizeof(BLOB_SNAPSHOT)) == 0);

	struct spdk_blob *blob = (struct spdk_blob *)arg;
	*value = &blob->id;
	*value_len = sizeof(blob->id);
}

static void
_spdk_bs_clone_newblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *clone = _blob;

	ctx->new.blob = clone;
	_spdk_bs_blob_list_add(clone);

	spdk_blob_close(clone, _spdk_bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
_spdk_bs_clone_newblob_create_cpl(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;

	ctx->cpl.u.blobid.blobid = blobid;
	spdk_bs_open_blob(ctx->original.blob->bs, blobid, _spdk_bs_clone_newblob_open_cpl, ctx);
}

static void
_spdk_bs_clone_origblob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx	*ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob_opts		opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	char *xattr_names[] = { BLOB_SNAPSHOT };

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;

	if (!_blob->data_ro || !_blob->md_ro) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Clone not from read-only blob\n");
		ctx->bserrno = -EINVAL;
		spdk_blob_close(_blob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot create clone - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	spdk_blob_opts_init(&opts);
	_spdk_blob_xattrs_init(&internal_xattrs);

	opts.thin_provision = true;
	opts.num_clusters = spdk_blob_get_num_clusters(_blob);
	opts.use_extent_table = _blob->use_extent_table;
	if (ctx->xattrs) {
		memcpy(&opts.xattrs, ctx->xattrs, sizeof(*ctx->xattrs));
	}

	/* Set internal xattr BLOB_SNAPSHOT */
	internal_xattrs.count = 1;
	internal_xattrs.ctx = _blob;
	internal_xattrs.names = xattr_names;
	internal_xattrs.get_value = _spdk_bs_xattr_clone;

	_spdk_bs_create_blob(_blob->bs, &opts, &internal_xattrs,
			     _spdk_bs_clone_newblob_create_cpl, ctx);
}

void spdk_bs_create_clone(struct spdk_blob_store *bs, spdk_blob_id blobid,
			  const struct spdk_blob_xattr_opts *clone_xattrs,
			  spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx	*ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		cb_fn(cb_arg, SPDK_BLOBID_INVALID, -ENOMEM);
		return;
	}

	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	ctx->cpl.u.blobid.cb_fn = cb_fn;
	ctx->cpl.u.blobid.cb_arg = cb_arg;
	ctx->cpl.u.blobid.blobid = SPDK_BLOBID_INVALID;
	ctx->bserrno = 0;
	ctx->xattrs = clone_xattrs;
	ctx->original.id = blobid;

	spdk_bs_open_blob(bs, ctx->original.id, _spdk_bs_clone_origblob_open_cpl, ctx);
}

/* END spdk_bs_create_clone */

/* START spdk_bs_inflate_blob */

static void
_spdk_bs_inflate_blob_set_parent_cpl(void *cb_arg, struct spdk_blob *_parent, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *_blob = ctx->original.blob;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	assert(_parent != NULL);

	_spdk_bs_blob_list_remove(_blob);
	_blob->parent_id = _parent->id;
	_spdk_blob_set_xattr(_blob, BLOB_SNAPSHOT, &_blob->parent_id,
			     sizeof(spdk_blob_id), true);

	_blob->back_bs_dev->destroy(_blob->back_bs_dev);
	_blob->back_bs_dev = bs_create_blob_bs_dev(_parent);
	_spdk_bs_blob_list_add(_blob);

	spdk_blob_sync_md(_blob, _spdk_bs_clone_snapshot_origblob_cleanup, ctx);
}

static void
_spdk_bs_inflate_blob_done(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *_blob = ctx->original.blob;
	struct spdk_blob *_parent;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	if (ctx->allocate_all) {
		/* remove thin provisioning */
		_spdk_bs_blob_list_remove(_blob);
		_spdk_blob_remove_xattr(_blob, BLOB_SNAPSHOT, true);
		_blob->invalid_flags = _blob->invalid_flags & ~SPDK_BLOB_THIN_PROV;
		_blob->back_bs_dev->destroy(_blob->back_bs_dev);
		_blob->back_bs_dev = NULL;
		_blob->parent_id = SPDK_BLOBID_INVALID;
	} else {
		_parent = ((struct spdk_blob_bs_dev *)(_blob->back_bs_dev))->blob;
		if (_parent->parent_id != SPDK_BLOBID_INVALID) {
			/* We must change the parent of the inflated blob */
			spdk_bs_open_blob(_blob->bs, _parent->parent_id,
					  _spdk_bs_inflate_blob_set_parent_cpl, ctx);
			return;
		}

		_spdk_bs_blob_list_remove(_blob);
		_spdk_blob_remove_xattr(_blob, BLOB_SNAPSHOT, true);
		_blob->parent_id = SPDK_BLOBID_INVALID;
		_blob->back_bs_dev->destroy(_blob->back_bs_dev);
		_blob->back_bs_dev = bs_create_zeroes_dev();
	}

	_blob->state = SPDK_BLOB_STATE_DIRTY;
	spdk_blob_sync_md(_blob, _spdk_bs_clone_snapshot_origblob_cleanup, ctx);
}

/* Check if cluster needs allocation */
static inline bool
_spdk_bs_cluster_needs_allocation(struct spdk_blob *blob, uint64_t cluster, bool allocate_all)
{
	struct spdk_blob_bs_dev *b;

	assert(blob != NULL);

	if (blob->active.clusters[cluster] != 0) {
		/* Cluster is already allocated */
		return false;
	}

	if (blob->parent_id == SPDK_BLOBID_INVALID) {
		/* Blob have no parent blob */
		return allocate_all;
	}

	b = (struct spdk_blob_bs_dev *)blob->back_bs_dev;
	return (allocate_all || b->blob->active.clusters[cluster] != 0);
}

static void
_spdk_bs_inflate_blob_touch_next(void *cb_arg, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	struct spdk_blob *_blob = ctx->original.blob;
	uint64_t offset;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, bserrno);
		return;
	}

	for (; ctx->cluster < _blob->active.num_clusters; ctx->cluster++) {
		if (_spdk_bs_cluster_needs_allocation(_blob, ctx->cluster, ctx->allocate_all)) {
			break;
		}
	}

	if (ctx->cluster < _blob->active.num_clusters) {
		offset = _spdk_bs_cluster_to_lba(_blob->bs, ctx->cluster);

		/* We may safely increment a cluster before write */
		ctx->cluster++;

		/* Use zero length write to touch a cluster */
		spdk_blob_io_write(_blob, ctx->channel, NULL, offset, 0,
				   _spdk_bs_inflate_blob_touch_next, ctx);
	} else {
		_spdk_bs_inflate_blob_done(cb_arg, bserrno);
	}
}

static void
_spdk_bs_inflate_blob_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_clone_snapshot_ctx *ctx = (struct spdk_clone_snapshot_ctx *)cb_arg;
	uint64_t lfc; /* lowest free cluster */
	uint64_t i;

	if (bserrno != 0) {
		_spdk_bs_clone_snapshot_cleanup_finish(ctx, bserrno);
		return;
	}

	ctx->original.blob = _blob;

	if (_blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot inflate blob - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(_blob, _spdk_bs_clone_snapshot_cleanup_finish, ctx);
		return;
	}

	_blob->locked_operation_in_progress = true;

	if (!ctx->allocate_all && _blob->parent_id == SPDK_BLOBID_INVALID) {
		/* This blob have no parent, so we cannot decouple it. */
		SPDK_ERRLOG("Cannot decouple parent of blob with no parent.\n");
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, -EINVAL);
		return;
	}

	if (spdk_blob_is_thin_provisioned(_blob) == false) {
		/* This is not thin provisioned blob. No need to inflate. */
		_spdk_bs_clone_snapshot_origblob_cleanup(ctx, 0);
		return;
	}

	/* Do two passes - one to verify that we can obtain enough clusters
	 * and another to actually claim them.
	 */
	lfc = 0;
	for (i = 0; i < _blob->active.num_clusters; i++) {
		if (_spdk_bs_cluster_needs_allocation(_blob, i, ctx->allocate_all)) {
			lfc = spdk_bit_array_find_first_clear(_blob->bs->used_clusters, lfc);
			if (lfc == UINT32_MAX) {
				/* No more free clusters. Cannot satisfy the request */
				_spdk_bs_clone_snapshot_origblob_cleanup(ctx, -ENOSPC);
				return;
			}
			lfc++;
		}
	}

	ctx->cluster = 0;
	_spdk_bs_inflate_blob_touch_next(ctx, 0);
}

static void
_spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
		      spdk_blob_id blobid, bool allocate_all, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_clone_snapshot_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	ctx->cpl.u.bs_basic.cb_fn = cb_fn;
	ctx->cpl.u.bs_basic.cb_arg = cb_arg;
	ctx->bserrno = 0;
	ctx->original.id = blobid;
	ctx->channel = channel;
	ctx->allocate_all = allocate_all;

	spdk_bs_open_blob(bs, ctx->original.id, _spdk_bs_inflate_blob_open_cpl, ctx);
}

void
spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
		     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_bs_inflate_blob(bs, channel, blobid, true, cb_fn, cb_arg);
}

void
spdk_bs_blob_decouple_parent(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_bs_inflate_blob(bs, channel, blobid, false, cb_fn, cb_arg);
}
/* END spdk_bs_inflate_blob */

/* START spdk_blob_resize */
struct spdk_bs_resize_ctx {
	spdk_blob_op_complete cb_fn;
	void *cb_arg;
	struct spdk_blob *blob;
	uint64_t sz;
	int rc;
};

static void
_spdk_bs_resize_unfreeze_cpl(void *cb_arg, int rc)
{
	struct spdk_bs_resize_ctx *ctx = (struct spdk_bs_resize_ctx *)cb_arg;

	if (rc != 0) {
		SPDK_ERRLOG("Unfreeze failed, rc=%d\n", rc);
	}

	if (ctx->rc != 0) {
		SPDK_ERRLOG("Unfreeze failed, ctx->rc=%d\n", ctx->rc);
		rc = ctx->rc;
	}

	ctx->blob->locked_operation_in_progress = false;

	ctx->cb_fn(ctx->cb_arg, rc);
	free(ctx);
}

static void
_spdk_bs_resize_freeze_cpl(void *cb_arg, int rc)
{
	struct spdk_bs_resize_ctx *ctx = (struct spdk_bs_resize_ctx *)cb_arg;

	if (rc != 0) {
		ctx->blob->locked_operation_in_progress = false;
		ctx->cb_fn(ctx->cb_arg, rc);
		free(ctx);
		return;
	}

	ctx->rc = _spdk_blob_resize(ctx->blob, ctx->sz);

	_spdk_blob_unfreeze_io(ctx->blob, _spdk_bs_resize_unfreeze_cpl, ctx);
}

void
spdk_blob_resize(struct spdk_blob *blob, uint64_t sz, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_resize_ctx *ctx;

	_spdk_blob_verify_md_op(blob);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Resizing blob %lu to %lu clusters\n", blob->id, sz);

	if (blob->md_ro) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (sz == blob->active.num_clusters) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (blob->locked_operation_in_progress) {
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob->locked_operation_in_progress = true;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->blob = blob;
	ctx->sz = sz;
	_spdk_blob_freeze_io(blob, _spdk_bs_resize_freeze_cpl, ctx);
}

/* END spdk_blob_resize */


/* START spdk_bs_delete_blob */

static void
_spdk_bs_delete_close_cpl(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_delete_persist_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno != 0) {
		/*
		 * We already removed this blob from the blobstore tailq, so
		 *  we need to free it here since this is the last reference
		 *  to it.
		 */
		_spdk_blob_free(blob);
		_spdk_bs_delete_close_cpl(seq, bserrno);
		return;
	}

	/*
	 * This will immediately decrement the ref_count and call
	 *  the completion routine since the metadata state is clean.
	 *  By calling spdk_blob_close, we reduce the number of call
	 *  points into code that touches the blob->open_ref count
	 *  and the blobstore's blob list.
	 */
	spdk_blob_close(blob, _spdk_bs_delete_close_cpl, seq);
}

struct delete_snapshot_ctx {
	struct spdk_blob_list *parent_snapshot_entry;
	struct spdk_blob *snapshot;
	bool snapshot_md_ro;
	struct spdk_blob *clone;
	bool clone_md_ro;
	spdk_blob_op_with_handle_complete cb_fn;
	void *cb_arg;
	int bserrno;
};

static void
_spdk_delete_blob_cleanup_finish(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Snapshot cleanup error %d\n", bserrno);
	}

	assert(ctx != NULL);

	if (bserrno != 0 && ctx->bserrno == 0) {
		ctx->bserrno = bserrno;
	}

	ctx->cb_fn(ctx->cb_arg, ctx->snapshot, ctx->bserrno);
	free(ctx);
}

static void
_spdk_delete_snapshot_cleanup_snapshot(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		ctx->bserrno = bserrno;
		SPDK_ERRLOG("Clone cleanup error %d\n", bserrno);
	}

	if (ctx->bserrno != 0) {
		assert(_spdk_blob_lookup(ctx->snapshot->bs, ctx->snapshot->id) == NULL);
		TAILQ_INSERT_HEAD(&ctx->snapshot->bs->blobs, ctx->snapshot, link);
	}

	ctx->snapshot->locked_operation_in_progress = false;
	ctx->snapshot->md_ro = ctx->snapshot_md_ro;

	spdk_blob_close(ctx->snapshot, _spdk_delete_blob_cleanup_finish, ctx);
}

static void
_spdk_delete_snapshot_cleanup_clone(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	ctx->clone->locked_operation_in_progress = false;
	ctx->clone->md_ro = ctx->clone_md_ro;

	spdk_blob_close(ctx->clone, _spdk_delete_snapshot_cleanup_snapshot, ctx);
}

static void
_spdk_delete_snapshot_unfreeze_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		ctx->bserrno = bserrno;
		_spdk_delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	ctx->clone->locked_operation_in_progress = false;
	spdk_blob_close(ctx->clone, _spdk_delete_blob_cleanup_finish, ctx);
}

static void
_spdk_delete_snapshot_sync_snapshot_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	struct spdk_blob_list *parent_snapshot_entry = NULL;
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob_list *snapshot_clone_entry = NULL;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD on blob\n");
		ctx->bserrno = bserrno;
		_spdk_delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Get snapshot entry for the snapshot we want to remove */
	snapshot_entry = _spdk_bs_get_snapshot_entry(ctx->snapshot->bs, ctx->snapshot->id);

	assert(snapshot_entry != NULL);

	/* Remove clone entry in this snapshot (at this point there can be only one clone) */
	clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
	assert(clone_entry != NULL);
	TAILQ_REMOVE(&snapshot_entry->clones, clone_entry, link);
	snapshot_entry->clone_count--;
	assert(TAILQ_EMPTY(&snapshot_entry->clones));

	if (ctx->snapshot->parent_id != SPDK_BLOBID_INVALID) {
		/* This snapshot is at the same time a clone of another snapshot - we need to
		 * update parent snapshot (remove current clone, add new one inherited from
		 * the snapshot that is being removed) */

		/* Get snapshot entry for parent snapshot and clone entry within that snapshot for
		 * snapshot that we are removing */
		_spdk_blob_get_snapshot_and_clone_entries(ctx->snapshot, &parent_snapshot_entry,
				&snapshot_clone_entry);

		/* Switch clone entry in parent snapshot */
		TAILQ_INSERT_TAIL(&parent_snapshot_entry->clones, clone_entry, link);
		TAILQ_REMOVE(&parent_snapshot_entry->clones, snapshot_clone_entry, link);
		free(snapshot_clone_entry);
	} else {
		/* No parent snapshot - just remove clone entry */
		free(clone_entry);
	}

	/* Restore md_ro flags */
	ctx->clone->md_ro = ctx->clone_md_ro;
	ctx->snapshot->md_ro = ctx->snapshot_md_ro;

	_spdk_blob_unfreeze_io(ctx->clone, _spdk_delete_snapshot_unfreeze_cpl, ctx);
}

static void
_spdk_delete_snapshot_sync_clone_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	uint64_t i;

	ctx->snapshot->md_ro = false;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD on clone\n");
		ctx->bserrno = bserrno;

		/* Restore snapshot to previous state */
		bserrno = _spdk_blob_remove_xattr(ctx->snapshot, SNAPSHOT_PENDING_REMOVAL, true);
		if (bserrno != 0) {
			_spdk_delete_snapshot_cleanup_clone(ctx, bserrno);
			return;
		}

		spdk_blob_sync_md(ctx->snapshot, _spdk_delete_snapshot_cleanup_clone, ctx);
		return;
	}

	/* Clear cluster map entries for snapshot */
	for (i = 0; i < ctx->snapshot->active.num_clusters && i < ctx->clone->active.num_clusters; i++) {
		if (ctx->clone->active.clusters[i] == ctx->snapshot->active.clusters[i]) {
			ctx->snapshot->active.clusters[i] = 0;
		}
	}
	for (i = 0; i < ctx->snapshot->active.num_extent_pages &&
	     i < ctx->clone->active.num_extent_pages; i++) {
		if (ctx->clone->active.extent_pages[i] == ctx->snapshot->active.extent_pages[i]) {
			ctx->snapshot->active.extent_pages[i] = 0;
		}
	}

	_spdk_blob_set_thin_provision(ctx->snapshot);
	ctx->snapshot->state = SPDK_BLOB_STATE_DIRTY;

	if (ctx->parent_snapshot_entry != NULL) {
		ctx->snapshot->back_bs_dev = NULL;
	}

	spdk_blob_sync_md(ctx->snapshot, _spdk_delete_snapshot_sync_snapshot_cpl, ctx);
}

static void
_spdk_delete_snapshot_sync_snapshot_xattr_cpl(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;
	uint64_t i;

	/* Temporarily override md_ro flag for clone for MD modification */
	ctx->clone_md_ro = ctx->clone->md_ro;
	ctx->clone->md_ro = false;

	if (bserrno) {
		SPDK_ERRLOG("Failed to sync MD with xattr on blob\n");
		ctx->bserrno = bserrno;
		_spdk_delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Copy snapshot map to clone map (only unallocated clusters in clone) */
	for (i = 0; i < ctx->snapshot->active.num_clusters && i < ctx->clone->active.num_clusters; i++) {
		if (ctx->clone->active.clusters[i] == 0) {
			ctx->clone->active.clusters[i] = ctx->snapshot->active.clusters[i];
		}
	}
	for (i = 0; i < ctx->snapshot->active.num_extent_pages &&
	     i < ctx->clone->active.num_extent_pages; i++) {
		if (ctx->clone->active.extent_pages[i] == 0) {
			ctx->clone->active.extent_pages[i] = ctx->snapshot->active.extent_pages[i];
		}
	}

	/* Delete old backing bs_dev from clone (related to snapshot that will be removed) */
	ctx->clone->back_bs_dev->destroy(ctx->clone->back_bs_dev);

	/* Set/remove snapshot xattr and switch parent ID and backing bs_dev on clone... */
	if (ctx->parent_snapshot_entry != NULL) {
		/* ...to parent snapshot */
		ctx->clone->parent_id = ctx->parent_snapshot_entry->id;
		ctx->clone->back_bs_dev = ctx->snapshot->back_bs_dev;
		_spdk_blob_set_xattr(ctx->clone, BLOB_SNAPSHOT, &ctx->parent_snapshot_entry->id,
				     sizeof(spdk_blob_id),
				     true);
	} else {
		/* ...to blobid invalid and zeroes dev */
		ctx->clone->parent_id = SPDK_BLOBID_INVALID;
		ctx->clone->back_bs_dev = bs_create_zeroes_dev();
		_spdk_blob_remove_xattr(ctx->clone, BLOB_SNAPSHOT, true);
	}

	spdk_blob_sync_md(ctx->clone, _spdk_delete_snapshot_sync_clone_cpl, ctx);
}

static void
_spdk_delete_snapshot_freeze_io_cb(void *cb_arg, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		SPDK_ERRLOG("Failed to freeze I/O on clone\n");
		ctx->bserrno = bserrno;
		_spdk_delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	/* Temporarily override md_ro flag for snapshot for MD modification */
	ctx->snapshot_md_ro = ctx->snapshot->md_ro;
	ctx->snapshot->md_ro = false;

	/* Mark blob as pending for removal for power failure safety, use clone id for recovery */
	ctx->bserrno = _spdk_blob_set_xattr(ctx->snapshot, SNAPSHOT_PENDING_REMOVAL, &ctx->clone->id,
					    sizeof(spdk_blob_id), true);
	if (ctx->bserrno != 0) {
		_spdk_delete_snapshot_cleanup_clone(ctx, 0);
		return;
	}

	spdk_blob_sync_md(ctx->snapshot, _spdk_delete_snapshot_sync_snapshot_xattr_cpl, ctx);
}

static void
_spdk_delete_snapshot_open_clone_cb(void *cb_arg, struct spdk_blob *clone, int bserrno)
{
	struct delete_snapshot_ctx *ctx = cb_arg;

	if (bserrno) {
		SPDK_ERRLOG("Failed to open clone\n");
		ctx->bserrno = bserrno;
		_spdk_delete_snapshot_cleanup_snapshot(ctx, 0);
		return;
	}

	ctx->clone = clone;

	if (clone->locked_operation_in_progress) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot remove blob - another operation in progress on its clone\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(ctx->clone, _spdk_delete_snapshot_cleanup_snapshot, ctx);
		return;
	}

	clone->locked_operation_in_progress = true;

	_spdk_blob_freeze_io(clone, _spdk_delete_snapshot_freeze_io_cb, ctx);
}

static void
_spdk_update_clone_on_snapshot_deletion(struct spdk_blob *snapshot, struct delete_snapshot_ctx *ctx)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob_list *snapshot_clone_entry = NULL;

	/* Get snapshot entry for the snapshot we want to remove */
	snapshot_entry = _spdk_bs_get_snapshot_entry(snapshot->bs, snapshot->id);

	assert(snapshot_entry != NULL);

	/* Get clone of the snapshot (at this point there can be only one clone) */
	clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
	assert(snapshot_entry->clone_count == 1);
	assert(clone_entry != NULL);

	/* Get snapshot entry for parent snapshot and clone entry within that snapshot for
	 * snapshot that we are removing */
	_spdk_blob_get_snapshot_and_clone_entries(snapshot, &ctx->parent_snapshot_entry,
			&snapshot_clone_entry);

	spdk_bs_open_blob(snapshot->bs, clone_entry->id, _spdk_delete_snapshot_open_clone_cb, ctx);
}

static void
_spdk_bs_delete_blob_finish(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	struct spdk_blob_list *snapshot_entry = NULL;
	uint32_t page_num;

	if (bserrno) {
		SPDK_ERRLOG("Failed to remove blob\n");
		bs_sequence_finish(seq, bserrno);
		return;
	}

	/* Remove snapshot from the list */
	snapshot_entry = _spdk_bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry != NULL) {
		TAILQ_REMOVE(&blob->bs->snapshots, snapshot_entry, link);
		free(snapshot_entry);
	}

	page_num = _spdk_bs_blobid_to_page(blob->id);
	spdk_bit_array_clear(blob->bs->used_blobids, page_num);
	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->active.num_pages = 0;
	_spdk_blob_resize(blob, 0);

	_spdk_blob_persist(seq, blob, _spdk_bs_delete_persist_cpl, blob);
}

static int
_spdk_bs_is_blob_deletable(struct spdk_blob *blob, bool *update_clone)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;
	struct spdk_blob *clone = NULL;
	bool has_one_clone = false;

	/* Check if this is a snapshot with clones */
	snapshot_entry = _spdk_bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry != NULL) {
		if (snapshot_entry->clone_count > 1) {
			SPDK_ERRLOG("Cannot remove snapshot with more than one clone\n");
			return -EBUSY;
		} else if (snapshot_entry->clone_count == 1) {
			has_one_clone = true;
		}
	}

	/* Check if someone has this blob open (besides this delete context):
	 * - open_ref = 1 - only this context opened blob, so it is ok to remove it
	 * - open_ref <= 2 && has_one_clone = true - clone is holding snapshot
	 *	and that is ok, because we will update it accordingly */
	if (blob->open_ref <= 2 && has_one_clone) {
		clone_entry = TAILQ_FIRST(&snapshot_entry->clones);
		assert(clone_entry != NULL);
		clone = _spdk_blob_lookup(blob->bs, clone_entry->id);

		if (blob->open_ref == 2 && clone == NULL) {
			/* Clone is closed and someone else opened this blob */
			SPDK_ERRLOG("Cannot remove snapshot because it is open\n");
			return -EBUSY;
		}

		*update_clone = true;
		return 0;
	}

	if (blob->open_ref > 1) {
		SPDK_ERRLOG("Cannot remove snapshot because it is open\n");
		return -EBUSY;
	}

	assert(has_one_clone == false);
	*update_clone = false;
	return 0;
}

static void
_spdk_bs_delete_enomem_close_cpl(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	bs_sequence_finish(seq, -ENOMEM);
}

static void
_spdk_bs_delete_open_cpl(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	struct delete_snapshot_ctx *ctx;
	bool update_clone = false;

	if (bserrno != 0) {
		bs_sequence_finish(seq, bserrno);
		return;
	}

	_spdk_blob_verify_md_op(blob);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_blob_close(blob, _spdk_bs_delete_enomem_close_cpl, seq);
		return;
	}

	ctx->snapshot = blob;
	ctx->cb_fn = _spdk_bs_delete_blob_finish;
	ctx->cb_arg = seq;

	/* Check if blob can be removed and if it is a snapshot with clone on top of it */
	ctx->bserrno = _spdk_bs_is_blob_deletable(blob, &update_clone);
	if (ctx->bserrno) {
		spdk_blob_close(blob, _spdk_delete_blob_cleanup_finish, ctx);
		return;
	}

	if (blob->locked_operation_in_progress) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Cannot remove blob - another operation in progress\n");
		ctx->bserrno = -EBUSY;
		spdk_blob_close(blob, _spdk_delete_blob_cleanup_finish, ctx);
		return;
	}

	blob->locked_operation_in_progress = true;

	/*
	 * Remove the blob from the blob_store list now, to ensure it does not
	 *  get returned after this point by _spdk_blob_lookup().
	 */
	TAILQ_REMOVE(&blob->bs->blobs, blob, link);

	if (update_clone) {
		/* This blob is a snapshot with active clone - update clone first */
		_spdk_update_clone_on_snapshot_deletion(blob, ctx);
	} else {
		/* This blob does not have any clones - just remove it */
		_spdk_bs_blob_list_remove(blob);
		_spdk_bs_delete_blob_finish(seq, blob, 0);
		free(ctx);
	}
}

void
spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Deleting blob %lu\n", blobid);

	assert(spdk_get_thread() == bs->md_thread);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	spdk_bs_open_blob(bs, blobid, _spdk_bs_delete_open_cpl, seq);
}

/* END spdk_bs_delete_blob */

/* START spdk_bs_open_blob */

static void
_spdk_bs_open_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno != 0) {
		_spdk_blob_free(blob);
		seq->cpl.u.blob_handle.blob = NULL;
		bs_sequence_finish(seq, bserrno);
		return;
	}

	blob->open_ref++;

	TAILQ_INSERT_HEAD(&blob->bs->blobs, blob, link);

	bs_sequence_finish(seq, bserrno);
}

static void _spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			       struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob		*blob;
	struct spdk_bs_cpl		cpl;
	struct spdk_blob_open_opts	opts_default;
	spdk_bs_sequence_t		*seq;
	uint32_t			page_num;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Opening blob %lu\n", blobid);
	assert(spdk_get_thread() == bs->md_thread);

	page_num = _spdk_bs_blobid_to_page(blobid);
	if (spdk_bit_array_get(bs->used_blobids, page_num) == false) {
		/* Invalid blobid */
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	blob = _spdk_blob_lookup(bs, blobid);
	if (blob) {
		blob->open_ref++;
		cb_fn(cb_arg, blob, 0);
		return;
	}

	blob = _spdk_blob_alloc(bs, blobid);
	if (!blob) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	if (!opts) {
		spdk_blob_open_opts_init(&opts_default);
		opts = &opts_default;
	}

	blob->clear_method = opts->clear_method;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_HANDLE;
	cpl.u.blob_handle.cb_fn = cb_fn;
	cpl.u.blob_handle.cb_arg = cb_arg;
	cpl.u.blob_handle.blob = blob;

	seq = bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	_spdk_blob_load(seq, blob, _spdk_bs_open_blob_cpl, blob);
}

void spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	_spdk_bs_open_blob(bs, blobid, NULL, cb_fn, cb_arg);
}

void spdk_bs_open_blob_ext(struct spdk_blob_store *bs, spdk_blob_id blobid,
			   struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	_spdk_bs_open_blob(bs, blobid, opts, cb_fn, cb_arg);
}

/* END spdk_bs_open_blob */

/* START spdk_blob_set_read_only */
int spdk_blob_set_read_only(struct spdk_blob *blob)
{
	_spdk_blob_verify_md_op(blob);

	blob->data_ro_flags |= SPDK_BLOB_READ_ONLY;

	blob->state = SPDK_BLOB_STATE_DIRTY;
	return 0;
}
/* END spdk_blob_set_read_only */

/* START spdk_blob_sync_md */

static void
_spdk_blob_sync_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno == 0 && (blob->data_ro_flags & SPDK_BLOB_READ_ONLY)) {
		blob->data_ro = true;
		blob->md_ro = true;
	}

	bs_sequence_finish(seq, bserrno);
}

static void
_spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	_spdk_blob_persist(seq, blob, _spdk_blob_sync_md_cpl, blob);
}

void
spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_verify_md_op(blob);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Syncing blob %lu\n", blob->id);

	if (blob->md_ro) {
		assert(blob->state == SPDK_BLOB_STATE_CLEAN);
		cb_fn(cb_arg, 0);
		return;
	}

	_spdk_blob_sync_md(blob, cb_fn, cb_arg);
}

/* END spdk_blob_sync_md */

struct spdk_blob_insert_cluster_ctx {
	struct spdk_thread	*thread;
	struct spdk_blob	*blob;
	uint32_t		cluster_num;	/* cluster index in blob */
	uint32_t		cluster;	/* cluster on disk */
	uint32_t		extent_page;	/* extent page on disk */
	int			rc;
	spdk_blob_op_complete	cb_fn;
	void			*cb_arg;
};

static void
_spdk_blob_insert_cluster_msg_cpl(void *arg)
{
	struct spdk_blob_insert_cluster_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, ctx->rc);
	free(ctx);
}

static void
_spdk_blob_insert_cluster_msg_cb(void *arg, int bserrno)
{
	struct spdk_blob_insert_cluster_ctx *ctx = arg;

	ctx->rc = bserrno;
	spdk_thread_send_msg(ctx->thread, _spdk_blob_insert_cluster_msg_cpl, ctx);
}

static void
_spdk_blob_persist_extent_page_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_md_page        *page = cb_arg;

	bs_sequence_finish(seq, bserrno);
	spdk_free(page);
}

static void
_spdk_blob_insert_extent(struct spdk_blob *blob, uint32_t extent, uint64_t cluster_num,
			 spdk_blob_op_complete cb_fn, void *cb_arg)
{
	spdk_bs_sequence_t		*seq;
	struct spdk_bs_cpl		cpl;
	struct spdk_blob_md_page	*page = NULL;
	uint32_t			page_count = 0;
	int				rc;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	rc = _spdk_blob_serialize_add_page(blob, &page, &page_count, &page);
	if (rc < 0) {
		bs_sequence_finish(seq, rc);
		return;
	}

	_spdk_blob_serialize_extent_page(blob, cluster_num, page);

	page->crc = _spdk_blob_md_page_calc_crc(page);

	assert(spdk_bit_array_get(blob->bs->used_md_pages, extent) == true);

	bs_sequence_write_dev(seq, page, _spdk_bs_md_page_to_lba(blob->bs, extent),
			      _spdk_bs_byte_to_lba(blob->bs, SPDK_BS_PAGE_SIZE),
			      _spdk_blob_persist_extent_page_cpl, page);
}

static void
_spdk_blob_insert_cluster_msg(void *arg)
{
	struct spdk_blob_insert_cluster_ctx *ctx = arg;
	uint32_t *extent_page;

	ctx->rc = _spdk_blob_insert_cluster(ctx->blob, ctx->cluster_num, ctx->cluster);
	if (ctx->rc != 0) {
		spdk_thread_send_msg(ctx->thread, _spdk_blob_insert_cluster_msg_cpl, ctx);
		return;
	}

	if (ctx->blob->use_extent_table == false) {
		/* Extent table is not used, proceed with sync of md that will only use extents_rle. */
		ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
		_spdk_blob_sync_md(ctx->blob, _spdk_blob_insert_cluster_msg_cb, ctx);
		return;
	}

	extent_page = _spdk_bs_cluster_to_extent_page(ctx->blob, ctx->cluster_num);
	if (*extent_page == 0) {
		/* Extent page requires allocation.
		 * It was already claimed in the used_md_pages map and placed in ctx.
		 * Blob persist will take care of writing out new extent page on disk. */
		assert(ctx->extent_page != 0);
		assert(spdk_bit_array_get(ctx->blob->bs->used_md_pages, ctx->extent_page) == true);
		*extent_page = ctx->extent_page;
		ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
		_spdk_blob_sync_md(ctx->blob, _spdk_blob_insert_cluster_msg_cb, ctx);
	} else {
		/* It is possible for original thread to allocate extent page for
		 * different cluster in the same extent page. In such case proceed with
		 * updating the existing extent page, but release the additional one. */
		if (ctx->extent_page != 0) {
			assert(spdk_bit_array_get(ctx->blob->bs->used_md_pages, ctx->extent_page) == true);
			_spdk_bs_release_md_page(ctx->blob->bs, ctx->extent_page);
		}
		/* Extent page already allocated.
		 * Every cluster allocation, requires just an update of single extent page. */
		_spdk_blob_insert_extent(ctx->blob, *extent_page, ctx->cluster_num,
					 _spdk_blob_insert_cluster_msg_cb, ctx);
	}
}

static void
_spdk_blob_insert_cluster_on_md_thread(struct spdk_blob *blob, uint32_t cluster_num,
				       uint64_t cluster, uint32_t extent_page, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_insert_cluster_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->thread = spdk_get_thread();
	ctx->blob = blob;
	ctx->cluster_num = cluster_num;
	ctx->cluster = cluster;
	ctx->extent_page = extent_page;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(blob->bs->md_thread, _spdk_blob_insert_cluster_msg, ctx);
}

/* START spdk_blob_close */

static void
_spdk_blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	if (bserrno == 0) {
		blob->open_ref--;
		if (blob->open_ref == 0) {
			/*
			 * Blobs with active.num_pages == 0 are deleted blobs.
			 *  these blobs are removed from the blob_store list
			 *  when the deletion process starts - so don't try to
			 *  remove them again.
			 */
			if (blob->active.num_pages > 0) {
				TAILQ_REMOVE(&blob->bs->blobs, blob, link);
			}
			_spdk_blob_free(blob);
		}
	}

	bs_sequence_finish(seq, bserrno);
}

void spdk_blob_close(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	_spdk_blob_verify_md_op(blob);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Closing blob %lu\n", blob->id);

	if (blob->open_ref == 0) {
		cb_fn(cb_arg, -EBADF);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = bs_sequence_start(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Sync metadata */
	_spdk_blob_persist(seq, blob, _spdk_blob_close_cpl, blob);
}

/* END spdk_blob_close */

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	return spdk_get_io_channel(bs);
}

void spdk_bs_free_io_channel(struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

void spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
			uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_UNMAP);
}

void spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			       uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_WRITE_ZEROES);
}

void spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
			void *payload, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_WRITE);
}

void spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       void *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_READ);
}

void spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
			 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			 spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, false);
}

void spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, true);
}

struct spdk_bs_iter_ctx {
	int64_t page_num;
	struct spdk_blob_store *bs;

	spdk_blob_op_with_handle_complete cb_fn;
	void *cb_arg;
};

static void
_spdk_bs_iter_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	struct spdk_bs_iter_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;
	spdk_blob_id id;

	if (bserrno == 0) {
		ctx->cb_fn(ctx->cb_arg, _blob, bserrno);
		free(ctx);
		return;
	}

	ctx->page_num++;
	ctx->page_num = spdk_bit_array_find_first_set(bs->used_blobids, ctx->page_num);
	if (ctx->page_num >= spdk_bit_array_capacity(bs->used_blobids)) {
		ctx->cb_fn(ctx->cb_arg, NULL, -ENOENT);
		free(ctx);
		return;
	}

	id = _spdk_bs_page_to_blobid(ctx->page_num);

	spdk_bs_open_blob(bs, id, _spdk_bs_iter_cpl, ctx);
}

void
spdk_bs_iter_first(struct spdk_blob_store *bs,
		   spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->page_num = -1;
	ctx->bs = bs;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	_spdk_bs_iter_cpl(ctx, NULL, -1);
}

static void
_spdk_bs_iter_close_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_iter_ctx *ctx = cb_arg;

	_spdk_bs_iter_cpl(ctx, NULL, -1);
}

void
spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *blob,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;

	assert(blob != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->page_num = _spdk_bs_blobid_to_page(blob->id);
	ctx->bs = bs;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Close the existing blob */
	spdk_blob_close(blob, _spdk_bs_iter_close_cpl, ctx);
}

static int
_spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		     uint16_t value_len, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr	*xattr;
	size_t			desc_size;

	_spdk_blob_verify_md_op(blob);

	if (blob->md_ro) {
		return -EPERM;
	}

	desc_size = sizeof(struct spdk_blob_md_descriptor_xattr) + strlen(name) + value_len;
	if (desc_size > SPDK_BS_MAX_DESC_SIZE) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Xattr '%s' of size %ld does not fix into single page %ld\n", name,
			      desc_size, SPDK_BS_MAX_DESC_SIZE);
		return -ENOMEM;
	}

	if (internal) {
		xattrs = &blob->xattrs_internal;
		blob->invalid_flags |= SPDK_BLOB_INTERNAL_XATTR;
	} else {
		xattrs = &blob->xattrs;
	}

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			free(xattr->value);
			xattr->value_len = value_len;
			xattr->value = malloc(value_len);
			memcpy(xattr->value, value, value_len);

			blob->state = SPDK_BLOB_STATE_DIRTY;

			return 0;
		}
	}

	xattr = calloc(1, sizeof(*xattr));
	if (!xattr) {
		return -ENOMEM;
	}
	xattr->name = strdup(name);
	xattr->value_len = value_len;
	xattr->value = malloc(value_len);
	memcpy(xattr->value, value, value_len);
	TAILQ_INSERT_TAIL(xattrs, xattr, link);

	blob->state = SPDK_BLOB_STATE_DIRTY;

	return 0;
}

int
spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		    uint16_t value_len)
{
	return _spdk_blob_set_xattr(blob, name, value, value_len, false);
}

static int
_spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr	*xattr;

	_spdk_blob_verify_md_op(blob);

	if (blob->md_ro) {
		return -EPERM;
	}
	xattrs = internal ? &blob->xattrs_internal : &blob->xattrs;

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			TAILQ_REMOVE(xattrs, xattr, link);
			free(xattr->value);
			free(xattr->name);
			free(xattr);

			if (internal && TAILQ_EMPTY(&blob->xattrs_internal)) {
				blob->invalid_flags &= ~SPDK_BLOB_INTERNAL_XATTR;
			}
			blob->state = SPDK_BLOB_STATE_DIRTY;

			return 0;
		}
	}

	return -ENOENT;
}

int
spdk_blob_remove_xattr(struct spdk_blob *blob, const char *name)
{
	return _spdk_blob_remove_xattr(blob, name, false);
}

static int
_spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			   const void **value, size_t *value_len, bool internal)
{
	struct spdk_xattr	*xattr;
	struct spdk_xattr_tailq *xattrs;

	xattrs = internal ? &blob->xattrs_internal : &blob->xattrs;

	TAILQ_FOREACH(xattr, xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			*value = xattr->value;
			*value_len = xattr->value_len;
			return 0;
		}
	}
	return -ENOENT;
}

int
spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			  const void **value, size_t *value_len)
{
	_spdk_blob_verify_md_op(blob);

	return _spdk_blob_get_xattr_value(blob, name, value, value_len, false);
}

struct spdk_xattr_names {
	uint32_t	count;
	const char	*names[0];
};

static int
_spdk_blob_get_xattr_names(struct spdk_xattr_tailq *xattrs, struct spdk_xattr_names **names)
{
	struct spdk_xattr	*xattr;
	int			count = 0;

	TAILQ_FOREACH(xattr, xattrs, link) {
		count++;
	}

	*names = calloc(1, sizeof(struct spdk_xattr_names) + count * sizeof(char *));
	if (*names == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(xattr, xattrs, link) {
		(*names)->names[(*names)->count++] = xattr->name;
	}

	return 0;
}

int
spdk_blob_get_xattr_names(struct spdk_blob *blob, struct spdk_xattr_names **names)
{
	_spdk_blob_verify_md_op(blob);

	return _spdk_blob_get_xattr_names(&blob->xattrs, names);
}

uint32_t
spdk_xattr_names_get_count(struct spdk_xattr_names *names)
{
	assert(names != NULL);

	return names->count;
}

const char *
spdk_xattr_names_get_name(struct spdk_xattr_names *names, uint32_t index)
{
	if (index >= names->count) {
		return NULL;
	}

	return names->names[index];
}

void
spdk_xattr_names_free(struct spdk_xattr_names *names)
{
	free(names);
}

struct spdk_bs_type
spdk_bs_get_bstype(struct spdk_blob_store *bs)
{
	return bs->bstype;
}

void
spdk_bs_set_bstype(struct spdk_blob_store *bs, struct spdk_bs_type bstype)
{
	memcpy(&bs->bstype, &bstype, sizeof(bstype));
}

bool
spdk_blob_is_read_only(struct spdk_blob *blob)
{
	assert(blob != NULL);
	return (blob->data_ro || blob->md_ro);
}

bool
spdk_blob_is_snapshot(struct spdk_blob *blob)
{
	struct spdk_blob_list *snapshot_entry;

	assert(blob != NULL);

	snapshot_entry = _spdk_bs_get_snapshot_entry(blob->bs, blob->id);
	if (snapshot_entry == NULL) {
		return false;
	}

	return true;
}

bool
spdk_blob_is_clone(struct spdk_blob *blob)
{
	assert(blob != NULL);

	if (blob->parent_id != SPDK_BLOBID_INVALID) {
		assert(spdk_blob_is_thin_provisioned(blob));
		return true;
	}

	return false;
}

bool
spdk_blob_is_thin_provisioned(struct spdk_blob *blob)
{
	assert(blob != NULL);
	return !!(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
}

static void
_spdk_blob_update_clear_method(struct spdk_blob *blob)
{
	enum blob_clear_method stored_cm;

	assert(blob != NULL);

	/* If BLOB_CLEAR_WITH_DEFAULT was passed in, use the setting stored
	 * in metadata previously.  If something other than the default was
	 * specified, ignore stored value and used what was passed in.
	 */
	stored_cm = ((blob->md_ro_flags & SPDK_BLOB_CLEAR_METHOD) >> SPDK_BLOB_CLEAR_METHOD_SHIFT);

	if (blob->clear_method == BLOB_CLEAR_WITH_DEFAULT) {
		blob->clear_method = stored_cm;
	} else if (blob->clear_method != stored_cm) {
		SPDK_WARNLOG("Using passed in clear method 0x%x instead of stored value of 0x%x\n",
			     blob->clear_method, stored_cm);
	}
}

spdk_blob_id
spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blob_id)
{
	struct spdk_blob_list *snapshot_entry = NULL;
	struct spdk_blob_list *clone_entry = NULL;

	TAILQ_FOREACH(snapshot_entry, &bs->snapshots, link) {
		TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
			if (clone_entry->id == blob_id) {
				return snapshot_entry->id;
			}
		}
	}

	return SPDK_BLOBID_INVALID;
}

int
spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
		     size_t *count)
{
	struct spdk_blob_list *snapshot_entry, *clone_entry;
	size_t n;

	snapshot_entry = _spdk_bs_get_snapshot_entry(bs, blobid);
	if (snapshot_entry == NULL) {
		*count = 0;
		return 0;
	}

	if (ids == NULL || *count < snapshot_entry->clone_count) {
		*count = snapshot_entry->clone_count;
		return -ENOMEM;
	}
	*count = snapshot_entry->clone_count;

	n = 0;
	TAILQ_FOREACH(clone_entry, &snapshot_entry->clones, link) {
		ids[n++] = clone_entry->id;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("blob", SPDK_LOG_BLOB)
