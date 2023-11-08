/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_layout_tracker_bdev.h"
#include "spdk/util.h"

#define REG_VER_ANY	UINT32_MAX

struct layout_tracker_entry {
	TAILQ_ENTRY(layout_tracker_entry) layout_entry;
	struct ftl_layout_tracker_bdev_region_props reg;
};

struct ftl_layout_tracker_bdev {
	TAILQ_HEAD(layout_tracker, layout_tracker_entry) layout_head;
	uint64_t bdev_blks;
	uint32_t regs_cnt;
};

struct layout_tracker_blob_entry {
	/* Region type */
	uint32_t type;

	/* Region version */
	uint32_t ver;

	/* Region offset in blocks */
	uint64_t blk_offs;

	/* Region size in blocks */
	uint64_t blk_sz;
} __attribute__((packed));

static int
layout_tracker_init_entries(struct ftl_layout_tracker_bdev *tracker, uint64_t bdev_blks)
{
	struct layout_tracker_entry *entry_free = calloc(1, sizeof(*entry_free));

	if (!entry_free) {
		return -ENOMEM;
	}

	assert(tracker);
	assert(tracker->regs_cnt == 0);

	tracker->bdev_blks = bdev_blks;
	tracker->regs_cnt = 1;
	TAILQ_INIT(&tracker->layout_head);

	entry_free->reg.blk_sz = bdev_blks;
	entry_free->reg.type = FTL_LAYOUT_REGION_TYPE_FREE;

	TAILQ_INSERT_HEAD(&tracker->layout_head, entry_free, layout_entry);
	return 0;
}

struct ftl_layout_tracker_bdev *
ftl_layout_tracker_bdev_init(uint64_t bdev_blks)
{
	struct ftl_layout_tracker_bdev *tracker = calloc(1, sizeof(*tracker));

	if (!tracker) {
		return NULL;
	}

	if (layout_tracker_init_entries(tracker, bdev_blks)) {
		free(tracker);
		return NULL;
	}

	return tracker;
}

static void
layout_tracker_free_entries(struct ftl_layout_tracker_bdev *tracker)
{
	struct layout_tracker_entry *entry;

	while ((entry = TAILQ_FIRST(&tracker->layout_head))) {
		TAILQ_REMOVE(&tracker->layout_head, entry, layout_entry);
		free(entry);
	}
	tracker->regs_cnt = 0;
}

void
ftl_layout_tracker_bdev_fini(struct ftl_layout_tracker_bdev *tracker)
{
	assert(tracker);
	layout_tracker_free_entries(tracker);
	free(tracker);
}

static struct layout_tracker_entry *
layout_region_find_min_free(struct ftl_layout_tracker_bdev *tracker, uint64_t blk_sz,
			    uint64_t blk_align)
{
	struct layout_tracker_entry *min_free_entry = NULL;
	struct layout_tracker_entry *entry;

	assert(tracker);

	TAILQ_FOREACH(entry, &tracker->layout_head, layout_entry) {
		uint64_t align_offs, align_sz;

		if (entry->reg.type != FTL_LAYOUT_REGION_TYPE_FREE) {
			continue;
		}

		align_offs = entry->reg.blk_offs;
		align_sz = entry->reg.blk_sz;
		if (blk_align) {
			align_offs = SPDK_ALIGN_CEIL(align_offs, blk_align);
			align_sz -= (align_offs - entry->reg.blk_offs);
		}

		if (align_sz >= blk_sz) {
			if (!min_free_entry || min_free_entry->reg.blk_sz > entry->reg.blk_sz) {
				min_free_entry = entry;
			}
		}
	}

	return min_free_entry;
}

static struct layout_tracker_entry *
layout_region_find_from(struct ftl_layout_tracker_bdev *tracker,
			enum ftl_layout_region_type reg_type,
			uint32_t reg_ver, struct layout_tracker_entry *entry)
{
	assert(tracker);

	TAILQ_FOREACH_FROM(entry, &tracker->layout_head, layout_entry) {
		if ((entry->reg.type == reg_type || reg_type == FTL_LAYOUT_REGION_TYPE_INVALID)
		    && (entry->reg.ver == reg_ver || reg_ver == REG_VER_ANY)) {
			return entry;
		}
	}

	return NULL;
}

