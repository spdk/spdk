/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "accel_dsa.h"

#include "spdk/stdinc.h"

#include "spdk/accel_module.h"
#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/idxd.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

#define ACCEL_DSA_MD_IOBUF_SMALL_CACHE_SIZE			128
#define ACCEL_DSA_MD_IOBUF_LARGE_CACHE_SIZE			32

static bool g_dsa_enable = false;
static bool g_kernel_mode = false;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_ERROR,
};

static bool g_dsa_initialized = false;

struct idxd_device {
	struct				spdk_idxd_device *dsa;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static TAILQ_HEAD(, idxd_device) g_dsa_devices = TAILQ_HEAD_INITIALIZER(g_dsa_devices);
static struct idxd_device *g_next_dev = NULL;
static uint32_t g_num_devices = 0;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

struct idxd_task {
	struct spdk_accel_task	task;
	struct idxd_io_channel	*chan;
	struct iovec		md_iov;
	struct spdk_iobuf_entry	iobuf;
};

struct idxd_io_channel {
	struct spdk_idxd_io_channel	*chan;
	struct idxd_device		*dev;
	enum channel_state		state;
	struct spdk_poller		*poller;
	uint32_t			num_outstanding;
	STAILQ_HEAD(, spdk_accel_task)	queued_tasks;
	struct spdk_iobuf_channel	iobuf;
};

static struct spdk_io_channel *dsa_get_io_channel(void);

static struct idxd_device *
idxd_select_device(struct idxd_io_channel *chan)
{
	uint32_t count = 0;
	struct idxd_device *dev;
	uint32_t numa_id = spdk_env_get_numa_id(spdk_env_get_current_core());

	/*
	 * We allow channels to share underlying devices,
	 * selection is round-robin based with a limitation
	 * on how many channel can share one device.
	 */
	do {
		/* select next device */
		pthread_mutex_lock(&g_dev_lock);
		g_next_dev = TAILQ_NEXT(g_next_dev, tailq);
		if (g_next_dev == NULL) {
			g_next_dev = TAILQ_FIRST(&g_dsa_devices);
		}
		dev = g_next_dev;
		pthread_mutex_unlock(&g_dev_lock);

		if (numa_id != spdk_idxd_get_socket(dev->dsa)) {
			continue;
		}

		/*
		 * Now see if a channel is available on this one. We only
		 * allow a specific number of channels to share a device
		 * to limit outstanding IO for flow control purposes.
		 */
		chan->chan = spdk_idxd_get_channel(dev->dsa);
		if (chan->chan != NULL) {
			SPDK_DEBUGLOG(accel_dsa, "On socket %d using device on numa %d\n",
				      numa_id, spdk_idxd_get_socket(dev->dsa));
			return dev;
		}
	} while (++count < g_num_devices);

	/* We are out of available channels and/or devices for the local socket. We fix the number
	 * of channels that we allocate per device and only allocate devices on the same socket
	 * that the current thread is on. If on a 2 socket system it may be possible to avoid
	 * this situation by spreading threads across the sockets.
	 */
	SPDK_ERRLOG("No more DSA devices available on the local socket.\n");
	return NULL;
}

static void
dsa_done(void *cb_arg, int status)
{
	struct idxd_task *idxd_task = cb_arg;
	struct idxd_io_channel *chan;
	int rc;

	chan = idxd_task->chan;

	/* If the DSA DIF Check operation detects an error, detailed info about
	 * this error (like actual/expected values) needs to be obtained by
	 * calling the software DIF Verify operation.
	 */
	if (spdk_unlikely(status == -EIO)) {
		if (idxd_task->task.op_code == SPDK_ACCEL_OPC_DIF_VERIFY ||
		    idxd_task->task.op_code == SPDK_ACCEL_OPC_DIF_VERIFY_COPY) {
			rc = spdk_dif_verify(idxd_task->task.s.iovs, idxd_task->task.s.iovcnt,
					     idxd_task->task.dif.num_blocks,
					     idxd_task->task.dif.ctx, idxd_task->task.dif.err);
			if (rc != 0) {
				SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
					    idxd_task->task.dif.err->err_type,
					    idxd_task->task.dif.err->err_offset);
			}
		}
	}

	assert(chan->num_outstanding > 0);
	spdk_trace_record(TRACE_ACCEL_DSA_OP_COMPLETE, 0, 0, 0, chan->num_outstanding - 1);
	chan->num_outstanding--;

