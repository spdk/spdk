/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_core.h"
#include "ftl_io.h"
#include "ftl_layout.h"
#include "utils/ftl_defs.h"

struct ftl_pl2_log_item {
	uint64_t lba;
	uint64_t num_blocks;
	uint64_t seq_id;
	ftl_addr addr;
};
#define FTL_P2L_LOG_ITEMS_IN_PAGE ((FTL_BLOCK_SIZE - sizeof(union ftl_md_vss)) / sizeof(struct ftl_pl2_log_item))
#define FTL_P2L_LOG_PAGE_COUNT_DEFAULT 128

struct ftl_p2l_log_page {
	union ftl_md_vss hdr;
	struct ftl_pl2_log_item items[FTL_P2L_LOG_ITEMS_IN_PAGE];
};
SPDK_STATIC_ASSERT(sizeof(struct ftl_p2l_log_page) == FTL_BLOCK_SIZE, "Invalid size of P2L page");

struct ftl_p2l_log_page_ctrl {
	struct ftl_p2l_log_page page;
	struct ftl_p2l_log *p2l;
	uint64_t entry_idx;
	TAILQ_HEAD(, ftl_io) ios;
	struct ftl_md_io_entry_ctx md_ctx;
};

struct ftl_p2l_log {
	struct spdk_ftl_dev		*dev;
	TAILQ_ENTRY(ftl_p2l_log)	link;
	TAILQ_HEAD(, ftl_io)		ios;
	struct ftl_md			*md;
	uint64_t			seq_id;
	struct ftl_mempool		*page_pool;
	uint64_t			entry_idx;
	uint64_t			entry_max;
	ftl_p2l_log_cb			cb_fn;
	uint32_t			ref_cnt;
	bool				in_use;

	struct {
		spdk_ftl_fn cb_fn;
		void *cb_arg;
		ftl_p2l_log_rd_cb cb_rd;
		uint64_t qd;
		uint64_t idx;
		uint64_t seq_id;
		int result;
	} read_ctx;
};

static void p2l_log_page_io(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl);
static void ftl_p2l_log_read_process(struct ftl_p2l_log *p2l);

static struct ftl_p2l_log *
p2l_log_create(struct spdk_ftl_dev *dev, uint32_t region_type)
{
	struct ftl_p2l_log *p2l;

	p2l = calloc(1, sizeof(struct ftl_p2l_log));
	if (!p2l) {
		return NULL;
	}

	TAILQ_INIT(&p2l->ios);
	p2l->dev = dev;
	p2l->md = dev->layout.md[region_type];
	p2l->entry_max = ftl_md_get_buffer_size(p2l->md) / FTL_BLOCK_SIZE;
	p2l->page_pool = ftl_mempool_create(FTL_P2L_LOG_PAGE_COUNT_DEFAULT,
					    sizeof(struct ftl_p2l_log_page_ctrl),
					    FTL_BLOCK_SIZE, SPDK_ENV_SOCKET_ID_ANY);
	if (!p2l->page_pool) {
		goto ERROR;
	}

	return p2l;
ERROR:
	free(p2l);
	return NULL;
}

static void
p2l_log_destroy(struct ftl_p2l_log *p2l)
{
	if (!p2l) {
		return;
	}

	ftl_mempool_destroy(p2l->page_pool);
	free(p2l);
}

static struct ftl_p2l_log_page_ctrl *
p2l_log_get_page(struct ftl_p2l_log *p2l)
{
	struct ftl_p2l_log_page_ctrl *ctrl;

	ctrl = ftl_mempool_get(p2l->page_pool);
	if (!ctrl) {
		return NULL;
	}

	/* Initialize P2L header */
	ctrl->page.hdr.p2l_ckpt.seq_id = p2l->seq_id;
	ctrl->page.hdr.p2l_ckpt.count = 0;
	ctrl->page.hdr.p2l_ckpt.p2l_checksum = 0;
	ctrl->page.hdr.p2l_ckpt.idx = ctrl->entry_idx = p2l->entry_idx;

	/* Initialize the page control structure */
	ctrl->p2l = p2l;
	TAILQ_INIT(&ctrl->ios);

	/* Increase P2L page index */
	p2l->entry_idx++;

	/* Check if the index exceeding the buffer size */
	ftl_bug(p2l->entry_idx > p2l->entry_max);

	return ctrl;
}

static bool
l2p_log_page_is_full(struct ftl_p2l_log_page_ctrl *ctrl)
{
	return ctrl->page.hdr.p2l_ckpt.count == FTL_P2L_LOG_ITEMS_IN_PAGE;
}

