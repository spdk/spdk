/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "common/lib/ut_multithread.c"
#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/thread.h"

#include "nvmf/subsystem.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB(spdk_bdev_module_claim_bdev,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);

DEFINE_STUB_V(spdk_bdev_module_release_bdev,
	      (struct spdk_bdev *bdev));

DEFINE_STUB(spdk_bdev_get_block_size, uint32_t,
	    (const struct spdk_bdev *bdev), 512);

DEFINE_STUB(spdk_bdev_get_md_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);

DEFINE_STUB(spdk_bdev_is_md_interleaved, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_nvmf_transport_stop_listen,
	    int,
	    (struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid), 0);

DEFINE_STUB_V(nvmf_update_discovery_log,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn));

DEFINE_STUB(spdk_nvmf_qpair_disconnect,
	    int,
	    (struct spdk_nvmf_qpair *qpair,
	     nvmf_qpair_disconnect_cb cb_fn, void *ctx), 0);

DEFINE_STUB(nvmf_transport_find_listener,
	    struct spdk_nvmf_listener *,
	    (struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid), NULL);

DEFINE_STUB(spdk_nvmf_request_complete,
	    int,
	    (struct spdk_nvmf_request *req), 0);

DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice,
	    int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_transport_id_trtype_str,
	    const char *,
	    (enum spdk_nvme_transport_type trtype), NULL);

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	return 0;
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

static struct spdk_nvmf_transport g_transport = {};

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name,
			   struct spdk_nvmf_transport_opts *tprt_opts)
{
	if (strcasecmp(transport_name, spdk_nvme_transport_id_trtype_str(SPDK_NVME_TRANSPORT_RDMA))) {
		return &g_transport;
	}

	return NULL;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	return NULL;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	if (strncmp(transport_name, SPDK_NVME_TRANSPORT_NAME_RDMA, SPDK_NVMF_TRSTRING_MAX_LEN)) {
		return &g_transport;
	}

	return NULL;
}

int
nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem)
{
	return 0;
}

int
nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	return 0;
}

void
nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_subsystem *subsystem,
				spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

int
spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str)
{
	if (trtype == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "PCIe") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_PCIE;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_RDMA;
	} else {
		return -ENOENT;
	}
	return 0;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

void
nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
}

static struct spdk_nvmf_ctrlr *g_ns_changed_ctrlr = NULL;
static uint32_t g_ns_changed_nsid = 0;
void
nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	g_ns_changed_ctrlr = ctrlr;
	g_ns_changed_nsid = nsid;
}

static struct spdk_bdev g_bdevs[] = {
	{ .name = "bdev1" },
	{ .name = "bdev2" },
};

struct spdk_bdev_desc {
	struct spdk_bdev	*bdev;
};

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	size_t i;

	for (i = 0; i < sizeof(g_bdevs); i++) {
		if (strcmp(bdev_name, g_bdevs[i].name) == 0) {

			desc = calloc(1, sizeof(*desc));
			SPDK_CU_ASSERT_FATAL(desc != NULL);

			desc->bdev = &g_bdevs[i];
			*_desc = desc;
			return 0;
		}
	}

	return -EINVAL;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	free(desc);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

static void
test_spdk_nvmf_subsystem_add_ns(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 0,
		.ns = NULL,
		.tgt = &tgt
	};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;
	int rc;

	tgt.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	/* Allow NSID to be assigned automatically */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev1", &ns_opts, sizeof(ns_opts), NULL);
	/* NSID 1 is the first unused ID */
	CU_ASSERT(nsid == 1);
	CU_ASSERT(subsystem.max_nsid == 1);
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[nsid - 1]);

	/* Request a specific NSID */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 5);
	CU_ASSERT(subsystem.max_nsid == 5);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[1]);

	/* Request an NSID that is already in use */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 5);

	/* Request 0xFFFFFFFF (invalid NSID, reserved for broadcast) */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 0xFFFFFFFF;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 5);

	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, 1);
	CU_ASSERT(rc == 0);
	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, 5);
	CU_ASSERT(rc == 0);

	free(subsystem.ns);
	free(tgt.subsystems);
}

