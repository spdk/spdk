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

#include "spdk/nvmf_spec.h"
#include "nvme_internal.h"

#include <uuid/uuid.h>

#define SPDK_NVME_DRIVER_NAME "spdk_nvme_driver"

struct nvme_driver	*g_spdk_nvme_driver;

int32_t			spdk_nvme_retry_count;

/* gross timeout of 180 seconds in milliseconds */
static int g_nvme_driver_timeout_ms = 3 * 60 * 1000;

static TAILQ_HEAD(, spdk_nvme_ctrlr) g_nvme_init_ctrlrs =
	TAILQ_HEAD_INITIALIZER(g_nvme_init_ctrlrs);

/* Per-process attached controller list */
static TAILQ_HEAD(, spdk_nvme_ctrlr) g_nvme_attached_ctrlrs =
	TAILQ_HEAD_INITIALIZER(g_nvme_attached_ctrlrs);

/* Returns true if ctrlr should be stored on the multi-process shared_attached_ctrlrs list */
static bool
nvme_ctrlr_shared(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE;
}

/* Caller must hold g_spdk_nvme_driver->lock */
void
nvme_ctrlr_connected(struct spdk_nvme_ctrlr *ctrlr)
{
	TAILQ_INSERT_TAIL(&g_nvme_init_ctrlrs, ctrlr, tailq);
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	nvme_ctrlr_proc_put_ref(ctrlr);

	if (nvme_ctrlr_get_ref_count(ctrlr) == 0) {
		if (nvme_ctrlr_shared(ctrlr)) {
			TAILQ_REMOVE(&g_spdk_nvme_driver->shared_attached_ctrlrs, ctrlr, tailq);
		} else {
			TAILQ_REMOVE(&g_nvme_attached_ctrlrs, ctrlr, tailq);
		}
		nvme_ctrlr_destruct(ctrlr);
	}

	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
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
nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		      const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	req = STAILQ_FIRST(&qpair->free_req);
	if (req == NULL) {
		return req;
	}

	STAILQ_REMOVE_HEAD(&qpair->free_req, stailq);

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
	req->qpair = qpair;
	req->pid = getpid();

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(struct spdk_nvme_qpair *qpair,
			     void *buffer, uint32_t payload_size,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = NULL;

	return nvme_allocate_request(qpair, &payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(qpair, NULL, 0, cb_fn, cb_arg);
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
			assert(req->pid == getpid());
			memcpy(req->user_buffer, req->payload.u.contig, req->payload_size);
		}

		spdk_dma_free(req->payload.u.contig);
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
nvme_allocate_request_user_copy(struct spdk_nvme_qpair *qpair,
				void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				void *cb_arg, bool host_to_controller)
{
	struct nvme_request *req;
	void *contig_buffer = NULL;
	uint64_t phys_addr;

	if (buffer && payload_size) {
		contig_buffer = spdk_dma_zmalloc(payload_size, 4096, &phys_addr);
		if (!contig_buffer) {
			return NULL;
		}

		if (host_to_controller) {
			memcpy(contig_buffer, buffer, payload_size);
		}
	}

	req = nvme_allocate_request_contig(qpair, contig_buffer, payload_size, nvme_user_copy_cmd_complete,
					   NULL);
	if (!req) {
		spdk_dma_free(contig_buffer);
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
	assert(req->qpair != NULL);

	STAILQ_INSERT_HEAD(&req->qpair->free_req, req, stailq);
}

int
nvme_robust_mutex_init_shared(pthread_mutex_t *mtx)
{
	int rc = 0;

#ifdef __FreeBSD__
	pthread_mutex_init(mtx, NULL);
#else
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ||
	    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
#endif

	return rc;
}

static int
nvme_driver_init(void)
{
	uuid_t host_id;
	int ret = 0;
	/* Any socket ID */
	int socket_id = -1;

	/*
	 * Only one thread from one process will do this driver init work.
	 * The primary process will reserve the shared memory and do the
	 *  initialization.
	 * The secondary process will lookup the existing reserved memory.
	 */
	if (spdk_process_is_primary()) {
		/* The unique named memzone already reserved. */
		if (g_spdk_nvme_driver != NULL) {
			assert(g_spdk_nvme_driver->initialized == true);

			return 0;
		} else {
			g_spdk_nvme_driver = spdk_memzone_reserve(SPDK_NVME_DRIVER_NAME,
					     sizeof(struct nvme_driver), socket_id, 0);
		}

		if (g_spdk_nvme_driver == NULL) {
			SPDK_ERRLOG("primary process failed to reserve memory\n");

			return -1;
		}
	} else {
		g_spdk_nvme_driver = spdk_memzone_lookup(SPDK_NVME_DRIVER_NAME);

		/* The unique named memzone already reserved by the primary process. */
		if (g_spdk_nvme_driver != NULL) {
			int ms_waited = 0;

			/* Wait the nvme driver to get initialized. */
			while ((g_spdk_nvme_driver->initialized == false) &&
			       (ms_waited < g_nvme_driver_timeout_ms)) {
				ms_waited++;
				nvme_delay(1000); /* delay 1ms */
			}
			if (g_spdk_nvme_driver->initialized == false) {
				SPDK_ERRLOG("timeout waiting for primary process to init\n");

				return -1;
			}
		} else {
			SPDK_ERRLOG("primary process is not started yet\n");

			return -1;
		}

		return 0;
	}

	/*
	 * At this moment, only one thread from the primary process will do
	 * the g_spdk_nvme_driver initialization
	 */
	assert(spdk_process_is_primary());

	ret = nvme_robust_mutex_init_shared(&g_spdk_nvme_driver->lock);
	if (ret != 0) {
		SPDK_ERRLOG("failed to initialize mutex\n");
		spdk_memzone_free(SPDK_NVME_DRIVER_NAME);
		return ret;
	}

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	g_spdk_nvme_driver->initialized = false;

	TAILQ_INIT(&g_spdk_nvme_driver->shared_attached_ctrlrs);

	SPDK_STATIC_ASSERT(sizeof(host_id) == sizeof(g_spdk_nvme_driver->default_extended_host_id),
			   "host ID size mismatch");
	uuid_generate(host_id);
	memcpy(g_spdk_nvme_driver->default_extended_host_id, host_id, sizeof(host_id));

	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	return ret;
}

int
nvme_ctrlr_probe(const struct spdk_nvme_transport_id *trid, void *devhandle,
		 spdk_nvme_probe_cb probe_cb, void *cb_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ctrlr_opts opts;

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));

	if (!probe_cb || probe_cb(cb_ctx, trid, &opts)) {
		ctrlr = nvme_transport_ctrlr_construct(trid, &opts, devhandle);
		if (ctrlr == NULL) {
			if (trid != NULL) {
				SPDK_ERRLOG("Failed to construct NVMe controller for SSD: %s\n", trid->traddr);
			} else {
				SPDK_ERRLOG("Failed to construct NVMe controller\n");
			}
			return -1;
		}

		TAILQ_INSERT_TAIL(&g_nvme_init_ctrlrs, ctrlr, tailq);
		return 0;
	}

	return 1;
}

static int
nvme_init_controllers(void *cb_ctx, spdk_nvme_attach_cb attach_cb)
{
	int rc = 0;
	int start_rc;
	struct spdk_nvme_ctrlr *ctrlr, *ctrlr_tmp;

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	/* Initialize all new controllers in the g_nvme_init_ctrlrs list in parallel. */
	while (!TAILQ_EMPTY(&g_nvme_init_ctrlrs)) {
		TAILQ_FOREACH_SAFE(ctrlr, &g_nvme_init_ctrlrs, tailq, ctrlr_tmp) {
			/* Drop the driver lock while calling nvme_ctrlr_process_init()
			 *  since it needs to acquire the driver lock internally when calling
			 *  nvme_ctrlr_start().
			 *
			 * TODO: Rethink the locking - maybe reset should take the lock so that start() and
			 *  the functions it calls (in particular nvme_ctrlr_set_num_qpairs())
			 *  can assume it is held.
			 */
			nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
			start_rc = nvme_ctrlr_process_init(ctrlr);
			nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

			if (start_rc) {
				/* Controller failed to initialize. */
				TAILQ_REMOVE(&g_nvme_init_ctrlrs, ctrlr, tailq);
				SPDK_ERRLOG("Failed to initialize SSD: %s\n", ctrlr->trid.traddr);
				nvme_ctrlr_destruct(ctrlr);
				rc = -1;
				break;
			}

			if (ctrlr->state == NVME_CTRLR_STATE_READY) {
				/*
				 * Controller has been initialized.
				 *  Move it to the attached_ctrlrs list.
				 */
				TAILQ_REMOVE(&g_nvme_init_ctrlrs, ctrlr, tailq);
				if (nvme_ctrlr_shared(ctrlr)) {
					TAILQ_INSERT_TAIL(&g_spdk_nvme_driver->shared_attached_ctrlrs, ctrlr, tailq);
				} else {
					TAILQ_INSERT_TAIL(&g_nvme_attached_ctrlrs, ctrlr, tailq);
				}

				/*
				 * Increase the ref count before calling attach_cb() as the user may
				 * call nvme_detach() immediately.
				 */
				nvme_ctrlr_proc_get_ref(ctrlr);

				/*
				 * Unlock while calling attach_cb() so the user can call other functions
				 *  that may take the driver lock, like nvme_detach().
				 */
				if (attach_cb) {
					nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
					attach_cb(cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
					nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
				}

				break;
			}
		}
	}

	g_spdk_nvme_driver->initialized = true;

	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
	return rc;
}

/* This function must not be called while holding g_spdk_nvme_driver->lock */
static struct spdk_nvme_ctrlr *
spdk_nvme_get_ctrlr_by_trid(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_ctrlr *ctrlr;

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
	ctrlr = spdk_nvme_get_ctrlr_by_trid_unsafe(trid);
	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

	return ctrlr;
}

/* This function must be called while holding g_spdk_nvme_driver->lock */
struct spdk_nvme_ctrlr *
spdk_nvme_get_ctrlr_by_trid_unsafe(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_ctrlr *ctrlr;

	/* Search per-process list */
	TAILQ_FOREACH(ctrlr, &g_nvme_attached_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, trid) == 0) {
			return ctrlr;
		}
	}

	/* Search multi-process shared list */
	TAILQ_FOREACH(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, trid) == 0) {
			return ctrlr;
		}
	}

	return NULL;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
