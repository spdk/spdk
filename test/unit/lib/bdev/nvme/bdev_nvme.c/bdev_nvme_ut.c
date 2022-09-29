/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_module.h"

#include "common/lib/ut_multithread.c"

#include "bdev/nvme/bdev_nvme.c"

#include "unit/lib/json_mock.c"

#include "bdev/nvme/bdev_mdns_client.c"

static void *g_accel_p = (void *)0xdeadbeaf;

DEFINE_STUB(spdk_nvme_probe_async, struct spdk_nvme_probe_ctx *,
	    (const struct spdk_nvme_transport_id *trid, void *cb_ctx,
	     spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
	     spdk_nvme_remove_cb remove_cb), NULL);

DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));

DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *, (enum spdk_nvme_transport_type trtype),
	    NULL);

DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_set_trid, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_transport_id *trid), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_set_remove_cb, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_remove_cb remove_cb, void *remove_ctx));

DEFINE_STUB(spdk_nvme_ctrlr_get_flags, uint64_t, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(accel_channel_create, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_channel_destroy, (void *io_device, void *ctx_buf));

DEFINE_STUB(spdk_nvme_ctrlr_get_discovery_log_page, int,
	    (struct spdk_nvme_ctrlr *ctrlr, spdk_nvme_discovery_cb cb_fn, void *cb_arg), 0);

DEFINE_RETURN_MOCK(spdk_nvme_ctrlr_get_memory_domains, int);

DEFINE_STUB_V(spdk_jsonrpc_send_error_response, (struct spdk_jsonrpc_request *request,
		int error_code, const char *msg));

DEFINE_STUB_V(spdk_nvme_transport_get_opts, (struct spdk_nvme_transport_opts *opts,
		size_t opts_size));

DEFINE_STUB(spdk_nvme_transport_set_opts, int, (const struct spdk_nvme_transport_opts *opts,
		size_t opts_size), 0);

DEFINE_STUB(spdk_bdev_io_get_submit_tsc, uint64_t, (struct spdk_bdev_io *bdev_io), 0);

DEFINE_STUB_V(spdk_bdev_reset_io_stat, (struct spdk_bdev_io_stat *stat,
					enum spdk_bdev_reset_stat_mode mode));
DEFINE_STUB_V(spdk_bdev_add_io_stat, (struct spdk_bdev_io_stat *total,
				      struct spdk_bdev_io_stat *add));

int
spdk_nvme_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	int i, min_array_size;

	if (ut_spdk_nvme_ctrlr_get_memory_domains > 0 && domains && array_size > 0) {
		min_array_size = spdk_min(ut_spdk_nvme_ctrlr_get_memory_domains, array_size);
		for (i = 0; i < min_array_size; i++) {
			domains[i] = (struct spdk_memory_domain *)0xf1f2f3f4f5;
		}
	}
	HANDLE_RETURN_MOCK(spdk_nvme_ctrlr_get_memory_domains);

	return 0;
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(g_accel_p);
}

void
spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts, size_t opts_size)
{
	/* Avoid warning that opts is used uninitialised */
	memset(opts, 0, opts_size);
}

DEFINE_STUB(spdk_nvme_ctrlr_get_opts, const struct spdk_nvme_ctrlr_opts *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_ctrlr_get_max_xfer_size, uint32_t,
	    (const struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_ctrlr_get_transport_id, const struct spdk_nvme_transport_id *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_nvme_ctrlr_register_aer_callback, (struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn, void *aer_cb_arg));

DEFINE_STUB_V(spdk_nvme_ctrlr_register_timeout_callback, (struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_io_us, uint64_t timeout_admin_us, spdk_nvme_timeout_cb cb_fn, void *cb_arg));

DEFINE_STUB(spdk_nvme_ctrlr_is_fabrics, bool, (struct spdk_nvme_ctrlr *ctrlr), true);

DEFINE_STUB(spdk_nvme_ctrlr_is_ocssd_supported, bool, (struct spdk_nvme_ctrlr *ctrlr), false);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_abort, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, uint16_t cid, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ctrlr_cmd_io_raw_with_md, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd, void *buf,
		uint32_t len, void *md_buf, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_cuse_get_ctrlr_name, int, (struct spdk_nvme_ctrlr *ctrlr, char *name,
		size_t *size), 0);

DEFINE_STUB(spdk_nvme_ns_get_max_io_xfer_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_extended_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_pi_type, enum spdk_nvme_pi_type, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_supports_compare, bool, (struct spdk_nvme_ns *ns), false);

DEFINE_STUB(spdk_nvme_ns_get_md_size, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_dealloc_logical_block_read_value,
	    enum spdk_nvme_dealloc_logical_block_read_value, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_ns_get_optimal_io_boundary, uint32_t, (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_cuse_get_ns_name, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		char *name, size_t *size), 0);

DEFINE_STUB(spdk_nvme_zns_ns_get_zone_size_sectors, uint64_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_zns_ctrlr_get_max_zone_append_size, uint32_t,
	    (const struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_zns_ns_get_max_open_zones, uint32_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_zns_ns_get_max_active_zones, uint32_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_zns_ns_get_num_zones, uint64_t,
	    (struct spdk_nvme_ns *ns), 0);

DEFINE_STUB(spdk_nvme_zns_zone_append_with_md, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, void *metadata,
	     uint64_t zslba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
	     uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_zns_zone_appendv_with_md, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t zslba,
	     uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
	     spdk_nvme_req_reset_sgl_cb reset_sgl_fn, spdk_nvme_req_next_sge_cb next_sge_fn,
	     void *metadata, uint16_t apptag_mask, uint16_t apptag), 0);

DEFINE_STUB(spdk_nvme_zns_report_zones, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	     void *payload, uint32_t payload_size, uint64_t slba,
	     enum spdk_nvme_zns_zra_report_opts report_opts, bool partial_report,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_zns_close_zone, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
	     bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_zns_finish_zone, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
	     bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_zns_open_zone, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
	     bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_zns_reset_zone, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
	     bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_ns_get_nguid, const uint8_t *, (const struct spdk_nvme_ns *ns), NULL);

DEFINE_STUB(spdk_nvme_zns_offline_zone, int,
	    (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, uint64_t slba,
	     bool select_all, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB(spdk_nvme_cpl_get_status_type_string, const char *,
	    (const struct spdk_nvme_status *status), NULL);

DEFINE_STUB(spdk_nvme_cpl_get_status_string, const char *,
	    (const struct spdk_nvme_status *status), NULL);

DEFINE_STUB_V(spdk_bdev_module_fini_done, (void));

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));

DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));

DEFINE_STUB(spdk_opal_dev_construct, struct spdk_opal_dev *, (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB_V(spdk_opal_dev_destruct, (struct spdk_opal_dev *dev));

DEFINE_STUB(spdk_accel_submit_crc32cv, int, (struct spdk_io_channel *ch, uint32_t *dst,
		struct iovec *iov,
		uint32_t iov_cnt, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_prepare_for_reset, (struct spdk_nvme_ctrlr *ctrlr));

struct ut_nvme_req {
	uint16_t			opc;
	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	struct spdk_nvme_cpl		cpl;
	TAILQ_ENTRY(ut_nvme_req)	tailq;
};

struct spdk_nvme_ns {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			id;
	bool				is_active;
	struct spdk_uuid		*uuid;
	enum spdk_nvme_ana_state	ana_state;
	enum spdk_nvme_csi		csi;
};

struct spdk_nvme_qpair {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint8_t				failure_reason;
	bool				is_connected;
	bool				in_completion_context;
	bool				delete_after_completion_context;
	TAILQ_HEAD(, ut_nvme_req)	outstanding_reqs;
	uint32_t			num_outstanding_reqs;
	TAILQ_ENTRY(spdk_nvme_qpair)	poll_group_tailq;
	struct spdk_nvme_poll_group	*poll_group;
	void				*poll_group_tailq_head;
	TAILQ_ENTRY(spdk_nvme_qpair)	tailq;
};

struct spdk_nvme_ctrlr {
	uint32_t			num_ns;
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ns_data	*nsdata;
	struct spdk_nvme_qpair		adminq;
	struct spdk_nvme_ctrlr_data	cdata;
	bool				attached;
	bool				is_failed;
	bool				fail_reset;
	bool				is_removed;
	struct spdk_nvme_transport_id	trid;
	TAILQ_HEAD(, spdk_nvme_qpair)	active_io_qpairs;
	TAILQ_ENTRY(spdk_nvme_ctrlr)	tailq;
	struct spdk_nvme_ctrlr_opts	opts;
};

struct spdk_nvme_poll_group {
	void				*ctx;
	struct spdk_nvme_accel_fn_table	accel_fn_table;
	TAILQ_HEAD(, spdk_nvme_qpair)	connected_qpairs;
	TAILQ_HEAD(, spdk_nvme_qpair)	disconnected_qpairs;
};

struct spdk_nvme_probe_ctx {
	struct spdk_nvme_transport_id	trid;
	void				*cb_ctx;
	spdk_nvme_attach_cb		attach_cb;
	struct spdk_nvme_ctrlr		*init_ctrlr;
};

uint32_t
spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;

	for (nsid = 1; nsid <= ctrlr->num_ns; nsid++) {
		if (ctrlr->ns[nsid - 1].is_active) {
			return nsid;
		}
	}

	return 0;
}

uint32_t
spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	for (nsid = nsid + 1; nsid <= ctrlr->num_ns; nsid++) {
		if (ctrlr->ns[nsid - 1].is_active) {
			return nsid;
		}
	}

	return 0;
}

uint32_t
spdk_nvme_qpair_get_num_outstanding_reqs(struct spdk_nvme_qpair *qpair)
{
	return qpair->num_outstanding_reqs;
}

static TAILQ_HEAD(, spdk_nvme_ctrlr) g_ut_init_ctrlrs = TAILQ_HEAD_INITIALIZER(g_ut_init_ctrlrs);
static TAILQ_HEAD(, spdk_nvme_ctrlr) g_ut_attached_ctrlrs = TAILQ_HEAD_INITIALIZER(
			g_ut_attached_ctrlrs);
static int g_ut_attach_ctrlr_status;
static size_t g_ut_attach_bdev_count;
static int g_ut_register_bdev_status;
static struct spdk_bdev *g_ut_registered_bdev;
static uint16_t g_ut_cntlid;
static struct nvme_path_id g_any_path = {};

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

static void
ut_init_trid3(struct spdk_nvme_transport_id *trid)
{
	trid->trtype = SPDK_NVME_TRANSPORT_TCP;
	snprintf(trid->subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(trid->traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.10");
	snprintf(trid->trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4420");
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

static struct spdk_nvme_ctrlr *
ut_attach_ctrlr(const struct spdk_nvme_transport_id *trid, uint32_t num_ns,
		bool ana_reporting, bool multipath)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t i;

	TAILQ_FOREACH(ctrlr, &g_ut_init_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, trid) == 0) {
			/* There is a ctrlr whose trid matches. */
			return NULL;
		}
	}

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		return NULL;
	}

	ctrlr->attached = true;
	ctrlr->adminq.ctrlr = ctrlr;
	TAILQ_INIT(&ctrlr->adminq.outstanding_reqs);
	ctrlr->adminq.is_connected = true;

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
			ctrlr->ns[i].is_active = true;
			ctrlr->ns[i].ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
			ctrlr->nsdata[i].nsze = 1024;
			ctrlr->nsdata[i].nmic.can_share = multipath;
		}

		ctrlr->cdata.nn = num_ns;
		ctrlr->cdata.mnan = num_ns;
		ctrlr->cdata.nanagrpid = num_ns;
	}

	ctrlr->cdata.cntlid = ++g_ut_cntlid;
	ctrlr->cdata.cmic.multi_ctrlr = multipath;
	ctrlr->cdata.cmic.ana_reporting = ana_reporting;
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
		       uint16_t opc, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
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

static struct ut_nvme_req *
ut_get_outstanding_nvme_request(struct spdk_nvme_qpair *qpair, void *cb_arg)
{
	struct ut_nvme_req *req;

	TAILQ_FOREACH(req, &qpair->outstanding_reqs, tailq) {
		if (req->cb_arg == cb_arg) {
			break;
		}
	}

	return req;
}

static struct spdk_bdev_io *
ut_alloc_bdev_io(enum spdk_bdev_io_type type, struct nvme_bdev *nbdev,
		 struct spdk_io_channel *ch)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct nvme_bdev_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);
	bdev_io->type = type;
	bdev_io->bdev = &nbdev->disk;
	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;

	return bdev_io;
}

static void
ut_bdev_io_set_buf(struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovcnt = 1;

	bdev_io->iov.iov_base = (void *)0xFEEDBEEF;
	bdev_io->iov.iov_len = 4096;
}

static void
nvme_ctrlr_poll_internal(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_probe_ctx *probe_ctx)
{
	if (ctrlr->is_failed) {
		free(ctrlr);
		return;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr->opts, sizeof(ctrlr->opts));
	if (probe_ctx->cb_ctx) {
		ctrlr->opts = *(struct spdk_nvme_ctrlr_opts *)probe_ctx->cb_ctx;
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
		if (spdk_nvme_transport_id_compare(&ctrlr->trid, &probe_ctx->trid) != 0) {
			continue;
		}
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

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->attached) {
		ut_detach_ctrlr(ctrlr);
	}

	return 0;
}

int
spdk_nvme_detach_async(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_detach_ctx **ctx)
{
	SPDK_CU_ASSERT_FATAL(ctx != NULL);
	*(struct spdk_nvme_ctrlr **)ctx = ctrlr;

	return 0;
}

int
spdk_nvme_detach_poll_async(struct spdk_nvme_detach_ctx *ctx)
{
	return spdk_nvme_detach((struct spdk_nvme_ctrlr *)ctx);
}

void
spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts, size_t opts_size)
{
	memset(opts, 0, opts_size);

	snprintf(opts->hostnqn, sizeof(opts->hostnqn),
		 "nqn.2014-08.org.nvmexpress:uuid:7391e776-0716-11ec-9a03-0242ac130003");
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

static void
nvme_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group = qpair->poll_group;

	CU_ASSERT(qpair->poll_group_tailq_head == &group->disconnected_qpairs);

	qpair->poll_group_tailq_head = &group->connected_qpairs;
	TAILQ_REMOVE(&group->disconnected_qpairs, qpair, poll_group_tailq);
	TAILQ_INSERT_TAIL(&group->connected_qpairs, qpair, poll_group_tailq);
}

static void
nvme_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group = qpair->poll_group;

	CU_ASSERT(qpair->poll_group_tailq_head == &group->connected_qpairs);

	qpair->poll_group_tailq_head = &group->disconnected_qpairs;
	TAILQ_REMOVE(&group->connected_qpairs, qpair, poll_group_tailq);
	TAILQ_INSERT_TAIL(&group->disconnected_qpairs, qpair, poll_group_tailq);
}

int
spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *qpair)
{
	if (qpair->is_connected) {
		return -EISCONN;
	}

	qpair->is_connected = true;
	qpair->failure_reason = SPDK_NVME_QPAIR_FAILURE_NONE;

	if (qpair->poll_group) {
		nvme_poll_group_connect_qpair(qpair);
	}

	return 0;
}

