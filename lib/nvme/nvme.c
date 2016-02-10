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

#include "nvme_internal.h"

/** \file
 *
 */

struct nvme_driver g_nvme_driver = {
	.lock = NVME_MUTEX_INITIALIZER,
	.max_io_queues = DEFAULT_MAX_IO_QUEUES,
	.init_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_driver.init_ctrlrs),
	.attached_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_driver.attached_ctrlrs),
};

int32_t		spdk_nvme_retry_count;
__thread int	nvme_thread_ioq_index = -1;


/**
 * \page nvme_initialization NVMe Initialization

\msc

	app [label="Application"], nvme [label="NVMe Driver"];
	app=>nvme [label="nvme_probe()"];
	app<<nvme [label="probe_cb(pci_dev)"];
	nvme=>nvme [label="nvme_attach(devhandle)"];
	nvme=>nvme [label="nvme_ctrlr_start(nvme_controller ptr)"];
	nvme=>nvme [label="identify controller"];
	nvme=>nvme [label="create queue pairs"];
	nvme=>nvme [label="identify namespace(s)"];
	app<<nvme [label="attach_cb(pci_dev, nvme_controller)"];
	app=>app [label="create block devices based on controller's namespaces"];

\endmsc

 */

static struct spdk_nvme_ctrlr *
nvme_attach(void *devhandle)
{
	struct spdk_nvme_ctrlr	*ctrlr;
	int			status;
	uint64_t		phys_addr = 0;

	ctrlr = nvme_malloc("nvme_ctrlr", sizeof(struct spdk_nvme_ctrlr),
			    64, &phys_addr);
	if (ctrlr == NULL) {
		nvme_printf(NULL, "could not allocate ctrlr\n");
		return NULL;
	}

	status = nvme_ctrlr_construct(ctrlr, devhandle);
	if (status != 0) {
		nvme_free(ctrlr);
		return NULL;
	}

	return ctrlr;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_driver	*driver = &g_nvme_driver;

	nvme_mutex_lock(&driver->lock);

	nvme_ctrlr_destruct(ctrlr);
	TAILQ_REMOVE(&g_nvme_driver.attached_ctrlrs, ctrlr, tailq);
	nvme_free(ctrlr);

	nvme_mutex_unlock(&driver->lock);
	return 0;
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_completion_poll_status	*status = arg;

	/*
	 * Copy status into the argument passed by the caller, so that
	 *  the caller can check the status to determine if the
	 *  the request passed or failed.
	 */
	memcpy(&status->cpl, cpl, sizeof(*cpl));
	status->done = true;
}

size_t
spdk_nvme_request_size(void)
{
	return sizeof(struct nvme_request);
}

struct nvme_request *
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req = NULL;

	nvme_alloc_request(&req);

	if (req == NULL) {
		return req;
	}

	/*
	 * Only memset up to (but not including) the children
	 *  TAILQ_ENTRY.  children, and following members, are
	 *  only used as part of I/O splitting so we avoid
	 *  memsetting them until it is actually needed.
	 *  They will be initialized in nvme_request_add_child()
	 *  if the request is split.
	 */
	memset(req, 0, offsetof(struct nvme_request, children));
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->timeout = true;
	req->parent = NULL;
	req->payload = *payload;
	req->payload_size = payload_size;

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	return nvme_allocate_request(&payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(NULL, 0, cb_fn, cb_arg);
}

void
nvme_free_request(struct nvme_request *req)
{
	nvme_assert(req != NULL, ("nvme_free_request(NULL)\n"));
	nvme_dealloc_request(req);
}

static int
nvme_allocate_ioq_index(void)
{
	struct nvme_driver	*driver = &g_nvme_driver;
	uint32_t		i;

	nvme_mutex_lock(&driver->lock);
	if (driver->ioq_index_pool == NULL) {
		driver->ioq_index_pool =
			calloc(driver->max_io_queues, sizeof(*driver->ioq_index_pool));
		if (driver->ioq_index_pool) {
			for (i = 0; i < driver->max_io_queues; i++) {
				driver->ioq_index_pool[i] = i;
			}
		} else {
			nvme_mutex_unlock(&driver->lock);
			return -1;
		}
		driver->ioq_index_pool_next = 0;
	}

	if (driver->ioq_index_pool_next < driver->max_io_queues) {
		nvme_thread_ioq_index = driver->ioq_index_pool[driver->ioq_index_pool_next];
		driver->ioq_index_pool[driver->ioq_index_pool_next] = -1;
		driver->ioq_index_pool_next++;
	} else {
		nvme_thread_ioq_index = -1;
	}

	nvme_mutex_unlock(&driver->lock);
	return 0;
}

