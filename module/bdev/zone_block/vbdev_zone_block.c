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

#include "spdk/log.h"

static int zone_block_init(void);
static int zone_block_get_ctx_size(void);
static void zone_block_finish(void);
static int zone_block_config_json(struct spdk_json_write_ctx *w);
static void zone_block_examine(struct spdk_bdev *bdev);

static struct spdk_bdev_module bdev_zoned_if = {
	.name = "bdev_zoned_block",
	.module_init = zone_block_init,
	.module_fini = zone_block_finish,
	.config_json = zone_block_config_json,
	.examine_config = zone_block_examine,
	.get_ctx_size = zone_block_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(bdev_zoned_block, &bdev_zoned_if)

/* List of block vbdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_zone_block_config {
	char					*vbdev_name;
	char					*bdev_name;
	uint64_t				zone_capacity;
	uint64_t				optimal_open_zones;
	TAILQ_ENTRY(bdev_zone_block_config)	link;
};
static TAILQ_HEAD(, bdev_zone_block_config) g_bdev_configs = TAILQ_HEAD_INITIALIZER(g_bdev_configs);

struct block_zone {
	struct spdk_bdev_zone_info zone_info;
	pthread_spinlock_t lock;
};

/* List of block vbdevs and associated info for each. */
struct bdev_zone_block {
	struct spdk_bdev		bdev;    /* the block zoned bdev */
	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	struct block_zone		*zones; /* array of zones */
	uint64_t			num_zones; /* number of zones */
	uint64_t			zone_capacity; /* zone capacity */
	uint64_t                        zone_shift; /* log2 of zone_size */
	TAILQ_ENTRY(bdev_zone_block)	link;
	struct spdk_thread		*thread; /* thread where base device is opened */
};
static TAILQ_HEAD(, bdev_zone_block) g_bdev_nodes = TAILQ_HEAD_INITIALIZER(g_bdev_nodes);

struct zone_block_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
};

struct zone_block_io {
	/* vbdev to which IO was issued */
	struct bdev_zone_block *bdev_zone_block;
};

static int
zone_block_init(void)
{
	return 0;
}

static void
zone_block_remove_config(struct bdev_zone_block_config *name)
{
	TAILQ_REMOVE(&g_bdev_configs, name, link);
	free(name->bdev_name);
	free(name->vbdev_name);
	free(name);
}

static void
zone_block_finish(void)
{
	struct bdev_zone_block_config *name;

	while ((name = TAILQ_FIRST(&g_bdev_configs))) {
		zone_block_remove_config(name);
	}
}

static int
zone_block_get_ctx_size(void)
{
	return sizeof(struct zone_block_io);
}

