/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include <linux/virtio_blk.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"

#include "vhost_internal.h"
#include "vhost_fs_internal.h"

/* forward declaration */
static const struct spdk_vhost_dev_backend vhost_fs_device_backend;

static inline struct spdk_vhost_fs_task *
fs_task_get(struct spdk_vhost_virtqueue *vq, uint16_t req_id)
{
	struct spdk_vhost_fs_task *task;
	struct spdk_vhost_fs_dev *fvdev;
	struct spdk_vhost_session *vsession;

	/* Get addr of vsession by one task in virtqueue */
	vsession = &((struct spdk_vhost_fs_task *)vq->tasks)[0].fvsession->vsession;
	fvdev = ((struct spdk_vhost_fs_task *)vq->tasks)[0].fvsession->fvdev;

	if (spdk_unlikely(req_id >= vq->vring.size)) {
		SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
			    fvdev->vdev.name, req_id, vq->vring.size);
		spdk_vhost_vq_used_ring_enqueue(vsession, vq, req_id, 0);

		return NULL;
	}

	task = &((struct spdk_vhost_fs_task *)vq->tasks)[req_id];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    fvdev->vdev.name, req_id);
		spdk_vhost_vq_used_ring_enqueue(vsession, vq, req_id, 0);

		return NULL;
	}

	vsession->task_cnt++;

	task->used = true;
	task->in_iovcnt = 0;
	task->out_iovcnt = 0;
	task->used_len = 0;

	return task;
}

static inline void
fs_task_put(struct spdk_vhost_fs_task *task)
{
	assert(task->fvsession->vsession.task_cnt > 0);
	task->fvsession->vsession.task_cnt--;
	task->used = false;
}

static inline void
fs_task_defer(struct spdk_vhost_fs_task *task)
{
	struct spdk_vhost_fs_session *fvsession = task->fvsession;

	TAILQ_INSERT_TAIL(&fvsession->queued_task_list, task, tailq);
}

void
fs_request_finish(struct spdk_vhost_fs_task *task, int err)
{
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Finished task (%p) req_idx=%d\n status: %s\n", task,
		      task->req_idx, !err ? "OK" : "FAIL");

	if (err == EBUSY) {
		fs_task_defer(task);
		return;
	}

	spdk_vhost_vq_used_ring_enqueue(&task->fvsession->vsession, task->vq, task->req_idx,
					task->used_len);
	fs_task_put(task);
}

static int
fs_request_process(struct spdk_vhost_fs_task *task)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;
	char *fuse_arg_in;
	int rc;

	/* Check first readable iov */
	iov = &task->out_iovs[0];
	fuse_in = iov->iov_base;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "FUSE request type '%"PRIu32"'(%s).\n", fuse_in->opcode,
		      spdk_fuse_ll_ops[fuse_in->opcode].op_name);

	/* In general, argument for FUSE operation is the second readable iov.
	 * But for some brief cmds, like Forget, its argument is also in the end of
	 * first readable iov.
	 */
	fuse_arg_in = task->out_iovs[1].iov_base;
	if (task->out_iovs[0].iov_len > sizeof(struct fuse_in_header)) {
		fuse_arg_in = task->out_iovs[0].iov_base + sizeof(struct fuse_in_header);
	}

	rc = spdk_fuse_ll_ops[fuse_in->opcode].func(task, fuse_in->nodeid, fuse_arg_in);
	if (rc < 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Not supported request type '%"PRIu32"'.\n", fuse_in->opcode);
		return -EINVAL;
	}

	return rc;
}

/*
 * Process task's descriptor chain and setup data related fields.
 */
