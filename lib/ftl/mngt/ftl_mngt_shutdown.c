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
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"

static const struct ftl_mngt_process_desc desc_shutdown;

static const struct ftl_mngt_process_desc desc_shutdown = {
	.name = "FTL shutdown",
	.error_handler = ftl_mngt_rollback_device,
	.steps = {
		{
			.name = "Stop task core",
			.action = ftl_mngt_stop_task_core
		},
		{
			.name = "Persist L2P",
			.action = ftl_mngt_persist_l2p
		},
		{
			.name = "Persist metadata",
			.action = ftl_mngt_persist_md
		},
		{
			.name = "Set FTL clean state",
			.action = ftl_mngt_set_clean
		},
		{
			.name = "Dump statistics",
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			.action = ftl_mngt_rollback_device
		},
		{}
	}
};

static const struct ftl_mngt_process_desc desc_fast_shutdown = {
	.name = "FTL fast shutdown",
	.steps = {
		{
			.name = "Stop task core",
			.action = ftl_mngt_stop_task_core
		},
		{
			.name = "Fast persist metadata",
			.action = ftl_mngt_fast_persist_md
		},
		{
			.name = "Set FTL SHM clean state",
			.action = ftl_mngt_set_shm_clean
		},
		{
			.name = "Dump statistics",
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			.action = ftl_mngt_rollback_device
		},
		{}
	}
};

int ftl_mngt_shutdown(struct spdk_ftl_dev *dev,
		      ftl_mngt_fn cb, void *cb_cntx)
{
	const struct ftl_mngt_process_desc *pdesc = (dev->conf.fast_shdn) ?
			&desc_fast_shutdown : &desc_shutdown;
	return ftl_mngt_execute(dev, pdesc, cb, cb_cntx);
}