static void
p2l_log_page_free(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl)
{
	ftl_mempool_put(p2l->page_pool, ctrl);
}

static void
p2l_log_handle_io_error(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl)
{
#ifdef SPDK_FTL_RETRY_ON_ERROR
	p2l_log_page_io(p2l, ctrl);
#else
	ftl_abort();
#endif
}

static uint32_t
p2l_log_page_crc(struct ftl_p2l_log_page *page)
{
	uint32_t crc = 0;
	void *buffer = page;
	size_t size = sizeof(*page);
	size_t offset = offsetof(struct ftl_p2l_log_page, hdr.p2l_ckpt.p2l_checksum);

	crc = spdk_crc32c_update(buffer, offset, crc);
	buffer += offset + sizeof(page->hdr.p2l_ckpt.p2l_checksum);
	size -= offset + sizeof(page->hdr.p2l_ckpt.p2l_checksum);

	return spdk_crc32c_update(buffer, size, crc);
}

static void
p2l_log_page_io_cb(int status, void *arg)
{
	struct ftl_p2l_log_page_ctrl *ctrl = arg;
	struct ftl_p2l_log *p2l = ctrl->p2l;
	struct ftl_io *io;

	if (status) {
		p2l_log_handle_io_error(p2l, ctrl);
		return;
	}

	while ((io = TAILQ_FIRST(&ctrl->ios))) {
		TAILQ_REMOVE(&ctrl->ios, io, queue_entry);
		p2l->cb_fn(io);
	}

	p2l_log_page_free(p2l, ctrl);
}

static void
p2l_log_page_io(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl)
{
	ctrl->page.hdr.p2l_ckpt.p2l_checksum = p2l_log_page_crc(&ctrl->page);

	ftl_md_persist_entries(p2l->md, ctrl->page.hdr.p2l_ckpt.idx, 1, &ctrl->page, NULL,
			       p2l_log_page_io_cb,
			       ctrl, &ctrl->md_ctx);
}

static void
p2l_log_add_io(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl, struct ftl_io *io)
{
	uint64_t i = ctrl->page.hdr.p2l_ckpt.count++;

	assert(i < FTL_P2L_LOG_ITEMS_IN_PAGE);
	ctrl->page.items[i].lba = io->lba;
	ctrl->page.items[i].num_blocks = io->num_blocks;
	ctrl->page.items[i].seq_id = io->nv_cache_chunk->md->seq_id;
	ctrl->page.items[i].addr = io->addr;

	/* TODO Make sure P2L map is updated respectively */

	TAILQ_REMOVE(&p2l->ios, io, queue_entry);
	TAILQ_INSERT_TAIL(&ctrl->ios, io, queue_entry);
}

void
ftl_p2l_log_io(struct ftl_p2l_log *p2l, struct ftl_io *io)
{
	TAILQ_INSERT_TAIL(&p2l->ios, io, queue_entry);
}

static void
p2l_log_flush(struct ftl_p2l_log *p2l)
{
	struct ftl_p2l_log_page_ctrl *ctrl = NULL;
	struct ftl_io *io;

	while ((io = TAILQ_FIRST(&p2l->ios))) {
		if (!ctrl) {
			ctrl = p2l_log_get_page(p2l);
			if (!ctrl) {
				/* No page at the moment, try next time */
				break;
			}
		}

		p2l_log_add_io(p2l, ctrl, io);

		if (l2p_log_page_is_full(ctrl)) {
			p2l_log_page_io(p2l, ctrl);
			ctrl = NULL;
		}
	}

	if (ctrl) {
		p2l_log_page_io(p2l, ctrl);
	}
}

void
ftl_p2l_log_flush(struct spdk_ftl_dev *dev)
{
	struct ftl_p2l_log *p2l;

	TAILQ_FOREACH(p2l, &dev->p2l_ckpt.log.inuse, link) {
		p2l_log_flush(p2l);
	}
}

uint64_t
ftl_p2l_log_get_md_blocks_required(struct spdk_ftl_dev *dev, uint64_t write_unit_blocks,
				   uint64_t max_user_data_blocks)
{
	return spdk_divide_round_up(max_user_data_blocks, write_unit_blocks);
}

int
ftl_p2l_log_init(struct spdk_ftl_dev *dev)
{
	struct ftl_p2l_log *p2l;
	uint32_t region_type;

	TAILQ_INIT(&dev->p2l_ckpt.log.free);
	TAILQ_INIT(&dev->p2l_ckpt.log.inuse);

	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX;
	     region_type++) {
		p2l = p2l_log_create(dev, region_type);
		if (!p2l) {
			return -ENOMEM;
		}

		TAILQ_INSERT_TAIL(&dev->p2l_ckpt.log.free, p2l, link);
	}

	return 0;
}