	spdk_accel_task_complete(&idxd_task->task, status);
}

static int
idxd_submit_dualcast(struct idxd_io_channel *ch, struct idxd_task *idxd_task, int flags)
{
	struct spdk_accel_task *task = &idxd_task->task;

	if (spdk_unlikely(task->d.iovcnt != 1 || task->d2.iovcnt != 1 || task->s.iovcnt != 1)) {
		return -EINVAL;
	}

	if (spdk_unlikely(task->d.iovs[0].iov_len != task->s.iovs[0].iov_len ||
			  task->d.iovs[0].iov_len != task->d2.iovs[0].iov_len)) {
		return -EINVAL;
	}

	return spdk_idxd_submit_dualcast(ch->chan, task->d.iovs[0].iov_base,
					 task->d2.iovs[0].iov_base, task->s.iovs[0].iov_base,
					 task->d.iovs[0].iov_len, flags, dsa_done, idxd_task);
}

static int
check_dsa_dif_strip_overlap_bufs(struct spdk_accel_task *task)
{
	uint64_t src_seg_addr_end_ext;
	uint64_t dst_seg_addr_end_ext;
	size_t i;

	/* The number of source and destination iovecs must be the same.
	 * If so, one of them can be used to iterate over both vectors
	 * later in the loop. */
	if (task->d.iovcnt != task->s.iovcnt) {
		SPDK_ERRLOG("Mismatched iovcnts: src=%d, dst=%d\n",
			    task->s.iovcnt, task->d.iovcnt);
		return -EINVAL;
	}

	for (i = 0; i < task->s.iovcnt; i++) {
		src_seg_addr_end_ext = (uint64_t)task->s.iovs[i].iov_base +
				       task->s.iovs[i].iov_len;

		dst_seg_addr_end_ext = (uint64_t)task->d.iovs[i].iov_base +
				       task->s.iovs[i].iov_len;

		if ((dst_seg_addr_end_ext >= (uint64_t)task->s.iovs[i].iov_base) &&
		    (dst_seg_addr_end_ext <= src_seg_addr_end_ext)) {
			return -EFAULT;
		}
	}

	return 0;
}

static void
spdk_accel_sw_task_complete(void *ctx)
{
	struct spdk_accel_task *task = (struct spdk_accel_task *)ctx;

	spdk_accel_task_complete(task, task->status);
}

static void
_accel_dsa_dix_verify_generate_cb(void *cb_arg, int status)
{
	struct idxd_task *idxd_task = cb_arg;
	struct iovec *original_mdiov = idxd_task->task.d.iovs;
	size_t mdiov_len = idxd_task->md_iov.iov_len;
	int rc;

	if (status != 0) {
		SPDK_ERRLOG("Unable to complete DIX Verify (DIX Generate failed)\n");
		goto end;
	}

	rc = memcmp(original_mdiov->iov_base, idxd_task->md_iov.iov_base, mdiov_len);
	if (rc != 0) {
		SPDK_ERRLOG("DIX Verify failed\n");
		status = -EINVAL;
		rc = spdk_dix_verify(idxd_task->task.s.iovs, idxd_task->task.s.iovcnt,
				     original_mdiov, idxd_task->task.dif.num_blocks,
				     idxd_task->task.dif.ctx, idxd_task->task.dif.err);
		if (rc != 0) {
			SPDK_ERRLOG("DIX error detected. type=%d, offset=%" PRIu32 "\n",
				    idxd_task->task.dif.err->err_type,
				    idxd_task->task.dif.err->err_offset);
		}
	}

end:
	spdk_iobuf_put(&idxd_task->chan->iobuf, idxd_task->md_iov.iov_base, mdiov_len);
	dsa_done(idxd_task, status);
}

static void
_accel_dsa_dix_verify(struct idxd_task *idxd_task)
{
	int rc;

	/* Since Intel DSA doesn't provide a separate DIX Verify operation, it is done
	 * in two steps: DIX Generate to a new buffer and mem compare.
	 */
	rc = spdk_idxd_submit_dix_generate(idxd_task->chan->chan, idxd_task->task.s.iovs,
					   idxd_task->task.s.iovcnt, &idxd_task->md_iov, idxd_task->task.dif.num_blocks,
					   idxd_task->task.dif.ctx, 0, _accel_dsa_dix_verify_generate_cb, idxd_task);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to complete DIX Verify (DIX Generate failed)\n");
		spdk_iobuf_put(&idxd_task->chan->iobuf, idxd_task->md_iov.iov_base,
			       idxd_task->md_iov.iov_len);
		dsa_done(idxd_task, rc);
	}
}

