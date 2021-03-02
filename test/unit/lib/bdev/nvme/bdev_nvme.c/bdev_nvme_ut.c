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

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"

#include "common/lib/ut_multithread.c"

#include "bdev/nvme/bdev_nvme.c"
#include "bdev/nvme/common.c"

#include "unit/lib/json_mock.c"

DEFINE_STUB(spdk_nvme_probe_async, struct spdk_nvme_probe_ctx *,
	    (const struct spdk_nvme_transport_id *trid, void *cb_ctx,
	     spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
	     spdk_nvme_remove_cb remove_cb), NULL);

DEFINE_STUB(spdk_nvme_detach, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));

DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *, (enum spdk_nvme_transport_type trtype),
	    NULL);

DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);

DEFINE_STUB_V(spdk_nvme_ctrlr_get_default_ctrlr_opts, (struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size));

DEFINE_STUB(spdk_nvme_ctrlr_set_trid, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_transport_id *trid), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_set_remove_cb, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_remove_cb remove_cb, void *remove_ctx));

DEFINE_STUB(spdk_nvme_ctrlr_process_admin_completions, int32_t,
	    (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_flags, uint64_t, (struct spdk_nvme_ctrlr *ctrlr), 0);

void
spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts, size_t opts_size)
{
	/* Avoid warning that opts is used uninitialised */
	memset(opts, 0, opts_size);
}

DEFINE_STUB(spdk_nvme_ctrlr_get_max_xfer_size, uint32_t,
	    (const struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_transport_id, const struct spdk_nvme_transport_id *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_nvme_ctrlr_register_aer_callback, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn, void *aer_cb_arg));

DEFINE_STUB_V(spdk_nvme_ctrlr_register_timeout_callback, (struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_us, spdk_nvme_timeout_cb cb_fn, void *cb_arg));

DEFINE_STUB(spdk_nvme_ctrlr_is_ocssd_supported, bool, (struct spdk_nvme_ctrlr *ctrlr), false);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_admin_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, void *buf, uint32_t len,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_abort, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, uint16_t cid, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_abort_ext, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, void *cmd_cb_arg, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw_with_md, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, void *md_buf, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ns_get_max_io_xfer_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_extended_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_pi_type, enum spdk_nvme_pi_type, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_supports_compare, bool, (struct spdk_nvme_ns *ns), false);

DEFINE_STUB(spdk_nvme_ns_get_md_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_dealloc_logical_block_read_value,
	    enum spdk_nvme_dealloc_logical_block_read_value, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_optimal_io_boundary, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_uuid, const struct spdk_uuid *,
	    (const struct spdk_nvme_ns *ns), NULL);

DEFINE_STUB(spdk_nvme_cuse_get_ns_name, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		char *name, size_t *size), 0);

DEFINE_STUB_V(spdk_bdev_module_finish_done, (void));

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));

DEFINE_STUB(spdk_opal_dev_construct, struct spdk_opal_dev *, (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_opal_dev_destruct, (struct spdk_opal_dev *dev));

DEFINE_STUB_V(bdev_ocssd_populate_namespace, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
		struct nvme_bdev_ns *nvme_ns, struct nvme_async_probe_ctx *ctx));

DEFINE_STUB_V(bdev_ocssd_depopulate_namespace, (struct nvme_bdev_ns *nvme_ns));

DEFINE_STUB_V(bdev_ocssd_namespace_config_json, (struct spdk_json_write_ctx *w,
		struct nvme_bdev_ns *nvme_ns));

DEFINE_STUB(bdev_ocssd_create_io_channel, int, (struct nvme_io_channel *ioch), 0);

DEFINE_STUB_V(bdev_ocssd_destroy_io_channel, (struct nvme_io_channel *ioch));

DEFINE_STUB(bdev_ocssd_init_ctrlr, int, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr), 0);

DEFINE_STUB_V(bdev_ocssd_fini_ctrlr, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr));

DEFINE_STUB_V(bdev_ocssd_handle_chunk_notification, (struct nvme_bdev_ctrlr *nvme_bdev_ctrlr));

