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

/*
 * This is a simple example of a virtual block device that takes a single
 * bdev and slices it into multiple smaller bdevs.
 */

#include "vbdev_split.h"

#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

struct spdk_vbdev_split_config {
	char *base_bdev;
	unsigned split_count;
	uint64_t split_size_mb;

	struct spdk_bdev_part_base *split_base;
	bool removed;

	TAILQ_ENTRY(spdk_vbdev_split_config) tailq;
};

static TAILQ_HEAD(, spdk_vbdev_split_config) g_split_config = TAILQ_HEAD_INITIALIZER(
			g_split_config);
static SPDK_BDEV_PART_TAILQ g_split_disks = TAILQ_HEAD_INITIALIZER(g_split_disks);

struct vbdev_split_channel {
	struct spdk_bdev_part_channel	part_ch;
};

struct vbdev_split_bdev_io {
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

static void vbdev_split_del_config(struct spdk_vbdev_split_config *cfg);

static int vbdev_split_init(void);
static void vbdev_split_fini(void);
static void vbdev_split_examine(struct spdk_bdev *bdev);
static int vbdev_split_config_json(struct spdk_json_write_ctx *w);
static int vbdev_split_get_ctx_size(void);

static void
vbdev_split_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

static struct spdk_bdev_module split_if = {
	.name = "split",
	.module_init = vbdev_split_init,
	.module_fini = vbdev_split_fini,
	.get_ctx_size = vbdev_split_get_ctx_size,
	.examine_config = vbdev_split_examine,
	.config_json = vbdev_split_config_json,
};

SPDK_BDEV_MODULE_REGISTER(&split_if)

static void
vbdev_split_base_free(void *ctx)
{
	struct spdk_vbdev_split_config *cfg = ctx;

	cfg->split_base = NULL;
	if (cfg->removed) {
		vbdev_split_del_config(cfg);
	}
}

static int
vbdev_split_destruct(void *ctx)
{
	struct spdk_bdev_part *part = ctx;

	return spdk_bdev_part_free(part);
}

static void
vbdev_split_base_bdev_hotremove_cb(void *_base_bdev)
{
	spdk_bdev_part_base_hotremove(_base_bdev, &g_split_disks);
}

static void
vbdev_split_resubmit_io(void *arg)
{
	struct vbdev_split_bdev_io *split_io = (struct vbdev_split_bdev_io *)arg;

	vbdev_split_submit_request(split_io->ch, split_io->bdev_io);
}

static void
vbdev_split_queue_io(struct vbdev_split_bdev_io *split_io)
{
	int rc;

	split_io->bdev_io_wait.bdev = split_io->bdev_io->bdev;
	split_io->bdev_io_wait.cb_fn = vbdev_split_resubmit_io;
	split_io->bdev_io_wait.cb_arg = split_io;

	rc = spdk_bdev_queue_io_wait(split_io->bdev_io->bdev,
				     split_io->ch, &split_io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_split_queue_io, rc=%d\n", rc);
		spdk_bdev_io_complete(split_io->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_split_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_split_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct vbdev_split_bdev_io *io_ctx = (struct vbdev_split_bdev_io *)bdev_io->driver_ctx;
	int rc;

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_SPLIT, "split: no memory, queue io.\n");
			io_ctx->ch = _ch;
			io_ctx->bdev_io = bdev_io;
			vbdev_split_queue_io(io_ctx);
		} else {
			SPDK_ERRLOG("split: error on io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static int
vbdev_split_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *split_base_bdev = spdk_bdev_part_get_base_bdev(part);
	uint64_t offset_blocks = spdk_bdev_part_get_offset_blocks(part);

	spdk_json_write_name(w, "split");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(split_base_bdev));
	spdk_json_write_name(w, "offset_blocks");
	spdk_json_write_uint64(w, offset_blocks);

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_split_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

static struct spdk_bdev_fn_table vbdev_split_fn_table = {
	.destruct		= vbdev_split_destruct,
	.submit_request		= vbdev_split_submit_request,
	.dump_info_json		= vbdev_split_dump_info_json,
	.write_config_json	= vbdev_split_write_config_json
};

static int
vbdev_split_create(struct spdk_vbdev_split_config *cfg)
{
	uint64_t split_size_blocks, offset_blocks;
	uint64_t split_count, max_split_count;
	uint64_t mb = 1024 * 1024;
	uint64_t i;
	int rc;
	char *name;
	struct spdk_bdev *base_bdev;
	struct spdk_bdev *split_base_bdev;
	struct bdev_part_tailq *split_base_tailq;

	assert(cfg->split_count > 0);

	base_bdev = spdk_bdev_get_by_name(cfg->base_bdev);
	if (!base_bdev) {
		return -ENODEV;
	}

	if (cfg->split_size_mb) {
		if (((cfg->split_size_mb * mb) % base_bdev->blocklen) != 0) {
			SPDK_ERRLOG("Split size %" PRIu64 " MB is not possible with block size "
				    "%" PRIu32 "\n",
				    cfg->split_size_mb, base_bdev->blocklen);
			return -EINVAL;
		}
		split_size_blocks = (cfg->split_size_mb * mb) / base_bdev->blocklen;
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_SPLIT, "Split size %" PRIu64 " MB specified by user\n",
			      cfg->split_size_mb);
	} else {
		split_size_blocks = base_bdev->blockcnt / cfg->split_count;
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_SPLIT, "Split size not specified by user\n");
	}

	max_split_count = base_bdev->blockcnt / split_size_blocks;
	split_count = cfg->split_count;
	if (split_count > max_split_count) {
		SPDK_WARNLOG("Split count %" PRIu64 " is greater than maximum possible split count "
			     "%" PRIu64 " - clamping\n", split_count, max_split_count);
		split_count = max_split_count;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VBDEV_SPLIT, "base_bdev: %s split_count: %" PRIu64
		      " split_size_blocks: %" PRIu64 "\n",
		      spdk_bdev_get_name(base_bdev), split_count, split_size_blocks);

	cfg->split_base = spdk_bdev_part_base_construct(base_bdev,
			  vbdev_split_base_bdev_hotremove_cb,
			  &split_if, &vbdev_split_fn_table,
			  &g_split_disks, vbdev_split_base_free, cfg,
			  sizeof(struct vbdev_split_channel), NULL, NULL);
	if (!cfg->split_base) {
		SPDK_ERRLOG("Cannot construct bdev part base\n");
		return -ENOMEM;
	}

	offset_blocks = 0;
	for (i = 0; i < split_count; i++) {
		struct spdk_bdev_part *d;

		d = calloc(1, sizeof(*d));
		if (d == NULL) {
			SPDK_ERRLOG("could not allocate bdev part\n");
			rc = -ENOMEM;
			goto err;
		}

		name = spdk_sprintf_alloc("%sp%" PRIu64, cfg->base_bdev, i);
		if (!name) {
			SPDK_ERRLOG("could not allocate name\n");
			free(d);
			rc = -ENOMEM;
			goto err;
		}

		rc = spdk_bdev_part_construct(d, cfg->split_base, name, offset_blocks, split_size_blocks,
					      "Split Disk");
		free(name);
		if (rc) {
			SPDK_ERRLOG("could not construct bdev part\n");
			/* spdk_bdev_part_construct will free name if it fails */
			free(d);
			rc = -ENOMEM;
			goto err;
		}

		offset_blocks += split_size_blocks;
	}

	return 0;
err:
	split_base_bdev = spdk_bdev_part_base_get_bdev(cfg->split_base);
	split_base_tailq = spdk_bdev_part_base_get_tailq(cfg->split_base);
	cfg->removed = true;
	spdk_bdev_part_base_hotremove(split_base_bdev, split_base_tailq);
	return rc;
}

static void
vbdev_split_del_config(struct spdk_vbdev_split_config *cfg)
{
	TAILQ_REMOVE(&g_split_config, cfg, tailq);
	free(cfg->base_bdev);
	free(cfg);
}

static void
vbdev_split_destruct_config(struct spdk_vbdev_split_config *cfg)
{
	struct spdk_bdev *split_base_bdev;
	struct bdev_part_tailq *split_base_tailq;

	cfg->removed = true;
	if (cfg->split_base != NULL) {
		split_base_bdev = spdk_bdev_part_base_get_bdev(cfg->split_base);
		split_base_tailq = spdk_bdev_part_base_get_tailq(cfg->split_base);
		spdk_bdev_part_base_hotremove(split_base_bdev, split_base_tailq);
	} else {
		vbdev_split_del_config(cfg);
	}
}

static void
vbdev_split_clear_config(void)
{
	struct spdk_vbdev_split_config *cfg, *tmp_cfg;

	TAILQ_FOREACH_SAFE(cfg, &g_split_config, tailq, tmp_cfg) {
		vbdev_split_destruct_config(cfg);
	}
}

static struct spdk_vbdev_split_config *
vbdev_split_config_find_by_base_name(const char *base_bdev_name)
{
	struct spdk_vbdev_split_config *cfg;

	TAILQ_FOREACH(cfg, &g_split_config, tailq) {
		if (strcmp(cfg->base_bdev, base_bdev_name) == 0) {
			return cfg;
		}
	}

	return NULL;
}

static int
vbdev_split_add_config(const char *base_bdev_name, unsigned split_count, uint64_t split_size,
		       struct spdk_vbdev_split_config **config)
{
	struct spdk_vbdev_split_config *cfg;
	assert(base_bdev_name);

