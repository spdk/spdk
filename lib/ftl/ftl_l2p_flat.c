/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_l2p.h"
#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_utils.h"
#include "ftl_l2p_flat.h"
#include "utils/ftl_addr_utils.h"

static struct ftl_md *
get_l2p_md(struct spdk_ftl_dev *dev)
{
	return dev->layout.md[FTL_LAYOUT_REGION_TYPE_L2P];
}

struct ftl_l2p_flat {
	void *l2p;
	bool is_halted;
};

void
ftl_l2p_flat_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx)
{
	assert(dev->num_lbas >= pin_ctx->lba + pin_ctx->count);

	ftl_l2p_pin_complete(dev, 0, pin_ctx);
}

void
ftl_l2p_flat_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	assert(dev->num_lbas >= lba + count);
}

void
ftl_l2p_flat_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	assert(dev->num_lbas > lba);

	ftl_addr_store(dev, l2p_flat->l2p, lba, addr);
}

ftl_addr
ftl_l2p_flat_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	assert(dev->num_lbas > lba);

	return ftl_addr_load(dev, l2p_flat->l2p, lba);
}

static void
md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	ftl_l2p_cb cb = md->owner.private;
	void *cb_ctx = md->owner.cb_ctx;

	cb(dev, status, cb_ctx);
}

void
ftl_l2p_flat_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;
	struct ftl_md *md;

	memset(l2p_flat->l2p, (int)FTL_ADDR_INVALID,
	       ftl_md_get_buffer_size(get_l2p_md(dev)));

	md = get_l2p_md(dev);
	md->cb = md_cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.private = cb;
	ftl_md_persist(md);
}

void
ftl_l2p_flat_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_md *md;

	md = get_l2p_md(dev);
	md->cb = md_cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.private = cb;
	ftl_md_restore(md);
}

void
ftl_l2p_flat_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_md *md;

	md = get_l2p_md(dev);
	md->cb = md_cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.private = cb;
	ftl_md_persist(md);
}

static int
ftl_l2p_flat_init_dram(struct spdk_ftl_dev *dev, struct ftl_l2p_flat *l2p_flat,
		       size_t l2p_size)
{
	struct ftl_md *md = get_l2p_md(dev);

	assert(ftl_md_get_buffer_size(md) >= l2p_size);

	l2p_flat->l2p = ftl_md_get_buffer(md);
	if (!l2p_flat->l2p) {
		FTL_ERRLOG(dev, "Failed to allocate l2p table\n");
		return -1;
	}

	return 0;
}

int
ftl_l2p_flat_init(struct spdk_ftl_dev *dev)
{
	size_t l2p_size = dev->num_lbas * dev->layout.l2p.addr_size;
	struct ftl_l2p_flat *l2p_flat;
	int ret;

	if (dev->num_lbas == 0) {
		FTL_ERRLOG(dev, "Invalid l2p table size\n");
		return -1;
	}

	if (dev->l2p) {
		FTL_ERRLOG(dev, "L2p table already allocated\n");
		return -1;
	}

	l2p_flat = calloc(1, sizeof(*l2p_flat));
	if (!l2p_flat) {
		FTL_ERRLOG(dev, "Failed to allocate l2p_flat\n");
		return -1;
	}

	ret = ftl_l2p_flat_init_dram(dev, l2p_flat, l2p_size);

	if (ret) {
		free(l2p_flat);
		return ret;
	}

	dev->l2p = l2p_flat;
	return 0;
}

void
ftl_l2p_flat_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	if (!l2p_flat) {
		return;
	}

	free(l2p_flat);

	dev->l2p = NULL;
}

void
ftl_l2p_flat_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	cb(dev, 0, cb_ctx);
}

void
ftl_l2p_flat_process(struct spdk_ftl_dev *dev)
{
}

bool
ftl_l2p_flat_is_halted(struct spdk_ftl_dev *dev)
{
	return true;
}

void
ftl_l2p_flat_halt(struct spdk_ftl_dev *dev)
{
}

void
ftl_l2p_flat_resume(struct spdk_ftl_dev *dev)
{
}
