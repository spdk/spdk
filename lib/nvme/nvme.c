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

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.init_ctrlrs = TAILQ_HEAD_INITIALIZER(_g_nvme_driver.init_ctrlrs),
	.attached_ctrlrs = TAILQ_HEAD_INITIALIZER(_g_nvme_driver.attached_ctrlrs),
	.request_mempool = NULL,
};

struct nvme_driver *g_spdk_nvme_driver = &_g_nvme_driver;

int32_t		spdk_nvme_retry_count;

static struct spdk_nvme_ctrlr *
nvme_attach(void *devhandle)
{
	const struct spdk_nvme_transport *transport;
	struct spdk_nvme_ctrlr	*ctrlr;
	int			status;
	uint64_t		phys_addr = 0;

	transport = &spdk_nvme_transport_pcie;

	ctrlr = spdk_zmalloc(transport->ctrlr_size, 64, &phys_addr);
	if (ctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	ctrlr->transport = transport;

	status = nvme_ctrlr_construct(ctrlr, devhandle);
	if (status != 0) {
		spdk_free(ctrlr);
		return NULL;
	}

	return ctrlr;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	pthread_mutex_lock(&g_spdk_nvme_driver->lock);

	nvme_ctrlr_destruct(ctrlr);
	TAILQ_REMOVE(&g_spdk_nvme_driver->attached_ctrlrs, ctrlr, tailq);
	spdk_free(ctrlr);

	pthread_mutex_unlock(&g_spdk_nvme_driver->lock);
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

struct nvme_request *
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req = NULL;

	req = spdk_mempool_get(g_spdk_nvme_driver->request_mempool);
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
	payload.md = NULL;

	return nvme_allocate_request(&payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(NULL, 0, cb_fn, cb_arg);
}

static void
nvme_user_copy_cmd_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *req = arg;
	enum spdk_nvme_data_transfer xfer;

	if (req->user_buffer && req->payload_size) {
		/* Copy back to the user buffer and free the contig buffer */
		assert(req->payload.type == NVME_PAYLOAD_TYPE_CONTIG);
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
		if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
		    xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
			memcpy(req->user_buffer, req->payload.u.contig, req->payload_size);
		}

		spdk_free(req->payload.u.contig);
	}

	/* Call the user's original callback now that the buffer has been copied */
	req->user_cb_fn(req->user_cb_arg, cpl);
}

/**
 * Allocate a request as well as a physically contiguous buffer to copy to/from the user's buffer.
 *
 * This is intended for use in non-fast-path functions (admin commands, reservations, etc.)
 * where the overhead of a copy is not a problem.
 */
struct nvme_request *
nvme_allocate_request_user_copy(void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				void *cb_arg, bool host_to_controller)
{
	struct nvme_request *req;
	void *contig_buffer = NULL;
	uint64_t phys_addr;

	if (buffer && payload_size) {
		contig_buffer = spdk_zmalloc(payload_size, 4096, &phys_addr);
		if (!contig_buffer) {
			return NULL;
		}

		if (host_to_controller) {
			memcpy(contig_buffer, buffer, payload_size);
		}
	}

	req = nvme_allocate_request_contig(contig_buffer, payload_size, nvme_user_copy_cmd_complete, NULL);
	if (!req) {
		spdk_free(buffer);
		return NULL;
	}

	req->user_cb_fn = cb_fn;
	req->user_cb_arg = cb_arg;
	req->user_buffer = buffer;
	req->cb_arg = req;

	return req;
}

void
nvme_free_request(struct nvme_request *req)
{
	assert(req != NULL);
	assert(req->num_children == 0);

	spdk_mempool_put(g_spdk_nvme_driver->request_mempool, req);
}

int
nvme_mutex_init_shared(pthread_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ||
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
	return rc;
}

struct nvme_enum_ctx {
	spdk_nvme_probe_cb probe_cb;
	void *cb_ctx;
};

