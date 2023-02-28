/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
	struct spdk_nvmf_transport *transport, struct spdk_nvmf_poll_group *group);
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

void nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
					struct spdk_nvmf_transport_poll_group *group,
					struct spdk_nvmf_transport *transport);

int nvmf_request_get_stripped_buffers(struct spdk_nvmf_request *req,
				      struct spdk_nvmf_transport_poll_group *group,
				      struct spdk_nvmf_transport *transport,
				      uint32_t length);

#endif /* SPDK_NVMF_TRANSPORT_H */
