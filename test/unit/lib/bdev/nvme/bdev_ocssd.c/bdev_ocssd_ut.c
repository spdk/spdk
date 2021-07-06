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
#include "spdk/nvme_ocssd_spec.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"

#include "bdev/nvme/bdev_ocssd.c"
#include "bdev/nvme/common.c"
#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_nvme_ctrlr_is_ocssd_ns, bool, (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid),
	    true);
DEFINE_STUB(spdk_nvme_ns_get_extended_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 4096);
DEFINE_STUB(spdk_nvme_ns_get_max_io_xfer_size, uint32_t, (struct spdk_nvme_ns *ns), 0);
DEFINE_STUB(spdk_nvme_ns_is_active, bool, (struct spdk_nvme_ns *ns), true);
DEFINE_STUB_V(spdk_opal_dev_destruct, (struct spdk_opal_dev *dev));
DEFINE_STUB_V(spdk_bdev_io_complete_nvme_status, (struct spdk_bdev_io *bdev_io, uint32_t cdw0,
		int sct, int sc));
DEFINE_STUB(spdk_bdev_push_media_events, int, (struct spdk_bdev *bdev,
		const struct spdk_bdev_media_event *events,
		size_t num_events), 0);
DEFINE_STUB_V(spdk_bdev_notify_media_management, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_module_finish_done, (void));
DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *, (enum spdk_nvme_transport_type trtype),
	    NULL);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);

struct nvme_request {
	spdk_nvme_cmd_cb cb_fn;
	void *cb_arg;
	TAILQ_ENTRY(nvme_request) tailq;
};

struct spdk_nvme_qpair {
	TAILQ_HEAD(, nvme_request) requests;
};

struct spdk_nvme_ns {
	uint32_t nsid;
	struct spdk_nvme_ctrlr *ctrlr;
};

struct spdk_nvme_ctrlr {
	struct spdk_nvme_transport_id trid;
	struct spdk_ocssd_geometry_data geometry;
	struct spdk_nvme_qpair *admin_qpair;
	struct spdk_nvme_ns *ns;
	uint32_t ns_count;
	struct spdk_ocssd_chunk_information_entry *chunk_info;
	uint64_t num_chunks;

	LIST_ENTRY(spdk_nvme_ctrlr) list;
};

static LIST_HEAD(, spdk_nvme_ctrlr) g_ctrlr_list = LIST_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, spdk_bdev) g_bdev_list = TAILQ_HEAD_INITIALIZER(g_bdev_list);
static struct spdk_thread *g_thread;

static struct spdk_nvme_ctrlr *
find_controller(const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_ctrlr *ctrlr;

	LIST_FOREACH(ctrlr, &g_ctrlr_list, list) {
		if (!spdk_nvme_transport_id_compare(trid, &ctrlr->trid)) {
			return ctrlr;
		}
	}

	return NULL;
}

static void
free_controller(struct spdk_nvme_ctrlr *ctrlr)
{
	CU_ASSERT(!nvme_ctrlr_get(&ctrlr->trid));
	LIST_REMOVE(ctrlr, list);
	spdk_nvme_ctrlr_free_io_qpair(ctrlr->admin_qpair);
	free(ctrlr->chunk_info);
	free(ctrlr->ns);
	free(ctrlr);
}

static uint64_t
chunk_offset_to_lba(struct spdk_ocssd_geometry_data *geo, uint64_t offset)
{
	uint64_t chk, pu, grp;
	uint64_t chk_off, pu_off, grp_off;

	chk_off = geo->lbaf.lbk_len;
	pu_off = geo->lbaf.chk_len + chk_off;
	grp_off = geo->lbaf.pu_len + pu_off;

	chk = offset % geo->num_chk;
	pu = (offset / geo->num_chk) % geo->num_pu;
	grp = (offset / (geo->num_chk * geo->num_pu)) % geo->num_grp;

	return chk << chk_off |
	       pu  << pu_off  |
	       grp << grp_off;
}

