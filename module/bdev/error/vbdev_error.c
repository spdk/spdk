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
 * This is a module for test purpose which will simulate error cases for bdev.
 */

#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/endian.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "vbdev_error.h"

struct spdk_vbdev_error_config {
	char *base_bdev;
	TAILQ_ENTRY(spdk_vbdev_error_config) tailq;
};

static TAILQ_HEAD(, spdk_vbdev_error_config) g_error_config
	= TAILQ_HEAD_INITIALIZER(g_error_config);

struct vbdev_error_info {
	uint32_t			error_type;
	uint32_t			error_num;
};

/* Context for each error bdev */
struct error_disk {
	struct spdk_bdev_part		part;
	struct vbdev_error_info		error_vector[SPDK_BDEV_IO_TYPE_RESET];
	TAILQ_HEAD(, spdk_bdev_io)	pending_ios;
};

struct error_channel {
	struct spdk_bdev_part_channel	part_ch;
};

static pthread_mutex_t g_vbdev_error_mutex = PTHREAD_MUTEX_INITIALIZER;
static SPDK_BDEV_PART_TAILQ g_error_disks = TAILQ_HEAD_INITIALIZER(g_error_disks);

static int vbdev_error_init(void);
static void vbdev_error_fini(void);

static void vbdev_error_examine(struct spdk_bdev *bdev);
static int vbdev_error_config_json(struct spdk_json_write_ctx *w);

static int vbdev_error_config_add(const char *base_bdev_name);
static int vbdev_error_config_remove(const char *base_bdev_name);

static struct spdk_bdev_module error_if = {
	.name = "error",
	.module_init = vbdev_error_init,
	.module_fini = vbdev_error_fini,
	.examine_config = vbdev_error_examine,
	.config_json = vbdev_error_config_json,

};

SPDK_BDEV_MODULE_REGISTER(error, &error_if)

int
vbdev_error_inject_error(char *name, uint32_t io_type, uint32_t error_type, uint32_t error_num)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_part *part;
	struct error_disk *error_disk = NULL;
	uint32_t i;

	pthread_mutex_lock(&g_vbdev_error_mutex);
	bdev = spdk_bdev_get_by_name(name);
	if (!bdev) {
		SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", name);
		pthread_mutex_unlock(&g_vbdev_error_mutex);
		return -ENODEV;
	}

	TAILQ_FOREACH(part, &g_error_disks, tailq) {
		if (bdev == spdk_bdev_part_get_bdev(part)) {
			error_disk = (struct error_disk *)part;
			break;
		}
	}

	if (error_disk == NULL) {
		SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", name);
		pthread_mutex_unlock(&g_vbdev_error_mutex);
		return -ENODEV;
	}

	if (0xffffffff == io_type) {
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].error_type = error_type;
			error_disk->error_vector[i].error_num = error_num;
		}
	} else if (0 == io_type) {
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].error_num = 0;
		}
	} else {
		error_disk->error_vector[io_type].error_type = error_type;
		error_disk->error_vector[io_type].error_num = error_num;
	}
	pthread_mutex_unlock(&g_vbdev_error_mutex);
	return 0;
}

static void
vbdev_error_reset(struct error_disk *error_disk, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *pending_io, *tmp;

	TAILQ_FOREACH_SAFE(pending_io, &error_disk->pending_ios, module_link, tmp) {
		TAILQ_REMOVE(&error_disk->pending_ios, pending_io, module_link);
		spdk_bdev_io_complete(pending_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static uint32_t
vbdev_error_get_error_type(struct error_disk *error_disk, uint32_t io_type)
{
	if (error_disk->error_vector[io_type].error_num) {
		return error_disk->error_vector[io_type].error_type;
	}
	return 0;
}

static void
vbdev_error_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct error_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct error_disk *error_disk = bdev_io->bdev->ctxt;
	uint32_t error_type;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		vbdev_error_reset(error_disk, bdev_io);
		return;
	default:
		SPDK_ERRLOG("Error Injection: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	error_type = vbdev_error_get_error_type(error_disk, bdev_io->type);
	if (error_type == 0) {
		int rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);

		if (rc) {
			SPDK_ERRLOG("bdev_error: submit request failed, rc=%d\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		return;
	} else if (error_type == VBDEV_IO_FAILURE) {
		error_disk->error_vector[bdev_io->type].error_num--;
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else if (error_type == VBDEV_IO_PENDING) {
		TAILQ_INSERT_TAIL(&error_disk->pending_ios, bdev_io, module_link);
		error_disk->error_vector[bdev_io->type].error_num--;
	}
}

static int
vbdev_error_destruct(void *ctx)
{
	struct error_disk *error_disk = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(&error_disk->part);
	int rc;

	rc = vbdev_error_config_remove(base_bdev->name);
	if (rc != 0) {
		SPDK_ERRLOG("vbdev_error_config_remove() failed\n");
	}

	return spdk_bdev_part_free(&error_disk->part);
}

static int
vbdev_error_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct error_disk *error_disk = ctx;
	struct spdk_bdev *base_bdev = spdk_bdev_part_get_base_bdev(&error_disk->part);

	spdk_json_write_named_object_begin(w, "error_disk");

	spdk_json_write_named_string(w, "base_bdev", base_bdev->name);

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_error_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev. */
}


static struct spdk_bdev_fn_table vbdev_error_fn_table = {
	.destruct		= vbdev_error_destruct,
	.submit_request		= vbdev_error_submit_request,
	.dump_info_json		= vbdev_error_dump_info_json,
	.write_config_json	= vbdev_error_write_config_json
};

static void
vbdev_error_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;

	spdk_bdev_part_base_hotremove(part_base, &g_error_disks);
}

static int
_vbdev_error_create(const char *base_bdev_name)
{
	struct spdk_bdev_part_base *base = NULL;
	struct error_disk *disk = NULL;
	struct spdk_bdev *base_bdev;
	char *name;
	int rc;

	rc = spdk_bdev_part_base_construct_ext(base_bdev_name,
					       vbdev_error_base_bdev_hotremove_cb,
					       &error_if, &vbdev_error_fn_table, &g_error_disks,
					       NULL, NULL, sizeof(struct error_channel),
					       NULL, NULL, &base);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("could not construct part base for bdev %s\n", base_bdev_name);
		}
		return rc;
	}

	base_bdev = spdk_bdev_part_base_get_bdev(base);

	disk = calloc(1, sizeof(*disk));
	if (!disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		spdk_bdev_part_base_free(base);
		return -ENOMEM;
	}

	name = spdk_sprintf_alloc("EE_%s", base_bdev_name);
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		spdk_bdev_part_base_free(base);
		free(disk);
		return -ENOMEM;
	}

	rc = spdk_bdev_part_construct(&disk->part, base, name, 0, base_bdev->blockcnt,
				      "Error Injection Disk");
	free(name);
	if (rc) {
		SPDK_ERRLOG("could not construct part for bdev %s\n", base_bdev_name);
		/* spdk_bdev_part_construct will free name on failure */
		spdk_bdev_part_base_free(base);
		free(disk);
		return rc;
	}

	TAILQ_INIT(&disk->pending_ios);

	return 0;
}

