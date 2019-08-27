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

#include "vbdev_zone_block.h"

#include "spdk/config.h"
#include "spdk/nvme.h"
#include "spdk/bdev_zone.h"

#include "spdk_internal/log.h"

static int vbdev_block_init(void);
static int vbdev_block_get_ctx_size(void);
static void vbdev_block_finish(void);
static int vbdev_block_config_json(struct spdk_json_write_ctx *w);
static void vbdev_block_examine(struct spdk_bdev *bdev);
static void vbdev_block_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

static struct spdk_bdev_module bdev_zoned_if = {
	.name = "bdev_zoned_block",
	.module_init = vbdev_block_init,
	.module_fini = vbdev_block_finish,
	.config_text = NULL,
	.config_json = vbdev_block_config_json,
	.examine_config = vbdev_block_examine,
	.get_ctx_size = vbdev_block_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(bdev_zoned_block, &bdev_zoned_if)

/* List of block_vbdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	size_t			num_zones;
	size_t			max_open_zones;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_zone {
	struct spdk_bdev_zone_info zone_info;
	size_t zone_id;
	bool write_inflight;
};

/* List of block vbdevs and associated info for each. */
struct vbdev_block {
	struct spdk_bdev		*base_bdev; /* the block device we're attaching to */
	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	struct spdk_bdev		bdev;    /* the block zoned bdev */
	struct vbdev_zone		*zone_buf; /* array of zones */
	size_t				num_zones; /* number of zones */
	size_t				open_zones; /* total open zones */
	TAILQ_ENTRY(vbdev_block)	link;
};
static TAILQ_HEAD(, vbdev_block) g_bdev_nodes = TAILQ_HEAD_INITIALIZER(g_bdev_nodes);

struct vbdev_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
};

struct block_vbdev_io {
	/* bdev related */
	struct spdk_io_channel *ch;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;

	/* bdev IO was issued to */
	struct vbdev_block *bdev_node;

	/* zone IO was issued to */
	struct vbdev_zone *zone;
};

static int
vbdev_block_init(void)
{
	return 0;
}

static void
vbdev_remove_config(struct bdev_names *name)
{
	TAILQ_REMOVE(&g_bdev_names, name, link);
	free(name->bdev_name);
	free(name->vbdev_name);
	free(name);
}

static void
vbdev_block_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		vbdev_remove_config(name);
	}

	return;
}

static int
vbdev_block_get_ctx_size(void)
{
	return sizeof(struct block_vbdev_io);
}

static int
vbdev_block_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_block *bdev_node;

	TAILQ_FOREACH(bdev_node, &g_bdev_nodes, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_zone_block_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(bdev_node->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
		spdk_json_write_named_uint64(w, "num_zones", bdev_node->num_zones);
		spdk_json_write_named_uint64(w, "max_open_zones", bdev_node->bdev.max_open_zones);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	return 0;
}

static void
vbdev_block_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	return;
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_block *bdev_node  = io_device;

	free(bdev_node->bdev.name);
	free(bdev_node->zone_buf);
	free(bdev_node);
}

static int
vbdev_block_destruct(void *ctx)
{
	struct vbdev_block *bdev_node = (struct vbdev_block *)ctx;

	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(bdev_node->base_bdev);

	/* Close the underlying bdev. */
	spdk_bdev_close(bdev_node->base_desc);

	/* Unregister the io_device. */
	spdk_io_device_unregister(bdev_node, _device_unregister_cb);

	return 0;
}

static void
vbdev_block_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)bdev_io->driver_ctx;

	vbdev_block_submit_request(io_ctx->ch, bdev_io);
}

static void
vbdev_block_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_block_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_block_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static struct vbdev_zone *
vbdev_get_zone_by_lba(struct vbdev_block *bdev_node, uint64_t start_lba)
{
	struct vbdev_zone *zone = NULL;
	size_t index = start_lba / bdev_node->bdev.zone_size;

	if (index >= bdev_node->num_zones) {
		return NULL;
	}

	if (bdev_node->zone_buf[index].zone_info.zone_id == start_lba) {
		zone = &bdev_node->zone_buf[index];
	}

	return zone;
}