static struct spdk_nvme_ctrlr *
create_controller(const struct spdk_nvme_transport_id *trid, uint32_t ns_count,
		  const struct spdk_ocssd_geometry_data *geo)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t nsid, offset;

	SPDK_CU_ASSERT_FATAL(!find_controller(trid));

	ctrlr = calloc(1, sizeof(*ctrlr));
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns = calloc(ns_count, sizeof(*ctrlr->ns));
	SPDK_CU_ASSERT_FATAL(ctrlr->ns != NULL);

	ctrlr->num_chunks = geo->num_grp * geo->num_pu * geo->num_chk;
	ctrlr->chunk_info = calloc(ctrlr->num_chunks, sizeof(*ctrlr->chunk_info));
	SPDK_CU_ASSERT_FATAL(ctrlr->chunk_info != NULL);

	for (nsid = 0; nsid < ns_count; ++nsid) {
		ctrlr->ns[nsid].nsid = nsid + 1;
		ctrlr->ns[nsid].ctrlr = ctrlr;
	}

	ctrlr->geometry = *geo;
	ctrlr->trid = *trid;
	ctrlr->ns_count = ns_count;
	ctrlr->admin_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);

	for (offset = 0; offset < ctrlr->num_chunks; ++offset) {
		ctrlr->chunk_info[offset].cs.free = 1;
		ctrlr->chunk_info[offset].slba = chunk_offset_to_lba(&ctrlr->geometry, offset);
		ctrlr->chunk_info[offset].wp = ctrlr->chunk_info[offset].slba;
	}

	SPDK_CU_ASSERT_FATAL(ctrlr->admin_qpair != NULL);

	LIST_INSERT_HEAD(&g_ctrlr_list, ctrlr, list);

	return ctrlr;
}

static int
io_channel_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
io_channel_destroy_cb(void *io_device, void *ctx_buf)
{}

void
nvme_ctrlr_populate_namespace_done(struct nvme_async_probe_ctx *ctx,
				   struct nvme_ns *ns, int rc)
{
	CU_ASSERT_EQUAL(rc, 0);
	ns->ctrlr->ref++;
}

static struct nvme_ctrlr *
create_nvme_bdev_controller(const struct spdk_nvme_transport_id *trid, const char *name)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_ctrlr_trid *trid_entry;
	uint32_t nsid;
	int rc;

	ctrlr = find_controller(trid);

	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);
	SPDK_CU_ASSERT_FATAL(!nvme_ctrlr_get(trid));

	nvme_ctrlr = calloc(1, sizeof(*nvme_ctrlr));
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	rc = pthread_mutex_init(&nvme_ctrlr->mutex, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	nvme_ctrlr->namespaces = calloc(ctrlr->ns_count, sizeof(struct nvme_ns *));
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr->namespaces != NULL);

	trid_entry = calloc(1, sizeof(struct nvme_ctrlr_trid));
	SPDK_CU_ASSERT_FATAL(trid_entry != NULL);
	trid_entry->trid = *trid;

	nvme_ctrlr->ctrlr = ctrlr;
	nvme_ctrlr->num_ns = ctrlr->ns_count;
	nvme_ctrlr->ref = 1;
	nvme_ctrlr->connected_trid = &trid_entry->trid;
	nvme_ctrlr->name = strdup(name);
	for (nsid = 0; nsid < ctrlr->ns_count; ++nsid) {
		nvme_ctrlr->namespaces[nsid] = calloc(1, sizeof(struct nvme_ns));
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr->namespaces[nsid] != NULL);

		nvme_ctrlr->namespaces[nsid]->id = nsid + 1;
		nvme_ctrlr->namespaces[nsid]->ctrlr = nvme_ctrlr;
		nvme_ctrlr->namespaces[nsid]->type = NVME_NS_OCSSD;

		bdev_ocssd_populate_namespace(nvme_ctrlr, nvme_ctrlr->namespaces[nsid], NULL);
	}

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	spdk_io_device_register(nvme_ctrlr, io_channel_create_cb,
				io_channel_destroy_cb, 0, name);

	TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, nvme_ctrlr, tailq);

	TAILQ_INIT(&nvme_ctrlr->trids);
	TAILQ_INSERT_HEAD(&nvme_ctrlr->trids, trid_entry, link);

	return nvme_ctrlr;
}

static struct nvme_request *
alloc_request(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *ctx;

	ctx = calloc(1, sizeof(*ctx));
	SPDK_CU_ASSERT_FATAL(ctx != NULL);

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	return ctx;
}

uint32_t
spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->ns_count;
}

uint32_t
spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns)
{
	return ns->nsid;
}

struct spdk_nvme_ctrlr *
spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns)
{
	return ns->ctrlr;
}

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	if (nsid == 0 || nsid > ctrlr->ns_count) {
		return NULL;
	}

	return &ctrlr->ns[nsid - 1];
}

