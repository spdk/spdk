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

#include "spdk/bdev_module.h"
#include "vbdev_opal.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"

/* OPAL locking range only supports operations on nsid=1 for now */
#define NSID_SUPPORTED		1

struct opal_bdev_ctrlr {
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	struct spdk_opal_dev *opal_dev;
	struct spdk_poller *opal_poller;

	TAILQ_ENTRY(opal_bdev_ctrlr) tailq;
};

static TAILQ_HEAD(, opal_bdev_ctrlr) g_opal_ctrlr = TAILQ_HEAD_INITIALIZER(g_opal_ctrlr);

struct spdk_vbdev_opal_config {
	char *nvme_ctrlr_name;
	uint8_t locking_range_id;
	uint64_t range_start;
	uint64_t range_length;
	SPDK_BDEV_PART_TAILQ *part_tailq;
	struct vbdev_opal_part_base *opal_base;
};

struct opal_vbdev {
	char *name;
	struct opal_bdev_ctrlr *ctrlr;
	struct spdk_bdev_part *bdev_part;
	struct spdk_vbdev_opal_config cfg;

	TAILQ_ENTRY(opal_vbdev) tailq;
};

static TAILQ_HEAD(, opal_vbdev) g_opal_vbdev =
	TAILQ_HEAD_INITIALIZER(g_opal_vbdev);

