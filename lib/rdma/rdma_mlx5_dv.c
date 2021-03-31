/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
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

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma.h"
#include "spdk/log.h"
#include "spdk/util.h"

struct spdk_rdma_mlx5_dv_qp {
	struct spdk_rdma_qp common;
	struct ibv_qp_ex *qpex;
};

static int
rdma_mlx5_dv_init_qpair(struct spdk_rdma_mlx5_dv_qp *mlx5_qp)
{
	struct ibv_qp_attr qp_attr;
	int qp_attr_mask, rc;

	qp_attr.qp_state = IBV_QPS_INIT;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTS;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTS) failed, rc %d\n", rc);
	}

	return rc;
}

struct spdk_rdma_qp *
spdk_rdma_qp_create(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	assert(cm_id);
	assert(qp_attr);

	struct ibv_qp *qp;
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.qp_context = qp_attr->qp_context,
		.send_cq = qp_attr->send_cq,
		.recv_cq = qp_attr->recv_cq,
		.srq = qp_attr->srq,
		.cap = qp_attr->cap,
		.qp_type = IBV_QPT_RC,
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		.pd = qp_attr->pd ? qp_attr->pd : cm_id->pd
	};

	assert(dv_qp_attr.pd);

	mlx5_qp = calloc(1, sizeof(*mlx5_qp));
	if (!mlx5_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		mlx5_qp->common.stats = qp_attr->stats;
		mlx5_qp->common.shared_stats = true;
	} else {
		mlx5_qp->common.stats = calloc(1, sizeof(*mlx5_qp->common.stats));
		if (!mlx5_qp->common.stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(mlx5_qp);
			return NULL;
		}
	}

	qp = mlx5dv_create_qp(cm_id->verbs, &dv_qp_attr, NULL);

	if (!qp) {
		SPDK_ERRLOG("Failed to create qpair, errno %s (%d)\n", spdk_strerror(errno), errno);
		free(mlx5_qp);
		return NULL;
	}

	mlx5_qp->common.qp = qp;
	mlx5_qp->common.cm_id = cm_id;
	mlx5_qp->qpex = ibv_qp_to_qp_ex(qp);

	if (!mlx5_qp->qpex) {
		spdk_rdma_qp_destroy(&mlx5_qp->common);
		return NULL;
	}

	qp_attr->cap = dv_qp_attr.cap;

	return &mlx5_qp->common;
}

int
spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;

	assert(spdk_rdma_qp != NULL);
	assert(spdk_rdma_qp->cm_id != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	/* NVMEoF target must move qpair to RTS state */
	if (rdma_mlx5_dv_init_qpair(mlx5_qp) != 0) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		errno = ECONNABORTED;
		return -1;
	}

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
}

int
spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	rc = rdma_mlx5_dv_init_qpair(mlx5_qp);
	if (rc) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		return rc;
	}

	rc = rdma_establish(mlx5_qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}

	return rc;
}

void
spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(spdk_rdma_qp != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (!mlx5_qp->common.shared_stats) {
		free(mlx5_qp->common.stats);
	}

	if (mlx5_qp->common.qp) {
		rc = ibv_destroy_qp(mlx5_qp->common.qp);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy ibv qp %p, rc %d\n", mlx5_qp->common.qp, rc);
		}
	}

	free(mlx5_qp);
}

int
spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp)
{
	int rc = 0;

	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->qp) {
		struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};

		rc = ibv_modify_qp(spdk_rdma_qp->qp, &qp_attr, IBV_QP_STATE);
		if (rc) {
			SPDK_ERRLOG("Failed to modify ibv qp %p state to ERR, rc %d\n", spdk_rdma_qp->qp, rc);
			return rc;
		}
	}

	if (spdk_rdma_qp->cm_id) {
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

bool
spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first)
{
	struct ibv_send_wr *tmp;
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	bool is_first;

	assert(spdk_rdma_qp);
	assert(first);

	is_first = spdk_rdma_qp->send_wrs.first == NULL;
	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (is_first) {
		ibv_wr_start(mlx5_qp->qpex);
		spdk_rdma_qp->send_wrs.first = first;
	} else {
		spdk_rdma_qp->send_wrs.last->next = first;
	}

	for (tmp = first; tmp != NULL; tmp = tmp->next) {
		mlx5_qp->qpex->wr_id = tmp->wr_id;
		mlx5_qp->qpex->wr_flags = tmp->send_flags;

		switch (tmp->opcode) {
		case IBV_WR_SEND:
			ibv_wr_send(mlx5_qp->qpex);
			break;
		case IBV_WR_SEND_WITH_INV:
			ibv_wr_send_inv(mlx5_qp->qpex, tmp->invalidate_rkey);
			break;
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		default:
			SPDK_ERRLOG("Unexpected opcode %d\n", tmp->opcode);
			assert(0);
		}

		ibv_wr_set_sge_list(mlx5_qp->qpex, tmp->num_sge, tmp->sg_list);

		spdk_rdma_qp->send_wrs.last = tmp;
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
	}

	return is_first;
}

int
spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(bad_wr);
	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_unlikely(spdk_rdma_qp->send_wrs.first == NULL)) {
		return 0;
	}

	rc = ibv_wr_complete(mlx5_qp->qpex);

	if (spdk_unlikely(rc)) {
		/* If ibv_wr_complete reports an error that means that no WRs are posted to NIC */
		*bad_wr = spdk_rdma_qp->send_wrs.first;
	}

	spdk_rdma_qp->send_wrs.first = NULL;
	spdk_rdma_qp->stats->send.doorbell_updates++;

	return rc;
}