static struct vbdev_zone *
vbdev_get_zone_containing_lba(struct vbdev_block *bdev_node, uint64_t lba)
{
	size_t index = lba / bdev_node->bdev.zone_size;

	if (index >= bdev_node->num_zones) {
		return NULL;
	}

	return &bdev_node->zone_buf[index];
}

static int
vbdev_block_get_zone_info(struct vbdev_block *bdev_node, struct vbdev_zone *zone,
			  struct spdk_bdev_io *bdev_io)
{
	void *buffer = bdev_io->u.zdev.buf;
	size_t i, zone_id;

	/* User can request info from more zones than exist, need to check both internal and user boundaries
	 */
	for (i = 0, zone_id = zone->zone_id; i < bdev_io->u.zdev.num_zones &&
	     zone_id < bdev_node->num_zones; i++, zone_id++) {
		zone = &bdev_node->zone_buf[zone_id];
		memcpy(((struct spdk_bdev_zone_info *)buffer) + i, &zone->zone_info,
		       sizeof(struct spdk_bdev_zone_info));
	}

	return 0;
}
static int
vbdev_block_open_zone(struct vbdev_block *bdev_node, struct vbdev_zone *zone)
{
	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_FULL:
	case SPDK_BDEV_ZONE_STATE_READ_ONLY:
	case SPDK_BDEV_ZONE_STATE_OFFLINE:
		return -EINVAL;
	case SPDK_BDEV_ZONE_STATE_OPEN:
		return 0;
	default:
		break;
	}
	size_t open_zones = __atomic_add_fetch(&bdev_node->open_zones, 1, __ATOMIC_SEQ_CST);
	if (open_zones > bdev_node->bdev.max_open_zones) {
		__atomic_sub_fetch(&bdev_node->open_zones, 1, __ATOMIC_SEQ_CST);
		SPDK_ERRLOG("Trying to open too many zones\n");
		return -EINVAL;
	}

	zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
	return 0;
}

static void
_vbdev_complete_unmap(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)orig_io->driver_ctx;
	struct vbdev_zone *zone = io_ctx->zone;

	if (success) {
		if (zone->zone_info.state == SPDK_BDEV_ZONE_STATE_OPEN) {
			__atomic_sub_fetch(&io_ctx->bdev_node->open_zones, 1, __ATOMIC_SEQ_CST);
		}

		if (zone->zone_info.state == SPDK_BDEV_ZONE_STATE_READ_ONLY) {
			zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OFFLINE;
		} else {
			zone->zone_info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
		}
		zone->zone_info.write_pointer = zone->zone_info.zone_id;
	}

	__atomic_store_n(&zone->write_inflight, false, __ATOMIC_SEQ_CST);

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
vbdev_block_reset_zone(struct vbdev_block *bdev_node, struct vbdev_io_channel *ch,
		       struct vbdev_zone *zone, struct spdk_bdev_io *bdev_io, bool *passed_request)
{
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)bdev_io->driver_ctx;
	int rc;

	*passed_request = false;
	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OFFLINE:
		return -EINVAL;
	case SPDK_BDEV_ZONE_STATE_EMPTY:
		return 0;
	default:
		break;
	}

	/* If a write is already being processed on the zone, reschedule it */
	if (__atomic_exchange_n(&zone->write_inflight, true, __ATOMIC_SEQ_CST)) {
		return -ENOMEM;
	}

	io_ctx->zone = zone;
	io_ctx->bdev_node = bdev_node;

	rc = spdk_bdev_unmap_blocks(bdev_node->base_desc, ch->base_ch,
				    zone->zone_info.zone_id, zone->zone_info.capacity,
				    _vbdev_complete_unmap, bdev_io);
	if (!rc) {
		*passed_request = true;
	}

	return rc;
}

static int
vbdev_block_zone_management(struct vbdev_block *bdev_node, struct vbdev_io_channel *ch,
			    struct spdk_bdev_io *bdev_io, bool *passed_request)
{
	struct vbdev_zone *zone;
	*passed_request = false;

	zone = vbdev_get_zone_by_lba(bdev_node, bdev_io->u.zdev.zone_id);
	if (!zone) {
		return -EINVAL;
	}

