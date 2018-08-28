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

#include "common/lib/test_env.c"
#include "spdk_cunit.h"
#include "spdk_internal/mock.h"

#include "nvmf/subsystem.c"

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

DEFINE_STUB(spdk_bdev_module_claim_bdev,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);

DEFINE_STUB_V(spdk_bdev_module_release_bdev,
	      (struct spdk_bdev *bdev));

static void
_subsystem_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

static void
subsystem_ns_remove_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

struct spdk_event *
spdk_event_allocate(uint32_t core, spdk_event_fn fn, void *arg1, void *arg2)
{
	return NULL;
}

void
spdk_event_call(struct spdk_event *event)
{

}

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	return 0;
}

void
spdk_nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				      struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

bool
spdk_nvmf_transport_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	return false;
}

static struct spdk_nvmf_transport g_transport = {};

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(enum spdk_nvme_transport_type type,
			   struct spdk_nvmf_transport_opts *tprt_opts)
{
	if (type == SPDK_NVME_TRANSPORT_RDMA) {
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
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, enum spdk_nvme_transport_type trtype)
{
	if (trtype == SPDK_NVME_TRANSPORT_RDMA) {
		return &g_transport;
	}

	return NULL;
}

int
spdk_nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem)
{
	return 0;
}

int
spdk_nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
				   struct spdk_nvmf_subsystem *subsystem,
				   spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	return 0;
}

void
spdk_nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem,
				      spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
spdk_nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem,
				     spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
spdk_nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
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
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
}

void
spdk_nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **desc)
{
	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
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
	struct spdk_bdev bdev1 = {}, bdev2 = {};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;

	tgt.opts.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.opts.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	/* Allow NSID to be assigned automatically */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev1, &ns_opts, sizeof(ns_opts));
	/* NSID 1 is the first unused ID */
	CU_ASSERT(nsid == 1);
	CU_ASSERT(subsystem.max_nsid == 1);
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &bdev1);

	/* Request a specific NSID */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, &ns_opts, sizeof(ns_opts));
	CU_ASSERT(nsid == 5);
	CU_ASSERT(subsystem.max_nsid == 5);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &bdev2);

	/* Request an NSID that is already in use */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, &ns_opts, sizeof(ns_opts));
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 5);

	/* Request 0xFFFFFFFF (invalid NSID, reserved for broadcast) */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 0xFFFFFFFF;
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, &ns_opts, sizeof(ns_opts));
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 5);

	spdk_nvmf_subsystem_remove_ns(&subsystem, 1, subsystem_ns_remove_cb, NULL);
	spdk_nvmf_subsystem_remove_ns(&subsystem, 5, subsystem_ns_remove_cb, NULL);

	free(subsystem.ns);
	free(tgt.subsystems);
}

static void
nvmf_test_create_subsystem(void)
{
	struct spdk_nvmf_tgt tgt = {};
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;

	tgt.opts.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.opts.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
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

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "create_subsystem", nvmf_test_create_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_subsystem_add_ns", test_spdk_nvmf_subsystem_add_ns) == NULL ||
		CU_add_test(suite, "nvmf_subsystem_set_sn", test_spdk_nvmf_subsystem_set_sn) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	spdk_allocate_thread(_subsystem_send_msg, NULL, NULL, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();

	return num_failures;
}
