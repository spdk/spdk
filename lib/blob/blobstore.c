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
#include "spdk/io_channel.h"
#include "spdk/bit_array.h"
#include "spdk/likely.h"

#include "spdk_internal/log.h"

#include "blobstore.h"

#define BLOB_CRC32C_INITIAL    0xffffffffUL

static int spdk_bs_register_md_thread(struct spdk_blob_store *bs);
static int spdk_bs_unregister_md_thread(struct spdk_blob_store *bs);
static void _spdk_blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno);
void _spdk_blob_insert_cluster_on_md_thread(struct spdk_blob_data *blob, uint32_t cluster_num,
		uint64_t cluster, spdk_blob_op_complete cb_fn, void *cb_arg);

static int _spdk_blob_set_xattr(struct spdk_blob_data *blob, const char *name, const void *value,
				uint16_t value_len, bool internal);
static int _spdk_blob_get_xattr_value(struct spdk_blob_data *blob, const char *name,
				      const void **value, size_t *value_len, bool internal);
static int _spdk_blob_remove_xattr(struct spdk_blob_data *blob, const char *name, bool internal);

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

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming cluster %u\n", cluster_num);

	spdk_bit_array_set(bs->used_clusters, cluster_num);
	bs->num_free_clusters--;
}

static int
_spdk_blob_insert_cluster(struct spdk_blob_data *blob, uint32_t cluster_num, uint64_t cluster)
{
	uint64_t *cluster_lba = &blob->active.clusters[cluster_num];

	assert(spdk_get_thread() == blob->bs->md_thread);

	if (*cluster_lba != 0) {
		return -EEXIST;
	}

	*cluster_lba = _spdk_bs_cluster_to_lba(blob->bs, cluster);
	return 0;
}

static int
_spdk_bs_allocate_cluster(struct spdk_blob_data *blob, uint32_t cluster_num,
			  uint64_t *lowest_free_cluster, bool update_map)
{
	pthread_mutex_lock(&blob->bs->used_clusters_mutex);
	*lowest_free_cluster = spdk_bit_array_find_first_clear(blob->bs->used_clusters,
			       *lowest_free_cluster);
	if (*lowest_free_cluster >= blob->bs->total_clusters) {
		/* No more free clusters. Cannot satisfy the request */
		pthread_mutex_unlock(&blob->bs->used_clusters_mutex);
		return -ENOSPC;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming cluster %lu for blob %lu\n", *lowest_free_cluster, blob->id);
	_spdk_bs_claim_cluster(blob->bs, *lowest_free_cluster);
	pthread_mutex_unlock(&blob->bs->used_clusters_mutex);

	if (update_map) {
		_spdk_blob_insert_cluster(blob, cluster_num, *lowest_free_cluster);
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
	_spdk_blob_xattrs_init(&opts->xattrs);
}

static struct spdk_blob_data *
_spdk_blob_alloc(struct spdk_blob_store *bs, spdk_blob_id id)
{
	struct spdk_blob_data *blob;

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
	TAILQ_INIT(&blob->xattrs_internal);

	return blob;
}

static void
_spdk_xattrs_free(struct spdk_xattr_tailq *xattrs)
{
	struct spdk_xattr 	*xattr, *xattr_tmp;

	TAILQ_FOREACH_SAFE(xattr, xattrs, link, xattr_tmp) {
		TAILQ_REMOVE(xattrs, xattr, link);
		free(xattr->name);
		free(xattr->value);
		free(xattr);
	}
}

static void
_spdk_blob_free(struct spdk_blob_data *blob)
{
	assert(blob != NULL);

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

static int
_spdk_blob_mark_clean(struct spdk_blob_data *blob)
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

static int
_spdk_blob_deserialize_xattr(struct spdk_blob_data *blob,
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
	strncpy(xattr->name, desc_xattr->name, desc_xattr->name_length);
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
_spdk_blob_parse_page(const struct spdk_blob_md_page *page, struct spdk_blob_data *blob)
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

		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_EXTENT) {
			struct spdk_blob_md_descriptor_extent	*desc_extent;
			unsigned int				i, j;
			unsigned int				cluster_count = blob->active.num_clusters;

			desc_extent = (struct spdk_blob_md_descriptor_extent *)desc;

			if (desc_extent->length == 0 ||
			    (desc_extent->length % sizeof(desc_extent->extents[0]) != 0)) {
				return -EINVAL;
			}

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->extents[0]); i++) {
				for (j = 0; j < desc_extent->extents[i].length; j++) {
					if (!spdk_bit_array_get(blob->bs->used_clusters,
								desc_extent->extents[i].cluster_idx + j)) {
						return -EINVAL;
					}
					cluster_count++;
				}
			}

			if (cluster_count == 0) {
				return -EINVAL;
			}
			tmp = realloc(blob->active.clusters, cluster_count * sizeof(uint64_t));
			if (tmp == NULL) {
				return -ENOMEM;
			}
			blob->active.clusters = tmp;
			blob->active.cluster_array_size = cluster_count;

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->extents[0]); i++) {
				for (j = 0; j < desc_extent->extents[i].length; j++) {
					if (desc_extent->extents[i].cluster_idx != 0) {
						blob->active.clusters[blob->active.num_clusters++] = _spdk_bs_cluster_to_lba(blob->bs,
								desc_extent->extents[i].cluster_idx + j);
					} else if (spdk_blob_is_thin_provisioned(blob)) {
						blob->active.clusters[blob->active.num_clusters++] = 0;
					} else {
						return -EINVAL;
					}
				}
			}

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

static int
_spdk_blob_parse(const struct spdk_blob_md_page *pages, uint32_t page_count,
		 struct spdk_blob_data *blob)
{
	const struct spdk_blob_md_page *page;
	uint32_t i;
	int rc;

	assert(page_count > 0);
	assert(pages[0].sequence_num == 0);
	assert(blob != NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);
	assert(blob->active.clusters == NULL);
	assert(blob->state == SPDK_BLOB_STATE_LOADING);

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
_spdk_blob_serialize_add_page(const struct spdk_blob_data *blob,
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
_spdk_blob_serialize_extent(const struct spdk_blob_data *blob,
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

static void
_spdk_blob_serialize_flags(const struct spdk_blob_data *blob,
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
_spdk_blob_serialize_xattrs(const struct spdk_blob_data *blob,
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
				spdk_dma_free(*pages);
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
				spdk_dma_free(*pages);
				*pages = NULL;
				*page_count = 0;
				return -1;
			}
		}

		*remaining_sz -= required_sz;
		*buf += required_sz;
	}

	return 0;
}

static int
_spdk_blob_serialize(const struct spdk_blob_data *blob, struct spdk_blob_md_page **pages,
		     uint32_t *page_count)
{
	struct spdk_blob_md_page		*cur_page;
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
	struct spdk_blob_data 		*blob;

	struct spdk_blob_md_page	*pages;
	uint32_t			num_pages;
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
	struct spdk_blob_load_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;

	_spdk_blob_mark_clean(blob);

	ctx->cb_fn(ctx->seq, ctx->cb_arg, bserrno);

	/* Free the memory */
	spdk_dma_free(ctx->pages);
	free(ctx);
}

static void
_spdk_blob_load_snapshot_cpl(void *cb_arg, struct spdk_blob *snapshot, int bserrno)
{
	struct spdk_blob_load_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;

	if (bserrno != 0) {
		goto error;
	}

	blob->back_bs_dev = spdk_bs_create_blob_bs_dev(snapshot);

	if (blob->back_bs_dev == NULL) {
		bserrno = -ENOMEM;
		goto error;
	}

	_spdk_blob_load_final(ctx, bserrno);
	return;

error:
	SPDK_ERRLOG("Snapshot fail\n");
	_spdk_blob_free(blob);
	ctx->cb_fn(ctx->seq, NULL, bserrno);
	spdk_dma_free(ctx->pages);
	free(ctx);
}

static void
_spdk_blob_load_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_load_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;
	struct spdk_blob_md_page	*page;
	const void			*value;
	size_t				len;
	int				rc;
	uint32_t			crc;