	switch (bdev_io->u.zdev.zone_action) {
	case SPDK_BDEV_ZONE_INFO:
		return vbdev_block_get_zone_info(bdev_node, zone, bdev_io);
	case SPDK_BDEV_ZONE_RESET:
		return vbdev_block_reset_zone(bdev_node, ch, zone, bdev_io, passed_request);
	case SPDK_BDEV_ZONE_OPEN:
		return vbdev_block_open_zone(bdev_node, zone);
	case SPDK_BDEV_ZONE_CLOSE:
	case SPDK_BDEV_ZONE_FINISH:
	default:
		return -EINVAL;
	}
}

static int
vbdev_open_zone(struct vbdev_block *bdev_node, struct vbdev_zone *zone)
{
	size_t open_zones = __atomic_add_fetch(&bdev_node->open_zones, 1, __ATOMIC_SEQ_CST);
	if (open_zones > bdev_node->bdev.max_open_zones) {
		__atomic_sub_fetch(&bdev_node->open_zones, 1, __ATOMIC_SEQ_CST);
		SPDK_ERRLOG("Trying to open too many zones\n");
		return -EINVAL;
	}

	zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
	return 0;
}

static void
_vbdev_complete_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)orig_io->driver_ctx;
	struct vbdev_zone *zone = io_ctx->zone;

	if (success) {
		zone->zone_info.write_pointer += bdev_io->u.bdev.num_blocks;
		assert(zone->zone_info.write_pointer <= zone->zone_info.zone_id + zone->zone_info.capacity);
		if (zone->zone_info.write_pointer == zone->zone_info.zone_id + zone->zone_info.capacity) {
			zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;
		}
	}

	__atomic_store_n(&zone->write_inflight, false, __ATOMIC_SEQ_CST);

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
vbdev_block_write(struct vbdev_block *bdev_node, struct vbdev_io_channel *ch,
		  struct spdk_bdev_io *bdev_io)
{
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)bdev_io->driver_ctx;
	struct vbdev_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t wp;
	int rc = 0;
	bool write_inflight;

	zone = vbdev_get_zone_containing_lba(bdev_node, lba);
	if (!zone) {
		SPDK_ERRLOG("Trying to write to invalid zone (lba 0x%lx)\n", lba);
		return -EINVAL;
	}

	io_ctx->zone = zone;

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_FULL:
	case SPDK_BDEV_ZONE_STATE_READ_ONLY:
	case SPDK_BDEV_ZONE_STATE_OFFLINE:
		SPDK_ERRLOG("Trying to write to zone in invalid state %u\n", zone->zone_info.state);
		return -EINVAL;
	default:
		break;
	}

	/* If a write is already being processed on the zone, reschedule it */
	write_inflight = __atomic_exchange_n(&zone->write_inflight, true, __ATOMIC_SEQ_CST);
	if (write_inflight) {
		return -ENOMEM;
	}

	wp = zone->zone_info.write_pointer;

	if (lba != wp) {
		SPDK_ERRLOG("Trying to write to zone with invalid address (lba 0x%lx, wp 0x%lx)\n", lba, wp);
		rc = -EINVAL;
		goto write_fail;
	}
	if ((lba + len) > (zone->zone_info.zone_id + zone->zone_info.capacity)) {
		SPDK_ERRLOG("Write exceeds zone capacity (lba 0x%lx, len 0x%lx, wp 0x%lx)\n", lba, len, wp);
		rc = -EINVAL;
		goto write_fail;
	}

	if (zone->zone_info.state == SPDK_BDEV_ZONE_STATE_EMPTY ||
	    zone->zone_info.state == SPDK_BDEV_ZONE_STATE_CLOSED) {
		rc = vbdev_open_zone(bdev_node, zone);
		if (rc) {
			goto write_fail;
		}
	}

	if (bdev_io->u.bdev.md_buf == NULL) {
		rc = spdk_bdev_writev_blocks(bdev_node->base_desc, ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks, _vbdev_complete_write,
					     bdev_io);
	} else {
		rc = spdk_bdev_writev_blocks_with_md(bdev_node->base_desc, ch->base_ch,
						     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						     bdev_io->u.bdev.md_buf,
						     bdev_io->u.bdev.offset_blocks,
						     bdev_io->u.bdev.num_blocks,
						     _vbdev_complete_write, bdev_io);
	}

	if (rc) {
		goto write_fail;
	}

	return rc;