struct ut_nvme_req {
	enum spdk_nvme_nvm_opcode	opc;
	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	struct spdk_nvme_cpl		cpl;
	TAILQ_ENTRY(ut_nvme_req)	tailq;
};

struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			id;
	bool				is_active;
};

struct spdk_nvme_ctrlr {
	uint32_t			num_ns;
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ns_data	*nsdata;
	struct spdk_nvme_ctrlr_data	cdata;
	bool				is_failed;
	struct spdk_nvme_transport_id	trid;
	TAILQ_HEAD(, spdk_nvme_qpair)	active_io_qpairs;
	TAILQ_ENTRY(spdk_nvme_ctrlr)	tailq;
	struct spdk_nvme_ctrlr_opts	opts;
};

struct spdk_nvme_poll_group {
	void				*ctx;
	TAILQ_HEAD(, spdk_nvme_qpair)	qpairs;
};

struct spdk_nvme_qpair {
	struct spdk_nvme_ctrlr		*ctrlr;
	bool				is_connected;
	TAILQ_HEAD(, ut_nvme_req)	outstanding_reqs;
	uint32_t			num_outstanding_reqs;
	TAILQ_ENTRY(spdk_nvme_qpair)	poll_group_tailq;
	struct spdk_nvme_poll_group	*poll_group;
	TAILQ_ENTRY(spdk_nvme_qpair)	tailq;
};

struct spdk_nvme_probe_ctx {
	struct spdk_nvme_transport_id	trid;
	void				*cb_ctx;
	spdk_nvme_attach_cb		attach_cb;
	struct spdk_nvme_ctrlr		*init_ctrlr;
};

static TAILQ_HEAD(, spdk_nvme_ctrlr) g_ut_init_ctrlrs = TAILQ_HEAD_INITIALIZER(g_ut_init_ctrlrs);
static TAILQ_HEAD(, spdk_nvme_ctrlr) g_ut_attached_ctrlrs = TAILQ_HEAD_INITIALIZER(
			g_ut_attached_ctrlrs);
static int g_ut_attach_ctrlr_status;
static size_t g_ut_attach_bdev_count;
static int g_ut_register_bdev_status;