	page = &ctx->pages[ctx->num_pages - 1];
	crc = _spdk_blob_md_page_calc_crc(page);
	if (crc != page->crc) {
		SPDK_ERRLOG("Metadata page %d crc mismatch\n", ctx->num_pages);
		_spdk_blob_free(blob);
		ctx->cb_fn(seq, NULL, -EINVAL);
		spdk_dma_free(ctx->pages);
		free(ctx);
		return;
	}

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

		spdk_bs_sequence_read_dev(seq, &ctx->pages[ctx->num_pages - 1],
					  next_lba,
					  _spdk_bs_byte_to_lba(blob->bs, sizeof(*page)),
					  _spdk_blob_load_cpl, ctx);
		return;
	}

	/* Parse the pages */
	rc = _spdk_blob_parse(ctx->pages, ctx->num_pages, blob);
	if (rc) {
		_spdk_blob_free(blob);
		ctx->cb_fn(seq, NULL, rc);
		spdk_dma_free(ctx->pages);
		free(ctx);
		return;
	}
	ctx->seq = seq;


	if (spdk_blob_is_thin_provisioned(blob)) {
		rc = _spdk_blob_get_xattr_value(blob, BLOB_SNAPSHOT, &value, &len, true);
		if (rc == 0) {
			if (len != sizeof(spdk_blob_id)) {
				_spdk_blob_free(blob);
				ctx->cb_fn(seq, NULL, -EINVAL);
				spdk_dma_free(ctx->pages);
				free(ctx);
				return;
			}
			/* open snapshot blob and continue in the callback function */
			spdk_bs_open_blob(blob->bs, *(spdk_blob_id *)value,
					  _spdk_blob_load_snapshot_cpl, ctx);
			return;
		} else {
			/* add zeroes_dev for thin provisioned blob */
			blob->back_bs_dev = spdk_bs_create_zeroes_dev();
		}
	} else {
		/* standard blob */
		blob->back_bs_dev = NULL;
	}
	_spdk_blob_load_final(ctx, bserrno);
}

/* Load a blob from disk given a blobid */
static void
_spdk_blob_load(spdk_bs_sequence_t *seq, struct spdk_blob_data *blob,
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

	spdk_bs_sequence_read_dev(seq, &ctx->pages[0], lba,
				  _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE),
				  _spdk_blob_load_cpl, ctx);
}

struct spdk_blob_persist_ctx {
	struct spdk_blob_data 		*blob;

	struct spdk_blob_md_page	*pages;

	uint64_t			idx;

	spdk_bs_sequence_cpl		cb_fn;
	void				*cb_arg;
};

static void
_spdk_blob_persist_complete(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;

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
	struct spdk_blob_data 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	void				*tmp;
	size_t				i;

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
	struct spdk_blob_data 		*blob = ctx->blob;
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

		if (next_lba > 0 && (lba + lba_count) == next_lba) {
			/* This cluster is contiguous with the previous one. */
			lba_count += next_lba_count;
			continue;
		}

		/* This cluster is not contiguous with the previous one. */

		/* If a run of LBAs previously existing, send them
		 * as an unmap.
		 */
		if (lba_count > 0) {
			spdk_bs_batch_unmap_dev(batch, lba, lba_count);
		}

		/* Start building the next batch */
		lba = next_lba;
		if (next_lba > 0) {
			lba_count = next_lba_count;
		} else {
			lba_count = 0;
		}
	}

	/* If we ended with a contiguous set of LBAs, send the unmap now */
	if (lba_count > 0) {
		spdk_bs_batch_unmap_dev(batch, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
}

static void
_spdk_blob_persist_zero_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	size_t				i;

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
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
_spdk_blob_persist_zero_pages(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	spdk_bs_batch_t			*batch;
	size_t				i;

	batch = spdk_bs_sequence_to_batch(seq, _spdk_blob_persist_zero_pages_cpl, ctx);

	lba_count = _spdk_bs_byte_to_lba(bs, SPDK_BS_PAGE_SIZE);

	/* This loop starts at 1 because the first page is special and handled
	 * below. The pages (except the first) are never written in place,
	 * so any pages in the clean list must be zeroed.
	 */
	for (i = 1; i < blob->clean.num_pages; i++) {
		lba = _spdk_bs_page_to_lba(bs, bs->md_start + blob->clean.pages[i]);

		spdk_bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	/* The first page will only be zeroed if this is a delete. */
	if (blob->active.num_pages == 0) {
		uint32_t page_num;

		/* The first page in the metadata goes where the blobid indicates */
		page_num = _spdk_bs_blobid_to_page(blob->id);
		lba = _spdk_bs_page_to_lba(bs, bs->md_start + page_num);

		spdk_bs_batch_write_zeroes_dev(batch, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
}

static void
_spdk_blob_persist_write_page_root(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx	*ctx = cb_arg;
	struct spdk_blob_data		*blob = ctx->blob;
	struct spdk_blob_store		*bs = blob->bs;
	uint64_t			lba;
	uint32_t			lba_count;
	struct spdk_blob_md_page	*page;

	if (blob->active.num_pages == 0) {
		/* Move on to the next step */
		_spdk_blob_persist_zero_pages(seq, ctx, 0);
		return;
	}

	lba_count = _spdk_bs_byte_to_lba(bs, sizeof(*page));

	page = &ctx->pages[0];
	/* The first page in the metadata goes where the blobid indicates */
	lba = _spdk_bs_page_to_lba(bs, bs->md_start + _spdk_bs_blobid_to_page(blob->id));

	spdk_bs_sequence_write_dev(seq, page, lba, lba_count,
				   _spdk_blob_persist_zero_pages, ctx);
}

static void
_spdk_blob_persist_write_page_chain(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_persist_ctx 	*ctx = cb_arg;
	struct spdk_blob_data 		*blob = ctx->blob;
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

		spdk_bs_batch_write_dev(batch, page, lba, lba_count);
	}

	spdk_bs_batch_close(batch);
}

static int
_spdk_resize_blob(struct spdk_blob_data *blob, uint64_t sz)
{
	uint64_t	i;
	uint64_t	*tmp;
	uint64_t	lfc; /* lowest free cluster */
	uint64_t	num_clusters;
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
		num_clusters = spdk_min(blob->active.cluster_array_size,
					sz);
	} else {
		num_clusters = blob->active.num_clusters;
	}

	/* Do two passes - one to verify that we can obtain enough clusters
	 * and another to actually claim them.
	 */

	if (spdk_blob_is_thin_provisioned(blob) == false) {
		lfc = 0;
		for (i = num_clusters; i < sz; i++) {
			lfc = spdk_bit_array_find_first_clear(bs->used_clusters, lfc);
			if (lfc >= bs->total_clusters) {
				/* No more free clusters. Cannot satisfy the request */
				return -ENOSPC;
			}
			lfc++;
		}
	}

	if (sz > num_clusters) {
		/* Expand the cluster array if necessary.
		 * We only shrink the array when persisting.
		 */
		tmp = realloc(blob->active.clusters, sizeof(uint64_t) * sz);
		if (sz > 0 && tmp == NULL) {
			return -ENOMEM;
		}
		memset(tmp + blob->active.cluster_array_size, 0,
		       sizeof(uint64_t) * (sz - blob->active.cluster_array_size));
		blob->active.clusters = tmp;
		blob->active.cluster_array_size = sz;
	}

	blob->state = SPDK_BLOB_STATE_DIRTY;

	if (spdk_blob_is_thin_provisioned(blob) == false) {
		lfc = 0;
		for (i = num_clusters; i < sz; i++) {
			_spdk_bs_allocate_cluster(blob, i, &lfc, true);
			lfc++;
		}
	}

	blob->active.num_clusters = sz;

	return 0;
}

/* Write a blob to disk */
static void
_spdk_blob_persist(spdk_bs_sequence_t *seq, struct spdk_blob_data *blob,
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
		_spdk_blob_persist_zero_pages(seq, ctx, 0);
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
		/* Now that previous metadata page is complete, calculate the crc for it. */
		ctx->pages[i - 1].crc = _spdk_blob_md_page_calc_crc(&ctx->pages[i - 1]);
		blob->active.pages[i] = page_num;
		spdk_bit_array_set(bs->used_md_pages, page_num);
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Claiming page %u for blob %lu\n", page_num, blob->id);
		page_num++;
	}
	ctx->pages[i - 1].crc = _spdk_blob_md_page_calc_crc(&ctx->pages[i - 1]);
	/* Start writing the metadata from last page to first */
	ctx->idx = blob->active.num_pages - 1;
	_spdk_blob_persist_write_page_chain(seq, ctx, 0);
}

struct spdk_blob_copy_cluster_ctx {
	struct spdk_blob_data *blob;
	uint8_t *buf;
	uint64_t page;
	uint64_t new_cluster;
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
			spdk_bs_user_op_execute(op);
		} else {
			spdk_bs_user_op_abort(op);
		}
	}

	spdk_dma_free(ctx->buf);
	free(ctx);
}