write_fail:
	__atomic_store_n(&zone->write_inflight, false, __ATOMIC_SEQ_CST);
	return rc;
}

static void
_vbdev_fill_iovec_with_zeros(struct spdk_bdev_io *bdev_io, size_t offset)
{
	size_t current_block = 0;
	struct iovec *iovs = bdev_io->u.bdev.iovs;
	struct iovec *iov;
	size_t iov_offset = 0;
	size_t block_size = bdev_io->bdev->blocklen;
	int iov_index = 0;
	do {
		assert(iov_index <= bdev_io->u.bdev.iovcnt);
		iov = &iovs[iov_index];

		if (current_block >= offset) {
			memset(((char *)iovs->iov_base) + block_size * iov_offset, 0, block_size);
		}

		iov_offset++;
		if (iov_offset * block_size == iov->iov_len) {
			iov_index++;
			iov_offset = 0;
		}

		current_block++;
	} while (current_block < bdev_io->u.bdev.num_blocks);
}

static void
_vbdev_complete_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	if (success) {
		if (bdev_io->u.bdev.num_blocks < orig_io->u.bdev.num_blocks) {
			_vbdev_fill_iovec_with_zeros(orig_io, bdev_io->u.bdev.num_blocks);
		}
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
vbdev_block_read(struct vbdev_block *bdev_node, struct vbdev_io_channel *ch,
		 struct spdk_bdev_io *bdev_io)
{
	struct vbdev_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t wp;
	int rc;

	zone = vbdev_get_zone_containing_lba(bdev_node, lba);
	if (!zone) {
		SPDK_ERRLOG("Trying to read from invalid zone (lba 0x%lx)\n", lba);
		return -EINVAL;
	}

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OFFLINE:
		SPDK_ERRLOG("Trying to read from zone in invalid state %u\n", zone->zone_info.state);
		return -EINVAL;
	default:
		break;
	}

	wp = zone->zone_info.write_pointer;
	if ((lba + len) > (zone->zone_info.zone_id + zone->zone_info.capacity)) {
		SPDK_ERRLOG("Read exceeds zone capacity (lba 0x%lx, len 0x%lx, wp 0x%lx)\n", lba, len, wp);
		return -EINVAL;
	}
	if (lba > wp || zone->zone_info.state == SPDK_BDEV_ZONE_STATE_EMPTY) {
		_vbdev_fill_iovec_with_zeros(bdev_io, 0);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	}

	len = spdk_min(wp - lba, len);

	if (bdev_io->u.bdev.md_buf == NULL) {
		rc = spdk_bdev_readv_blocks(bdev_node->base_desc, ch->base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.offset_blocks,
					    len, _vbdev_complete_read,
					    bdev_io);
	} else {
		rc = spdk_bdev_readv_blocks_with_md(bdev_node->base_desc, ch->base_ch,
						    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.md_buf,
						    bdev_io->u.bdev.offset_blocks,
						    len,
						    _vbdev_complete_read, bdev_io);
	}

	return rc;
}

static void
vbdev_block_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_block *bdev_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_block, bdev);
	struct block_vbdev_io *io_ctx = (struct block_vbdev_io *)bdev_io->driver_ctx;
	struct vbdev_io_channel *dev_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0;
	bool passed_request = true;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = vbdev_block_zone_management(bdev_node, dev_ch, bdev_io, &passed_request);
		passed_request = false;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = vbdev_block_write(bdev_node, dev_ch, bdev_io);
		passed_request = true;
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		rc = vbdev_block_read(bdev_node, dev_ch, bdev_io);
		passed_request = true;
		break;
	default:
		SPDK_ERRLOG("vbdev_block: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for vbdev.\n");
			io_ctx->ch = ch;
			vbdev_block_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	} else if (!passed_request) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
}

static bool
vbdev_block_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_block *bdev_node = (struct vbdev_block *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return spdk_bdev_io_type_supported(bdev_node->base_bdev, io_type);
	default:
		return false;
	}
}

static struct spdk_io_channel *
vbdev_block_get_io_channel(void *ctx)
{
	struct vbdev_block *bdev_node = (struct vbdev_block *)ctx;

	return spdk_get_io_channel(bdev_node);
}