static void
ut_init_trid(struct spdk_nvme_transport_id *trid)
{
	trid->trtype = SPDK_NVME_TRANSPORT_TCP;
	snprintf(trid->subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(trid->traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.8");
	snprintf(trid->trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4420");
}

static void
ut_init_trid2(struct spdk_nvme_transport_id *trid)
{
	trid->trtype = SPDK_NVME_TRANSPORT_TCP;
	snprintf(trid->subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(trid->traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.9");
	snprintf(trid->trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4420");
}

static struct spdk_nvme_ctrlr *
ut_attach_ctrlr(const struct spdk_nvme_transport_id *trid, uint32_t num_ns)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t i;

	ctrlr = calloc(1, sizeof(*ctrlr));
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	if (num_ns != 0) {
		ctrlr->num_ns = num_ns;
		ctrlr->ns = calloc(num_ns, sizeof(struct spdk_nvme_ns));
		if (ctrlr->ns == NULL) {
			free(ctrlr);
			return NULL;
		}

		ctrlr->nsdata = calloc(num_ns, sizeof(struct spdk_nvme_ns_data));
		if (ctrlr->nsdata == NULL) {
			free(ctrlr->ns);
			free(ctrlr);
			return NULL;
		}

		for (i = 0; i < num_ns; i++) {
			ctrlr->ns[i].id = i + 1;
			ctrlr->ns[i].ctrlr = ctrlr;
		}
	}

	ctrlr->trid = *trid;
	TAILQ_INIT(&ctrlr->active_io_qpairs);

	TAILQ_INSERT_TAIL(&g_ut_init_ctrlrs, ctrlr, tailq);

	return ctrlr;
}

static void
ut_detach_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	CU_ASSERT(TAILQ_EMPTY(&ctrlr->active_io_qpairs));

	TAILQ_REMOVE(&g_ut_attached_ctrlrs, ctrlr, tailq);
	free(ctrlr->nsdata);
	free(ctrlr->ns);
	free(ctrlr);
}

static int
ut_submit_nvme_request(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       enum spdk_nvme_nvm_opcode opc, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct ut_nvme_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}

	req->opc = opc;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	req->cpl.status.sc = SPDK_NVME_SC_SUCCESS;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_INSERT_TAIL(&qpair->outstanding_reqs, req, tailq);
	qpair->num_outstanding_reqs++;

	return 0;
}

static void
ut_bdev_io_set_buf(struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovcnt = 1;

	bdev_io->iov.iov_base = (void *)0xFEEDBEEF;
	bdev_io->iov.iov_len = 4096;
}

static int
cmp_int(int a, int b)
{
	return a - b;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	int cmp;

	/* We assume trtype is TCP for now. */
	CU_ASSERT(trid1->trtype == SPDK_NVME_TRANSPORT_TCP);

	cmp = cmp_int(trid1->trtype, trid2->trtype);
	if (cmp) {
		return cmp;
	}

	cmp = strcasecmp(trid1->traddr, trid2->traddr);
	if (cmp) {
		return cmp;
	}

	cmp = cmp_int(trid1->adrfam, trid2->adrfam);
	if (cmp) {
		return cmp;
	}

	cmp = strcasecmp(trid1->trsvcid, trid2->trsvcid);
	if (cmp) {
		return cmp;
	}

	cmp = strcmp(trid1->subnqn, trid2->subnqn);
	if (cmp) {
		return cmp;
	}

	return 0;
}

static void
nvme_ctrlr_poll_internal(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_probe_ctx *probe_ctx)
{
	if (ctrlr->is_failed) {
		free(ctrlr);
		return;
	}

	TAILQ_INSERT_TAIL(&g_ut_attached_ctrlrs, ctrlr, tailq);

	if (probe_ctx->attach_cb) {
		probe_ctx->attach_cb(probe_ctx->cb_ctx, &ctrlr->trid, ctrlr, &ctrlr->opts);
	}
}

int
spdk_nvme_probe_poll_async(struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr, *tmp;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ut_init_ctrlrs, tailq, tmp) {
		TAILQ_REMOVE(&g_ut_init_ctrlrs, ctrlr, tailq);
		nvme_ctrlr_poll_internal(ctrlr, probe_ctx);
	}

	free(probe_ctx);

	return 0;
}

struct spdk_nvme_probe_ctx *
spdk_nvme_connect_async(const struct spdk_nvme_transport_id *trid,
			const struct spdk_nvme_ctrlr_opts *opts,
			spdk_nvme_attach_cb attach_cb)
{
	struct spdk_nvme_probe_ctx *probe_ctx;

	if (trid == NULL) {
		return NULL;
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (probe_ctx == NULL) {
		return NULL;
	}

	probe_ctx->trid = *trid;
	probe_ctx->cb_ctx = (void *)opts;
	probe_ctx->attach_cb = attach_cb;

	return probe_ctx;
}

const struct spdk_nvme_ctrlr_data *
spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr)
{
	return &ctrlr->cdata;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->num_ns;
}

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	if (nsid < 1 || nsid > ctrlr->num_ns) {
		return NULL;
	}

	return &ctrlr->ns[nsid - 1];
}

bool
spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	if (nsid < 1 || nsid > ctrlr->num_ns) {
		return false;
	}

	return ctrlr->ns[nsid - 1].is_active;
}

union spdk_nvme_csts_register
	spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_csts_register csts;

	csts.raw = 0;

	return csts;
}

union spdk_nvme_vs_register
	spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr)
{
	union spdk_nvme_vs_register vs;

	vs.raw = 0;

	return vs;
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
{
	struct spdk_nvme_qpair *qpair;

	qpair = calloc(1, sizeof(*qpair));
	if (qpair == NULL) {
		return NULL;
	}

	qpair->ctrlr = ctrlr;
	TAILQ_INIT(&qpair->outstanding_reqs);
	TAILQ_INSERT_TAIL(&ctrlr->active_io_qpairs, qpair, tailq);

	return qpair;
}

int
spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *qpair)
{
	if (qpair->is_connected) {
		return -EISCONN;
	}

	qpair->is_connected = true;

	return 0;
}

int
spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr;

	ctrlr = qpair->ctrlr;

	if (ctrlr->is_failed) {
		return -ENXIO;
	}
	qpair->is_connected = true;

	return 0;
}