static int
zone_block_config_json(struct spdk_json_write_ctx *w)
{
	struct bdev_zone_block *bdev_node;
	struct spdk_bdev *base_bdev = NULL;

	TAILQ_FOREACH(bdev_node, &g_bdev_nodes, link) {
		base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_zone_block_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
		spdk_json_write_named_uint64(w, "zone_capacity", bdev_node->zone_capacity);
		spdk_json_write_named_uint64(w, "optimal_open_zones", bdev_node->bdev.optimal_open_zones);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	return 0;
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct bdev_zone_block *bdev_node = io_device;
	uint64_t i;

	free(bdev_node->bdev.name);
	for (i = 0; i < bdev_node->num_zones; i++) {
		pthread_spin_destroy(&bdev_node->zones[i].lock);
	}
	free(bdev_node->zones);
	free(bdev_node);
}

static void
_zone_block_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

static int
zone_block_destruct(void *ctx)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;

	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(bdev_node->base_desc));

	/* Close the underlying bdev on its same opened thread. */
	if (bdev_node->thread && bdev_node->thread != spdk_get_thread()) {
		spdk_thread_send_msg(bdev_node->thread, _zone_block_destruct, bdev_node->base_desc);
	} else {
		spdk_bdev_close(bdev_node->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(bdev_node, _device_unregister_cb);

	return 0;
}

static struct block_zone *
zone_block_get_zone_containing_lba(struct bdev_zone_block *bdev_node, uint64_t lba)
{
	size_t index = lba >> bdev_node->zone_shift;

	if (index >= bdev_node->num_zones) {
		return NULL;
	}

	return &bdev_node->zones[index];
}

static struct block_zone *
zone_block_get_zone_by_slba(struct bdev_zone_block *bdev_node, uint64_t start_lba)
{
	struct block_zone *zone = zone_block_get_zone_containing_lba(bdev_node, start_lba);

	if (zone && zone->zone_info.zone_id == start_lba) {
		return zone;
	} else {
		return NULL;
	}
}

static int
zone_block_get_zone_info(struct bdev_zone_block *bdev_node, struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	struct spdk_bdev_zone_info *zone_info = bdev_io->u.zone_mgmt.buf;
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;
	size_t i;

	/* User can request info for more zones than exist, need to check both internal and user
	 * boundaries
	 */
	for (i = 0; i < bdev_io->u.zone_mgmt.num_zones; i++, zone_id += bdev_node->bdev.zone_size) {
		zone = zone_block_get_zone_by_slba(bdev_node, zone_id);
		if (!zone) {
			return -EINVAL;
		}
		memcpy(&zone_info[i], &zone->zone_info, sizeof(*zone_info));
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	return 0;
}

static int
zone_block_open_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_EMPTY:
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	default:
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static void
_zone_block_complete_unmap(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_reset_zone(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		      struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_EMPTY:
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_FULL:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
		zone->zone_info.write_pointer = zone->zone_info.zone_id;
		pthread_spin_unlock(&zone->lock);
		return spdk_bdev_unmap_blocks(bdev_node->base_desc, ch->base_ch,
					      zone->zone_info.zone_id, zone->zone_info.capacity,
					      _zone_block_complete_unmap, bdev_io);
	default:
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static int
zone_block_close_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_CLOSED;
		pthread_spin_unlock(&zone->lock);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	default:
		pthread_spin_unlock(&zone->lock);
		return -EINVAL;
	}
}

static int
zone_block_finish_zone(struct block_zone *zone, struct spdk_bdev_io *bdev_io)
{
	pthread_spin_lock(&zone->lock);

	zone->zone_info.write_pointer = zone->zone_info.zone_id + zone->zone_info.capacity;
	zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;

	pthread_spin_unlock(&zone->lock);
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	return 0;
}

static int
zone_block_zone_management(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
			   struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;

	zone = zone_block_get_zone_by_slba(bdev_node, bdev_io->u.zone_mgmt.zone_id);
	if (!zone) {
		return -EINVAL;
	}

	switch (bdev_io->u.zone_mgmt.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		return zone_block_reset_zone(bdev_node, ch, zone, bdev_io);
	case SPDK_BDEV_ZONE_OPEN:
		return zone_block_open_zone(zone, bdev_io);
	case SPDK_BDEV_ZONE_CLOSE:
		return zone_block_close_zone(zone, bdev_io);
	case SPDK_BDEV_ZONE_FINISH:
		return zone_block_finish_zone(zone, bdev_io);
	default:
		return -EINVAL;
	}
}

static void
_zone_block_complete_write(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	if (success && orig_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND) {
		orig_io->u.bdev.offset_blocks = bdev_io->u.bdev.offset_blocks;
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_write(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		 struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	uint64_t num_blocks_left, wp;
	int rc = 0;
	bool is_append = bdev_io->type == SPDK_BDEV_IO_TYPE_ZONE_APPEND;

	if (is_append) {
		zone = zone_block_get_zone_by_slba(bdev_node, lba);
	} else {
		zone = zone_block_get_zone_containing_lba(bdev_node, lba);
	}
	if (!zone) {
		SPDK_ERRLOG("Trying to write to invalid zone (lba 0x%" PRIx64 ")\n", lba);
		return -EINVAL;
	}

	pthread_spin_lock(&zone->lock);

	switch (zone->zone_info.state) {
	case SPDK_BDEV_ZONE_STATE_OPEN:
	case SPDK_BDEV_ZONE_STATE_EMPTY:
	case SPDK_BDEV_ZONE_STATE_CLOSED:
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_OPEN;
		break;
	default:
		SPDK_ERRLOG("Trying to write to zone in invalid state %u\n", zone->zone_info.state);
		rc = -EINVAL;
		goto write_fail;
	}

	wp = zone->zone_info.write_pointer;
	if (is_append) {
		lba = wp;
	} else {
		if (lba != wp) {
			SPDK_ERRLOG("Trying to write to zone with invalid address (lba 0x%" PRIx64 ", wp 0x%" PRIx64 ")\n",
				    lba, wp);
			rc = -EINVAL;
			goto write_fail;
		}
	}

	num_blocks_left = zone->zone_info.zone_id + zone->zone_info.capacity - wp;
	if (len > num_blocks_left) {
		SPDK_ERRLOG("Write exceeds zone capacity (lba 0x%" PRIx64 ", len 0x%" PRIx64 ", wp 0x%" PRIx64
			    ")\n", lba, len, wp);
		rc = -EINVAL;
		goto write_fail;
	}

	zone->zone_info.write_pointer += bdev_io->u.bdev.num_blocks;
	assert(zone->zone_info.write_pointer <= zone->zone_info.zone_id + zone->zone_info.capacity);
	if (zone->zone_info.write_pointer == zone->zone_info.zone_id + zone->zone_info.capacity) {
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;
	}
	pthread_spin_unlock(&zone->lock);

	if (bdev_io->u.bdev.md_buf == NULL) {
		rc = spdk_bdev_writev_blocks(bdev_node->base_desc, ch->base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, lba,
					     bdev_io->u.bdev.num_blocks, _zone_block_complete_write,
					     bdev_io);
	} else {
		rc = spdk_bdev_writev_blocks_with_md(bdev_node->base_desc, ch->base_ch,
						     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						     bdev_io->u.bdev.md_buf,
						     lba, bdev_io->u.bdev.num_blocks,
						     _zone_block_complete_write, bdev_io);
	}

	return rc;

write_fail:
	pthread_spin_unlock(&zone->lock);
	return rc;
}

static void
_zone_block_complete_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_reqeust.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static int
zone_block_read(struct bdev_zone_block *bdev_node, struct zone_block_io_channel *ch,
		struct spdk_bdev_io *bdev_io)
{
	struct block_zone *zone;
	uint64_t len = bdev_io->u.bdev.num_blocks;
	uint64_t lba = bdev_io->u.bdev.offset_blocks;
	int rc;

	zone = zone_block_get_zone_containing_lba(bdev_node, lba);
	if (!zone) {
		SPDK_ERRLOG("Trying to read from invalid zone (lba 0x%" PRIx64 ")\n", lba);
		return -EINVAL;
	}

	if ((lba + len) > (zone->zone_info.zone_id + zone->zone_info.capacity)) {
		SPDK_ERRLOG("Read exceeds zone capacity (lba 0x%" PRIx64 ", len 0x%" PRIx64 ")\n", lba, len);
		return -EINVAL;
	}

	if (bdev_io->u.bdev.md_buf == NULL) {
		rc = spdk_bdev_readv_blocks(bdev_node->base_desc, ch->base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, lba,
					    len, _zone_block_complete_read,
					    bdev_io);
	} else {
		rc = spdk_bdev_readv_blocks_with_md(bdev_node->base_desc, ch->base_ch,
						    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.md_buf,
						    lba, len,
						    _zone_block_complete_read, bdev_io);
	}

	return rc;
}

static void
zone_block_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_zone_block *bdev_node = SPDK_CONTAINEROF(bdev_io->bdev, struct bdev_zone_block, bdev);
	struct zone_block_io_channel *dev_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		rc = zone_block_get_zone_info(bdev_node, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = zone_block_zone_management(bdev_node, dev_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		rc = zone_block_write(bdev_node, dev_ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		rc = zone_block_read(bdev_node, dev_ch, bdev_io);
		break;
	default:
		SPDK_ERRLOG("vbdev_block: unknown I/O type %u\n", bdev_io->type);
		rc = -ENOTSUP;
		break;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_WARNLOG("ENOMEM, start to queue io for vbdev.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
zone_block_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
zone_block_get_io_channel(void *ctx)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;

	return spdk_get_io_channel(bdev_node);
}

static int
zone_block_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_zone_block *bdev_node = (struct bdev_zone_block *)ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(bdev_node->base_desc);

	spdk_json_write_name(w, "zoned_block");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&bdev_node->bdev));
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
	spdk_json_write_named_uint64(w, "zone_capacity", bdev_node->zone_capacity);
	spdk_json_write_named_uint64(w, "optimal_open_zones", bdev_node->bdev.optimal_open_zones);
	spdk_json_write_object_end(w);

	return 0;
}

/* When we register our vbdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table zone_block_fn_table = {
	.destruct		= zone_block_destruct,
	.submit_request		= zone_block_submit_request,
	.io_type_supported	= zone_block_io_type_supported,
	.get_io_channel		= zone_block_get_io_channel,
	.dump_info_json		= zone_block_dump_info_json,
};

static void
zone_block_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct bdev_zone_block *bdev_node, *tmp;

	TAILQ_FOREACH_SAFE(bdev_node, &g_bdev_nodes, link, tmp) {
		if (bdev_find == spdk_bdev_desc_get_bdev(bdev_node->base_desc)) {
			spdk_bdev_unregister(&bdev_node->bdev, NULL, NULL);
		}
	}
}

static void
zone_block_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			      void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		zone_block_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static int
_zone_block_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct zone_block_io_channel *bdev_ch = ctx_buf;
	struct bdev_zone_block *bdev_node = io_device;

	bdev_ch->base_ch = spdk_bdev_get_io_channel(bdev_node->base_desc);
	if (!bdev_ch->base_ch) {
		return -ENOMEM;
	}

	return 0;
}

static void
_zone_block_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct zone_block_io_channel *bdev_ch = ctx_buf;

	spdk_put_io_channel(bdev_ch->base_ch);
}

static int
zone_block_insert_name(const char *bdev_name, const char *vbdev_name, uint64_t zone_capacity,
		       uint64_t optimal_open_zones)
{
	struct bdev_zone_block_config *name;

	TAILQ_FOREACH(name, &g_bdev_configs, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("block zoned bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
		if (strcmp(bdev_name, name->bdev_name) == 0) {
			SPDK_ERRLOG("base bdev %s already claimed\n", bdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(*name));
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

	name->zone_capacity = zone_capacity;
	name->optimal_open_zones = optimal_open_zones;

	TAILQ_INSERT_TAIL(&g_bdev_configs, name, link);

	return 0;
}

static int
zone_block_init_zone_info(struct bdev_zone_block *bdev_node)
{
	size_t i;
	struct block_zone *zone;
	int rc = 0;

	for (i = 0; i < bdev_node->num_zones; i++) {
		zone = &bdev_node->zones[i];
		zone->zone_info.zone_id = bdev_node->bdev.zone_size * i;
		zone->zone_info.capacity = bdev_node->zone_capacity;
		zone->zone_info.write_pointer = zone->zone_info.zone_id + zone->zone_info.capacity;
		zone->zone_info.state = SPDK_BDEV_ZONE_STATE_FULL;
		if (pthread_spin_init(&zone->lock, PTHREAD_PROCESS_PRIVATE)) {
			SPDK_ERRLOG("pthread_spin_init() failed\n");
			rc = -ENOMEM;
			break;
		}
	}

	if (rc) {
		for (; i > 0; i--) {
			pthread_spin_destroy(&bdev_node->zones[i - 1].lock);
		}
	}

	return rc;
}

static int
zone_block_register(const char *base_bdev_name)
{
	struct spdk_bdev_desc *base_desc;
	struct spdk_bdev *base_bdev;
	struct bdev_zone_block_config *name, *tmp;
	struct bdev_zone_block *bdev_node;
	uint64_t zone_size;
	int rc = 0;

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the bdev_node & bdev accordingly.
	 */
	TAILQ_FOREACH_SAFE(name, &g_bdev_configs, link, tmp) {
		if (strcmp(name->bdev_name, base_bdev_name) != 0) {
			continue;
		}

		rc = spdk_bdev_open_ext(base_bdev_name, true, zone_block_base_bdev_event_cb,
					NULL, &base_desc);
		if (rc == -ENODEV) {
			return -ENODEV;
		} else if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", base_bdev_name);
			goto free_config;
		}

		base_bdev = spdk_bdev_desc_get_bdev(base_desc);

		if (spdk_bdev_is_zoned(base_bdev)) {
			SPDK_ERRLOG("Base bdev %s is already a zoned bdev\n", base_bdev_name);
			rc = -EEXIST;
			goto zone_exist;
		}

		bdev_node = calloc(1, sizeof(struct bdev_zone_block));
		if (!bdev_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node\n");
			goto zone_exist;
		}

		bdev_node->base_desc = base_desc;

		/* The base bdev that we're attaching to. */
		bdev_node->bdev.name = strdup(name->vbdev_name);
		if (!bdev_node->bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate bdev_node name\n");
			goto strdup_failed;
		}

		zone_size = spdk_align64pow2(name->zone_capacity);
		if (zone_size == 0) {
			rc = -EINVAL;
			SPDK_ERRLOG("invalid zone size\n");
			goto roundup_failed;
		}

		bdev_node->zone_shift = spdk_u64log2(zone_size);
		bdev_node->num_zones = base_bdev->blockcnt / zone_size;

		/* Align num_zones to optimal_open_zones */
		bdev_node->num_zones -= bdev_node->num_zones % name->optimal_open_zones;
		bdev_node->zones = calloc(bdev_node->num_zones, sizeof(struct block_zone));
		if (!bdev_node->zones) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate zones\n");
			goto calloc_failed;
		}

		bdev_node->bdev.product_name = "zone_block";

		/* Copy some properties from the underlying base bdev. */
		bdev_node->bdev.write_cache = base_bdev->write_cache;
		bdev_node->bdev.required_alignment = base_bdev->required_alignment;
		bdev_node->bdev.optimal_io_boundary = base_bdev->optimal_io_boundary;

		bdev_node->bdev.blocklen = base_bdev->blocklen;
		bdev_node->bdev.blockcnt = bdev_node->num_zones * zone_size;

		if (bdev_node->num_zones * name->zone_capacity != base_bdev->blockcnt) {
			SPDK_DEBUGLOG(vbdev_zone_block,
				      "Lost %" PRIu64 " blocks due to zone capacity and base bdev size misalignment\n",
				      base_bdev->blockcnt - bdev_node->num_zones * name->zone_capacity);
		}

		bdev_node->bdev.write_unit_size = base_bdev->write_unit_size;

		bdev_node->bdev.md_interleave = base_bdev->md_interleave;
		bdev_node->bdev.md_len = base_bdev->md_len;
		bdev_node->bdev.dif_type = base_bdev->dif_type;
		bdev_node->bdev.dif_is_head_of_md = base_bdev->dif_is_head_of_md;
		bdev_node->bdev.dif_check_flags = base_bdev->dif_check_flags;

		bdev_node->bdev.zoned = true;
		bdev_node->bdev.ctxt = bdev_node;
		bdev_node->bdev.fn_table = &zone_block_fn_table;
		bdev_node->bdev.module = &bdev_zoned_if;

		/* bdev specific info */
		bdev_node->bdev.zone_size = zone_size;

		bdev_node->zone_capacity = name->zone_capacity;
		bdev_node->bdev.optimal_open_zones = name->optimal_open_zones;
		bdev_node->bdev.max_open_zones = 0;
		rc = zone_block_init_zone_info(bdev_node);
		if (rc) {
			SPDK_ERRLOG("could not init zone info\n");
			goto zone_info_failed;
		}

		TAILQ_INSERT_TAIL(&g_bdev_nodes, bdev_node, link);

		spdk_io_device_register(bdev_node, _zone_block_ch_create_cb, _zone_block_ch_destroy_cb,
					sizeof(struct zone_block_io_channel),
					name->vbdev_name);

		/* Save the thread where the base device is opened */
		bdev_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(base_bdev, base_desc, bdev_node->bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", base_bdev_name);
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
	TAILQ_REMOVE(&g_bdev_nodes, bdev_node, link);
	spdk_io_device_unregister(bdev_node, NULL);
zone_info_failed:
	free(bdev_node->zones);
calloc_failed:
roundup_failed:
	free(bdev_node->bdev.name);
strdup_failed:
	free(bdev_node);
zone_exist:
	spdk_bdev_close(base_desc);
free_config:
	zone_block_remove_config(name);
	return rc;
}

int
vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name, uint64_t zone_capacity,
			uint64_t optimal_open_zones)
{
	int rc = 0;

	if (zone_capacity == 0) {
		SPDK_ERRLOG("Zone capacity can't be 0\n");
		return -EINVAL;
	}

	if (optimal_open_zones == 0) {
		SPDK_ERRLOG("Optimal open zones can't be 0\n");
		return -EINVAL;
	}

	/* Insert the bdev into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = zone_block_insert_name(bdev_name, vbdev_name, zone_capacity, optimal_open_zones);
	if (rc) {
		return rc;
	}

	rc = zone_block_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, even though the bdev is not present at this time it may
		 * still show up later.
		 */
		rc = 0;
	}
	return rc;
}

void
vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_zone_block_config *name_node;
	struct spdk_bdev *bdev = NULL;

	bdev = spdk_bdev_get_by_name(name);
	if (!bdev || bdev->module != &bdev_zoned_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	TAILQ_FOREACH(name_node, &g_bdev_configs, link) {
		if (strcmp(name_node->vbdev_name, bdev->name) == 0) {
			zone_block_remove_config(name_node);
			break;
		}
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static void
zone_block_examine(struct spdk_bdev *bdev)
{
	zone_block_register(bdev->name);

	spdk_bdev_module_examine_done(&bdev_zoned_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_zone_block)
