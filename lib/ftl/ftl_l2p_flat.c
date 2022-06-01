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

#include "ftl_l2p.h"
#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_utils.h"
#include "ftl_l2p_flat.h"
#include "utils/ftl_addr_utils.h"

static struct ftl_md *get_l2p_md(struct spdk_ftl_dev *dev)
{
	return dev->layout.md[ftl_layout_region_type_l2p];
}

struct ftl_l2p_flat {
	void *l2p;
	/* Size of pages mmapped for l2p, valid only for mapping on persistent memory */
	size_t l2p_pmem_len;
	bool is_halted;
};

static void
ftl_l2p_flat_lba_persist(const struct spdk_ftl_dev *dev, uint64_t lba)
{
#ifdef SPDK_CONFIG_PMDK
	struct ftl_l2p_flat *l2p_flat = dev->l2p;
	size_t ftl_addr_size = ftl_addr_packed(dev) ? 4 : 8;
	pmem_persist((char *)l2p_flat->l2p + (lba * ftl_addr_size), ftl_addr_size);
#else /* SPDK_CONFIG_PMDK */
	FTL_ERRLOG(dev, "Libpmem not available, cannot flush l2p to pmem\n");
	assert(0);
#endif /* SPDK_CONFIG_PMDK */
}

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

	if (l2p_flat->l2p_pmem_len != 0) {
		ftl_l2p_flat_lba_persist(dev, lba);
	}
}

ftl_addr
ftl_l2p_flat_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	assert(dev->num_lbas > lba);

	return ftl_addr_load(dev, l2p_flat->l2p, lba);
}

static void md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	ftl_l2p_cb cb = md->owner.cb;
	void *cb_cntx = md->owner.cb_ctx;

	cb(dev, status, cb_cntx);
}

void
ftl_l2p_flat_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	if (l2p_flat->l2p_pmem_len != 0) {
#ifdef SPDK_CONFIG_PMDK
		size_t addr_size = dev->layout.l2p.addr_size;//use new layout get addr_size
		size_t l2p_size = dev->num_lbas * addr_size;

		pmem_memset_persist(l2p_flat->l2p, (int)FTL_ADDR_INVALID, l2p_size);
#endif
		cb(dev, 0, cb_ctx);
	} else {
		memset(l2p_flat->l2p, (int)FTL_ADDR_INVALID,
		       ftl_md_get_buffer_size(get_l2p_md(dev)));

		struct ftl_md *md = get_l2p_md(dev);
		md->cb = md_cb;
		md->owner.cb_ctx = cb_ctx;
		md->owner.cb = cb;
		ftl_md_persist(md);
	}
}

void
ftl_l2p_flat_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p;

	if (l2p_flat->l2p_pmem_len != 0) {
		cb(dev, 0, cb_ctx);
		return;
	}

	struct ftl_md *md = get_l2p_md(dev);
	md->cb = md_cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.cb = cb;
	ftl_md_persist(md);
}

static int
ftl_l2p_flat_init_pmem(struct spdk_ftl_dev *dev, struct ftl_l2p_flat *l2p_flat, size_t l2p_size,
		       const char *l2p_path)
{
#ifdef SPDK_CONFIG_PMDK
	int is_pmem;

	if ((l2p_flat->l2p = pmem_map_file(l2p_path, 0,
					   0, 0, &l2p_flat->l2p_pmem_len, &is_pmem)) == NULL) {
		FTL_ERRLOG(dev, "Failed to mmap l2p_path\n");
		return -1;
	}

	if (!is_pmem) {
		FTL_NOTICELOG(dev, "l2p_path mapped on non-pmem device\n");
	}

	if (l2p_flat->l2p_pmem_len < l2p_size) {
		FTL_ERRLOG(dev, "l2p_path file is too small\n");
		return -1;
	}

	return 0;
#else /* SPDK_CONFIG_PMDK */
	FTL_ERRLOG(dev, "Libpmem not available, cannot use pmem l2p_path\n");
	return -1;
#endif /* SPDK_CONFIG_PMDK */
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

static void
ftl_l2p_flat_deinit_dram(struct spdk_ftl_dev *dev, struct ftl_l2p_flat *l2p_flat)
{
}

int
ftl_l2p_flat_init(struct spdk_ftl_dev *dev)
{
	size_t l2p_size = dev->num_lbas * dev->layout.l2p.addr_size;
	const char *l2p_path = dev->conf.l2p_path;
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

	if (l2p_path) {
		ret = ftl_l2p_flat_init_pmem(dev, l2p_flat, l2p_size, l2p_path);
	} else {
		ret = ftl_l2p_flat_init_dram(dev, l2p_flat, l2p_size);
	}

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

	if (l2p_flat->l2p_pmem_len != 0) {
#ifdef SPDK_CONFIG_PMDK
		pmem_unmap(l2p_flat->l2p, l2p_flat->l2p_pmem_len);
#endif /* SPDK_CONFIG_PMDK */
	} else {
		ftl_l2p_flat_deinit_dram(dev, l2p_flat);
	}
	free(l2p_flat);

	dev->l2p = NULL;
}

void
ftl_l2p_flat_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
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
