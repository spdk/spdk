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
#include "common/lib/test_env.c"

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int len, i;

	if (trstring == NULL) {
		return -EINVAL;
	}

	len = strnlen(trstring, SPDK_NVMF_TRSTRING_MAX_LEN);
	if (len == SPDK_NVMF_TRSTRING_MAX_LEN) {
		return -EINVAL;
	}

	/* cast official trstring to uppercase version of input. */
	for (i = 0; i < len; i++) {
		trid->trstring[i] = toupper(trstring[i]);
	}
	return 0;
}

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB_V(nvme_ctrlr_destruct_finish, (struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB(nvme_ctrlr_construct, int, (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB_V(nvme_ctrlr_destruct, (struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB(nvme_ctrlr_get_vs, int, (struct spdk_nvme_ctrlr *ctrlr,
				     union spdk_nvme_vs_register *vs), 0);
DEFINE_STUB(nvme_ctrlr_get_cap, int, (struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_cap_register *cap), 0);
DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB_V(spdk_nvme_transport_register, (const struct spdk_nvme_transport_ops *ops));
DEFINE_STUB(nvme_transport_ctrlr_connect_qpair, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), (struct spdk_nvme_ctrlr_process *)(uintptr_t)0x1);
DEFINE_STUB(nvme_ctrlr_add_process, int, (struct spdk_nvme_ctrlr *ctrlr, void *devhandle), 0);
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB(nvme_get_transport, const struct spdk_nvme_transport *, (const char *transport_name),
	    NULL);
DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t, (struct spdk_nvme_qpair *qpair,
		uint32_t max_completions), 0);

/* Fabric transports only */
DEFINE_STUB_V(nvme_ctrlr_disconnect_qpair, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(nvme_fabric_ctrlr_set_reg_4, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value), 0);
DEFINE_STUB(nvme_fabric_ctrlr_set_reg_8, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t value), 0);
DEFINE_STUB(nvme_fabric_ctrlr_get_reg_4, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t *value), 0);
DEFINE_STUB(nvme_fabric_ctrlr_get_reg_8, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t *value), 0);
DEFINE_STUB(nvme_fabric_ctrlr_set_reg_4_async, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value, spdk_nvme_reg_cb cb, void *ctx), 0);
DEFINE_STUB(nvme_fabric_ctrlr_set_reg_8_async, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t value, spdk_nvme_reg_cb cb, void *ctx), 0);
DEFINE_STUB(nvme_fabric_ctrlr_get_reg_4_async, int, (struct spdk_nvme_ctrlr *ctrlr,
		uint32_t offset, spdk_nvme_reg_cb cb, void *ctx), 0);
DEFINE_STUB(nvme_fabric_ctrlr_get_reg_8_async, int, (struct spdk_nvme_ctrlr *ctrlr,
		uint32_t offset, spdk_nvme_reg_cb cb, void *ctx), 0);
DEFINE_STUB(nvme_fabric_ctrlr_scan, int, (struct spdk_nvme_probe_ctx *probe_ctx,
		bool direct_connect), 0);
DEFINE_STUB(nvme_fabric_qpair_connect, int, (struct spdk_nvme_qpair *qpair, uint32_t num_entries),
	    0);
DEFINE_STUB(nvme_fabric_qpair_connect_async, int, (struct spdk_nvme_qpair *qpair,
		uint32_t num_entries), 0);
DEFINE_STUB(nvme_fabric_qpair_connect_poll, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB_V(nvme_transport_ctrlr_disconnect_qpair, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair));
DEFINE_STUB(nvme_poll_group_disconnect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests, bool async)
{
	qpair->ctrlr = ctrlr;
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->async = async;
	qpair->trtype = SPDK_NVME_TRANSPORT_TCP;
	qpair->poll_group = (void *)0xDEADBEEF;

	return 0;
}