struct spdk_nvme_ctrlr *
spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		  const struct spdk_nvme_ctrlr_opts *opts,
		  size_t opts_size)
{
	return find_controller(trid);
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev *bdev;

	SPDK_CU_ASSERT_FATAL(bdev_name != NULL);

	TAILQ_FOREACH(bdev, &g_bdev_list, internal.link) {
		if (!strcmp(bdev->name, bdev_name)) {
			return bdev;
		}
	}

	return NULL;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	CU_ASSERT_PTR_NULL(spdk_bdev_get_by_name(bdev->name));
	TAILQ_INSERT_TAIL(&g_bdev_list, bdev, internal.link);

	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	int rc;

	CU_ASSERT_EQUAL(spdk_bdev_get_by_name(bdev->name), bdev);
	TAILQ_REMOVE(&g_bdev_list, bdev, internal.link);

	rc = bdev->fn_table->destruct(bdev->ctxt);
	if (rc <= 0 && cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}
}

size_t
spdk_bdev_get_zone_size(const struct spdk_bdev *bdev)
{
	return bdev->zone_size;
}

int
spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   void *payload, uint32_t payload_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_cpl cpl = {};

	CU_ASSERT_EQUAL(payload_size, sizeof(ctrlr->geometry));
	memcpy(payload, &ctrlr->geometry, sizeof(ctrlr->geometry));

	cb_fn(cb_arg, &cpl);

	return 0;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return memcmp(trid1, trid2, sizeof(*trid1));
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
}

struct spdk_io_channel *
spdk_bdev_io_get_io_channel(struct spdk_bdev_io *bdev_io)
{
	return (struct spdk_io_channel *)bdev_io->internal.ch;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return spdk_nvme_qpair_process_completions(ctrlr->admin_qpair, 0);
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *opts,
			       size_t opts_size)
{
	struct spdk_nvme_qpair *qpair;

	qpair = calloc(1, sizeof(*qpair));
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	TAILQ_INIT(&qpair->requests);
	return qpair;
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	CU_ASSERT(TAILQ_EMPTY(&qpair->requests));
	free(qpair);

	return 0;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl = {};
	int32_t num_requests = 0;

	while ((req = TAILQ_FIRST(&qpair->requests))) {
		TAILQ_REMOVE(&qpair->requests, req, tailq);

		req->cb_fn(req->cb_arg, &cpl);
		free(req);

		num_requests++;
	}

	return num_requests;
}

int
spdk_nvme_ns_cmd_readv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       uint64_t lba, uint32_t lba_count,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			       spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
			       uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;

	req = alloc_request(cb_fn, cb_arg);
	TAILQ_INSERT_TAIL(&qpair->requests, req, tailq);

	return 0;
}

int
spdk_nvme_ns_cmd_writev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				uint64_t lba, uint32_t lba_count,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;

	req = alloc_request(cb_fn, cb_arg);
	TAILQ_INSERT_TAIL(&qpair->requests, req, tailq);

	return 0;
}

int
spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    uint64_t *lba_list, uint32_t num_lbas,
				    struct spdk_ocssd_chunk_information_entry *chunk_info,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	req = alloc_request(cb_fn, cb_arg);
	TAILQ_INSERT_TAIL(&qpair->requests, req, tailq);

	return 0;
}

static struct spdk_nvme_cpl g_chunk_info_cpl;
static bool g_zone_info_status = true;

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr,
				 uint8_t log_page, uint32_t nsid,
				 void *payload, uint32_t payload_size,
				 uint64_t offset,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	SPDK_CU_ASSERT_FATAL(offset + payload_size <= sizeof(*ctrlr->chunk_info) * ctrlr->num_chunks);
	memcpy(payload, ((char *)ctrlr->chunk_info) + offset, payload_size);

	cb_fn(cb_arg, &g_chunk_info_cpl);

	return 0;
}

static void
create_bdev_cb(const char *bdev_name, int status, void *ctx)
{
	*(int *)ctx = status;
}

static int
create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid)
{
	int status = EFAULT;

	bdev_ocssd_create_bdev(ctrlr_name, bdev_name, nsid, create_bdev_cb, &status);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	return status;
}