static int
vbdev_block_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_block *bdev_node = (struct vbdev_block *)ctx;

	spdk_json_write_name(w, "vbdev");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
	spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(bdev_node->base_bdev));
	spdk_json_write_named_uint64(w, "num_zones", bdev_node->num_zones);
	spdk_json_write_named_uint64(w, "max_open_zones", bdev_node->bdev.max_open_zones);
	spdk_json_write_object_end(w);

	return 0;
}

/* When we register our vbdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_block_fn_table = {
	.destruct		= vbdev_block_destruct,
	.submit_request		= vbdev_block_submit_request,
	.io_type_supported	= vbdev_block_io_type_supported,
	.get_io_channel		= vbdev_block_get_io_channel,
	.dump_info_json		= vbdev_block_dump_info_json,
	.write_config_json	= vbdev_block_write_config_json,
};

static void
vbdev_block_base_bdev_hotremove_cb(void *ctx)
{
	struct vbdev_block *bdev_node, *tmp;
	struct spdk_bdev *bdev_find = ctx;

	TAILQ_FOREACH_SAFE(bdev_node, &g_bdev_nodes, link, tmp) {
		if (bdev_find == bdev_node->base_bdev) {
			spdk_bdev_unregister(&bdev_node->bdev, NULL, NULL);
		}
	}
}

static int
block_vbdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_io_channel *bdev_ch = ctx_buf;
	struct vbdev_block *bdev_node = io_device;

	bdev_ch->base_ch = spdk_bdev_get_io_channel(bdev_node->base_desc);

	return 0;
}

static void
block_vbdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_io_channel *bdev_ch = ctx_buf;

	spdk_put_io_channel(bdev_ch->base_ch);
}

static int
block_vbdev_insert_name(const char *bdev_name, const char *vbdev_name, size_t num_zones,
			size_t max_open_zones)
{
	struct bdev_names *name;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("block zoned bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
		if (strcmp(bdev_name, name->bdev_name) == 0) {
			SPDK_ERRLOG("base bdev %s already claimed\n", bdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}

	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		free(name);
		return -ENOMEM;
	}

	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		free(name->bdev_name);
		free(name);
		return -ENOMEM;
	}

	name->num_zones = num_zones;
	name->max_open_zones = max_open_zones;

	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;
}

static void
block_vbdev_init_zone_info(struct vbdev_block *bdev_node)
{
	size_t i;
	struct vbdev_zone *zone;

	for (i = 0; i < bdev_node->num_zones; i++) {
		zone = &bdev_node->zone_buf[i];
		zone->zone_info.zone_id = bdev_node->bdev.zone_size * i;
		zone->zone_info.write_pointer = zone->zone_info.zone_id;
		zone->zone_info.capacity = bdev_node->bdev.zone_size;
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
		zone->zone_id = i;
		/* TODO switch to these after testing */
		/* zone->zone_info.write_pointer = zone->zone_info.zone_id + zone->zone_info.capacity; */
		/* zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL; */
	}
}

