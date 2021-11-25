/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#ifndef SPDK_NVMF_TRANSPORT_H
#define SPDK_NVMF_TRANSPORT_H

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_transport.h"

void nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				      struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_discovery_log_page_entry *entry);

struct spdk_nvmf_transport_poll_group *nvmf_transport_poll_group_create(
	struct spdk_nvmf_transport *transport);
struct spdk_nvmf_transport_poll_group *nvmf_transport_get_optimal_poll_group(
	struct spdk_nvmf_transport *transport, struct spdk_nvmf_qpair *qpair);

void nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

int nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_qpair *qpair);

int nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				     struct spdk_nvmf_qpair *qpair);

int nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

int nvmf_transport_req_free(struct spdk_nvmf_request *req);

int nvmf_transport_req_complete(struct spdk_nvmf_request *req);

void nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair,
			       spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg);

int nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				       struct spdk_nvme_transport_id *trid);

int nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
					struct spdk_nvme_transport_id *trid);

int nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid);

void nvmf_transport_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
					struct spdk_nvmf_request *req);

#endif /* SPDK_NVMF_TRANSPORT_H */