static void
delete_nvme_bdev_controller(struct nvme_ctrlr *nvme_ctrlr)
{
	uint32_t nsid;

	nvme_ctrlr->destruct = true;

	for (nsid = 0; nsid < nvme_ctrlr->num_ns; ++nsid) {
		bdev_ocssd_depopulate_namespace(nvme_ctrlr->namespaces[nsid]);
	}

	nvme_ctrlr_release(nvme_ctrlr);
	spdk_delay_us(1000);

	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

	CU_ASSERT(TAILQ_EMPTY(&g_nvme_ctrlrs));
}

static void
test_create_controller(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	struct spdk_ocssd_geometry_data geometry = {};
	struct spdk_bdev *bdev;
	const char *controller_name = "nvme0";
	const size_t ns_count = 16;
	char namebuf[128];
	uint32_t nsid;
	int rc;

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 512,
		.num_chk = 64,
		.num_pu = 8,
		.num_grp = 4,
		.maxoc = 69,
		.maxocpu = 68,
		.ws_opt = 86,
		.lbaf = {
			.lbk_len = 9,
			.chk_len = 6,
			.pu_len = 3,
			.grp_len = 2,
		}
	};

	ctrlr = create_controller(&trid, ns_count, &geometry);
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);

	for (nsid = 1; nsid <= ns_count; ++nsid) {
		snprintf(namebuf, sizeof(namebuf), "%sn%"PRIu32, controller_name, nsid);
		rc = create_bdev(controller_name, namebuf, nsid);
		CU_ASSERT_EQUAL(rc, 0);

		bdev = spdk_bdev_get_by_name(namebuf);
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		CU_ASSERT_TRUE(bdev->zoned);
	}

	delete_nvme_bdev_controller(nvme_ctrlr);

	/* Verify that after deletion the bdevs can still be created */
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);

	for (nsid = 1; nsid <= ns_count; ++nsid) {
		snprintf(namebuf, sizeof(namebuf), "%sn%"PRIu32, controller_name, nsid);
		rc = create_bdev(controller_name, namebuf, nsid);
		CU_ASSERT_EQUAL(rc, 0);

		bdev = spdk_bdev_get_by_name(namebuf);
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		CU_ASSERT_TRUE(bdev->zoned);
	}

	delete_nvme_bdev_controller(nvme_ctrlr);

	free_controller(ctrlr);
}

static void
test_device_geometry(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	const char *controller_name = "nvme0";
	const char *bdev_name = "nvme0n1";
	struct spdk_ocssd_geometry_data geometry;
	struct spdk_bdev *bdev;
	int rc;

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 512,
		.num_chk = 64,
		.num_pu = 8,
		.num_grp = 4,
		.maxoc = 69,
		.maxocpu = 68,
		.ws_opt = 86,
		.lbaf = {
			.lbk_len = 9,
			.chk_len = 6,
			.pu_len = 3,
			.grp_len = 2,
		}
	};

	ctrlr = create_controller(&trid, 1, &geometry);
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);

	rc = create_bdev(controller_name, bdev_name, 1);
	CU_ASSERT_EQUAL(rc, 0);

	bdev = spdk_bdev_get_by_name(bdev_name);
	CU_ASSERT_EQUAL(bdev->blockcnt, geometry.clba *
			geometry.num_chk *
			geometry.num_pu *
			geometry.num_grp);
	CU_ASSERT_EQUAL(bdev->zone_size, geometry.clba);
	CU_ASSERT_EQUAL(bdev->optimal_open_zones, geometry.num_pu * geometry.num_grp);
	CU_ASSERT_EQUAL(bdev->max_open_zones, geometry.maxocpu);
	CU_ASSERT_EQUAL(bdev->write_unit_size, geometry.ws_opt);

	delete_nvme_bdev_controller(nvme_ctrlr);

	free_controller(ctrlr);
}

static uint64_t
generate_lba(const struct spdk_ocssd_geometry_data *geo, uint64_t lbk,
	     uint64_t chk, uint64_t pu, uint64_t grp)
{
	uint64_t lba, len;

	lba = lbk;
	len = geo->lbaf.lbk_len;
	CU_ASSERT(lbk < (1ull << geo->lbaf.lbk_len));

	lba |= chk << len;
	len += geo->lbaf.chk_len;
	CU_ASSERT(chk < (1ull << geo->lbaf.chk_len));

	lba |= pu << len;
	len += geo->lbaf.pu_len;
	CU_ASSERT(pu < (1ull << geo->lbaf.pu_len));

	lba |= grp << len;

	return lba;
}