static int
vbdev_block_register(struct spdk_bdev *bdev)
{
	struct bdev_names *name, *tmp;
	struct vbdev_block *bdev_node;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the pt_node & bdev accordingly.
	 */
	TAILQ_FOREACH_SAFE(name, &g_bdev_names, link, tmp) {
		if (strcmp(name->bdev_name, bdev->name) != 0) {
			continue;
		}

		if (spdk_bdev_is_zoned(bdev)) {
			SPDK_ERRLOG("Base bdev (%s) is a zoned bdev\n", bdev->name);
			rc = -ENODEV;
			goto free_config;
		}

		bdev_node = calloc(1, sizeof(struct vbdev_block));
		if (!bdev_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node\n");
			goto free_config;
		}

		/* The base bdev that we're attaching to. */
		bdev_node->base_bdev = bdev;
		bdev_node->bdev.name = strdup(name->vbdev_name);
		if (!bdev_node->bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node name\n");
			goto strdup_failed;
		}

		bdev_node->num_zones = name->num_zones;
		bdev_node->zone_buf = calloc(bdev_node->num_zones, sizeof(struct vbdev_zone));
		if (!bdev_node->zone_buf) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate zone_buf\n");
			goto calloc_failed;
		}

		bdev_node->bdev.product_name = "vbdev_block";

		/* Copy some properties from the underlying base bdev. */
		bdev_node->bdev.write_cache = bdev->write_cache;
		bdev_node->bdev.required_alignment = bdev->required_alignment;
		bdev_node->bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		bdev_node->bdev.blocklen = bdev->blocklen;
		bdev_node->bdev.blockcnt = bdev->blockcnt;
		bdev_node->bdev.write_unit_size = bdev->write_unit_size;

		bdev_node->bdev.md_interleave = bdev->md_interleave;
		bdev_node->bdev.md_len = bdev->md_len;
		bdev_node->bdev.dif_type = bdev->dif_type;
		bdev_node->bdev.dif_is_head_of_md = bdev->dif_is_head_of_md;
		bdev_node->bdev.dif_check_flags = bdev->dif_check_flags;

		bdev_node->bdev.zoned = true;
		bdev_node->bdev.ctxt = bdev_node;
		bdev_node->bdev.fn_table = &vbdev_block_fn_table;
		bdev_node->bdev.module = &bdev_zoned_if;

		/* bdev specific info */
		bdev_node->bdev.zone_size = bdev->blockcnt / bdev_node->num_zones;
		bdev_node->bdev.optimal_open_zones = name->max_open_zones;
		bdev_node->bdev.max_open_zones = name->max_open_zones;
		block_vbdev_init_zone_info(bdev_node);

		TAILQ_INSERT_TAIL(&g_bdev_nodes, bdev_node, link);

		spdk_io_device_register(bdev_node, block_vbdev_ch_create_cb, block_vbdev_ch_destroy_cb,
					sizeof(struct vbdev_io_channel),
					name->vbdev_name);

		rc = spdk_bdev_open(bdev, true, vbdev_block_base_bdev_hotremove_cb,
				    bdev, &bdev_node->base_desc);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
			goto open_failed;
		}

		rc = spdk_bdev_module_claim_bdev(bdev, bdev_node->base_desc, bdev_node->bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto claim_failed;
		}

		rc = spdk_bdev_register(&bdev_node->bdev);
		if (rc) {
			SPDK_ERRLOG("could not register zoned bdev\n");
			goto register_failed;
		}
	}

	return rc;

register_failed:
	spdk_bdev_module_release_bdev(&bdev_node->bdev);
claim_failed:
	spdk_bdev_close(bdev_node->base_desc);
open_failed:
	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);
	spdk_io_device_unregister(bdev_node, NULL);
	free(bdev_node->zone_buf);
calloc_failed:
	free(bdev_node->bdev.name);
strdup_failed:
	free(bdev_node);
free_config:
	vbdev_remove_config(name);
	return rc;
}

int
spdk_vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name, size_t num_zones,
			     size_t max_open_zones)
{
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	if (num_zones == 0) {
		SPDK_ERRLOG("Can't create vbdev with 0 zones\n");
		return -EINVAL;
	}

	if (num_zones < max_open_zones) {
		SPDK_ERRLOG("Number of zones (%lu) is smaller than max open zones (%lu)\n",
			    num_zones, max_open_zones);
		return -EINVAL;
	}

	/* Insert the bdev into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = block_vbdev_insert_name(bdev_name, vbdev_name, num_zones, max_open_zones);
	if (rc) {
		return rc;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		/* This is not an error, we tracked the name above and it still
		 * may show up later.
		 */
		return 0;
	}

	rc = vbdev_block_register(bdev);

	return rc;
}

void
spdk_vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_names *name_node;
	struct spdk_bdev *bdev = NULL;

	bdev = spdk_bdev_get_by_name(name);
	if (!bdev || bdev->module != &bdev_zoned_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	TAILQ_FOREACH(name_node, &g_bdev_names, link) {
		if (strcmp(name_node->vbdev_name, bdev->name) == 0) {
			vbdev_remove_config(name_node);
			break;
		}
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static void
vbdev_block_examine(struct spdk_bdev *bdev)
{
	vbdev_block_register(bdev);

	spdk_bdev_module_examine_done(&bdev_zoned_if);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_zone_block", SPDK_LOG_VBDEV_ZONE_BLOCK)