static void
_spdk_blob_insert_cluster_cpl(void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno) {
		uint32_t cluster_number;

		if (bserrno == -EEXIST) {
			/* The metadata insert failed because another thread
			 * allocated the cluster first. Free our cluster
			 * but continue without error. */
			bserrno = 0;
		}

		cluster_number = _spdk_bs_page_to_cluster(ctx->blob->bs, ctx->page);
		_spdk_bs_release_cluster(ctx->blob->bs, cluster_number);
	}

	spdk_bs_sequence_finish(ctx->seq, bserrno);
}

static void
_spdk_blob_write_copy_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;
	uint32_t cluster_number;

	if (bserrno) {
		/* The write failed, so jump to the final completion handler */
		spdk_bs_sequence_finish(seq, bserrno);
		return;
	}

	cluster_number = _spdk_bs_page_to_cluster(ctx->blob->bs, ctx->page);

	_spdk_blob_insert_cluster_on_md_thread(ctx->blob, cluster_number, ctx->new_cluster,
					       _spdk_blob_insert_cluster_cpl, ctx);
}

static void
_spdk_blob_write_copy(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_copy_cluster_ctx *ctx = cb_arg;

	if (bserrno != 0) {
		/* The read failed, so jump to the final completion handler */
		spdk_bs_sequence_finish(seq, bserrno);
		return;
	}

	/* Write whole cluster */
	spdk_bs_sequence_write_dev(seq, ctx->buf,
				   _spdk_bs_cluster_to_lba(ctx->blob->bs, ctx->new_cluster),
				   _spdk_bs_cluster_to_lba(ctx->blob->bs, 1),
				   _spdk_blob_write_copy_cpl, ctx);
}

static void
_spdk_bs_allocate_and_copy_cluster(struct spdk_blob_data *blob,
				   struct spdk_io_channel *_ch,
				   uint64_t offset, spdk_bs_user_op_t *op)
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

	/* Round the page offset down to the first page in the cluster */
	cluster_start_page = _spdk_bs_page_to_cluster_start(blob, offset);

	/* Calculate which index in the metadata cluster array the corresponding
	 * cluster is supposed to be at. */
	cluster_number = _spdk_bs_page_to_cluster(blob->bs, cluster_start_page);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_bs_user_op_abort(op);
		return;
	}

	assert(blob->bs->cluster_sz % blob->back_bs_dev->blocklen == 0);

	ctx->blob = blob;
	ctx->page = cluster_start_page;

	ctx->buf = spdk_dma_malloc(blob->bs->cluster_sz, blob->back_bs_dev->blocklen, NULL);
	if (!ctx->buf) {
		SPDK_ERRLOG("DMA allocation for cluster of size = %" PRIu32 " failed.\n",
			    blob->bs->cluster_sz);
		free(ctx);
		spdk_bs_user_op_abort(op);
		return;
	}

	rc = _spdk_bs_allocate_cluster(blob, cluster_number, &ctx->new_cluster, false);
	if (rc != 0) {
		spdk_dma_free(ctx->buf);
		free(ctx);
		spdk_bs_user_op_abort(op);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = _spdk_blob_allocate_and_copy_cluster_cpl;
	cpl.u.blob_basic.cb_arg = ctx;

	ctx->seq = spdk_bs_sequence_start(_ch, &cpl);
	if (!ctx->seq) {
		_spdk_bs_release_cluster(blob->bs, ctx->new_cluster);
		spdk_dma_free(ctx->buf);
		free(ctx);
		spdk_bs_user_op_abort(op);
		return;
	}

	/* Queue the user op to block other incoming operations */
	TAILQ_INSERT_TAIL(&ch->need_cluster_alloc, op, link);

	/* Read cluster from backing device */
	spdk_bs_sequence_read_bs_dev(ctx->seq, blob->back_bs_dev, ctx->buf,
				     _spdk_bs_dev_page_to_lba(blob->back_bs_dev, cluster_start_page),
				     _spdk_bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz),
				     _spdk_blob_write_copy, ctx);
}

static void
_spdk_blob_calculate_lba_and_lba_count(struct spdk_blob_data *blob, uint64_t page, uint64_t length,
				       uint64_t *lba,	uint32_t *lba_count)
{
	*lba_count = _spdk_bs_page_to_lba(blob->bs, length);

	if (!_spdk_bs_page_is_allocated(blob, page)) {
		assert(blob->back_bs_dev != NULL);
		*lba = _spdk_bs_dev_page_to_lba(blob->back_bs_dev, page);
		*lba_count = _spdk_bs_blob_lba_to_back_dev_lba(blob, *lba_count);
	} else {
		*lba = _spdk_bs_blob_page_to_lba(blob, page);
	}
}

static void
_spdk_blob_request_submit_op_split(struct spdk_io_channel *ch, struct spdk_blob *_blob,
				   void *payload, uint64_t offset, uint64_t length,
				   spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	spdk_bs_batch_t		*batch;
	struct spdk_bs_cpl	cpl;
	struct spdk_blob_data	*blob = __blob_to_data(_blob);
	uint64_t		op_length;
	uint8_t			*buf;

	assert(blob != NULL);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	batch = spdk_bs_batch_open(ch, &cpl);
	if (!batch) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	buf = payload;
	while (length > 0) {
		op_length = spdk_min(length, _spdk_bs_num_pages_to_cluster_boundary(blob, offset));

		switch (op_type) {
		case SPDK_BLOB_READ:
			spdk_bs_batch_read_blob(batch, _blob, buf, offset, op_length);
			break;
		case SPDK_BLOB_WRITE:
			spdk_bs_batch_write_blob(batch, _blob, buf, offset, op_length);
			break;
		case SPDK_BLOB_UNMAP:
			spdk_bs_batch_unmap_blob(batch, _blob, offset, op_length);
			break;
		case SPDK_BLOB_WRITE_ZEROES:
			spdk_bs_batch_write_zeroes_blob(batch, _blob, offset, op_length);
			break;
		case SPDK_BLOB_READV:
		case SPDK_BLOB_WRITEV:
			SPDK_ERRLOG("readv/write not valid for %s\n", __func__);
			break;
		}

		length -= op_length;
		offset += op_length;
		if (op_type == SPDK_BLOB_WRITE || op_type == SPDK_BLOB_READ) {
			buf += op_length * SPDK_BS_PAGE_SIZE;
		}
	}

	spdk_bs_batch_close(batch);
}