static void
test_lba_translation(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	const char *controller_name = "nvme0";
	const char *bdev_name = "nvme0n1";
	struct spdk_ocssd_geometry_data geometry = {};
	struct bdev_ocssd_ns *ocssd_ns;
	struct spdk_bdev *bdev;
	uint64_t lba;
	int rc;

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 512,
		.num_chk = 64,
		.num_pu = 8,
		.num_grp = 4,
		.lbaf = {
			.lbk_len = 9,
			.chk_len = 6,
			.pu_len = 3,
			.grp_len = 2,
		}
	};

	ctrlr = create_controller(&trid, 1, &geometry);
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr->namespaces[0] != NULL);
	ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ctrlr->namespaces[0]);

	rc = create_bdev(controller_name, bdev_name, 1);
	CU_ASSERT_EQUAL(rc, 0);

	bdev = spdk_bdev_get_by_name(bdev_name);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, 0);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), 0);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size - 1);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, bdev->zone_size - 1, 0, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), bdev->zone_size - 1);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, 1, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), bdev->zone_size);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size * geometry.num_pu);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, 0, 1));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba),
			bdev->zone_size * geometry.num_pu);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size * geometry.num_pu + 68);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 68, 0, 0, 1));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba),
			bdev->zone_size * geometry.num_pu + 68);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size + 68);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 68, 0, 1, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), bdev->zone_size + 68);

	delete_nvme_bdev_controller(nvme_ctrlr);
	free_controller(ctrlr);

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 5120,
		.num_chk = 501,
		.num_pu = 9,
		.num_grp = 1,
		.lbaf = {
			.lbk_len = 13,
			.chk_len = 9,
			.pu_len = 4,
			.grp_len = 1,
		}
	};

	ctrlr = create_controller(&trid, 1, &geometry);
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr != NULL);

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr->namespaces[0] != NULL);
	ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ctrlr->namespaces[0]);

	rc = create_bdev(controller_name, bdev_name, 1);
	CU_ASSERT_EQUAL(rc, 0);

	bdev = spdk_bdev_get_by_name(bdev_name);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, 0);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), 0);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size - 1);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, bdev->zone_size - 1, 0, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), bdev->zone_size - 1);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, 1, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba), bdev->zone_size);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns, bdev->zone_size * (geometry.num_pu - 1));
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 0, geometry.num_pu - 1, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba),
			bdev->zone_size * (geometry.num_pu - 1));

	lba = bdev_ocssd_to_disk_lba(ocssd_ns,
				     bdev->zone_size * geometry.num_pu * geometry.num_grp);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 0, 1, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba),
			bdev->zone_size * geometry.num_pu * geometry.num_grp);

	lba = bdev_ocssd_to_disk_lba(ocssd_ns,
				     bdev->zone_size * geometry.num_pu * geometry.num_grp + 68);
	CU_ASSERT_EQUAL(lba, generate_lba(&geometry, 68, 1, 0, 0));
	CU_ASSERT_EQUAL(bdev_ocssd_from_disk_lba(ocssd_ns, lba),
			bdev->zone_size * geometry.num_pu * geometry.num_grp + 68);

	delete_nvme_bdev_controller(nvme_ctrlr);

	free_controller(ctrlr);
}

static void
get_zone_info_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	CU_ASSERT_EQUAL(g_zone_info_status, success);
}

static uint64_t
generate_chunk_offset(const struct spdk_ocssd_geometry_data *geo, uint64_t chk,
		      uint64_t pu, uint64_t grp)
{
	return grp * geo->num_pu * geo->num_chk +
	       pu * geo->num_chk + chk;
}

static struct spdk_bdev_io *
alloc_ocssd_io(void)
{
	struct spdk_bdev_io *bdev_io;

	bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct bdev_ocssd_io));
	SPDK_CU_ASSERT_FATAL(bdev_io != NULL);

	return bdev_io;
}

static struct spdk_ocssd_chunk_information_entry *
get_chunk_info(struct spdk_nvme_ctrlr *ctrlr, uint64_t offset)
{
	assert(offset < ctrlr->num_chunks);
	SPDK_CU_ASSERT_FATAL(offset < ctrlr->num_chunks);
	return &ctrlr->chunk_info[offset];
}

enum chunk_state {
	CHUNK_STATE_FREE,
	CHUNK_STATE_CLOSED,
	CHUNK_STATE_OPEN,
	CHUNK_STATE_OFFLINE
};

