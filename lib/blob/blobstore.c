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
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/io_channel.h"
#include "spdk/bit_array.h"
#include "spdk/likely.h"

#include "spdk_internal/log.h"

#include "blobstore.h"
#include "request.h"

static inline size_t
divide_round_up(size_t num, size_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static void
_spdk_bs_claim_cluster(struct spdk_blob_store *bs, uint32_t cluster_num)
{
	assert(cluster_num < spdk_bit_array_capacity(bs->used_clusters));
	assert(spdk_bit_array_get(bs->used_clusters, cluster_num) == false);
	assert(bs->num_free_clusters > 0);

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Claiming cluster %u\n", cluster_num);

	spdk_bit_array_set(bs->used_clusters, cluster_num);
	bs->num_free_clusters--;
}

static void
_spdk_bs_release_cluster(struct spdk_blob_store *bs, uint32_t cluster_num)
{
	assert(cluster_num < spdk_bit_array_capacity(bs->used_clusters));
	assert(spdk_bit_array_get(bs->used_clusters, cluster_num) == true);
	assert(bs->num_free_clusters < bs->total_clusters);

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Releasing cluster %u\n", cluster_num);

	spdk_bit_array_clear(bs->used_clusters, cluster_num);
	bs->num_free_clusters++;
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

	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->active.num_pages = 1;
	blob->active.pages = calloc(1, sizeof(*blob->active.pages));
	if (!blob->active.pages) {
		free(blob);
		return NULL;
	}

	blob->active.pages[0] = _spdk_bs_blobid_to_page(id);

	TAILQ_INIT(&blob->xattrs);

	return blob;
}

static void
_spdk_blob_free(struct spdk_blob *blob)
{
	struct spdk_xattr 	*xattr, *xattr_tmp;

	assert(blob != NULL);

	free(blob->active.clusters);
	free(blob->clean.clusters);
	free(blob->active.pages);
	free(blob->clean.pages);

	TAILQ_FOREACH_SAFE(xattr, &blob->xattrs, link, xattr_tmp) {
		TAILQ_REMOVE(&blob->xattrs, xattr, link);
		free(xattr->name);
		free(xattr->value);
		free(xattr);
	}

	free(blob);
}

static int
_spdk_blob_mark_clean(struct spdk_blob *blob)
{
	uint64_t *clusters = NULL;
	uint32_t *pages = NULL;

	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING ||
	       blob->state == SPDK_BLOB_STATE_SYNCING);

	if (blob->active.num_clusters) {
		assert(blob->active.clusters);
		clusters = calloc(blob->active.num_clusters, sizeof(*blob->active.clusters));
		if (!clusters) {
			return -1;
		}
		memcpy(clusters, blob->active.clusters, blob->active.num_clusters * sizeof(*clusters));
	}

	if (blob->active.num_pages) {
		assert(blob->active.pages);
		pages = calloc(blob->active.num_pages, sizeof(*blob->active.pages));
		if (!pages) {
			free(clusters);
			return -1;
		}
		memcpy(pages, blob->active.pages, blob->active.num_pages * sizeof(*pages));
	}

	free(blob->clean.clusters);
	free(blob->clean.pages);

	blob->clean.num_clusters = blob->active.num_clusters;
	blob->clean.clusters = blob->active.clusters;
	blob->clean.num_pages = blob->active.num_pages;
	blob->clean.pages = blob->active.pages;

	blob->active.clusters = clusters;
	blob->active.pages = pages;

	blob->state = SPDK_BLOB_STATE_CLEAN;

	return 0;
}

static void
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
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT) {
			struct spdk_blob_md_descriptor_extent	*desc_extent;
			unsigned int				i, j;
			unsigned int				cluster_count = blob->active.num_clusters;

			desc_extent = (struct spdk_blob_md_descriptor_extent *)desc;

			assert(desc_extent->length > 0);
			assert(desc_extent->length % sizeof(desc_extent->extents[0]) == 0);

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->extents[0]); i++) {
				for (j = 0; j < desc_extent->extents[i].length; j++) {
					assert(spdk_bit_array_get(blob->bs->used_clusters, desc_extent->extents[i].cluster_idx + j));
					cluster_count++;
				}
			}

			assert(cluster_count > 0);
			tmp = realloc(blob->active.clusters, cluster_count * sizeof(uint64_t));
			assert(tmp != NULL);
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = cluster_count;

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->extents[0]); i++) {
				for (j = 0; j < desc_extent->extents[i].length; j++) {
					blob->active.clusters[blob->active.num_clusters++] = _spdk_bs_cluster_to_lba(blob->bs,
							desc_extent->extents[i].cluster_idx + j);
				}
			}

		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			struct spdk_blob_md_descriptor_xattr	*desc_xattr;
			struct spdk_xattr 			*xattr;

			desc_xattr = (struct spdk_blob_md_descriptor_xattr *)desc;

			assert(desc_xattr->length == sizeof(desc_xattr->name_length) +
			       sizeof(desc_xattr->value_length) +
			       desc_xattr->name_length + desc_xattr->value_length);

			xattr = calloc(1, sizeof(*xattr));
			assert(xattr != NULL);

			xattr->name = malloc(desc_xattr->name_length + 1);
			assert(xattr->name);
			strncpy(xattr->name, desc_xattr->name, desc_xattr->name_length);
			xattr->name[desc_xattr->name_length] = '\0';

			xattr->value = malloc(desc_xattr->value_length);
			assert(xattr->value != NULL);
			xattr->value_len = desc_xattr->value_length;
			memcpy(xattr->value,
			       (void *)((uintptr_t)desc_xattr->name + desc_xattr->name_length),
			       desc_xattr->value_length);

			TAILQ_INSERT_TAIL(&blob->xattrs, xattr, link);
		} else {
			/* Error */
			break;
		}

		/* Advance to the next descriptor */
		cur_desc += sizeof(*desc) + desc->length;
		if (cur_desc + sizeof(*desc) > sizeof(page->descriptors)) {
			break;
		}
		desc = (struct spdk_blob_md_descriptor *)((uintptr_t)page->descriptors + cur_desc);
	}
}

static int
_spdk_blob_parse(const struct spdk_blob_md_page *pages, uint32_t page_count,
		 struct spdk_blob *blob)
{
	const struct spdk_blob_md_page *page;
	uint32_t i;