void
ftl_p2l_log_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_p2l_log *p2l, *p2l_next;

	TAILQ_FOREACH_SAFE(p2l, &dev->p2l_ckpt.log.free, link, p2l_next) {
		TAILQ_REMOVE(&dev->p2l_ckpt.log.free, p2l, link);
		p2l_log_destroy(p2l);
	}

	TAILQ_FOREACH_SAFE(p2l, &dev->p2l_ckpt.log.inuse, link, p2l_next) {
		TAILQ_REMOVE(&dev->p2l_ckpt.log.inuse, p2l, link);
		p2l_log_destroy(p2l);
	}
}

enum ftl_layout_region_type
ftl_p2l_log_type(struct ftl_p2l_log *p2l) {
	return p2l->md->region->type;
}

struct ftl_p2l_log *
ftl_p2l_log_acquire(struct spdk_ftl_dev *dev, uint64_t seq_id, ftl_p2l_log_cb cb)
{
	struct ftl_p2l_log *p2l;

	p2l = TAILQ_FIRST(&dev->p2l_ckpt.log.free);
	assert(p2l);
	TAILQ_REMOVE(&dev->p2l_ckpt.log.free, p2l, link);
	TAILQ_INSERT_TAIL(&dev->p2l_ckpt.log.inuse, p2l, link);

	p2l->entry_idx = 0;
	p2l->seq_id = seq_id;
	p2l->cb_fn = cb;

	return p2l;
}

void
ftl_p2l_log_release(struct spdk_ftl_dev *dev, struct ftl_p2l_log *p2l)
{
	assert(p2l);

	/* TODO: Add assert if no ongoing IOs on the P2L log */
	/* TODO: Add assert if the P2L log already open */

	TAILQ_REMOVE(&dev->p2l_ckpt.log.inuse, p2l, link);
	TAILQ_INSERT_TAIL(&dev->p2l_ckpt.log.free, p2l, link);
}

static struct ftl_p2l_log *
p2l_log_get(struct spdk_ftl_dev *dev, enum ftl_layout_region_type type)
{
	struct ftl_p2l_log *p2l_Log;

	TAILQ_FOREACH(p2l_Log, &dev->p2l_ckpt.log.free, link) {
		if (type == p2l_Log->md->region->type) {
			return p2l_Log;
		}
	}

	TAILQ_FOREACH(p2l_Log, &dev->p2l_ckpt.log.inuse, link) {
		if (type == p2l_Log->md->region->type) {
			return p2l_Log;
		}
	}

	return NULL;
}

static bool
p2l_log_read_in_progress(struct ftl_p2l_log *p2l)
{
	return p2l->read_ctx.cb_fn ? true : false;
}

static bool
ftl_p2l_log_read_is_next(struct ftl_p2l_log *p2l)
{
	if (p2l->read_ctx.result) {
		return false;
	}

	return p2l->read_ctx.idx < p2l->entry_max;
}

static bool
ftl_p2l_log_read_is_qd(struct ftl_p2l_log *p2l)
{
	return p2l->read_ctx.qd > 0;
}

static bool
ftl_p2l_log_read_is_finished(struct ftl_p2l_log *p2l)
{
	if (ftl_p2l_log_read_is_next(p2l) || ftl_p2l_log_read_is_qd(p2l)) {
		return false;
	}

	return true;
}

static void
ftl_p2l_log_read_finish(struct ftl_p2l_log *p2l)
{
	spdk_ftl_fn cb_fn = p2l->read_ctx.cb_fn;
	void *cb_arg = p2l->read_ctx.cb_arg;
	int result = p2l->read_ctx.result;

	memset(&p2l->read_ctx, 0, sizeof(p2l->read_ctx));
	cb_fn(cb_arg, result);
}

