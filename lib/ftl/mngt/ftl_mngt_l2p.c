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

#include "spdk/thread.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_l2p.h"

static void l2p_cb(struct spdk_ftl_dev *dev, int status, void *ctx)
{
	struct ftl_mngt *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_l2p_init(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_l2p_deinit(dev);
	ftl_mngt_next_step(mngt);
}

void ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_l2p_clear(dev, l2p_cb, mngt);
}

void ftl_mngt_persist_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_l2p_persist(dev, l2p_cb, mngt);
}

void ftl_mngt_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_l2p_restore(dev, l2p_cb, mngt);
}