static void
_spdk_blob_request_submit_op_single(struct spdk_io_channel *_ch, struct spdk_blob *_blob,
				    void *payload, uint64_t offset, uint64_t length,
				    spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct spdk_blob_data *blob = __blob_to_data(_blob);
	struct spdk_bs_cpl cpl;
	uint64_t lba;
	uint32_t lba_count;

	assert(blob != NULL);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	_spdk_blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

	switch (op_type) {
	case SPDK_BLOB_READ: {
		spdk_bs_batch_t *batch;

		batch = spdk_bs_batch_open(_ch, &cpl);
		if (!batch) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		if (_spdk_bs_page_is_allocated(blob, offset)) {
			/* Read from the blob */
			spdk_bs_batch_read_dev(batch, payload, lba, lba_count);
		} else {
			/* Read from the backing block device */
			spdk_bs_batch_read_bs_dev(batch, blob->back_bs_dev, payload, lba, lba_count);
		}

		spdk_bs_batch_close(batch);
		break;
	}
	case SPDK_BLOB_WRITE:
	case SPDK_BLOB_WRITE_ZEROES: {
		if (_spdk_bs_page_is_allocated(blob, offset)) {
			/* Write to the blob */
			spdk_bs_batch_t *batch;

			batch = spdk_bs_batch_open(_ch, &cpl);
			if (!batch) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (op_type == SPDK_BLOB_WRITE) {
				spdk_bs_batch_write_dev(batch, payload, lba, lba_count);
			} else {
				spdk_bs_batch_write_zeroes_dev(batch, lba, lba_count);
			}

			spdk_bs_batch_close(batch);
		} else {
			/* Queue this operation and allocate the cluster */
			spdk_bs_user_op_t *op;

			op = spdk_bs_user_op_alloc(_ch, &cpl, op_type, _blob, payload, 0, offset, length);
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

		batch = spdk_bs_batch_open(_ch, &cpl);
		if (!batch) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		if (_spdk_bs_page_is_allocated(blob, offset)) {
			spdk_bs_batch_unmap_dev(batch, lba, lba_count);
		}

		spdk_bs_batch_close(batch);
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
_spdk_blob_request_submit_op(struct spdk_blob *_blob, struct spdk_io_channel *_channel,
			     void *payload, uint64_t offset, uint64_t length,
			     spdk_blob_op_complete cb_fn, void *cb_arg, enum spdk_blob_op_type op_type)
{
	struct spdk_blob_data		*blob = __blob_to_data(_blob);

	assert(blob != NULL);

	if (blob->data_ro && op_type != SPDK_BLOB_READ) {
		cb_fn(cb_arg, -EPERM);
		return;
	}

	if (offset + length > blob->active.num_clusters * blob->bs->pages_per_cluster) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (length <= _spdk_bs_num_pages_to_cluster_boundary(blob, offset)) {
		_spdk_blob_request_submit_op_single(_channel, _blob, payload, offset, length,
						    cb_fn, cb_arg, op_type);
	} else {
		_spdk_blob_request_submit_op_split(_channel, _blob, payload, offset, length,
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
_spdk_rw_iov_split_next(void *cb_arg, int bserrno)
{
	struct rw_iov_ctx *ctx = cb_arg;
	struct spdk_blob_data *blob = __blob_to_data(ctx->blob);
	struct iovec *iov, *orig_iov;
	int iovcnt;
	size_t orig_iovoff;
	uint64_t page_count, pages_to_boundary, page_offset;
	uint64_t byte_count;

	if (bserrno != 0 || ctx->pages_remaining == 0) {
		ctx->cb_fn(ctx->cb_arg, bserrno);
		free(ctx);
		return;
	}

	page_offset = ctx->page_offset;
	pages_to_boundary = _spdk_bs_num_pages_to_cluster_boundary(blob, page_offset);
	page_count = spdk_min(ctx->pages_remaining, pages_to_boundary);

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
		spdk_bs_io_readv_blob(ctx->blob, ctx->channel, iov, iovcnt, page_offset,
				      page_count, _spdk_rw_iov_split_next, ctx);
	} else {
		spdk_bs_io_writev_blob(ctx->blob, ctx->channel, iov, iovcnt, page_offset,
				       page_count, _spdk_rw_iov_split_next, ctx);
	}
}

static void
_spdk_blob_request_submit_rw_iov(struct spdk_blob *_blob, struct spdk_io_channel *_channel,
				 struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
				 spdk_blob_op_complete cb_fn, void *cb_arg, bool read)
{
	struct spdk_blob_data	*blob = __blob_to_data(_blob);
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

	if (offset + length > blob->active.num_clusters * blob->bs->pages_per_cluster) {
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
	if (spdk_likely(length <= _spdk_bs_num_pages_to_cluster_boundary(blob, offset))) {
		uint32_t lba_count;
		uint64_t lba;

		_spdk_blob_calculate_lba_and_lba_count(blob, offset, length, &lba, &lba_count);

		cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
		cpl.u.blob_basic.cb_fn = cb_fn;
		cpl.u.blob_basic.cb_arg = cb_arg;

		if (read) {
			spdk_bs_sequence_t *seq;

			seq = spdk_bs_sequence_start(_channel, &cpl);
			if (!seq) {
				cb_fn(cb_arg, -ENOMEM);
				return;
			}

			if (_spdk_bs_page_is_allocated(blob, offset)) {
				spdk_bs_sequence_readv_dev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
			} else {
				spdk_bs_sequence_readv_bs_dev(seq, blob->back_bs_dev, iov, iovcnt, lba, lba_count,
							      _spdk_rw_iov_done, NULL);
			}
		} else {
			if (_spdk_bs_page_is_allocated(blob, offset)) {
				spdk_bs_sequence_t *seq;

				seq = spdk_bs_sequence_start(_channel, &cpl);
				if (!seq) {
					cb_fn(cb_arg, -ENOMEM);
					return;
				}

				spdk_bs_sequence_writev_dev(seq, iov, iovcnt, lba, lba_count, _spdk_rw_iov_done, NULL);
			} else {
				/* Queue this operation and allocate the cluster */
				spdk_bs_user_op_t *op;

				op = spdk_bs_user_op_alloc(_channel, &cpl, SPDK_BLOB_WRITEV, _blob, iov, iovcnt, offset, length);
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

		ctx->blob = _blob;
		ctx->channel = _channel;
		ctx->cb_fn = cb_fn;
		ctx->cb_arg = cb_arg;
		ctx->read = read;
		ctx->orig_iov = iov;
		ctx->iovcnt = iovcnt;
		ctx->page_offset = offset;
		ctx->pages_remaining = length;
		ctx->pages_done = 0;

		_spdk_rw_iov_split_next(ctx, 0);
	}
}

static struct spdk_blob_data *
_spdk_blob_lookup(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	struct spdk_blob_data *blob;

	TAILQ_FOREACH(blob, &bs->blobs, link) {
		if (blob->id == blobid) {
			return blob;
		}
	}

	return NULL;
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
		spdk_bs_user_op_abort(op);
	}

	free(channel->req_mem);
	channel->dev->destroy_channel(channel->dev, channel->dev_channel);
}

static void
_spdk_bs_dev_destroy(void *io_device)
{
	struct spdk_blob_store *bs = io_device;
	struct spdk_blob_data	*blob, *blob_tmp;

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
	spdk_bs_call_cpl(&bs->unload_cpl, bs->unload_err);

	free(bs);
}

static void
_spdk_bs_free(struct spdk_blob_store *bs)
{
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

static struct spdk_blob_store *
_spdk_bs_alloc(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts)
{
	struct spdk_blob_store	*bs;
	uint64_t dev_size;
	int rc;

	dev_size = dev->blocklen * dev->blockcnt;
	if (dev_size < opts->cluster_sz) {
		/* Device size cannot be smaller than cluster size of blobstore */
		SPDK_ERRLOG("Device size %" PRIu64 " is smaller than cluster size %" PRIu32 "\n",
			    dev_size, opts->cluster_sz);
		return NULL;
	}
	if (opts->cluster_sz < SPDK_BS_PAGE_SIZE) {
		/* Cluster size cannot be smaller than page size */
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than page size %d\n",
			    opts->cluster_sz, SPDK_BS_PAGE_SIZE);
		return NULL;
	}
	bs = calloc(1, sizeof(struct spdk_blob_store));
	if (!bs) {
		return NULL;
	}

	TAILQ_INIT(&bs->blobs);
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
	bs->num_free_clusters = bs->total_clusters;
	bs->used_clusters = spdk_bit_array_create(bs->total_clusters);
	if (bs->used_clusters == NULL) {
		free(bs);
		return NULL;
	}

	bs->max_channel_ops = opts->max_channel_ops;
	bs->super_blob = SPDK_BLOBID_INVALID;
	memcpy(&bs->bstype, &opts->bstype, sizeof(opts->bstype));

	/* The metadata is assumed to be at least 1 page */
	bs->used_md_pages = spdk_bit_array_create(1);
	bs->used_blobids = spdk_bit_array_create(0);

	pthread_mutex_init(&bs->used_clusters_mutex, NULL);

	spdk_io_device_register(bs, _spdk_bs_channel_create, _spdk_bs_channel_destroy,
				sizeof(struct spdk_bs_channel));
	rc = spdk_bs_register_md_thread(bs);
	if (rc == -1) {
		spdk_io_device_unregister(bs, NULL);
		pthread_mutex_destroy(&bs->used_clusters_mutex);
		spdk_bit_array_free(&bs->used_blobids);
		spdk_bit_array_free(&bs->used_md_pages);
		spdk_bit_array_free(&bs->used_clusters);
		free(bs);
		return NULL;
	}

	return bs;
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
	bool				is_load;

	spdk_bs_sequence_t			*seq;
	spdk_blob_op_with_handle_complete	iter_cb_fn;
	void					*iter_cb_arg;
};

static void
_spdk_bs_load_ctx_fail(spdk_bs_sequence_t *seq, struct spdk_bs_load_ctx *ctx, int bserrno)
{
	assert(bserrno != 0);

	spdk_dma_free(ctx->super);
	/*
	 * Only free the blobstore when a load fails.  If an unload fails (for some reason)
	 *  we want to keep the blobstore in case the caller wants to try again.
	 */
	if (ctx->is_load) {
		_spdk_bs_free(ctx->bs);
	}
	free(ctx);
	spdk_bs_sequence_finish(seq, bserrno);
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

static void
_spdk_bs_write_super(spdk_bs_sequence_t *seq, struct spdk_blob_store *bs,
		     struct spdk_bs_super_block *super, spdk_bs_sequence_cpl cb_fn, void *cb_arg)
{
	/* Update the values in the super block */
	super->super_blob = bs->super_blob;
	memcpy(&super->bstype, &bs->bstype, sizeof(bs->bstype));
	super->crc = _spdk_blob_md_page_calc_crc(super);
	spdk_bs_sequence_write_dev(seq, super, _spdk_bs_page_to_lba(bs, 0),
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
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_CLUSTERS;
	ctx->mask->length = ctx->bs->total_clusters;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_clusters));

	_spdk_bs_set_mask(ctx->bs->used_clusters, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	spdk_bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void
_spdk_bs_write_used_md(spdk_bs_sequence_t *seq, void *arg, spdk_bs_sequence_cpl cb_fn)
{
	struct spdk_bs_load_ctx	*ctx = arg;
	uint64_t	mask_size, lba, lba_count;

	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_PAGES;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_md_pages));

	_spdk_bs_set_mask(ctx->bs->used_md_pages, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	spdk_bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
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
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_BLOBIDS;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_blobids));

	_spdk_bs_set_mask(ctx->bs->used_blobids, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	spdk_bs_sequence_write_dev(seq, ctx->mask, lba, lba_count, cb_fn, arg);
}

static void _spdk_bs_load_complete(spdk_bs_sequence_t *seq, struct spdk_bs_load_ctx *ctx,
				   int bserrno);

static void
_spdk_bs_load_iter(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = arg;

	if (bserrno == 0) {
		ctx->iter_cb_fn(ctx->iter_cb_arg, blob, 0);
		spdk_bs_iter_next(ctx->bs, blob, _spdk_bs_load_iter, ctx);
		return;
	}

	if (bserrno == -ENOENT) {
		bserrno = 0;
	} else {
		/*
		 * This case needs to be looked at further.  Same problem
		 *  exists with applications that rely on explicit blob
		 *  iteration.  We should just skip the blob that failed
		 *  to load and coontinue on to the next one.
		 */
		SPDK_ERRLOG("Error in iterating blobs\n");
	}

	ctx->iter_cb_fn = NULL;
	_spdk_bs_load_complete(ctx->seq, ctx, bserrno);
}

static void
_spdk_bs_load_complete(spdk_bs_sequence_t *seq, struct spdk_bs_load_ctx *ctx, int bserrno)
{
	if (ctx->iter_cb_fn) {
		ctx->seq = seq;
		spdk_bs_iter_first(ctx->bs, _spdk_bs_load_iter, ctx);
		return;
	}

	spdk_dma_free(ctx->super);
	spdk_dma_free(ctx->mask);
	free(ctx);
	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_load_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t i, j;
	int rc;

	/* The type must be correct */
	assert(ctx->mask->type == SPDK_MD_MASK_TYPE_USED_BLOBIDS);

	/* The length of the mask (in bits) must not be greater than
	 * the length of the buffer (converted to bits) */
	assert(ctx->mask->length <= (ctx->super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE * 8));

	/* The length of the mask must be exactly equal to the size
	 * (in pages) of the metadata region */
	assert(ctx->mask->length == ctx->super->md_len);

	rc = spdk_bit_array_resize(&ctx->bs->used_blobids, ctx->mask->length);
	if (rc < 0) {
		spdk_dma_free(ctx->mask);
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	for (i = 0; i < ctx->mask->length / 8; i++) {
		uint8_t segment = ctx->mask->mask[i];
		for (j = 0; segment; j++) {
			if (segment & 1U) {
				spdk_bit_array_set(ctx->bs->used_blobids, (i * 8) + j);
			}
			segment >>= 1U;
		}
	}

	_spdk_bs_load_complete(seq, ctx, bserrno);
}

static void
_spdk_bs_load_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t		lba, lba_count, mask_size;
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
		spdk_dma_free(ctx->mask);
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
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

	spdk_dma_free(ctx->mask);

	/* Read the used blobids mask */
	mask_size = ctx->super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_blobid_mask_len);
	spdk_bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
				  _spdk_bs_load_used_blobids_cpl, ctx);
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
		spdk_dma_free(ctx->mask);
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
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
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	spdk_bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
				  _spdk_bs_load_used_clusters_cpl, ctx);
}

static void
_spdk_bs_load_write_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;
	uint64_t lba, lba_count, mask_size;

	/* Read the used pages mask */
	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	spdk_bs_sequence_read_dev(seq, ctx->mask, lba, lba_count,
				  _spdk_bs_load_used_pages_cpl, ctx);
}

static int
_spdk_bs_load_replay_md_parse_page(const struct spdk_blob_md_page *page, struct spdk_blob_store *bs)
{
	struct spdk_blob_md_descriptor *desc;
	size_t	cur_desc = 0;

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
			unsigned int				cluster_count = 0;

			desc_extent = (struct spdk_blob_md_descriptor_extent *)desc;

			for (i = 0; i < desc_extent->length / sizeof(desc_extent->extents[0]); i++) {
				for (j = 0; j < desc_extent->extents[i].length; j++) {
					spdk_bit_array_set(bs->used_clusters, desc_extent->extents[i].cluster_idx + j);
					if (bs->num_free_clusters == 0) {
						return -1;
					}
					bs->num_free_clusters--;
					cluster_count++;
				}
			}
			if (cluster_count == 0) {
				return -1;
			}
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_XATTR_INTERNAL) {
			/* Skip this item */
		} else if (desc->type == SPDK_MD_DESCRIPTOR_TYPE_FLAGS) {
			/* Skip this item */
		} else {
			/* Error */
			return -1;
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

static bool _spdk_bs_load_cur_md_page_valid(struct spdk_bs_load_ctx *ctx)
{
	uint32_t crc;

	crc = _spdk_blob_md_page_calc_crc(ctx->page);
	if (crc != ctx->page->crc) {
		return false;
	}

	if (_spdk_bs_page_to_blobid(ctx->cur_page) != ctx->page->id) {
		return false;
	}
	return true;
}

static void
_spdk_bs_load_replay_cur_md_page(spdk_bs_sequence_t *seq, void *cb_arg);

static void
_spdk_bs_load_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	_spdk_bs_load_complete(seq, ctx, bserrno);
}

static void
_spdk_bs_load_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);
	ctx->mask = NULL;

	_spdk_bs_write_used_clusters(seq, cb_arg, _spdk_bs_load_write_used_clusters_cpl);
}

static void
_spdk_bs_load_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);
	ctx->mask = NULL;

	_spdk_bs_write_used_blobids(seq, cb_arg, _spdk_bs_load_write_used_blobids_cpl);
}

