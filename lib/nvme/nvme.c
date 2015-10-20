/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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
	.max_io_queues = NVME_MAX_IO_QUEUES
};

int32_t		nvme_retry_count;
int __thread	nvme_thread_ioq_index = -1;


/**
 * \page nvme_initialization NVMe Initialization

\msc

	app [label="Application"], nvme [label="NVMe Driver"];
	app=>nvme [label="nvme_attach(devhandle)"];
	app<<nvme [label="nvme_controller ptr"];
	app=>nvme [label="nvme_ctrlr_start(nvme_controller ptr)"];
	nvme=>nvme [label="identify controller"];
	nvme=>nvme [label="create queue pairs"];
	nvme=>nvme [label="identify namespace(s)"];
	app=>app [label="create block devices based on controller's namespaces"];

\endmsc

 */

struct nvme_controller *
nvme_attach(void *devhandle)
{
	struct nvme_controller	*ctrlr;
	int			status;
	uint64_t		phys_addr = 0;

	ctrlr = nvme_malloc("nvme_ctrlr", sizeof(struct nvme_controller),
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

	if (nvme_ctrlr_start(ctrlr) != 0) {
		nvme_ctrlr_destruct(ctrlr);
		nvme_free(ctrlr);
		return NULL;
	}

	return ctrlr;
}

int
nvme_detach(struct nvme_controller *ctrlr)
{
	nvme_ctrlr_destruct(ctrlr);
	nvme_free(ctrlr);
	return 0;
}

void
nvme_completion_poll_cb(void *arg, const struct nvme_completion *cpl)
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
nvme_request_size(void)
{
	return sizeof(struct nvme_request);
}

struct nvme_request *
nvme_allocate_request(void *payload, uint32_t payload_size,
		      nvme_cb_fn_t cb_fn, void *cb_arg)
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
	nvme_assert((payload == NULL && payload_size == 0) ||
		    (payload != NULL && payload_size != 0),
		    ("Invalid argument combination of payload and payload_size\n"));
	if (payload == NULL || payload_size == 0) {
		req->u.payload = NULL;
		req->payload_size = 0;
	} else {
		req->u.payload = payload;
		req->payload_size = payload_size;
	}

	return req;
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
nvme_register_io_thread(void)
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
nvme_unregister_io_thread(void)
{
	nvme_free_ioq_index();
}