void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	if (!qpair->is_connected) {
		return;
	}

	qpair->is_connected = false;

	if (qpair->poll_group != NULL) {
		nvme_poll_group_disconnect_qpair(qpair);
	}
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	SPDK_CU_ASSERT_FATAL(qpair->ctrlr != NULL);

	if (qpair->in_completion_context) {
		qpair->delete_after_completion_context = true;
		return 0;
	}

	spdk_nvme_ctrlr_disconnect_io_qpair(qpair);

	if (qpair->poll_group != NULL) {
		spdk_nvme_poll_group_remove(qpair->poll_group, qpair);
	}

	TAILQ_REMOVE(&qpair->ctrlr->active_io_qpairs, qpair, tailq);

	CU_ASSERT(qpair->num_outstanding_reqs == 0);

	free(qpair);

	return 0;
}

int
spdk_nvme_ctrlr_reconnect_poll_async(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->fail_reset) {
		ctrlr->is_failed = true;
		return -EIO;
	}

	ctrlr->adminq.is_connected = true;
	return 0;
}

void
spdk_nvme_ctrlr_reconnect_async(struct spdk_nvme_ctrlr *ctrlr)
{
}

int
spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->is_removed) {
		return -ENXIO;
	}

	ctrlr->adminq.is_connected = false;
	ctrlr->is_failed = false;

	return 0;
}

void
spdk_nvme_ctrlr_fail(struct spdk_nvme_ctrlr *ctrlr)
{
	ctrlr->is_failed = true;
}

bool
spdk_nvme_ctrlr_is_failed(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->is_failed;
}

spdk_nvme_qp_failure_reason
spdk_nvme_ctrlr_get_admin_qp_failure_reason(struct spdk_nvme_ctrlr *ctrlr)
{
	return spdk_nvme_qpair_get_failure_reason(&ctrlr->adminq);
}

#define UT_ANA_DESC_SIZE	(sizeof(struct spdk_nvme_ana_group_descriptor) +	\
				 sizeof(uint32_t))
static void
ut_create_ana_log_page(struct spdk_nvme_ctrlr *ctrlr, char *buf, uint32_t length)
{
	struct spdk_nvme_ana_page ana_hdr;
	char _ana_desc[UT_ANA_DESC_SIZE];
	struct spdk_nvme_ana_group_descriptor *ana_desc;
	struct spdk_nvme_ns *ns;
	uint32_t i;

	memset(&ana_hdr, 0, sizeof(ana_hdr));
	ana_hdr.num_ana_group_desc = ctrlr->num_ns;

	SPDK_CU_ASSERT_FATAL(sizeof(ana_hdr) <= length);
	memcpy(buf, (char *)&ana_hdr, sizeof(ana_hdr));

	buf += sizeof(ana_hdr);
	length -= sizeof(ana_hdr);

	ana_desc = (struct spdk_nvme_ana_group_descriptor *)_ana_desc;

	for (i = 0; i < ctrlr->num_ns; i++) {
		ns = &ctrlr->ns[i];

		if (!ns->is_active) {
			continue;
		}

		memset(ana_desc, 0, UT_ANA_DESC_SIZE);

		ana_desc->ana_group_id = ns->id;
		ana_desc->num_of_nsid = 1;
		ana_desc->ana_state = ns->ana_state;
		ana_desc->nsid[0] = ns->id;

		SPDK_CU_ASSERT_FATAL(UT_ANA_DESC_SIZE <= length);
		memcpy(buf, (char *)ana_desc, UT_ANA_DESC_SIZE);

		buf += UT_ANA_DESC_SIZE;
		length -= UT_ANA_DESC_SIZE;
	}
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr,
				 uint8_t log_page, uint32_t nsid,
				 void *payload, uint32_t payload_size,
				 uint64_t offset,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	if (log_page == SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS) {
		SPDK_CU_ASSERT_FATAL(offset == 0);
		ut_create_ana_log_page(ctrlr, payload, payload_size);
	}

	return ut_submit_nvme_request(NULL, &ctrlr->adminq, SPDK_NVME_OPC_GET_LOG_PAGE,
				      cb_fn, cb_arg);
}

int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd, void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ut_submit_nvme_request(NULL, &ctrlr->adminq, cmd->opc, cb_fn, cb_arg);
}

int
spdk_nvme_ctrlr_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			      void *cmd_cb_arg,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct ut_nvme_req *req = NULL, *abort_req;

	if (qpair == NULL) {
		qpair = &ctrlr->adminq;
	}

	abort_req = calloc(1, sizeof(*abort_req));
	if (abort_req == NULL) {
		return -ENOMEM;
	}

	TAILQ_FOREACH(req, &qpair->outstanding_reqs, tailq) {
		if (req->cb_arg == cmd_cb_arg) {
			break;
		}
	}

	if (req == NULL) {
		free(abort_req);
		return -ENOENT;
	}

	req->cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	abort_req->opc = SPDK_NVME_OPC_ABORT;
	abort_req->cb_fn = cb_fn;
	abort_req->cb_arg = cb_arg;

	abort_req->cpl.status.sc = SPDK_NVME_SC_SUCCESS;
	abort_req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	abort_req->cpl.cdw0 = 0;

	TAILQ_INSERT_TAIL(&ctrlr->adminq.outstanding_reqs, abort_req, tailq);
	ctrlr->adminq.num_outstanding_reqs++;

	return 0;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
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

const struct spdk_uuid *
spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns)
{
	return ns->uuid;
}

enum spdk_nvme_csi
spdk_nvme_ns_get_csi(const struct spdk_nvme_ns *ns) {
	return ns->csi;
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

static bool g_ut_readv_ext_called;
int
spdk_nvme_ns_cmd_readv_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn,
			   struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	g_ut_readv_ext_called = true;
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_READ, cb_fn, cb_arg);
}

static bool g_ut_writev_ext_called;
int
spdk_nvme_ns_cmd_writev_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn,
			    struct spdk_nvme_ns_cmd_ext_io_opts *opts)
{
	g_ut_writev_ext_called = true;
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

int
spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_WRITE_ZEROES, cb_fn, cb_arg);
}

int
spdk_nvme_ns_cmd_copy(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const struct spdk_nvme_scc_source_range *ranges,
		      uint16_t num_ranges, uint64_t dest_lba,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return ut_submit_nvme_request(ns, qpair, SPDK_NVME_OPC_COPY, cb_fn, cb_arg);
}

struct spdk_nvme_poll_group *
spdk_nvme_poll_group_create(void *ctx, struct spdk_nvme_accel_fn_table *table)
{
	struct spdk_nvme_poll_group *group;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	group->ctx = ctx;
	if (table != NULL) {
		group->accel_fn_table = *table;
	}
	TAILQ_INIT(&group->connected_qpairs);
	TAILQ_INIT(&group->disconnected_qpairs);

	return group;
}

int
spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group)
{
	if (!TAILQ_EMPTY(&group->connected_qpairs) ||
	    !TAILQ_EMPTY(&group->disconnected_qpairs)) {
		return -EBUSY;
	}

	free(group);

	return 0;
}

spdk_nvme_qp_failure_reason
spdk_nvme_qpair_get_failure_reason(struct spdk_nvme_qpair *qpair)
{
	return qpair->failure_reason;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct ut_nvme_req *req, *tmp;
	uint32_t num_completions = 0;

	if (!qpair->is_connected) {
		return -ENXIO;
	}

	qpair->in_completion_context = true;

	TAILQ_FOREACH_SAFE(req, &qpair->outstanding_reqs, tailq, tmp) {
		TAILQ_REMOVE(&qpair->outstanding_reqs, req, tailq);
		qpair->num_outstanding_reqs--;

		req->cb_fn(req->cb_arg, &req->cpl);

		free(req);
		num_completions++;
	}

	qpair->in_completion_context = false;
	if (qpair->delete_after_completion_context) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
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

	TAILQ_FOREACH_SAFE(qpair, &group->disconnected_qpairs, poll_group_tailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, group->ctx);
	}

	TAILQ_FOREACH_SAFE(qpair, &group->connected_qpairs, poll_group_tailq, tmp_qpair) {
		if (qpair->failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE) {
			spdk_nvme_ctrlr_disconnect_io_qpair(qpair);
			/* Bump the number of completions so this counts as "busy" */
			num_completions++;
			continue;
		}

		local_completions = spdk_nvme_qpair_process_completions(qpair,
				    completions_per_qpair);
		if (local_completions < 0 && error_reason == 0) {
			error_reason = local_completions;
		} else {
			num_completions += local_completions;
			assert(num_completions >= 0);
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
	qpair->poll_group_tailq_head = &group->disconnected_qpairs;
	TAILQ_INSERT_TAIL(&group->disconnected_qpairs, qpair, poll_group_tailq);

	return 0;
}

int
spdk_nvme_poll_group_remove(struct spdk_nvme_poll_group *group,
			    struct spdk_nvme_qpair *qpair)
{
	CU_ASSERT(!qpair->is_connected);

	if (qpair->poll_group == NULL) {
		return -ENOENT;
	}

	CU_ASSERT(qpair->poll_group_tailq_head == &group->disconnected_qpairs);

	TAILQ_REMOVE(&group->disconnected_qpairs, qpair, poll_group_tailq);

	qpair->poll_group = NULL;
	qpair->poll_group_tailq_head = NULL;

	return 0;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	g_ut_registered_bdev = bdev;

	return g_ut_register_bdev_status;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	int rc;

	rc = bdev->fn_table->destruct(bdev->ctxt);

	if (bdev == g_ut_registered_bdev) {
		g_ut_registered_bdev = NULL;
	}

	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, rc);
	}
}

int
spdk_bdev_open_ext(const char *bdev_name, bool write,
		   spdk_bdev_event_cb_t event_cb, void *event_ctx,
		   struct spdk_bdev_desc **desc)
{
	if (g_ut_registered_bdev == NULL ||
	    strcmp(g_ut_registered_bdev->name, bdev_name) != 0) {
		return -ENODEV;
	}

	*desc = (struct spdk_bdev_desc *)g_ut_registered_bdev;

	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return (struct spdk_bdev *)desc;
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
	} else if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
		bdev_io->internal.status = SPDK_BDEV_IO_STATUS_ABORTED;
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

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid, NULL);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") != NULL);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") != NULL);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
ut_check_hotplug_on_reset(void *cb_arg, bool success)
{
	bool *detect_remove = cb_arg;

	CU_ASSERT(success == false);
	SPDK_CU_ASSERT_FATAL(detect_remove != NULL);

	*detect_remove = true;
}

static void
test_reset_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	struct nvme_path_id *curr_trid;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_ctrlr_channel *ctrlr_ch1, *ctrlr_ch2;
	bool detect_remove;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	curr_trid = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	ch1 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	ctrlr_ch1 = spdk_io_channel_get_ctx(ch1);
	CU_ASSERT(ctrlr_ch1->qpair != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	ctrlr_ch2 = spdk_io_channel_get_ctx(ch2);
	CU_ASSERT(ctrlr_ch2->qpair != NULL);

	/* Reset starts from thread 1. */
	set_thread(1);

	/* Case 1: ctrlr is already being destructed. */
	nvme_ctrlr->destruct = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == -ENXIO);

	/* Case 2: reset is in progress. */
	nvme_ctrlr->destruct = false;
	nvme_ctrlr->resetting = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == -EBUSY);

	/* Case 3: reset completes successfully. */
	nvme_ctrlr->resetting = false;
	curr_trid->is_failed = true;
	ctrlr.is_failed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(ctrlr_ch1->qpair != NULL);
	CU_ASSERT(ctrlr_ch2->qpair != NULL);

	poll_thread_times(0, 3);
	CU_ASSERT(ctrlr_ch1->qpair->qpair == NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair != NULL);

	poll_thread_times(0, 1);
	poll_thread_times(1, 1);
	CU_ASSERT(ctrlr_ch1->qpair->qpair == NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair == NULL);
	CU_ASSERT(ctrlr.is_failed == true);

	poll_thread_times(1, 1);
	poll_thread_times(0, 1);
	CU_ASSERT(ctrlr.is_failed == false);
	CU_ASSERT(ctrlr.adminq.is_connected == false);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(0, 2);
	CU_ASSERT(ctrlr.adminq.is_connected == true);

	poll_thread_times(0, 1);
	CU_ASSERT(ctrlr_ch1->qpair->qpair != NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair == NULL);

	poll_thread_times(1, 1);
	CU_ASSERT(ctrlr_ch1->qpair->qpair != NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair != NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(curr_trid->is_failed == true);

	poll_thread_times(0, 2);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	poll_thread_times(1, 1);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(curr_trid->is_failed == false);

	/* Case 4: ctrlr is already removed. */
	ctrlr.is_removed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	detect_remove = false;
	nvme_ctrlr->reset_cb_fn = ut_check_hotplug_on_reset;
	nvme_ctrlr->reset_cb_arg = &detect_remove;

	poll_threads();

	CU_ASSERT(nvme_ctrlr->reset_cb_fn == NULL);
	CU_ASSERT(nvme_ctrlr->reset_cb_arg == NULL);
	CU_ASSERT(detect_remove == true);

	ctrlr.is_removed = false;

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_race_between_reset_and_destruct_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_io_channel *ch1, *ch2;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	/* Reset starts from thread 1. */
	set_thread(1);

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	/* Try destructing ctrlr while ctrlr is being reset, but it will be deferred. */
	set_thread(0);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);
	CU_ASSERT(nvme_ctrlr->destruct == true);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	/* Reset completed but ctrlr is not still destructed yet. */
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);
	CU_ASSERT(nvme_ctrlr->destruct == true);
	CU_ASSERT(nvme_ctrlr->resetting == false);

	/* New reset request is rejected. */
	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == -ENXIO);

	/* Additional polling called spdk_io_device_unregister() to ctrlr,
	 * However there are two channels and destruct is not completed yet.
	 */
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);

	set_thread(0);

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_failover_ctrlr(void)
{
	struct spdk_nvme_transport_id trid1 = {}, trid2 = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	struct nvme_path_id *curr_trid, *next_trid;
	struct spdk_io_channel *ch1, *ch2;
	int rc;

	ut_init_trid(&trid1);
	ut_init_trid2(&trid2);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid1, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	/* First, test one trid case. */
	curr_trid = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	/* Failover starts from thread 1. */
	set_thread(1);

	/* Case 1: ctrlr is already being destructed. */
	nvme_ctrlr->destruct = true;

	rc = bdev_nvme_failover(nvme_ctrlr, false);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(curr_trid->is_failed == false);

	/* Case 2: reset is in progress. */
	nvme_ctrlr->destruct = false;
	nvme_ctrlr->resetting = true;

	rc = bdev_nvme_failover(nvme_ctrlr, false);
	CU_ASSERT(rc == -EBUSY);

	/* Case 3: reset completes successfully. */
	nvme_ctrlr->resetting = false;

	rc = bdev_nvme_failover(nvme_ctrlr, false);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(curr_trid->is_failed == true);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	curr_trid = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(curr_trid->is_failed == false);

	set_thread(0);

	/* Second, test two trids case. */
	rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, &ctrlr, &trid2);
	CU_ASSERT(rc == 0);

	curr_trid = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(curr_trid != NULL);
	CU_ASSERT(curr_trid == nvme_ctrlr->active_path_id);
	CU_ASSERT(spdk_nvme_transport_id_compare(&curr_trid->trid, &trid1) == 0);

	/* Failover starts from thread 1. */
	set_thread(1);

	/* Case 4: reset is in progress. */
	nvme_ctrlr->resetting = true;

	rc = bdev_nvme_failover(nvme_ctrlr, false);
	CU_ASSERT(rc == -EBUSY);

	/* Case 5: failover completes successfully. */
	nvme_ctrlr->resetting = false;

	rc = bdev_nvme_failover(nvme_ctrlr, false);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_ctrlr->resetting == true);

	next_trid = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(next_trid != NULL);
	CU_ASSERT(next_trid != curr_trid);
	CU_ASSERT(next_trid == nvme_ctrlr->active_path_id);
	CU_ASSERT(spdk_nvme_transport_id_compare(&next_trid->trid, &trid2) == 0);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

