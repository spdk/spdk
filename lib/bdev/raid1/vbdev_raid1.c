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

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"


SPDK_DECLARE_BDEV_MODULE(raid1);

#define MAX_CHILD_DEVS  8

struct spdk_vbdev_base;
typedef void (*spdk_vbdev_base_free_fn)(struct spdk_vbdev_base *base);

struct spdk_vbdev_base {
	struct spdk_bdev			*bdev[MAX_CHILD_DEVS];
	struct spdk_bdev_desc		*desc[MAX_CHILD_DEVS];

	uint32_t					num_childs;
	uint32_t					ref;
	uint32_t					channel_size;
	bool						claimed;

	struct spdk_bdev_fn_table	*fn_table;
	spdk_vbdev_base_free_fn		base_free_fn;
	struct spdk_bdev_module_if	*module;
	spdk_io_channel_create_cb	ch_create_cb;
	spdk_io_channel_destroy_cb	ch_destroy_cb;
};

struct raid1_disk {
	struct spdk_bdev		bdev;
	struct spdk_vbdev_base	*base;
	struct raid1_disk	*next;
};

struct raid1_conf {
	char				*name;
	char				*first_child_name;
	char				*second_child_name;
	uint32_t 			real_child_num;
	struct raid1_conf	*next;
};

static struct raid1_disk *g_raid1_disk_head = NULL;
static struct raid1_conf *g_raid1_conf_head = NULL;

struct raid1_channel {
	struct raid1_disk			*disk;
	struct spdk_io_channel		*base_ch[MAX_CHILD_DEVS];
	uint32_t					next_child_idx;
};

struct raid1_io_task {
	uint32_t child_io_num;
};

static void
spdk_vbdev_base_free(struct spdk_vbdev_base *base)
{
	uint32_t i;

	for (i = 0; i < base->num_childs; i++) {
		if (base->bdev[i]->claim_module != NULL) {
			spdk_bdev_module_release_bdev(base->bdev[i]);
		}
		if (base->desc[i]) {
			spdk_bdev_close(base->desc[i]);
			base->desc[i] = NULL;
		}
	}

	base->base_free_fn(base);
}

static int
vbdev_raid1_destruct(void *ctx)
{
	struct raid1_disk *raid1_disk = ctx;

	spdk_vbdev_base_free(raid1_disk->base);
	return 0;
}

static int
vbdev_raid1_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct raid1_disk *rdisk = ctx;
	struct spdk_bdev *base_bdev;

	spdk_json_write_name(w, "raid1_disk");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "bdev");
	spdk_json_write_string(w, rdisk->bdev.name);
	spdk_json_write_name(w, "child_bdev_num");
	spdk_json_write_int32(w, rdisk->base->num_childs);
	TAILQ_FOREACH(base_bdev, &rdisk->bdev.base_bdevs, base_bdev_link) {
		spdk_json_write_name(w, "child_bdev");
		spdk_json_write_string(w, base_bdev->name);
	}

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_raid1_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *parent_io = cb_arg;
	struct raid1_io_task *task = (struct raid1_io_task *)parent_io->driver_ctx;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	/* when all split I/Os have completed,
	   then complete the blockdev/raid1 I/O */
	if (--task->child_io_num == 0) {
		spdk_bdev_io_complete(parent_io, status);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_raid1_get_buf_cb(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct raid1_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct raid1_disk *rdisk = ch->disk;
	struct spdk_io_channel *base_ch;
	struct spdk_bdev_desc *base_desc;
	struct raid1_io_task *task;
	int rc;

	task = (struct raid1_io_task *)bdev_io->driver_ctx;
	task->child_io_num = 1;
	assert(ch->next_child_idx < rdisk->base->num_childs);
	/* select the child device in a round robin way */
	base_ch = ch->base_ch[ch->next_child_idx];
	base_desc = rdisk->base->desc[ch->next_child_idx];
	ch->next_child_idx = (ch->next_child_idx + 1) % rdisk->base->num_childs;

	rc = spdk_bdev_readv_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt, 0,
				    bdev_io->u.bdev.num_blocks, vbdev_raid1_complete_io,
				    bdev_io);
	if (rc < 0) {
		task->child_io_num--;
	}
}

