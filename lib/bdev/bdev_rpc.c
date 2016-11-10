/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2016 Song Jin <song.jin@istuary.com>.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "spdk/log.h"
#include "spdk/scsi.h"

#include "bdev_rpc.h"

struct spdk_scsi_dev *
spdk_bdev_get_scsi_dev(const char *target_name)
{
	struct spdk_scsi_dev *scsi_devs = spdk_scsi_dev_get_list();
	int i = 0;

	if (target_name == NULL) {
		SPDK_ERRLOG("target_name %s is null pointer\n", target_name);
		return NULL;
	}

	for (i = 0; i < SPDK_SCSI_MAX_DEVS; i++) {
		struct spdk_scsi_dev *scsi_dev = &scsi_devs[i];

		if (!scsi_dev->is_allocated) {
			continue;
		}
		
		if (!strcmp(target_name, scsi_dev->name)) {
			return scsi_dev;
		}
	}

	return NULL;
}

int spdk_bdev_rpc_add(struct spdk_bdev *bdev, const char *target_name)
{
	struct spdk_scsi_dev *scsi_dev = NULL;
	struct spdk_scsi_lun *lun = NULL;

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev is null pointer\n");
		return -1;
	}

	if ((scsi_dev = spdk_bdev_get_scsi_dev(target_name)) == NULL) {
		SPDK_ERRLOG("%s iscsi target doesn't exist\n", target_name);
		return -1;
	}

	lun = spdk_scsi_lun_construct(bdev->name, bdev);
	if (lun == NULL) {
		SPDK_ERRLOG("Construct lun %s failed\n", bdev->name);
		return -1;
	}

	spdk_scsi_dev_add_lun(scsi_dev, lun, scsi_dev->maxlun);

	return 0;
}