/* We had a bug when running test/nvmf/host/multipath.sh. The bug was the following.
 *
 * A nvme_ctrlr had trid1 and trid2 first. trid1 was active. A connection to trid1 was
 * disconnected and reset ctrlr failed repeatedly before starting failover from trid1
 * to trid2. While processing the failed reset, trid3 was added. trid1 should
 * have been active, i.e., the head of the list until the failover completed.
 * However trid3 was inserted to the head of the list by mistake.
 *
 * I/O qpairs have smaller polling period than admin qpair. When a connection is
 * detected, I/O qpair may detect the error earlier than admin qpair. I/O qpair error
 * invokes reset ctrlr and admin qpair error invokes failover ctrlr. Hence reset ctrlr
 * may be executed repeatedly before failover is executed. Hence this bug is real.
 *
 * The following test verifies the fix.
 */
static void
test_race_between_failover_and_add_secondary_trid(void)
{
	struct spdk_nvme_transport_id trid1 = {}, trid2 = {}, trid3 = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	struct nvme_path_id *path_id1, *path_id2, *path_id3;
	struct spdk_io_channel *ch1, *ch2;
	int rc;

	ut_init_trid(&trid1);
	ut_init_trid2(&trid2);
	ut_init_trid3(&trid3);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid1, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	ch1 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	set_thread(0);

	rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, &ctrlr, &trid2);
	CU_ASSERT(rc == 0);

	path_id1 = TAILQ_FIRST(&nvme_ctrlr->trids);
	SPDK_CU_ASSERT_FATAL(path_id1 != NULL);
	CU_ASSERT(path_id1 == nvme_ctrlr->active_path_id);
	CU_ASSERT(spdk_nvme_transport_id_compare(&path_id1->trid, &trid1) == 0);
	path_id2 = TAILQ_NEXT(path_id1, link);
	SPDK_CU_ASSERT_FATAL(path_id2 != NULL);
	CU_ASSERT(spdk_nvme_transport_id_compare(&path_id2->trid, &trid2) == 0);

	ctrlr.fail_reset = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	poll_threads();

	CU_ASSERT(path_id1->is_failed == true);
	CU_ASSERT(path_id1 == nvme_ctrlr->active_path_id);

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, &ctrlr, &trid3);
	CU_ASSERT(rc == 0);

	CU_ASSERT(path_id1 == TAILQ_FIRST(&nvme_ctrlr->trids));
	CU_ASSERT(path_id1 == nvme_ctrlr->active_path_id);
	CU_ASSERT(spdk_nvme_transport_id_compare(&path_id1->trid, &trid1) == 0);
	CU_ASSERT(path_id2 == TAILQ_NEXT(path_id1, link));
	CU_ASSERT(spdk_nvme_transport_id_compare(&path_id2->trid, &trid2) == 0);
	path_id3 = TAILQ_NEXT(path_id2, link);
	SPDK_CU_ASSERT_FATAL(path_id3 != NULL);
	CU_ASSERT(spdk_nvme_transport_id_compare(&path_id3->trid, &trid3) == 0);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	set_thread(0);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
attach_ctrlr_done(void *cb_ctx, size_t bdev_count, int rc)
{
	CU_ASSERT(rc == g_ut_attach_ctrlr_status);
	CU_ASSERT(bdev_count == g_ut_attach_bdev_count);
}

static void
test_pending_reset(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *first_bdev_io, *second_bdev_io;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_bdev_channel *nbdev_ch1, *nbdev_ch2;
	struct nvme_io_path *io_path1, *io_path2;
	struct nvme_ctrlr_channel *ctrlr_ch1, *ctrlr_ch2;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	bdev = nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev;
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	ch1 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	nbdev_ch1 = spdk_io_channel_get_ctx(ch1);
	io_path1 = STAILQ_FIRST(&nbdev_ch1->io_path_list);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);
	ctrlr_ch1 = io_path1->qpair->ctrlr_ch;
	SPDK_CU_ASSERT_FATAL(ctrlr_ch1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	nbdev_ch2 = spdk_io_channel_get_ctx(ch2);
	io_path2 = STAILQ_FIRST(&nbdev_ch2->io_path_list);
	SPDK_CU_ASSERT_FATAL(io_path2 != NULL);
	ctrlr_ch2 = io_path2->qpair->ctrlr_ch;
	SPDK_CU_ASSERT_FATAL(ctrlr_ch2 != NULL);

	first_bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_RESET, bdev, ch2);
	first_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;

	second_bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_RESET, bdev, ch1);
	second_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;

	/* The first reset request is submitted on thread 1, and the second reset request
	 * is submitted on thread 0 while processing the first request.
	 */
	bdev_nvme_submit_request(ch2, first_bdev_io);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(TAILQ_EMPTY(&ctrlr_ch2->pending_resets));

	set_thread(0);

	bdev_nvme_submit_request(ch1, second_bdev_io);
	CU_ASSERT(TAILQ_FIRST(&ctrlr_ch1->pending_resets) == second_bdev_io);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(first_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(second_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* The first reset request is submitted on thread 1, and the second reset request
	 * is submitted on thread 0 while processing the first request.
	 *
	 * The difference from the above scenario is that the controller is removed while
	 * processing the first request. Hence both reset requests should fail.
	 */
	set_thread(1);

	bdev_nvme_submit_request(ch2, first_bdev_io);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(TAILQ_EMPTY(&ctrlr_ch2->pending_resets));

	set_thread(0);

	bdev_nvme_submit_request(ch1, second_bdev_io);
	CU_ASSERT(TAILQ_FIRST(&ctrlr_ch1->pending_resets) == second_bdev_io);

	ctrlr->fail_reset = true;

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(first_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(second_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	set_thread(0);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	free(first_bdev_io);
	free(second_bdev_io);
}

static void
test_attach_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *nbdev;
	int rc;

	set_thread(0);

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	/* If ctrlr fails, no nvme_ctrlr is created. Failed ctrlr is removed
	 * by probe polling.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->is_failed = true;
	g_ut_attach_ctrlr_status = -EIO;
	g_ut_attach_bdev_count = 0;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	/* If ctrlr has no namespace, one nvme_ctrlr with no namespace is created */
	ctrlr = ut_attach_ctrlr(&trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);
	CU_ASSERT(nvme_ctrlr->ctrlr == ctrlr);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	/* If ctrlr has one namespace, one nvme_ctrlr with one namespace and
	 * one nvme_bdev is created.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);
	CU_ASSERT(nvme_ctrlr->ctrlr == ctrlr);

	CU_ASSERT(attached_names[0] != NULL && strcmp(attached_names[0], "nvme0n1") == 0);
	attached_names[0] = NULL;

	nbdev = nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev;
	SPDK_CU_ASSERT_FATAL(nbdev != NULL);
	CU_ASSERT(bdev_nvme_get_ctrlr(&nbdev->disk) == ctrlr);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	/* Ctrlr has one namespace but one nvme_ctrlr with no namespace is
	 * created because creating one nvme_bdev failed.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_register_bdev_status = -EINVAL;
	g_ut_attach_bdev_count = 0;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);
	CU_ASSERT(nvme_ctrlr->ctrlr == ctrlr);

	CU_ASSERT(attached_names[0] == NULL);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	g_ut_register_bdev_status = 0;
}

static void
test_aer_cb(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_bdev *bdev;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	union spdk_nvme_async_event_completion event = {};
	struct spdk_nvme_cpl cpl = {};
	int rc;

	set_thread(0);

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	/* Attach a ctrlr, whose max number of namespaces is 4, and 2nd, 3rd, and 4th
	 * namespaces are populated.
	 */
	ctrlr = ut_attach_ctrlr(&trid, 4, true, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].is_active = false;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 3;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1) == NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 3) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4) != NULL);

	bdev = nvme_ctrlr_get_ns(nvme_ctrlr, 4)->bdev;
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

	aer_cb(nvme_ctrlr, &cpl);

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 3) == NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4) != NULL);
	CU_ASSERT(bdev->disk.blockcnt == 2048);

	/* Change ANA state of active namespaces. */
	ctrlr->ns[0].ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	ctrlr->ns[1].ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	ctrlr->ns[3].ana_state = SPDK_NVME_ANA_CHANGE_STATE;

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_ANA_CHANGE;
	cpl.cdw0 = event.raw;

	aer_cb(nvme_ctrlr, &cpl);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1)->ana_state == SPDK_NVME_ANA_NON_OPTIMIZED_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2)->ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4)->ana_state == SPDK_NVME_ANA_CHANGE_STATE);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
ut_test_submit_nvme_cmd(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			enum spdk_bdev_io_type io_type)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_io_path *io_path;
	struct spdk_nvme_qpair *qpair;

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);
	qpair = io_path->qpair->qpair;
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	bdev_io->type = io_type;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(qpair->num_outstanding_reqs == 1);

	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(qpair->num_outstanding_reqs == 0);
}

static void
ut_test_submit_nop(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		   enum spdk_bdev_io_type io_type)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_io_path *io_path;
	struct spdk_nvme_qpair *qpair;

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);
	qpair = io_path->qpair->qpair;
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	bdev_io->type = io_type;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(qpair->num_outstanding_reqs == 0);
}

static void
ut_test_submit_fused_nvme_cmd(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_bdev_io *bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	struct ut_nvme_req *req;
	struct nvme_io_path *io_path;
	struct spdk_nvme_qpair *qpair;

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);
	qpair = io_path->qpair->qpair;
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	/* Only compare and write now. */
	bdev_io->type = SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(qpair->num_outstanding_reqs == 2);
	CU_ASSERT(bio->first_fused_submitted == true);

	/* First outstanding request is compare operation. */
	req = TAILQ_FIRST(&qpair->outstanding_reqs);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	CU_ASSERT(req->opc == SPDK_NVME_OPC_COMPARE);
	req->cpl.cdw0 = SPDK_NVME_OPC_COMPARE;

	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(qpair->num_outstanding_reqs == 0);
}

static void
ut_test_submit_admin_cmd(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			 struct spdk_nvme_ctrlr *ctrlr)
{
	bdev_io->type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io->internal.in_submit_request = true;
	bdev_io->u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_GET_FEATURES;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 1);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(1, 1);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);

	poll_thread_times(0, 1);

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
}

