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

#ifndef NVMF_CONN_H
#define NVMF_CONN_H

#include <stdint.h>

#include "spdk/event.h"
#include "nvmf_internal.h"
#include "rdma.h"
#include "spdk/queue.h"

/* RDMA transport connection states */
enum conn_state {
	CONN_STATE_INVALID = 0,
	CONN_STATE_RUNNING = 1,
	CONN_STATE_FABRIC_DISCONNECT = 2,
	CONN_STATE_EXITING = 4,
};

enum conn_type {
	CONN_TYPE_AQ = 0,
	CONN_TYPE_IOQ = 1,
};

struct spdk_nvmf_conn {
	uint32_t			is_valid;

	struct nvmf_session		*sess;


	/*
	 * values saved from fabric connect and private data
	 */
	uint8_t				responder_resources;
	uint8_t				initiator_depth;
	uint16_t			sq_depth;
	uint16_t			cq_depth;
	uint16_t			qid;
	uint16_t			cntlid;

	enum conn_type			type;
	volatile enum conn_state	state;

	uint16_t			sq_head;

	struct spdk_nvmf_rdma_conn	rdma;

	TAILQ_ENTRY(spdk_nvmf_conn) 	link;
	struct spdk_poller		poller;
};

int spdk_initialize_nvmf_conns(int max_connections);

void spdk_shutdown_nvmf_conns(void);

struct spdk_nvmf_conn *
spdk_nvmf_allocate_conn(void);

void
nvmf_init_conn_properites(struct spdk_nvmf_conn *conn,
			  struct nvmf_session *session,
			  struct spdk_nvmf_fabric_connect_rsp *response);

int spdk_nvmf_startup_conn(struct spdk_nvmf_conn *conn);

void
nvmf_deactive_tx_desc(struct nvme_qp_tx_desc *tx_desc);

#endif /* NVMF_CONN_H */