	assert(page_count > 0);
	assert(pages[0].sequence_num == 0);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);
	assert(blob->active.clusters == NULL);
	assert(blob->id == pages[0].id);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);

	for (i = 0; i < page_count; i++) {
		page = &pages[i];

		assert(page->id == blob->id);
		assert(page->sequence_num == i);

		_spdk_blob_parse_page(page, blob);
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
		*pages = spdk_dma_malloc(SPDK_BS_PAGE_SIZE,
					 SPDK_BS_PAGE_SIZE,
					 NULL);
	} else {
		assert(*pages != NULL);
		(*page_count)++;
		*pages = spdk_dma_realloc(*pages,
					  SPDK_BS_PAGE_SIZE * (*page_count),
					  SPDK_BS_PAGE_SIZE,
					  NULL);
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
			   size_t *required_sz)
{
	struct spdk_blob_md_descriptor_xattr	*desc;

	*required_sz = sizeof(struct spdk_blob_md_descriptor_xattr) +
		       strlen(xattr->name) +
		       xattr->value_len;

	if (buf_sz < *required_sz) {
		return -1;
	}

	desc = (struct spdk_blob_md_descriptor_xattr *)buf;

	desc->type = SPDK_MD_DESCRIPTOR_TYPE_XATTR;
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
_spdk_blob_serialize_extent(const struct spdk_blob *blob,
			    uint64_t start_cluster, uint64_t *next_cluster,
			    uint8_t *buf, size_t buf_sz)
{
	struct spdk_blob_md_descriptor_extent *desc;
	size_t cur_sz;
	uint64_t i, extent_idx;
	uint32_t lba, lba_per_cluster, lba_count;

	/* The buffer must have room for at least one extent */
	cur_sz = sizeof(struct spdk_blob_md_descriptor) + sizeof(desc->extents[0]);
	if (buf_sz < cur_sz) {
		*next_cluster = start_cluster;
		return;
	}

	desc = (struct spdk_blob_md_descriptor_extent *)buf;
	desc->type = SPDK_MD_DESCRIPTOR_TYPE_EXTENT;

	lba_per_cluster = _spdk_bs_cluster_to_lba(blob->bs, 1);

	lba = blob->active.clusters[start_cluster];
	lba_count = lba_per_cluster;
	extent_idx = 0;
	for (i = start_cluster + 1; i < blob->active.num_clusters; i++) {
		if ((lba + lba_count) == blob->active.clusters[i]) {
			lba_count += lba_per_cluster;
			continue;
		}
		desc->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
		desc->extents[extent_idx].length = lba_count / lba_per_cluster;
		extent_idx++;

		cur_sz += sizeof(desc->extents[extent_idx]);

		if (buf_sz < cur_sz) {
			/* If we ran out of buffer space, return */
			desc->length = sizeof(desc->extents[0]) * extent_idx;
			*next_cluster = i;
			return;
		}

		lba = blob->active.clusters[i];
		lba_count = lba_per_cluster;
	}

	desc->extents[extent_idx].cluster_idx = lba / lba_per_cluster;
	desc->extents[extent_idx].length = lba_count / lba_per_cluster;
	extent_idx++;

	desc->length = sizeof(desc->extents[0]) * extent_idx;
	*next_cluster = blob->active.num_clusters;

	return;
}

static int
_spdk_blob_serialize(const struct spdk_blob *blob, struct spdk_blob_md_page **pages,
		     uint32_t *page_count)
{
	struct spdk_blob_md_page		*cur_page;
	const struct spdk_xattr			*xattr;
	int 					rc;
	uint8_t					*buf;
	size_t					remaining_sz;
	uint64_t				last_cluster;

	assert(pages != NULL);
	assert(page_count != NULL);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_SYNCING);

	*pages = NULL;
	*page_count = 0;

	/* A blob always has at least 1 page, even if it has no descriptors */
	rc = _spdk_blob_serialize_add_page(blob, pages, page_count, &cur_page);
	if (rc < 0) {
		return rc;
	}

	buf = (uint8_t *)cur_page->descriptors;
	remaining_sz = sizeof(cur_page->descriptors);

	/* Serialize xattrs */
	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
		size_t required_sz = 0;
		rc = _spdk_blob_serialize_xattr(xattr,
						buf, remaining_sz,
						&required_sz);
		if (rc < 0) {
			/* Need to add a new page to the chain */
			rc = _spdk_blob_serialize_add_page(blob, pages, page_count,
							   &cur_page);
			if (rc < 0) {
				spdk_dma_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return rc;
			}

			buf = (uint8_t *)cur_page->descriptors;
			remaining_sz = sizeof(cur_page->descriptors);

			/* Try again */
			required_sz = 0;
			rc = _spdk_blob_serialize_xattr(xattr,
							buf, remaining_sz,
							&required_sz);

			if (rc < 0) {
				spdk_dma_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return -1;
			}
		}

		remaining_sz -= required_sz;
		buf += required_sz;
	}

	/* Serialize extents */
	last_cluster = 0;
	while (last_cluster < blob->active.num_clusters) {
		_spdk_blob_serialize_extent(blob, last_cluster, &last_cluster,
					    buf, remaining_sz);

		if (last_cluster == blob->active.num_clusters) {
			break;
		}

		rc = _spdk_blob_serialize_add_page(blob, pages, page_count,
						   &cur_page);
		if (rc < 0) {
			return rc;
		}

		buf = (uint8_t *)cur_page->descriptors;
		remaining_sz = sizeof(cur_page->descriptors);
	}

	return 0;
}

struct spdk_blob_load_ctx {
	struct spdk_blob 		*blob;

	struct spdk_blob_md_page	*pages;
	uint32_t			num_pages;

	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static void
_spdk_blob_load_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	int				rc;

	page = &ctx->pages[ctx->num_pages - 1];

	if (page->next != SPDK_INVALID_MD_PAGE) {
		uint32_t next_page = page->next;
		uint64_t next_lba = _spdk_bs_page_to_lba(blob->bs, blob->bs->md_start + next_page);


		assert(next_lba < (blob->bs->md_start + blob->bs->md_len));

		/* Read the next page */
		ctx->num_pages++;
		ctx->pages = spdk_dma_realloc(ctx->pages, (sizeof(*page) * ctx->num_pages),
					      sizeof(*page), NULL);
		if (ctx->pages == NULL) {
			ctx->cb_fn(seq, ctx->cb_arg, -ENOMEM);
			free(ctx);
			return;
		}

		spdk_bs_sequence_read(seq, &ctx->pages[ctx->num_pages - 1],
				      next_lba,
				      _spdk_bs_byte_to_lba(blob->bs, sizeof(*page)),
				      _spdk_blob_load_cpl, ctx);
		return;
	}

	/* Parse the pages */
	rc = _spdk_blob_parse(ctx->pages, ctx->num_pages, blob);