static void
test_submit_nvme_cmd(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_io_channel *ch;
	struct spdk_bdev_ext_io_opts ext_io_opts = {};
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	set_thread(1);

	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	bdev = nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev;
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	set_thread(0);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_INVALID, bdev, ch);

	bdev_io->u.bdev.iovs = NULL;

	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_READ);

	ut_bdev_io_set_buf(bdev_io);

	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_READ);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_WRITE);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_COMPARE);
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_UNMAP);

	ut_test_submit_nop(ch, bdev_io, SPDK_BDEV_IO_TYPE_FLUSH);

	ut_test_submit_fused_nvme_cmd(ch, bdev_io);

	/* Verify that ext NVME API is called if bdev_io ext_opts is set */
	bdev_io->u.bdev.ext_opts = &ext_io_opts;
	g_ut_readv_ext_called = false;
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(g_ut_readv_ext_called == true);
	g_ut_readv_ext_called = false;

	g_ut_writev_ext_called = false;
	ut_test_submit_nvme_cmd(ch, bdev_io, SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(g_ut_writev_ext_called == true);
	g_ut_writev_ext_called = false;
	bdev_io->u.bdev.ext_opts = NULL;

	ut_test_submit_admin_cmd(ch, bdev_io, ctrlr);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	set_thread(1);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_add_remove_trid(void)
{
	struct nvme_path_id path1 = {}, path2 = {}, path3 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2, *ctrlr3;
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_path_id *ctrid;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	ut_init_trid3(&path3.trid);

	set_thread(0);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 0;

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path1.trid) == 0);

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path1.trid) == 0);
	TAILQ_FOREACH(ctrid, &nvme_ctrlr->trids, link) {
		if (spdk_nvme_transport_id_compare(&ctrid->trid, &path2.trid) == 0) {
			break;
		}
	}
	CU_ASSERT(ctrid != NULL);

	/* trid3 is not in the registered list. */
	rc = bdev_nvme_delete("nvme0", &path3);
	CU_ASSERT(rc == -ENXIO);

	/* trid2 is not used, and simply removed. */
	rc = bdev_nvme_delete("nvme0", &path2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);
	TAILQ_FOREACH(ctrid, &nvme_ctrlr->trids, link) {
		CU_ASSERT(spdk_nvme_transport_id_compare(&ctrid->trid, &path2.trid) != 0);
	}

	ctrlr3 = ut_attach_ctrlr(&path3.trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr3 != NULL);

	rc = bdev_nvme_create(&path3.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path1.trid) == 0);
	TAILQ_FOREACH(ctrid, &nvme_ctrlr->trids, link) {
		if (spdk_nvme_transport_id_compare(&ctrid->trid, &path3.trid) == 0) {
			break;
		}
	}
	CU_ASSERT(ctrid != NULL);

	/* path1 is currently used and path3 is an alternative path.
	 * If we remove path1, path is changed to path3.
	 */
	rc = bdev_nvme_delete("nvme0", &path1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	TAILQ_FOREACH(ctrid, &nvme_ctrlr->trids, link) {
		CU_ASSERT(spdk_nvme_transport_id_compare(&ctrid->trid, &path1.trid) != 0);
	}
	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path3.trid) == 0);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);

	/* path3 is the current and only path. If we remove path3, the corresponding
	 * nvme_ctrlr is removed.
	 */
	rc = bdev_nvme_delete("nvme0", &path3);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path1.trid) == 0);

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 0, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(spdk_nvme_transport_id_compare(&nvme_ctrlr->active_path_id->trid, &path1.trid) == 0);
	TAILQ_FOREACH(ctrid, &nvme_ctrlr->trids, link) {
		if (spdk_nvme_transport_id_compare(&ctrid->trid, &path2.trid) == 0) {
			break;
		}
	}
	CU_ASSERT(ctrid != NULL);

	/* If trid is not specified, nvme_ctrlr itself is removed. */
	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == nvme_ctrlr);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_abort(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *write_io, *fuse_io, *admin_io, *abort_io;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_bdev_channel *nbdev_ch1;
	struct nvme_io_path *io_path1;
	struct nvme_qpair *nvme_qpair1;
	int rc;

	/* Create ctrlr on thread 1 and submit I/O and admin requests to be aborted on
	 * thread 0. Aborting I/O requests are submitted on thread 0. Aborting admin requests
	 * are submitted on thread 1. Both should succeed.
	 */

	ut_init_trid(&trid);

	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(1);

	opts.ctrlr_loss_timeout_sec = -1;
	opts.reconnect_delay_sec = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	bdev = nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev;
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	write_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(write_io);

	fuse_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(fuse_io);

	admin_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_NVME_ADMIN, bdev, NULL);
	admin_io->u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_GET_FEATURES;

	abort_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_ABORT, bdev, NULL);

	set_thread(0);

	ch1 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);
	nbdev_ch1 = spdk_io_channel_get_ctx(ch1);
	io_path1 = STAILQ_FIRST(&nbdev_ch1->io_path_list);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);
	nvme_qpair1 = io_path1->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	write_io->internal.ch = (struct spdk_bdev_channel *)ch1;
	fuse_io->internal.ch = (struct spdk_bdev_channel *)ch1;
	abort_io->internal.ch = (struct spdk_bdev_channel *)ch1;

	/* Aborting the already completed request should fail. */
	write_io->internal.in_submit_request = true;
	bdev_nvme_submit_request(ch1, write_io);
	poll_threads();

	CU_ASSERT(write_io->internal.in_submit_request == false);

	abort_io->u.abort.bio_to_abort = write_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch1, abort_io);

	poll_threads();

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);

	admin_io->internal.ch = (struct spdk_bdev_channel *)ch1;
	abort_io->internal.ch = (struct spdk_bdev_channel *)ch2;

	admin_io->internal.in_submit_request = true;
	bdev_nvme_submit_request(ch1, admin_io);
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(admin_io->internal.in_submit_request == false);

	abort_io->u.abort.bio_to_abort = admin_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch2, abort_io);

	poll_threads();

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);

	/* Aborting the write request should succeed. */
	write_io->internal.in_submit_request = true;
	bdev_nvme_submit_request(ch1, write_io);

	CU_ASSERT(write_io->internal.in_submit_request == true);
	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 1);

	abort_io->internal.ch = (struct spdk_bdev_channel *)ch1;
	abort_io->u.abort.bio_to_abort = write_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch1, abort_io);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(write_io->internal.in_submit_request == false);
	CU_ASSERT(write_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);

	/* Aborting the fuse request should succeed. */
	fuse_io->internal.in_submit_request = true;
	bdev_nvme_submit_request(ch1, fuse_io);

	CU_ASSERT(fuse_io->internal.in_submit_request == true);
	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 2);

	abort_io->u.abort.bio_to_abort = fuse_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch1, abort_io);

	spdk_delay_us(10000);
	poll_threads();

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(fuse_io->internal.in_submit_request == false);
	CU_ASSERT(fuse_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);

	/* Aborting the admin request should succeed. */
	admin_io->internal.in_submit_request = true;
	bdev_nvme_submit_request(ch1, admin_io);

	CU_ASSERT(admin_io->internal.in_submit_request == true);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 1);

	abort_io->internal.ch = (struct spdk_bdev_channel *)ch2;
	abort_io->u.abort.bio_to_abort = admin_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch2, abort_io);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(admin_io->internal.in_submit_request == false);
	CU_ASSERT(admin_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);

	set_thread(0);

	/* If qpair is disconnected, it is freed and then reconnected via resetting
	 * the corresponding nvme_ctrlr. I/O should be queued if it is submitted
	 * while resetting the nvme_ctrlr.
	 */
	nvme_qpair1->qpair->failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;

	poll_thread_times(0, 3);

	CU_ASSERT(nvme_qpair1->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	write_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch1, write_io);

	CU_ASSERT(write_io->internal.in_submit_request == true);
	CU_ASSERT(write_io == TAILQ_FIRST(&nbdev_ch1->retry_io_list));

	/* Aborting the queued write request should succeed immediately. */
	abort_io->internal.ch = (struct spdk_bdev_channel *)ch1;
	abort_io->u.abort.bio_to_abort = write_io;
	abort_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch1, abort_io);

	CU_ASSERT(abort_io->internal.in_submit_request == false);
	CU_ASSERT(abort_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(write_io->internal.in_submit_request == false);
	CU_ASSERT(write_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	free(write_io);
	free(fuse_io);
	free(admin_io);
	free(abort_io);

	set_thread(1);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_get_io_qpair(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	struct spdk_io_channel *ch;
	struct nvme_ctrlr_channel *ctrlr_ch;
	struct spdk_nvme_qpair *qpair;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	ch = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	ctrlr_ch = spdk_io_channel_get_ctx(ch);
	CU_ASSERT(ctrlr_ch->qpair->qpair != NULL);

	qpair = bdev_nvme_get_io_qpair(ch);
	CU_ASSERT(qpair == ctrlr_ch->qpair->qpair);

	spdk_put_io_channel(ch);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

/* Test a scenario that the bdev subsystem starts shutdown when there still exists
 * any NVMe bdev. In this scenario, spdk_bdev_unregister() is called first. Add a
 * test case to avoid regression for this scenario. spdk_bdev_unregister() calls
 * bdev_nvme_destruct() in the end, and so call bdev_nvme_destruct() directly.
 */
static void
test_bdev_unregister(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_ns *nvme_ns1, *nvme_ns2;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev1, *bdev2;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	ctrlr = ut_attach_ctrlr(&trid, 2, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 2;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	nvme_ns1 = nvme_ctrlr_get_ns(nvme_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(nvme_ns1 != NULL);

	bdev1 = nvme_ns1->bdev;
	SPDK_CU_ASSERT_FATAL(bdev1 != NULL);

	nvme_ns2 = nvme_ctrlr_get_ns(nvme_ctrlr, 2);
	SPDK_CU_ASSERT_FATAL(nvme_ns2 != NULL);

	bdev2 = nvme_ns2->bdev;
	SPDK_CU_ASSERT_FATAL(bdev2 != NULL);

	bdev_nvme_destruct(&bdev1->disk);
	bdev_nvme_destruct(&bdev2->disk);

	poll_threads();

	CU_ASSERT(nvme_ns1->bdev == NULL);
	CU_ASSERT(nvme_ns2->bdev == NULL);

	nvme_ctrlr->destruct = true;
	_nvme_ctrlr_destruct(nvme_ctrlr);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_compare_ns(void)
{
	struct spdk_nvme_ns_data nsdata1 = {}, nsdata2 = {};
	struct spdk_nvme_ctrlr ctrlr1 = { .nsdata = &nsdata1, }, ctrlr2 = { .nsdata = &nsdata2, };
	struct spdk_nvme_ns ns1 = { .id = 1, .ctrlr = &ctrlr1, }, ns2 = { .id = 1, .ctrlr = &ctrlr2, };
	struct spdk_uuid uuid1 = { .u.raw = { 0xAA } };
	struct spdk_uuid uuid2 = { .u.raw = { 0xAB } };

	/* No IDs are defined. */
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == true);

	/* Only EUI64 are defined and not matched. */
	nsdata1.eui64 = 0xABCDEF0123456789;
	nsdata2.eui64 = 0xBBCDEF0123456789;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == false);

	/* Only EUI64 are defined and matched. */
	nsdata2.eui64 = 0xABCDEF0123456789;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == true);

	/* Only NGUID are defined and not matched. */
	nsdata1.eui64 = 0x0;
	nsdata2.eui64 = 0x0;
	nsdata1.nguid[0] = 0x12;
	nsdata2.nguid[0] = 0x10;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == false);

	/* Only NGUID are defined and matched. */
	nsdata2.nguid[0] = 0x12;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == true);

	/* Only UUID are defined and not matched. */
	nsdata1.nguid[0] = 0x0;
	nsdata2.nguid[0] = 0x0;
	ns1.uuid = &uuid1;
	ns2.uuid = &uuid2;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == false);

	/* Only one UUID is defined. */
	ns1.uuid = NULL;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == false);

	/* Only UUID are defined and matched. */
	ns1.uuid = &uuid2;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == true);

	/* All EUI64, NGUID, and UUID are defined and matched. */
	nsdata1.eui64 = 0x123456789ABCDEF;
	nsdata2.eui64 = 0x123456789ABCDEF;
	nsdata1.nguid[15] = 0x34;
	nsdata2.nguid[15] = 0x34;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == true);

	/* CSI are not matched. */
	ns1.csi = SPDK_NVME_CSI_ZNS;
	CU_ASSERT(bdev_nvme_compare_ns(&ns1, &ns2) == false);
}

static void
test_init_ana_log_page(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	int rc;

	set_thread(0);

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	ctrlr = ut_attach_ctrlr(&trid, 5, true, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	ctrlr->ns[1].ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	ctrlr->ns[2].ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	ctrlr->ns[3].ana_state = SPDK_NVME_ANA_PERSISTENT_LOSS_STATE;
	ctrlr->ns[4].ana_state = SPDK_NVME_ANA_CHANGE_STATE;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 5;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 3) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 5) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1)->ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2)->ana_state == SPDK_NVME_ANA_NON_OPTIMIZED_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 3)->ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4)->ana_state == SPDK_NVME_ANA_PERSISTENT_LOSS_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 5)->ana_state == SPDK_NVME_ANA_CHANGE_STATE);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 2)->bdev != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 3)->bdev != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 4)->bdev != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr, 5)->bdev != NULL);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
init_accel(void)
{
	spdk_io_device_register(g_accel_p, accel_channel_create, accel_channel_destroy,
				sizeof(int), "accel_p");
}

static void
fini_accel(void)
{
	spdk_io_device_unregister(g_accel_p, NULL);
}

static void
test_get_memory_domains(void)
{
	struct nvme_ctrlr ctrlr_1 = { .ctrlr = (struct spdk_nvme_ctrlr *) 0xbaadbeef };
	struct nvme_ctrlr ctrlr_2 = { .ctrlr = (struct spdk_nvme_ctrlr *) 0xbaaadbeeef };
	struct nvme_ns ns_1 = { .ctrlr = &ctrlr_1 };
	struct nvme_ns ns_2 = { .ctrlr = &ctrlr_2 };
	struct nvme_bdev nbdev = { .nvme_ns_list = TAILQ_HEAD_INITIALIZER(nbdev.nvme_ns_list) };
	struct spdk_memory_domain *domains[4] = {};
	int rc = 0;

	TAILQ_INSERT_TAIL(&nbdev.nvme_ns_list, &ns_1, tailq);

	/* nvme controller doesn't have memory domains */
	MOCK_SET(spdk_nvme_ctrlr_get_memory_domains, 0);
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(domains[0] == NULL);
	CU_ASSERT(domains[1] == NULL);

	/* nvme controller has a memory domain */
	MOCK_SET(spdk_nvme_ctrlr_get_memory_domains, 1);
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 2);
	CU_ASSERT(rc == 1);
	CU_ASSERT(domains[0] != NULL);
	memset(domains, 0, sizeof(domains));

	/* multipath, 2 controllers report 1 memory domain each */
	TAILQ_INSERT_TAIL(&nbdev.nvme_ns_list, &ns_2, tailq);

	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 2);
	CU_ASSERT(rc == 2);
	CU_ASSERT(domains[0] != NULL);
	CU_ASSERT(domains[1] != NULL);
	memset(domains, 0, sizeof(domains));

	/* multipath, 2 controllers report 1 memory domain each, NULL domains ptr */
	rc = bdev_nvme_get_memory_domains(&nbdev, NULL, 2);
	CU_ASSERT(rc == 2);

	/* multipath, 2 controllers report 1 memory domain each, array_size = 0 */
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 0);
	CU_ASSERT(rc == 2);
	CU_ASSERT(domains[0] == NULL);
	CU_ASSERT(domains[1] == NULL);

	/* multipath, 2 controllers report 1 memory domain each, array_size = 1 */
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 1);
	CU_ASSERT(rc == 2);
	CU_ASSERT(domains[0] != NULL);
	CU_ASSERT(domains[1] == NULL);
	memset(domains, 0, sizeof(domains));

	/* multipath, 2 controllers report 2 memory domain each (not possible, just for test) */
	MOCK_SET(spdk_nvme_ctrlr_get_memory_domains, 2);
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 4);
	CU_ASSERT(rc == 4);
	CU_ASSERT(domains[0] != NULL);
	CU_ASSERT(domains[1] != NULL);
	CU_ASSERT(domains[2] != NULL);
	CU_ASSERT(domains[3] != NULL);
	memset(domains, 0, sizeof(domains));

	/* multipath, 2 controllers report 2 memory domain each (not possible, just for test)
	 * Array size is less than the number of memory domains */
	MOCK_SET(spdk_nvme_ctrlr_get_memory_domains, 2);
	rc = bdev_nvme_get_memory_domains(&nbdev, domains, 3);
	CU_ASSERT(rc == 4);
	CU_ASSERT(domains[0] != NULL);
	CU_ASSERT(domains[1] != NULL);
	CU_ASSERT(domains[2] != NULL);
	CU_ASSERT(domains[3] == NULL);
	memset(domains, 0, sizeof(domains));

	MOCK_CLEAR(spdk_nvme_ctrlr_get_memory_domains);
}

static void
test_reconnect_qpair(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_bdev_channel *nbdev_ch1, *nbdev_ch2;
	struct nvme_io_path *io_path1, *io_path2;
	struct nvme_qpair *nvme_qpair1, *nvme_qpair2;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	bdev = nvme_ctrlr_get_ns(nvme_ctrlr, 1)->bdev;
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	ch1 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	nbdev_ch1 = spdk_io_channel_get_ctx(ch1);
	io_path1 = STAILQ_FIRST(&nbdev_ch1->io_path_list);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);
	nvme_qpair1 = io_path1->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair1 != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	nbdev_ch2 = spdk_io_channel_get_ctx(ch2);
	io_path2 = STAILQ_FIRST(&nbdev_ch2->io_path_list);
	SPDK_CU_ASSERT_FATAL(io_path2 != NULL);
	nvme_qpair2 = io_path2->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair2 != NULL);

	/* If a qpair is disconnected, it is freed and then reconnected via
	 * resetting the corresponding nvme_ctrlr.
	 */
	nvme_qpair2->qpair->failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	ctrlr->is_failed = true;

	poll_thread_times(1, 3);
	CU_ASSERT(nvme_qpair1->qpair != NULL);
	CU_ASSERT(nvme_qpair2->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	poll_thread_times(0, 3);
	CU_ASSERT(nvme_qpair1->qpair == NULL);
	CU_ASSERT(nvme_qpair2->qpair == NULL);
	CU_ASSERT(ctrlr->is_failed == true);

	poll_thread_times(1, 2);
	poll_thread_times(0, 1);
	CU_ASSERT(ctrlr->is_failed == false);
	CU_ASSERT(ctrlr->adminq.is_connected == false);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(0, 2);
	CU_ASSERT(ctrlr->adminq.is_connected == true);

	poll_thread_times(0, 1);
	poll_thread_times(1, 1);
	CU_ASSERT(nvme_qpair1->qpair != NULL);
	CU_ASSERT(nvme_qpair2->qpair != NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	poll_thread_times(0, 2);
	poll_thread_times(1, 1);
	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ctrlr->resetting == false);

	poll_threads();

	/* If a qpair is disconnected and resetting the corresponding nvme_ctrlr
	 * fails, the qpair is just freed.
	 */
	nvme_qpair2->qpair->failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	ctrlr->is_failed = true;
	ctrlr->fail_reset = true;

	poll_thread_times(1, 3);
	CU_ASSERT(nvme_qpair1->qpair != NULL);
	CU_ASSERT(nvme_qpair2->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);

	poll_thread_times(0, 3);
	poll_thread_times(1, 1);
	CU_ASSERT(nvme_qpair1->qpair == NULL);
	CU_ASSERT(nvme_qpair2->qpair == NULL);
	CU_ASSERT(ctrlr->is_failed == true);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(0, 3);
	poll_thread_times(1, 1);
	poll_thread_times(0, 1);
	CU_ASSERT(ctrlr->is_failed == true);
	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(nvme_qpair1->qpair == NULL);
	CU_ASSERT(nvme_qpair2->qpair == NULL);

	poll_threads();

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_create_bdev_ctrlr(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 0, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 0;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) != NULL);

	/* cntlid is duplicated, and adding the second ctrlr should fail. */
	g_ut_attach_ctrlr_status = -EINVAL;

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 0, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->cdata.cntlid = ctrlr1->cdata.cntlid;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) == NULL);

	/* cntlid is not duplicated, and adding the third ctrlr should succeed. */
	g_ut_attach_ctrlr_status = 0;

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 0, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) != NULL);

	/* Delete two ctrlrs at once. */
	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nbdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) != NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) != NULL);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	/* Add two ctrlrs and delete one by one. */
	ctrlr1 = ut_attach_ctrlr(&path1.trid, 0, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 0, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	rc = bdev_nvme_delete("nvme0", &path1);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nbdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) != NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) != NULL);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nbdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) == NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) != NULL);

	rc = bdev_nvme_delete("nvme0", &path2);
	CU_ASSERT(rc == 0);

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nbdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) == NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) != NULL);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static struct nvme_ns *
_nvme_bdev_get_ns(struct nvme_bdev *bdev, struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_ns *nvme_ns;

	TAILQ_FOREACH(nvme_ns, &bdev->nvme_ns_list, tailq) {
		if (nvme_ns->ctrlr == nvme_ctrlr) {
			return nvme_ns;
		}
	}

	return NULL;
}

