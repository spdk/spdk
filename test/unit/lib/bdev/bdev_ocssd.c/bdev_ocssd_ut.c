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
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk_internal/mock.h"

#include "bdev/nvme/bdev_ocssd.c"
#include "bdev/nvme/common.c"
#include "common/lib/test_env.c"

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_io_complete_nvme_status, (struct spdk_bdev_io *bdev_io, int sct, int sc));
DEFINE_STUB(spdk_nvme_ctrlr_is_ocssd_supported, bool, (struct spdk_nvme_ctrlr *ctrlr), true);
DEFINE_STUB(spdk_nvme_ns_get_extended_sector_size, uint32_t, (struct spdk_nvme_ns *ns), 4096);

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
};

struct spdk_nvme_ctrlr {
	struct spdk_nvme_transport_id trid;
	struct spdk_ocssd_geometry_data geometry;
	struct spdk_nvme_qpair *admin_qpair;
	struct spdk_nvme_ns *ns;
	uint32_t ns_count;

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
	LIST_REMOVE(ctrlr, list);

	spdk_nvme_ctrlr_free_io_qpair(ctrlr->admin_qpair);
	free(ctrlr->ns);
	free(ctrlr);
}

static struct spdk_nvme_ctrlr *
create_controller(const struct spdk_nvme_transport_id *trid, uint32_t ns_count,
		  const struct spdk_ocssd_geometry_data *geo)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t nsid;

	SPDK_CU_ASSERT_FATAL(!find_controller(trid));

	ctrlr = calloc(1, sizeof(*ctrlr));
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);

	ctrlr->ns = calloc(ns_count, sizeof(*ctrlr->ns));
	SPDK_CU_ASSERT_FATAL(ctrlr->ns != NULL);

	for (nsid = 0; nsid < ns_count; ++nsid) {
		ctrlr->ns[nsid].nsid = nsid + 1;
	}

	ctrlr->geometry = *geo;
	ctrlr->trid = *trid;
	ctrlr->ns_count = ns_count;
	ctrlr->admin_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(ctrlr->admin_qpair != NULL);

	LIST_INSERT_HEAD(&g_ctrlr_list, ctrlr, list);

	return ctrlr;
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

struct spdk_nvme_ns *
spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	SPDK_CU_ASSERT_FATAL(nsid > 0 && nsid <= ctrlr->ns_count);
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

struct geometry_ctx {
	spdk_nvme_cmd_cb cb_fn;
	void *cb_arg;
};

static void
geometry_cb(void *_ctx)
{
	struct geometry_ctx *ctx = _ctx;
	struct spdk_nvme_cpl cpl = {};

	ctx->cb_fn(ctx->cb_arg, &cpl);
	free(ctx);
}

int
spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   void *payload, uint32_t payload_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct geometry_ctx *ctx;

	CU_ASSERT_EQUAL(payload_size, sizeof(ctrlr->geometry));
	memcpy(payload, &ctrlr->geometry, sizeof(ctrlr->geometry));

	ctx = malloc(sizeof(*ctx));
	SPDK_CU_ASSERT_FATAL(ctx != NULL);

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/*
	 * Defer the callback function via spdk_thread_send_msg to avoid
	 * scan-build's false complaints about use-after-free of the attach_ctx
	 * in spdk_bdev_ocssd_attach_controller.
	 */
	spdk_thread_send_msg(g_thread, geometry_cb, ctx);
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}

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

static void
attach_controller_cb(void *_ctx)
{}

