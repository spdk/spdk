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

#include "spdk/opal.h"
#include "spdk/bdev_module.h"
#include "vbdev_opal.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"

#define NVME_LENGTH     4
#define MIN_LENGTH      8

static TAILQ_HEAD(, spdk_vbdev_opal_config) g_opal_config =
	TAILQ_HEAD_INITIALIZER(g_opal_config);

struct opal_vbdev {
	struct spdk_bdev *base_bdev;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	struct spdk_opal_dev *opal_dev;
};

struct vbdev_opal_bdev_io {
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

struct vbdev_opal_channel {
	struct spdk_bdev_part_channel part_ch;
};

struct vbdev_opal_part_base {
	struct spdk_bdev_part_base *part_base;
	int num_of_part;
	TAILQ_ENTRY(vbdev_opal_part_base) tailq;
};

static TAILQ_HEAD(, vbdev_opal_part_base) g_opal_base = TAILQ_HEAD_INITIALIZER(g_opal_base);

static void
vbdev_opal_clear_config(void);

static int
vbdev_opal_get_ctx_size(void);

static void
vbdev_opal_examine(struct spdk_bdev *bdev);

static void
_vbdev_opal_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

static void
vbdev_opal_destruct_config(struct spdk_vbdev_opal_config *cfg);

static int
vbdev_opal_init(void)
{
	/* Do nothing */
	return 0;
}

static void
vbdev_opal_fini(void)
{
	vbdev_opal_clear_config();
}

static void
vbdev_opal_delete_config(struct spdk_vbdev_opal_config *cfg)
{
	TAILQ_REMOVE(&g_opal_config, cfg, tailq);
	free(cfg->base_bdev_name);
	free(cfg->password);
	free(cfg->opal_bdev);
	free(cfg);
}

/* delete all the config of the same base bdev */
static void
vbdev_opal_delete_all_base_config(struct spdk_vbdev_opal_config *cfg)
{
	struct spdk_vbdev_opal_config *tmp, *tmp_cfg;

	if (cfg->base_bdev_name != NULL) {
		TAILQ_FOREACH_SAFE(tmp_cfg, &g_opal_config, tailq, tmp) {
			if (!strcmp(cfg->base_bdev_name, tmp_cfg->base_bdev_name)) {
				vbdev_opal_delete_config(tmp_cfg);
			}
		}
	}
}

static int
vbdev_opal_destruct(void *ctx)
{
	struct spdk_bdev_part *part = ctx;

	return spdk_bdev_part_free(part);
}

static void
vbdev_opal_base_free(void *ctx)
{
	struct vbdev_opal_part_base *base = ctx;
	struct bdev_part_tailq *part_tailq = spdk_bdev_part_base_get_tailq(base->part_base);

	free(part_tailq);
	free(base);
}

static void
vbdev_opal_resubmit_io(void *arg)
{
	struct vbdev_opal_bdev_io *io_ctx = (struct vbdev_opal_bdev_io *)arg;

	_vbdev_opal_submit_request(io_ctx->ch, io_ctx->bdev_io);
}

static void
vbdev_opal_queue_io(struct vbdev_opal_bdev_io *io_ctx)
{
	int rc;

	io_ctx->bdev_io_wait.bdev = io_ctx->bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_opal_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = io_ctx;

	rc = spdk_bdev_queue_io_wait(io_ctx->bdev_io->bdev, io_ctx->ch, &io_ctx->bdev_io_wait);

	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_opal_queue_io: %d\n", rc);
		spdk_bdev_io_complete(io_ctx->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
_vbdev_opal_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_opal_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct vbdev_opal_bdev_io *io_ctx = (struct vbdev_opal_bdev_io *)bdev_io->driver_ctx;
	int rc;

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_OPAL, "opal: no memory, queue io.\n");
			io_ctx->ch = _ch;
			io_ctx->bdev_io = bdev_io;
			vbdev_opal_queue_io(io_ctx);
		} else {
			SPDK_ERRLOG("opal: error on io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
vbdev_opal_submit_request_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_vbdev_opal_submit_request(ch, bdev_io);
}

static void
vbdev_opal_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, vbdev_opal_submit_request_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		_vbdev_opal_submit_request(ch, bdev_io);
		break;
	}
}