static void
ftl_p2l_log_read_visit(struct ftl_p2l_log *p2l, struct ftl_p2l_log_page_ctrl *ctrl)
{
	struct ftl_p2l_log_page *page = &ctrl->page;
	struct spdk_ftl_dev *dev = p2l->dev;
	ftl_p2l_log_rd_cb cb_rd = p2l->read_ctx.cb_rd;
	void *cb_arg = p2l->read_ctx.cb_arg;
	uint64_t crc = p2l_log_page_crc(&ctrl->page);
	uint64_t i, j;
	int rc = 0;

	ftl_bug(ctrl->entry_idx > p2l->entry_max);

	if (p2l->read_ctx.seq_id != page->hdr.p2l_ckpt.seq_id) {
		/* This page contains entires older than the owner's sequence ID */
		return;
	}

	if (ctrl->entry_idx != page->hdr.p2l_ckpt.idx) {
		FTL_ERRLOG(p2l->dev, "Read P2L IO Logs ERROR, invalid index, type %d\n",
			   p2l->md->region->type);
		p2l->read_ctx.result = -EINVAL;
		return;
	}

	if (crc != page->hdr.p2l_ckpt.p2l_checksum) {
		FTL_ERRLOG(p2l->dev, "Read P2L IO Log ERROR, CRC problem, type %d\n",
			   p2l->md->region->type);
		p2l->read_ctx.result = -EINVAL;
		return;
	}

	if (page->hdr.p2l_ckpt.count > SPDK_COUNTOF(page->items)) {
		FTL_ERRLOG(p2l->dev, "Read P2L IO Log ERROR, inconsistent format, type %d\n",
			   p2l->md->region->type);
		p2l->read_ctx.result = -EINVAL;
		return;
	}

	for (i = 0; i < page->hdr.p2l_ckpt.count; i++) {
		struct ftl_pl2_log_item *item = &page->items[i];

		for (j = 0; j < item->num_blocks; j++) {
			rc = cb_rd(dev, cb_arg, item->lba + j, item->addr + j, item->seq_id);
			if (rc) {
				p2l->read_ctx.result = rc;
				break;
			}
		}

		if (rc) {
			break;
		}
	}
}

static void
ftl_p2l_log_read_cb(int status, void *arg)
{
	struct ftl_p2l_log_page_ctrl *ctrl = arg;
	struct ftl_p2l_log *p2l = ctrl->p2l;

	assert(p2l->read_ctx.qd > 0);
	p2l->read_ctx.qd--;

	if (status) {
		p2l->read_ctx.result = status;
	} else {
		ftl_p2l_log_read_visit(p2l, ctrl);
	}

	/* Release page control */
	ftl_mempool_put(p2l->page_pool, ctrl);
	ftl_p2l_log_read_process(p2l);
}

static void
ftl_p2l_log_read_process(struct ftl_p2l_log *p2l)
{
	struct ftl_p2l_log_page_ctrl *ctrl;

	while (ftl_p2l_log_read_is_next(p2l)) {
		ctrl = ftl_mempool_get(p2l->page_pool);
		if (!ctrl) {
			break;
		}

		ctrl->p2l = p2l;
		ctrl->entry_idx = p2l->read_ctx.idx++;

		/* Check if the index exceeding the buffer size */
		ftl_bug(p2l->read_ctx.idx > p2l->entry_max);

		p2l->read_ctx.qd++;
		ftl_md_read_entry(p2l->md, ctrl->entry_idx, &ctrl->page, NULL,
				  ftl_p2l_log_read_cb, ctrl, &ctrl->md_ctx);
	}

	if (ftl_p2l_log_read_is_finished(p2l)) {
		ftl_p2l_log_read_finish(p2l);
	}
}

int
ftl_p2l_log_read(struct spdk_ftl_dev *dev, enum ftl_layout_region_type type, uint64_t seq_id,
		 spdk_ftl_fn cb_fn, void *cb_arg, ftl_p2l_log_rd_cb cb_rd)
{
	struct ftl_p2l_log *p2l_log = p2l_log_get(dev, type);

	if (!p2l_log) {
		FTL_ERRLOG(dev, "Read P2L IO Log ERROR, no such log, type %d\n", type);
		return -ENODEV;
	}
	if (p2l_log_read_in_progress(p2l_log)) {
		FTL_ERRLOG(dev, "Read P2L IO Log ERROR, read busy, type %d\n", type);
		return -EBUSY;
	}

	memset(&p2l_log->read_ctx, 0, sizeof(p2l_log->read_ctx));
	p2l_log->read_ctx.cb_fn = cb_fn;
	p2l_log->read_ctx.cb_arg = cb_arg;
	p2l_log->read_ctx.cb_rd = cb_rd;
	p2l_log->read_ctx.seq_id = seq_id;

	ftl_p2l_log_read_process(p2l_log);
	if (ftl_p2l_log_read_is_qd(p2l_log)) {
		/* Read in progress */
		return 0;
	} else {
		FTL_ERRLOG(dev, "Read P2L IO Log ERROR, operation not started, type %d\n", type);
		return -EINVAL;
	}
}