static int
spdk_nvme_probe_internal(const struct spdk_nvme_transport_id *trid, void *cb_ctx,
			 spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
			 spdk_nvme_remove_cb remove_cb, struct spdk_nvme_ctrlr **connected_ctrlr)
{
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	bool direct_connect = (connected_ctrlr != NULL);

	if (!spdk_nvme_transport_available(trid->trtype)) {
		SPDK_ERRLOG("NVMe trtype %u not available\n", trid->trtype);
		return -1;
	}

	nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);

	nvme_transport_ctrlr_scan(trid, cb_ctx, probe_cb, remove_cb, direct_connect);

	/*
	 * Probe controllers on the shared_attached_ctrlrs list
	 */
	if (!spdk_process_is_primary() && (trid->trtype == SPDK_NVME_TRANSPORT_PCIE)) {
		TAILQ_FOREACH(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq) {
			/* Do not attach other ctrlrs if user specify a valid trid */
			if ((strlen(trid->traddr) != 0) &&
			    (spdk_nvme_transport_id_compare(trid, &ctrlr->trid))) {
				continue;
			}

			nvme_ctrlr_proc_get_ref(ctrlr);

			/*
			 * Unlock while calling attach_cb() so the user can call other functions
			 *  that may take the driver lock, like nvme_detach().
			 */
			if (attach_cb) {
				nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
				attach_cb(cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
			}
		}

		nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);

		rc = 0;

		goto exit;
	}

	nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
	/*
	 * Keep going even if one or more nvme_attach() calls failed,
	 *  but maintain the value of rc to signal errors when we return.
	 */

	rc = nvme_init_controllers(cb_ctx, attach_cb);