void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	qpair->is_connected = false;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	SPDK_CU_ASSERT_FATAL(qpair->ctrlr != NULL);

	qpair->is_connected = false;

	if (qpair->poll_group != NULL) {
		spdk_nvme_poll_group_remove(qpair->poll_group, qpair);
	}

	TAILQ_REMOVE(&qpair->ctrlr->active_io_qpairs, qpair, tailq);

	CU_ASSERT(qpair->num_outstanding_reqs == 0);

	free(qpair);

	return 0;
}

int
spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->is_failed = false;

	return 0;
}

void
spdk_nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->is_failed = true;
}

uint32_t
spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns)
{
	return ns->id;
}

struct spdk_nvme_ctrlr *
spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr;
}

static inline struct spdk_nvme_ns_data *
_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return &ns->ctrlr->nsdata[ns->id - 1];
}

const struct spdk_nvme_ns_data *
spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns)
{
	return _nvme_ns_get_data(ns);
}

uint64_t
spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns)
{
	return _nvme_ns_get_data(ns)->nsze;
}
int
spdk_nvme_ns_cmd_read_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
			      void *metadata, uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_READ, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_write_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *buffer, void *metadata, uint64_t lba,
			       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_WRITE, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_readv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t lba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			       spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
			       uint16_t apptag_mask, uint16_t apptag)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_READ, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_writev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t lba, uint32_t lba_count,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				uint16_t apptag_mask, uint16_t apptag)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_WRITE, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_comparev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  uint64_t lba, uint32_t lba_count,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				  spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				  spdk_nvme_req_next_sge_cb next_sge_fn,
				  void *metadata, uint16_t apptag_mask, uint16_t apptag)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_COMPARE, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_dataset_management(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint32_t type, const struct spdk_nvme_dsm_range *ranges, uint16_t num_ranges,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_DATASET_MANAGEMENT, cb_fn, cb_arg);
}

struct spdk_nvme_poll_group *
spdk_nvme_poll_group_create(void *ctx)
{
	struct spdk_nvme_poll_group *group;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	group->ctx = ctx;
	TAILQ_INIT(&group->qpairs);

	return group;
}

int
spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group)
{
	if (!TAILQ_EMPTY(&group->qpairs)) {
		return -EBUSY;
	}

	free(group);

	return 0;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct ut_nvme_req *req, *tmp;
	uint32_t num_completions = 0;

	TAILQ_FOREACH_SAFE(req, &qpair->outstanding_reqs, tailq, tmp) {
		TAILQ_REMOVE(&qpair->outstanding_reqs, req, tailq);
		qpair->num_outstanding_reqs--;

		req->cb_fn(req->cb_arg, &req->cpl);

		free(req);
		num_completions++;
	}

	return num_completions;
}

int64_t
spdk_nvme_poll_group_process_completions(struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	int64_t local_completions = 0, error_reason = 0, num_completions = 0;

	SPDK_CU_ASSERT_FATAL(completions_per_qpair == 0);

	if (disconnected_qpair_cb == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, poll_group_tailq, tmp_qpair) {
		if (qpair->is_connected) {
			local_completions = spdk_nvme_qpair_process_completions(qpair,
					    completions_per_qpair);
			if (local_completions < 0 && error_reason == 0) {
				error_reason = local_completions;
			} else {
				num_completions += local_completions;
				assert(num_completions >= 0);
			}
		}
	}

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, poll_group_tailq, tmp_qpair) {
		if (!qpair->is_connected) {
			disconnected_qpair_cb(qpair, group->ctx);
		}
	}

	return error_reason ? error_reason : num_completions;
}

int
spdk_nvme_poll_group_add(struct spdk_nvme_poll_group *group,
			 struct spdk_nvme_qpair *qpair)
{
	CU_ASSERT(!qpair->is_connected);

	qpair->poll_group = group;
	TAILQ_INSERT_TAIL(&group->qpairs, qpair, poll_group_tailq);

	return 0;
}

int
spdk_nvme_poll_group_remove(struct spdk_nvme_poll_group *group,
			    struct spdk_nvme_qpair *qpair)
{
	CU_ASSERT(!qpair->is_connected);