	if (base_bdev_name == NULL) {
		SPDK_ERRLOG("Split bdev config: no base bdev provided.");
		return -EINVAL;
	}

	if (split_count == 0) {
		SPDK_ERRLOG("Split bdev config: split_count can't be 0.");
		return -EINVAL;
	}

	/* Check if we already have 'base_bdev_name' registered in config */
	cfg = vbdev_split_config_find_by_base_name(base_bdev_name);
	if (cfg) {
		SPDK_ERRLOG("Split bdev config for base bdev '%s' already exist.", base_bdev_name);
		return -EEXIST;
	}

	cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		SPDK_ERRLOG("calloc(): Out of memory");
		return -ENOMEM;
	}

	cfg->base_bdev = strdup(base_bdev_name);
	if (!cfg->base_bdev) {
		SPDK_ERRLOG("strdup(): Out of memory");
		free(cfg);
		return -ENOMEM;
	}

	cfg->split_count = split_count;
	cfg->split_size_mb = split_size;
	TAILQ_INSERT_TAIL(&g_split_config, cfg, tailq);
	if (config) {
		*config = cfg;
	}

	return 0;
}

static int
vbdev_split_init(void)
{

	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	const char *split_count_str;
	const char *split_size_str;
	int rc, i, split_count, split_size;

	sp = spdk_conf_find_section(NULL, "Split");
	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "Split", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("Split configuration missing bdev name\n");
			rc = -EINVAL;
			goto err;
		}

		split_count_str = spdk_conf_section_get_nmval(sp, "Split", i, 1);
		if (!split_count_str) {
			SPDK_ERRLOG("Split configuration missing split count\n");
			rc = -EINVAL;
			goto err;
		}

		split_count = atoi(split_count_str);
		if (split_count < 1) {
			SPDK_ERRLOG("Invalid Split count %d\n", split_count);
			rc = -EINVAL;
			goto err;
		}

		/* Optional split size in MB */
		split_size = 0;
		split_size_str = spdk_conf_section_get_nmval(sp, "Split", i, 2);
		if (split_size_str) {
			split_size = atoi(split_size_str);
			if (split_size <= 0) {
				SPDK_ERRLOG("Invalid Split size %d\n", split_size);
				rc = -EINVAL;
				goto err;
			}
		}

		rc = vbdev_split_add_config(base_bdev_name, split_count, split_size, NULL);
		if (rc != 0) {
			goto err;
		}
	}

	return 0;