/* This function must only be called while holding g_spdk_nvme_driver->lock */
static int
nvme_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct nvme_enum_ctx *enum_ctx = ctx;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ctrlr_opts opts;

	/* Verify that this controller is not already attached */
	TAILQ_FOREACH(ctrlr, &g_spdk_nvme_driver->attached_ctrlrs, tailq) {
		/* NOTE: In the case like multi-process environment where the device handle is
		 * different per each process, we compare by BDF to determine whether it is the
		 * same controller.
		 */
		if (spdk_pci_device_compare_addr(pci_dev, &ctrlr->pci_addr)) {
			return 0;
		}
	}

	spdk_nvme_ctrlr_opts_set_defaults(&opts);

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev, &opts)) {
		ctrlr = nvme_attach(pci_dev);
		if (ctrlr == NULL) {
			SPDK_ERRLOG("nvme_attach() failed\n");
			return -1;
		}

		ctrlr->opts = opts;

		TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->init_ctrlrs, ctrlr, tailq);
	}

	return 0;
}

int
spdk_nvme_probe(void *cb_ctx, spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb)
{
	int rc, start_rc;
	struct nvme_enum_ctx enum_ctx;
	struct spdk_nvme_ctrlr *ctrlr, *ctrlr_tmp;

	pthread_mutex_lock(&g_spdk_nvme_driver->lock);

	if (g_spdk_nvme_driver->request_mempool == NULL) {
		g_spdk_nvme_driver->request_mempool = spdk_mempool_create("nvme_request", 8192,
						      sizeof(struct nvme_request), -1);
		if (g_spdk_nvme_driver->request_mempool == NULL) {
			SPDK_ERRLOG("Unable to allocate pool of requests\n");
			pthread_mutex_unlock(&g_spdk_nvme_driver->lock);
			return -1;
		}
	}

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.cb_ctx = cb_ctx;

	rc = spdk_pci_enumerate(SPDK_PCI_DEVICE_NVME, nvme_enum_cb, &enum_ctx);
	/*
	 * Keep going even if one or more nvme_attach() calls failed,
	 *  but maintain the value of rc to signal errors when we return.
	 */

	/* Initialize all new controllers in the init_ctrlrs list in parallel. */
	while (!TAILQ_EMPTY(&g_spdk_nvme_driver->init_ctrlrs)) {
		TAILQ_FOREACH_SAFE(ctrlr, &g_spdk_nvme_driver->init_ctrlrs, tailq, ctrlr_tmp) {
			/* Drop the driver lock while calling nvme_ctrlr_process_init()
			 *  since it needs to acquire the driver lock internally when calling
			 *  nvme_ctrlr_start().
			 *
			 * TODO: Rethink the locking - maybe reset should take the lock so that start() and
			 *  the functions it calls (in particular nvme_ctrlr_set_num_qpairs())
			 *  can assume it is held.
			 */
			pthread_mutex_unlock(&g_spdk_nvme_driver->lock);
			start_rc = nvme_ctrlr_process_init(ctrlr);
			pthread_mutex_lock(&g_spdk_nvme_driver->lock);

			if (start_rc) {
				/* Controller failed to initialize. */
				TAILQ_REMOVE(&g_spdk_nvme_driver->init_ctrlrs, ctrlr, tailq);
				nvme_ctrlr_destruct(ctrlr);
				spdk_free(ctrlr);
				rc = -1;
				break;
			}

			if (ctrlr->state == NVME_CTRLR_STATE_READY) {
				/*
				 * Controller has been initialized.
				 *  Move it to the attached_ctrlrs list.
				 */
				TAILQ_REMOVE(&g_spdk_nvme_driver->init_ctrlrs, ctrlr, tailq);
				TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->attached_ctrlrs, ctrlr, tailq);

				/*
				 * Unlock while calling attach_cb() so the user can call other functions
				 *  that may take the driver lock, like nvme_detach().
				 */
				pthread_mutex_unlock(&g_spdk_nvme_driver->lock);
				attach_cb(cb_ctx, ctrlr->devhandle, ctrlr, &ctrlr->opts);
				pthread_mutex_lock(&g_spdk_nvme_driver->lock);

				break;
			}
		}
	}

	pthread_mutex_unlock(&g_spdk_nvme_driver->lock);
	return rc;
}

SPDK_LOG_REGISTER_TRACE_FLAG("nvme", SPDK_TRACE_NVME)