	_spdk_blob_mark_clean(blob);

	ctx->cb_fn(seq, ctx->cb_arg, rc);

	/* Free the memory */
	spdk_dma_free(ctx->pages);
	free(ctx);
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

	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_CLEAN ||
	       blob->state == SPDK_BLOB_STATE_DIRTY);

	bs = blob->bs;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}

	ctx->blob = blob;
	ctx->pages = spdk_dma_realloc(ctx->pages, SPDK_BS_PAGE_SIZE,
				      SPDK_BS_PAGE_SIZE, NULL);
	if (!ctx->pages) {
		free(ctx);
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->num_pages = 1;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	page_num = _spdk_bs_blobid_to_page(blob->id);
	lba = _spdk_bs_page_to_lba(blob->bs, bs->md_start + page_num);

	blob->state = SPDK_BLOB_STATE_LOADING;

	spdk_bs_sequence_read(seq, &ctx->pages[0], lba,
			      _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE),
			      _spdk_blob_load_cpl, ctx);
}

struct spdk_blob_persist_ctx {
	struct spdk_blob 		*blob;

	struct spdk_blob_md_page	*pages;

	uint64_t			idx;

	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static void
_spdk_blob_persist_complete(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;

	if (bserrno == 0) {
		_spdk_blob_mark_clean(blob);
	}

	/* Call user callback */
	ctx->cb_fn(seq, ctx->cb_arg, bserrno);

	/* Free the memory */
	spdk_dma_free(ctx->pages);
	free(ctx);
}

static void
_spdk_blob_persist_unmap_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	void				*tmp;
	size_t				i;

	/* Release all clusters that were truncated */
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint32_t cluster_num = _spdk_bs_lba_to_cluster(bs, blob->active.clusters[i]);

		_spdk_bs_release_cluster(bs, cluster_num);
	}

	if (blob->active.num_clusters == 0) {
		free(blob->active.clusters);
		blob->active.clusters = NULL;
		blob->active.cluster_array_size = 0;
	} else {
		tmp = realloc(blob->active.clusters, sizeof(uint64_t) * blob->active.num_clusters);
		assert(tmp != NULL);
		blob->active.clusters = tmp;
		blob->active.cluster_array_size = blob->active.num_clusters;
	}

	_spdk_blob_persist_complete(seq, ctx, bserrno);
}

static void
_spdk_blob_persist_unmap_clusters(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	spdk_bs_batch_t			*batch;
	size_t				i;
	uint64_t			lba;
	uint32_t			lba_count;

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	batch = spdk_bs_sequence_to_batch(seq, _spdk_blob_persist_unmap_clusters_cpl, ctx);

	/* Unmap all clusters that were truncated */
	lba = 0;
	lba_count = 0;
	for (i = blob->active.num_clusters; i < blob->active.cluster_array_size; i++) {
		uint64_t next_lba = blob->active.clusters[i];
		uint32_t next_lba_count = _spdk_bs_cluster_to_lba(bs, 1);

		if ((lba + lba_count) == next_lba) {
			/* This cluster is contiguous with the previous one. */
			lba_count += next_lba_count;
			continue;
		}

		/* This cluster is not contiguous with the previous one. */

		/* If a run of LBAs previously existing, send them
		 * as an unmap.
		 */
		if (lba_count > 0) {
			spdk_bs_batch_unmap(batch, lba, lba_count);
		}

		/* Start building the next batch */
		lba = next_lba;
		lba_count = next_lba_count;
	}

	/* If we ended with a contiguous set of LBAs, send the unmap now */
	if (lba_count > 0) {
		spdk_bs_batch_unmap(batch, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
}

static void
_spdk_blob_persist_unmap_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be unmapped.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		spdk_bit_array_clear(bs->used_md_pages, blob->clean.pages[i]);
	}

	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		page_num = _spdk_bs_blobid_to_page(blob->id);
		spdk_bit_array_clear(bs->used_md_pages, page_num);
	}

	/* Move on to unmapping clusters */
	_spdk_blob_persist_unmap_clusters(seq, ctx, 0);
}

static void
_spdk_blob_persist_unmap_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	spdk_bs_batch_t			*batch;
	size_t				i;

	batch = spdk_bs_sequence_to_batch(seq, _spdk_blob_persist_unmap_pages_cpl, ctx);

	lba_count = _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE);

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be unmapped.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		lba = _spdk_bs_page_to_lba(bs, bs->md_start + blob->clean.pages[i]);

		spdk_bs_batch_unmap(batch, lba, lba_count);
	}

	/* The first page will only be unmapped if this is a delete. */
	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		/* The first page in the metadata goes where the blobid indicates */
		page_num = _spdk_bs_blobid_to_page(blob->id);
		lba = _spdk_bs_page_to_lba(bs, bs->md_start + page_num);

		spdk_bs_batch_unmap(batch, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
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

	if (blob->active.num_pages == 0) {
		/* Move on to the next step */
		_spdk_blob_persist_unmap_pages(seq, ctx, 0);
		return;
	}

	lba_count = _spdk_bs_byte_to_lba(bs, sizeof(*page));

	page = &ctx->pages[0];
	/* The first page in the metadata goes where the blobid indicates */
	lba = _spdk_bs_page_to_lba(bs, bs->md_start + _spdk_bs_blobid_to_page(blob->id));

	spdk_bs_sequence_write(seq, page, lba, lba_count,
			       _spdk_blob_persist_unmap_pages, ctx);
}