static void
nvmf_test_create_subsystem(void)
{
	struct spdk_nvmf_tgt tgt = {};
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;

	tgt.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* valid name with complex reverse domain */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk-full--rev-domain.name:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* Valid name discovery controller */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);


	/* Invalid name, no user supplied string */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Valid name, only contains top-level domain name */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* Invalid name, domain label > 63 characters */
	snprintf(nqn, sizeof(nqn),
		 "nqn.2016-06.io.abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz:sub");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label starts with digit */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.3spdk:sub");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label starts with - */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.-spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label ends with - */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk-:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label with multiple consecutive periods */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io..spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Longest valid name */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* Invalid name, too long */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	memset(nqn + strlen(nqn), 'a', 224 - strlen(nqn));
	nqn[224] = '\0';
	CU_ASSERT(strlen(nqn) == 224);
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	CU_ASSERT(subsystem == NULL);

	/* Valid name using uuid format */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:11111111-aaaa-bbdd-FFEE-123456789abc");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* Invalid name user string contains an invalid utf-8 character */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:\xFFsubsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Valid name with non-ascii but valid utf-8 characters */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:\xe1\x8a\x88subsystem1\xca\x80");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_subsystem_destroy(subsystem);

	/* Invalid uuid (too long) */
	snprintf(nqn, sizeof(nqn),
		 "nqn.2014-08.org.nvmexpress:uuid:11111111-aaaa-bbdd-FFEE-123456789abcdef");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid uuid (dashes placed incorrectly) */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:111111-11aaaa-bbdd-FFEE-123456789abc");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid uuid (invalid characters in uuid) */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:111hg111-aaaa-bbdd-FFEE-123456789abc");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	free(tgt.subsystems);
}

static void
test_spdk_nvmf_subsystem_set_sn(void)
{
	struct spdk_nvmf_subsystem subsystem = {};

	/* Basic valid serial number */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "abcd xyz") == 0);
	CU_ASSERT(strcmp(subsystem.sn, "abcd xyz") == 0);

	/* Exactly 20 characters (valid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "12345678901234567890") == 0);
	CU_ASSERT(strcmp(subsystem.sn, "12345678901234567890") == 0);

	/* 21 characters (too long, invalid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "123456789012345678901") < 0);

	/* Non-ASCII characters (invalid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "abcd\txyz") < 0);
}

/*
 * Reservation Unit Test Configuration
 *       --------             --------    --------
 *      | Host A |           | Host B |  | Host C |
 *       --------             --------    --------
 *      /        \               |           |
 *  --------   --------       -------     -------
 * |Ctrlr1_A| |Ctrlr2_A|     |Ctrlr_B|   |Ctrlr_C|
 *  --------   --------       -------     -------
 *    \           \              /           /
 *     \           \            /           /
 *      \           \          /           /
 *      --------------------------------------
 *     |            NAMESPACE 1               |
 *      --------------------------------------
 */
static struct spdk_nvmf_subsystem g_subsystem;
static struct spdk_nvmf_ctrlr g_ctrlr1_A, g_ctrlr2_A, g_ctrlr_B, g_ctrlr_C;
static struct spdk_nvmf_ns g_ns;
static struct spdk_bdev g_bdev;
struct spdk_nvmf_subsystem_pg_ns_info g_ns_info;

void
nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr)
{
}