struct vbdev_opal_bdev_io {
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

struct vbdev_opal_channel {
	struct spdk_bdev_part_channel part_ch;
};

struct vbdev_opal_part_base {
	char *nvme_ctrlr_name;
	struct spdk_bdev_part_base *part_base;
	int num_of_part;
	TAILQ_ENTRY(vbdev_opal_part_base) tailq;
};

static TAILQ_HEAD(, vbdev_opal_part_base) g_opal_base = TAILQ_HEAD_INITIALIZER(g_opal_base);

static void _vbdev_opal_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

static void vbdev_opal_examine(struct spdk_bdev *bdev);

static struct opal_bdev_ctrlr *
opal_ctrlr_get_by_nvme_ctrlr_name(const char *nvme_ctrlr_name)
{
	struct opal_bdev_ctrlr *opal_ctrlr;

	TAILQ_FOREACH(opal_ctrlr, &g_opal_ctrlr, tailq) {
		if (strcmp(opal_ctrlr->nvme_ctrlr->name, nvme_ctrlr_name) == 0) {
			return opal_ctrlr;
		}
	}

	return NULL;
}

static void
vbdev_opal_delete(struct opal_vbdev *opal_bdev)
{
	TAILQ_REMOVE(&g_opal_vbdev, opal_bdev, tailq);
	free(opal_bdev->name);
	free(opal_bdev->cfg.nvme_ctrlr_name);
	free(opal_bdev);
	opal_bdev = NULL;
}

static void
vbdev_opal_clear(void)
{
	struct opal_vbdev *opal_bdev, *tmp;

	TAILQ_FOREACH_SAFE(opal_bdev, &g_opal_vbdev, tailq, tmp) {
		vbdev_opal_delete(opal_bdev);
	}
}

static int
vbdev_opal_init(void)
{
	return 0;
}

static void
vbdev_opal_fini(void)
{
	struct opal_bdev_ctrlr *opal_ctrlr, *tmp;

	vbdev_opal_clear();

	TAILQ_FOREACH_SAFE(opal_ctrlr, &g_opal_ctrlr, tailq, tmp) {
		if (opal_ctrlr->opal_poller != NULL) {   /* opal is busy */
			spdk_poller_unregister(&opal_ctrlr->opal_poller);
			opal_ctrlr->opal_poller = NULL;
		}
		spdk_opal_close(opal_ctrlr->opal_dev);
		free(opal_ctrlr);
	}
}

static int
vbdev_opal_get_ctx_size(void)
{
	return sizeof(struct vbdev_opal_bdev_io);
}

/* delete all the config of the same base bdev */
static void
vbdev_opal_delete_all_base_config(struct vbdev_opal_part_base *base)
{
	char *nvme_ctrlr_name = base->nvme_ctrlr_name;
	struct opal_vbdev *bdev, *tmp_bdev;

	TAILQ_FOREACH_SAFE(bdev, &g_opal_vbdev, tailq, tmp_bdev) {
		if (!strcmp(nvme_ctrlr_name, bdev->ctrlr->nvme_ctrlr->name)) {
			vbdev_opal_delete(bdev);
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
	SPDK_BDEV_PART_TAILQ *part_tailq = spdk_bdev_part_base_get_tailq(base->part_base);

	free(part_tailq);
	free(base->nvme_ctrlr_name);
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
vbdev_opal_io_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
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
		spdk_bdev_io_get_buf(bdev_io, vbdev_opal_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		_vbdev_opal_submit_request(ch, bdev_io);
		break;
	}
}

struct spdk_opal_locking_range_info *
spdk_vbdev_opal_get_info_from_bdev(const char *opal_bdev_name, const char *password)
{
	struct opal_vbdev *vbdev;
	struct opal_bdev_ctrlr *opal_ctrlr;
	int locking_range_id;
	int rc;
	enum spdk_opal_dev_state state;

	TAILQ_FOREACH(vbdev, &g_opal_vbdev, tailq) {
		if (strcmp(vbdev->name, opal_bdev_name) == 0) {
			break;
		}
	}

	if (vbdev == NULL) {
		SPDK_ERRLOG("%s not found\n", opal_bdev_name);
		return NULL;
	}

	opal_ctrlr = vbdev->ctrlr;
	if (opal_ctrlr == NULL) {
		SPDK_ERRLOG("can't find ctrlr of %s\n", vbdev->name);
		return NULL;
	}

	state = spdk_opal_get_dev_state(opal_ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return NULL;
	}

	if (spdk_opal_get_max_locking_ranges(opal_ctrlr->opal_dev) == 0) {
		rc = spdk_opal_cmd_get_max_ranges(opal_ctrlr->opal_dev, password);
		if (rc) {
			SPDK_ERRLOG("Get locking range number failure: %d\n", rc);
			return NULL;
		}
	}

	locking_range_id = vbdev->cfg.locking_range_id;
	rc = spdk_opal_cmd_get_locking_range_info(opal_ctrlr->opal_dev, password,
			OPAL_ADMIN1, locking_range_id);
	if (rc) {
		SPDK_ERRLOG("Get locking range info error: %d\n", rc);
		return NULL;
	}

	return spdk_opal_get_locking_range_info(opal_ctrlr->opal_dev, locking_range_id);
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
vbdev_opal_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;
	struct vbdev_opal_part_base *base = spdk_bdev_part_base_get_ctx(part_base);
	struct opal_bdev_ctrlr *opal_ctrlr;

	opal_ctrlr = opal_ctrlr_get_by_nvme_ctrlr_name(base->nvme_ctrlr_name);
	if (opal_ctrlr->opal_poller != NULL) {   /* opal is busy */
		spdk_poller_unregister(&opal_ctrlr->opal_poller);
		opal_ctrlr->opal_poller = NULL;
		spdk_nvme_ctrlr_reset(opal_ctrlr->nvme_ctrlr->ctrlr);    /* abort command */
	}
	TAILQ_REMOVE(&g_opal_ctrlr, opal_ctrlr, tailq);
	spdk_opal_close(opal_ctrlr->opal_dev);
	free(opal_ctrlr);

	spdk_bdev_part_base_hotremove(part_base, spdk_bdev_part_base_get_tailq(part_base));
	vbdev_opal_delete_all_base_config(base);
}

static bool
vbdev_opal_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(part);

	return spdk_bdev_io_type_supported(base_bdev, io_type);
}

static struct spdk_bdev_fn_table opal_vbdev_fn_table = {
	.destruct           = vbdev_opal_destruct,
	.submit_request     = vbdev_opal_submit_request,
	.io_type_supported  = vbdev_opal_io_type_supported,
	.dump_info_json     = vbdev_opal_dump_info_json,
	.write_config_json  = NULL,
};

static struct spdk_bdev_module opal_if = {
	.name           = "opal",
	.module_init    = vbdev_opal_init,
	.module_fini    = vbdev_opal_fini,
	.get_ctx_size   = vbdev_opal_get_ctx_size,
	.examine_config = vbdev_opal_examine,
	.config_json    = NULL,
};

SPDK_BDEV_MODULE_REGISTER(opal, &opal_if)

int
spdk_vbdev_opal_discovery(const char *nvme_ctrlr_name, enum spdk_opal_dev_state *state)
{
	struct opal_bdev_ctrlr *opal_ctrlr;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	struct spdk_opal_dev *opal_dev;
	int rc;

	opal_ctrlr = opal_ctrlr_get_by_nvme_ctrlr_name(nvme_ctrlr_name);
	if (opal_ctrlr) {
		/* ctrlr is already in the list, update state */
		*state = spdk_opal_get_dev_state(opal_ctrlr->opal_dev);
		return 0;
	}

	/* this is a new controller */
	nvme_ctrlr = nvme_bdev_ctrlr_get_by_name(nvme_ctrlr_name);
	if (!nvme_ctrlr) {
		SPDK_ERRLOG("get nvme ctrlr failed\n");
		return -ENODEV;
	}

	opal_ctrlr = calloc(1, sizeof(struct opal_bdev_ctrlr));
	if (opal_ctrlr == NULL) {
		SPDK_ERRLOG("memory allocation failure\n");
		return -ENOMEM;
	}
	opal_ctrlr->nvme_ctrlr = nvme_ctrlr;
	opal_ctrlr->opal_dev = NULL;

	if (spdk_nvme_ctrlr_get_flags(nvme_ctrlr->ctrlr) &
	    SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		opal_dev = spdk_opal_init_dev(nvme_ctrlr->ctrlr);
		if (opal_dev == NULL) {
			SPDK_ERRLOG("Failed to initialize Opal\n");
			free(opal_ctrlr);
			return -ENOMEM;
		}
		opal_ctrlr->opal_dev = opal_dev;
	}

	if (opal_ctrlr->opal_dev == NULL) { /* security send receive not supported */
		free(opal_ctrlr);
		return -ENODEV;
	} else if (!spdk_opal_supported(opal_ctrlr->opal_dev)) {
		spdk_opal_close(opal_ctrlr->opal_dev);
		free(opal_ctrlr);
		return -ENODEV;
	} else { /* Opal supported */
		rc = spdk_opal_alloc_nvme_ctrlr_name(opal_dev, nvme_ctrlr_name);
		if (rc) {
			spdk_opal_close(opal_ctrlr->opal_dev);
			free(opal_ctrlr);
			return rc;
		}

		TAILQ_INSERT_TAIL(&g_opal_ctrlr, opal_ctrlr, tailq);
		*state = spdk_opal_get_dev_state(opal_dev);
		return 0;
	}
}

int
spdk_vbdev_opal_init(const char *nvme_ctrlr_name, char *password)
{
	struct opal_bdev_ctrlr *opal_ctrlr;
	int rc;
	enum spdk_opal_dev_state state;

	opal_ctrlr = opal_ctrlr_get_by_nvme_ctrlr_name(nvme_ctrlr_name);
	if (opal_ctrlr == NULL) {
		SPDK_ERRLOG("Opal ctrlr not found\n");
		return -ENODEV;
	}

	state = spdk_opal_get_dev_state(opal_ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later\n");
		return -EBUSY;
	}

	if (state == OPAL_DEV_STATE_ENABLED) {
		SPDK_ERRLOG("This drive is already initialized.\n");
		return -EINVAL;
	}

	/* take ownership */
	rc = spdk_opal_cmd_take_ownership(opal_ctrlr->opal_dev, password);
	if (rc) {
		SPDK_ERRLOG("Take ownership failure: %d\n", rc);
		return rc;
	}

	/* activate locking SP */
	rc = spdk_opal_cmd_activate_locking_sp(opal_ctrlr->opal_dev, password);
	if (rc) {
		SPDK_ERRLOG("Activate locking SP failure: %d\n", rc);
		return rc;
	}

	return 0;
}

static void
vbdev_opal_free_bdev(struct opal_vbdev *opal_bdev)
{
	free(opal_bdev->cfg.nvme_ctrlr_name);
	free(opal_bdev);
}

int
spdk_vbdev_opal_create(const char *nvme_ctrlr_name, uint32_t nsid, uint8_t locking_range_id,
		       uint64_t range_start, uint64_t range_length, const char *password)
{
	int rc;
	char *opal_vbdev_name;
	char *base_bdev_name;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	struct opal_bdev_ctrlr *opal_ctrlr;
	struct opal_vbdev *opal_bdev;
	struct vbdev_opal_part_base *opal_part_base;
	struct spdk_bdev_part *part_bdev;
	SPDK_BDEV_PART_TAILQ *part_tailq;
	struct spdk_vbdev_opal_config *cfg;
	struct nvme_bdev *nvme_bdev;
	enum spdk_opal_dev_state state;

	if (nsid != NSID_SUPPORTED) {
		SPDK_ERRLOG("nsid %d not supported", nsid);
		return -EINVAL;
	}

	opal_ctrlr = opal_ctrlr_get_by_nvme_ctrlr_name(nvme_ctrlr_name);
	if (opal_ctrlr == NULL) {
		SPDK_ERRLOG("Can't find Opal ctrlr for %s. Please Init Opal first.\n", nvme_ctrlr_name);
		return -ENODEV;
	}

	if (opal_ctrlr->opal_dev == NULL || !spdk_opal_supported(opal_ctrlr->opal_dev)) {
		SPDK_ERRLOG("Opal not supported for %s", nvme_ctrlr_name);
		return -EINVAL;
	}

	state = spdk_opal_get_dev_state(opal_ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return -EBUSY;
	}

	if (state == OPAL_DEV_STATE_DEFAULT) {
		SPDK_ERRLOG("This drive is not initialized. Can't build opal bdev on it");
		return -EINVAL;
	}

	nvme_ctrlr = opal_ctrlr->nvme_ctrlr;

	opal_bdev = calloc(1, sizeof(struct opal_vbdev));
	if (!opal_bdev) {
		SPDK_ERRLOG("allocation for opal_bdev failed\n");
		return -ENOMEM;
	}

	cfg = &opal_bdev->cfg;
	cfg->nvme_ctrlr_name = strdup(nvme_ctrlr_name);
	if (!cfg->nvme_ctrlr_name) {
		SPDK_ERRLOG("allocation for nvme_ctrlr_name failed\n");
		free(opal_bdev);
		return -ENOMEM;
	}

	cfg->locking_range_id = locking_range_id;
	cfg->range_start = range_start;
	cfg->range_length = range_length;

	opal_bdev->ctrlr = opal_ctrlr;
	/* no need to check opal support here as g_opal_ctrlr only stores opal supported ctrlrs */

	nvme_bdev = TAILQ_FIRST(&nvme_ctrlr->namespaces[nsid - 1]->bdevs);
	assert(nvme_bdev != NULL);
	base_bdev_name = nvme_bdev->disk.name;

	/* traverse base list to see if part_base is already create for this base bdev */
	TAILQ_FOREACH(opal_part_base, &g_opal_base, tailq) {
		if (!strcmp(spdk_bdev_part_base_get_bdev_name(opal_part_base->part_base), base_bdev_name)) {
			cfg->opal_base = opal_part_base;
		}
	}

	/* If there is not a corresponding opal_part_base, a new opal_part_base will be created.
	   For each new part_base, there will be one tailq to store all the parts of this base */
	if (cfg->opal_base == NULL) {
		part_tailq = calloc(1, sizeof(*part_tailq));
		if (part_tailq == NULL) {
			SPDK_ERRLOG("Could not allocate bdev_part_tailq\n");
			vbdev_opal_free_bdev(opal_bdev);
			return -ENOMEM;
		}

		TAILQ_INIT(part_tailq);
		cfg->part_tailq = part_tailq;
		opal_part_base = calloc(1, sizeof(*opal_part_base));
		if (opal_part_base == NULL) {
			SPDK_ERRLOG("Could not allocate opal_part_base\n");
			vbdev_opal_free_bdev(opal_bdev);
			free(part_tailq);
			return -ENOMEM;
		}

		opal_part_base->part_base = spdk_bdev_part_base_construct(spdk_bdev_get_by_name(base_bdev_name),
					    vbdev_opal_base_bdev_hotremove_cb, &opal_if,
					    &opal_vbdev_fn_table, part_tailq, vbdev_opal_base_free, opal_part_base,
					    sizeof(struct vbdev_opal_channel), NULL, NULL);
		if (opal_part_base->part_base == NULL) {
			SPDK_ERRLOG("Could not allocate part_base\n");
			vbdev_opal_free_bdev(opal_bdev);
			free(part_tailq);
			free(opal_part_base);
			return -ENOMEM;
		}
		opal_part_base->num_of_part = 0;
		opal_part_base->nvme_ctrlr_name = strdup(cfg->nvme_ctrlr_name);
		if (opal_part_base->nvme_ctrlr_name == NULL) {
			vbdev_opal_free_bdev(opal_bdev);
			spdk_bdev_part_base_free(opal_part_base->part_base);
			return -ENOMEM;
		}

		cfg->opal_base = opal_part_base;
		TAILQ_INSERT_TAIL(&g_opal_base, opal_part_base, tailq);
	}

	part_bdev = calloc(1, sizeof(struct spdk_bdev_part));
	if (!part_bdev) {
		SPDK_ERRLOG("Could not allocate part_bdev\n");
		vbdev_opal_free_bdev(opal_bdev);
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_opal_vbdev, opal_bdev, tailq);
	opal_vbdev_name = spdk_sprintf_alloc("%sr%" PRIu8, base_bdev_name,
					     cfg->locking_range_id);  /* e.g.: nvme0n1r1 */
	if (opal_vbdev_name == NULL) {
		SPDK_ERRLOG("Could not allocate opal_vbdev_name\n");
		rc = -ENOMEM;
		goto err;
	}

	opal_bdev->name = opal_vbdev_name;
	rc = spdk_opal_cmd_setup_locking_range(opal_bdev->ctrlr->opal_dev, OPAL_ADMIN1,
					       cfg->locking_range_id, cfg->range_start, cfg->range_length, password);
	if (rc) {
		SPDK_ERRLOG("Error construct %s\n", opal_vbdev_name);
		goto err;
	}

	rc = spdk_bdev_part_construct(part_bdev, cfg->opal_base->part_base, opal_vbdev_name,
				      cfg->range_start, cfg->range_length, "Opal locking range");
	if (rc) {
		SPDK_ERRLOG("Could not allocate bdev part\n");
		goto err;
	}

	/* lock this bdev initially */
	rc = spdk_opal_cmd_lock_unlock(opal_bdev->ctrlr->opal_dev, OPAL_ADMIN1, OPAL_RWLOCK,
				       locking_range_id,
				       password);
	if (rc) {
		SPDK_ERRLOG("Error lock %s\n", opal_vbdev_name);
		goto err;
	}

	opal_bdev->bdev_part = part_bdev;
	cfg->opal_base->num_of_part++;
	return 0;

err:
	vbdev_opal_delete(opal_bdev);
	free(part_bdev);
	return rc;
}

static void
vbdev_opal_destruct_bdev(struct opal_vbdev *opal_bdev)
{
	SPDK_BDEV_PART_TAILQ *opal_part_tailq;
	struct spdk_bdev_part *part;
	struct spdk_vbdev_opal_config *cfg = &opal_bdev->cfg;

	if (cfg->opal_base != NULL) {
		part = opal_bdev->bdev_part;
		opal_part_tailq = spdk_bdev_part_base_get_tailq(cfg->opal_base->part_base);
		if (opal_part_tailq == NULL) {
			SPDK_ERRLOG("Can't get tailq for this opal_base\n");
			return;
		}
		if (cfg->range_start == spdk_bdev_part_get_offset_blocks(part)) {
			if (cfg->opal_base->num_of_part <= 1) {
				/* if there is only one part for this base, we can remove the base now */
				spdk_bdev_part_base_hotremove(cfg->opal_base->part_base, opal_part_tailq);

				/* remove from the tailq vbdev_opal_part_base */
				TAILQ_REMOVE(&g_opal_base, cfg->opal_base, tailq);
			} else {
				spdk_bdev_unregister(spdk_bdev_part_get_bdev(part), NULL, NULL);
				cfg->opal_base->num_of_part--;
			}
		}
	}
	vbdev_opal_delete(opal_bdev);
}

int
spdk_vbdev_opal_destruct(const char *bdev_name, const char *password)
{
	struct spdk_vbdev_opal_config *cfg;
	struct opal_bdev_ctrlr *ctrlr;
	int locking_range_id;
	int rc;
	struct opal_vbdev *opal_bdev;
	enum spdk_opal_dev_state state;

	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		rc = -ENODEV;
		goto err;
	}

	cfg = &opal_bdev->cfg;
	locking_range_id = cfg->locking_range_id;

	ctrlr = opal_bdev->ctrlr;
	if (ctrlr == NULL) {
		SPDK_ERRLOG("can't find opal_bdev_ctrlr of %s\n", bdev_name);
		return -ENODEV;
	}

	state = spdk_opal_get_dev_state(ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return -EBUSY;
	}

	/* secure erase locking range */
	rc = spdk_opal_cmd_erase_locking_range(ctrlr->opal_dev, OPAL_ADMIN1, locking_range_id,
					       password);
	if (rc) {
		SPDK_ERRLOG("opal erase locking range failed\n");
		goto err;
	}

	/* reset the locking range to 0 */
	rc = spdk_opal_cmd_setup_locking_range(ctrlr->opal_dev, OPAL_ADMIN1, locking_range_id, 0,
					       0, password);
	if (rc) {
		SPDK_ERRLOG("opal reset locking range failed\n");
		goto err;
	}

	spdk_opal_free_locking_range_info(ctrlr->opal_dev, locking_range_id);
	vbdev_opal_destruct_bdev(opal_bdev);
	return 0;

err:
	return rc;
}

static void
vbdev_opal_examine(struct spdk_bdev *bdev)
{
	/* TODO */
	spdk_bdev_module_examine_done(&opal_if);
}

static int
vbdev_opal_recv_poll(void *arg)
{
	struct opal_bdev_ctrlr *ctrlr = arg;
	int rc;

	if (ctrlr->nvme_ctrlr->destruct) {
		spdk_opal_revert_callback(ctrlr->opal_dev, -ENODEV);
		goto end;
	}

	rc = spdk_opal_revert_poll(ctrlr->opal_dev);
	if (rc == -EAGAIN) {
		return -1;
	}

	/* receive end or nvme ctrlr destruct */
end:
	spdk_poller_unregister(&ctrlr->opal_poller);
	ctrlr->opal_poller = NULL;
	if (ctrlr->nvme_ctrlr->destruct) { /* nvme ctrlr is destructing */
		TAILQ_REMOVE(&g_opal_ctrlr, ctrlr, tailq);
		spdk_opal_close(ctrlr->opal_dev);
		free(ctrlr);
	}
	return 1;
}

int
spdk_vbdev_opal_revert_tper(struct nvme_bdev_ctrlr *nvme_ctrlr, const char *password,
			    spdk_opal_revert_cb cb_fn, void *cb_ctx)
{
	int rc;
	struct opal_bdev_ctrlr *opal_ctrlr;
	enum spdk_opal_dev_state state;

	opal_ctrlr = opal_ctrlr_get_by_nvme_ctrlr_name(nvme_ctrlr->name);
	if (opal_ctrlr == NULL) {
		SPDK_ERRLOG("Can't find Opal ctrlr for %s. Please Init Opal first.\n", nvme_ctrlr->name);
		return -ENODEV;
	}

	state = spdk_opal_get_dev_state(opal_ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return -EBUSY;
	}

	if (state == OPAL_DEV_STATE_DEFAULT) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_OPAL, "The drive is in default state.\n");
		return 0;
	}

	rc = spdk_opal_cmd_revert_tper_async(opal_ctrlr->opal_dev, password, cb_fn, cb_ctx);
	if (rc) {
		SPDK_ERRLOG("%s revert tper failure: %d\n", nvme_ctrlr->name, rc);
		return rc;
	}
	opal_ctrlr->opal_poller = spdk_poller_register(vbdev_opal_recv_poll, opal_ctrlr, 50);
	return 0;
}