static void
_spdk_blob_persist_write_page_chain(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t 			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;
	spdk_bs_batch_t			*batch;
	size_t				i;

	/* Clusters don't move around in blobs. The list shrinks or grows
	 * at the end, but no changes ever occur in the middle of the list.
	 */

	lba_count = _spdk_bs_byte_to_lba(bs, sizeof(*page));

	batch = spdk_bs_sequence_to_batch(seq, _spdk_blob_persist_write_page_root, ctx);

	/* This starts at 1. The root page is not written until
	 * all of the others are finished
	 */
	for (i = 1; i < blob->active.num_pages; i++) {
		page = &ctx->pages[i];
		assert(page->sequence_num == i);

		lba = _spdk_bs_page_to_lba(bs, bs->md_start + blob->active.pages[i]);

		spdk_bs_batch_write(batch, page, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
}

static int
_spdk_resize_blob(struct spdk_blob *blob, uint64_t sz)
{
	uint64_t	i;
	uint64_t	*tmp;
	uint64_t	lfc; /* lowest free cluster */
	struct spdk_blob_store *bs;

	bs = blob->bs;

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->active.num_clusters == sz) {
		return 0;
	}

	if (blob->active.num_clusters < blob->active.cluster_array_size) {
		/* If this blob was resized to be larger, then smaller, then
		 * larger without syncing, then the cluster array already
		 * contains spare assigned clusters we can use.
		 */
		blob->active.num_clusters = spdk_min(blob->active.cluster_array_size,
						     sz);
	}

	blob->state = SPDK_BLOB_STATE_DIRTY;

	/* Do two passes - one to verify that we can obtain enough clusters
	 * and another to actually claim them.
	 */

	lfc = 0;
	for (i = blob->active.num_clusters; i < sz; i++) {
		lfc = spdk_bit_array_find_first_clear(bs->used_clusters, lfc);
		if (lfc >= bs->total_clusters) {
			/* No more free clusters. Cannot satisfy the request */
			assert(false);
			return -1;
		}
		lfc++;
	}

	if (sz > blob->active.num_clusters) {
		/* Expand the cluster array if necessary.
		 * We only shrink the array when persisting.
		 */
		tmp = realloc(blob->active.clusters, sizeof(uint64_t) * sz);
		if (sz > 0 && tmp == NULL) {
			assert(false);
			return -1;
		}
		blob->active.clusters = tmp;
		blob->active.cluster_array_size = sz;
	}

	lfc = 0;
	for (i = blob->active.num_clusters; i < sz; i++) {
		lfc = spdk_bit_array_find_first_clear(bs->used_clusters, lfc);
		SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Claiming cluster %lu for blob %lu\n", lfc, blob->id);
		_spdk_bs_claim_cluster(bs, lfc);
		blob->active.clusters[i] = _spdk_bs_cluster_to_lba(bs, lfc);
		lfc++;
	}

	blob->active.num_clusters = sz;

	return 0;
}

/* Write a blob to disk */
static void
_spdk_blob_persist(spdk_bs_sequence_t *seq, struct spdk_blob *blob,
		   spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	struct spdk_blob_persist_ctx *ctx;
	int rc;
	uint64_t i;
	uint32_t page_num;
	struct spdk_blob_store *bs;

	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_CLEAN ||
	       blob->state == SPDK_BLOB_STATE_DIRTY);

	if (blob->state == SPDK_BLOB_STATE_CLEAN) {
		cb_fn(seq, cb_arg, 0);
		return;
	}

	bs = blob->bs;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}
	ctx->blob = blob;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	blob->state = SPDK_BLOB_STATE_SYNCING;

	if (blob->active.num_pages == 0) {
		/* This is the signal that the blob should be deleted.
		 * Immediately jump to the clean up routine. */
		assert(blob->clean.num_pages > 0);
		ctx->idx = blob->clean.num_pages - 1;
		_spdk_blob_persist_unmap_pages(seq, ctx, 0);
		return;

	}

	/* Generate the new metadata */
	rc = _spdk_blob_serialize(blob, &ctx->pages, &blob->active.num_pages);
	if (rc < 0) {
		free(ctx);
		cb_fn(seq, cb_arg, rc);
		return;
	}

	assert(blob->active.num_pages >= 1);

	/* Resize the cache of page indices */
	blob->active.pages = realloc(blob->active.pages,
				     blob->active.num_pages * sizeof(*blob->active.pages));
	if (!blob->active.pages) {
		free(ctx);
		cb_fn(seq, cb_arg, -ENOMEM);
		return;
	}

	/* Assign this metadata to pages. This requires two passes -
	 * one to verify that there are enough pages and a second
	 * to actually claim them. */
	page_num = 0;
	/* Note that this loop starts at one. The first page location is fixed by the blobid. */
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		if (page_num >= spdk_bit_array_capacity(bs->used_md_pages)) {
			spdk_dma_free(ctx->pages);
			free(ctx);
			blob->state = SPDK_BLOB_STATE_DIRTY;
			cb_fn(seq, cb_arg, -ENOMEM);
			return;
		}
		page_num++;
	}

	page_num = 0;
	blob->active.pages[0] = _spdk_bs_blobid_to_page(blob->id);
	for (i = 1; i < blob->active.num_pages; i++) {
		page_num = spdk_bit_array_find_first_clear(bs->used_md_pages, page_num);
		ctx->pages[i - 1].next = page_num;
		blob->active.pages[i] = page_num;
		spdk_bit_array_set(bs->used_md_pages, page_num);
		SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Claiming page %u for blob %lu\n", page_num, blob->id);
		page_num++;
	}

	/* Start writing the metadata from last page to first */
	ctx->idx = blob->active.num_pages - 1;
	_spdk_blob_persist_write_page_chain(seq, ctx, 0);
}

static void
_spdk_blob_request_submit_rw(struct spdk_blob *blob, struct spdk_io_channel *_channel,
			     void *payload, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg, bool read)
{
	spdk_bs_batch_t			*batch;
	struct spdk_bs_cpl		cpl;
	uint64_t			lba;
	uint32_t			lba_count;
	uint8_t				*buf;
	uint64_t			page;

	assert(blob != NULL);

	if (offset + length > blob->active.num_clusters * blob->bs->pages_per_cluster) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	batch = spdk_bs_batch_open(_channel, &cpl);
	if (!batch) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	length = _spdk_bs_page_to_lba(blob->bs, length);
	page = offset;
	buf = payload;
	while (length > 0) {
		lba = _spdk_bs_blob_page_to_lba(blob, page);
		lba_count = spdk_min(length,
				     _spdk_bs_page_to_lba(blob->bs,
						     _spdk_bs_num_pages_to_cluster_boundary(blob, page)));

		if (read) {
			spdk_bs_batch_read(batch, buf, lba, lba_count);
		} else {
			spdk_bs_batch_write(batch, buf, lba, lba_count);
		}

		length -= lba_count;
		buf += _spdk_bs_lba_to_byte(blob->bs, lba_count);
		page += _spdk_bs_lba_to_page(blob->bs, lba_count);
	}

	spdk_bs_batch_close(batch);
}

struct rw_iov_ctx {
	struct spdk_blob *blob;
	bool read;
	int iovcnt;
	struct iovec *orig_iov;
	uint64_t page_offset;
	uint64_t pages_remaining;
	uint64_t pages_done;
	struct iovec iov[0];
};