static int
fs_task_iovs_setup(struct spdk_vhost_fs_task *task, struct spdk_vhost_virtqueue *vq,
		   uint32_t *length)
{
	uint16_t req_idx = task->req_idx;
	struct spdk_vhost_fs_session *fvsession = task->fvsession;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size, len = 0;
	uint32_t desc_handled_cnt;
	int rc;

	rc = spdk_vhost_vq_get_desc(vsession, vq, req_idx, &desc, &desc_table, &desc_table_size);
	if (rc != 0) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return -1;
	}

	desc_handled_cnt = 0;
	while (1) {
		struct iovec *iovs;
		uint16_t *cnt;

		if (spdk_vhost_vring_desc_is_wr(desc)) {
			iovs = task->in_iovs;
			cnt = &task->in_iovcnt;
		} else {
			iovs = task->out_iovs;
			cnt = &task->out_iovcnt;
		}

		/*
		 * Check whether reach maximum iov cnt.
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(*cnt == SPDK_COUNTOF(task->in_iovs))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Max IOVs in request reached (req_idx = %"PRIu16").\n",
				      req_idx);
			return -1;
		}

		if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vsession, iovs, cnt, desc))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      req_idx, *cnt);
			return -1;
		}

		len += desc->len;

		rc = spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		if (rc != 0) {
			SPDK_ERRLOG("%s: Descriptor chain at index %"PRIu16" terminated unexpectedly.\n",
				    vdev->name, req_idx);
			return -1;
		} else if (desc == NULL) {
			break;
		}

		desc_handled_cnt++;
		if (spdk_unlikely(desc_handled_cnt > desc_table_size)) {
			/* Break a cycle and report an error, if any. */
			SPDK_ERRLOG("%s: found a cycle in the descriptor chain: desc_table_size = %d, desc_handled_cnt = %d.\n",
				    vdev->name, desc_table_size, desc_handled_cnt);
			return -1;
		}
	}

	/*
	 * From FUSE protocol, at least there is one descriptor for host to read.
	 */
	if (spdk_unlikely(task->out_iovcnt == 0)) {
		return -1;
	}

	*length = len;
	return 0;
}

static int
process_fs_vq_requests(struct spdk_vhost_fs_task *task,
		   struct spdk_vhost_fs_session *fvsession,
		   struct spdk_vhost_virtqueue *vq)
{
	struct iovec *iov;
	struct fuse_in_header *fuse_in;
	//TODO: consider remove payload_len
	uint32_t payload_len;

	if (fs_task_iovs_setup(task, vq, &payload_len)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);

		fs_request_finish(task, EINVAL);
		return -1;
	}

	/* Check first writable iov if it has */
	if (task->in_iovcnt > 0) {
		iov = &task->in_iovs[0];
		if (spdk_unlikely(iov->iov_len != sizeof(struct fuse_out_header))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS,
				      "Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
				      iov->iov_len, 1, task->req_idx);

			fs_request_finish(task, EINVAL);
			return -1;
		}
	}

	/* Check first readable iov */
	iov = &task->out_iovs[0];
	if (spdk_unlikely(iov->iov_len < sizeof(struct fuse_in_header))) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS,
			      "First descriptor size is %zu but expected at least %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(struct fuse_in_header), task->req_idx);

		fs_request_finish(task, EINVAL);
		return -1;
	}

	fuse_in = iov->iov_base;
	task->unique = fuse_in->unique;

	return fs_request_process(task);
}

static void
process_fs_vq(struct spdk_vhost_fs_session *fvsession, uint16_t q_idx)
{
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_virtqueue *vq = &vsession->virtqueue[q_idx];
	struct spdk_vhost_fs_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;
	int rc;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return;
	}

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		task = fs_task_get(vq, reqs[i]);
		if (task == NULL) {
			continue;
		}

		rc = process_fs_vq_requests(task, fvsession, vq);

		if (likely(rc == 0)) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (rc > 0) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d finished early ======\n", task,
				      task->req_idx);
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d failed ======\n", task,
				      task->req_idx);
		}
	}
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_fs_session *fvsession = arg;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	uint16_t q_idx;

	struct spdk_vhost_fs_task *task, *task_tmp;

	/* Check and process deferred tasks due to EBUSY */
	if (!TAILQ_EMPTY(&fvsession->queued_task_list)) {
		TAILQ_FOREACH_SAFE(task, &fvsession->queued_task_list, tailq, task_tmp) {
			int rc;

			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Re-process request idx %"PRIu16"======\n",
					task->req_idx);

			TAILQ_REMOVE(&fvsession->queued_task_list, task, tailq);
			rc = fs_request_process(task);

			if (likely(rc == 0)) {
				SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d submitted ======\n", task,
					      task->req_idx);
			} else if (rc > 0) {
				SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d finished early ======\n", task,
					      task->req_idx);
			} else {
				SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d failed ======\n", task,
					      task->req_idx);
			}
		}
	}

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		process_fs_vq(fvsession, q_idx);
	}

	spdk_vhost_session_used_signal(vsession);

	return -1;
}