err:
	vbdev_split_clear_config();
	return rc;
}

static void
vbdev_split_fini(void)
{
	vbdev_split_clear_config();
}

static void
vbdev_split_examine(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_split_config *cfg = vbdev_split_config_find_by_base_name(bdev->name);

	if (cfg != NULL && cfg->removed == false) {
		assert(cfg->split_base == NULL);

		if (vbdev_split_create(cfg)) {
			SPDK_ERRLOG("could not split bdev %s\n", bdev->name);
		}
	}
	spdk_bdev_module_examine_done(&split_if);
}

static int
vbdev_split_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vbdev_split_config *cfg;

	TAILQ_FOREACH(cfg, &g_split_config, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "construct_split_vbdev");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev", cfg->base_bdev);
		spdk_json_write_named_uint32(w, "split_count", cfg->split_count);
		spdk_json_write_named_uint64(w, "split_size_mb", cfg->split_size_mb);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	return 0;
}

int
create_vbdev_split(const char *base_bdev_name, unsigned split_count, uint64_t split_size_mb)
{
	int rc;
	struct spdk_vbdev_split_config *cfg;

	rc = vbdev_split_add_config(base_bdev_name, split_count, split_size_mb, &cfg);
	if (rc) {
		return rc;
	}

	rc = vbdev_split_create(cfg);
	if (rc == -ENODEV) {
		/* It is ok if base bdev does not exist yet. */
		rc = 0;
	}

	return rc;
}

int
spdk_vbdev_split_destruct(const char *base_bdev_name)
{
	struct spdk_vbdev_split_config *cfg = vbdev_split_config_find_by_base_name(base_bdev_name);

	if (!cfg) {
		SPDK_ERRLOG("Split configuration for '%s' not found\n", base_bdev_name);
		return -ENOENT;
	}

	vbdev_split_destruct_config(cfg);
	return 0;
}

struct spdk_bdev_part_base *
spdk_vbdev_split_get_part_base(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_split_config *cfg;

	cfg = vbdev_split_config_find_by_base_name(spdk_bdev_get_name(bdev));

	if (cfg == NULL) {
		return NULL;
	}

	return cfg->split_base;
}

/*
 * During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_split_get_ctx_size(void)
{
	return sizeof(struct vbdev_split_bdev_io);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_split", SPDK_LOG_VBDEV_SPLIT)
