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

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_conf_is_valid(&dev->conf)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void user_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;

	if (status) {
		FTL_ERRLOG(ftl_mngt_get_dev(mngt), "FTL NV Cache: ERROR of clearing user cache data\n");
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_data_nvc];
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_data_nvc];

	FTL_NOTICELOG(dev, "First startup needs to scrub nv cache data region, this may take some time.\n");
	FTL_NOTICELOG(dev, "Scrubbing %lluGiB\n", region->current.blocks * FTL_BLOCK_SIZE / GiB);

	/* Need to scrub user data, so in case of dirty shutdown the recovery won't
	 * pull in data during open chunks recovery from any previous instance
	 */
	md->cb = user_clear_cb;
	md->owner.cb_ctx = mngt;

	union ftl_md_vss vss;
	vss.version.md_version = region->current.version;
	vss.nv_cache.lba = FTL_ADDR_INVALID;
	uint64_t block_data = 0;
	ftl_md_clear(md, &block_data, sizeof(block_data), &vss);
}

void ftl_mngt_finalize_init(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->initialized = 1;

	ftl_mngt_next_step(mngt);
}
