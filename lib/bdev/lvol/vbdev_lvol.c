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


#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/io_channel.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/lvol.h"
#include "spdk_internal/log.h"

/* Context for each lvol virtual bdev */
struct lvol_disk {
	struct spdk_bdev	disk;
	struct spdk_bdev	*base_bdev;
	TAILQ_ENTRY(lvol_disk)	tailq;
};

static TAILQ_HEAD(, lvol_disk) g_lvol_disks = TAILQ_HEAD_INITIALIZER(g_lvol_disks);
static int g_lvol_base_num;

static void
vbdev_lvol_free(struct lvol_disk *lvol_disk)
{

}
/* TODO:
static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_destruct,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_config_json	= vbdev_lvol_dump_config_json,
};
*/

static void
vbdev_lvol_init(void)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	int i, rc = 0;
	struct spdk_bdev *base_bdev;

	sp = spdk_conf_find_section(NULL, "Lvol");
	if (sp == NULL) {
		rc = 0;
		goto end;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "Split", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("lvol configuration missing blockdev name\n");
			rc = -1;
			goto end;
		}

		base_bdev = spdk_bdev_get_by_name(base_bdev_name);
		if (!base_bdev) {
			SPDK_ERRLOG("Could not find lvol bdev %s\n", base_bdev_name);
			rc = -1;
			goto end;
		}

		if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
			SPDK_ERRLOG("lvol bdev %s is already claimed\n", spdk_bdev_get_name(base_bdev));
			rc = -1;
			goto end;
		}

		/*	TODO: rc = vbdev_lvol_create(base_bdev); */
		if (rc) {
			rc = -1;
			goto end;
		}
		g_lvol_base_num++;
	}

end:
	/* if no lvol bdev num is counted, just call vbdev_module_init_next */
	if (!g_lvol_base_num) {
		spdk_vbdev_module_init_next(rc);
	}
}

static void
vbdev_lvol_fini(void)
{
	struct lvol_disk *lvol_disk, *tmp;

	TAILQ_FOREACH_SAFE(lvol_disk, &g_lvol_disks, tailq, tmp) {
		vbdev_lvol_free(lvol_disk);
	}
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_lvol_init, vbdev_lvol_fini, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