static struct layout_tracker_entry *
layout_region_find_first(struct ftl_layout_tracker_bdev *tracker,
			 enum ftl_layout_region_type reg_type,
			 uint32_t reg_ver)
{
	return layout_region_find_from(tracker, reg_type, reg_ver, TAILQ_FIRST(&tracker->layout_head));
}

static struct layout_tracker_entry *
layout_region_find_next(struct ftl_layout_tracker_bdev *tracker,
			enum ftl_layout_region_type reg_type,
			uint32_t reg_ver, struct layout_tracker_entry *entry)
{
	if ((entry = TAILQ_NEXT(entry, layout_entry))) {
		return layout_region_find_from(tracker, reg_type, reg_ver, entry);
	}
	return NULL;
}

const struct ftl_layout_tracker_bdev_region_props *
ftl_layout_tracker_bdev_add_region(struct ftl_layout_tracker_bdev *tracker,
				   enum ftl_layout_region_type reg_type, uint32_t reg_ver, uint64_t blk_sz, uint64_t blk_align)
{
	struct layout_tracker_entry *entry_free;
	struct layout_tracker_entry *entry_new;
	uint64_t entry_free_blks_left;

	assert(tracker);
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);

	entry_new = layout_region_find_first(tracker, reg_type, reg_ver);
	if (entry_new) {
		/* Region already exists */
		return NULL;
	}

	entry_free = layout_region_find_min_free(tracker, blk_sz, blk_align);
	if (!entry_free) {
		/* No free space */
		return NULL;
	}

	/* Takce care of the alignment */
	if (blk_align) {
		/* Calculate the aligned region's offs and size */
		uint64_t align_offs = SPDK_ALIGN_CEIL(entry_free->reg.blk_offs, blk_align);
		assert(align_offs >= entry_free->reg.blk_offs);

		/* Subdivide the free region in two: unaligned free region, followed by the aligned free region */
		if (align_offs > entry_free->reg.blk_offs) {
			uint64_t unaligned_sz = align_offs - entry_free->reg.blk_offs;

			/* Setup the unaligned region */
			entry_new = calloc(1, sizeof(*entry_new));
			if (!entry_new) {
				return NULL;
			}
			entry_new->reg = entry_free->reg;
			entry_new->reg.blk_sz = unaligned_sz;

			/* Setup the aligned region - shrink the free region found */
			entry_free->reg.blk_offs = align_offs;
			entry_free->reg.blk_sz -= unaligned_sz;

			/* Add the unaligned region prev to the aligned one */
			TAILQ_INSERT_BEFORE(entry_free, entry_new, layout_entry);
			tracker->regs_cnt++;
		}
	}

	entry_free_blks_left = entry_free->reg.blk_sz - blk_sz;

	if (entry_free_blks_left) {
		/* Subdivide the free region */
		entry_new = calloc(1, sizeof(*entry_new));
		if (!entry_new) {
			return NULL;
		}

		/* Setup the new region at the beginning of the free region found */
		entry_new->reg.type = reg_type;
		entry_new->reg.ver = reg_ver;
		entry_new->reg.blk_offs = entry_free->reg.blk_offs;
		entry_new->reg.blk_sz = blk_sz;

		/* Shrink the free region found */
		entry_free->reg.blk_offs += blk_sz;
		entry_free->reg.blk_sz = entry_free_blks_left;

		/* Add the new region */
		TAILQ_INSERT_BEFORE(entry_free, entry_new, layout_entry);
		tracker->regs_cnt++;
	} else {
		/* Setup the new region in place */
		entry_new = entry_free;
		entry_new->reg.type = reg_type;
		entry_new->reg.ver = reg_ver;
	}

	return &entry_new->reg;
}

