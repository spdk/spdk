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

#include <rte_config.h>
#include <rte_mempool.h>

#include "spdk_internal/log.h"
#include "spdk_internal/event.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"
#include "task.h"

static struct rte_mempool *g_task_pool;

void
spdk_vhost_task_put(struct spdk_vhost_task *task)
{
	spdk_scsi_task_put(&task->scsi);
}

static void
spdk_vhost_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_task *task = container_of(scsi_task, struct spdk_vhost_task, scsi);

	assert(((struct spdk_vhost_dev *) task->svdev)->task_cnt > 0);
	((struct spdk_vhost_dev *) task->svdev)->task_cnt--;
	rte_mempool_put(g_task_pool, task);
}

void
spdk_vhost_task_get(struct spdk_vhost_scsi_dev *svdev, void **tasks, int count,
		    spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_vhost_task *task;
	int rc, i;

	rc = rte_mempool_get_bulk(g_task_pool, tasks, count);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to get task\n");
		rte_panic("no memory\n");
	}

	assert(((struct spdk_vhost_dev *) svdev)->task_cnt <= INT_MAX - count);
	((struct spdk_vhost_dev *) svdev)->task_cnt += count;

	for (i = 0; i < count; ++i) {
		task = tasks[i];
		memset(task, 0, sizeof(*task));

		task->svdev = svdev;
		spdk_scsi_task_construct(&task->scsi,
					 cpl_fn,
					 spdk_vhost_task_free_cb,
					 NULL);
	}
}

int
spdk_vhost_init(void)
{
	g_task_pool = rte_mempool_create("vhost task pool", 16384, sizeof(struct spdk_vhost_task),
					 128, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
	if (!g_task_pool) {
		SPDK_ERRLOG("create task pool failed\n");
		return -1;
	}

	return 0;
}

int
spdk_vhost_fini(void)
{
	return 0;
}