static struct spdk_vhost_fs_session *
to_fs_session(struct spdk_vhost_session *vsession)
{
	if (vsession == NULL) {
		return NULL;
	}

	if (vsession->vdev->backend != &vhost_fs_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-fs device\n", vsession->vdev->name);
		return NULL;
	}

	return (struct spdk_vhost_fs_session *)vsession;
}

static struct spdk_vhost_fs_dev *
to_fs_dev(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		return NULL;
	}

	if (vdev->backend != &vhost_fs_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-fs device\n", vdev->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(vdev, struct spdk_vhost_fs_dev, vdev);
}

static void
_vhost_fs_unload_cb(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_dev *fvdev = ctx;

	SPDK_NOTICELOG("vhost-fs %s destoryed\n", fvdev->name);

	free(fvdev->name);
	free(fvdev->cpumask);
	spdk_dma_free(fvdev);
}

static int
_spdk_vhost_session_bdev_remove_cb(struct spdk_vhost_dev *vdev, struct spdk_vhost_session *vsession,
				   void *ctx)
{
	struct spdk_vhost_fs_session *fvsession;

	if (vdev == NULL) {
		/* Nothing to do */
		return 0;
	}

	if (vsession == NULL) {
		/* All sessions have been notified, time to unload fs */
		struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);

		assert(fvdev != NULL);

		/* Use fvdev->bdev as a flag to avoid repeated spdk_fs_unload between
		 * bdev_remove_cb and .remove_device */
		if (fvdev->bdev) {
			spdk_fs_unload(fvdev->fs, _vhost_fs_unload_cb, fvdev);
			fvdev->bdev = NULL;
		}
		return 0;
	}

	fvsession = (struct spdk_vhost_fs_session *)vsession;
	if (fvsession->requestq_poller) {
		spdk_poller_unregister(&fvsession->requestq_poller);
	}

	return 0;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_fs_dev *fvdev = remove_ctx;

	SPDK_WARNLOG("Controller %s: Destory/Hot-removing bdev - all further requests will fail.\n",
		     fvdev->name);

	spdk_vhost_lock();
	spdk_vhost_dev_foreach_session(&fvdev->vdev, _spdk_vhost_session_bdev_remove_cb, NULL);
	spdk_vhost_unlock();
}

static void
free_task_pool(struct spdk_vhost_fs_session *fvsession)
{
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_dma_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_fs_session *fvsession)
{
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_fs_dev *fvdev = fvsession->fvdev;
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_fs_task *task;
	uint32_t task_cnt;
	uint16_t i;
	uint32_t j;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->vring.desc == NULL) {
			continue;
		}

		task_cnt = vq->vring.size;
		if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
			/* sanity check */
			SPDK_ERRLOG("Controller %s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    fvdev->vdev.name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(fvsession);
			return -1;
		}
		vq->tasks = spdk_dma_zmalloc(sizeof(struct spdk_vhost_fs_task) * task_cnt,
					     SPDK_CACHE_LINE_SIZE, NULL);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("Controller %s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    fvdev->vdev.name, task_cnt, i);
			free_task_pool(fvsession);
			return -1;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_vhost_fs_task *)vq->tasks)[j];
			task->fvsession = fvsession;
			task->req_idx = j;
			task->vq = vq;
		}
	}

	return 0;
}

