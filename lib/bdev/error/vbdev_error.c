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
#include "spdk/endian.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "vbdev_error.h"

/* Context for each error bdev */
struct vbdev_error_disk {
	struct spdk_bdev		disk;
	struct spdk_bdev		*base_bdev;
	TAILQ_ENTRY(vbdev_error_disk)	tailq;
};

static uint32_t g_io_type_mask;
static uint32_t g_error_num;
static pthread_mutex_t g_vbdev_error_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, vbdev_error_disk) g_vbdev_error_disks = TAILQ_HEAD_INITIALIZER(
			g_vbdev_error_disks);

void
spdk_vbdev_inject_error(uint32_t io_type_mask, uint32_t error_num)
{
	pthread_mutex_lock(&g_vbdev_error_mutex);
	g_io_type_mask = io_type_mask;
	g_error_num = error_num;
	pthread_mutex_unlock(&g_vbdev_error_mutex);
}

static void
vbdev_error_reset(struct vbdev_error_disk *error_disk, struct spdk_bdev_io *bdev_io)
{
	/*
	 * pass the I/O through unmodified.
	 *
	 * However, we do need to increment the generation count for the error bdev,
	 * since the spdk_bdev_io_complete() path that normally updates it will not execute
	 * after we resubmit the I/O to the base_bdev.
	 */
	error_disk->disk.gencnt++;
}

static void
vbdev_error_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_error_disk *error_disk = bdev_io->bdev->ctxt;
	uint32_t io_type_mask;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		vbdev_error_reset(error_disk, bdev_io);
		break;
	default:
		SPDK_ERRLOG("Error Injection: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	io_type_mask = 1U << bdev_io->type;

	if (g_error_num == 0 || !(g_io_type_mask & io_type_mask)) {
		spdk_bdev_io_resubmit(bdev_io, error_disk->base_bdev);
		return;
	}

	pthread_mutex_lock(&g_vbdev_error_mutex);
	/* check again to make sure g_error_num has not been decremented since we checked it above */
	if (g_error_num == 0) {
		spdk_bdev_io_resubmit(bdev_io, error_disk->base_bdev);
	} else {
		g_error_num--;
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
	pthread_mutex_unlock(&g_vbdev_error_mutex);
}

static void
vbdev_error_disk_free(struct vbdev_error_disk *disk)
{
	if (!disk) {
		return;
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

	spdk_bdev_unclaim(error_disk->base_bdev);
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

	if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
		SPDK_ERRLOG("Error bdev %s is already claimed\n", base_bdev->name);
		return -1;
	}

	disk = calloc(1, sizeof(*disk));
	if (!disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		rc = -1;
		goto cleanup;
	}

	disk->base_bdev = base_bdev;
	memcpy(&disk->disk, base_bdev, sizeof(*base_bdev));
	disk->disk.name = spdk_sprintf_alloc("EE_%s", base_bdev->name);
	if (!disk->disk.name) {
		rc = -ENOMEM;
		goto cleanup;
	}
	disk->disk.product_name = "Error Injection Disk";
	disk->disk.ctxt = disk;
	disk->disk.fn_table = &vbdev_error_fn_table;

	spdk_bdev_register(&disk->disk);

	TAILQ_INSERT_TAIL(&g_vbdev_error_disks, disk, tailq);

	rc = 0;
	return rc;
cleanup:
	vbdev_error_disk_free(disk);
	return rc;
}

static void
vbdev_error_init(void)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	int i, rc = 0;
	struct spdk_bdev *base_bdev;

	sp = spdk_conf_find_section(NULL, "BdevError");
	if (sp == NULL) {
		goto end;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "BdevError", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "BdevError", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("ErrorInjection configuration missing blockdev name\n");
			rc = -1;
			goto end;
		}

		base_bdev = spdk_bdev_get_by_name(base_bdev_name);
		if (!base_bdev) {
			SPDK_ERRLOG("Could not find ErrorInjection bdev %s\n", base_bdev_name);
			rc = -1;
			goto end;
		}

		if (spdk_vbdev_error_create(base_bdev)) {
			rc = -1;
			goto end;
		}
	}

end:
	spdk_vbdev_module_init_next(rc);
}

static void
vbdev_error_fini(void)
{
	struct vbdev_error_disk *error_disk, *tmp;

	TAILQ_FOREACH_SAFE(error_disk, &g_vbdev_error_disks, tailq, tmp) {
		vbdev_error_free(error_disk);
	}
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_error_init, vbdev_error_fini, NULL, NULL, NULL)