static void
ut_reservation_init(void)
{

	TAILQ_INIT(&g_subsystem.ctrlrs);

	memset(&g_ns, 0, sizeof(g_ns));
	TAILQ_INIT(&g_ns.registrants);
	g_ns.subsystem = &g_subsystem;
	g_ns.ptpl_file = NULL;
	g_ns.ptpl_activated = false;
	spdk_uuid_generate(&g_bdev.uuid);
	g_ns.bdev = &g_bdev;

	/* Host A has two controllers */
	spdk_uuid_generate(&g_ctrlr1_A.hostid);
	TAILQ_INIT(&g_ctrlr1_A.log_head);
	g_ctrlr1_A.subsys = &g_subsystem;
	g_ctrlr1_A.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr1_A, link);
	spdk_uuid_copy(&g_ctrlr2_A.hostid, &g_ctrlr1_A.hostid);
	TAILQ_INIT(&g_ctrlr2_A.log_head);
	g_ctrlr2_A.subsys = &g_subsystem;
	g_ctrlr2_A.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr2_A, link);

	/* Host B has 1 controller */
	spdk_uuid_generate(&g_ctrlr_B.hostid);
	TAILQ_INIT(&g_ctrlr_B.log_head);
	g_ctrlr_B.subsys = &g_subsystem;
	g_ctrlr_B.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr_B, link);

	/* Host C has 1 controller */
	spdk_uuid_generate(&g_ctrlr_C.hostid);
	TAILQ_INIT(&g_ctrlr_C.log_head);
	g_ctrlr_C.subsys = &g_subsystem;
	g_ctrlr_C.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr_C, link);
}

static void
ut_reservation_deinit(void)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct spdk_nvmf_ctrlr *ctrlr, *ctrlr_tmp;

	TAILQ_FOREACH_SAFE(reg, &g_ns.registrants, link, tmp) {
		TAILQ_REMOVE(&g_ns.registrants, reg, link);
		free(reg);
	}
	TAILQ_FOREACH_SAFE(log, &g_ctrlr1_A.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr1_A.log_head, log, link);
		free(log);
	}
	g_ctrlr1_A.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr2_A.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr2_A.log_head, log, link);
		free(log);
	}
	g_ctrlr2_A.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr_B.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr_B.log_head, log, link);
		free(log);
	}
	g_ctrlr_B.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr_C.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr_C.log_head, log, link);
		free(log);
	}
	g_ctrlr_C.num_avail_log_pages = 0;

	TAILQ_FOREACH_SAFE(ctrlr, &g_subsystem.ctrlrs, link, ctrlr_tmp) {
		TAILQ_REMOVE(&g_subsystem.ctrlrs, ctrlr, link);
	}
}

static struct spdk_nvmf_request *
ut_reservation_build_req(uint32_t length)
{
	struct spdk_nvmf_request *req;

	req = calloc(1, sizeof(*req));
	assert(req != NULL);

	req->data = calloc(1, length);
	assert(req->data != NULL);
	req->length = length;

	req->cmd = (union nvmf_h2c_msg *)calloc(1, sizeof(union nvmf_h2c_msg));
	assert(req->cmd != NULL);

	req->rsp = (union nvmf_c2h_msg *)calloc(1, sizeof(union nvmf_c2h_msg));
	assert(req->rsp != NULL);

	return req;
}

static void
ut_reservation_free_req(struct spdk_nvmf_request *req)
{
	free(req->cmd);
	free(req->rsp);
	free(req->data);
	free(req);
}

static void
ut_reservation_build_register_request(struct spdk_nvmf_request *req,
				      uint8_t rrega, uint8_t iekey,
				      uint8_t cptpl, uint64_t crkey,
				      uint64_t nrkey)
{
	struct spdk_nvme_reservation_register_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	key.crkey = crkey;
	key.nrkey = nrkey;
	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_register.rrega = rrega;
	cmd->cdw10_bits.resv_register.iekey = iekey;
	cmd->cdw10_bits.resv_register.cptpl = cptpl;
	memcpy(req->data, &key, sizeof(key));
}

static void
ut_reservation_build_acquire_request(struct spdk_nvmf_request *req,
				     uint8_t racqa, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey,
				     uint64_t prkey)
{
	struct spdk_nvme_reservation_acquire_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	key.crkey = crkey;
	key.prkey = prkey;
	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_acquire.racqa = racqa;
	cmd->cdw10_bits.resv_acquire.iekey = iekey;
	cmd->cdw10_bits.resv_acquire.rtype = rtype;
	memcpy(req->data, &key, sizeof(key));
}