static int
vbdev_opal_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(part);
	uint64_t offset = spdk_bdev_part_get_offset_blocks(part);

	spdk_json_write_named_object_begin(w, "opal");

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(base_bdev));
	spdk_json_write_named_uint64(w, "offset_blocks", offset);

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_opal_base_bdev_hotremove_cb(void *ctx)
{
	struct spdk_bdev_part_base *part_base = ctx;
	struct spdk_vbdev_opal_config *cfg = spdk_bdev_part_base_get_ctx(part_base);

	spdk_bdev_part_base_hotremove(part_base, cfg->part_tailq);
	vbdev_opal_delete_all_base_config(cfg);
}

static bool
vbdev_opal_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(part);

	return spdk_bdev_io_type_supported(base_bdev, io_type);
}

static struct spdk_bdev_fn_table opal_vbdev_fn_table = {
	.destruct   = vbdev_opal_destruct,
	.submit_request = vbdev_opal_submit_request,
	.io_type_supported = vbdev_opal_io_type_supported,
	.dump_info_json = vbdev_opal_dump_info_json,
	.write_config_json = NULL
};

static struct spdk_bdev_module opal_if = {
	.name = "opal",
	.module_init = vbdev_opal_init,
	.module_fini = vbdev_opal_fini,
	.get_ctx_size = vbdev_opal_get_ctx_size,
	.examine_config = vbdev_opal_examine,
	.config_json = NULL,
};

SPDK_BDEV_MODULE_REGISTER(opal, &opal_if)

int
spdk_vbdev_opal_config_init(const char *base_bdev_name, uint8_t locking_range_id,
			    uint64_t range_start,
			    uint64_t range_length, const char *password, struct spdk_vbdev_opal_config **config)
{
	struct spdk_vbdev_opal_config *cfg;

	cfg = calloc(1, sizeof(struct spdk_vbdev_opal_config));
	if (!cfg) {
		SPDK_ERRLOG("allocation for cfg failed\n");
		return -ENOMEM;
	}

	cfg->base_bdev_name = strdup(base_bdev_name);
	if (!cfg->base_bdev_name) {
		SPDK_ERRLOG("allocation for base_bdev_name failed\n");
		free(cfg);
		return -ENOMEM;
	}

	cfg->locking_range_id = locking_range_id;
	cfg->range_start = range_start;
	cfg->range_length = range_length;
	cfg->password = strdup(password);
	if (!cfg->password) {
		SPDK_ERRLOG("allocation for password failed\n");
		free(cfg->base_bdev_name);
		free(cfg);
		return -ENOMEM;
	}

	if (config) {
		*config = cfg;
	}
	return 0;
}

static struct nvme_bdev_ctrlr *
vbdev_opal_get_nvme_ctrlr_by_bdev_name(const char *bdev_name)
{
	int count = NVME_LENGTH;
	char *ctrlr_name;
	struct nvme_bdev_ctrlr *ctrlr;

	while (bdev_name[count] != 'n') {
		count++;
	}

	ctrlr_name = calloc(1, MIN_LENGTH);
	if (!ctrlr_name) {
		SPDK_ERRLOG("allocation for ctrlr_name failed\n");
		return NULL;
	}

	spdk_strcpy_pad(ctrlr_name, bdev_name, count, ' ');
	ctrlr = nvme_bdev_ctrlr_get_by_name(ctrlr_name);
	if (ctrlr == NULL) {
		SPDK_ERRLOG("nvme bdev ctrlr %s not found\n", ctrlr_name);
		free(ctrlr_name);
		return NULL;
	}

	free(ctrlr_name);
	return ctrlr;
}