static int
spdk_vhost_fs_start_cb(struct spdk_vhost_dev *vdev,
		       struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_fs_dev *fvdev;
	struct spdk_vhost_fs_session *fvsession;
	int i, rc = 0;

	fvsession = to_fs_session(vsession);
	if (fvsession == NULL) {
		SPDK_ERRLOG("Trying to start non-fs controller as a fs one.\n");
		rc = -1;
		goto out;
	}

	fvdev = to_fs_dev(vdev);
	assert(fvdev != NULL);
	fvsession->fvdev = fvdev;

	/* validate all I/O queues are in a contiguous index range */
	for (i = 0; i < vsession->max_queues; i++) {
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vdev->name, i);
			rc = -1;
			goto out;
		}
	}

	TAILQ_INIT(&fvsession->queued_task_list);

	rc = alloc_task_pool(fvsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", fvdev->vdev.name);
		goto out;
	}

	if (fvdev->fs) {
		fvsession->io_channel = spdk_fs_alloc_io_channel(fvdev->fs);
		if (!fvsession->io_channel) {
			free_task_pool(fvsession);
			SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", vdev->name);
			rc = -1;
			goto out;
		}
	}

	fvsession->requestq_poller = spdk_poller_register(vdev_worker,
				     fvsession, 0);
	SPDK_INFOLOG(SPDK_LOG_VHOST, "Started poller for vhost controller %s on lcore %d\n",
		     vdev->name, vsession->lcore);

out:
	spdk_vhost_session_start_done(vsession, rc);
	return rc;
}

static int
spdk_vhost_fs_start(struct spdk_vhost_session *vsession)
{
	int32_t lcore;
	int rc;

	lcore = spdk_vhost_allocate_reactor(vsession->vdev->cpumask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "controller allocated lcore %d\n", lcore);
	/* the load process of blobfs only can be applied at the master core temporarily */
	assert(lcore == 0);

	rc = spdk_vhost_session_send_event(lcore, vsession, spdk_vhost_fs_start_cb,
					   3, "start session");

	if (rc != 0) {
		spdk_vhost_free_reactor(vsession->lcore);
	}

	return rc;
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_fs_session *fvsession = arg;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	int i;

	if (vsession->task_cnt > 0) {
		return -1;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		vsession->virtqueue[i].next_event_time = 0;
		spdk_vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Stopping poller for vhost controller %s\n", vsession->vdev->name);

	if (fvsession->io_channel) {
		spdk_put_io_channel(fvsession->io_channel);
		fvsession->io_channel = NULL;
	}

	free_task_pool(fvsession);
	spdk_poller_unregister(&fvsession->stop_poller);
	spdk_vhost_session_stop_done(vsession, 0);

	return -1;
}

static int
spdk_vhost_fs_stop_cb(struct spdk_vhost_dev *vdev,
		      struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_fs_session *fvsession;

	fvsession = to_fs_session(vsession);
	if (fvsession == NULL) {
		SPDK_ERRLOG("Trying to stop non-fs controller as a fs one.\n");
		goto err;
	}

	spdk_poller_unregister(&fvsession->requestq_poller);
	fvsession->stop_poller = spdk_poller_register(destroy_session_poller_cb,
				 fvsession, 1000);
	return 0;

err:
	spdk_vhost_session_stop_done(vsession, -1);
	return -1;
}

static int
spdk_vhost_fs_stop(struct spdk_vhost_session *vsession)
{
	SPDK_NOTICELOG("Start to stop vhost fs session\n");
	return spdk_vhost_session_send_event(vsession->lcore, vsession, spdk_vhost_fs_stop_cb,
					     3, "stop session");

	// TODO: justify whether following free_reactor is necessary
//	spdk_vhost_free_reactor(vsession->lcore);
}

static struct spdk_bdev *
spdk_vhost_fs_get_bdev(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);

	assert(fvdev != NULL);
	return fvdev->bdev;
}

static void
spdk_vhost_fs_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev *bdev = spdk_vhost_fs_get_bdev(vdev);
	struct spdk_vhost_fs_dev *fvdev;

	fvdev = to_fs_dev(vdev);
	if (fvdev == NULL) {
		return;
	}

	assert(fvdev != NULL);
	spdk_json_write_named_object_begin(w, "fuse");

	spdk_json_write_named_bool(w, "readonly", fvdev->readonly);

	spdk_json_write_name(w, "bdev");
	if (bdev) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	} else {
		spdk_json_write_null(w);
	}

	spdk_json_write_object_end(w);
}