static void
test_add_multi_ns_to_bdev(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_ctrlr *nvme_ctrlr1, *nvme_ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ns *nvme_ns1, *nvme_ns2;
	struct nvme_bdev *bdev1, *bdev2, *bdev3, *bdev4;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	struct spdk_uuid uuid2 = { .u.raw = { 0x2 } };
	struct spdk_uuid uuid3 = { .u.raw = { 0x3 } };
	struct spdk_uuid uuid4 = { .u.raw = { 0x4 } };
	struct spdk_uuid uuid44 = { .u.raw = { 0x44 } };
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);

	/* Create nvme_bdevs, some of which have shared namespaces between two ctrlrs. */

	/* Attach 1st ctrlr, whose max number of namespaces is 5, and 1st, 3rd, and 4th
	 * namespaces are populated.
	 */
	ctrlr1 = ut_attach_ctrlr(&path1.trid, 5, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[1].is_active = false;
	ctrlr1->ns[4].is_active = false;
	ctrlr1->ns[0].uuid = &uuid1;
	ctrlr1->ns[2].uuid = &uuid3;
	ctrlr1->ns[3].uuid = &uuid4;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 3;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, 32,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	/* Attach 2nd ctrlr, whose max number of namespaces is 5, and 1st, 2nd, and 4th
	 * namespaces are populated. The uuid of 4th namespace is different, and hence
	 * adding 4th namespace to a bdev should fail.
	 */
	ctrlr2 = ut_attach_ctrlr(&path2.trid, 5, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[2].is_active = false;
	ctrlr2->ns[4].is_active = false;
	ctrlr2->ns[0].uuid = &uuid1;
	ctrlr2->ns[1].uuid = &uuid2;
	ctrlr2->ns[3].uuid = &uuid44;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 2;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, 32,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr1, 1) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr1, 2) == NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr1, 3) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr1, 4) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr1, 5) == NULL);

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr2 != NULL);

	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr2, 1) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr2, 2) != NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr2, 3) == NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr2, 4) == NULL);
	CU_ASSERT(nvme_ctrlr_get_ns(nvme_ctrlr2, 5) == NULL);

	bdev1 = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev1 != NULL);
	bdev2 = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 2);
	SPDK_CU_ASSERT_FATAL(bdev2 != NULL);
	bdev3 = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(bdev3 != NULL);
	bdev4 = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 4);
	SPDK_CU_ASSERT_FATAL(bdev4 != NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 5) == NULL);

	CU_ASSERT(bdev1->ref == 2);
	CU_ASSERT(bdev2->ref == 1);
	CU_ASSERT(bdev3->ref == 1);
	CU_ASSERT(bdev4->ref == 1);

	/* Test if nvme_bdevs can be deleted by deleting ctrlr one by one. */
	rc = bdev_nvme_delete("nvme0", &path1);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == nbdev_ctrlr);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) == NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) == nvme_ctrlr2);

	rc = bdev_nvme_delete("nvme0", &path2);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	/* Test if a nvme_bdev which has a shared namespace between two ctrlrs
	 * can be deleted when the bdev subsystem shutdown.
	 */
	g_ut_attach_bdev_count = 1;

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, 32,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ut_init_trid2(&path2.trid);

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, 32,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	bdev1 = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev1 != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr2 != NULL);

	/* Check if a nvme_bdev has two nvme_ns. */
	nvme_ns1 = _nvme_bdev_get_ns(bdev1, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(nvme_ns1 != NULL);
	CU_ASSERT(nvme_ns1->bdev == bdev1);

	nvme_ns2 = _nvme_bdev_get_ns(bdev1, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(nvme_ns2 != NULL);
	CU_ASSERT(nvme_ns2->bdev == bdev1);

	/* Delete nvme_bdev first when the bdev subsystem shutdown. */
	bdev_nvme_destruct(&bdev1->disk);

	poll_threads();

	CU_ASSERT(nvme_ns1->bdev == NULL);
	CU_ASSERT(nvme_ns2->bdev == NULL);

	nvme_ctrlr1->destruct = true;
	_nvme_ctrlr_destruct(nvme_ctrlr1);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	nvme_ctrlr2->destruct = true;
	_nvme_ctrlr_destruct(nvme_ctrlr2);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_add_multi_io_paths_to_nbdev_ch(void)
{
	struct nvme_path_id path1 = {}, path2 = {}, path3 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2, *ctrlr3;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr1, *nvme_ctrlr2, *nvme_ctrlr3;
	struct nvme_ns *nvme_ns1, *nvme_ns2, *nvme_ns3;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path1, *io_path2, *io_path3;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	ut_init_trid3(&path3.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(1);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr2 != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	nvme_ns1 = _nvme_bdev_get_ns(bdev, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(nvme_ns1 != NULL);

	nvme_ns2 = _nvme_bdev_get_ns(bdev, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(nvme_ns2 != NULL);

	set_thread(0);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path1 = _bdev_nvme_get_io_path(nbdev_ch, nvme_ns1);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);

	io_path2 = _bdev_nvme_get_io_path(nbdev_ch, nvme_ns2);
	SPDK_CU_ASSERT_FATAL(io_path2 != NULL);

	set_thread(1);

	/* Check if I/O path is dynamically added to nvme_bdev_channel. */
	ctrlr3 = ut_attach_ctrlr(&path3.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr3 != NULL);

	ctrlr3->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path3.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nvme_ctrlr3 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path3.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr3 != NULL);

	nvme_ns3 = _nvme_bdev_get_ns(bdev, nvme_ctrlr3);
	SPDK_CU_ASSERT_FATAL(nvme_ns3 != NULL);

	io_path3 = _bdev_nvme_get_io_path(nbdev_ch, nvme_ns3);
	SPDK_CU_ASSERT_FATAL(io_path3 != NULL);

	/* Check if I/O path is dynamically deleted from nvme_bdev_channel. */
	rc = bdev_nvme_delete("nvme0", &path2);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid) == nvme_ctrlr1);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid) == NULL);
	CU_ASSERT(nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path3.trid) == nvme_ctrlr3);

	CU_ASSERT(_bdev_nvme_get_io_path(nbdev_ch, nvme_ns1) == io_path1);
	CU_ASSERT(_bdev_nvme_get_io_path(nbdev_ch, nvme_ns2) == NULL);
	CU_ASSERT(_bdev_nvme_get_io_path(nbdev_ch, nvme_ns3) == io_path3);

	set_thread(0);

	spdk_put_io_channel(ch);

	poll_threads();

	set_thread(1);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_admin_path(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_NVME_ADMIN, bdev, ch);
	bdev_io->u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_GET_FEATURES;

	/* ctrlr1 is failed but ctrlr2 is not failed. admin command is
	 * submitted to ctrlr2.
	 */
	ctrlr1->is_failed = true;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(ctrlr1->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(ctrlr2->adminq.num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(ctrlr2->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* both ctrlr1 and ctrlr2 are failed. admin command is failed to submit. */
	ctrlr2->is_failed = true;
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(ctrlr1->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(ctrlr2->adminq.num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static struct nvme_io_path *
ut_get_io_path_by_ctrlr(struct nvme_bdev_channel *nbdev_ch,
			struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_io_path *io_path;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (io_path->qpair->ctrlr == nvme_ctrlr) {
			return io_path;
		}
	}

	return NULL;
}

static void
test_reset_bdev_ctrlr(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr1, *nvme_ctrlr2;
	struct nvme_path_id *curr_path1, *curr_path2;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *first_bdev_io, *second_bdev_io;
	struct nvme_bdev_io *first_bio;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_bdev_channel *nbdev_ch1, *nbdev_ch2;
	struct nvme_io_path *io_path11, *io_path12, *io_path21, *io_path22;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	curr_path1 = TAILQ_FIRST(&nvme_ctrlr1->trids);
	SPDK_CU_ASSERT_FATAL(curr_path1 != NULL);

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr2 != NULL);

	curr_path2 = TAILQ_FIRST(&nvme_ctrlr2->trids);
	SPDK_CU_ASSERT_FATAL(curr_path2 != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	set_thread(0);

	ch1 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	nbdev_ch1 = spdk_io_channel_get_ctx(ch1);
	io_path11 = ut_get_io_path_by_ctrlr(nbdev_ch1, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(io_path11 != NULL);
	io_path12 = ut_get_io_path_by_ctrlr(nbdev_ch1, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(io_path12 != NULL);

	first_bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_RESET, bdev, ch1);
	first_bio = (struct nvme_bdev_io *)first_bdev_io->driver_ctx;

	set_thread(1);

	ch2 = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	nbdev_ch2 = spdk_io_channel_get_ctx(ch2);
	io_path21 = ut_get_io_path_by_ctrlr(nbdev_ch2, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(io_path21 != NULL);
	io_path22 = ut_get_io_path_by_ctrlr(nbdev_ch2, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(io_path22 != NULL);

	second_bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_RESET, bdev, ch2);

	/* The first reset request from bdev_io is submitted on thread 0.
	 * Check if ctrlr1 is reset and then ctrlr2 is reset.
	 *
	 * A few extra polls are necessary after resetting ctrlr1 to check
	 * pending reset requests for ctrlr1.
	 */
	ctrlr1->is_failed = true;
	curr_path1->is_failed = true;
	ctrlr2->is_failed = true;
	curr_path2->is_failed = true;

	set_thread(0);

	bdev_nvme_submit_request(ch1, first_bdev_io);
	CU_ASSERT(first_bio->io_path == io_path11);
	CU_ASSERT(nvme_ctrlr1->resetting == true);
	CU_ASSERT(nvme_ctrlr1->reset_cb_arg == first_bio);

	poll_thread_times(0, 3);
	CU_ASSERT(io_path11->qpair->qpair == NULL);
	CU_ASSERT(io_path21->qpair->qpair != NULL);

	poll_thread_times(1, 2);
	CU_ASSERT(io_path11->qpair->qpair == NULL);
	CU_ASSERT(io_path21->qpair->qpair == NULL);
	CU_ASSERT(ctrlr1->is_failed == true);

	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ctrlr1->resetting == true);
	CU_ASSERT(ctrlr1->is_failed == false);
	CU_ASSERT(ctrlr1->adminq.is_connected == false);
	CU_ASSERT(curr_path1->is_failed == true);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(0, 2);
	CU_ASSERT(ctrlr1->adminq.is_connected == true);

	poll_thread_times(0, 1);
	CU_ASSERT(io_path11->qpair->qpair != NULL);
	CU_ASSERT(io_path21->qpair->qpair == NULL);

	poll_thread_times(1, 1);
	CU_ASSERT(io_path11->qpair->qpair != NULL);
	CU_ASSERT(io_path21->qpair->qpair != NULL);

	poll_thread_times(0, 2);
	CU_ASSERT(nvme_ctrlr1->resetting == true);
	poll_thread_times(1, 1);
	CU_ASSERT(nvme_ctrlr1->resetting == true);
	poll_thread_times(0, 2);
	CU_ASSERT(nvme_ctrlr1->resetting == false);
	CU_ASSERT(curr_path1->is_failed == false);
	CU_ASSERT(first_bio->io_path == io_path12);
	CU_ASSERT(nvme_ctrlr2->resetting == true);

	poll_thread_times(0, 3);
	CU_ASSERT(io_path12->qpair->qpair == NULL);
	CU_ASSERT(io_path22->qpair->qpair != NULL);

	poll_thread_times(1, 2);
	CU_ASSERT(io_path12->qpair->qpair == NULL);
	CU_ASSERT(io_path22->qpair->qpair == NULL);
	CU_ASSERT(ctrlr2->is_failed == true);

	poll_thread_times(0, 1);
	CU_ASSERT(nvme_ctrlr2->resetting == true);
	CU_ASSERT(ctrlr2->is_failed == false);
	CU_ASSERT(ctrlr2->adminq.is_connected == false);
	CU_ASSERT(curr_path2->is_failed == true);

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_thread_times(0, 2);
	CU_ASSERT(ctrlr2->adminq.is_connected == true);

	poll_thread_times(0, 1);
	CU_ASSERT(io_path12->qpair->qpair != NULL);
	CU_ASSERT(io_path22->qpair->qpair == NULL);

	poll_thread_times(1, 2);
	CU_ASSERT(io_path12->qpair->qpair != NULL);
	CU_ASSERT(io_path22->qpair->qpair != NULL);

	poll_thread_times(0, 2);
	CU_ASSERT(nvme_ctrlr2->resetting == true);
	poll_thread_times(1, 1);
	CU_ASSERT(nvme_ctrlr2->resetting == true);
	poll_thread_times(0, 2);
	CU_ASSERT(first_bio->io_path == NULL);
	CU_ASSERT(nvme_ctrlr2->resetting == false);
	CU_ASSERT(curr_path2->is_failed == false);

	poll_threads();

	/* There is a race between two reset requests from bdev_io.
	 *
	 * The first reset request is submitted on thread 0, and the second reset
	 * request is submitted on thread 1 while the first is resetting ctrlr1.
	 * The second is pending on ctrlr1. After the first completes resetting ctrlr1,
	 * both reset requests go to ctrlr2. The first comes earlier than the second.
	 * The second is pending on ctrlr2 again. After the first completes resetting
	 * ctrl2, both complete successfully.
	 */
	ctrlr1->is_failed = true;
	curr_path1->is_failed = true;
	ctrlr2->is_failed = true;
	curr_path2->is_failed = true;
	first_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	second_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;

	set_thread(0);

	bdev_nvme_submit_request(ch1, first_bdev_io);

	set_thread(1);

	bdev_nvme_submit_request(ch2, second_bdev_io);

	CU_ASSERT(nvme_ctrlr1->resetting == true);
	CU_ASSERT(nvme_ctrlr1->reset_cb_arg == first_bio);
	CU_ASSERT(TAILQ_FIRST(&io_path21->qpair->ctrlr_ch->pending_resets) == second_bdev_io);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(ctrlr1->is_failed == false);
	CU_ASSERT(curr_path1->is_failed == false);
	CU_ASSERT(ctrlr2->is_failed == false);
	CU_ASSERT(curr_path2->is_failed == false);
	CU_ASSERT(first_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(second_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	set_thread(0);

	spdk_put_io_channel(ch1);

	set_thread(1);

	spdk_put_io_channel(ch2);

	poll_threads();

	set_thread(0);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	free(first_bdev_io);
	free(second_bdev_io);
}

static void
test_find_io_path(void)
{
	struct nvme_bdev_channel nbdev_ch = {
		.io_path_list = STAILQ_HEAD_INITIALIZER(nbdev_ch.io_path_list),
	};
	struct spdk_nvme_qpair qpair1 = {}, qpair2 = {};
	struct spdk_nvme_ctrlr ctrlr1 = {}, ctrlr2 = {};
	struct nvme_ctrlr nvme_ctrlr1 = { .ctrlr = &ctrlr1, }, nvme_ctrlr2 = { .ctrlr = &ctrlr2, };
	struct nvme_ctrlr_channel ctrlr_ch1 = {}, ctrlr_ch2 = {};
	struct nvme_qpair nvme_qpair1 = { .ctrlr_ch = &ctrlr_ch1, .ctrlr = &nvme_ctrlr1, };
	struct nvme_qpair nvme_qpair2 = { .ctrlr_ch = &ctrlr_ch2, .ctrlr = &nvme_ctrlr2, };
	struct nvme_ns nvme_ns1 = {}, nvme_ns2 = {};
	struct nvme_io_path io_path1 = { .qpair = &nvme_qpair1, .nvme_ns = &nvme_ns1, };
	struct nvme_io_path io_path2 = { .qpair = &nvme_qpair2, .nvme_ns = &nvme_ns2, };

	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path1, stailq);

	/* Test if io_path whose ANA state is not accessible is excluded. */

	nvme_qpair1.qpair = &qpair1;
	nvme_ns1.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == NULL);

	nvme_ns1.ana_state = SPDK_NVME_ANA_PERSISTENT_LOSS_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == NULL);

	nvme_ns1.ana_state = SPDK_NVME_ANA_CHANGE_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == NULL);

	nvme_ns1.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);

	nbdev_ch.current_io_path = NULL;

	nvme_ns1.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);

	nbdev_ch.current_io_path = NULL;

	/* Test if io_path whose qpair is resetting is excluded. */

	nvme_qpair1.qpair = NULL;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == NULL);

	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path2, stailq);

	/* Test if ANA optimized state or the first found ANA non-optimized state
	 * is prioritized.
	 */

	nvme_qpair1.qpair = &qpair1;
	nvme_ns1.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	nvme_qpair2.qpair = &qpair2;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nbdev_ch.current_io_path = NULL;

	nvme_ns2.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);

	nbdev_ch.current_io_path = NULL;
}

