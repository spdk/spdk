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

#include "spdk/stdinc.h"

#include "vbdev_zone_block.h"

#include "spdk/config.h"
#include "spdk/nvme.h"

#include "spdk_internal/log.h"

int
spdk_vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name, uint64_t zone_size,
			     uint64_t optimal_open_zones)
{
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	bdev = spdk_bdev_get_by_name(bdev_name);

	if (!bdev) {
		SPDK_ERRLOG("Base bdev (%s) doesn't exist\n", bdev_name);
		return -ENODEV;
	}

	if (spdk_bdev_is_zoned(bdev)) {
		SPDK_ERRLOG("Base bdev %s is already a zoned bdev\n", bdev_name);
		return -ENODEV;
	}

	if (zone_size == 0) {
		SPDK_ERRLOG("Zone size can't be 0\n");
		return -EINVAL;
	}


	if (optimal_open_zones == 0) {
		optimal_open_zones = DEFAULT_OPTIMAL_ZONES;
		SPDK_WARNLOG("Optimal open zones can't be 0, changed to default value %lu\n", optimal_open_zones);
	}

	return rc;
}

void
spdk_vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	return;
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_zone_block", SPDK_LOG_VBDEV_ZONE_BLOCK)