static void
_vbdev_raid1_submit_request(struct raid1_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid1_disk *rdisk = ch->disk;
	struct spdk_io_channel *base_ch;
	struct spdk_bdev_desc *base_desc;
	struct raid1_io_task *task;
	int rc = 0;
	uint32_t i;

	task = (struct raid1_io_task *)bdev_io->driver_ctx;
	task->child_io_num = rdisk->base->num_childs;
	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, vbdev_raid1_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);

		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		for (i = 0; i < rdisk->base->num_childs; i++) {
			base_ch = ch->base_ch[i];
			base_desc = rdisk->base->desc[i];
			rc = spdk_bdev_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
						     bdev_io->u.bdev.iovcnt, 0,
						     bdev_io->u.bdev.num_blocks, vbdev_raid1_complete_io,
						     bdev_io);
			if (rc < 0) {
				task->child_io_num--;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		for (i = 0; i < rdisk->base->num_childs; i++) {
			base_ch = ch->base_ch[i];
			base_desc = rdisk->base->desc[i];
			rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, bdev_io->u.bdev.offset_blocks,
							   bdev_io->u.bdev.num_blocks, vbdev_raid1_complete_io, bdev_io);
			if (rc < 0) {
				task->child_io_num--;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		for (i = 0; i < rdisk->base->num_childs; i++) {
			base_ch = ch->base_ch[i];
			base_desc = rdisk->base->desc[i];
			rc = spdk_bdev_unmap_blocks(base_desc, base_ch, bdev_io->u.bdev.offset_blocks,
						    bdev_io->u.bdev.num_blocks, vbdev_raid1_complete_io,
						    bdev_io);
			if (rc < 0) {
				task->child_io_num--;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		for (i = 0; i < rdisk->base->num_childs; i++) {
			base_ch = ch->base_ch[i];
			base_desc = rdisk->base->desc[i];
			rc = spdk_bdev_flush_blocks(base_desc, base_ch, bdev_io->u.bdev.offset_blocks,
						    bdev_io->u.bdev.num_blocks, vbdev_raid1_complete_io,
						    bdev_io);
			if (rc < 0) {
				task->child_io_num--;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		for (i = 0; i < rdisk->base->num_childs; i++) {
			base_ch = ch->base_ch[i];
			base_desc = rdisk->base->desc[i];
			rc = spdk_bdev_reset(base_desc, base_ch, vbdev_raid1_complete_io,
					     bdev_io);
			if (rc < 0) {
				task->child_io_num--;
			}
		}
		break;
	default:
		SPDK_ERRLOG("vbdev_raid1: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (!task->child_io_num) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_raid1_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct raid1_channel *ch = spdk_io_channel_get_ctx(_ch);

	_vbdev_raid1_submit_request(ch, bdev_io);
}

static bool
vbdev_raid1_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct raid1_disk *rdisk = _part;

	/* suppose all the type of child devs are same including size */
	return rdisk->base->bdev[0]->fn_table->io_type_supported(rdisk->base->bdev[0], io_type);
}

static struct spdk_io_channel *
vbdev_raid1_get_io_channel(void *ctx)

{
	struct raid1_disk *rdisk = ctx;

	return spdk_get_io_channel(&rdisk->base);
}

static struct spdk_bdev_fn_table vbdev_raid1_fn_table = {
	.destruct			= vbdev_raid1_destruct,
	.submit_request		= vbdev_raid1_submit_request,
	.dump_config_json	= vbdev_raid1_dump_config_json,
	.get_io_channel		= vbdev_raid1_get_io_channel,
	.io_type_supported	= vbdev_raid1_io_type_supported,
};

static int
vbdev_raid1_get_ctx_size(void)
{
	return sizeof(struct raid1_io_task);
}

static int
spdk_vbdev_raid1_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct raid1_disk *rdisk = SPDK_CONTAINEROF(io_device, struct raid1_disk, base);
	struct raid1_channel *ch = ctx_buf;
	uint32_t i;

	ch->disk = rdisk;
	ch->next_child_idx = 0;

	for (i = 0; i < rdisk->base->num_childs; i++) {
		ch->base_ch[i] = spdk_bdev_get_io_channel(rdisk->base->desc[i]);
		assert(ch != NULL);
	}

	if (rdisk->base->ch_create_cb) {
		return rdisk->base->ch_create_cb(io_device, ctx_buf);
	} else {
		return 0;
	}
}

static void
spdk_vbdev_raid1_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct raid1_disk *rdisk = SPDK_CONTAINEROF(io_device, struct raid1_disk, base);
	struct raid1_channel *ch = ctx_buf;
	uint32_t i;

	if (rdisk->base->ch_destroy_cb) {
		rdisk->base->ch_destroy_cb(io_device, ctx_buf);
	}

	for (i = 0; i < rdisk->base->num_childs; i++) {
		spdk_put_io_channel(ch->base_ch[i]);
	}
}

static void
spdk_vbdev_free(struct raid1_disk *rdisk)
{
	struct spdk_vbdev_base *base;

	assert(rdisk);
	assert(rdisk->base);

	base = rdisk->base;
	spdk_io_device_unregister(&rdisk->base, NULL);
	free(rdisk->bdev.name);

	if (__sync_sub_and_fetch(&base->ref, 1) == 0) {
		spdk_vbdev_base_free(base);
	}

	free(rdisk);
}

static int
spdk_vbdev_base_construct(struct spdk_vbdev_base *base, struct spdk_bdev **bdev,
			  uint32_t bdev_num, spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module_if *module,
			  struct spdk_bdev_fn_table *fn_table, spdk_vbdev_base_free_fn free_fn,
			  uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
			  spdk_io_channel_destroy_cb ch_destroy_cb)
{
	int rc;
	uint32_t i;

	base->claimed = false;
	base->num_childs = bdev_num;
	base->ref = 0;
	base->module = module;
	base->fn_table = fn_table;
	base->base_free_fn = free_fn;
	base->channel_size = channel_size;
	base->ch_create_cb = ch_create_cb;
	base->ch_destroy_cb = ch_destroy_cb;


	for (i = 0; i < bdev_num; i++) {
		base->bdev[i] = bdev[i];
		rc = spdk_bdev_open(bdev[i], false, remove_cb, bdev, &base->desc[i]);
		if (rc) {
			SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev[i]));
			return -1;
		}
	}

	return 0;
}

static int
spdk_raid1_disk_construct(struct raid1_disk *rdisk, struct spdk_vbdev_base *base,
			  char *name, char *product_name)
{
	uint32_t i;

	rdisk->bdev.name = name;
	/* Assuming same type of block dev for both child devs */
	/* for block length, we need to choose the minimal */
	rdisk->bdev.blocklen = base->bdev[0]->blocklen;
	for (i = 1; i < base->num_childs; i++) {
		if (rdisk->bdev.blocklen > base->bdev[i]->blocklen) {
			rdisk->bdev.blocklen = base->bdev[i]->blocklen;
		}
	}
	rdisk->bdev.blockcnt = base->bdev[0]->blockcnt;
	rdisk->bdev.write_cache = base->bdev[0]->write_cache;
	rdisk->bdev.need_aligned_buffer = base->bdev[0]->need_aligned_buffer;
	rdisk->bdev.product_name = product_name;
	rdisk->bdev.ctxt = rdisk;
	rdisk->bdev.module = base->module;
	rdisk->bdev.fn_table = base->fn_table;

	__sync_fetch_and_add(&base->ref, 1);
	rdisk->base = base;

	if (!base->claimed) {
		int rc;

		for (i = 0; i < base->num_childs; i++) {
			rc = spdk_bdev_module_claim_bdev(base->bdev[i], base->desc[i], base->module);
			if (rc) {
				SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base->bdev[i]));
				return -1;
			}
		}
		base->claimed = true;
	}

	spdk_io_device_register(&rdisk->base, spdk_vbdev_raid1_channel_create_cb,
				spdk_vbdev_raid1_channel_destroy_cb,
				base->channel_size);
	spdk_vbdev_register(&rdisk->bdev, base->bdev, base->num_childs);

	rdisk->next = g_raid1_disk_head;
	g_raid1_disk_head = rdisk;

	return 0;
}