static void
test_retry_io_if_ana_state_is_updating(void)
{
	struct nvme_path_id path = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns;
	struct spdk_bdev_io *bdev_io1;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct nvme_qpair *nvme_qpair;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path.trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&path.trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	opts.ctrlr_loss_timeout_sec = -1;
	opts.reconnect_delay_sec = 1;

	rc = bdev_nvme_create(&path.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path.trid);
	CU_ASSERT(nvme_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	CU_ASSERT(nvme_ns != NULL);

	bdev_io1 = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io1);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	nvme_qpair = io_path->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair->qpair != NULL);

	bdev_io1->internal.ch = (struct spdk_bdev_channel *)ch;

	/* If qpair is connected, I/O should succeed. */
	bdev_io1->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io1);
	CU_ASSERT(bdev_io1->internal.in_submit_request == true);

	poll_threads();
	CU_ASSERT(bdev_io1->internal.in_submit_request == false);
	CU_ASSERT(bdev_io1->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	/* If ANA state of namespace is inaccessible, I/O should be queued. */
	nvme_ns->ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	nbdev_ch->current_io_path = NULL;

	bdev_io1->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io1->internal.in_submit_request == true);
	CU_ASSERT(bdev_io1 == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	/* ANA state became accessible while I/O was queued. */
	nvme_ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	spdk_delay_us(1000000);

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io1->internal.in_submit_request == true);
	CU_ASSERT(TAILQ_EMPTY(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io1->internal.in_submit_request == false);
	CU_ASSERT(bdev_io1->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io1);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_retry_io_for_io_path_error(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr1, *nvme_ctrlr2;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns1, *nvme_ns2;
	struct spdk_bdev_io *bdev_io;
	struct nvme_bdev_io *bio;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path1, *io_path2;
	struct nvme_qpair *nvme_qpair1, *nvme_qpair2;
	struct ut_nvme_req *req;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);

	g_opts.bdev_retry_count = 1;

	set_thread(0);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	CU_ASSERT(nvme_ctrlr1 != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns1 = nvme_ctrlr_get_first_active_ns(nvme_ctrlr1);
	CU_ASSERT(nvme_ns1 != NULL);
	CU_ASSERT(nvme_ns1 == _nvme_bdev_get_ns(bdev, nvme_ctrlr1));

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io);

	bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path1 = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);

	nvme_qpair1 = io_path1->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair1 != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair1->qpair != NULL);

	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;

	/* I/O got a temporary I/O path error, but it should not retry if DNR is set. */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair1->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	req->cpl.status.sct = SPDK_NVME_SCT_PATH;
	req->cpl.status.dnr = 1;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR);

	/* I/O got a temporary I/O path error, but it should succeed after retry. */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair1->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	req->cpl.status.sct = SPDK_NVME_SCT_PATH;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Add io_path2 dynamically, and create a multipath configuration. */
	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	CU_ASSERT(nvme_ctrlr2 != NULL);

	nvme_ns2 = nvme_ctrlr_get_first_active_ns(nvme_ctrlr2);
	CU_ASSERT(nvme_ns2 != NULL);
	CU_ASSERT(nvme_ns2 == _nvme_bdev_get_ns(bdev, nvme_ctrlr2));

	io_path2 = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(io_path2 != NULL);

	nvme_qpair2 = io_path2->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair2 != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair2->qpair != NULL);

	/* I/O is submitted to io_path1, but qpair of io_path1 was disconnected
	 * and deleted. Hence the I/O was aborted. But io_path2 is available.
	 * So after a retry, I/O is submitted to io_path2 and should succeed.
	 */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(nvme_qpair2->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair1->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair1->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(nvme_qpair2->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	spdk_nvme_ctrlr_free_io_qpair(nvme_qpair1->qpair);
	nvme_qpair1->qpair = NULL;

	poll_threads();

	CU_ASSERT(nvme_qpair2->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	g_opts.bdev_retry_count = 0;
}

static void
test_retry_io_count(void)
{
	struct nvme_path_id path = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns;
	struct spdk_bdev_io *bdev_io;
	struct nvme_bdev_io *bio;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct nvme_qpair *nvme_qpair;
	struct ut_nvme_req *req;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path.trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&path.trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&path.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path.trid);
	CU_ASSERT(nvme_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	CU_ASSERT(nvme_ns != NULL);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io);

	bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	nvme_qpair = io_path->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair->qpair != NULL);

	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;

	/* If I/O is aborted by request, it should not be retried. */
	g_opts.bdev_retry_count = 1;

	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_ABORTED);

	/* If bio->retry_count is not less than g_opts.bdev_retry_count,
	 * the failed I/O should not be retried.
	 */
	g_opts.bdev_retry_count = 4;

	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	bio->retry_count = 4;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NVME_ERROR);

	/* If g_opts.bdev_retry_count is -1, the failed I/O always should be retried. */
	g_opts.bdev_retry_count = -1;

	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	bio->retry_count = 4;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* If bio->retry_count is less than g_opts.bdev_retry_count,
	 * the failed I/O should be retried.
	 */
	g_opts.bdev_retry_count = 4;

	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	req->cpl.status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	bio->retry_count = 3;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	g_opts.bdev_retry_count = 0;
}

static void
test_concurrent_read_ana_log_page(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&trid, 1, true, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns[0].ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	nvme_ctrlr_read_ana_log_page(nvme_ctrlr);

	CU_ASSERT(nvme_ctrlr->ana_log_page_updating == true);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 1);

	/* Following read request should be rejected. */
	nvme_ctrlr_read_ana_log_page(nvme_ctrlr);

	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 1);

	set_thread(1);

	nvme_ctrlr_read_ana_log_page(nvme_ctrlr);

	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 1);

	/* Reset request while reading ANA log page should not be rejected. */
	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->ana_log_page_updating == false);
	CU_ASSERT(ctrlr->adminq.num_outstanding_reqs == 0);

	/* Read ANA log page while resetting ctrlr should be rejected. */
	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	nvme_ctrlr_read_ana_log_page(nvme_ctrlr);

	CU_ASSERT(nvme_ctrlr->ana_log_page_updating == false);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	set_thread(0);

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_retry_io_for_ana_error(void)
{
	struct nvme_path_id path = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns;
	struct spdk_bdev_io *bdev_io;
	struct nvme_bdev_io *bio;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct nvme_qpair *nvme_qpair;
	struct ut_nvme_req *req;
	uint64_t now;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path.trid);

	g_opts.bdev_retry_count = 1;

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&path.trid, 1, true, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	rc = bdev_nvme_create(&path.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path.trid);
	CU_ASSERT(nvme_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	CU_ASSERT(nvme_ns != NULL);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io);

	bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	nvme_qpair = io_path->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair->qpair != NULL);

	now = spdk_get_ticks();

	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;

	/* If I/O got ANA error, it should be queued, the corresponding namespace
	 * should be freezed and its ANA state should be updated.
	 */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);

	req = ut_get_outstanding_nvme_request(nvme_qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	nvme_ns->ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	req->cpl.status.sc = SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE;
	req->cpl.status.sct = SPDK_NVME_SCT_PATH;

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));
	/* I/O should be retried immediately. */
	CU_ASSERT(bio->retry_ticks == now);
	CU_ASSERT(nvme_ns->ana_state_updating == true);
	CU_ASSERT(nvme_ctrlr->ana_log_page_updating == true);

	poll_threads();

	/* Namespace is inaccessible, and hence I/O should be queued again. */
	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));
	/* I/O should be retried after a second if no I/O path was found but
	 * any I/O path may become available.
	 */
	CU_ASSERT(bio->retry_ticks == now + spdk_get_ticks_hz());

	/* Namespace should be unfreezed after completing to update its ANA state. */
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ns->ana_state_updating == false);
	CU_ASSERT(nvme_ns->ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE);
	CU_ASSERT(nvme_ctrlr->ana_log_page_updating == false);

	/* Retry the queued I/O should succeed. */
	spdk_delay_us(spdk_get_ticks_hz() - g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	g_opts.bdev_retry_count = 0;
}

static void
test_check_io_error_resiliency_params(void)
{
	/* 1st parameter is ctrlr_loss_timeout_sec, 2nd parameter is reconnect_delay_sec, and
	 * 3rd parameter is fast_io_fail_timeout_sec.
	 */
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-2, 1, 0) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, 0, 0) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(1, 0, 0) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(1, 2, 0) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(0, 1, 0) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, 1, 0) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(2, 2, 0) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(2, 1, 0) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(INT32_MAX, INT32_MAX, 0) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, UINT32_MAX, 0) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(0, 0, 1) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, 2, 1) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(3, 2, 4) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(3, 2, 1) == false);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, 1, 1) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(2, 1, 2) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(2, 1, 1) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(INT32_MAX, INT32_MAX, INT32_MAX) == true);
	CU_ASSERT(bdev_nvme_check_io_error_resiliency_params(-1, UINT32_MAX, UINT32_MAX) == true);
}

static void
test_retry_io_if_ctrlr_is_resetting(void)
{
	struct nvme_path_id path = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns;
	struct spdk_bdev_io *bdev_io1, *bdev_io2;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct nvme_qpair *nvme_qpair;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path.trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&path.trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	opts.ctrlr_loss_timeout_sec = -1;
	opts.reconnect_delay_sec = 1;

	rc = bdev_nvme_create(&path.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path.trid);
	CU_ASSERT(nvme_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	CU_ASSERT(nvme_ns != NULL);

	bdev_io1 = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io1);

	bdev_io2 = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, NULL);
	ut_bdev_io_set_buf(bdev_io2);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	nvme_qpair = io_path->qpair;
	SPDK_CU_ASSERT_FATAL(nvme_qpair != NULL);
	SPDK_CU_ASSERT_FATAL(nvme_qpair->qpair != NULL);

	bdev_io1->internal.ch = (struct spdk_bdev_channel *)ch;
	bdev_io2->internal.ch = (struct spdk_bdev_channel *)ch;

	/* If qpair is connected, I/O should succeed. */
	bdev_io1->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io1);
	CU_ASSERT(bdev_io1->internal.in_submit_request == true);

	poll_threads();
	CU_ASSERT(bdev_io1->internal.in_submit_request == false);
	CU_ASSERT(bdev_io1->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	/* If qpair is disconnected, it is freed and then reconnected via resetting
	 * the corresponding nvme_ctrlr. I/O should be queued if it is submitted
	 * while resetting the nvme_ctrlr.
	 */
	nvme_qpair->qpair->failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	ctrlr->is_failed = true;

	poll_thread_times(0, 5);

	CU_ASSERT(nvme_qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(ctrlr->is_failed == false);

	bdev_io1->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io1);

	spdk_delay_us(1);

	bdev_io2->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io2);

	CU_ASSERT(bdev_io1->internal.in_submit_request == true);
	CU_ASSERT(bdev_io2->internal.in_submit_request == true);
	CU_ASSERT(bdev_io1 == TAILQ_FIRST(&nbdev_ch->retry_io_list));
	CU_ASSERT(bdev_io2 == TAILQ_NEXT(bdev_io1, module_link));

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_qpair->qpair != NULL);
	CU_ASSERT(nvme_ctrlr->resetting == false);

	spdk_delay_us(999999 - g_opts.nvme_adminq_poll_period_us);

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io1->internal.in_submit_request == true);
	CU_ASSERT(bdev_io2->internal.in_submit_request == true);
	CU_ASSERT(bdev_io2 == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io1->internal.in_submit_request == false);
	CU_ASSERT(bdev_io1->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(bdev_io2->internal.in_submit_request == true);
	CU_ASSERT(bdev_io2 == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	spdk_delay_us(1);

	poll_thread_times(0, 1);

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 1);
	CU_ASSERT(bdev_io2->internal.in_submit_request == true);
	CU_ASSERT(TAILQ_EMPTY(&nbdev_ch->retry_io_list));

	poll_threads();

	CU_ASSERT(nvme_qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io2->internal.in_submit_request == false);
	CU_ASSERT(bdev_io2->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io1);
	free(bdev_io2);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_reconnect_ctrlr(void)
{
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_io_channel *ch1, *ch2;
	struct nvme_ctrlr_channel *ctrlr_ch1, *ctrlr_ch2;
	int rc;

	ut_init_trid(&trid);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	nvme_ctrlr->opts.ctrlr_loss_timeout_sec = 2;
	nvme_ctrlr->opts.reconnect_delay_sec = 1;

	ch1 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	ctrlr_ch1 = spdk_io_channel_get_ctx(ch1);
	CU_ASSERT(ctrlr_ch1->qpair != NULL);

	set_thread(1);

	ch2 = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	ctrlr_ch2 = spdk_io_channel_get_ctx(ch2);

	/* Reset starts from thread 1. */
	set_thread(1);

	/* The reset should fail and a reconnect timer should be registered. */
	ctrlr.fail_reset = true;
	ctrlr.is_failed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(ctrlr.is_failed == true);

	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr.is_failed == false);
	CU_ASSERT(ctrlr_ch1->qpair->qpair == NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == true);

	/* Then a reconnect retry should suceeed. */
	ctrlr.fail_reset = false;

	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_thread_times(0, 1);

	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer == NULL);

	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr_ch1->qpair->qpair != NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == false);

	/* The reset should fail and a reconnect timer should be registered. */
	ctrlr.fail_reset = true;
	ctrlr.is_failed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(ctrlr.is_failed == true);

	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr.is_failed == false);
	CU_ASSERT(ctrlr_ch1->qpair->qpair == NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == true);

	/* Then a reconnect retry should still fail. */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_thread_times(0, 1);

	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer == NULL);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr.is_failed == false);
	CU_ASSERT(ctrlr_ch1->qpair->qpair == NULL);
	CU_ASSERT(ctrlr_ch2->qpair->qpair == NULL);
	CU_ASSERT(bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr) == false);

	/* Then a reconnect retry should still fail and the ctrlr should be deleted. */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	CU_ASSERT(nvme_ctrlr == nvme_ctrlr_get_by_name("nvme0"));
	CU_ASSERT(bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr) == true);
	CU_ASSERT(nvme_ctrlr->destruct == true);

	spdk_put_io_channel(ch2);

	set_thread(0);

	spdk_put_io_channel(ch1);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static struct nvme_path_id *