static void
spdk_vhost_fs_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_fs_dev *fvdev;

	fvdev = to_fs_dev(vdev);
	if (fvdev == NULL) {
		return;
	}

	if (!fvdev->bdev) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_vhost_fs_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(fvdev->bdev));
	spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(vdev->cpumask));
	spdk_json_write_named_bool(w, "readonly", fvdev->readonly);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int spdk_vhost_fs_destroy(struct spdk_vhost_dev *vdev);

static const struct spdk_vhost_dev_backend vhost_fs_device_backend = {
	.virtio_features = SPDK_VHOST_FEATURES,
	/* VIRTIO_RING_F_EVENT_IDX is requested by Guest */
	.disabled_features = (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY),

	.session_ctx_size = sizeof(struct spdk_vhost_fs_session) - sizeof(struct spdk_vhost_session),
	.start_session =  spdk_vhost_fs_start,
	.stop_session = spdk_vhost_fs_stop,
	.dump_info_json = spdk_vhost_fs_dump_info_json,
	.remove_device = spdk_vhost_fs_destroy,
	.write_config_json = spdk_vhost_fs_write_config_json,
};

static void _vhost_fs_controller_construct_next(void *cb_arg);

/* start vhost_fs_controller_construct */
static void
_vhost_fs_controller_construct_next_cb(void *cb_arg, int rc)
{
	struct spdk_conf_section *sp = cb_arg;

	if (rc) {
		unsigned ctrlr_num;

		sscanf(spdk_conf_section_get_name(sp), "VhostFS%u", &ctrlr_num);
		SPDK_ERRLOG("VhostFS%u: failed to consturct vhost-fs\n", ctrlr_num);
	}

	sp = spdk_conf_next_section(sp);
	if (sp) {
		_vhost_fs_controller_construct_next(sp);
	}
}

static void
_vhost_fs_controller_construct_next(void *cb_arg)
{
	struct spdk_conf_section *sp = cb_arg;
	unsigned ctrlr_num;
	char *bdev_name;
	char *cpumask;
	char *name;
	bool readonly;
	int rc;

	for (; sp != NULL; sp = spdk_conf_next_section(sp)) {
		if (!spdk_conf_section_match_prefix(sp, "VhostFS")) {
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostFS%u", &ctrlr_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			continue;
		}

		name = spdk_conf_section_get_val(sp, "Name");
		if (name == NULL) {
			SPDK_ERRLOG("VhostFS%u: missing Name\n", ctrlr_num);
			continue;
		}

		cpumask = spdk_conf_section_get_val(sp, "Cpumask");
		if (cpumask == NULL) {
		} else if (strcmp(cpumask, "0x1") != 0 &&
			   strcmp(cpumask, "0X1") != 0) {
			SPDK_WARNLOG("VhostFS%u: Cpumask must be 0x1 temporarily\n", ctrlr_num);
		}
		cpumask = "0x1";

		readonly = spdk_conf_section_get_boolval(sp, "ReadOnly", false);
		if (readonly == true) {
			SPDK_WARNLOG("VhostFS%u: Readonly is not supported temporarily\n", ctrlr_num);
		}
		readonly = false;

		bdev_name = spdk_conf_section_get_val(sp, "Dev");
		if (bdev_name == NULL) {
			SPDK_ERRLOG("VhostFS%u: missing Dev for bdev\n", ctrlr_num);
			continue;
		}

		rc = spdk_vhost_fs_construct(name, cpumask, bdev_name, readonly,
					     _vhost_fs_controller_construct_next_cb, sp);
		if (rc != 0) {
			SPDK_ERRLOG("VhostFS%u: failed to consturct vhost-fs\n", ctrlr_num);
			continue;
		}

		/* leave and wait for callback */
		return;
	}
}

int
spdk_vhost_fs_controller_construct(void)
{
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	_vhost_fs_controller_construct_next(sp);

	return 0;
}
/* End of vhost_fs_controller_construct */