static void
test_attach_controller(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	struct spdk_ocssd_geometry_data geo = {};
#define MAX_BDEV_NAMES 128
	const char *names[MAX_BDEV_NAMES] = {};
	const char *controller_name = "nvme0";
	const size_t ns_count = 16;
	char namebuf[128];
	size_t num_bdevs = MAX_BDEV_NAMES, nsid;
	int rc;

	ctrlr = create_controller(&trid, ns_count, &geo);
	rc = spdk_bdev_ocssd_attach_controller(&trid, controller_name, names, &num_bdevs,
					       attach_controller_cb, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(num_bdevs, ns_count);

	for (nsid = 1; nsid <= ns_count; ++nsid) {
		struct spdk_bdev *bdev;

		snprintf(namebuf, sizeof(namebuf), "%sn%zu", controller_name, nsid);
		SPDK_CU_ASSERT_FATAL(names[nsid - 1] != NULL);
		CU_ASSERT_EQUAL(strncmp(namebuf, names[nsid - 1], sizeof(namebuf)), 0);

		bdev = spdk_bdev_get_by_name(namebuf);
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		CU_ASSERT_TRUE(bdev->zoned);
	}

	spdk_bdev_ocssd_detach_controller(controller_name);
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_list));

	/* Verify the name[] are only filled up to num_bdevs */
	num_bdevs = 4;
	memset(names, 0, sizeof(names));

	rc = spdk_bdev_ocssd_attach_controller(&trid, controller_name, names, &num_bdevs,
					       attach_controller_cb, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(num_bdevs, ns_count);
	for (nsid = 1; nsid <= 4; nsid++) {
		struct spdk_bdev *bdev;

		snprintf(namebuf, sizeof(namebuf), "%sn%zu", controller_name, nsid);
		SPDK_CU_ASSERT_FATAL(names[nsid - 1] != NULL);
		CU_ASSERT_EQUAL(strncmp(namebuf, names[nsid - 1], sizeof(namebuf)), 0);

		bdev = spdk_bdev_get_by_name(namebuf);
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		CU_ASSERT_TRUE(bdev->zoned);
	}

	for (; nsid <= ns_count; ++nsid) {
		CU_ASSERT_PTR_NULL(names[nsid - 1]);
	}

	spdk_bdev_ocssd_detach_controller(controller_name);
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_list));
	num_bdevs = ns_count;

	/* Verify correct error is returned in case the controller cannot be found */
	trid.traddr[0] = ~trid.traddr[0];
	rc = spdk_bdev_ocssd_attach_controller(&trid, controller_name, names, &num_bdevs,
					       attach_controller_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENODEV);
	trid.traddr[0] = ~trid.traddr[0];

	/* Verify the bdevs aren't created if the controller doesn't support OC */
	MOCK_SET(spdk_nvme_ctrlr_is_ocssd_supported, false);
	rc = spdk_bdev_ocssd_attach_controller(&trid, controller_name, names, &num_bdevs,
					       attach_controller_cb, NULL);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	MOCK_SET(spdk_nvme_ctrlr_is_ocssd_supported, true);

	free_controller(ctrlr);
}

static void
test_device_geometry(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_transport_id trid = { .traddr = "00:00:00" };
	const char *controller_name = "nvme0";
	struct spdk_ocssd_geometry_data geometry;
	const char *names[1] = {};
	struct spdk_bdev *bdev;
	size_t num_bdevs = 1, ns_count = 1;
	int rc;

	geometry = (struct spdk_ocssd_geometry_data) {
		.clba = 512,
		.num_chk = 64,
		.num_pu = 8,
		.num_grp = 4,
		.maxoc = 69,
		.maxocpu = 68,
		.ws_opt = 86,
	};

	ctrlr = create_controller(&trid, ns_count, &geometry);
	rc = spdk_bdev_ocssd_attach_controller(&trid, controller_name, names, &num_bdevs,
					       attach_controller_cb, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(num_bdevs, ns_count);

	bdev = spdk_bdev_get_by_name(names[0]);
	CU_ASSERT_EQUAL(bdev->blockcnt, geometry.clba *
			geometry.num_chk *
			geometry.num_pu *
			geometry.num_grp);
	CU_ASSERT_EQUAL(bdev->zone_size, geometry.clba);
	CU_ASSERT_EQUAL(bdev->optimal_open_zones, geometry.num_pu * geometry.num_grp);
	CU_ASSERT_EQUAL(bdev->max_open_zones, geometry.maxocpu);
	CU_ASSERT_EQUAL(bdev->write_unit_size, geometry.ws_opt);

	spdk_bdev_ocssd_detach_controller(controller_name);
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {}
	CU_ASSERT(TAILQ_EMPTY(&g_bdev_list));

	free_controller(ctrlr);
}

int
main(int argc, const char **argv)
{
	CU_pSuite       suite = NULL;
	unsigned int    num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ocssd", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_attach_controller", test_attach_controller) == NULL ||
		CU_add_test(suite, "test_device_geometry", test_device_geometry) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_thread = spdk_thread_create("test", NULL);
	spdk_set_thread(g_thread);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();

	spdk_thread_exit(g_thread);
	spdk_thread_destroy(g_thread);

	CU_cleanup_registry();

	return num_failures;
}