static void
_spdk_bs_load_write_used_md(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	_spdk_bs_write_used_md(seq, cb_arg, _spdk_bs_load_write_used_pages_cpl);
}

static void
_spdk_bs_load_replay_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t num_md_clusters;
	uint64_t i;
	uint32_t page_num;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, bserrno);
		return;
	}

	page_num = ctx->cur_page;
	if (_spdk_bs_load_cur_md_page_valid(ctx) == true) {
		if (ctx->page->sequence_num == 0 || ctx->in_page_chain == true) {
			spdk_bit_array_set(ctx->bs->used_md_pages, page_num);
			if (ctx->page->sequence_num == 0) {
				spdk_bit_array_set(ctx->bs->used_blobids, page_num);
			}
			if (_spdk_bs_load_replay_md_parse_page(ctx->page, ctx->bs)) {
				_spdk_bs_load_ctx_fail(seq, ctx, -EILSEQ);
				return;
			}
			if (ctx->page->next != SPDK_INVALID_MD_PAGE) {
				ctx->in_page_chain = true;
				ctx->cur_page = ctx->page->next;
				_spdk_bs_load_replay_cur_md_page(seq, cb_arg);
				return;
			}
		}
	}

	ctx->in_page_chain = false;

	do {
		ctx->page_index++;
	} while (spdk_bit_array_get(ctx->bs->used_md_pages, ctx->page_index) == true);

	if (ctx->page_index < ctx->super->md_len) {
		ctx->cur_page = ctx->page_index;
		_spdk_bs_load_replay_cur_md_page(seq, cb_arg);
	} else {
		/* Claim all of the clusters used by the metadata */
		num_md_clusters = divide_round_up(ctx->super->md_len, ctx->bs->pages_per_cluster);
		for (i = 0; i < num_md_clusters; i++) {
			_spdk_bs_claim_cluster(ctx->bs, i);
		}
		spdk_dma_free(ctx->page);
		_spdk_bs_load_write_used_md(seq, ctx, bserrno);
	}
}