const struct ftl_layout_tracker_bdev_region_props *
ftl_layout_tracker_bdev_insert_region(struct ftl_layout_tracker_bdev *tracker,
				      enum ftl_layout_region_type reg_type, uint32_t reg_ver,
				      uint64_t blk_offs, uint64_t blk_sz)
{
	struct layout_tracker_entry *entry_free;
	struct layout_tracker_entry *entry_new;
	uint64_t entry_free_blks_left;

	assert(tracker);

	if (reg_type >= FTL_LAYOUT_REGION_TYPE_MAX && reg_type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		/* Invalid region type */
		return NULL;
	}

	entry_new = layout_region_find_first(tracker, reg_type, reg_ver);
	if (entry_new) {
		/* Region already exists */
		return NULL;
	}

	/* Look up for the free region corresponding to the blk_offs */
	for (entry_free = layout_region_find_first(tracker, FTL_LAYOUT_REGION_TYPE_FREE, REG_VER_ANY);
	     entry_free;
	     entry_free = layout_region_find_next(tracker, FTL_LAYOUT_REGION_TYPE_FREE, REG_VER_ANY,
			     entry_free)) {
		/* Test if the region being added fits into the free region */
		if (entry_free->reg.blk_offs <= blk_offs
		    && blk_offs + blk_sz <= entry_free->reg.blk_offs + entry_free->reg.blk_sz) {
			break;
		}
	}

	if (!entry_free) {
		/* Did not found the corresponding free region */
		return NULL;
	}

	entry_free_blks_left = blk_offs - entry_free->reg.blk_offs;
	if (entry_free_blks_left) {
		/* Subdivide the free region */
		entry_new = calloc(1, sizeof(*entry_new));
		if (!entry_new) {
			return NULL;
		}

		/* Setup another free region at the beginning of the free region found */
		entry_new->reg.type = FTL_LAYOUT_REGION_TYPE_FREE;
		entry_new->reg.blk_offs = entry_free->reg.blk_offs;
		entry_new->reg.blk_sz = entry_free_blks_left;

		/* Shrink the free region found */
		entry_free->reg.blk_offs += entry_free_blks_left;
		assert(entry_free->reg.blk_sz > entry_free_blks_left);
		entry_free->reg.blk_sz -= entry_free_blks_left;

		/* Add the new free region */
		TAILQ_INSERT_BEFORE(entry_free, entry_new, layout_entry);
		tracker->regs_cnt++;
	}

	assert(entry_free->reg.blk_offs == blk_offs);
	assert(blk_sz <= entry_free->reg.blk_sz);

	entry_free_blks_left = entry_free->reg.blk_sz - blk_sz;
	if (entry_free_blks_left) {
		/* Subdivide the free region */
		entry_new = calloc(1, sizeof(*entry_new));
		if (!entry_new) {
			return NULL;
		}

		/* Setup the new region at the beginning of the free region found */
		entry_new->reg.type = reg_type;
		entry_new->reg.ver = reg_ver;
		entry_new->reg.blk_offs = entry_free->reg.blk_offs;
		entry_new->reg.blk_sz = blk_sz;

		/* Shrink the free region found */
		entry_free->reg.blk_offs += blk_sz;
		entry_free->reg.blk_sz = entry_free_blks_left;

		/* Add the new region */
		TAILQ_INSERT_BEFORE(entry_free, entry_new, layout_entry);
		tracker->regs_cnt++;
	} else {
		/* Setup the new region in place */
		entry_new = entry_free;
		entry_new->reg.type = reg_type;
		entry_new->reg.ver = reg_ver;
	}

	return &entry_new->reg;
}

int
ftl_layout_tracker_bdev_rm_region(struct ftl_layout_tracker_bdev *tracker,
				  enum ftl_layout_region_type reg_type, uint32_t reg_ver)
{
	struct layout_tracker_entry *entry_rm, *entry_check __attribute__((unused));
	struct layout_tracker_entry *entry = layout_region_find_first(tracker, reg_type, reg_ver);

	if (!entry) {
		return -1;
	}

	/* Free the region */
	entry->reg.type = FTL_LAYOUT_REGION_TYPE_FREE;
	entry->reg.ver = 0;

	/* Join with the adjacent free region prev to the current region */
	entry_rm = TAILQ_PREV(entry, layout_tracker, layout_entry);
	if (entry_rm && entry_rm->reg.type == FTL_LAYOUT_REGION_TYPE_FREE) {
		TAILQ_REMOVE(&tracker->layout_head, entry_rm, layout_entry);
		entry->reg.blk_offs = entry_rm->reg.blk_offs;
		entry->reg.blk_sz += entry_rm->reg.blk_sz;

#if defined(DEBUG)
		entry_check = TAILQ_PREV(entry, layout_tracker, layout_entry);
		if (entry_check) {
			assert(entry_check->reg.type != FTL_LAYOUT_REGION_TYPE_FREE);
		}
#endif

		free(entry_rm);
		tracker->regs_cnt--;
	}