static void
_spdk_rw_iov_done(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	assert(cb_arg == NULL);
	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_rw_iov_split_next(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct rw_iov_ctx *ctx = cb_arg;
	struct iovec *iov, *orig_iov;
	int iovcnt;
	size_t orig_iovoff;
	uint64_t lba;
	uint64_t page_count, pages_to_boundary;
	uint32_t lba_count;
	uint64_t byte_count;

	if (bserrno != 0 || ctx->pages_remaining == 0) {
		free(ctx);
		spdk_bs_sequence_finish(seq, bserrno);
		return;
	}

	pages_to_boundary = _spdk_bs_num_pages_to_cluster_boundary(ctx->blob, ctx->page_offset);
	page_count = spdk_min(ctx->pages_remaining, pages_to_boundary);
	lba = _spdk_bs_blob_page_to_lba(ctx->blob, ctx->page_offset);
	lba_count = _spdk_bs_page_to_lba(ctx->blob->bs, page_count);

	/*
	 * Get index and offset into the original iov array for our current position in the I/O sequence.
	 *  byte_count will keep track of how many bytes remaining until orig_iov and orig_iovoff will
	 *  point to the current position in the I/O sequence.
	 */
	byte_count = ctx->pages_done * sizeof(struct spdk_blob_md_page);
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
	byte_count = page_count * sizeof(struct spdk_blob_md_page);
	iov = &ctx->iov[0];
	iovcnt = 0;
	while (byte_count > 0) {
		iov->iov_len = spdk_min(byte_count, orig_iov->iov_len - orig_iovoff);
		iov->iov_base = orig_iov->iov_base + orig_iovoff;
		byte_count -= iov->iov_len;
		orig_iovoff = 0;
		orig_iov++;
		iov++;
		iovcnt++;
	}

	ctx->page_offset += page_count;
	ctx->pages_done += page_count;
	ctx->pages_remaining -= page_count;
	iov = &ctx->iov[0];

	if (ctx->read) {
		spdk_bs_sequence_readv(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_split_next, ctx);
	} else {
		spdk_bs_sequence_writev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_split_next, ctx);
	}
}

static void
_spdk_blob_request_submit_rw_iov(struct spdk_blob *blob, struct spdk_io_channel *_channel,
				 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
				 spdk_blob_op_complete cb_fn, void *cb_arg, bool read)
{
	spdk_bs_sequence_t		*seq;
	struct spdk_bs_cpl		cpl;

	assert(blob != NULL);

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	if (offset + length > blob->active.num_clusters * blob->bs->pages_per_cluster) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

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
	seq = spdk_bs_sequence_start(_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	if (spdk_likely(length <= _spdk_bs_num_pages_to_cluster_boundary(blob, offset))) {
		uint64_t lba = _spdk_bs_blob_page_to_lba(blob, offset);
		uint32_t lba_count = _spdk_bs_page_to_lba(blob->bs, length);

		if (read) {
			spdk_bs_sequence_readv(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
		} else {
			spdk_bs_sequence_writev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
		}
	} else {
		struct rw_iov_ctx *ctx;

		ctx = calloc(1, sizeof(struct rw_iov_ctx) + iovcnt * sizeof(struct iovec));
		if (ctx == NULL) {
			spdk_bs_sequence_finish(seq, -ENOMEM);
			return;
		}

		ctx->blob = blob;
		ctx->read = read;
		ctx->orig_iov = iov;
		ctx->iovcnt = iovcnt;
		ctx->page_offset = offset;
		ctx->pages_remaining = length;
		ctx->pages_done = 0;

		_spdk_rw_iov_split_next(seq, ctx, 0);
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

static int
_spdk_bs_channel_create(struct spdk_blob_store *bs, struct spdk_bs_channel *channel,
			uint32_t max_ops)
{
	struct spdk_bs_dev		*dev;
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

	return 0;
}

static int
_spdk_bs_md_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_blob_store		*bs;
	struct spdk_bs_channel		*channel = ctx_buf;

	bs = SPDK_CONTAINEROF(io_device, struct spdk_blob_store, md_target);

	return _spdk_bs_channel_create(bs, channel, bs->md_target.max_md_ops);
}

static int
_spdk_bs_io_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_blob_store		*bs;
	struct spdk_bs_channel		*channel = ctx_buf;

	bs = SPDK_CONTAINEROF(io_device, struct spdk_blob_store, io_target);

	return _spdk_bs_channel_create(bs, channel, bs->io_target.max_channel_ops);
}


static void
_spdk_bs_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_bs_channel *channel = ctx_buf;

	free(channel->req_mem);
	channel->dev->destroy_channel(channel->dev, channel->dev_channel);
}

static void
_spdk_bs_dev_destroy(void *io_device)
{
	struct spdk_blob_store *bs;
	struct spdk_blob	*blob, *blob_tmp;

	bs = SPDK_CONTAINEROF(io_device, struct spdk_blob_store, md_target);
	bs->dev->destroy(bs->dev);

	TAILQ_FOREACH_SAFE(blob, &bs->blobs, link, blob_tmp) {
		TAILQ_REMOVE(&bs->blobs, blob, link);
		_spdk_blob_free(blob);
	}

	spdk_bit_array_free(&bs->used_md_pages);
	spdk_bit_array_free(&bs->used_clusters);
	free(bs);
}

static void
_spdk_bs_free(struct spdk_blob_store *bs)
{
	spdk_bs_unregister_md_thread(bs);
	spdk_io_device_unregister(&bs->io_target, NULL);
	spdk_io_device_unregister(&bs->md_target, _spdk_bs_dev_destroy);
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_MAX_CHANNEL_OPS;
}

static struct spdk_blob_store *
_spdk_bs_alloc(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts)
{
	struct spdk_blob_store	*bs;

	bs = calloc(1, sizeof(struct spdk_blob_store));
	if (!bs) {
		return NULL;
	}

	TAILQ_INIT(&bs->blobs);
	bs->dev = dev;

	/*
	 * Do not use _spdk_bs_lba_to_cluster() here since blockcnt may not be an
	 *  even multiple of the cluster size.
	 */
	bs->cluster_sz = opts->cluster_sz;
	bs->total_clusters = dev->blockcnt / (bs->cluster_sz / dev->blocklen);
	bs->pages_per_cluster = bs->cluster_sz / SPDK_BS_PAGE_SIZE;
	bs->num_free_clusters = bs->total_clusters;
	bs->used_clusters = spdk_bit_array_create(bs->total_clusters);
	if (bs->used_clusters == NULL) {
		_spdk_bs_free(bs);
		return NULL;
	}

	bs->md_target.max_md_ops = opts->max_md_ops;
	bs->io_target.max_channel_ops = opts->max_channel_ops;
	bs->super_blob = SPDK_BLOBID_INVALID;

	/* The metadata is assumed to be at least 1 page */
	bs->used_md_pages = spdk_bit_array_create(1);

	spdk_io_device_register(&bs->md_target, _spdk_bs_md_channel_create, _spdk_bs_channel_destroy,
				sizeof(struct spdk_bs_channel));
	spdk_bs_register_md_thread(bs);

	spdk_io_device_register(&bs->io_target, _spdk_bs_io_channel_create, _spdk_bs_channel_destroy,
				sizeof(struct spdk_bs_channel));

	return bs;
}

/* START spdk_bs_load */

struct spdk_bs_load_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;

	struct spdk_bs_md_mask		*mask;
};

