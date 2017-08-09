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
struct vbdev_error_disk {
	struct spdk_bdev		disk;
	struct spdk_bdev		*base_bdev;
	struct spdk_bdev_desc		*base_bdev_desc;
	struct vbdev_error_info		error_vector[SPDK_BDEV_IO_TYPE_RESET];

	TAILQ_ENTRY(vbdev_error_disk)	tailq;
	TAILQ_HEAD(, spdk_bdev_io)	pending_ios;
};

static pthread_mutex_t g_vbdev_error_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, vbdev_error_disk) g_vbdev_error_disks = TAILQ_HEAD_INITIALIZER(
			g_vbdev_error_disks);

int
spdk_vbdev_inject_error(char *name, uint32_t io_type, uint32_t error_type, uint32_t error_num)
{
	struct spdk_bdev *bdev;
	struct vbdev_error_disk *error_disk;
	uint32_t i;

	pthread_mutex_lock(&g_vbdev_error_mutex);
	bdev = spdk_bdev_get_by_name(name);
	if (!bdev) {
		SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", name);
		pthread_mutex_unlock(&g_vbdev_error_mutex);
		return -1;
	}

	TAILQ_FOREACH(error_disk, &g_vbdev_error_disks, tailq) {
		if (bdev == &error_disk->disk) {
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
vbdev_error_reset(struct vbdev_error_disk *error_disk, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *pending_io, *tmp;

	TAILQ_FOREACH_SAFE(pending_io, &error_disk->pending_ios, module_link, tmp) {
		TAILQ_REMOVE(&error_disk->pending_ios, pending_io, module_link);
		spdk_bdev_io_complete(pending_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static uint32_t
vbdev_error_get_error_type(struct vbdev_error_disk *error_disk, uint32_t io_type)
{
	if (error_disk->error_vector[io_type].enabled &&
	    error_disk->error_vector[io_type].error_num) {
		return error_disk->error_vector[io_type].error_type;
	}
	return 0;
}

static void
vbdev_error_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_error_disk *error_disk = bdev_io->bdev->ctxt;
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
		spdk_bdev_io_resubmit(bdev_io, error_disk->base_bdev_desc);
		return;
	} else if (error_type == VBDEV_IO_FAILURE) {
		error_disk->error_vector[bdev_io->type].error_num--;
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else if (error_type == VBDEV_IO_PENDING) {
		TAILQ_INSERT_TAIL(&error_disk->pending_ios, bdev_io, module_link);
		error_disk->error_vector[bdev_io->type].error_num--;
	}
}

static void
vbdev_error_disk_free(struct vbdev_error_disk *disk)
{
	if (!disk) {
		return;
	}

	if (disk->base_bdev) {
		spdk_bdev_module_release_bdev(disk->base_bdev);
	}

	if (disk->base_bdev_desc) {
		spdk_bdev_close(disk->base_bdev_desc);
	}

	free(disk->disk.name);
	free(disk);
}

static void
vbdev_error_free(struct vbdev_error_disk *error_disk)
{
	if (!error_disk) {
		return;
	}

	TAILQ_REMOVE(&g_vbdev_error_disks, error_disk, tailq);

	vbdev_error_disk_free(error_disk);
}

static int
vbdev_error_destruct(void *ctx)
{
	struct vbdev_error_disk *error_disk = ctx;

	vbdev_error_free(error_disk);
	return 0;
}

static bool
vbdev_error_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_error_disk *error_disk = ctx;

	return error_disk->base_bdev->fn_table->io_type_supported(error_disk->base_bdev,
			io_type);
}

static struct spdk_io_channel *
vbdev_error_get_io_channel(void *ctx)
{
	struct vbdev_error_disk *error_disk = ctx;

	return error_disk->base_bdev->fn_table->get_io_channel(error_disk->base_bdev);
}

static int
vbdev_error_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_error_disk *error_disk = ctx;

	spdk_json_write_name(w, "error_disk");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, error_disk->base_bdev->name);

	spdk_json_write_object_end(w);

	return 0;
}

static struct spdk_bdev_fn_table vbdev_error_fn_table = {
	.destruct		= vbdev_error_destruct,
	.io_type_supported	= vbdev_error_io_type_supported,
	.submit_request		= vbdev_error_submit_request,
	.get_io_channel		= vbdev_error_get_io_channel,
	.dump_config_json	= vbdev_error_dump_config_json,
};

int
spdk_vbdev_error_create(struct spdk_bdev *base_bdev)
{
	struct vbdev_error_disk *disk;
	int rc;

	disk = calloc(1, sizeof(*disk));
	if (!disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		rc = -1;
		goto cleanup;
	}

	rc = spdk_bdev_open(base_bdev, false, NULL, NULL, &disk->base_bdev_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(base_bdev));
	}

	rc = spdk_bdev_module_claim_bdev(base_bdev, disk->base_bdev_desc, SPDK_GET_BDEV_MODULE(error));
	if (rc) {
		SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base_bdev));
		goto cleanup;
	}

	disk->base_bdev = base_bdev;
	disk->disk.name = spdk_sprintf_alloc("EE_%s", base_bdev->name);
	if (!disk->disk.name) {
		rc = -ENOMEM;
		goto cleanup;
	}
	disk->disk.blockcnt = base_bdev->blockcnt;
	disk->disk.blocklen = base_bdev->blocklen;
	disk->disk.write_cache = base_bdev->write_cache;
	disk->disk.product_name = "Error Injection Disk";
	disk->disk.ctxt = disk;
	disk->disk.fn_table = &vbdev_error_fn_table;
	disk->disk.module = SPDK_GET_BDEV_MODULE(error);
	spdk_vbdev_register(&disk->disk, &base_bdev, 1);
	TAILQ_INIT(&disk->pending_ios);
	TAILQ_INSERT_TAIL(&g_vbdev_error_disks, disk, tailq);

	rc = 0;
	return rc;
cleanup:
	vbdev_error_disk_free(disk);
	return rc;
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

static void
vbdev_error_fini(void)
{
	struct vbdev_error_disk *error_disk, *tmp;

	TAILQ_FOREACH_SAFE(error_disk, &g_vbdev_error_disks, tailq, tmp) {
		vbdev_error_free(error_disk);
	}
}

SPDK_BDEV_MODULE_REGISTER(error, vbdev_error_init, vbdev_error_fini, NULL, NULL,
			  vbdev_error_examine)
