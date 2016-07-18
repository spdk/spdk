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

#ifndef NVMF_SESSION_H
#define NVMF_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#include "request.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

/* define a virtual controller limit to the number of QPs supported */
#define MAX_SESSION_IO_QUEUES 64

struct spdk_nvmf_transport;

enum conn_type {
	CONN_TYPE_AQ = 0,
	CONN_TYPE_IOQ = 1,
};

struct spdk_nvmf_conn {
	const struct spdk_nvmf_transport	*transport;
	struct nvmf_session			*sess;
	enum conn_type				type;

	uint16_t				sq_head;

	TAILQ_ENTRY(spdk_nvmf_conn) 		link;
};

/*
 * This structure maintains the NVMf virtual controller session
 * state. Each NVMf session permits some number of connections.
 * At least one admin connection and additional IOQ connections.
 */
struct nvmf_session {
	struct spdk_nvmf_subsystem *subsys;

	struct {
		union spdk_nvme_cap_register	cap;
		union spdk_nvme_vs_register	vs;
		union spdk_nvme_cc_register	cc;
		union spdk_nvme_csts_register	csts;
	} vcprop; /* virtual controller properties */
	struct spdk_nvme_ctrlr_data	vcdata; /* virtual controller data */

	TAILQ_HEAD(connection_q, spdk_nvmf_conn) connections;
	int num_connections;
	int max_connections_allowed;

	struct spdk_nvmf_request *aer_req;
};

void spdk_nvmf_session_connect(struct spdk_nvmf_conn *conn,
			       struct spdk_nvmf_fabric_connect_cmd *cmd,
			       struct spdk_nvmf_fabric_connect_data *data,
			       struct spdk_nvmf_fabric_connect_rsp *rsp);

void
nvmf_disconnect(struct nvmf_session *session, struct spdk_nvmf_conn *conn);

void
nvmf_property_get(struct nvmf_session *session,
		  struct spdk_nvmf_fabric_prop_get_cmd *cmd,
		  struct spdk_nvmf_fabric_prop_get_rsp *response);

void
nvmf_property_set(struct nvmf_session *session,
		  struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		  struct spdk_nvme_cpl *rsp);

int spdk_nvmf_session_poll(struct nvmf_session *session);

void spdk_nvmf_session_destruct(struct nvmf_session *session);

#endif