static void
_spdk_bs_load_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t		i, j;
	int			rc;

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_cluster_mask_len * sizeof(
					     struct spdk_blob_md_page) * 8));
	/* The length of the mask must be exactly equal to the total number of clusters */
	assert(ctx->mask->length == ctx->bs->total_clusters);

	rc = spdk_bit_array_resize(&ctx->bs->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		spdk_dma_free(ctx->super);
		spdk_dma_free(ctx->mask);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	ctx->bs->num_free_clusters = ctx->bs->total_clusters;
	for (i = 0; i < ctx->mask->length / 8; i++) {
		uint8_t segment = ctx->mask->mask[i];
		for (j = 0; segment && (j < 8); j++) {
			if (segment & 1U) {
				spdk_bit_array_set(ctx->bs->used_clusters, (i * 8) + j);
				assert(ctx->bs->num_free_clusters > 0);
				ctx->bs->num_free_clusters--;
			}
			segment >>= 1U;
		}
	}

	spdk_dma_free(ctx->super);
	spdk_dma_free(ctx->mask);
	free(ctx);

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_load_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
	uint32_t		i, j;
	int			rc;

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_PAGES);
	/* The length of the mask (in bits) must not be greater than the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE *
				     8));
	/* The length of the mask must be exactly equal to the size (in pages) of the metadata region */
	assert(ctx->mask->length == ctx->super->md_len);

	rc = spdk_bit_array_resize(&ctx->bs->used_md_pages, ctx->mask->length);
	if (rc < 0) {
		spdk_dma_free(ctx->super);
		spdk_dma_free(ctx->mask);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	for (i = 0; i < ctx->mask->length / 8; i++) {
		uint8_t segment = ctx->mask->mask[i];
		for (j = 0; segment && (j < 8); j++) {
			if (segment & 1U) {
				spdk_bit_array_set(ctx->bs->used_md_pages, (i * 8) + j);
			}
			segment >>= 1U;
		}
	}
	spdk_dma_free(ctx->mask);

	/* Read the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	spdk_bs_sequence_read(seq, ctx->mask, lba, lba_count,
			      _spdk_bs_load_used_clusters_cpl, ctx);
}

static void
_spdk_bs_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;

	if (ctx->super->version != SPDK_BS_VERSION) {
		spdk_dma_free(ctx->super);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -EILSEQ);
		return;
	}

	if (memcmp(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(ctx->super->signature)) != 0) {
		spdk_dma_free(ctx->super);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -EILSEQ);
		return;
	}

	if (ctx->super->clean != 1) {
		/* TODO: ONLY CLEAN SHUTDOWN IS CURRENTLY SUPPORTED.
		 * All of the necessary data to recover is available
		 * on disk - the code just has not been written yet.
		 */
		assert(false);
		spdk_dma_free(ctx->super);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -EILSEQ);
		return;
	}
	ctx->super->clean = 0;

	/* Parse the super block */
	ctx->bs->cluster_sz = ctx->super->cluster_size;
	ctx->bs->total_clusters = ctx->bs->dev->blockcnt / (ctx->bs->cluster_sz / ctx->bs->dev->blocklen);
	ctx->bs->pages_per_cluster = ctx->bs->cluster_sz / SPDK_BS_PAGE_SIZE;
	ctx->bs->md_start = ctx->super->md_start;
	ctx->bs->md_len = ctx->super->md_len;
	ctx->bs->super_blob = ctx->super->super_blob;

	/* Read the used pages mask */
	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		_spdk_bs_free(ctx->bs);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	spdk_bs_sequence_read(seq, ctx->mask, lba, lba_count,
			      _spdk_bs_load_used_pages_cpl, ctx);
}

void
spdk_bs_load(struct spdk_bs_dev *dev,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_load_ctx *ctx;
	struct spdk_bs_opts	opts = {};

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Loading blobstore from dev %p\n", dev);

	spdk_bs_opts_init(&opts);

	bs = _spdk_bs_alloc(dev, &opts);
	if (!bs) {
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
	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* Read the super block */
	spdk_bs_sequence_read(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			      _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			      _spdk_bs_load_super_cpl, ctx);
}

/* END spdk_bs_load */

/* START spdk_bs_init */

struct spdk_bs_init_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;
};

static void
_spdk_bs_init_persist_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;

	spdk_dma_free(ctx->super);
	free(ctx);

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_init_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;

	/* Write super block */
	spdk_bs_sequence_write(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
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
	uint64_t		num_md_pages;
	uint32_t		i;
	struct spdk_bs_opts	opts = {};
	int			rc;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Initializing blobstore on dev %p\n", dev);

	if ((SPDK_BS_PAGE_SIZE % dev->blocklen) != 0) {
		SPDK_ERRLOG("unsupported dev block length of %d\n",
			    dev->blocklen);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (o) {
		opts = *o;
	} else {
		spdk_bs_opts_init(&opts);
	}

	bs = _spdk_bs_alloc(dev, &opts);
	if (!bs) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	if (opts.num_md_pages == UINT32_MAX) {
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

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	/* Allocate memory for the super block */
	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		return;
	}
	memcpy(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
	       sizeof(ctx->super->signature));
	ctx->super->version = SPDK_BS_VERSION;
	ctx->super->length = sizeof(*ctx->super);
	ctx->super->super_blob = bs->super_blob;
	ctx->super->clean = 0;
	ctx->super->cluster_size = bs->cluster_sz;

	/* Calculate how many pages the metadata consumes at the front
	 * of the disk.
	 */

	/* The super block uses 1 page */
	num_md_pages = 1;

	/* The used_md_pages mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_page_mask_start = num_md_pages;
	ctx->super->used_page_mask_len = divide_round_up(sizeof(struct spdk_bs_md_mask) +
					 divide_round_up(bs->md_len, 8),
					 SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_page_mask_len;

	/* The used_clusters mask requires 1 bit per cluster, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_cluster_mask_start = num_md_pages;
	ctx->super->used_cluster_mask_len = divide_round_up(sizeof(struct spdk_bs_md_mask) +
					    divide_round_up(bs->total_clusters, 8),
					    SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_cluster_mask_len;

	/* The metadata region size was chosen above */
	ctx->super->md_start = bs->md_start = num_md_pages;
	ctx->super->md_len = bs->md_len;
	num_md_pages += bs->md_len;

	/* Claim all of the clusters used by the metadata */
	for (i = 0; i < divide_round_up(num_md_pages, bs->pages_per_cluster); i++) {
		_spdk_bs_claim_cluster(bs, i);
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* TRIM the entire device */
	spdk_bs_sequence_unmap(seq, 0, bs->dev->blockcnt, _spdk_bs_init_trim_cpl, ctx);
}

/* END spdk_bs_init */

/* START spdk_bs_unload */

struct spdk_bs_unload_ctx {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block	*super;

	struct spdk_bs_md_mask		*mask;
};

static void
_spdk_bs_unload_write_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->super);

	spdk_bs_sequence_finish(seq, bserrno);

	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void
_spdk_bs_unload_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);

	/* Update the values in the super block */
	ctx->super->super_blob = ctx->bs->super_blob;
	ctx->super->clean = 1;

	spdk_bs_sequence_write(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
			       _spdk_bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			       _spdk_bs_unload_write_super_cpl, ctx);
}