int
spdk_vbdev_opal_create(struct spdk_vbdev_opal_config *cfg)
{
	int rc;
	char *opal_vbdev_name;
	struct spdk_bdev *base_bdev;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	struct opal_vbdev *opal_bdev;
	struct vbdev_opal_part_base *opal_part_base;
	struct spdk_bdev_part *part_bdev;
	struct bdev_part_tailq *part_tailq;

	if (cfg->base_bdev_name == NULL) {
		SPDK_ERRLOG("opal bdev config: no base bdev provided.");
		return -EINVAL;
	}

	base_bdev = spdk_bdev_get_by_name(cfg->base_bdev_name);
	if (!base_bdev) {
		SPDK_ERRLOG("No bdev name %s found\n", cfg->base_bdev_name);
		return -ENODEV;
	}

	nvme_ctrlr = vbdev_opal_get_nvme_ctrlr_by_bdev_name(cfg->base_bdev_name);
	if (!nvme_ctrlr) {
		SPDK_ERRLOG("get nvme ctrlr failed\n");
		return -ENODEV;
	}

	opal_bdev = calloc(1, sizeof(struct opal_vbdev));
	if (!opal_bdev) {
		SPDK_ERRLOG("allocation for opal_bdev failed\n");
		return -ENOMEM;
	}

	opal_bdev->nvme_ctrlr = nvme_ctrlr;
	opal_bdev->opal_dev = nvme_ctrlr->opal_dev;
	opal_bdev->base_bdev = base_bdev;
	if (!spdk_opal_supported(opal_bdev->opal_dev)) {
		SPDK_ERRLOG("Opal not supported\n");
		free(opal_bdev);
		return -EINVAL;
	}
	cfg->opal_bdev = opal_bdev;

	/* traverse base list to see if part_base is already create for this base bdev */
	TAILQ_FOREACH(opal_part_base, &g_opal_base, tailq) {
		if (!strcmp(spdk_bdev_part_base_get_bdev_name(opal_part_base->part_base), cfg->base_bdev_name)) {
			cfg->opal_base = opal_part_base;
			goto next;
		}
	}

	/* If there is not corresponding opal_part_base, new opal_part_base will be created
	   For each new part_base, there will be one tailq to store all the part of this base */
	part_tailq = calloc(1, sizeof(*part_tailq));
	if (part_tailq == NULL) {
		SPDK_ERRLOG("Could not allocate bdev_part_tailq\n");
		return -ENOMEM;
	}

	TAILQ_INIT(part_tailq);
	cfg->part_tailq = part_tailq;
	opal_part_base = calloc(1, sizeof(*opal_part_base));
	if (opal_part_base == NULL) {
		SPDK_ERRLOG("Could not allocate opal_part_base\n");
		free(part_tailq);
		return -ENOMEM;
	}

	opal_part_base->part_base = spdk_bdev_part_base_construct(base_bdev,
				    vbdev_opal_base_bdev_hotremove_cb, &opal_if,
				    &opal_vbdev_fn_table, part_tailq, vbdev_opal_base_free, opal_part_base,
				    sizeof(struct vbdev_opal_channel), NULL, NULL);
	if (opal_part_base->part_base == NULL) {
		SPDK_ERRLOG("Could not allocate part_base\n");
		free(part_tailq);
		free(opal_part_base);
		return -ENOMEM;
	}
	opal_part_base->num_of_part = 0;
	cfg->opal_base = opal_part_base;
	TAILQ_INSERT_TAIL(&g_opal_base, opal_part_base, tailq);

next:
	part_bdev = calloc(1, sizeof(struct spdk_bdev_part));
	if (!part_bdev) {
		SPDK_ERRLOG("Could not allocate part_bdev\n");
		vbdev_opal_delete_config(cfg);
		return -ENOMEM;
	}

	opal_vbdev_name = spdk_sprintf_alloc("%sl%" PRIu8, cfg->base_bdev_name, cfg->locking_range_id);
	if (opal_vbdev_name == NULL) {
		SPDK_ERRLOG("Could not allocate opal_vbdev_name\n");
		rc = -ENOMEM;
		goto err;
	}

	rc = spdk_opal_cmd_setup_locking_range(opal_bdev->opal_dev, OPAL_ADMIN1,
					       cfg->locking_range_id, cfg->range_start, cfg->range_length, cfg->password);
	if (rc) {
		SPDK_ERRLOG("Error construct %s\n", opal_vbdev_name);
		goto err;
	}

	rc = spdk_bdev_part_construct(part_bdev, cfg->opal_base->part_base, opal_vbdev_name,
				      cfg->range_start, cfg->range_length, "Opal locking range");
	if (rc) {
		SPDK_ERRLOG("Could not allocate bdev part\n");
		rc = -ENOMEM;
		goto err;
	}

	opal_part_base->num_of_part ++ ;
	TAILQ_INSERT_TAIL(&g_opal_config, cfg, tailq);

	free(opal_vbdev_name);
	return 0;
err:
	vbdev_opal_delete_config(cfg);
	free(part_bdev);
	free(opal_vbdev_name);
	return rc;
}

static void
vbdev_opal_examine(struct spdk_bdev *bdev)
{
	/* not needed for now */
	spdk_bdev_module_examine_done(&opal_if);
}

static int
vbdev_opal_get_ctx_size(void)
{
	return sizeof(struct vbdev_opal_bdev_io);
}

static struct spdk_vbdev_opal_config *
vbdev_opal_config_find_by_base_name(const char *base_bdev_name)
{
	struct spdk_vbdev_opal_config *cfg;

	TAILQ_FOREACH(cfg, &g_opal_config, tailq) {
		if (strcmp(cfg->base_bdev_name, base_bdev_name) == 0) {
			return cfg;
		}
	}

	return NULL;
}

