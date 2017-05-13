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
#include "task.h"

#undef container_of
#define container_of(ptr, type, member) ({ \
		typeof(((type *)0)->member) *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })

typedef TAILQ_HEAD(, spdk_vhost_task) need_iovecs_tailq_t;

static struct rte_mempool *g_task_pool;
static struct rte_mempool *g_iov_buffer_pool;

static need_iovecs_tailq_t g_need_iovecs[RTE_MAX_LCORE];

void
spdk_vhost_task_put(struct spdk_vhost_task *task)
{
	assert(&task->scsi.iov == task->scsi.iovs);
	assert(task->scsi.iovcnt == 1);
	spdk_scsi_task_put(&task->scsi);
}

static void
spdk_vhost_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_task *task = container_of(scsi_task, struct spdk_vhost_task, scsi);

	rte_mempool_put(g_task_pool, task);
	spdk_vhost_scsi_ctrlr_task_unref(task->vdev);
}

struct spdk_vhost_task *
spdk_vhost_task_get(struct spdk_vhost_scsi_ctrlr *vdev)
{
	struct spdk_vhost_task *task;
	int rc;

	rc = rte_mempool_get(g_task_pool, (void **)&task);
	if ((rc < 0) || !task) {
		SPDK_ERRLOG("Unable to get task\n");
		rte_panic("no memory\n");
	}

	memset(task, 0, sizeof(*task));
	task->vdev = vdev;
	spdk_vhost_scsi_ctrlr_task_ref(task->vdev);
	spdk_scsi_task_construct(&task->scsi, spdk_vhost_task_free_cb, NULL);

	return task;
}

void
spdk_vhost_enqueue_task(struct spdk_vhost_task *task)
{
	need_iovecs_tailq_t *tailq = &g_need_iovecs[rte_lcore_id()];

	TAILQ_INSERT_TAIL(tailq, task, iovecs_link);
}

struct spdk_vhost_task *
spdk_vhost_dequeue_task(void)
{
	need_iovecs_tailq_t *tailq = &g_need_iovecs[rte_lcore_id()];
	struct spdk_vhost_task *task;

	if (TAILQ_EMPTY(tailq))
		return NULL;

	task = TAILQ_FIRST(tailq);
	TAILQ_REMOVE(tailq, task, iovecs_link);

	return task;
}

struct iovec *
spdk_vhost_iovec_alloc(void)
{
	struct iovec *iov = NULL;

	rte_mempool_get(g_iov_buffer_pool, (void **)&iov);
	return iov;
}

void
spdk_vhost_iovec_free(struct iovec *iov)
{
	rte_mempool_put(g_iov_buffer_pool, iov);
}

static int
spdk_vhost_subsystem_init(void)
{
	g_task_pool = rte_mempool_create("vhost task pool", 16384, sizeof(struct spdk_vhost_task),
					 128, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
	if (!g_task_pool) {
		SPDK_ERRLOG("create task pool failed\n");
		return -1;
	}

	g_iov_buffer_pool = rte_mempool_create("vhost iov buffer pool", 2048,
					       VHOST_SCSI_IOVS_LEN * sizeof(struct iovec),
					       128, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
	if (!g_iov_buffer_pool) {
		SPDK_ERRLOG("create iov buffer pool failed\n");
		return -1;
	}

	for (int i = 0; i < RTE_MAX_LCORE; i++) {
		TAILQ_INIT(&g_need_iovecs[i]);
	}

	return 0;
}

static int
spdk_vhost_subsystem_fini(void)
{
	return 0;
}

SPDK_SUBSYSTEM_REGISTER(vhost, spdk_vhost_subsystem_init, spdk_vhost_subsystem_fini, NULL)
SPDK_SUBSYSTEM_DEPEND(vhost, scsi)
