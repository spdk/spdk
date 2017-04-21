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
#define TRANSPORT_DEFAULT(trtype)	default: nvme_transport_unknown(trtype);
#else
#define TRANSPORT_DEFAULT(trtype)
#endif

#define TRANSPORT_PCIE(func_name, args)	case SPDK_NVME_TRANSPORT_PCIE: return nvme_pcie_ ## func_name args;
#ifdef SPDK_CONFIG_RDMA
#define TRANSPORT_FABRICS_RDMA(func_name, args)	case SPDK_NVME_TRANSPORT_RDMA: return nvme_rdma_ ## func_name args;
#define TRANSPORT_RDMA_AVAILABLE		true
#else
#define TRANSPORT_FABRICS_RDMA(func_name, args)	case SPDK_NVME_TRANSPORT_RDMA: SPDK_UNREACHABLE();
#define TRANSPORT_RDMA_AVAILABLE		false
#endif
#define NVME_TRANSPORT_CALL(trtype, func_name, args) 	\
	do {							\
		switch (trtype) {				\
		TRANSPORT_PCIE(func_name, args)			\
		TRANSPORT_FABRICS_RDMA(func_name, args)		\
		TRANSPORT_DEFAULT(trtype)			\
		}						\
		SPDK_UNREACHABLE();				\
	} while (0)

bool
spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return true;

	case SPDK_NVME_TRANSPORT_RDMA:
		return TRANSPORT_RDMA_AVAILABLE;
	}

	return false;
}

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	NVME_TRANSPORT_CALL(trid->trtype, ctrlr_construct, (trid, opts, devhandle));
}

int
nvme_transport_ctrlr_scan(const struct spdk_nvme_transport_id *trid,
			  void *cb_ctx,
			  spdk_nvme_probe_cb probe_cb,
			  spdk_nvme_remove_cb remove_cb,
			  bool direct_connect)
{
	NVME_TRANSPORT_CALL(trid->trtype, ctrlr_scan, (trid, cb_ctx, probe_cb, remove_cb, direct_connect));
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_destruct, (ctrlr));
}

int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_enable, (ctrlr));
}

int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_set_reg_4, (ctrlr, offset, value));
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_set_reg_8, (ctrlr, offset, value));
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_get_reg_4, (ctrlr, offset, value));
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_get_reg_8, (ctrlr, offset, value));
}

uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_get_max_xfer_size, (ctrlr));
}

uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_get_max_sges, (ctrlr));
}

void *
nvme_transport_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_alloc_cmb_io_buffer, (ctrlr, size));
}

int
nvme_transport_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_free_cmb_io_buffer, (ctrlr, buf, size));
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_create_io_qpair, (ctrlr, qid, opts));
}

int
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_delete_io_qpair, (ctrlr, qpair));
}

int
nvme_transport_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(ctrlr->trid.trtype, ctrlr_reinit_io_qpair, (ctrlr, qpair));
}

int
nvme_transport_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_enable, (qpair));
}

int
nvme_transport_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_disable, (qpair));
}

int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_reset, (qpair));
}

int
nvme_transport_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_fail, (qpair));
}

int
nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_submit_request, (qpair, req));
}

int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	NVME_TRANSPORT_CALL(qpair->trtype, qpair_process_completions, (qpair, max_completions));
}