static void
_spdk_bs_unload_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx	*ctx = cb_arg;
	uint32_t			i;
	uint64_t			lba, lba_count, mask_size;

	spdk_dma_free(ctx->mask);

	/* Write out the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_CLUSTERS;
	ctx->mask->length = ctx->bs->total_clusters;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_clusters));

	i = 0;
	while (true) {
		i = spdk_bit_array_find_first_set(ctx->bs->used_clusters, i);
		if (i > ctx->mask->length) {
			break;
		}
		ctx->mask->mask[i / 8] |= 1U << (i % 8);
		i++;
	}

	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	spdk_bs_sequence_write(seq, ctx->mask, lba, lba_count,
			       _spdk_bs_unload_write_used_clusters_cpl, ctx);
}

static void
_spdk_bs_unload_read_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx	*ctx = cb_arg;
	uint32_t			i;
	uint64_t			lba, lba_count, mask_size;

	/* Write out the used page mask */
	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_PAGES;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_md_pages));

	i = 0;
	while (true) {
		i = spdk_bit_array_find_first_set(ctx->bs->used_md_pages, i);
		if (i > ctx->mask->length) {
			break;
		}
		ctx->mask->mask[i / 8] |= 1U << (i % 8);
		i++;
	}

	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	spdk_bs_sequence_write(seq, ctx->mask, lba, lba_count,
			       _spdk_bs_unload_write_used_pages_cpl, ctx);
}

void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_unload_ctx *ctx;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Syncing blobstore\n");

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	assert(TAILQ_EMPTY(&bs->blobs));

	/* Read super block */
	spdk_bs_sequence_read(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			      _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			      _spdk_bs_unload_read_super_cpl, ctx);
}

/* END spdk_bs_unload */

void
spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	bs->super_blob = blobid;
	cb_fn(cb_arg, 0);
}

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
spdk_bs_free_cluster_count(struct spdk_blob_store *bs)
{
	return bs->num_free_clusters;
}

int spdk_bs_register_md_thread(struct spdk_blob_store *bs)
{
	bs->md_target.md_channel = spdk_get_io_channel(&bs->md_target);

	return 0;
}

int spdk_bs_unregister_md_thread(struct spdk_blob_store *bs)
{
	spdk_put_io_channel(bs->md_target.md_channel);

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

uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	assert(blob != NULL);

	return blob->active.num_clusters;
}

/* START spdk_bs_md_create_blob */

static void
_spdk_bs_md_create_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	_spdk_blob_free(blob);

	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_bs_md_create_blob(struct spdk_blob_store *bs,
			    spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_blob	*blob;
	uint32_t		page_idx;
	struct spdk_bs_cpl 	cpl;
	spdk_bs_sequence_t	*seq;
	spdk_blob_id		id;

	page_idx = spdk_bit_array_find_first_clear(bs->used_md_pages, 0);
	if (page_idx >= spdk_bit_array_capacity(bs->used_md_pages)) {
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}
	spdk_bit_array_set(bs->used_md_pages, page_idx);

	/* The blob id is a 64 bit number. The lower 32 bits are the page_idx. The upper
	 * 32 bits are not currently used. Stick a 1 there just to catch bugs where the
	 * code assumes blob id == page_idx.
	 */
	id = (1ULL << 32) | page_idx;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Creating blob with id %lu at page %u\n", id, page_idx);

	blob = _spdk_blob_alloc(bs, id);
	if (!blob) {
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	cpl.u.blobid.cb_fn = cb_fn;
	cpl.u.blobid.cb_arg = cb_arg;
	cpl.u.blobid.blobid = blob->id;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}

	_spdk_blob_persist(seq, blob, _spdk_bs_md_create_blob_cpl, blob);
}

/* END spdk_bs_md_create_blob */

/* START spdk_bs_md_resize_blob */
int
spdk_bs_md_resize_blob(struct spdk_blob *blob, uint64_t sz)
{
	int			rc;

	assert(blob != NULL);

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Resizing blob %lu to %lu clusters\n", blob->id, sz);

	if (sz == blob->active.num_clusters) {
		return 0;
	}

	rc = _spdk_resize_blob(blob, sz);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

/* END spdk_bs_md_resize_blob */


/* START spdk_bs_md_delete_blob */

static void
_spdk_bs_md_delete_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	_spdk_blob_free(blob);

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_md_delete_open_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->active.num_pages = 0;
	_spdk_resize_blob(blob, 0);

	_spdk_blob_persist(seq, blob, _spdk_bs_md_delete_blob_cpl, blob);
}

void
spdk_bs_md_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob	*blob;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t 	*seq;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Deleting blob %lu\n", blobid);

	blob = _spdk_blob_lookup(bs, blobid);
	if (blob) {
		assert(blob->open_ref > 0);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	blob = _spdk_blob_alloc(bs, blobid);
	if (!blob) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	_spdk_blob_load(seq, blob, _spdk_bs_md_delete_open_cpl, blob);
}

/* END spdk_bs_md_delete_blob */

/* START spdk_bs_md_open_blob */

