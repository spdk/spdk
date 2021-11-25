/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
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

#include <accel-config/libaccel_config.h>

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd.h"

#define MAX_DSA_DEVICE_ID  16

struct device_config g_kernel_dev_cfg = {};

struct spdk_wq_context {
	struct accfg_wq *wq;
	unsigned int    max_batch_size;
	unsigned int    max_xfer_size;
	unsigned int    max_xfer_bits;

	int fd;
	int wq_idx;
	void *wq_reg;
	int wq_size;
	int dedicated;
	int bof;

	unsigned int wq_max_batch_size;
	unsigned long wq_max_xfer_size;
};

struct spdk_kernel_idxd_device {
	struct spdk_idxd_device idxd;
	struct accfg_ctx        *ctx;
	struct spdk_wq_context  *wq_ctx;
	uint32_t                wq_active_num;
};

#define __kernel_idxd(idxd) SPDK_CONTAINEROF(idxd, struct spdk_kernel_idxd_device, idxd)

/* Bit scan reverse */
static uint32_t bsr(uint32_t val)
{
	uint32_t msb;

	msb = (val == 0) ? 0 : 32 - __builtin_clz(val);
	return msb - 1;
}

static void init_idxd_impl(struct spdk_idxd_device *idxd);

static int
dsa_setup_single_wq(struct spdk_kernel_idxd_device *kernel_idxd, struct accfg_wq *wq, int shared)
{
	struct accfg_device *dev;
	int major, minor;
	char path[1024];
	struct spdk_wq_context *wq_ctx = &kernel_idxd->wq_ctx[kernel_idxd->wq_active_num];

	dev = accfg_wq_get_device(wq);
	major = accfg_device_get_cdev_major(dev);
	if (major < 0) {
		return -ENODEV;
	}
	minor = accfg_wq_get_cdev_minor(wq);
	if (minor < 0) {
		return -ENODEV;
	}

	snprintf(path, sizeof(path), "/dev/char/%u:%u", major, minor);
	wq_ctx->fd = open(path, O_RDWR);
	if (wq_ctx->fd < 0) {
		SPDK_ERRLOG("Can not open the Working queue file descriptor on path=%s\n",
			    path);
		return -errno;
	}

	wq_ctx->wq_reg = mmap(NULL, 0x1000, PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE, wq_ctx->fd, 0);
	if (wq_ctx->wq_reg == MAP_FAILED) {
		perror("mmap");
		return -errno;
	}

	wq_ctx->dedicated = !shared;
	wq_ctx->wq_size = accfg_wq_get_size(wq);
	wq_ctx->wq_idx = accfg_wq_get_id(wq);
	wq_ctx->bof = accfg_wq_get_block_on_fault(wq);
	wq_ctx->wq_max_batch_size = accfg_wq_get_max_batch_size(wq);
	wq_ctx->wq_max_xfer_size = accfg_wq_get_max_transfer_size(wq);

	wq_ctx->max_batch_size = accfg_device_get_max_batch_size(dev);
	wq_ctx->max_xfer_size = accfg_device_get_max_transfer_size(dev);
	wq_ctx->max_xfer_bits = bsr(wq_ctx->max_xfer_size);

	SPDK_NOTICELOG("alloc wq %d shared %d size %d addr %p batch sz %#x xfer sz %#x\n",
		       wq_ctx->wq_idx, shared, wq_ctx->wq_size, wq_ctx->wq_reg,
		       wq_ctx->max_batch_size, wq_ctx->max_xfer_size);

	wq_ctx->wq = wq;

	/* Update the active_wq_num of the kernel device */
	kernel_idxd->wq_active_num++;
	kernel_idxd->idxd.total_wq_size += wq_ctx->wq_size;
	kernel_idxd->idxd.socket_id = accfg_device_get_numa_node(dev);

	return 0;
}

static int
config_wqs(struct spdk_kernel_idxd_device *kernel_idxd,
	   int dev_id, int shared)
{
	struct accfg_device *device;
	struct accfg_wq *wq;
	int rc;

	accfg_device_foreach(kernel_idxd->ctx, device) {
		enum accfg_device_state dstate;

		/* Make sure that the device is enabled */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED) {
			continue;
		}

		/* Match the device to the id requested */
		if (accfg_device_get_id(device) != dev_id &&
		    dev_id != -1) {
			continue;
		}

		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;
			enum accfg_wq_mode mode;
			enum accfg_wq_type type;

			/* Get a workqueue that's enabled */
			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED) {
				continue;
			}

			/* The wq type should be user */
			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER) {
				continue;
			}

			/* Make sure the mode is correct */
			mode = accfg_wq_get_mode(wq);
			if ((mode == ACCFG_WQ_SHARED && !shared)
			    || (mode == ACCFG_WQ_DEDICATED && shared)) {
				continue;
			}

			/* We already config enough work queues */
			if (kernel_idxd->wq_active_num == g_kernel_dev_cfg.total_wqs) {
				break;
			}

			rc = dsa_setup_single_wq(kernel_idxd, wq, shared);
			if (rc < 0) {
				return -1;
			}
		}
	}

	if ((kernel_idxd->wq_active_num != 0) &&
	    (kernel_idxd->wq_active_num != g_kernel_dev_cfg.total_wqs)) {
		SPDK_ERRLOG("Failed to configure the expected wq nums=%d, and get the real wq nums=%d\n",
			    g_kernel_dev_cfg.total_wqs, kernel_idxd->wq_active_num);
		return -1;
	}

	/* Spread the channels we allow per device based on the total number of WQE to try
	 * and achieve optimal performance for common cases.
	 */
	kernel_idxd->idxd.chan_per_device = (kernel_idxd->idxd.total_wq_size >= 128) ? 8 : 4;
	return 0;
}

