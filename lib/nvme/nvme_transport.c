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

/*
 * NVMe transport abstraction
 */

#include "nvme_internal.h"
#include "spdk/queue.h"

TAILQ_HEAD(nvme_transport_list, nvme_transport) g_spdk_nvme_transports =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvme_transports);

/*
 * Unfortunately, due to NVMe PCIe multiprocess support, we cannot store the
 * transport object in either the controller struct or the admin qpair. THis means
 * that a lot of admin related transport calls will have to call nvme_get_transport
 * in order to knwo which functions to call.
 * In the I/O path, we have the ability to store the transport struct in the I/O
 * qpairs to avoid taking a performance hit.
 */
const struct nvme_transport *
nvme_get_transport(const char *transport_name)
{
	struct nvme_transport *registered_transport;

	TAILQ_FOREACH(registered_transport, &g_spdk_nvme_transports, link) {
		if (strcasecmp(transport_name, registered_transport->ops.name) == 0) {
			return registered_transport;
		}
	}

	return NULL;
}

bool
spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype)
{
	return nvme_get_transport(spdk_nvme_transport_id_trtype_str(trtype)) == NULL ? false : true;
}

bool
spdk_nvme_transport_available_by_name(const char *transport_name)
{
	return nvme_get_transport(transport_name) == NULL ? false : true;
}

void spdk_nvme_transport_register(const struct spdk_nvme_transport_ops *ops)
{
	struct nvme_transport *new_transport;

	if (nvme_get_transport(ops->name)) {
		SPDK_ERRLOG("Double registering NVMe transport %s is prohibited.\n", ops->name);
		assert(false);
	}

	new_transport = calloc(1, sizeof(*new_transport));
	if (new_transport == NULL) {
		SPDK_ERRLOG("Unable to allocate memory to register new NVMe transport.\n");
		assert(false);
		return;
	}

	new_transport->ops = *ops;
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, new_transport, link);
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	const struct nvme_transport *transport = nvme_get_transport(trid->trstring);
	struct spdk_nvme_ctrlr *ctrlr;

	if (transport == NULL) {
		SPDK_ERRLOG("Transport %s doesn't exist.", trid->trstring);
		return NULL;
	}

	ctrlr = transport->ops.ctrlr_construct(trid, opts, devhandle);

	return ctrlr;
}

int
nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
			  bool direct_connect)
{
	const struct nvme_transport *transport = nvme_get_transport(probe_ctx->trid.trstring);

	if (transport == NULL) {
		SPDK_ERRLOG("Transport %s doesn't exist.", probe_ctx->trid.trstring);
		return -ENOENT;
	}

	return transport->ops.ctrlr_scan(probe_ctx, direct_connect);
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_destruct(ctrlr);
}

int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_enable(ctrlr);
}

int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_set_reg_4(ctrlr, offset, value);
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_set_reg_8(ctrlr, offset, value);
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_reg_4(ctrlr, offset, value);
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_reg_8(ctrlr, offset, value);
}

uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_max_xfer_size(ctrlr);
}

uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_max_sges(ctrlr);
}

void *
nvme_transport_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_alloc_cmb_io_buffer(ctrlr, size);
}

int
nvme_transport_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_free_cmb_io_buffer(ctrlr, buf, size);
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_qpair *qpair;
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	qpair = transport->ops.ctrlr_create_io_qpair(ctrlr, qid, opts);
	if (qpair != NULL && !nvme_qpair_is_admin_queue(qpair)) {
		qpair->transport = transport;
	}

	return qpair;
}

int
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return qpair->transport->ops.ctrlr_delete_io_qpair(ctrlr, qpair);
}

int
nvme_transport_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (!nvme_qpair_is_admin_queue(qpair)) {
		qpair->transport = transport;
	}
	nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);
	return transport->ops.ctrlr_connect_qpair(ctrlr, qpair);
}

volatile struct spdk_nvme_registers *
nvme_transport_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_registers(ctrlr);
}

void
nvme_transport_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	const struct nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	transport->ops.ctrlr_disconnect_qpair(ctrlr, qpair);
}

void
nvme_transport_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	const struct nvme_transport *transport;

	assert(dnr <= 1);
	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		qpair->transport->ops.qpair_abort_reqs(qpair, dnr);
	} else {
		transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
		assert(transport != NULL);
		transport->ops.qpair_abort_reqs(qpair, dnr);
	}
}

int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	const struct nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		return qpair->transport->ops.qpair_reset(qpair);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_reset(qpair);
}

int
nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	const struct nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		return qpair->transport->ops.qpair_submit_request(qpair, req);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_submit_request(qpair, req);
}

int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	const struct nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		return qpair->transport->ops.qpair_process_completions(qpair, max_completions);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_process_completions(qpair, max_completions);
}

void
nvme_transport_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	const struct nvme_transport *transport = nvme_get_transport(qpair->ctrlr->trid.trstring);

	assert(transport != NULL);
	transport->ops.admin_qpair_abort_aers(qpair);
}