static void
_spdk_bs_load_replay_cur_md_page(spdk_bs_sequence_t *seq, void *cb_arg)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint64_t lba;

	assert(ctx->cur_page < ctx->super->md_len);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->md_start + ctx->cur_page);
	spdk_bs_sequence_read_dev(seq, ctx->page, lba,
				  _spdk_bs_byte_to_lba(ctx->bs, SPDK_BS_PAGE_SIZE),
				  _spdk_bs_load_replay_md_cpl, ctx);
}

static void
_spdk_bs_load_replay_md(spdk_bs_sequence_t *seq, void *cb_arg)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;

	ctx->page_index = 0;
	ctx->cur_page = 0;
	ctx->page = spdk_dma_zmalloc(SPDK_BS_PAGE_SIZE,
				     SPDK_BS_PAGE_SIZE,
				     NULL);
	if (!ctx->page) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}
	_spdk_bs_load_replay_cur_md_page(seq, cb_arg);
}

static void
_spdk_bs_recover(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	int 		rc;

	if (bserrno != 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, -EIO);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_md_pages, ctx->super->md_len);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_blobids, ctx->super->md_len);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	rc = spdk_bit_array_resize(&ctx->bs->used_clusters, ctx->bs->total_clusters);
	if (rc < 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, -ENOMEM);
		return;
	}

	ctx->bs->num_free_clusters = ctx->bs->total_clusters;
	_spdk_bs_load_replay_md(seq, cb_arg);
}

static void
_spdk_bs_load_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx *ctx = cb_arg;
	uint32_t	crc;
	static const char zeros[SPDK_BLOBSTORE_TYPE_LENGTH];

	if (ctx->super->version > SPDK_BS_VERSION ||
	    ctx->super->version < SPDK_BS_INITIAL_VERSION) {
		_spdk_bs_load_ctx_fail(seq, ctx, -EILSEQ);
		return;
	}

	if (memcmp(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
		   sizeof(ctx->super->signature)) != 0) {
		_spdk_bs_load_ctx_fail(seq, ctx, -EILSEQ);
		return;
	}

	crc = _spdk_blob_md_page_calc_crc(ctx->super);
	if (crc != ctx->super->crc) {
		_spdk_bs_load_ctx_fail(seq, ctx, -EILSEQ);
		return;
	}

	if (memcmp(&ctx->bs->bstype, &ctx->super->bstype, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Bstype matched - loading blobstore\n");
	} else if (memcmp(&ctx->bs->bstype, zeros, SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Bstype wildcard used - loading blobstore regardless bstype\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Unexpected bstype\n");
		SPDK_TRACEDUMP(SPDK_LOG_BLOB, "Expected:", ctx->bs->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		SPDK_TRACEDUMP(SPDK_LOG_BLOB, "Found:", ctx->super->bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH);
		_spdk_bs_load_ctx_fail(seq, ctx, -ENXIO);
		return;
	}

	/* Parse the super block */
	ctx->bs->cluster_sz = ctx->super->cluster_size;
	ctx->bs->total_clusters = ctx->bs->dev->blockcnt / (ctx->bs->cluster_sz / ctx->bs->dev->blocklen);
	ctx->bs->pages_per_cluster = ctx->bs->cluster_sz / SPDK_BS_PAGE_SIZE;
	ctx->bs->md_start = ctx->super->md_start;
	ctx->bs->md_len = ctx->super->md_len;
	ctx->bs->total_data_clusters = ctx->bs->total_clusters - divide_round_up(
					       ctx->bs->md_start + ctx->bs->md_len, ctx->bs->pages_per_cluster);
	ctx->bs->super_blob = ctx->super->super_blob;
	memcpy(&ctx->bs->bstype, &ctx->super->bstype, sizeof(ctx->super->bstype));

	if (ctx->super->clean == 0) {
		_spdk_bs_recover(seq, ctx, 0);
	} else if (ctx->super->used_blobid_mask_len == 0) {
		/*
		 * Metadata is clean, but this is an old metadata format without
		 *  a blobid mask.  Clear the clean bit and then build the masks
		 *  using _spdk_bs_recover.
		 */
		ctx->super->clean = 0;
		_spdk_bs_write_super(seq, ctx->bs, ctx->super, _spdk_bs_recover, ctx);
	} else {
		ctx->super->clean = 0;
		_spdk_bs_write_super(seq, ctx->bs, ctx->super, _spdk_bs_load_write_super_cpl, ctx);
	}
}

void
spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_load_ctx *ctx;
	struct spdk_bs_opts	opts = {};

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Loading blobstore from dev %p\n", dev);

	if (o) {
		opts = *o;
	} else {
		spdk_bs_opts_init(&opts);
	}

	if (opts.max_md_ops == 0 || opts.max_channel_ops == 0) {
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

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
	ctx->is_load = true;
	ctx->iter_cb_fn = opts.iter_cb_fn;
	ctx->iter_cb_arg = opts.iter_cb_arg;

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

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* Read the super block */
	spdk_bs_sequence_read_dev(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
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
	spdk_bs_sequence_write_dev(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
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

	bs = _spdk_bs_alloc(dev, &opts);
	if (!bs) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -ENOMEM);
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

	/* The used_blobids mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_blobid_mask_start = num_md_pages;
	ctx->super->used_blobid_mask_len = divide_round_up(sizeof(struct spdk_bs_md_mask) +
					   divide_round_up(bs->md_len, 8),
					   SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_blobid_mask_len;

	/* The metadata region size was chosen above */
	ctx->super->md_start = bs->md_start = num_md_pages;
	ctx->super->md_len = bs->md_len;
	num_md_pages += bs->md_len;

	num_md_lba = _spdk_bs_page_to_lba(bs, num_md_pages);

	ctx->super->crc = _spdk_blob_md_page_calc_crc(ctx->super);

	num_md_clusters = divide_round_up(num_md_pages, bs->pages_per_cluster);
	if (num_md_clusters > bs->total_clusters) {
		SPDK_ERRLOG("Blobstore metadata cannot use more clusters than is available, "
			    "please decrease number of pages reserved for metadata "
			    "or increase cluster size.\n");
		spdk_dma_free(ctx->super);
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

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	batch = spdk_bs_sequence_to_batch(seq, _spdk_bs_init_trim_cpl, ctx);

	/* Clear metadata space */
	spdk_bs_batch_write_zeroes_dev(batch, 0, num_md_lba);
	/* Trim data clusters */
	spdk_bs_batch_unmap_dev(batch, num_md_lba, ctx->bs->dev->blockcnt - num_md_lba);

	spdk_bs_batch_close(batch);
}

/* END spdk_bs_init */

/* START spdk_bs_destroy */

static void
_spdk_bs_destroy_trim_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx *ctx = cb_arg;
	struct spdk_blob_store *bs = ctx->bs;

	/*
	 * We need to defer calling spdk_bs_call_cpl() until after
	 * dev destruction, so tuck these away for later use.
	 */
	bs->unload_err = bserrno;
	memcpy(&bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	spdk_bs_sequence_finish(seq, bserrno);

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

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Write zeroes to the super block */
	spdk_bs_sequence_write_zeroes_dev(seq,
					  _spdk_bs_page_to_lba(bs, 0),
					  _spdk_bs_byte_to_lba(bs, sizeof(struct spdk_bs_super_block)),
					  _spdk_bs_destroy_trim_cpl, ctx);
}

/* END spdk_bs_destroy */

/* START spdk_bs_unload */

static void
_spdk_bs_unload_write_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->super);

	/*
	 * We need to defer calling spdk_bs_call_cpl() until after
	 * dev destuction, so tuck these away for later use.
	 */
	ctx->bs->unload_err = bserrno;
	memcpy(&ctx->bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	spdk_bs_sequence_finish(seq, bserrno);

	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void
_spdk_bs_unload_write_used_clusters_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);
	ctx->super->clean = 1;

	_spdk_bs_write_super(seq, ctx->bs, ctx->super, _spdk_bs_unload_write_super_cpl, ctx);
}

static void
_spdk_bs_unload_write_used_blobids_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);
	ctx->mask = NULL;

	_spdk_bs_write_used_clusters(seq, cb_arg, _spdk_bs_unload_write_used_clusters_cpl);
}