static void
kernel_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	uint32_t i;
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);

	if (kernel_idxd->wq_ctx) {
		for (i = 0; i < kernel_idxd->wq_active_num; i++) {
			if (munmap(kernel_idxd->wq_ctx[i].wq_reg, 0x1000)) {
				SPDK_ERRLOG("munmap failed %d on kernel_device=%p on dsa_context with wq_reg=%p\n",
					    errno, kernel_idxd, kernel_idxd->wq_ctx[i].wq_reg);
			}
			close(kernel_idxd->wq_ctx[i].fd);
		}
		free(kernel_idxd->wq_ctx);
	}

	accfg_unref(kernel_idxd->ctx);
	free(idxd);
}

/*
 * Build work queue (WQ) config based on getting info from the device combined
 * with the defined configuration. Once built, it is written to the device.
 */
static int
kernel_idxd_wq_config(struct spdk_kernel_idxd_device *kernel_idxd)
{
	uint32_t i;
	struct idxd_wq *queue;
	struct spdk_idxd_device *idxd = &kernel_idxd->idxd;

	/* initialize the group */
	idxd->groups = calloc(g_kernel_dev_cfg.num_groups, sizeof(struct idxd_group));
	if (idxd->groups == NULL) {
		SPDK_ERRLOG("Failed to allocate group memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_kernel_dev_cfg.num_groups; i++) {
		idxd->groups[i].idxd = idxd;
		idxd->groups[i].id = i;
	}

	idxd->queues = calloc(g_kernel_dev_cfg.total_wqs, sizeof(struct idxd_wq));
	if (idxd->queues == NULL) {
		SPDK_ERRLOG("Failed to allocate queue memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_kernel_dev_cfg.total_wqs; i++) {
		queue = &idxd->queues[i];
		queue->wqcfg.wq_size = kernel_idxd->wq_ctx[i].wq_size;
		queue->wqcfg.mode = WQ_MODE_DEDICATED;
		queue->wqcfg.max_batch_shift = LOG2_WQ_MAX_BATCH;
		queue->wqcfg.max_xfer_shift = LOG2_WQ_MAX_XFER;
		queue->wqcfg.wq_state = WQ_ENABLED;
		queue->wqcfg.priority = WQ_PRIORITY_1;

		/* Not part of the config struct */
		queue->idxd = idxd;
		queue->group = &idxd->groups[i % g_kernel_dev_cfg.num_groups];
	}

	return 0;
}

static int
_kernel_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb, int dev_id)
{
	int rc;
	struct spdk_kernel_idxd_device *kernel_idxd;
	struct accfg_ctx *ctx;

	kernel_idxd = calloc(1, sizeof(struct spdk_kernel_idxd_device));
	if (kernel_idxd == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for kernel_idxd device.\n");
		return -ENOMEM;
	}

	kernel_idxd->wq_ctx = calloc(g_kernel_dev_cfg.total_wqs, sizeof(struct spdk_wq_context));
	if (kernel_idxd->wq_ctx == NULL) {
		rc = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for the work queue contexts on kernel_idxd=%p.\n",
			    kernel_idxd);
		goto end;
	}

	rc = accfg_new(&ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to allocate accfg context when probe kernel_idxd=%p\n", kernel_idxd);
		goto end;
	}

	init_idxd_impl(&kernel_idxd->idxd);
	kernel_idxd->ctx = ctx;

	/* Supporting non-shared mode first.
	 * Todo: Add the shared mode support later.
	 */
	rc = config_wqs(kernel_idxd, dev_id, 0);
	if (rc) {
		SPDK_ERRLOG("Failed to probe requested wqs on kernel device context=%p\n", ctx);
		return -ENODEV;
	}

	/* No active work queues */
	if (kernel_idxd->wq_active_num == 0) {
		goto end;
	}

	kernel_idxd_wq_config(kernel_idxd);

	attach_cb(cb_ctx, &kernel_idxd->idxd);

	SPDK_NOTICELOG("Successfully got an kernel device=%p\n", kernel_idxd);
	return 0;

end:
	kernel_idxd_device_destruct(&kernel_idxd->idxd);
	return rc;
}

static int
kernel_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb)
{
	int i;

	for (i = 0; i < MAX_DSA_DEVICE_ID; i++) {
		_kernel_idxd_probe(cb_ctx, attach_cb, i);
	}

	return 0;
}

static void
kernel_idxd_dump_sw_error(struct spdk_idxd_device *idxd, void *portal)
{
	/* Need to be enhanced later */
}

static void
kernel_idxd_set_config(struct device_config *dev_cfg, uint32_t config_num)
{
	g_kernel_dev_cfg = *dev_cfg;
}

static char *
kernel_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);
	assert(idxd->wq_id <= (g_kernel_dev_cfg.total_wqs - 1));
	return (char *)kernel_idxd->wq_ctx[idxd->wq_id].wq_reg;
}

static struct spdk_idxd_impl g_kernel_idxd_impl = {
	.name			= "kernel",
	.set_config		= kernel_idxd_set_config,
	.probe			= kernel_idxd_probe,
	.destruct		= kernel_idxd_device_destruct,
	.dump_sw_error		= kernel_idxd_dump_sw_error,
	.portal_get_addr	= kernel_idxd_portal_get_addr,
};

static void
init_idxd_impl(struct spdk_idxd_device *idxd)
{
	idxd->impl = &g_kernel_idxd_impl;
}

SPDK_IDXD_IMPL_REGISTER(kernel, &g_kernel_idxd_impl);