static void
spdk_raid1_free_base(struct spdk_vbdev_base *base)
{
	free(base);
}

static int
spdk_vbdev_raid1_create(struct raid1_conf *conf)
{
	struct raid1_disk *rdisk;
	struct spdk_bdev *bdevs[2];
	struct spdk_vbdev_base *base;
	int rc;
	char *name;

	bdevs[0] = spdk_bdev_get_by_name(conf->first_child_name);
	bdevs[1] = spdk_bdev_get_by_name(conf->second_child_name);

	if (!bdevs[0] || !bdevs[1]) {
		return -1;
	}

	base = calloc(1, sizeof(*base));
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	rc = spdk_vbdev_base_construct(base, bdevs, 2, NULL,
				       SPDK_GET_BDEV_MODULE(raid1), &vbdev_raid1_fn_table,
				       spdk_raid1_free_base, sizeof(struct raid1_channel), NULL, NULL);

	if (rc) {
		spdk_vbdev_base_free(base);
		SPDK_ERRLOG("could not construct vbdev base\n");
		return -1;
	}

	rdisk = calloc(1, sizeof(*rdisk));
	if (!rdisk) {
		SPDK_ERRLOG("Unable to allocate rdisk\n");
		return -1;
	}

	name = spdk_sprintf_alloc("%s", conf->name);
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		spdk_vbdev_base_free(base);
		free(rdisk);
		return -1;
	}

	rc = spdk_raid1_disk_construct(rdisk, base, name, "Raid1 disk");
	if (rc < 0) {
		spdk_vbdev_base_free(base);
		free(name);
		free(rdisk);
	}

	return 0;
}