	/* Join with the adjacent free region next to the current region */
	entry_rm = TAILQ_NEXT(entry, layout_entry);
	if (entry_rm && entry_rm->reg.type == FTL_LAYOUT_REGION_TYPE_FREE) {
		TAILQ_REMOVE(&tracker->layout_head, entry_rm, layout_entry);
		entry->reg.blk_sz += entry_rm->reg.blk_sz;

#if defined(DEBUG)
		entry_check = TAILQ_NEXT(entry, layout_entry);
		if (entry_check) {
			assert(entry_check->reg.type != FTL_LAYOUT_REGION_TYPE_FREE);
		}
#endif
		free(entry_rm);
		tracker->regs_cnt--;
	}

	return 0;
}

void
ftl_layout_tracker_bdev_find_next_region(struct ftl_layout_tracker_bdev *tracker,
		enum ftl_layout_region_type reg_type,
		const struct ftl_layout_tracker_bdev_region_props **search_ctx)
{
	struct layout_tracker_entry *entry;

	if (!search_ctx) {
		return;
	}

	if (*search_ctx == NULL) {
		/* Return the first region found */
		entry = layout_region_find_first(tracker, reg_type, REG_VER_ANY);
	} else {
		/* Find the next region */
		entry = SPDK_CONTAINEROF(*search_ctx, struct layout_tracker_entry, reg);
		entry = layout_region_find_next(tracker, reg_type, REG_VER_ANY, entry);
	}
	*search_ctx = entry ? &entry->reg : NULL;
}

size_t
ftl_layout_tracker_bdev_blob_store(struct ftl_layout_tracker_bdev *tracker, void *blob_buf,
				   size_t blob_buf_sz)
{
	struct layout_tracker_blob_entry *blob_entry = blob_buf;
	struct layout_tracker_entry *entry;
	size_t blob_sz = 0;

	assert(tracker);

	TAILQ_FOREACH(entry, &tracker->layout_head, layout_entry) {
		if (blob_sz + sizeof(*blob_entry) > blob_buf_sz) {
			/* Ran out of buf space */
			assert(false);
			return 0;
		}

		/* Skip the free space entries */
		if (entry->reg.type == FTL_LAYOUT_REGION_TYPE_FREE) {
			continue;
		}

		/* Store the entry */
		blob_entry->type = entry->reg.type;
		blob_entry->ver = entry->reg.ver;
		blob_entry->blk_offs = entry->reg.blk_offs;
		blob_entry->blk_sz = entry->reg.blk_sz;

		/* Move to the next entry */
		blob_entry++;
		blob_sz += sizeof(*blob_entry);
	}

	return blob_sz;
}

int
ftl_layout_tracker_bdev_blob_load(struct ftl_layout_tracker_bdev *tracker, void *blob_buf,
				  size_t blob_sz)
{
	struct layout_tracker_blob_entry *blob_entry = blob_buf;
	size_t blob_entry_num = blob_sz / sizeof(*blob_entry);
	struct layout_tracker_blob_entry *blob_entry_end = blob_entry + blob_entry_num;

	if (blob_sz % sizeof(*blob_entry) != 0) {
		/* Invalid blob size */
		return -1;
	}

	/* Free the current MD layout tracking info */
	layout_tracker_free_entries(tracker);

	/* Reinit MD layout tracking info */
	if (layout_tracker_init_entries(tracker, tracker->bdev_blks)) {
		return -1;
	}

	for (; blob_entry < blob_entry_end; blob_entry++) {
		/* Verify the type */
		if (blob_entry->type == FTL_LAYOUT_REGION_TYPE_FREE) {
			return -1;
		}

		/* Load the entry */
		if (!ftl_layout_tracker_bdev_insert_region(tracker, blob_entry->type, blob_entry->ver,
				blob_entry->blk_offs, blob_entry->blk_sz)) {
			return -1;
		}
	}

	return 0;
}
