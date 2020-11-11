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
#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "nvmf/ctrlr_discovery.c"
#include "nvmf/subsystem.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB_V(spdk_bdev_module_release_bdev,
	      (struct spdk_bdev *bdev));

DEFINE_STUB(spdk_bdev_get_block_size, uint32_t,
	    (const struct spdk_bdev *bdev), 512);

DEFINE_STUB(spdk_nvmf_transport_stop_listen,
	    int,
	    (struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid), 0);

DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));

DEFINE_STUB(nvmf_ctrlr_async_event_discovery_log_change_notice,
	    int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvmf_qpair_disconnect, int,
	    (struct spdk_nvmf_qpair *qpair,
	     nvmf_qpair_disconnect_cb cb_fn, void *ctx), 0);

DEFINE_STUB(spdk_bdev_open_ext, int,
	    (const char *bdev_name, bool write,	spdk_bdev_event_cb_t event_cb,
	     void *event_ctx, struct spdk_bdev_desc **desc), 0);

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *,
	    (struct spdk_bdev_desc *desc), NULL);

DEFINE_STUB(spdk_bdev_get_md_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);

DEFINE_STUB(spdk_bdev_is_md_interleaved, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_module_claim_bdev, int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);

DEFINE_STUB_V(nvmf_ctrlr_reservation_notice_log,
	      (struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvmf_ns *ns,
	       enum spdk_nvme_reservation_notification_log_page_type type));

DEFINE_STUB(spdk_nvmf_request_complete, int,
	    (struct spdk_nvmf_request *req), -1);

DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice, int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *,
	    (enum spdk_nvme_transport_type trtype), NULL);

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

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	return 0;
}

static struct spdk_nvmf_listener g_listener = {};

struct spdk_nvmf_listener *
nvmf_transport_find_listener(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvme_transport_id *trid)
{
	return &g_listener;
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

struct spdk_nvmf_transport_ops g_transport_ops = {};

static struct spdk_nvmf_transport g_transport = {
	.ops = &g_transport_ops
};

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
	return &g_transport;
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

void
nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
}

void
nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
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

static void
_subsystem_add_listen_done(void *cb_arg, int status)
{
	SPDK_CU_ASSERT_FATAL(status == 0);
}

static void
test_discovery_log(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem *subsystem;
	uint8_t buffer[8192];
	struct iovec iov;
	struct spdk_nvmf_discovery_log_page *disc_log;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvme_transport_id trid = {};

	iov.iov_base = buffer;
	iov.iov_len = 8192;

	tgt.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	/* Add one subsystem and verify that the discovery log contains it */
	subsystem = spdk_nvmf_subsystem_create(&tgt, "nqn.2016-06.io.spdk:subsystem1",
					       SPDK_NVMF_SUBTYPE_NVME, 0);
	subsystem->flags.allow_any_host = true;
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr, sizeof(trid.traddr), "1234");
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "5678");
	spdk_nvmf_subsystem_add_listener(subsystem, &trid, _subsystem_add_listen_done, NULL);
	subsystem->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	/* Get only genctr (first field in the header) */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, "nqn.2016-06.io.spdk:host1", &iov, 1, 0,
				    sizeof(disc_log->genctr));
	CU_ASSERT(disc_log->genctr == 2); /* one added subsystem and listener */

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, "nqn.2016-06.io.spdk:host1", &iov, 1, 0, sizeof(*disc_log));
	CU_ASSERT(disc_log->genctr == 2);
	CU_ASSERT(disc_log->numrec == 1);

	/* Offset 0, exact size match */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, "nqn.2016-06.io.spdk:host1", &iov, 1, 0,
				    sizeof(*disc_log) + sizeof(disc_log->entries[0]));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);

	/* Offset 0, oversize buffer */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, "nqn.2016-06.io.spdk:host1", &iov, 1, 0, sizeof(buffer));
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);
	CU_ASSERT(spdk_mem_all_zero(buffer + sizeof(*disc_log) + sizeof(disc_log->entries[0]),
				    sizeof(buffer) - (sizeof(*disc_log) + sizeof(disc_log->entries[0]))));

	/* Get just the first entry, no header */
	memset(buffer, 0xCC, sizeof(buffer));
	entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
	nvmf_get_discovery_log_page(&tgt, "nqn.2016-06.io.spdk:host1", &iov,
				    1,
				    offsetof(struct spdk_nvmf_discovery_log_page, entries[0]),
				    sizeof(*entry));
	CU_ASSERT(entry->trtype == 42);
	subsystem->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	spdk_nvmf_subsystem_destroy(subsystem);
	free(tgt.subsystems);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_discovery_log);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