static void
vbdev_raid1_examine(struct spdk_bdev *bdev)
{
	struct spdk_conf_section *sp;
	struct raid1_conf *conf;

	sp = spdk_conf_find_section(NULL, "Raid1");
	if (sp == NULL) {
		goto ret;
	}

	conf = g_raid1_conf_head;
	while (conf != NULL) {
		if (!strcmp(conf->first_child_name, bdev->name) ||
		    !strcmp(conf->second_child_name, bdev->name)) {
			conf->real_child_num++;
			if (conf->real_child_num == 2) {
				if (spdk_vbdev_raid1_create(conf)) {
					SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_RAID1, "could not create raid1 vbdev %s\n", conf->name);
				}
			}
			goto ret;
		}
		conf = conf->next;
	}

ret:
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(raid1));
}

static int
vbdev_raid1_init(void)
{

	struct spdk_conf_section *sp;
	int i;
	struct raid1_conf conf;
	struct raid1_conf *conf_ptr;

	sp = spdk_conf_find_section(NULL, "Raid1");
	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {
		memset(&conf, 0, sizeof(conf));
		if (!spdk_conf_section_get_nval(sp, "Raid1", i)) {
			break;
		}

		conf.name = spdk_conf_section_get_nmval(sp, "Raid1", i, 0);
		if (!conf.name) {
			SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_RAID1, "Raid configuration missing raid bdev name\n");
			continue;
		}

		conf.first_child_name = spdk_conf_section_get_nmval(sp, "Raid1", i, 1);
		conf.second_child_name = spdk_conf_section_get_nmval(sp, "Raid1", i, 2);
		if (!conf.first_child_name || !conf.second_child_name ||
		    !strcmp(conf.first_child_name, conf.second_child_name)) {
			SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_RAID1, "Invaid child device name: first_child_name=%s"
				      "second_child_name=%s\n", conf.first_child_name, conf.second_child_name);
			continue;
		}

		conf_ptr = calloc(1, sizeof(*conf_ptr));
		if (!conf_ptr) {
			return -1;
		}

		*conf_ptr = conf;
		conf_ptr->next = g_raid1_conf_head;
		g_raid1_conf_head = conf_ptr;
	}

	return 0;
}

static void
vbdev_raid1_finish(void)
{
	struct raid1_conf *conf;
	struct raid1_disk *disk;

	while (g_raid1_conf_head != NULL) {
		conf = g_raid1_conf_head;
		g_raid1_conf_head = conf->next;
		free(conf);
	}

	while (g_raid1_disk_head != NULL) {
		disk = g_raid1_disk_head;
		g_raid1_disk_head = disk->next;
		spdk_vbdev_free(disk);
	}
}

SPDK_BDEV_MODULE_REGISTER(raid1, vbdev_raid1_init, vbdev_raid1_finish, NULL,
			  vbdev_raid1_get_ctx_size, vbdev_raid1_examine)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_raid1", SPDK_TRACE_VBDEV_RAID1)