static void
_spdk_bs_unload_write_used_pages_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_load_ctx	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);
	ctx->mask = NULL;

	_spdk_bs_write_used_blobids(seq, cb_arg, _spdk_bs_unload_write_used_blobids_cpl);
}

static void
_spdk_bs_unload_read_super_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	_spdk_bs_write_used_md(seq, cb_arg, _spdk_bs_unload_write_used_pages_cpl);
}

void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
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
	ctx->is_load = false;

	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Read super block */
	spdk_bs_sequence_read_dev(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
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

spdk_blob_id spdk_blob_get_id(struct spdk_blob *_blob)
{
	struct spdk_blob_data *blob = __blob_to_data(_blob);

	assert(blob != NULL);

	return blob->id;
}

uint64_t spdk_blob_get_num_pages(struct spdk_blob *_blob)
{
	struct spdk_blob_data *blob = __blob_to_data(_blob);

	assert(blob != NULL);

	return _spdk_bs_cluster_to_page(blob->bs, blob->active.num_clusters);
}

uint64_t spdk_blob_get_num_clusters(struct spdk_blob *_blob)
{
	struct spdk_blob_data *blob = __blob_to_data(_blob);

	assert(blob != NULL);

	return blob->active.num_clusters;
}

/* START spdk_bs_create_blob */

static void
_spdk_bs_create_blob_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_data *blob = cb_arg;

	_spdk_blob_free(blob);

	spdk_bs_sequence_finish(seq, bserrno);
}

static int
_spdk_blob_set_xattrs(struct spdk_blob_data *blob, const struct spdk_blob_xattr_opts *xattrs,
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
_spdk_blob_set_thin_provision(struct spdk_blob_data *blob)
{
	blob->invalid_flags |= SPDK_BLOB_THIN_PROV;
	blob->state = SPDK_BLOB_STATE_DIRTY;
}

static void
_spdk_bs_create_blob(struct spdk_blob_store *bs,
		     const struct spdk_blob_opts *opts,
		     const struct spdk_blob_xattr_opts *internal_xattrs,
		     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_data	*blob;
	uint32_t		page_idx;
	struct spdk_bs_cpl 	cpl;
	struct spdk_blob_opts	opts_default;
	struct spdk_blob_xattr_opts internal_xattrs_default;
	spdk_bs_sequence_t	*seq;
	spdk_blob_id		id;
	int rc;

	page_idx = spdk_bit_array_find_first_clear(bs->used_md_pages, 0);
	if (page_idx >= spdk_bit_array_capacity(bs->used_md_pages)) {
		cb_fn(cb_arg, 0, -ENOMEM);
		return;
	}
	spdk_bit_array_set(bs->used_blobids, page_idx);
	spdk_bit_array_set(bs->used_md_pages, page_idx);

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

	rc = spdk_blob_resize(__data_to_blob(blob), opts->num_clusters);
	if (rc < 0) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, 0, rc);
		return;
	}
	cpl.type = SPDK_BS_CPL_TYPE_BLOBID;
	cpl.u.blobid.cb_fn = cb_fn;
	cpl.u.blobid.cb_arg = cb_arg;
	cpl.u.blobid.blobid = blob->id;

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
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