static void
ut_reservation_build_release_request(struct spdk_nvmf_request *req,
				     uint8_t rrela, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_release.rrela = rrela;
	cmd->cdw10_bits.resv_release.iekey = iekey;
	cmd->cdw10_bits.resv_release.rtype = rtype;
	memcpy(req->data, &crkey, sizeof(crkey));
}

/*
 * Construct four registrants for other test cases.
 *
 * g_ctrlr1_A register with key 0xa1.
 * g_ctrlr2_A register with key 0xa1.
 * g_ctrlr_B register with key 0xb1.
 * g_ctrlr_C register with key 0xc1.
 * */
static void
ut_reservation_build_registrants(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);
	gen = g_ns.gen;

	/* TEST CASE: g_ctrlr1_A register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 1);

	/* TEST CASE: g_ctrlr2_A register with a new key, because it has same
	 * Host Identifier with g_ctrlr1_A, so the register key should same.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa2);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr2_A, req);
	/* Reservation conflict for other key than 0xa1 */
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* g_ctrlr_B register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xb1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xb1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 2);

	/* g_ctrlr_C register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xc1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xc1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 3);

	ut_reservation_free_req(req);
}

static void
test_reservation_register(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* TEST CASE: Replace g_ctrlr1_A with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      0, 0, 0xa1, 0xa11);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa11);

	/* TEST CASE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xa11, 0x0);
	gen = g_ns.gen;
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa11);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen);

	/* TEST CASE: g_ctrlr_C unregister with IEKEY enabled */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      1, 0, 0, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: g_ctrlr_B unregister with correct key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xb1, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: g_ctrlr1_A unregister with correct key,
	 * reservation should be removed as well.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xa11, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_register_with_ptpl(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	bool update_sgroup = false;
	int rc;
	struct spdk_nvmf_reservation_info info;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* TEST CASE: No persistent file, register with PTPL enabled will fail */
	g_ns.ptpl_file = NULL;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == false);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc != SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: Enable PTPL */
	g_ns.ptpl_file = "/tmp/Ns1PR.cfg";
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == true);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &reg->hostid));
	/* Load reservation information from configuration file */
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_load_reservation(g_ns.ptpl_file, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);

	/* TEST CASE: Disable PTPL */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == false);
	rc = nvmf_ns_load_reservation(g_ns.ptpl_file, &info);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	unlink(g_ns.ptpl_file);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_acquire_preempt_1(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	gen = g_ns.gen;
	/* ACQUIRE: Host A with g_ctrlr1_A acquire reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE.
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen);

	/* TEST CASE: g_ctrlr1_A holds the reservation, g_ctrlr_B preempt g_ctrl1_A,
	 * g_ctrl1_A registrant is unregistred.
	 */
	gen = g_ns.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1, 0xa1);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.gen > gen);

	/* TEST CASE: g_ctrlr_B holds the reservation, g_ctrlr_C preempt g_ctrlr_B
	 * with valid key and PRKEY set to 0, all registrants other the host that issued
	 * the command are unregistered.
	 */
	gen = g_ns.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xc1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr2_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.gen > gen);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_acquire_release_with_ptpl(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	bool update_sgroup = false;
	struct spdk_uuid holder_uuid;
	int rc;
	struct spdk_nvmf_reservation_info info;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* TEST CASE: Enable PTPL */
	g_ns.ptpl_file = "/tmp/Ns1PR.cfg";
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == true);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &reg->hostid));
	/* Load reservation information from configuration file */
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_load_reservation(g_ns.ptpl_file, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);

	/* TEST CASE: Acquire the reservation */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	update_sgroup = nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_load_reservation(g_ns.ptpl_file, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);
	SPDK_CU_ASSERT_FATAL(info.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	SPDK_CU_ASSERT_FATAL(info.crkey == 0xa1);
	spdk_uuid_parse(&holder_uuid, info.holder_uuid);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &holder_uuid));

	/* TEST CASE: Release the reservation */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1);
	update_sgroup = nvmf_ns_reservation_release(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_load_reservation(g_ns.ptpl_file, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.rtype == 0);
	SPDK_CU_ASSERT_FATAL(info.crkey == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);
	unlink(g_ns.ptpl_file);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* ACQUIRE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);

	/* Test Case: Host B release the reservation */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	/* Test Case: Host C clear the registrants */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_CLEAR, 0,
					     0, 0xc1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr2_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