exit:
	if (connected_ctrlr) {
		*connected_ctrlr = spdk_nvme_get_ctrlr_by_trid(trid);
	}

	return rc;
}

int
spdk_nvme_probe(const struct spdk_nvme_transport_id *trid, void *cb_ctx,
		spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb)
{
	int rc;
	struct spdk_nvme_transport_id trid_pcie;

	rc = nvme_driver_init();
	if (rc != 0) {
		return rc;
	}

	if (trid == NULL) {
		memset(&trid_pcie, 0, sizeof(trid_pcie));
		trid_pcie.trtype = SPDK_NVME_TRANSPORT_PCIE;
		trid = &trid_pcie;
	}

	return spdk_nvme_probe_internal(trid, cb_ctx, probe_cb, attach_cb, remove_cb, NULL);
}

static bool
spdk_nvme_connect_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			   struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_connect_opts *requested_opts = cb_ctx;

	assert(requested_opts->opts);

	assert(requested_opts->opts_size != 0);

	memcpy(opts, requested_opts->opts, spdk_min(sizeof(*opts), requested_opts->opts_size));

	return true;
}

struct spdk_nvme_ctrlr *
spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		  const struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	int rc;
	struct spdk_nvme_ctrlr_connect_opts connect_opts = {};
	struct spdk_nvme_ctrlr_connect_opts *user_connect_opts = NULL;
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	spdk_nvme_probe_cb probe_cb = NULL;

	if (trid == NULL) {
		SPDK_ERRLOG("No transport ID specified\n");
		return NULL;
	}

	rc = nvme_driver_init();
	if (rc != 0) {
		return NULL;
	}

	if (opts && opts_size > 0) {
		connect_opts.opts = opts;
		connect_opts.opts_size = opts_size;
		user_connect_opts = &connect_opts;
		probe_cb = spdk_nvme_connect_probe_cb;
	}

	spdk_nvme_probe_internal(trid, user_connect_opts, probe_cb, NULL, NULL, &ctrlr);

	return ctrlr;
}