int
spdk_vbdev_opal_set_lock_state(const char *bdev_name, uint16_t user_id, const char *password,
			       const char *lock_state)
{
	struct opal_bdev_ctrlr *ctrlr;
	int locking_range_id;
	int rc;
	enum spdk_opal_lock_state state_flag;
	struct opal_vbdev *opal_bdev;
	enum spdk_opal_dev_state state;

	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		return -ENODEV;
	}

	ctrlr = opal_bdev->ctrlr;
	if (ctrlr == NULL) {
		SPDK_ERRLOG("can't find ctrlr of %s\n", opal_bdev->name);
		return -ENODEV;
	}

	state = spdk_opal_get_dev_state(ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return -EBUSY;
	}
	/* since bdev was created, no need to check the state for OPAL_DEV_STATE_DEFAULT */

	if (strcasecmp(lock_state, "READWRITE") == 0) {
		state_flag = OPAL_READWRITE;
	} else if (strcasecmp(lock_state, "READONLY") == 0) {
		state_flag = OPAL_READONLY;
	} else if (strcasecmp(lock_state, "RWLOCK") == 0) {
		state_flag = OPAL_RWLOCK;
	} else {
		SPDK_ERRLOG("Invalid OPAL lock state input\n");
		return -EINVAL;
	}

	locking_range_id = opal_bdev->cfg.locking_range_id;
	rc = spdk_opal_cmd_lock_unlock(ctrlr->opal_dev, user_id, state_flag, locking_range_id,
				       password);
	if (rc) {
		SPDK_ERRLOG("%s lock/unlock failure: %d\n", bdev_name, rc);
	}

	return rc;
}