static void
fs_init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct spdk_vhost_fs_dev *fvdev = ctx;
	uint64_t features = 0;
	int ret = 0;
	char *cpumask = fvdev->cpumask;

	if (fserrno) {
		SPDK_ERRLOG("Failed to mount BlobFS for %s\n", fvdev->name);
		ret = -fserrno;
		goto out;
	}

	SPDK_NOTICELOG("Mounted BlobFS on bdev %s for vhost %s\n", spdk_bdev_get_name(fvdev->bdev),
		       fvdev->name);
	fvdev->fs = fs;

	ret = spdk_vhost_dev_register(&fvdev->vdev, fvdev->name, cpumask, &vhost_fs_device_backend);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to register vhost dev for %s\n", fvdev->name);
		goto out;
	}

	//TODO: set FUSE related virtio features.
	/* Current no special FUSE related virtio features are defined. */
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Controller %s enable features 0x%lx\n", fvdev->name, features);
	if (features && rte_vhost_driver_enable_features(fvdev->vdev.path, features)) {
		SPDK_ERRLOG("Controller %s: failed to enable features 0x%"PRIx64"\n", fvdev->name, features);

		if (spdk_vhost_dev_unregister(&fvdev->vdev) != 0) {
			SPDK_ERRLOG("Controller %s: failed to remove controller\n", fvdev->name);
		}

		ret = -1;
		goto out;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: using bdev '%s'\n", fvdev->name,
		     spdk_bdev_get_name(fvdev->bdev));

out:
	spdk_vhost_unlock();

	fvdev->cb_fn(fvdev->cb_arg, ret);

	if (ret != 0 && fvdev) {
		spdk_dma_free(fvdev);
	}
}

static void
__fs_call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

/* The function for sending sync request to polling thread.
 * It is not necessary for blobfs async API.
 */
static void
__blobfs_send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __fs_call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

int
spdk_vhost_fs_construct(const char *name, const char *cpumask, const char *dev_name, bool readonly,
			spdk_vhost_fs_construct_cb cb_fn, void *cb_arg)
{
	struct spdk_vhost_fs_dev *fvdev = NULL;
	struct spdk_bdev *bdev;
	int ret = 0;

	spdk_vhost_lock();
	bdev = spdk_bdev_get_by_name(dev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Controller %s: bdev '%s' not found\n",
			    name, dev_name);
		ret = -ENODEV;
		goto err;
	}

	fvdev = spdk_dma_zmalloc(sizeof(*fvdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (fvdev == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	fvdev->bdev = bdev;
	fvdev->bs_dev = spdk_bdev_create_bs_dev(bdev, bdev_remove_cb, fvdev);
	if (fvdev->bs_dev == NULL) {
		SPDK_ERRLOG("Failed to mount blobstore on bdev %s\n",
			    spdk_bdev_get_name(bdev));
		goto err;
	}

	SPDK_NOTICELOG("Mounting BlobFS on bdev %s for vhost %s\n", spdk_bdev_get_name(bdev), name);

	fvdev->cb_fn = cb_fn;
	fvdev->cb_arg = cb_arg;
	fvdev->name = strdup(name);
	fvdev->cpumask = strdup(cpumask);
	fvdev->readonly = readonly;
	spdk_fs_load(fvdev->bs_dev, __blobfs_send_request, fs_init_cb, fvdev);

	return 0;

err:
	if (ret != 0 && fvdev) {
		spdk_dma_free(fvdev);
	}
	spdk_vhost_unlock();

	return -1;
}

static int
spdk_vhost_fs_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);
	int rc;

	if (!fvdev) {
		return -EINVAL;
	}

	SPDK_NOTICELOG("destroy vhost-fs %s\n", fvdev->name);
	rc = spdk_vhost_dev_unregister(&fvdev->vdev);
	if (rc != 0) {
		return rc;
	}

	if (fvdev->bdev) {
		spdk_fs_unload(fvdev->fs, _vhost_fs_unload_cb, fvdev);
		fvdev->bdev = NULL;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vhost_fs", SPDK_LOG_VHOST_FS)
