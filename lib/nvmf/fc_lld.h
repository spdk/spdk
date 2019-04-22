/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

#ifndef __FC_LLD_H__
#define __FC_LLD_H__

extern struct spdk_nvmf_fc_ll_drvr_ops *g_spdk_nvmf_fc_lld_ops;

static inline int
spdk_nvmf_fc_lld_init(void)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->lld_init();
}

static inline void
spdk_nvmf_fc_lld_start(void)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	g_spdk_nvmf_fc_lld_ops->lld_start();
}

static inline void
spdk_nvmf_fc_lld_fini(void)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	g_spdk_nvmf_fc_lld_ops->lld_fini();
}

static inline int
spdk_nvmf_fc_lld_queue_init(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->init_q(hwqp);
}

static inline void
spdk_nvmf_fc_lld_queue_reinit(void *queues_prev, void *queues_curr)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	g_spdk_nvmf_fc_lld_ops->reinit_q(queues_prev, queues_curr);
}

static inline int
spdk_nvmf_fc_lld_init_queue_buffers(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->init_q_buffers(hwqp);
}

static inline int
spdk_nvmf_fc_lld_set_queue_state(struct spdk_nvmf_fc_hwqp *hwqp, bool online)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->set_q_online_state(hwqp, online);
}


static inline struct spdk_nvmf_fc_xchg *
spdk_nvmf_fc_lld_get_xchg(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->get_xchg(hwqp);
}

static inline int
spdk_nvmf_fc_lld_put_xchg(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xchg)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->put_xchg(hwqp, xchg);
}

static inline uint32_t
spdk_nvmf_fc_lld_poll_queue(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->poll_queue(hwqp);
}

static inline int
spdk_nvmf_fc_lld_recv_data(struct spdk_nvmf_fc_request *fc_req)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->recv_data(fc_req);
}

static inline int
spdk_nvmf_fc_lld_send_data(struct spdk_nvmf_fc_request *fc_req)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->send_data(fc_req);
}

static inline int
spdk_nvmf_fc_lld_xmt_rsp(struct spdk_nvmf_fc_request *fc_req, uint8_t *ersp_buf, uint32_t ersp_len)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->xmt_rsp(fc_req, ersp_buf, ersp_len);
}

static inline int
spdk_nvmf_fc_lld_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->xmt_ls_rsp(tgtport, ls_rqst);
}

static inline int
spdk_nvmf_fc_lld_abort_xchg(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xchg,
			    spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->issue_abort(hwqp, xchg, cb, cb_args);
}

static inline int
spdk_nvmf_fc_lld_xmt_bls_rsp(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t ox_id, uint16_t rx_id,
			     uint16_t rpi, bool rjt, uint8_t rjt_exp, spdk_nvmf_fc_caller_cb cb,
			     void *cb_args)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->xmt_bls_rsp(hwqp, ox_id, rx_id, rpi, rjt, rjt_exp, cb, cb_args);
}

static inline int
spdk_nvmf_fc_lld_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
			      struct spdk_nvmf_fc_srsr_bufs *srsr_bufs,
			      spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->xmt_srsr_req(hwqp, srsr_bufs, cb, cb_args);
}

static inline int
spdk_nvmf_fc_lld_queue_sync_available(void)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->q_sync_available();
}

static inline int
spdk_nvmf_fc_lld_issue_queue_sync(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t u_id, uint16_t skip_rq)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->issue_q_sync(hwqp, u_id, skip_rq);
}

static inline void
spdk_nvmf_fc_lld_dump_queues(struct spdk_nvmf_fc_hwqp *ls_queues,
			     struct spdk_nvmf_fc_hwqp *io_queues,
			     uint32_t num_queues, struct spdk_nvmf_fc_queue_dump_info *dump_info)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->dump_all_queues(ls_queues, io_queues, num_queues, dump_info);
}

static inline bool
spdk_nvmf_fc_lld_assign_conn_to_hwqp(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t *conn_id,
				     uint32_t sq_size)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->assign_conn_to_hwqp(hwqp, conn_id, sq_size);
}

static inline struct spdk_nvmf_fc_hwqp *
spdk_nvmf_fc_lld_get_hwqp_from_conn_id(struct spdk_nvmf_fc_hwqp *hwqp, uint32_t num_queues,
				       uint64_t conn_id)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->get_hwqp_from_conn_id(hwqp, num_queues, conn_id);
}

static inline void
spdk_nvmf_fc_lld_queue_buffer_release(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t buff_idx)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	g_spdk_nvmf_fc_lld_ops->q_buffer_release(hwqp, buff_idx);
}

static inline struct spdk_nvmf_fc_srsr_bufs *
spdk_nvmf_fc_lld_alloc_srsr_bufs(size_t rqst_len, size_t rsp_len)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->alloc_srsr_bufs(rqst_len, rsp_len);
}

static inline void
spdk_nvmf_fc_lld_free_srsr_bufs(struct spdk_nvmf_fc_srsr_bufs *disconnect_bufs)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	g_spdk_nvmf_fc_lld_ops->free_srsr_bufs(disconnect_bufs);
}

static inline void
spdk_nvmf_fc_lld_release_conn(struct spdk_nvmf_fc_hwqp *hwqp, uint64_t conn_id, uint32_t sq_size)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->release_conn(hwqp, conn_id, sq_size);
}

static inline void
spdk_nvmf_fc_lld_get_xchg_info(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg_info *info)
{
	assert(g_spdk_nvmf_fc_lld_ops != NULL);

	return g_spdk_nvmf_fc_lld_ops->get_xchg_info(hwqp, info);
}

#endif
