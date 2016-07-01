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

#ifndef _NVMF_RDMA_H_
#define _NVMF_RDMA_H_

#include <infiniband/verbs.h>

#include "nvmf_internal.h"
#include "request.h"
#include "spdk/nvmf_spec.h"
#include "spdk/queue.h"

struct spdk_nvmf_rdma_conn {
	struct rdma_cm_id		*cm_id;
	struct ibv_context		*ctx;
	struct ibv_comp_channel		*comp_channel;
	struct ibv_cq			*cq;
	struct ibv_qp			*qp;

	uint16_t			queue_depth;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)		rdma_reqs;
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	STAILQ_ENTRY(spdk_nvmf_rdma_request)	link;

	union nvmf_h2c_msg			cmd;
	struct ibv_mr				*cmd_mr;

	union nvmf_c2h_msg			rsp;
	struct ibv_mr				*rsp_mr;

	struct ibv_sge				send_sgl;
	struct ibv_sge				recv_sgl[2];

	struct ibv_mr				*bb_mr;
	uint8_t					*bb;
	uint32_t				bb_len;
};

int spdk_nvmf_rdma_init(void);
int spdk_nvmf_rdma_fini(void);

int nvmf_post_rdma_read(struct spdk_nvmf_conn *conn,
			struct spdk_nvmf_request *req);
int spdk_nvmf_rdma_request_complete(struct spdk_nvmf_conn *conn,
				    struct spdk_nvmf_request *req);
int spdk_nvmf_rdma_request_release(struct spdk_nvmf_conn *conn,
				   struct spdk_nvmf_request *req);

int spdk_nvmf_rdma_alloc_reqs(struct spdk_nvmf_conn *conn);
void spdk_nvmf_rdma_free_reqs(struct spdk_nvmf_conn *conn);
void spdk_nvmf_rdma_free_req(struct spdk_nvmf_request *req);
void nvmf_rdma_conn_cleanup(struct spdk_nvmf_conn *conn);

int nvmf_acceptor_start(void);
void nvmf_acceptor_stop(void);

int nvmf_check_rdma_completions(struct spdk_nvmf_conn *conn);

#endif /* _NVMF_RDMA_H_ */
