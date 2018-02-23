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
#include "spdk/conf.h"
#include "spdk/util.h"
#include "spdk/endian.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "vbdev_error.h"

SPDK_DECLARE_BDEV_MODULE(error);

struct vbdev_error_info {
	bool				enabled;
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

static void
spdk_error_free_base(struct spdk_bdev_part_base *base)
{
	free(base);
}

int
spdk_vbdev_inject_error(char *name, uint32_t io_type, uint32_t error_type, uint32_t error_num)
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
		return -1;
	}

	TAILQ_FOREACH(part, &g_error_disks, tailq) {
		if (bdev == &part->bdev) {
			error_disk = (struct error_disk *)part;
			break;
		}
	}

	if (error_disk == NULL) {
		SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", name);
		pthread_mutex_unlock(&g_vbdev_error_mutex);
		return -1;
	}

	if (0xffffffff == io_type) {
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].enabled = true;
			error_disk->error_vector[i].error_type = error_type;
			error_disk->error_vector[i].error_num = error_num;
		}
	} else if (0 == io_type) {
		for (i = 0; i < SPDK_COUNTOF(error_disk->error_vector); i++) {
			error_disk->error_vector[i].enabled = false;
			error_disk->error_vector[i].error_num = 0;
		}
	} else {
		error_disk->error_vector[io_type].enabled = true;
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
	if (error_disk->error_vector[io_type].enabled &&
	    error_disk->error_vector[io_type].error_num) {
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
		spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
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

	spdk_bdev_part_free(&error_disk->part);
	return 0;
}

static int
vbdev_error_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct error_disk *error_disk = ctx;

	spdk_json_write_name(w, "error_disk");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, error_disk->part.base->bdev->name);

	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_error_dump_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct error_disk *error_disk = bdev->ctxt;

	spdk_json_write_named_string(w, "base_bdev", error_disk->part.base->bdev->name);

	return 0;
}

static struct spdk_bdev_fn_table vbdev_error_fn_table = {
	.destruct		= vbdev_error_destruct,
	.submit_request		= vbdev_error_submit_request,
	.dump_info_json		= vbdev_error_dump_info_json,
	.dump_config_json	= vbdev_error_dump_config_json,
};

static void
spdk_vbdev_error_base_bdev_hotremove_cb(void *_base_bdev)
{
	spdk_bdev_part_base_hotremove(_base_bdev, &g_error_disks);
}

int
spdk_vbdev_error_create(struct spdk_bdev *base_bdev)
{
	struct spdk_bdev_part_base *base = NULL;
	struct error_disk *disk = NULL;
	char *name;
	int rc;

	base = calloc(1, sizeof(*base));
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	rc = spdk_bdev_part_base_construct(base, base_bdev,
					   spdk_vbdev_error_base_bdev_hotremove_cb,
					   SPDK_GET_BDEV_MODULE(error), &vbdev_error_fn_table,
					   &g_error_disks, spdk_error_free_base,
					   sizeof(struct error_channel), NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("could not construct part base for bdev %s\n", spdk_bdev_get_name(base_bdev));
		return -1;
	}

	disk = calloc(1, sizeof(*disk));
	if (!disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		spdk_error_free_base(base);
		return -1;
	}

	name = spdk_sprintf_alloc("EE_%s", spdk_bdev_get_name(base_bdev));
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		spdk_error_free_base(base);
		free(disk);
		return -1;
	}

	rc = spdk_bdev_part_construct(&disk->part, base, name, 0, base_bdev->blockcnt,
				      "Error Injection Disk");
	if (rc) {
		SPDK_ERRLOG("could not construct part for bdev %s\n", spdk_bdev_get_name(base_bdev));
		/* spdk_bdev_part_construct will free name on failure */
		spdk_error_free_base(base);
		free(disk);
		return -1;
	}

	TAILQ_INIT(&disk->pending_ios);

	return 0;
}

static int
vbdev_error_init(void)
{
	return 0;
}

static void
vbdev_error_examine(struct spdk_bdev *bdev)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	int i;

	sp = spdk_conf_find_section(NULL, "BdevError");
	if (sp == NULL) {
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(error));
		return;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "BdevError", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "BdevError", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("ErrorInjection configuration missing bdev name\n");
			break;
		}

		if (strcmp(base_bdev_name, bdev->name) != 0) {
			continue;
		}

		if (spdk_vbdev_error_create(bdev)) {
			SPDK_ERRLOG("could not create error vbdev for bdev %s\n", bdev->name);
			break;
		}
	}

	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(error));
}

SPDK_BDEV_MODULE_REGISTER(error, vbdev_error_init, NULL, NULL, NULL,
			  vbdev_error_examine)
