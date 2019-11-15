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

#ifdef DEBUG
static __attribute__((noreturn)) void
nvme_transport_unknown(enum spdk_nvme_transport_type trtype)
{
	SPDK_ERRLOG("Unknown transport %d\n", (int)trtype);
	abort();
}
#define TRANSPORT_DEFAULT(trtype)				default: nvme_transport_unknown(trtype);
#else
#define TRANSPORT_DEFAULT(trtype)
#endif

#define TRANSPORT_PCIE(return_var, func_name, args)		case SPDK_NVME_TRANSPORT_PCIE: return_var = nvme_pcie_ ## func_name args; break;

#define TRANSPORT_FABRICS_TCP(return_var, func_name, args)	case SPDK_NVME_TRANSPORT_TCP: return_var = nvme_tcp_ ## func_name args; break;

#ifdef SPDK_CONFIG_RDMA
#define TRANSPORT_FABRICS_RDMA(return_var, func_name, args)	case SPDK_NVME_TRANSPORT_RDMA: return_var = nvme_rdma_ ## func_name args; break;
#define TRANSPORT_RDMA_AVAILABLE				true
#else
#define TRANSPORT_FABRICS_RDMA(return_var, func_name, args)	case SPDK_NVME_TRANSPORT_RDMA: SPDK_UNREACHABLE();
#define TRANSPORT_RDMA_AVAILABLE				false
#endif
#define TRANSPORT_FABRICS_FC(return_var, func_name, args)	case SPDK_NVME_TRANSPORT_FC: SPDK_UNREACHABLE();

#define TRANSPORT_PCIE_V(func_name, args)			case SPDK_NVME_TRANSPORT_PCIE: nvme_pcie_ ## func_name args; break;

#define TRANSPORT_FABRICS_TCP_V(func_name, args)		case SPDK_NVME_TRANSPORT_TCP: nvme_tcp_ ## func_name args; break;

#ifdef SPDK_CONFIG_RDMA
#define TRANSPORT_FABRICS_RDMA_V(func_name, args)		case SPDK_NVME_TRANSPORT_RDMA: nvme_rdma_ ## func_name args; break;
#define TRANSPORT_RDMA_AVAILABLE				true
#else
#define TRANSPORT_FABRICS_RDMA_V(func_name, args)		case SPDK_NVME_TRANSPORT_RDMA: SPDK_UNREACHABLE();
#define TRANSPORT_RDMA_AVAILABLE				false
#endif
#define TRANSPORT_FABRICS_FC_V(func_name, args)			case SPDK_NVME_TRANSPORT_FC: SPDK_UNREACHABLE();

#define NVME_TRANSPORT_CALL(trtype, return_var, func_name, args)	\
	do {								\
		switch (trtype) {					\
		TRANSPORT_PCIE(return_var, func_name, args)		\
		TRANSPORT_FABRICS_RDMA(return_var, func_name, args)	\
		TRANSPORT_FABRICS_FC(return_var, func_name, args)	\
		TRANSPORT_FABRICS_TCP(return_var, func_name, args)	\
		TRANSPORT_DEFAULT(trtype)				\
		}							\
	} while (0)

#define NVME_TRANSPORT_CALL_V(trtype, func_name, args)			\
	do {								\
		switch (trtype) {					\
		TRANSPORT_PCIE_V(func_name, args)			\
		TRANSPORT_FABRICS_RDMA_V(func_name, args)		\
		TRANSPORT_FABRICS_FC_V(func_name, args)			\
		TRANSPORT_FABRICS_TCP_V(func_name, args)		\
		TRANSPORT_DEFAULT(trtype)				\
		}							\
	} while (0)

bool
spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
	case SPDK_NVME_TRANSPORT_TCP:
		return true;

	case SPDK_NVME_TRANSPORT_RDMA:
		return TRANSPORT_RDMA_AVAILABLE;

	case SPDK_NVME_TRANSPORT_FC:
		return false;
	}

	return false;
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct spdk_nvme_ctrlr *ctrlr = NULL;

	NVME_TRANSPORT_CALL(trid->trtype, ctrlr, ctrlr_construct, (trid, opts, devhandle));
	return ctrlr;
}

int
nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
			  bool direct_connect)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(probe_ctx->trid.trtype, rc, ctrlr_scan, (probe_ctx, direct_connect));
	return rc;
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_destruct, (ctrlr));
	return rc;
}

int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_enable, (ctrlr));
	return rc;
}

int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_set_reg_4, (ctrlr, offset, value));
	return rc;
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_set_reg_8, (ctrlr, offset, value));
	return rc;
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_get_reg_4, (ctrlr, offset, value));
	return rc;
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_get_reg_8, (ctrlr, offset, value));
	return rc;
}

uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_get_max_xfer_size, (ctrlr));
	return rc;
}

uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	uint16_t rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_get_max_sges, (ctrlr));
	return rc;
}

void *
nvme_transport_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	void *buffer = NULL;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, buffer, ctrlr_alloc_cmb_io_buffer, (ctrlr, size));
	return buffer;
}

int
nvme_transport_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_free_cmb_io_buffer, (ctrlr, buf, size));
	return rc;
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_qpair *qpair = NULL;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, qpair, ctrlr_create_io_qpair, (ctrlr, qid, opts));
	return qpair;
}

int
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_delete_io_qpair, (ctrlr, qpair));
	return rc;
}

int
nvme_transport_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, rc, ctrlr_connect_qpair, (ctrlr, qpair));

	if (rc) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_DISABLED);
		qpair->transport_qp_is_failed = true;
	}
	return rc;
}

volatile struct spdk_nvme_registers *
nvme_transport_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	volatile struct spdk_nvme_registers *regs = NULL;
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, regs, ctrlr_get_registers, (ctrlr));

	return regs;
}

void
nvme_transport_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL_V(ctrlr->trid.trtype, ctrlr_disconnect_qpair, (ctrlr, qpair));
}

void
nvme_transport_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	assert(dnr <= 1);
	NVME_TRANSPORT_CALL_V(qpair->trtype, qpair_abort_reqs, (qpair, dnr));
}

int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(qpair->trtype, rc, qpair_reset, (qpair));
	return rc;
}

int
nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int rc = 0;

	NVME_TRANSPORT_CALL(qpair->trtype, rc, qpair_submit_request, (qpair, req));
	return rc;
}

int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	int32_t rc = 0;

	NVME_TRANSPORT_CALL(qpair->trtype, rc, qpair_process_completions, (qpair, max_completions));
	return rc;
}

void
nvme_transport_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL_V(qpair->trtype, admin_qpair_abort_aers, (qpair));
}