static void
set_chunk_state(struct spdk_ocssd_chunk_information_entry *chunk, enum chunk_state state)
{
	memset(&chunk->cs, 0, sizeof(chunk->cs));
	switch (state) {
	case CHUNK_STATE_FREE:
		chunk->cs.free = 1;
		break;
	case CHUNK_STATE_CLOSED:
		chunk->cs.closed = 1;
		break;
	case CHUNK_STATE_OPEN:
		chunk->cs.open = 1;
		break;
	case CHUNK_STATE_OFFLINE:
		chunk->cs.offline = 1;
		break;
	default:
		SPDK_CU_ASSERT_FATAL(0 && "Invalid state");
	}
}

static void
test_get_zone_info(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	const char *controller_name = "nvme0";
	const char *bdev_name = "nvme0n1";
	struct spdk_bdev *bdev;
	struct spdk_bdev_io *bdev_io;
	struct spdk_io_channel *ch;
	struct nvme_ctrlr_channel *ctrlr_ch;
#define MAX_ZONE_INFO_COUNT 64
	struct spdk_bdev_zone_info zone_info[MAX_ZONE_INFO_COUNT];
	struct spdk_ocssd_chunk_information_entry *chunk_info;
	struct spdk_ocssd_geometry_data geometry;
	uint64_t chunk_offset;
	int rc, offset;

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 512,
		.num_chk = 64,
		.num_pu = 8,
		.num_grp = 4,
		.lbaf = {
			.lbk_len = 9,
			.chk_len = 6,
			.pu_len = 3,
			.grp_len = 2,
		}
	};

	ctrlr = create_controller(&trid, 1, &geometry);
	nvme_ctrlr = create_nvme_bdev_controller(&trid, controller_name);

	rc = create_bdev(controller_name, bdev_name, 1);
	CU_ASSERT_EQUAL(rc, 0);

	bdev = spdk_bdev_get_by_name(bdev_name);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	ch = calloc(1, sizeof(*ch) + sizeof(*ctrlr_ch));
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ctrlr_ch = spdk_io_channel_get_ctx(ch);
	ctrlr_ch->ctrlr = nvme_ctrlr;
	ctrlr_ch->qpair = (struct spdk_nvme_qpair *)0x1;

	bdev_io = alloc_ocssd_io();
	bdev_io->internal.cb = get_zone_info_cb;
	bdev_io->internal.ch = (struct spdk_bdev_channel *)ch;
	bdev_io->bdev = bdev;
	bdev_io->type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;

	/* Verify empty zone */
	bdev_io->u.zone_mgmt.zone_id = 0;
	bdev_io->u.zone_mgmt.num_zones = 1;
	bdev_io->u.zone_mgmt.buf = &zone_info;
	chunk_info = get_chunk_info(ctrlr, 0);
	set_chunk_state(chunk_info, CHUNK_STATE_FREE);
	chunk_info->wp = 0;

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, 0);

	CU_ASSERT_EQUAL(zone_info[0].state, SPDK_BDEV_ZONE_STATE_EMPTY);
	CU_ASSERT_EQUAL(zone_info[0].zone_id, 0);
	CU_ASSERT_EQUAL(zone_info[0].write_pointer, 0);
	CU_ASSERT_EQUAL(zone_info[0].capacity, geometry.clba);

	/* Verify open zone */
	bdev_io->u.zone_mgmt.zone_id = bdev->zone_size;
	bdev_io->u.zone_mgmt.num_zones = 1;
	bdev_io->u.zone_mgmt.buf = &zone_info;
	chunk_info = get_chunk_info(ctrlr, generate_chunk_offset(&geometry, 0, 1, 0));
	set_chunk_state(chunk_info, CHUNK_STATE_OPEN);
	chunk_info->wp = chunk_info->slba + 68;
	chunk_info->cnlb = 511;
	chunk_info->ct.size_deviate = 1;

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, 0);

	CU_ASSERT_EQUAL(zone_info[0].state, SPDK_BDEV_ZONE_STATE_OPEN);
	CU_ASSERT_EQUAL(zone_info[0].zone_id, bdev->zone_size);
	CU_ASSERT_EQUAL(zone_info[0].write_pointer, bdev->zone_size + 68);
	CU_ASSERT_EQUAL(zone_info[0].capacity, chunk_info->cnlb);

	/* Verify offline zone at 2nd chunk */
	bdev_io->u.zone_mgmt.zone_id = bdev->zone_size * geometry.num_pu * geometry.num_grp;
	bdev_io->u.zone_mgmt.num_zones = 1;
	bdev_io->u.zone_mgmt.buf = &zone_info;
	chunk_info = get_chunk_info(ctrlr, generate_chunk_offset(&geometry, 1, 0, 0));
	set_chunk_state(chunk_info, CHUNK_STATE_OFFLINE);
	chunk_info->wp = chunk_info->slba;

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, 0);

	CU_ASSERT_EQUAL(zone_info[0].state, SPDK_BDEV_ZONE_STATE_OFFLINE);
	CU_ASSERT_EQUAL(zone_info[0].zone_id, bdev_io->u.zone_mgmt.zone_id);
	CU_ASSERT_EQUAL(zone_info[0].write_pointer, bdev_io->u.zone_mgmt.zone_id);

	/* Verify multiple zones at a time */
	bdev_io->u.zone_mgmt.zone_id = 0;
	bdev_io->u.zone_mgmt.num_zones = MAX_ZONE_INFO_COUNT;
	bdev_io->u.zone_mgmt.buf = &zone_info;

	for (offset = 0; offset < MAX_ZONE_INFO_COUNT; ++offset) {
		chunk_offset = generate_chunk_offset(&geometry,
						     (offset / (geometry.num_grp * geometry.num_pu)) % geometry.num_chk,
						     offset % geometry.num_pu,
						     (offset / geometry.num_pu) % geometry.num_grp);


		chunk_info = get_chunk_info(ctrlr, chunk_offset);
		set_chunk_state(chunk_info, CHUNK_STATE_OPEN);
		chunk_info->wp = chunk_info->slba + 68;
		chunk_info->ct.size_deviate = 0;
	}

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, 0);

	for (offset = 0; offset < MAX_ZONE_INFO_COUNT; ++offset) {
		CU_ASSERT_EQUAL(zone_info[offset].state, SPDK_BDEV_ZONE_STATE_OPEN);
		CU_ASSERT_EQUAL(zone_info[offset].zone_id, bdev->zone_size * offset);
		CU_ASSERT_EQUAL(zone_info[offset].write_pointer, bdev->zone_size * offset + 68);
		CU_ASSERT_EQUAL(zone_info[offset].capacity, geometry.clba);
	}

	/* Verify misaligned start zone LBA */
	bdev_io->u.zone_mgmt.zone_id = 1;
	bdev_io->u.zone_mgmt.num_zones = MAX_ZONE_INFO_COUNT;
	bdev_io->u.zone_mgmt.buf = &zone_info;

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, -EINVAL);

	/* Verify correct NVMe error forwarding */
	bdev_io->u.zone_mgmt.zone_id = 0;
	bdev_io->u.zone_mgmt.num_zones = MAX_ZONE_INFO_COUNT;
	bdev_io->u.zone_mgmt.buf = &zone_info;
	chunk_info = get_chunk_info(ctrlr, 0);
	set_chunk_state(chunk_info, CHUNK_STATE_FREE);

	rc = _bdev_ocssd_submit_request(ch, bdev_io);
	CU_ASSERT_EQUAL(rc, 0);
	g_chunk_info_cpl = (struct spdk_nvme_cpl) {
		.status = {
			.sct = SPDK_NVME_SCT_GENERIC,
			.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR
		}
	};
	g_zone_info_status = false;

	g_chunk_info_cpl = (struct spdk_nvme_cpl) {};
	g_zone_info_status = true;

	delete_nvme_bdev_controller(nvme_ctrlr);

	free(bdev_io);
	free(ch);
	free_controller(ctrlr);
}

int
main(int argc, const char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ocssd", NULL, NULL);

	CU_ADD_TEST(suite, test_create_controller);
	CU_ADD_TEST(suite, test_device_geometry);
	CU_ADD_TEST(suite, test_lba_translation);
	CU_ADD_TEST(suite, test_get_zone_info);

	g_thread = spdk_thread_create("test", NULL);
	spdk_set_thread(g_thread);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();

	spdk_thread_exit(g_thread);
	while (!spdk_thread_is_exited(g_thread)) {
		spdk_thread_poll(g_thread, 0, 0);
	}
	spdk_thread_destroy(g_thread);

	CU_cleanup_registry();

	return num_failures;
}
