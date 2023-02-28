/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"
#include "spdk/env.h"

#include "ftl_io.h"
#include "ftl_core.h"

struct ftl_rq *
ftl_rq_new(struct spdk_ftl_dev *dev, uint32_t io_md_size)
{
	struct ftl_rq *rq;
	struct ftl_rq_entry *entry;
	struct iovec *io_vec;
	void *io_payload, *io_md = NULL;
	uint64_t i;
	size_t size;
	uint32_t num_blocks = dev->xfer_size;

	size = sizeof(*rq) + (sizeof(rq->entries[0]) * num_blocks);
	rq = calloc(1, size);
	if (!rq) {
		return NULL;
	}
	rq->dev = dev;
	rq->num_blocks = num_blocks;

	/* Allocate payload for IO and IO vector */
	io_payload = rq->io_payload = spdk_zmalloc(FTL_BLOCK_SIZE * num_blocks,
				      FTL_BLOCK_SIZE, NULL, SPDK_ENV_LCORE_ID_ANY,
				      SPDK_MALLOC_DMA);
	if (!io_payload) {
		goto error;
	}
	rq->io_vec = calloc(num_blocks, sizeof(rq->io_vec[0]));
	if (!rq->io_vec) {
		goto error;
	}
	rq->io_vec_size = num_blocks;

	/* Allocate extended metadata for IO */
	if (io_md_size) {
		rq->io_md_size = io_md_size;
		io_md = rq->io_md = spdk_zmalloc(io_md_size * num_blocks,
						 FTL_BLOCK_SIZE, NULL,
						 SPDK_ENV_LCORE_ID_ANY,
						 SPDK_MALLOC_DMA);
		if (!io_md) {
			goto error;
		}
	}

	entry = rq->entries;
	io_vec = rq->io_vec;
	for (i = 0; i < num_blocks; ++i) {
		uint64_t *index = (uint64_t *)&entry->index;
		*index = i;

		entry->addr = FTL_ADDR_INVALID;
		entry->lba = FTL_LBA_INVALID;
		entry->io_payload = io_payload;
		entry->seq_id = 0;

		if (io_md_size) {
			entry->io_md = io_md;
		}

		io_vec->iov_base = io_payload;
		io_vec->iov_len = FTL_BLOCK_SIZE;

		entry++;
		io_vec++;
		io_payload += FTL_BLOCK_SIZE;
		io_md += io_md_size;
	}

	return rq;
error:
	ftl_rq_del(rq);
	return NULL;
}

void
ftl_rq_del(struct ftl_rq *rq)
{
	if (!rq) {
		return;
	}

	spdk_free(rq->io_payload);
	spdk_free(rq->io_md);
	free(rq->io_vec);

	free(rq);
}

void
ftl_rq_unpin(struct ftl_rq *rq)
{
	struct ftl_l2p_pin_ctx *pin_ctx;
	uint64_t i;

	for (i = 0; i < rq->iter.count; i++) {
		pin_ctx = &rq->entries[i].l2p_pin_ctx;
		if (pin_ctx->lba != FTL_LBA_INVALID) {
			ftl_l2p_unpin(rq->dev, pin_ctx->lba, pin_ctx->count);
		}
	}
}