/* START spdk_blob_resize */
int
spdk_blob_resize(struct spdk_blob *_blob, uint64_t sz)
{
	struct spdk_blob_data	*blob = __blob_to_data(_blob);
	int			rc;

	assert(blob != NULL);
	assert(spdk_get_thread() == blob->bs->md_thread);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Resizing blob %lu to %lu clusters\n", blob->id, sz);

	if (blob->md_ro) {
		return -EPERM;
	}

	if (sz == blob->active.num_clusters) {
		return 0;
	}

	rc = _spdk_resize_blob(blob, sz);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

/* END spdk_blob_resize */


/* START spdk_bs_delete_blob */

static void
_spdk_bs_delete_close_cpl(void *cb_arg, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_delete_persist_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob *_blob = cb_arg;
	struct spdk_blob_data *blob = __blob_to_data(_blob);

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
	spdk_blob_close(_blob, _spdk_bs_delete_close_cpl, seq);
}

static void
_spdk_bs_delete_open_cpl(void *cb_arg, struct spdk_blob *_blob, int bserrno)
{
	spdk_bs_sequence_t *seq = cb_arg;
	struct spdk_blob_data *blob = __blob_to_data(_blob);
	uint32_t page_num;

	if (bserrno != 0) {
		spdk_bs_sequence_finish(seq, bserrno);
		return;
	}

	if (blob->open_ref > 1) {
		/*
		 * Someone has this blob open (besides this delete context).
		 *  Decrement the ref count directly and return -EBUSY.
		 */
		blob->open_ref--;
		spdk_bs_sequence_finish(seq, -EBUSY);
		return;
	}

	/*
	 * Remove the blob from the blob_store list now, to ensure it does not
	 *  get returned after this point by _spdk_blob_lookup().
	 */
	TAILQ_REMOVE(&blob->bs->blobs, blob, link);
	page_num = _spdk_bs_blobid_to_page(blob->id);
	spdk_bit_array_clear(blob->bs->used_blobids, page_num);
	blob->state = SPDK_BLOB_STATE_DIRTY;
	blob->active.num_pages = 0;
	_spdk_resize_blob(blob, 0);

	_spdk_blob_persist(seq, blob, _spdk_bs_delete_persist_cpl, _blob);
}

void
spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t 	*seq;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Deleting blob %lu\n", blobid);

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
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
	struct spdk_blob_data *blob = cb_arg;

	/* If the blob have crc error, we just return NULL. */
	if (blob == NULL) {
		seq->cpl.u.blob_handle.blob = NULL;
		spdk_bs_sequence_finish(seq, bserrno);
		return;
	}

	blob->open_ref++;

	TAILQ_INSERT_HEAD(&blob->bs->blobs, blob, link);

	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_data		*blob;
	struct spdk_bs_cpl		cpl;
	spdk_bs_sequence_t		*seq;
	uint32_t			page_num;

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Opening blob %lu\n", blobid);

	page_num = _spdk_bs_blobid_to_page(blobid);
	if (spdk_bit_array_get(bs->used_blobids, page_num) == false) {
		/* Invalid blobid */
		cb_fn(cb_arg, NULL, -ENOENT);
		return;
	}

	blob = _spdk_blob_lookup(bs, blobid);
	if (blob) {
		blob->open_ref++;
		cb_fn(cb_arg, __data_to_blob(blob), 0);
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
	cpl.u.blob_handle.blob = __data_to_blob(blob);

	seq = spdk_bs_sequence_start(bs->md_channel, &cpl);
	if (!seq) {
		_spdk_blob_free(blob);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	_spdk_blob_load(seq, blob, _spdk_bs_open_blob_cpl, blob);
}
/* END spdk_bs_open_blob */

/* START spdk_blob_set_read_only */
void spdk_blob_set_read_only(struct spdk_blob *b)
{
	struct spdk_blob_data *blob = __blob_to_data(b);

	assert(spdk_get_thread() == blob->bs->md_thread);

	blob->data_ro_flags |= SPDK_BLOB_READ_ONLY;

	blob->state = SPDK_BLOB_STATE_DIRTY;
}
/* END spdk_blob_set_read_only */

/* START spdk_blob_sync_md */

static void
_spdk_blob_sync_md_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_data *blob = __blob_to_data(cb_arg);

	if (bserrno == 0 && (blob->data_ro_flags & SPDK_BLOB_READ_ONLY)) {
		blob->data_ro = true;
		blob->md_ro = true;
	}

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_blob_sync_md(struct spdk_blob_data *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	_spdk_blob_persist(seq, blob, _spdk_blob_sync_md_cpl, blob);
}

void
spdk_blob_sync_md(struct spdk_blob *_blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_data	*blob = __blob_to_data(_blob);

	assert(blob != NULL);
	assert(spdk_get_thread() == blob->bs->md_thread);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Syncing blob %lu\n", blob->id);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->md_ro) {
		assert(blob->state == SPDK_BLOB_STATE_CLEAN);
		cb_fn(cb_arg, 0);
		return;
	}

	if (blob->state == SPDK_BLOB_STATE_CLEAN) {
		cb_fn(cb_arg, 0);
		return;
	}

	_spdk_blob_sync_md(blob, cb_fn, cb_arg);
}

/* END spdk_blob_sync_md */

struct spdk_blob_insert_cluster_ctx {
	struct spdk_thread	*thread;
	struct spdk_blob_data	*blob;
	uint32_t		cluster_num;	/* cluster index in blob */
	uint32_t		cluster;	/* cluster on disk */
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
_spdk_blob_insert_cluster_msg(void *arg)
{
	struct spdk_blob_insert_cluster_ctx *ctx = arg;

	ctx->rc = _spdk_blob_insert_cluster(ctx->blob, ctx->cluster_num, ctx->cluster);
	if (ctx->rc != 0) {
		spdk_thread_send_msg(ctx->thread, _spdk_blob_insert_cluster_msg_cpl, ctx);
		return;
	}

	ctx->blob->state = SPDK_BLOB_STATE_DIRTY;
	_spdk_blob_sync_md(ctx->blob, _spdk_blob_insert_cluster_msg_cb, ctx);
}

void
_spdk_blob_insert_cluster_on_md_thread(struct spdk_blob_data *blob, uint32_t cluster_num,
				       uint64_t cluster, spdk_blob_op_complete cb_fn, void *cb_arg)
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
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(blob->bs->md_thread, _spdk_blob_insert_cluster_msg, ctx);
}

/* START spdk_blob_close */

static void
_spdk_blob_close_cpl(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_blob_data *blob = cb_arg;

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

	spdk_bs_sequence_finish(seq, bserrno);
}

void spdk_blob_close(struct spdk_blob *b, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	struct spdk_blob_data	*blob;
	spdk_bs_sequence_t	*seq;

	assert(b != NULL);
	blob = __blob_to_data(b);
	assert(blob != NULL);
	assert(spdk_get_thread() == blob->bs->md_thread);

	SPDK_DEBUGLOG(SPDK_LOG_BLOB, "Closing blob %lu\n", blob->id);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->open_ref == 0) {
		cb_fn(cb_arg, -EBADF);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BLOB_BASIC;
	cpl.u.blob_basic.cb_fn = cb_fn;
	cpl.u.blob_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(blob->bs->md_channel, &cpl);
	if (!seq) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	if (blob->state == SPDK_BLOB_STATE_CLEAN) {
		_spdk_blob_close_cpl(seq, blob, 0);
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

void spdk_bs_io_unmap_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_UNMAP);
}

void spdk_bs_io_write_zeroes_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
				  uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, NULL, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_WRITE_ZEROES);
}

void spdk_bs_io_write_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			   void *payload, uint64_t offset, uint64_t length,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_WRITE);
}

void spdk_bs_io_read_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  void *payload, uint64_t offset, uint64_t length,
			  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	_spdk_blob_request_submit_op(blob, channel, payload, offset, length, cb_fn, cb_arg,
				     SPDK_BLOB_READ);
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
spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *b,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_iter_ctx *ctx;
	struct spdk_blob_data	*blob;

	assert(b != NULL);
	blob = __blob_to_data(b);
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
	spdk_blob_close(b, _spdk_bs_iter_close_cpl, ctx);
}

static int
_spdk_blob_set_xattr(struct spdk_blob_data *blob, const char *name, const void *value,
		     uint16_t value_len, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr 	*xattr;

	assert(blob != NULL);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

	if (blob->md_ro) {
		return -EPERM;
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
		return -1;
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
	return _spdk_blob_set_xattr(__blob_to_data(blob), name, value, value_len, false);
}

static int
_spdk_blob_remove_xattr(struct spdk_blob_data *blob, const char *name, bool internal)
{
	struct spdk_xattr_tailq *xattrs;
	struct spdk_xattr	*xattr;

	assert(blob != NULL);

	assert(blob->state != SPDK_BLOB_STATE_LOADING &&
	       blob->state != SPDK_BLOB_STATE_SYNCING);

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
	return _spdk_blob_remove_xattr(__blob_to_data(blob), name, false);
}

static int
_spdk_blob_get_xattr_value(struct spdk_blob_data *blob, const char *name,
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
	return _spdk_blob_get_xattr_value(__blob_to_data(blob), name, value, value_len, false);
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
spdk_blob_get_xattr_names(struct spdk_blob *_blob, struct spdk_xattr_names **names)
{
	return _spdk_blob_get_xattr_names(&__blob_to_data(_blob)->xattrs, names);
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

SPDK_LOG_REGISTER_COMPONENT("blob", SPDK_LOG_BLOB)