static void
nvme_free_ioq_index(void)
{
	struct nvme_driver	*driver = &g_nvme_driver;

	nvme_mutex_lock(&driver->lock);
	if (nvme_thread_ioq_index >= 0) {
		driver->ioq_index_pool_next--;
		driver->ioq_index_pool[driver->ioq_index_pool_next] = nvme_thread_ioq_index;
		nvme_thread_ioq_index = -1;
	}
	nvme_mutex_unlock(&driver->lock);
}

int
spdk_nvme_register_io_thread(void)
{
	int rc = 0;

	if (nvme_thread_ioq_index >= 0) {
		nvme_printf(NULL, "thread already registered\n");
		return -1;
	}

	rc = nvme_allocate_ioq_index();
	if (rc) {
		nvme_printf(NULL, "ioq_index_pool alloc failed\n");
		return rc;
	}
	return (nvme_thread_ioq_index >= 0) ? 0 : -1;
}

void
spdk_nvme_unregister_io_thread(void)
{
	nvme_free_ioq_index();
}

struct nvme_enum_ctx {
	spdk_nvme_probe_cb probe_cb;
	void *cb_ctx;
};

/* This function must only be called while holding g_nvme_driver.lock */
static int
nvme_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct nvme_enum_ctx *enum_ctx = ctx;
	struct spdk_nvme_ctrlr *ctrlr;

	/* Verify that this controller is not already attached */
	TAILQ_FOREACH(ctrlr, &g_nvme_driver.attached_ctrlrs, tailq) {
		/* NOTE: This assumes that the PCI abstraction layer will use the same device handle
		 *  across enumerations; we could compare by BDF instead if this is not true.
		 */
		if (pci_dev == ctrlr->devhandle) {
			return 0;
		}
	}

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		ctrlr = nvme_attach(pci_dev);
		if (ctrlr == NULL) {
			nvme_printf(NULL, "nvme_attach() failed\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&g_nvme_driver.init_ctrlrs, ctrlr, tailq);
	}

	return 0;
}

int
spdk_nvme_probe(void *cb_ctx, spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb)
{
	int rc, start_rc;
	struct nvme_enum_ctx enum_ctx;
	struct spdk_nvme_ctrlr *ctrlr;

	nvme_mutex_lock(&g_nvme_driver.lock);

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.cb_ctx = cb_ctx;

	rc = nvme_pci_enumerate(nvme_enum_cb, &enum_ctx);
	/*
	 * Keep going even if one or more nvme_attach() calls failed,
	 *  but maintain the value of rc to signal errors when we return.
	 */

	/* TODO: This could be reworked to start all the controllers in parallel. */
	while (!TAILQ_EMPTY(&g_nvme_driver.init_ctrlrs)) {
		/* Remove ctrlr from init_ctrlrs and attempt to start it */
		ctrlr = TAILQ_FIRST(&g_nvme_driver.init_ctrlrs);
		TAILQ_REMOVE(&g_nvme_driver.init_ctrlrs, ctrlr, tailq);

		/*
		 * Drop the driver lock while calling nvme_ctrlr_start() since it needs to acquire
		 *  the driver lock internally.
		 *
		 * TODO: Rethink the locking - maybe reset should take the lock so that start() and
		 *  the functions it calls (in particular nvme_ctrlr_set_num_qpairs())
		 *  can assume it is held.
		 */
		nvme_mutex_unlock(&g_nvme_driver.lock);
		start_rc = nvme_ctrlr_start(ctrlr);
		nvme_mutex_lock(&g_nvme_driver.lock);

		if (start_rc == 0) {
			TAILQ_INSERT_TAIL(&g_nvme_driver.attached_ctrlrs, ctrlr, tailq);

			/*
			 * Unlock while calling attach_cb() so the user can call other functions
			 *  that may take the driver lock, like nvme_detach().
			 */
			nvme_mutex_unlock(&g_nvme_driver.lock);
			attach_cb(cb_ctx, ctrlr->devhandle, ctrlr);
			nvme_mutex_lock(&g_nvme_driver.lock);
		} else {
			nvme_ctrlr_destruct(ctrlr);
			nvme_free(ctrlr);
			rc = -1;
		}
	}

	nvme_mutex_unlock(&g_nvme_driver.lock);
	return rc;
}