ut_get_path_id_by_trid(struct nvme_ctrlr *nvme_ctrlr,
		       const struct spdk_nvme_transport_id *trid)
{
	struct nvme_path_id *p;

	TAILQ_FOREACH(p, &nvme_ctrlr->trids, link) {
		if (spdk_nvme_transport_id_compare(&p->trid, trid) == 0) {
			break;
		}
	}

	return p;
}

static void
test_retry_failover_ctrlr(void)
{
	struct spdk_nvme_transport_id trid1 = {}, trid2 = {}, trid3 = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;
	struct nvme_path_id *path_id1, *path_id2, *path_id3;
	struct spdk_io_channel *ch;
	struct nvme_ctrlr_channel *ctrlr_ch;
	int rc;

	ut_init_trid(&trid1);
	ut_init_trid2(&trid2);
	ut_init_trid3(&trid3);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	set_thread(0);

	rc = nvme_ctrlr_create(&ctrlr, "nvme0", &trid1, NULL);
	CU_ASSERT(rc == 0);

	nvme_ctrlr = nvme_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	nvme_ctrlr->opts.ctrlr_loss_timeout_sec = -1;
	nvme_ctrlr->opts.reconnect_delay_sec = 1;

	rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, &ctrlr, &trid2);
	CU_ASSERT(rc == 0);

	rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, &ctrlr, &trid3);
	CU_ASSERT(rc == 0);

	ch = spdk_get_io_channel(nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ctrlr_ch = spdk_io_channel_get_ctx(ch);

	path_id1 = ut_get_path_id_by_trid(nvme_ctrlr, &trid1);
	SPDK_CU_ASSERT_FATAL(path_id1 != NULL);
	CU_ASSERT(path_id1->is_failed == false);
	CU_ASSERT(path_id1 == nvme_ctrlr->active_path_id);

	/* If reset failed and reconnect is scheduled, path_id is switched from trid1 to trid2. */
	ctrlr.fail_reset = true;
	ctrlr.is_failed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr.is_failed == false);
	CU_ASSERT(ctrlr_ch->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == true);
	CU_ASSERT(path_id1->is_failed == true);

	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == true);

	path_id2 = ut_get_path_id_by_trid(nvme_ctrlr, &trid2);
	SPDK_CU_ASSERT_FATAL(path_id2 != NULL);
	CU_ASSERT(path_id2->is_failed == false);
	CU_ASSERT(path_id2 == nvme_ctrlr->active_path_id);

	/* If we remove trid2 while reconnect is scheduled, trid2 is removed and path_id is
	 * switched to trid3 but reset is not started.
	 */
	rc = bdev_nvme_failover(nvme_ctrlr, true);
	CU_ASSERT(rc == 0);

	CU_ASSERT(ut_get_path_id_by_trid(nvme_ctrlr, &trid2) == NULL);

	path_id3 = ut_get_path_id_by_trid(nvme_ctrlr, &trid3);
	SPDK_CU_ASSERT_FATAL(path_id3 != NULL);
	CU_ASSERT(path_id3->is_failed == false);
	CU_ASSERT(path_id3 == nvme_ctrlr->active_path_id);

	CU_ASSERT(nvme_ctrlr->resetting == false);

	/* If reconnect succeeds, trid3 should be the active path_id */
	ctrlr.fail_reset = false;

	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_thread_times(0, 1);

	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer == NULL);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(path_id3->is_failed == false);
	CU_ASSERT(path_id3 == nvme_ctrlr->active_path_id);
	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr_ch->qpair->qpair != NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_is_delayed == false);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_fail_path(void)
{
	struct nvme_path_id path = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct nvme_ns *nvme_ns;
	struct spdk_bdev_io *bdev_io;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct nvme_ctrlr_channel *ctrlr_ch;
	int rc;

	/* The test scenario is the following.
	 * - We set ctrlr_fail_timeout_sec to be smaller than ctrlr_loss_timeout_sec.
	 * - Rresetting a ctrlr fails and reconnecting the ctrlr is repeated.
	 * - While reconnecting the ctrlr, an I/O is submitted and queued.
	 * - The I/O waits until the ctrlr is recovered but ctrlr_fail_timeout_sec
	 *   comes first. The queued I/O is failed.
	 * - After ctrlr_fail_timeout_sec, any I/O is failed immediately.
	 * - Then ctrlr_loss_timeout_sec comes and the ctrlr is deleted.
	 */

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path.trid);

	set_thread(0);

	ctrlr = ut_attach_ctrlr(&path.trid, 1, false, false);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	opts.ctrlr_loss_timeout_sec = 4;
	opts.reconnect_delay_sec = 1;
	opts.fast_io_fail_timeout_sec = 2;

	rc = bdev_nvme_create(&path.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, false);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path.trid);
	CU_ASSERT(nvme_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	CU_ASSERT(bdev != NULL);

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	CU_ASSERT(nvme_ns != NULL);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	ctrlr_ch = io_path->qpair->ctrlr_ch;
	SPDK_CU_ASSERT_FATAL(ctrlr_ch != NULL);
	SPDK_CU_ASSERT_FATAL(ctrlr_ch->qpair->qpair != NULL);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, ch);
	ut_bdev_io_set_buf(bdev_io);


	/* Resetting a ctrlr should fail and a reconnect timer should be registered. */
	ctrlr->fail_reset = true;
	ctrlr->is_failed = true;

	rc = bdev_nvme_reset(nvme_ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvme_ctrlr->resetting == true);
	CU_ASSERT(ctrlr->is_failed == true);

	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr->is_failed == false);
	CU_ASSERT(ctrlr_ch->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(nvme_ctrlr->reset_start_tsc != 0);
	CU_ASSERT(nvme_ctrlr->fast_io_fail_timedout == false);

	/* I/O should be queued. */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	/* After a second, the I/O should be still queued and the ctrlr should be
	 * still recovering.
	 */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr->is_failed == false);
	CU_ASSERT(ctrlr_ch->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr) == false);
	CU_ASSERT(nvme_ctrlr->fast_io_fail_timedout == false);

	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);

	/* After two seconds, ctrlr_fail_timeout_sec should expire. */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	CU_ASSERT(nvme_ctrlr->resetting == false);
	CU_ASSERT(ctrlr->is_failed == false);
	CU_ASSERT(ctrlr_ch->qpair->qpair == NULL);
	CU_ASSERT(nvme_ctrlr->reconnect_delay_timer != NULL);
	CU_ASSERT(bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr) == false);
	CU_ASSERT(nvme_ctrlr->fast_io_fail_timedout == true);

	/* Then within a second, pending I/O should be failed. */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(TAILQ_EMPTY(&nbdev_ch->retry_io_list));

	/* Another I/O submission should be failed immediately. */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);

	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);

	/* After four seconds, path_loss_timeout_sec should expire and ctrlr should
	 * be deleted.
	 */
	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(nvme_ctrlr == nvme_ctrlr_get_by_name("nvme0"));
	CU_ASSERT(bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr) == true);
	CU_ASSERT(nvme_ctrlr->destruct == true);

	spdk_put_io_channel(ch);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	free(bdev_io);
}

static void
test_nvme_ns_cmp(void)
{
	struct nvme_ns nvme_ns1 = {}, nvme_ns2 = {};

	nvme_ns1.id = 0;
	nvme_ns2.id = UINT32_MAX;

	CU_ASSERT(nvme_ns_cmp(&nvme_ns1, &nvme_ns2) < 0);
	CU_ASSERT(nvme_ns_cmp(&nvme_ns2, &nvme_ns1) > 0);
}

static void
test_ana_transition(void)
{
	struct spdk_nvme_ctrlr ctrlr = { .cdata.anatt = 10, };
	struct nvme_ctrlr nvme_ctrlr = { .ctrlr = &ctrlr, };
	struct nvme_ns nvme_ns = { .ctrlr = &nvme_ctrlr, };
	struct spdk_nvme_ana_group_descriptor desc = { .ana_group_id = 1, };

	/* case 1: ANA transition timedout is canceled. */
	nvme_ns.ana_state = SPDK_NVME_ANA_CHANGE_STATE;
	nvme_ns.ana_transition_timedout = true;

	desc.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	_nvme_ns_set_ana_state(&nvme_ns, &desc);

	CU_ASSERT(nvme_ns.ana_transition_timedout == false);
	CU_ASSERT(nvme_ns.ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE);

	/* case 2: ANATT timer is kept. */
	nvme_ns.ana_state = SPDK_NVME_ANA_CHANGE_STATE;
	nvme_ns.anatt_timer = SPDK_POLLER_REGISTER(nvme_ns_ana_transition_timedout,
			      &nvme_ns,
			      ctrlr.cdata.anatt * SPDK_SEC_TO_USEC);

	desc.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;

	_nvme_ns_set_ana_state(&nvme_ns, &desc);

	CU_ASSERT(nvme_ns.anatt_timer != NULL);
	CU_ASSERT(nvme_ns.ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE);

	/* case 3: ANATT timer is stopped. */
	desc.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	_nvme_ns_set_ana_state(&nvme_ns, &desc);

	CU_ASSERT(nvme_ns.anatt_timer == NULL);
	CU_ASSERT(nvme_ns.ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE);

	/* ANATT timer is started. */
	desc.ana_state = SPDK_NVME_ANA_CHANGE_STATE;

	_nvme_ns_set_ana_state(&nvme_ns, &desc);

	CU_ASSERT(nvme_ns.anatt_timer != NULL);
	CU_ASSERT(nvme_ns.ana_state == SPDK_NVME_ANA_CHANGE_STATE);

	/* ANATT timer is expired. */
	spdk_delay_us(ctrlr.cdata.anatt * SPDK_SEC_TO_USEC);

	poll_threads();

	CU_ASSERT(nvme_ns.anatt_timer == NULL);
	CU_ASSERT(nvme_ns.ana_transition_timedout == true);
}

static void
_set_preferred_path_cb(void *cb_arg, int rc)
{
	bool *done = cb_arg;

	*done = true;
}

static void
test_set_preferred_path(void)
{
	struct nvme_path_id path1 = {}, path2 = {}, path3 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2, *ctrlr3;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	const struct spdk_nvme_ctrlr_data *cdata;
	bool done;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	ut_init_trid3(&path3.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr3 = ut_attach_ctrlr(&path3.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr3 != NULL);

	ctrlr3->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path3.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	/* ctrlr1 was added first. Hence io_path to ctrlr1 should be preferred. */

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr1);

	/* If io_path to ctrlr2 is set to the preferred path dynamically, find_io_path()
	 * should return io_path to ctrlr2.
	 */

	cdata = spdk_nvme_ctrlr_get_data(ctrlr2);
	done = false;

	bdev_nvme_set_preferred_path(bdev->disk.name, cdata->cntlid, _set_preferred_path_cb, &done);

	poll_threads();
	CU_ASSERT(done == true);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr2);

	/* If io_path to ctrlr3 is set to the preferred path and then a new I/O channel is
	 * acquired, find_io_path() should return io_path to ctrlr3.
	 */

	spdk_put_io_channel(ch);

	poll_threads();

	cdata = spdk_nvme_ctrlr_get_data(ctrlr3);
	done = false;

	bdev_nvme_set_preferred_path(bdev->disk.name, cdata->cntlid, _set_preferred_path_cb, &done);

	poll_threads();
	CU_ASSERT(done == true);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr3);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_find_next_io_path(void)
{
	struct nvme_bdev_channel nbdev_ch = {
		.io_path_list = STAILQ_HEAD_INITIALIZER(nbdev_ch.io_path_list),
		.mp_policy = BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
		.mp_selector = BDEV_NVME_MP_SELECTOR_ROUND_ROBIN,
	};
	struct spdk_nvme_qpair qpair1 = {}, qpair2 = {}, qpair3 = {};
	struct spdk_nvme_ctrlr ctrlr1 = {}, ctrlr2 = {}, ctrlr3 = {};
	struct nvme_ctrlr nvme_ctrlr1 = { .ctrlr = &ctrlr1, };
	struct nvme_ctrlr nvme_ctrlr2 = { .ctrlr = &ctrlr2, };
	struct nvme_ctrlr nvme_ctrlr3 = { .ctrlr = &ctrlr3, };
	struct nvme_ctrlr_channel ctrlr_ch1 = {};
	struct nvme_ctrlr_channel ctrlr_ch2 = {};
	struct nvme_ctrlr_channel ctrlr_ch3 = {};
	struct nvme_qpair nvme_qpair1 = { .ctrlr_ch = &ctrlr_ch1, .ctrlr = &nvme_ctrlr1, .qpair = &qpair1, };
	struct nvme_qpair nvme_qpair2 = { .ctrlr_ch = &ctrlr_ch2, .ctrlr = &nvme_ctrlr2, .qpair = &qpair2, };
	struct nvme_qpair nvme_qpair3 = { .ctrlr_ch = &ctrlr_ch3, .ctrlr = &nvme_ctrlr3, .qpair = &qpair3, };
	struct nvme_ns nvme_ns1 = {}, nvme_ns2 = {}, nvme_ns3 = {};
	struct nvme_io_path io_path1 = { .qpair = &nvme_qpair1, .nvme_ns = &nvme_ns1, };
	struct nvme_io_path io_path2 = { .qpair = &nvme_qpair2, .nvme_ns = &nvme_ns2, };
	struct nvme_io_path io_path3 = { .qpair = &nvme_qpair3, .nvme_ns = &nvme_ns3, };

	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path1, stailq);
	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path2, stailq);
	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path3, stailq);

	/* test the case when nbdev_ch->current_io_path is filled, the case of current_io_path = NULL
	 * is covered in test_find_io_path.
	 */

	nbdev_ch.current_io_path = &io_path2;
	nvme_ns1.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nvme_ns1.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nvme_ns1.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);

	nbdev_ch.current_io_path = &io_path3;
	nvme_ns1.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	/* Test if next io_path is selected according to rr_min_io */

	nbdev_ch.current_io_path = NULL;
	nbdev_ch.rr_min_io = 2;
	nbdev_ch.rr_counter = 0;
	nvme_ns1.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nvme_ns3.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);
}