static void
accel_dsa_dix_verify_get_iobuf_cb(struct spdk_iobuf_entry *iobuf, void *buf)
{
	struct idxd_task *idxd_task;

	idxd_task = SPDK_CONTAINEROF(iobuf, struct idxd_task, iobuf);
	idxd_task->md_iov.iov_base = buf;
	_accel_dsa_dix_verify(idxd_task);
}

static int
accel_dsa_dix_verify(struct idxd_io_channel *chan, int flags,
		     struct idxd_task *idxd_task)
{
	idxd_task->md_iov.iov_len = idxd_task->task.d.iovs[0].iov_len;
	idxd_task->md_iov.iov_base = spdk_iobuf_get(&chan->iobuf, idxd_task->md_iov.iov_len,
				     &idxd_task->iobuf, accel_dsa_dix_verify_get_iobuf_cb);

	if (idxd_task->md_iov.iov_base != NULL) {
		_accel_dsa_dix_verify(idxd_task);
	}

	return 0;
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_task *idxd_task;
	int rc = 0, flags = 0;

	idxd_task = SPDK_CONTAINEROF(task, struct idxd_task, task);
	idxd_task->chan = chan;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_COPY:
		rc = spdk_idxd_submit_copy(chan->chan, task->d.iovs, task->d.iovcnt,
					   task->s.iovs, task->s.iovcnt, flags, dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_DUALCAST:
		rc = idxd_submit_dualcast(chan, idxd_task, flags);
		break;
	case SPDK_ACCEL_OPC_COMPARE:
		rc = spdk_idxd_submit_compare(chan->chan, task->s.iovs, task->s.iovcnt,
					      task->s2.iovs, task->s2.iovcnt, flags,
					      dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_FILL:
		rc = spdk_idxd_submit_fill(chan->chan, task->d.iovs, task->d.iovcnt,
					   task->fill_pattern, flags, dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		rc = spdk_idxd_submit_crc32c(chan->chan, task->s.iovs, task->s.iovcnt, task->seed,
					     task->crc_dst, flags, dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		rc = spdk_idxd_submit_copy_crc32c(chan->chan, task->d.iovs, task->d.iovcnt,
						  task->s.iovs, task->s.iovcnt,
						  task->seed, task->crc_dst, flags,
						  dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_DIF_VERIFY:
		rc = spdk_idxd_submit_dif_check(chan->chan,
						task->s.iovs, task->s.iovcnt,
						task->dif.num_blocks, task->dif.ctx, flags,
						dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
		rc = spdk_idxd_submit_dif_insert(chan->chan,
						 task->d.iovs, task->d.iovcnt,
						 task->s.iovs, task->s.iovcnt,
						 task->dif.num_blocks, task->dif.ctx, flags,
						 dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
		/* For DIF strip operations, DSA may incorrectly report an overlapping buffer
		 * error if the destination buffer immediately precedes the source buffer.
		 * This is because DSA uses the transfer size in the descriptor for both
		 * the source and destination buffers when checking for buffer overlap.
		 * Since the transfer size applies to the source buffer, which is larger
		 * than the destination buffer by metadata, it should not be used as
		 * the destination buffer size. To avoid reporting errors by DSA, the software
		 * checks whether such an error condition can occur, and if so the software
		 * fallback is performed. */
		rc = check_dsa_dif_strip_overlap_bufs(task);
		if (rc == 0) {
			rc = spdk_idxd_submit_dif_strip(chan->chan,
							task->d.iovs, task->d.iovcnt,
							task->s.iovs, task->s.iovcnt,
							task->dif.num_blocks, task->dif.ctx, flags,
							dsa_done, idxd_task);
		} else if (rc == -EFAULT) {
			rc = spdk_dif_verify_copy(task->d.iovs,
						  task->d.iovcnt,
						  task->s.iovs,
						  task->s.iovcnt,
						  task->dif.num_blocks,
						  task->dif.ctx,
						  task->dif.err);
			idxd_task->task.status = rc;
			spdk_thread_send_msg(spdk_get_thread(), spdk_accel_sw_task_complete, (void *)&idxd_task->task);
			rc = 0;
		}
		break;
	case SPDK_ACCEL_OPC_DIX_GENERATE:
		rc = spdk_idxd_submit_dix_generate(chan->chan, task->s.iovs, task->s.iovcnt,
						   task->d.iovs, task->dif.num_blocks,
						   task->dif.ctx, flags, dsa_done, idxd_task);
		break;
	case SPDK_ACCEL_OPC_DIX_VERIFY:
		rc = accel_dsa_dix_verify(chan, flags, idxd_task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		chan->num_outstanding++;
		spdk_trace_record(TRACE_ACCEL_DSA_OP_SUBMIT, 0, 0, 0, chan->num_outstanding);
	}

	return rc;
}

static int
dsa_submit_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	assert(STAILQ_NEXT(task, link) == NULL);

	if (spdk_unlikely(chan->state == IDXD_CHANNEL_ERROR)) {
		spdk_accel_task_complete(task, -EINVAL);
		return 0;
	}

	if (!STAILQ_EMPTY(&chan->queued_tasks)) {
		STAILQ_INSERT_TAIL(&chan->queued_tasks, task, link);
		return 0;
	}

	rc = _process_single_task(ch, task);
	if (rc == -EBUSY) {
		STAILQ_INSERT_TAIL(&chan->queued_tasks, task, link);
	} else if (rc) {
		spdk_accel_task_complete(task, rc);
	}

	return 0;
}

static int
dsa_submit_queued_tasks(struct idxd_io_channel *chan)
{
	struct spdk_accel_task *task, *tmp;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(chan);
	int rc = 0;

	if (spdk_unlikely(chan->state == IDXD_CHANNEL_ERROR)) {
		/* Complete queued tasks with error and clear the list */
		while ((task = STAILQ_FIRST(&chan->queued_tasks))) {
			STAILQ_REMOVE_HEAD(&chan->queued_tasks, link);
			spdk_accel_task_complete(task, -EINVAL);
		}
		return 0;
	}

	STAILQ_FOREACH_SAFE(task, &chan->queued_tasks, link, tmp) {
		rc = _process_single_task(ch, task);
		if (rc == -EBUSY) {
			return rc;
		}
		STAILQ_REMOVE_HEAD(&chan->queued_tasks, link);
		if (rc) {
			spdk_accel_task_complete(task, rc);
		}
	}

	return 0;
}

static int
idxd_poll(void *arg)
{
	struct idxd_io_channel *chan = arg;
	int count;

	count = spdk_idxd_process_events(chan->chan);

	/* Check if there are any pending ops to process if the channel is active */
	if (!STAILQ_EMPTY(&chan->queued_tasks)) {
		dsa_submit_queued_tasks(chan);
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static size_t
accel_dsa_get_ctx_size(void)
{
	return sizeof(struct idxd_task);
}

static bool
dsa_supports_opcode(enum spdk_accel_opcode opc)
{
	if (!g_dsa_initialized) {
		assert(0);
		return false;
	}

	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_DUALCAST:
	case SPDK_ACCEL_OPC_COMPARE:
	case SPDK_ACCEL_OPC_CRC32C:
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		return true;
	case SPDK_ACCEL_OPC_DIF_VERIFY:
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
	case SPDK_ACCEL_OPC_DIF_VERIFY_COPY:
	/* In theory, DIX Generate could work without the iommu, but iommu is required
	 * for consistency with other DIF operations.
	 */
	case SPDK_ACCEL_OPC_DIX_GENERATE:
	case SPDK_ACCEL_OPC_DIX_VERIFY:
		/* Supported only if the IOMMU is enabled */
		return spdk_iommu_is_enabled();
	default:
		return false;
	}
}

static int accel_dsa_init(void);
static void accel_dsa_exit(void *ctx);
static void accel_dsa_write_config_json(struct spdk_json_write_ctx *w);

static struct spdk_accel_module_if g_dsa_module = {
	.module_init = accel_dsa_init,
	.module_fini = accel_dsa_exit,
	.write_config_json = accel_dsa_write_config_json,
	.get_ctx_size = accel_dsa_get_ctx_size,
	.name			= "dsa",
	.supports_opcode	= dsa_supports_opcode,
	.get_io_channel		= dsa_get_io_channel,
	.submit_tasks		= dsa_submit_task
};

static int
dsa_create_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	struct idxd_device *dsa;
	int rc;

	dsa = idxd_select_device(chan);
	if (dsa == NULL) {
		SPDK_ERRLOG("Failed to get an idxd channel\n");
		return -EINVAL;
	}

	chan->dev = dsa;
	chan->poller = SPDK_POLLER_REGISTER(idxd_poll, chan, 0);
	STAILQ_INIT(&chan->queued_tasks);
	chan->num_outstanding = 0;
	chan->state = IDXD_CHANNEL_ACTIVE;
	rc = spdk_iobuf_channel_init(&chan->iobuf, "accel_dsa",
				     ACCEL_DSA_MD_IOBUF_SMALL_CACHE_SIZE,
				     ACCEL_DSA_MD_IOBUF_LARGE_CACHE_SIZE);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create an iobuf channel in accel dsa\n");
		return -ENOMEM;
	}

	return 0;
}

static void
dsa_destroy_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;

	spdk_iobuf_channel_fini(&chan->iobuf);
	spdk_poller_unregister(&chan->poller);
	spdk_idxd_put_channel(chan->chan);
}

static struct spdk_io_channel *
dsa_get_io_channel(void)
{
	return spdk_get_io_channel(&g_dsa_module);
}

static void
attach_cb(void *cb_ctx, struct spdk_idxd_device *idxd)
{
	struct idxd_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->dsa = idxd;
	if (g_next_dev == NULL) {
		g_next_dev = dev;
	}

	TAILQ_INSERT_TAIL(&g_dsa_devices, dev, tailq);
	g_num_devices++;
}

int
accel_dsa_enable_probe(bool kernel_mode)
{
	int rc;

	if (g_dsa_enable) {
		return -EALREADY;
	}

	rc = spdk_idxd_set_config(kernel_mode);
	if (rc != 0) {
		return rc;
	}

	spdk_accel_module_list_add(&g_dsa_module);
	g_kernel_mode = kernel_mode;
	g_dsa_enable = true;

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (dev->id.device_id == PCI_DEVICE_ID_INTEL_DSA || dev->id.device_id == PCI_DEVICE_ID_INTEL_DSA3) {
		return true;
	}

	return false;
}

static int
accel_dsa_init(void)
{
	int rc;

	if (!g_dsa_enable) {
		return -EINVAL;
	}

	if (spdk_idxd_probe(NULL, attach_cb, probe_cb) != 0) {
		SPDK_ERRLOG("spdk_idxd_probe() failed\n");
		return -EINVAL;
	}

	if (TAILQ_EMPTY(&g_dsa_devices)) {
		return -ENODEV;
	}

	rc = spdk_iobuf_register_module("accel_dsa");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register accel_dsa iobuf module\n");
		return rc;
	}

	g_dsa_initialized = true;
	spdk_io_device_register(&g_dsa_module, dsa_create_cb, dsa_destroy_cb,
				sizeof(struct idxd_io_channel), "dsa_accel_module");
	return 0;
}

static void
accel_dsa_exit(void *ctx)
{
	struct idxd_device *dev;

	if (g_dsa_initialized) {
		spdk_io_device_unregister(&g_dsa_module, NULL);
		g_dsa_initialized = false;
	}

	while (!TAILQ_EMPTY(&g_dsa_devices)) {
		dev = TAILQ_FIRST(&g_dsa_devices);
		TAILQ_REMOVE(&g_dsa_devices, dev, tailq);
		spdk_idxd_detach(dev->dsa);
		free(dev);
	}

	spdk_accel_module_finish();
}

static void
accel_dsa_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_dsa_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "dsa_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_bool(w, "config_kernel_mode", g_kernel_mode);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

static void
dsa_trace(void)
{
	spdk_trace_register_description("DSA_OP_SUBMIT", TRACE_ACCEL_DSA_OP_SUBMIT, OWNER_TYPE_NONE,
					OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "count");
	spdk_trace_register_description("DSA_OP_COMPLETE", TRACE_ACCEL_DSA_OP_COMPLETE, OWNER_TYPE_NONE,
					OBJECT_NONE,
					0, SPDK_TRACE_ARG_TYPE_INT, "count");
}
SPDK_TRACE_REGISTER_FN(dsa_trace, "dsa", TRACE_GROUP_ACCEL_DSA)

SPDK_LOG_REGISTER_COMPONENT(accel_dsa)