int
vbdev_error_create(const char *base_bdev_name)
{
	int rc;

	rc = vbdev_error_config_add(base_bdev_name);
	if (rc != 0) {
		SPDK_ERRLOG("Adding config for ErrorInjection bdev %s failed (rc=%d)\n",
			    base_bdev_name, rc);
		return rc;
	}

	rc = _vbdev_error_create(base_bdev_name);
	if (rc == -ENODEV) {
		rc = 0;
	} else if (rc != 0) {
		vbdev_error_config_remove(base_bdev_name);
		SPDK_ERRLOG("Could not create ErrorInjection bdev %s (rc=%d)\n",
			    base_bdev_name, rc);
	}

	return rc;
}

void
vbdev_error_delete(struct spdk_bdev *vbdev, spdk_delete_error_complete cb_fn, void *cb_arg)
{
	if (!vbdev || vbdev->module != &error_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(vbdev, cb_fn, cb_arg);
}

static void
vbdev_error_clear_config(void)
{
	struct spdk_vbdev_error_config *cfg;

	while ((cfg = TAILQ_FIRST(&g_error_config))) {
		TAILQ_REMOVE(&g_error_config, cfg, tailq);
		free(cfg->base_bdev);
		free(cfg);
	}
}

static struct spdk_vbdev_error_config *
vbdev_error_config_find_by_base_name(const char *base_bdev_name)
{
	struct spdk_vbdev_error_config *cfg;

	TAILQ_FOREACH(cfg, &g_error_config, tailq) {
		if (strcmp(cfg->base_bdev, base_bdev_name) == 0) {
			return cfg;
		}
	}

	return NULL;
}

static int
vbdev_error_config_add(const char *base_bdev_name)
{
	struct spdk_vbdev_error_config *cfg;

	cfg = vbdev_error_config_find_by_base_name(base_bdev_name);
	if (cfg) {
		SPDK_ERRLOG("vbdev_error_config for bdev %s already exists\n",
			    base_bdev_name);
		return -EEXIST;
	}

	cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		SPDK_ERRLOG("calloc() failed for vbdev_error_config\n");
		return -ENOMEM;
	}

	cfg->base_bdev = strdup(base_bdev_name);
	if (!cfg->base_bdev) {
		free(cfg);
		SPDK_ERRLOG("strdup() failed for base_bdev_name\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_error_config, cfg, tailq);

	return 0;
}

static int
vbdev_error_config_remove(const char *base_bdev_name)
{
	struct spdk_vbdev_error_config *cfg;

	cfg = vbdev_error_config_find_by_base_name(base_bdev_name);
	if (!cfg) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&g_error_config, cfg, tailq);
	free(cfg->base_bdev);
	free(cfg);
	return 0;
}

static int
vbdev_error_init(void)
{
	return 0;
}

static void
vbdev_error_fini(void)
{
	vbdev_error_clear_config();
}

static void
vbdev_error_examine(struct spdk_bdev *bdev)
{
	struct spdk_vbdev_error_config *cfg;
	int rc;

	cfg = vbdev_error_config_find_by_base_name(bdev->name);
	if (cfg != NULL) {
		rc = _vbdev_error_create(bdev->name);
		if (rc != 0) {
			SPDK_ERRLOG("could not create error vbdev for bdev %s at examine\n",
				    bdev->name);
		}
	}

	spdk_bdev_module_examine_done(&error_if);
}

static int
vbdev_error_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vbdev_error_config *cfg;

	TAILQ_FOREACH(cfg, &g_error_config, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_error_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_name", cfg->base_bdev);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	return 0;
}