	TAILQ_REMOVE(&group->qpairs, qpair, poll_group_tailq);

	return 0;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	return g_ut_register_bdev_status;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	int rc;

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, rc);
	}
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;

	return 0;
}

struct spdk_io_channel *
spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io)
{
	return (struct spdk_io_channel *)bdev_io->internal.ch;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
	bdev_io->internal.in_submit_request = false;
}

void
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct, int sc)
{
	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
	}

	bdev_io->internal.error.nvme.cdw0 = cdw0;
	bdev_io->internal.error.nvme.sct = sct;
	bdev_io->internal.error.nvme.sc = sc;

	spdk_bdev_io_complete(bdev_io, bdev_io->internal.status);
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	struct spdk_io_channel *ch = spdk_bdev_io_get_io_channel(bdev_io);

	ut_bdev_io_set_buf(bdev_io);

	cb(ch, bdev_io, true);
}

static void
test_create_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	int rc;

	ut_init_trid(&trid);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0, NULL);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") != NULL);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") != NULL);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_reset_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = NULL;
	struct nvme_bdev_ctrlr_trid *curr_trid;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_io_channel *nvme_ch1, *nvme_ch2;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0, &nvme_bdev_ctrlr);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	curr_trid = TAILQ_FIRST(&nvme_bdev_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	ch1 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	nvme_ch1 = spdk_io_channel_get_ctx(ch1);
	CU_ASSERT(nvme_ch1->qpair != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	nvme_ch2 = spdk_io_channel_get_ctx(ch2);
	CU_ASSERT(nvme_ch2->qpair != NULL);

	/* Reset starts from thread 1. */
	set_thread(1);

	/* Case 1: ctrlr is already being destructed. */
	nvme_bdev_ctrlr->destruct = true;

	rc = _bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	CU_ASSERT(rc == -EBUSY);

	/* Case 2: reset is in progress. */
	nvme_bdev_ctrlr->destruct = false;
	nvme_bdev_ctrlr->resetting = true;

	rc = _bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	CU_ASSERT(rc == -EAGAIN);

	/* Case 3: reset completes successfully. */
	nvme_bdev_ctrlr->resetting = false;
	curr_trid->is_failed = true;
	ctrlr.is_failed = true;

	rc = _bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);
	CU_ASSERT(nvme_ch1->qpair != NULL);
	CU_ASSERT(nvme_ch2->qpair != NULL);

	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ch1->qpair == NULL);
	CU_ASSERT(nvme_ch2->qpair != NULL);

	poll_thread_times(1, 1);
	CU_ASSERT(nvme_ch1->qpair == NULL);
	CU_ASSERT(nvme_ch2->qpair == NULL);
	CU_ASSERT(ctrlr.is_failed == true);

	poll_thread_times(1, 1);
	CU_ASSERT(ctrlr.is_failed == false);

	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ch1->qpair != NULL);
	CU_ASSERT(nvme_ch2->qpair == NULL);

	poll_thread_times(1, 1);
	CU_ASSERT(nvme_ch1->qpair != NULL);
	CU_ASSERT(nvme_ch2->qpair != NULL);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);
	CU_ASSERT(curr_trid->is_failed == true);

	poll_thread_times(1, 1);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == false);
	CU_ASSERT(curr_trid->is_failed == false);

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_race_between_reset_and_destruct_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct spdk_io_channel *ch1, *ch2;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0, &nvme_bdev_ctrlr);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	/* Reset starts from thread 1. */
	set_thread(1);

	rc = _bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);

	/* Try destructing ctrlr while ctrlr is being reset, but it will be deferred. */
	set_thread(0);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nvme_bdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr->destruct == true);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);

	poll_threads();

	/* Reset completed but ctrlr is not still destructed yet. */
	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nvme_bdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr->destruct == true);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == false);

	/* Additional polling called spdk_io_device_unregister() to ctrlr,
	 * However there are two channels and destruct is not completed yet.
	 */
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nvme_bdev_ctrlr);

	set_thread(0);

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_failover_ctrlr(void)
{
	struct spdk_nvme_transport_id trid1 = {}, trid2 = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = NULL;
	struct nvme_bdev_ctrlr_trid *curr_trid, *next_trid;
	struct spdk_io_channel *ch1, *ch2;
	int rc;

	ut_init_trid(&trid1);
	ut_init_trid2(&trid2);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid1, 0, &nvme_bdev_ctrlr);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	/* First, test one trid case. */
	curr_trid = TAILQ_FIRST(&nvme_bdev_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	/* Failover starts from thread 1. */
	set_thread(1);

	/* Case 1: ctrlr is already being destructed. */
	nvme_bdev_ctrlr->destruct = true;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(curr_trid->is_failed == false);

	/* Case 2: reset is in progress. */
	nvme_bdev_ctrlr->destruct = false;
	nvme_bdev_ctrlr->resetting = true;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);

	/* Case 3: failover is in progress. */
	nvme_bdev_ctrlr->failover_in_progress = true;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(curr_trid->is_failed == false);

	/* Case 4: reset completes successfully. */
	nvme_bdev_ctrlr->resetting = false;
	nvme_bdev_ctrlr->failover_in_progress = false;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);
	CU_ASSERT(curr_trid->is_failed == true);

	poll_threads();

	curr_trid = TAILQ_FIRST(&nvme_bdev_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	CU_ASSERT(nvme_bdev_ctrlr->resetting == false);
	CU_ASSERT(curr_trid->is_failed == false);

	set_thread(0);

	/* Second, test two trids case. */
	rc = bdev_nvme_add_trid(nvme_bdev_ctrlr, &ctrlr, &trid2);
	CU_ASSERT(rc == 0);

	curr_trid = TAILQ_FIRST(&nvme_bdev_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);
	CU_ASSERT(&curr_trid->trid == nvme_bdev_ctrlr->connected_trid);
	CU_ASSERT(spdk_nvme_transport_id_compare(&curr_trid->trid, &trid1) == 0);

	/* Failover starts from thread 1. */
	set_thread(1);

	/* Case 5: reset is in progress. */
	nvme_bdev_ctrlr->resetting = true;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == -EAGAIN);

	/* Case 5: failover is in progress. */
	nvme_bdev_ctrlr->failover_in_progress = true;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);

	/* Case 6: failover completes successfully. */
	nvme_bdev_ctrlr->resetting = false;
	nvme_bdev_ctrlr->failover_in_progress = false;

	rc = bdev_nvme_failover(nvme_bdev_ctrlr, false);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);
	CU_ASSERT(nvme_bdev_ctrlr->failover_in_progress == true);

	next_trid = TAILQ_FIRST(&nvme_bdev_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(next_trid != NULL);
	CU_ASSERT(next_trid != curr_trid);
	CU_ASSERT(&next_trid->trid == nvme_bdev_ctrlr->connected_trid);
	CU_ASSERT(spdk_nvme_transport_id_compare(&next_trid->trid, &trid2) == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr->resetting == false);
	CU_ASSERT(nvme_bdev_ctrlr->failover_in_progress == false);

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_pending_reset(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = NULL;
	struct spdk_bdev_io *first_bdev_io, *second_bdev_io;
	struct nvme_bdev_io *first_bio, *second_bio;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_io_channel *nvme_ch1, *nvme_ch2;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	first_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct nvme_bdev_io));
	SPDK_CU_ASSERT_FATAL(first_bdev_io != NULL);
	first_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	first_bio = (struct nvme_bdev_io *)first_bdev_io->driver_ctx;

	second_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct nvme_bdev_io));
	SPDK_CU_ASSERT_FATAL(second_bdev_io != NULL);
	second_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	second_bio = (struct nvme_bdev_io *)second_bdev_io->driver_ctx;

	set_thread(0);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0, &nvme_bdev_ctrlr);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	nvme_ch1 = spdk_io_channel_get_ctx(ch1);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	nvme_ch2 = spdk_io_channel_get_ctx(ch2);

	/* The first abort request is submitted on thread 1, and the second abort request
	 * is submitted on thread 0 while processing the first request.
	 */
	rc = bdev_nvme_reset(nvme_ch2, first_bio);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_bdev_ctrlr->resetting == true);
	CU_ASSERT(TAILQ_EMPTY(&nvme_ch2->pending_resets));

	set_thread(0);

	rc = bdev_nvme_reset(nvme_ch1, second_bio);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_FIRST(&nvme_ch1->pending_resets) == second_bdev_io);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr->resetting == false);
	CU_ASSERT(first_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(second_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	set_thread(0);


	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	free(first_bdev_io);
	free(second_bdev_io);
}

static void
attach_ctrlr_done(void *cb_ctx, size_t bdev_count, int rc)
{
	CU_ASSERT(rc == g_ut_attach_ctrlr_status);
	CU_ASSERT(bdev_count == g_ut_attach_bdev_count);
}

static void
test_attach_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_host_id hostid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	const char *attached_names[32] = {};
	int rc;

	set_thread(0);

	ut_init_trid(&trid);

	/* If ctrlr fails, no nvme_bdev_ctrlr is created. Failed ctrlr is removed
	 * by probe polling.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->is_failed = true;
	g_ut_attach_ctrlr_status = -EIO;
	g_ut_attach_bdev_count = 0;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	/* If ctrlr has no namespace, one nvme_bdev_ctrlr with no namespace is created */
	ctrlr = ut_attach_ctrlr(&trid, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);
	CU_ASSERT(nvme_bdev_ctrlr->ctrlr == ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr->num_ns == 0);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
	ut_detach_ctrlr(ctrlr);

	/* If ctrlr has one namespace, one nvme_bdev_ctrlr with one namespace and
	 * one nvme_bdev is created.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 1);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].is_active = true;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);
	CU_ASSERT(nvme_bdev_ctrlr->ctrlr == ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr->num_ns == 1);

	CU_ASSERT(attached_names[0] != NULL && strcmp(attached_names[0], "nvme0n1") == 0);
	attached_names[0] = NULL;

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	ut_detach_ctrlr(ctrlr);

	/* Ctrlr has one namespace but one nvme_bdev_ctrlr with no namespace is
	 * created because creating one nvme_bdev failed.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 1);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].is_active = true;
	g_ut_register_bdev_status = -EINVAL;
	g_ut_attach_bdev_count = 0;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);
	CU_ASSERT(nvme_bdev_ctrlr->ctrlr == ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr->num_ns == 1);

	CU_ASSERT(attached_names[0] == NULL);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	ut_detach_ctrlr(ctrlr);

	g_ut_register_bdev_status = 0;
}

static void
test_reconnect_qpair(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = NULL;
	struct spdk_io_channel *ch;
	struct nvme_io_channel *nvme_ch;
	int rc;

	set_thread(0);

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	rc = nvme_bdev_ctrlr_create(&ctrlr, "nvme0", &trid, 0, &nvme_bdev_ctrlr);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	ch = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nvme_ch = spdk_io_channel_get_ctx(ch);
	CU_ASSERT(nvme_ch->qpair != NULL);
	CU_ASSERT(nvme_ch->group != NULL);
	CU_ASSERT(nvme_ch->group->group != NULL);
	CU_ASSERT(nvme_ch->group->poller != NULL);

	/* Test if the disconnected qpair is reconnected. */
	nvme_ch->qpair->is_connected = false;

	poll_threads();

	CU_ASSERT(nvme_ch->qpair->is_connected == true);

	/* If the ctrlr is failed, reconnecting qpair should fail too. */
	nvme_ch->qpair->is_connected = false;
	ctrlr.is_failed = true;

	poll_threads();

	CU_ASSERT(nvme_ch->qpair->is_connected == false);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_aer_cb(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_host_id hostid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *bdev;
	const char *attached_names[32] = {};
	union spdk_nvme_async_event_completion event = {};
	struct spdk_nvme_cpl cpl = {};
	int rc;

	set_thread(0);

	ut_init_trid(&trid);

	/* Attach a ctrlr, whose max number of namespaces is 4, and 2nd, 3rd, and 4th
	 * namespaces are populated.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 4);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[1].is_active = true;
	ctrlr->ns[2].is_active = true;
	ctrlr->ns[3].is_active = true;

	ctrlr->nsdata[3].nsze = 1024;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 3;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	CU_ASSERT(nvme_bdev_ctrlr->num_ns == 4);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[0]->populated == false);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[1]->populated == true);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[2]->populated == true);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[3]->populated == true);

	bdev = nvme_bdev_ns_to_bdev(nvme_bdev_ctrlr->namespaces[3]);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);
	CU_ASSERT(bdev->disk.blockcnt == 1024);

	/* Dynamically populate 1st namespace and depopulate 3rd namespace, and
	 * change the size of the 4th namespace.
	 */
	ctrlr->ns[0].is_active = true;
	ctrlr->ns[2].is_active = false;
	ctrlr->nsdata[3].nsze = 2048;

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED;
	cpl.cdw0 = event.raw;

	aer_cb(nvme_bdev_ctrlr, &cpl);

	CU_ASSERT(nvme_bdev_ctrlr->namespaces[0]->populated == true);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[1]->populated == true);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[2]->populated == false);
	CU_ASSERT(nvme_bdev_ctrlr->namespaces[3]->populated == true);
	CU_ASSERT(bdev->disk.blockcnt == 2048);

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	ut_detach_ctrlr(ctrlr);
}