int
spdk_vbdev_opal_enable_new_user(const char *bdev_name, const char *admin_password, uint16_t user_id,
				const char *user_password)
{
	struct opal_bdev_ctrlr *ctrlr;
	int locking_range_id;
	int rc;
	struct opal_vbdev *opal_bdev;
	enum spdk_opal_dev_state state;

	TAILQ_FOREACH(opal_bdev, &g_opal_vbdev, tailq) {
		if (strcmp(opal_bdev->name, bdev_name) == 0) {
			break;
		}
	}

	if (opal_bdev == NULL) {
		SPDK_ERRLOG("%s not found\n", bdev_name);
		return -ENODEV;
	}

	ctrlr = opal_bdev->ctrlr;
	if (ctrlr == NULL) {
		SPDK_ERRLOG("can't find ctrlr of %s\n", opal_bdev->name);
		return -ENODEV;
	}

	state = spdk_opal_get_dev_state(ctrlr->opal_dev);
	if (state == OPAL_DEV_STATE_BUSY) {
		SPDK_ERRLOG("SP Busy, try again later");
		return -EBUSY;
	}
	/* since bdev was created, no need to check the state for OPAL_DEV_STATE_DEFAULT */

	rc = spdk_opal_cmd_enable_user(ctrlr->opal_dev, user_id, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s enable user error: %d\n", bdev_name, rc);
		return rc;
	}

	rc = spdk_opal_cmd_set_new_passwd(ctrlr->opal_dev, user_id, user_password, admin_password,
					  true);
	if (rc) {
		SPDK_ERRLOG("%s set user password error: %d\n", bdev_name, rc);
		return rc;
	}

	locking_range_id = opal_bdev->cfg.locking_range_id;
	rc = spdk_opal_cmd_add_user_to_locking_range(ctrlr->opal_dev, user_id, locking_range_id,
			OPAL_READONLY, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s add user READONLY priority error: %d\n", bdev_name, rc);
		return rc;
	}

	rc = spdk_opal_cmd_add_user_to_locking_range(ctrlr->opal_dev, user_id, locking_range_id,
			OPAL_READWRITE, admin_password);
	if (rc) {
		SPDK_ERRLOG("%s add user READWRITE priority error: %d\n", bdev_name, rc);
		return rc;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_opal", SPDK_LOG_VBDEV_OPAL)