static void
test_find_io_path_min_qd(void)
{
	struct nvme_bdev_channel nbdev_ch = {
		.io_path_list = STAILQ_HEAD_INITIALIZER(nbdev_ch.io_path_list),
		.mp_policy = BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
		.mp_selector = BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH,
	};
	struct spdk_nvme_qpair qpair1 = {}, qpair2 = {}, qpair3 = {};
	struct spdk_nvme_ctrlr ctrlr1 = {}, ctrlr2 = {}, ctrlr3 = {};
	struct nvme_ctrlr nvme_ctrlr1 = { .ctrlr = &ctrlr1, };
	struct nvme_ctrlr nvme_ctrlr2 = { .ctrlr = &ctrlr2, };
	struct nvme_ctrlr nvme_ctrlr3 = { .ctrlr = &ctrlr3, };
	struct nvme_ctrlr_channel ctrlr_ch1 = {};
	struct nvme_ctrlr_channel ctrlr_ch2 = {};
	struct nvme_ctrlr_channel ctrlr_ch3 = {};
	struct nvme_qpair nvme_qpair1 = { .ctrlr_ch = &ctrlr_ch1, .ctrlr = &nvme_ctrlr1, .qpair = &qpair1, };
	struct nvme_qpair nvme_qpair2 = { .ctrlr_ch = &ctrlr_ch2, .ctrlr = &nvme_ctrlr2, .qpair = &qpair2, };
	struct nvme_qpair nvme_qpair3 = { .ctrlr_ch = &ctrlr_ch3, .ctrlr = &nvme_ctrlr3, .qpair = &qpair3, };
	struct nvme_ns nvme_ns1 = {}, nvme_ns2 = {}, nvme_ns3 = {};
	struct nvme_io_path io_path1 = { .qpair = &nvme_qpair1, .nvme_ns = &nvme_ns1, };
	struct nvme_io_path io_path2 = { .qpair = &nvme_qpair2, .nvme_ns = &nvme_ns2, };
	struct nvme_io_path io_path3 = { .qpair = &nvme_qpair3, .nvme_ns = &nvme_ns3, };

	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path1, stailq);
	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path2, stailq);
	STAILQ_INSERT_TAIL(&nbdev_ch.io_path_list, &io_path3, stailq);

	/* Test if the minumum io_outstanding or the ANA optimized state is
	 * prioritized when using least queue depth selector
	 */
	qpair1.num_outstanding_reqs = 2;
	qpair2.num_outstanding_reqs = 1;
	qpair3.num_outstanding_reqs = 0;
	nvme_ns1.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nvme_ns1.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	nvme_ns1.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns2.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvme_ns3.ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path2);

	qpair2.num_outstanding_reqs = 4;
	CU_ASSERT(bdev_nvme_find_io_path(&nbdev_ch) == &io_path1);
}

static void
test_disable_auto_failback(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr1;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	const struct spdk_nvme_ctrlr_data *cdata;
	bool done;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	g_opts.disable_auto_failback = true;

	opts.ctrlr_loss_timeout_sec = -1;
	opts.reconnect_delay_sec = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	/* ctrlr1 was added first. Hence io_path to ctrlr1 should be preferred. */

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr1);

	/* If resetting ctrlr1 failed, io_path to ctrlr2 should be used. */
	ctrlr1->fail_reset = true;
	ctrlr1->is_failed = true;

	bdev_nvme_reset(nvme_ctrlr1);

	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	CU_ASSERT(ctrlr1->adminq.is_connected == false);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr2);

	/* After a second, ctrlr1 is recovered. However, automatic failback is disabled.
	 * Hence, io_path to ctrlr2 should still be used.
	 */
	ctrlr1->fail_reset = false;

	spdk_delay_us(SPDK_SEC_TO_USEC);
	poll_threads();

	CU_ASSERT(ctrlr1->adminq.is_connected == true);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr2);

	/* Set io_path to ctrlr1 to preferred explicitly. Then io_path to ctrlr1 should
	 * be used again.
	 */

	cdata = spdk_nvme_ctrlr_get_data(ctrlr1);
	done = false;

	bdev_nvme_set_preferred_path(bdev->disk.name, cdata->cntlid, _set_preferred_path_cb, &done);

	poll_threads();
	CU_ASSERT(done == true);

	io_path = bdev_nvme_find_io_path(nbdev_ch);
	SPDK_CU_ASSERT_FATAL(io_path != NULL);

	CU_ASSERT(io_path->nvme_ns->ctrlr->ctrlr == ctrlr1);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);

	g_opts.disable_auto_failback = false;
}

static void
ut_set_multipath_policy_done(void *cb_arg, int rc)
{
	int *done = cb_arg;

	SPDK_CU_ASSERT_FATAL(done != NULL);
	*done = rc;
}

static void
test_set_multipath_policy(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct nvme_ctrlr_opts opts = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	int done;
	int rc;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	g_opts.disable_auto_failback = true;

	opts.ctrlr_loss_timeout_sec = -1;
	opts.reconnect_delay_sec = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, &opts, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	/* If multipath policy is updated before getting any I/O channel,
	 * an new I/O channel should have the update.
	 */
	done = -1;
	bdev_nvme_set_multipath_policy(bdev->disk.name, BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
				       BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH, UINT32_MAX,
				       ut_set_multipath_policy_done, &done);
	poll_threads();
	CU_ASSERT(done == 0);

	CU_ASSERT(bdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE);
	CU_ASSERT(bdev->mp_selector == BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH);
	CU_ASSERT(bdev->rr_min_io == UINT32_MAX);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	CU_ASSERT(nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE);
	CU_ASSERT(nbdev_ch->mp_selector == BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH);
	CU_ASSERT(nbdev_ch->rr_min_io == UINT32_MAX);

	/* If multipath policy is updated while a I/O channel is active,
	 * the update should be applied to the I/O channel immediately.
	 */
	done = -1;
	bdev_nvme_set_multipath_policy(bdev->disk.name, BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE,
				       BDEV_NVME_MP_SELECTOR_ROUND_ROBIN, UINT32_MAX,
				       ut_set_multipath_policy_done, &done);
	poll_threads();
	CU_ASSERT(done == 0);

	CU_ASSERT(bdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE);
	CU_ASSERT(nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE);
	CU_ASSERT(bdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN);
	CU_ASSERT(nbdev_ch->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN);
	CU_ASSERT(bdev->rr_min_io == UINT32_MAX);
	CU_ASSERT(nbdev_ch->rr_min_io == UINT32_MAX);

	spdk_put_io_channel(ch);

	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_ctrlr_get_by_name("nvme0") == NULL);
}

static void
test_uuid_generation(void)
{
	uint32_t nsid1 = 1, nsid2 = 2;
	char sn1[21] = "SPDK CTRLR SERIAL 01", sn2[21] = "SPDK CTRLR SERIAL 02";
	char sn3[21] = "                    ";
	char uuid_str[SPDK_UUID_STRING_LEN] = {'\0'};
	struct spdk_uuid uuid1, uuid2;

	/* Test case 1:
	 * Serial numbers are the same, nsids are different.
	 * Compare two generated UUID - they should be different. */
	uuid1 = nvme_generate_uuid(sn1, nsid1);
	uuid2 = nvme_generate_uuid(sn1, nsid2);

	CU_ASSERT((spdk_uuid_compare(&uuid1, &uuid2)) != 0);

	/* Test case 2:
	 * Serial numbers differ only by one character, nsids are the same.
	 * Compare two generated UUID - they should be different. */
	uuid1 = nvme_generate_uuid(sn1, nsid1);
	uuid2 = nvme_generate_uuid(sn2, nsid1);

	CU_ASSERT((spdk_uuid_compare(&uuid1, &uuid2)) != 0);

	/* Test case 3:
	 * Serial number comprises only of space characters.
	 * Validate the generated UUID. */
	uuid1 = nvme_generate_uuid(sn3, nsid1);
	CU_ASSERT((spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &uuid1)) == 0);
}

static void
test_retry_io_to_same_path(void)
{
	struct nvme_path_id path1 = {}, path2 = {};
	struct spdk_nvme_ctrlr *ctrlr1, *ctrlr2;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr1, *nvme_ctrlr2;
	const int STRING_SIZE = 32;
	const char *attached_names[STRING_SIZE];
	struct nvme_bdev *bdev;
	struct spdk_bdev_io *bdev_io;
	struct nvme_bdev_io *bio;
	struct spdk_io_channel *ch;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path1, *io_path2;
	struct ut_nvme_req *req;
	struct spdk_uuid uuid1 = { .u.raw = { 0x1 } };
	int done;
	int rc;

	g_opts.nvme_ioq_poll_period_us = 1;

	memset(attached_names, 0, sizeof(char *) * STRING_SIZE);
	ut_init_trid(&path1.trid);
	ut_init_trid2(&path2.trid);
	g_ut_attach_ctrlr_status = 0;
	g_ut_attach_bdev_count = 1;

	set_thread(0);

	ctrlr1 = ut_attach_ctrlr(&path1.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr1 != NULL);

	ctrlr1->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path1.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	ctrlr2 = ut_attach_ctrlr(&path2.trid, 1, true, true);
	SPDK_CU_ASSERT_FATAL(ctrlr2 != NULL);

	ctrlr2->ns[0].uuid = &uuid1;

	rc = bdev_nvme_create(&path2.trid, "nvme0", attached_names, STRING_SIZE,
			      attach_ctrlr_done, NULL, NULL, NULL, true);
	CU_ASSERT(rc == 0);

	spdk_delay_us(1000);
	poll_threads();
	spdk_delay_us(g_opts.nvme_adminq_poll_period_us);
	poll_threads();

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name("nvme0");
	SPDK_CU_ASSERT_FATAL(nbdev_ctrlr != NULL);

	nvme_ctrlr1 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path1.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr1 != NULL);

	nvme_ctrlr2 = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, &path2.trid);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr2 != NULL);

	bdev = nvme_bdev_ctrlr_get_bdev(nbdev_ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	done = -1;
	bdev_nvme_set_multipath_policy(bdev->disk.name, BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
				       BDEV_NVME_MP_SELECTOR_ROUND_ROBIN, 1, ut_set_multipath_policy_done, &done);
	poll_threads();
	CU_ASSERT(done == 0);

	CU_ASSERT(bdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE);
	CU_ASSERT(bdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN);
	CU_ASSERT(bdev->rr_min_io == 1);

	ch = spdk_get_io_channel(bdev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	nbdev_ch = spdk_io_channel_get_ctx(ch);

	CU_ASSERT(nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE);
	CU_ASSERT(bdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN);
	CU_ASSERT(nbdev_ch->rr_min_io == 1);

	bdev_io = ut_alloc_bdev_io(SPDK_BDEV_IO_TYPE_WRITE, bdev, ch);
	ut_bdev_io_set_buf(bdev_io);

	bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	io_path1 = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr1);
	SPDK_CU_ASSERT_FATAL(io_path1 != NULL);

	io_path2 = ut_get_io_path_by_ctrlr(nbdev_ch, nvme_ctrlr2);
	SPDK_CU_ASSERT_FATAL(io_path2 != NULL);

	/* The 1st I/O should be submitted to io_path1. */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bio->io_path == io_path1);
	CU_ASSERT(io_path1->qpair->qpair->num_outstanding_reqs == 1);

	spdk_delay_us(1);

	poll_threads();
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	/* The 2nd I/O should be submitted to io_path2 because the path selection
	 * policy is round-robin.
	 */
	bdev_io->internal.in_submit_request = true;

	bdev_nvme_submit_request(ch, bdev_io);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bio->io_path == io_path2);
	CU_ASSERT(io_path2->qpair->qpair->num_outstanding_reqs == 1);

	req = ut_get_outstanding_nvme_request(io_path2->qpair->qpair, bio);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Set retry count to non-zero. */
	g_opts.bdev_retry_count = 1;

	/* Inject an I/O error. */
	req->cpl.status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
	req->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	/* The 2nd I/O should be queued to nbdev_ch. */
	spdk_delay_us(1);
	poll_thread_times(0, 1);

	CU_ASSERT(io_path2->qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bdev_io == TAILQ_FIRST(&nbdev_ch->retry_io_list));

	/* The 2nd I/O should keep caching io_path2. */
	CU_ASSERT(bio->io_path == io_path2);

	/* The 2nd I/O should be submitted to io_path2 again. */
	poll_thread_times(0, 1);

	CU_ASSERT(bdev_io->internal.in_submit_request == true);
	CU_ASSERT(bio->io_path == io_path2);
	CU_ASSERT(io_path2->qpair->qpair->num_outstanding_reqs == 1);

	spdk_delay_us(1);
	poll_threads();

	CU_ASSERT(io_path2->qpair->qpair->num_outstanding_reqs == 0);
	CU_ASSERT(bdev_io->internal.in_submit_request == false);
	CU_ASSERT(bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);

	free(bdev_io);

	spdk_put_io_channel(ch);

	poll_threads();
	spdk_delay_us(1);
	poll_threads();

	rc = bdev_nvme_delete("nvme0", &g_any_path);
	CU_ASSERT(rc == 0);

	poll_threads();
	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(nvme_bdev_ctrlr_get_by_name("nvme0") == NULL);

	g_opts.nvme_ioq_poll_period_us = 0;
	g_opts.bdev_retry_count = 0;
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
	CU_ADD_TEST(suite, test_race_between_failover_and_add_secondary_trid);
	CU_ADD_TEST(suite, test_pending_reset);
	CU_ADD_TEST(suite, test_attach_ctrlr);
	CU_ADD_TEST(suite, test_aer_cb);
	CU_ADD_TEST(suite, test_submit_nvme_cmd);
	CU_ADD_TEST(suite, test_add_remove_trid);
	CU_ADD_TEST(suite, test_abort);
	CU_ADD_TEST(suite, test_get_io_qpair);
	CU_ADD_TEST(suite, test_bdev_unregister);
	CU_ADD_TEST(suite, test_compare_ns);
	CU_ADD_TEST(suite, test_init_ana_log_page);
	CU_ADD_TEST(suite, test_get_memory_domains);
	CU_ADD_TEST(suite, test_reconnect_qpair);
	CU_ADD_TEST(suite, test_create_bdev_ctrlr);
	CU_ADD_TEST(suite, test_add_multi_ns_to_bdev);
	CU_ADD_TEST(suite, test_add_multi_io_paths_to_nbdev_ch);
	CU_ADD_TEST(suite, test_admin_path);
	CU_ADD_TEST(suite, test_reset_bdev_ctrlr);
	CU_ADD_TEST(suite, test_find_io_path);
	CU_ADD_TEST(suite, test_retry_io_if_ana_state_is_updating);
	CU_ADD_TEST(suite, test_retry_io_for_io_path_error);
	CU_ADD_TEST(suite, test_retry_io_count);
	CU_ADD_TEST(suite, test_concurrent_read_ana_log_page);
	CU_ADD_TEST(suite, test_retry_io_for_ana_error);
	CU_ADD_TEST(suite, test_check_io_error_resiliency_params);
	CU_ADD_TEST(suite, test_retry_io_if_ctrlr_is_resetting);
	CU_ADD_TEST(suite, test_reconnect_ctrlr);
	CU_ADD_TEST(suite, test_retry_failover_ctrlr);
	CU_ADD_TEST(suite, test_fail_path);
	CU_ADD_TEST(suite, test_nvme_ns_cmp);
	CU_ADD_TEST(suite, test_ana_transition);
	CU_ADD_TEST(suite, test_set_preferred_path);
	CU_ADD_TEST(suite, test_find_next_io_path);
	CU_ADD_TEST(suite, test_find_io_path_min_qd);
	CU_ADD_TEST(suite, test_disable_auto_failback);
	CU_ADD_TEST(suite, test_set_multipath_policy);
	CU_ADD_TEST(suite, test_uuid_generation);
	CU_ADD_TEST(suite, test_retry_io_to_same_path);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	allocate_threads(3);
	set_thread(0);
	bdev_nvme_library_init();
	init_accel();

	CU_basic_run_tests();

	set_thread(0);
	bdev_nvme_library_fini();
	fini_accel();
	free_threads();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