int
spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str)
{
	if (trtype == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "PCIe") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_PCIE;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_RDMA;
	} else {
		return -ENOENT;
	}
	return 0;
}

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_parse_adrfam(enum spdk_nvmf_adrfam *adrfam, const char *str)
{
	if (adrfam == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "IPv4") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (strcasecmp(str, "IPv6") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else if (strcasecmp(str, "IB") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_IB;
	} else if (strcasecmp(str, "FC") == 0) {
		*adrfam = SPDK_NVMF_ADRFAM_FC;
	} else {
		return -ENOENT;
	}
	return 0;
}

const char *
spdk_nvme_transport_id_adrfam_str(enum spdk_nvmf_adrfam adrfam)
{
	switch (adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		return "IPv4";
	case SPDK_NVMF_ADRFAM_IPV6:
		return "IPv6";
	case SPDK_NVMF_ADRFAM_IB:
		return "IB";
	case SPDK_NVMF_ADRFAM_FC:
		return "FC";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_parse(struct spdk_nvme_transport_id *trid, const char *str)
{
	const char *sep;
	const char *whitespace = " \t\n";
	size_t key_len, val_len;
	char key[32];
	char val[1024];

	if (trid == NULL || str == NULL) {
		return -EINVAL;
	}

	while (*str != '\0') {
		str += strspn(str, whitespace);

		sep = strchr(str, ':');
		if (!sep) {
			sep = strchr(str, '=');
			if (!sep) {
				SPDK_ERRLOG("Key without ':' or '=' separator\n");
				return -EINVAL;
			}
		}

		key_len = sep - str;
		if (key_len >= sizeof(key)) {
			SPDK_ERRLOG("Transport key length %zu greater than maximum allowed %zu\n",
				    key_len, sizeof(key) - 1);
			return -EINVAL;
		}

		memcpy(key, str, key_len);
		key[key_len] = '\0';

		str += key_len + 1; /* Skip key: */
		val_len = strcspn(str, whitespace);
		if (val_len == 0) {
			SPDK_ERRLOG("Key without value\n");
			return -EINVAL;
		}

		if (val_len >= sizeof(val)) {
			SPDK_ERRLOG("Transport value length %zu greater than maximum allowed %zu\n",
				    val_len, sizeof(val) - 1);
			return -EINVAL;
		}

		memcpy(val, str, val_len);
		val[val_len] = '\0';

		str += val_len;

		if (strcasecmp(key, "trtype") == 0) {
			if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, val) != 0) {
				SPDK_ERRLOG("Unknown trtype '%s'\n", val);
				return -EINVAL;
			}
		} else if (strcasecmp(key, "adrfam") == 0) {
			if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, val) != 0) {
				SPDK_ERRLOG("Unknown adrfam '%s'\n", val);
				return -EINVAL;
			}
		} else if (strcasecmp(key, "traddr") == 0) {
			if (val_len > SPDK_NVMF_TRADDR_MAX_LEN) {
				SPDK_ERRLOG("traddr length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRADDR_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->traddr, val, val_len + 1);
		} else if (strcasecmp(key, "trsvcid") == 0) {
			if (val_len > SPDK_NVMF_TRSVCID_MAX_LEN) {
				SPDK_ERRLOG("trsvcid length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_TRSVCID_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->trsvcid, val, val_len + 1);
		} else if (strcasecmp(key, "subnqn") == 0) {
			if (val_len > SPDK_NVMF_NQN_MAX_LEN) {
				SPDK_ERRLOG("subnqn length %zu greater than maximum allowed %u\n",
					    val_len, SPDK_NVMF_NQN_MAX_LEN);
				return -EINVAL;
			}
			memcpy(trid->subnqn, val, val_len + 1);
		} else {
			SPDK_ERRLOG("Unknown transport ID key '%s'\n", key);
		}
	}

	return 0;
}

static int
cmp_int(int a, int b)
{
	return a - b;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	int cmp;

	cmp = cmp_int(trid1->trtype, trid2->trtype);
	if (cmp) {
		return cmp;
	}

	if (trid1->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		struct spdk_pci_addr pci_addr1;
		struct spdk_pci_addr pci_addr2;

		/* Normalize PCI addresses before comparing */
		if (spdk_pci_addr_parse(&pci_addr1, trid1->traddr) < 0 ||
		    spdk_pci_addr_parse(&pci_addr2, trid2->traddr) < 0) {
			return -1;
		}

		/* PCIe transport ID only uses trtype and traddr */
		return spdk_pci_addr_compare(&pci_addr1, &pci_addr2);
	}

	cmp = strcasecmp(trid1->traddr, trid2->traddr);
	if (cmp) {
		return cmp;
	}

	cmp = cmp_int(trid1->adrfam, trid2->adrfam);
	if (cmp) {
		return cmp;
	}

	cmp = strcasecmp(trid1->trsvcid, trid2->trsvcid);
	if (cmp) {
		return cmp;
	}

	cmp = strcmp(trid1->subnqn, trid2->subnqn);
	if (cmp) {
		return cmp;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)