void
nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_ns *ns,
				  enum spdk_nvme_reservation_notification_log_page_type type)
{
	ctrlr->num_avail_log_pages++;
}

static void
test_reservation_unregister_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B unregister the registration.
	 * Reservation release notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C only for
	 * SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY or SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY
	 * type.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xb1, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B release the reservation.
	 * Reservation release notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release_notification_write_exclusive(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B release the reservation.
	 * Because the reservation type is SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
	 * no reservation notification occurs.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 5;
	g_ctrlr2_A.num_avail_log_pages = 5;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 5;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_clear_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B clear the reservation.
	 * Reservation Preempted notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_CLEAR, 0,
					     0, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_preempt_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_C preempt g_ctrlr_B,
	 * g_ctrlr_B registrant is unregistred, and reservation is preempted.
	 * Registration Preempted notification sends to g_ctrlr_B.
	 * Reservation Preempted notification sends to g_ctrlr1_A/g_ctrlr2_A.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 0;
	g_ctrlr_C.num_avail_log_pages = 5;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xc1, 0xb1);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_spdk_nvmf_ns_event(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 0,
		.ns = NULL,
		.tgt = &tgt
	};
	struct spdk_nvmf_ctrlr ctrlr = {
		.subsys = &subsystem
	};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;
	struct spdk_bdev *bdev;

	tgt.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	/* Add one namespace */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev1", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 1);
	CU_ASSERT(NULL != subsystem.ns[0]);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[nsid - 1]);

	bdev = subsystem.ns[nsid - 1]->bdev;

	/* Add one controller */
	TAILQ_INIT(&subsystem.ctrlrs);
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &ctrlr, link);

	/* Namespace resize event */
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	g_ns_changed_nsid = 0xFFFFFFFF;
	g_ns_changed_ctrlr = NULL;
	nvmf_ns_event(SPDK_BDEV_EVENT_RESIZE, bdev, subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_PAUSING == subsystem.state);

	poll_threads();
	CU_ASSERT(1 == g_ns_changed_nsid);
	CU_ASSERT(&ctrlr == g_ns_changed_ctrlr);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_ACTIVE == subsystem.state);

	/* Namespace remove event */
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	g_ns_changed_nsid = 0xFFFFFFFF;
	g_ns_changed_ctrlr = NULL;
	nvmf_ns_event(SPDK_BDEV_EVENT_REMOVE, bdev, subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_PAUSING == subsystem.state);
	CU_ASSERT(0xFFFFFFFF == g_ns_changed_nsid);
	CU_ASSERT(NULL == g_ns_changed_ctrlr);

	poll_threads();
	CU_ASSERT(1 == g_ns_changed_nsid);
	CU_ASSERT(&ctrlr == g_ns_changed_ctrlr);
	CU_ASSERT(NULL == subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_ACTIVE == subsystem.state);

	free(subsystem.ns);
	free(tgt.subsystems);
}


int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, nvmf_test_create_subsystem);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_add_ns);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_set_sn);
	CU_ADD_TEST(suite, test_reservation_register);
	CU_ADD_TEST(suite, test_reservation_register_with_ptpl);
	CU_ADD_TEST(suite, test_reservation_acquire_preempt_1);
	CU_ADD_TEST(suite, test_reservation_acquire_release_with_ptpl);
	CU_ADD_TEST(suite, test_reservation_release);
	CU_ADD_TEST(suite, test_reservation_unregister_notification);
	CU_ADD_TEST(suite, test_reservation_release_notification);
	CU_ADD_TEST(suite, test_reservation_release_notification_write_exclusive);
	CU_ADD_TEST(suite, test_reservation_clear_notification);
	CU_ADD_TEST(suite, test_reservation_preempt_notification);
	CU_ADD_TEST(suite, test_spdk_nvmf_ns_event);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