static void
_spdk_bs_md_open_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;

	blob->open_ref++;

	TAILQ_INSERT_HEAD(&blob->bs->blobs, blob, link);

	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_bs_md_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
			  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob		*blob;
	struct spdk_bs_cpl		cpl;
	spdk_bs_sequence_t		*seq;
	uint32_t			page_num;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Opening blob %lu\n", blobid);

	blob = _spdk_blob_lookup(bs, blobid);
	if (blob) {
		blob->open_ref++;
		cb_fn(cb_arg, blob, 0);
		return;
	}

	page_num = _spdk_bs_blobid_to_page(blobid);
	if (spdk_bit_array_get(bs->used_md_pages, page_num) == false) {
		/* Invalid blobid */
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	blob = _spdk_blob_alloc(bs, blobid);
	if (!blob) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_HANDLE;
	cpl.u.blob_handle.cb_fn = cb_fn;
	cpl.u.blob_handle.cb_arg = cb_arg;
	cpl.u.blob_handle.blob = blob;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	_spdk_blob_load(seq, blob, _spdk_bs_md_open_blob_cpl, blob);
}

/* START spdk_bs_md_sync_blob */
static void
_spdk_blob_sync_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_bs_md_sync_blob(struct spdk_blob *blob,
			  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	assert(blob != NULL);

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Syncing blob %lu\n", blob->id);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->state == SPDK_BLOB_STATE_CLEAN) {
		cb_fn(cb_arg, 0);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(blob->bs->md_target.md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	_spdk_blob_persist(seq, blob, _spdk_blob_sync_cpl, blob);
}

/* END spdk_bs_md_sync_blob */

/* START spdk_bs_md_close_blob */

static void
_spdk_blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob **blob = cb_arg;

	if ((*blob)->open_ref == 0) {
		TAILQ_REMOVE(&(*blob)->bs->blobs, (*blob), link);
		_spdk_blob_free((*blob));
	}

	*blob = NULL;

	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_bs_md_close_blob(struct spdk_blob **b,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	struct spdk_blob	*blob;
	spdk_bs_sequence_t	*seq;

	assert(b != NULL);
	blob = *b;
	assert(blob != NULL);

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Closing blob %lu\n", blob->id);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->open_ref == 0) {
		cb_fn(cb_arg, -EBADF);
		return;
	}

	blob->open_ref--;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(blob->bs->md_target.md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	if (blob->state == SPDK_BLOB_STATE_CLEAN) {
		_spdk_blob_close_cpl(seq, b, 0);
		return;
	}

	/* Sync metadata */
	_spdk_blob_persist(seq, blob, _spdk_blob_close_cpl, b);
}

/* END spdk_bs_md_close_blob */

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	return spdk_get_io_channel(&bs->io_target);
}

void spdk_bs_free_io_channel(struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

void spdk_bs_io_flush_channel(struct spdk_io_channel *channel,
			      spdk_blob_op_complete cb_fn, void *cb_arg)
{
	/* Flush is synchronous right now */
	cb_fn(cb_arg, 0);
}

void spdk_bs_io_write_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   void *payload, uint64_t offset, uint64_t length,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_rw(blob, channel, payload, offset, length, cb_fn, cb_arg, false);
}

void spdk_bs_io_read_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  void *payload, uint64_t offset, uint64_t length,
			  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_rw(blob, channel, payload, offset, length, cb_fn, cb_arg, true);
}

void spdk_bs_io_writev_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_rw_iov(blob, channel, iov, iovcnt, offset, length, cb_fn, cb_arg, false);
}

void spdk_bs_io_readv_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
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
_spdk_bs_iter_cpl(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_iter_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;
	spdk_blob_id id;

	if (bserrno == 0) {
		ctx->cb_fn(ctx->cb_arg, blob, bserrno);
		free(ctx);
		return;
	}

	ctx->page_num++;
	ctx->page_num = spdk_bit_array_find_first_set(bs->used_md_pages, ctx->page_num);
	if (ctx->page_num >= spdk_bit_array_capacity(bs->used_md_pages)) {
		ctx->cb_fn(ctx->cb_arg, NULL, -ENOENT);
		free(ctx);
		return;
	}

	id = (1ULL << 32) | ctx->page_num;

	blob = _spdk_blob_lookup(bs, id);
	if (blob) {
		blob->open_ref++;
		ctx->cb_fn(ctx->cb_arg, blob, 0);
		free(ctx);
		return;
	}

	spdk_bs_md_open_blob(bs, id, _spdk_bs_iter_cpl, ctx);
}

void
spdk_bs_md_iter_first(struct spdk_blob_store *bs,
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
spdk_bs_md_iter_next(struct spdk_blob_store *bs, struct spdk_blob **b,
		     spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;
	struct spdk_blob	*blob;

	assert(b != NULL);
	blob = *b;
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
	spdk_bs_md_close_blob(b, _spdk_bs_iter_close_cpl, ctx);
}

int
spdk_blob_md_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		       uint16_t value_len)
{
	struct spdk_xattr 	*xattr;

	assert(blob != NULL);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
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
		return -1;
	}
	xattr->name = strdup(name);
	xattr->value_len = value_len;
	xattr->value = malloc(value_len);
	memcpy(xattr->value, value, value_len);
	TAILQ_INSERT_TAIL(&blob->xattrs, xattr, link);

	blob->state = SPDK_BLOB_STATE_DIRTY;

	return 0;
}

int
spdk_blob_md_remove_xattr(struct spdk_blob *blob, const char *name)
{
	struct spdk_xattr	*xattr;

	assert(blob != NULL);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			TAILQ_REMOVE(&blob->xattrs, xattr, link);
			free(xattr->value);
			free(xattr->name);
			free(xattr);

			blob->state = SPDK_BLOB_STATE_DIRTY;

			return 0;
		}
	}

	return -ENOENT;
}

int
spdk_bs_md_get_xattr_value(struct spdk_blob *blob, const char *name,
			   const void **value, size_t *value_len)
{
	struct spdk_xattr	*xattr;

	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
		if (!strcmp(name, xattr->name)) {
			*value = xattr->value;
			*value_len = xattr->value_len;
			return 0;
		}
	}

	return -ENOENT;
}

struct spdk_xattr_names {
	uint32_t	count;
	const char	*names[0];
};

int
spdk_bs_md_get_xattr_names(struct spdk_blob *blob,
			   struct spdk_xattr_names **names)
{
	struct spdk_xattr	*xattr;
	int			count = 0;

	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
		count++;
	}

	*names = calloc(1, sizeof(struct spdk_xattr_names) + count * sizeof(char *));
	if (*names == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(xattr, &blob->xattrs, link) {
		(*names)->names[(*names)->count++] = xattr->name;
	}

	return 0;
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

SPDK_LOG_REGISTER_TRACE_FLAG("blob", SPDK_TRACE_BLOB);