static void
ut_test_submit_nvme_cmd(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			enum spdk_bdev_io_type io_type)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	bdev_io->type = io_type;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(nvme_ch->qpair->num_outstanding_reqs == 1);

	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(nvme_ch->qpair->num_outstanding_reqs == 0);
}

static void
ut_test_submit_nop(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		   enum spdk_bdev_io_type io_type)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	bdev_io->type = io_type;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(nvme_ch->qpair->num_outstanding_reqs == 0);
}

static void
ut_test_submit_fused_nvme_cmd(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_bdev_io *bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	struct ut_nvme_req *req;

	/* Only compare and write now. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(nvme_ch->qpair->num_outstanding_reqs == 2);
	CU_ASSERT(bio->first_fused_submitted == true);

	/* First outstanding request is compare operation. */
	req = TAILQ_FIRST(&nvme_ch->qpair->outstanding_reqs);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->opc == SPDK_NVME_OPC_COMPARE);
	req->cpl.cdw0 = SPDK_NVME_OPC_COMPARE;

	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(nvme_ch->qpair->num_outstanding_reqs == 0);
}

static void
test_submit_nvme_cmd(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_host_id hostid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	const char *attached_names[32] = {};
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_io_channel *ch;
	int rc;

	ut_init_trid(&trid);

	ctrlr = ut_attach_ctrlr(&trid, 1);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].is_active = true;
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, &hostid, "nvme0", attached_names, 32, NULL, 0,
			      attach_ctrlr_done, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_bdev_ctrlr != NULL);

	bdev = nvme_bdev_ns_to_bdev(nvme_bdev_ctrlr->namespaces[0]);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	ch = spdk_get_io_channel(nvme_bdev_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct nvme_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->bdev = &bdev->disk;
	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;

	bdev_io->u.bdev.iovs = NULL;

	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_READ);

	ut_bdev_io_set_buf(bdev_io);

	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_READ);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_WRITE);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_COMPARE);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_UNMAP);

	ut_test_submit_nop(ch, bdev_io, SPDK_BDEV_IO_TYPE_FLUSH);

	ut_test_submit_fused_nvme_cmd(ch, bdev_io);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0");
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	ut_detach_ctrlr(ctrlr);
}

int
main(int argc, const char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme", NULL, NULL);

	CU_ADD_TEST(suite, test_create_ctrlr);
	CU_ADD_TEST(suite, test_reset_ctrlr);
	CU_ADD_TEST(suite, test_race_between_reset_and_destruct_ctrlr);
	CU_ADD_TEST(suite, test_failover_ctrlr);
	CU_ADD_TEST(suite, test_pending_reset);
	CU_ADD_TEST(suite, test_attach_ctrlr);
	CU_ADD_TEST(suite, test_reconnect_qpair);
	CU_ADD_TEST(suite, test_aer_cb);
	CU_ADD_TEST(suite, test_submit_nvme_cmd);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	allocate_threads(3);
	set_thread(0);
	bdev_nvme_library_init();

	CU_basic_run_tests();

	set_thread(0);
	bdev_nvme_library_fini();
	free_threads();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
