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

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/log.h"
#include "spdk/nvmf.h"
#include "spdk/queue.h"
#include "spdk/util.h"

static const struct spdk_nvmf_transport_ops *const g_transport_ops[] = {
#ifdef SPDK_CONFIG_RDMA
	&spdk_nvmf_transport_rdma,
#endif
};

#define NUM_TRANSPORTS (SPDK_COUNTOF(g_transport_ops))

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(struct spdk_nvmf_tgt *tgt,
			   enum spdk_nvme_transport_type type)
{
	size_t i;
	const struct spdk_nvmf_transport_ops *ops = NULL;
	struct spdk_nvmf_transport *transport;

	for (i = 0; i != NUM_TRANSPORTS; i++) {
		if (g_transport_ops[i]->type == type) {
			ops = g_transport_ops[i];
			break;
		}
	}

	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n",
			    spdk_nvme_transport_id_trtype_str(type));
		return NULL;
	}

	transport = ops->create(tgt);
	if (!transport) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n",
			    spdk_nvme_transport_id_trtype_str(type));
		return NULL;
	}

	transport->ops = ops;
	transport->tgt = tgt;

	return transport;
}

int
spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport)
{
	return transport->ops->destroy(transport);
}

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	return transport->ops->listen(transport, trid);
}

int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid)
{
	return transport->ops->stop_listen(transport, trid);
}

void
spdk_nvmf_transport_accept(struct spdk_nvmf_transport *transport)
{
	transport->ops->accept(transport);
}

void
spdk_nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				      struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
}

struct spdk_nvmf_transport_poll_group *
spdk_nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *group;

	group = transport->ops->poll_group_create(transport);
	group->transport = transport;

	return group;
}

void
spdk_nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	group->transport->ops->poll_group_destroy(group);
}

int
spdk_nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
				   struct spdk_nvmf_qpair *qpair)
{
	if (qpair->transport) {
		assert(qpair->transport == group->transport);
		if (qpair->transport != group->transport) {
			return -1;
		}
	} else {
		qpair->transport = group->transport;
	}

	return group->transport->ops->poll_group_add(group, qpair);
}

int
spdk_nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				      struct spdk_nvmf_qpair *qpair)
{
	return group->transport->ops->poll_group_remove(group, qpair);
}

int
spdk_nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	return group->transport->ops->poll_group_poll(group);
}

int
spdk_nvmf_transport_req_complete(struct spdk_nvmf_request *req)
{
	return req->qpair->transport->ops->req_complete(req);
}

void
spdk_nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair)
{
	qpair->transport->ops->qpair_fini(qpair);
}

bool
spdk_nvmf_transport_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	return qpair->transport->ops->qpair_is_idle(qpair);
}