struct spdk_bdev_part_base *
spdk_vbdev_opal_get_part_base(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_opal_config *cfg;

	cfg = vbdev_opal_config_find_by_base_name(spdk_bdev_get_name(bdev));

	if (cfg == NULL) {
		return NULL;
	}

	return cfg->opal_base->part_base;
}

static struct spdk_vbdev_opal_config *
vbdev_opal_find_config(const char *base_bdev_name, uint8_t locking_range_id)
{
	struct spdk_vbdev_opal_config *cfg;

	TAILQ_FOREACH(cfg, &g_opal_config, tailq) {
		if (strcmp(cfg->base_bdev_name, base_bdev_name) == 0 && cfg->locking_range_id == locking_range_id) {
			return cfg;
		}
	}
	return NULL;
}

static void
vbdev_opal_destruct_config(struct spdk_vbdev_opal_config *cfg)
{
	struct bdev_part_tailq *opal_part_tailq;
	struct spdk_bdev_part *part, *tmp;

	if (cfg->opal_base != NULL) {
		opal_part_tailq = spdk_bdev_part_base_get_tailq(cfg->opal_base->part_base);
		if (opal_part_tailq == NULL) {
			SPDK_ERRLOG("Can't get tailq for each part of this opal_base\n");
			return;
		}
		TAILQ_FOREACH_SAFE(part, opal_part_tailq, tailq, tmp) {
			if (cfg->range_start == spdk_bdev_part_get_offset_blocks(part)) {
				if (cfg->opal_base->num_of_part <= 1) {
					/* if there is only one part for this base, we can remove the base now */
					spdk_bdev_part_base_hotremove(cfg->opal_base->part_base, opal_part_tailq);

					/* remove from the tailq vbdev_opal_part_base */
					TAILQ_REMOVE(&g_opal_base, cfg->opal_base, tailq);
					break;
				} else {
					spdk_bdev_unregister(spdk_bdev_part_get_bdev(part), NULL, NULL);
					cfg->opal_base->num_of_part --;
					break;
				}
			}
		}
	}
	vbdev_opal_delete_config(cfg);
}

static void
vbdev_opal_clear_config(void)
{
	struct spdk_vbdev_opal_config *cfg, *tmp;

	TAILQ_FOREACH_SAFE(cfg, &g_opal_config, tailq, tmp) {
		vbdev_opal_delete_config(cfg);
	}
}

static int
vbdev_opal_get_base_bdev_name(const char *vbdev_name, char **base_bdev_name)
{
	int count = 0;
	char *base_name;

	while (vbdev_name[count] != 'l') {
		count++;
	}

	base_name = calloc(1, MIN_LENGTH);
	if (base_name == NULL) {
		SPDK_ERRLOG("Could not allocate base_bdev_name\n");
		return -ENOMEM;
	}

	spdk_strcpy_pad(base_name, vbdev_name, count, 0);
	*base_bdev_name = base_name;

	return count;
}

int
spdk_vbdev_opal_destruct(const char *bdev_name, const char *password)
{
	struct spdk_vbdev_opal_config *cfg;
	char *base_bdev_name;
	int locking_range_id;
	int rc;
	int index;

	index = vbdev_opal_get_base_bdev_name(bdev_name, &base_bdev_name);
	if (index < 0) {
		SPDK_ERRLOG("Get base bdev name failed\n");
		return index;
	}
	locking_range_id = spdk_strtol(bdev_name + index + 1, 10);
	cfg = vbdev_opal_find_config(base_bdev_name, locking_range_id);
	if (!cfg) {
		SPDK_ERRLOG("Configuration for this virtual bdev is not found\n");
		rc = -ENOENT;
		goto err;
	}

	/* secure erase locking range */
	rc = spdk_opal_cmd_erase_locking_range(cfg->opal_bdev->opal_dev, OPAL_ADMIN1, locking_range_id,
					       password);
	if (rc) {
		SPDK_ERRLOG("opal erase locking range failed\n");
		goto err;
	}

	/* reset the locking range to 0 */
	rc = spdk_opal_cmd_setup_locking_range(cfg->opal_bdev->opal_dev, OPAL_ADMIN1, locking_range_id, 0,
					       0, password);
	if (rc) {
		SPDK_ERRLOG("opal reset locking range failed\n");
		goto err;
	}

	free(base_bdev_name);
	vbdev_opal_destruct_config(cfg);
	return 0;

err:
	free(base_bdev_name);
	return rc;
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_opal", SPDK_LOG_VBDEV_OPAL)
