/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include "spdk/bdev_module.h"
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

DEFINE_STUB(spdk_nvmf_transport_get_first,
	    struct spdk_nvmf_transport *,
	    (struct spdk_nvmf_tgt *tgt), NULL);

DEFINE_STUB(spdk_nvmf_transport_get_next,
	    struct spdk_nvmf_transport *,
	    (struct spdk_nvmf_transport *transport), NULL);

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

DEFINE_STUB(spdk_bdev_io_type_supported, bool,
	    (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), false);

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
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return !(trid1->trtype == trid2->trtype && strcasecmp(trid1->traddr, trid2->traddr) == 0 &&
		 strcasecmp(trid1->trsvcid, trid2->trsvcid) == 0);
}

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts)
{
	return 0;
}

static struct spdk_nvmf_listener g_listener = {};

struct spdk_nvmf_listener *
nvmf_transport_find_listener(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	if (TAILQ_EMPTY(&transport->listeners)) {
		return &g_listener;
	}

	TAILQ_FOREACH(listener, &transport->listeners, link) {
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
}

static void
test_dummy_listener_discover(struct spdk_nvmf_transport *transport,
			     struct spdk_nvme_transport_id *trid, struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

struct spdk_nvmf_transport_ops g_transport_ops = { .listener_discover = test_dummy_listener_discover };

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

DEFINE_RETURN_MOCK(spdk_nvmf_tgt_get_transport, struct spdk_nvmf_transport *);
struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	HANDLE_RETURN_MOCK(spdk_nvmf_tgt_get_transport);
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
				uint32_t nsid,
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
test_gen_trid(struct spdk_nvme_transport_id *trid, enum spdk_nvme_transport_type trtype,
	      enum spdk_nvmf_adrfam adrfam, const char *tradd, const char *trsvcid)
{
	snprintf(trid->traddr, sizeof(trid->traddr), "%s", tradd);
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", trsvcid);
	trid->adrfam = adrfam;
	trid->trtype = trtype;
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_RDMA:
		snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", SPDK_NVME_TRANSPORT_NAME_RDMA);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		snprintf(trid->trstring, SPDK_NVMF_TRSTRING_MAX_LEN, "%s", SPDK_NVME_TRANSPORT_NAME_TCP);
		break;
	default:
		SPDK_CU_ASSERT_FATAL(0 && "not supported by test");
	}
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
	const char *hostnqn = "nqn.2016-06.io.spdk:host1";
	int rc;

	iov.iov_base = buffer;
	iov.iov_len = 8192;

	tgt.max_subsystems = 1024;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	/* Add one subsystem and verify that the discovery log contains it */
	subsystem = spdk_nvmf_subsystem_create(&tgt, "nqn.2016-06.io.spdk:subsystem1",
					       SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	rc = spdk_nvmf_subsystem_add_host(subsystem, hostnqn);
	CU_ASSERT(rc == 0);

	/* Get only genctr (first field in the header) */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(disc_log->genctr),
				    &trid);
	/* No listeners yet on new subsystem, so genctr should still be 0. */
	CU_ASSERT(disc_log->genctr == 0);

	test_gen_trid(&trid, SPDK_NVME_TRANSPORT_RDMA, SPDK_NVMF_ADRFAM_IPV4, "1234", "5678");
	spdk_nvmf_subsystem_add_listener(subsystem, &trid, _subsystem_add_listen_done, NULL);
	subsystem->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	/* Get only genctr (first field in the header) */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(disc_log->genctr),
				    &trid);
	CU_ASSERT(disc_log->genctr == 1); /* one added subsystem and listener */

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(*disc_log),
				    &trid);
	CU_ASSERT(disc_log->genctr == 1);
	CU_ASSERT(disc_log->numrec == 1);

	/* Offset 0, exact size match */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0,
				    sizeof(*disc_log) + sizeof(disc_log->entries[0]), &trid);
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);

	/* Offset 0, oversize buffer */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(buffer), &trid);
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(disc_log->entries[0].trtype == 42);
	CU_ASSERT(spdk_mem_all_zero(buffer + sizeof(*disc_log) + sizeof(disc_log->entries[0]),
				    sizeof(buffer) - (sizeof(*disc_log) + sizeof(disc_log->entries[0]))));

	/* Get just the first entry, no header */
	memset(buffer, 0xCC, sizeof(buffer));
	entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1,
				    offsetof(struct spdk_nvmf_discovery_log_page, entries[0]), sizeof(*entry), &trid);
	CU_ASSERT(entry->trtype == 42);

	/* remove the host and verify that the discovery log contains nothing */
	rc = spdk_nvmf_subsystem_remove_host(subsystem, hostnqn);
	CU_ASSERT(rc == 0);

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(*disc_log),
				    &trid);
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 0);

	/* destroy the subsystem and verify that the discovery log contains nothing */
	subsystem->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Get only the header, no entries */
	memset(buffer, 0xCC, sizeof(buffer));
	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, sizeof(*disc_log),
				    &trid);
	CU_ASSERT(disc_log->genctr != 0);
	CU_ASSERT(disc_log->numrec == 0);

	free(tgt.subsystems);
}

static void
test_rdma_discover(struct spdk_nvmf_transport *transport, struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	memcpy(entry->traddr, trid->traddr, sizeof(entry->traddr));
	memcpy(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid));
}

static void
test_tcp_discover(struct spdk_nvmf_transport *transport, struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_TCP;
	entry->adrfam = trid->adrfam;
	memcpy(entry->traddr, trid->traddr, sizeof(entry->traddr));
	memcpy(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid));
}

static void
test_discovery_log_with_filters(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_transport_ops rdma_tr_ops = { .listener_discover = test_rdma_discover }, tcp_tr_ops
		= { .listener_discover = test_tcp_discover };
	struct spdk_nvmf_transport rdma_tr = {.ops = &rdma_tr_ops }, tcp_tr = { .ops = &tcp_tr_ops };
	struct spdk_nvmf_subsystem *subsystem;
	const char *hostnqn = "nqn.2016-06.io.spdk:host1";
	uint8_t buffer[8192];
	struct iovec iov;
	struct spdk_nvmf_discovery_log_page *disc_log;
	struct spdk_nvmf_listener rdma_listener_1 = {}, rdma_listener_2 = {}, rdma_listener_3 = {},
	tcp_listener_1 = {}, tcp_listener_2 = {}, tcp_listener_3 = {};
	struct spdk_nvme_transport_id rdma_trid_1 = {}, rdma_trid_2 = {}, rdma_trid_3 = {}, tcp_trid_1 = {},
	tcp_trid_2 = {}, tcp_trid_3 = {};

	iov.iov_base = buffer;
	iov.iov_len = 8192;

	tgt.max_subsystems = 4;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	subsystem = spdk_nvmf_subsystem_create(&tgt, "nqn.2016-06.io.spdk:subsystem1",
					       SPDK_NVMF_SUBTYPE_NVME, 0);
	subsystem->flags.allow_any_host = true;
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	test_gen_trid(&rdma_trid_1, SPDK_NVME_TRANSPORT_RDMA, SPDK_NVMF_ADRFAM_IPV4, "10.10.10.10", "4420");
	test_gen_trid(&rdma_trid_2, SPDK_NVME_TRANSPORT_RDMA, SPDK_NVMF_ADRFAM_IPV4, "11.11.11.11", "4420");
	test_gen_trid(&rdma_trid_3, SPDK_NVME_TRANSPORT_RDMA, SPDK_NVMF_ADRFAM_IPV4, "10.10.10.10", "4421");
	test_gen_trid(&tcp_trid_1, SPDK_NVME_TRANSPORT_TCP, SPDK_NVMF_ADRFAM_IPV4, "11.11.11.11", "4421");
	test_gen_trid(&tcp_trid_2, SPDK_NVME_TRANSPORT_TCP, SPDK_NVMF_ADRFAM_IPV4, "10.10.10.10", "4422");
	test_gen_trid(&tcp_trid_3, SPDK_NVME_TRANSPORT_TCP, SPDK_NVMF_ADRFAM_IPV4, "11.11.11.11", "4422");

	rdma_listener_1.trid = rdma_trid_1;
	rdma_listener_2.trid = rdma_trid_2;
	rdma_listener_3.trid = rdma_trid_3;
	TAILQ_INIT(&rdma_tr.listeners);
	TAILQ_INSERT_TAIL(&rdma_tr.listeners, &rdma_listener_1, link);
	TAILQ_INSERT_TAIL(&rdma_tr.listeners, &rdma_listener_2, link);
	TAILQ_INSERT_TAIL(&rdma_tr.listeners, &rdma_listener_3, link);

	tcp_listener_1.trid = tcp_trid_1;
	tcp_listener_2.trid = tcp_trid_2;
	tcp_listener_3.trid = tcp_trid_3;
	TAILQ_INIT(&tcp_tr.listeners);
	TAILQ_INSERT_TAIL(&tcp_tr.listeners, &tcp_listener_1, link);
	TAILQ_INSERT_TAIL(&tcp_tr.listeners, &tcp_listener_2, link);
	TAILQ_INSERT_TAIL(&tcp_tr.listeners, &tcp_listener_3, link);

	MOCK_SET(spdk_nvmf_tgt_get_transport, &rdma_tr);
	spdk_nvmf_subsystem_add_listener(subsystem, &rdma_trid_1, _subsystem_add_listen_done, NULL);
	spdk_nvmf_subsystem_add_listener(subsystem, &rdma_trid_2, _subsystem_add_listen_done, NULL);
	spdk_nvmf_subsystem_add_listener(subsystem, &rdma_trid_3, _subsystem_add_listen_done, NULL);
	MOCK_SET(spdk_nvmf_tgt_get_transport, &tcp_tr);
	spdk_nvmf_subsystem_add_listener(subsystem, &tcp_trid_1, _subsystem_add_listen_done, NULL);
	spdk_nvmf_subsystem_add_listener(subsystem, &tcp_trid_2, _subsystem_add_listen_done, NULL);
	spdk_nvmf_subsystem_add_listener(subsystem, &tcp_trid_3, _subsystem_add_listen_done, NULL);
	MOCK_CLEAR(spdk_nvmf_tgt_get_transport);

	subsystem->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	disc_log = (struct spdk_nvmf_discovery_log_page *)buffer;
	memset(buffer, 0, sizeof(buffer));

	/* Test case 1 - check that all trids are reported */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 6);

	/* Test case 2 - check that only entries of the same transport type are returned */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 3);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_1.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == rdma_trid_1.trtype);
	CU_ASSERT(disc_log->entries[2].trtype == rdma_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 3);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_1.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == tcp_trid_1.trtype);
	CU_ASSERT(disc_log->entries[2].trtype == tcp_trid_1.trtype);

	/* Test case 3 - check that only entries of the same transport address are returned */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 3);
	/* one tcp and 2 rdma  */
	CU_ASSERT((disc_log->entries[0].trtype ^ disc_log->entries[1].trtype ^ disc_log->entries[2].trtype)
		  != 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[2].traddr, rdma_trid_1.traddr) == 0);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 3);
	/* one rdma and two tcp */
	CU_ASSERT((disc_log->entries[0].trtype ^ disc_log->entries[1].trtype ^ disc_log->entries[2].trtype)
		  != 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[2].traddr, tcp_trid_1.traddr) == 0);

	/* Test case 4 - check that only entries of the same transport address and type returned */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE |
			       SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 2);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_1.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == rdma_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_2.traddr) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 2);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_1.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == tcp_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_2.traddr) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_2.trtype);

	/* Test case 5 - check that only entries of the same transport address and type returned */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE |
			       SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 2);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_1.trsvcid) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].trsvcid, rdma_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_1.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == rdma_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_3);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_3.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_3.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_1.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_2);
	CU_ASSERT(disc_log->numrec == 2);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_2.trsvcid) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[1].trsvcid, tcp_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_2.trtype);
	CU_ASSERT(disc_log->entries[1].trtype == tcp_trid_2.trtype);

	/* Test case 6 - check that only entries of the same transport address and type returned.
	 * That also implies trtype since RDMA and TCP listeners can't occupy the same socket */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS |
			       SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_1.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_2.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_3);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_3.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_3.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_3.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_1.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_2.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_3);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_3.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_3.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_3.trtype);

	/* Test case 7 - check that only entries of the same transport address, svcid and type returned */
	tgt.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE |
			       SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS |
			       SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID;
	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_1);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_1.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_2.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &rdma_trid_3);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, rdma_trid_3.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, rdma_trid_3.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == rdma_trid_3.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_1);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_1.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_1.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_1.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_2);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_2.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_2.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_2.trtype);

	nvmf_get_discovery_log_page(&tgt, hostnqn, &iov, 1, 0, 8192, &tcp_trid_3);
	CU_ASSERT(disc_log->numrec == 1);
	CU_ASSERT(strcasecmp(disc_log->entries[0].traddr, tcp_trid_3.traddr) == 0);
	CU_ASSERT(strcasecmp(disc_log->entries[0].trsvcid, tcp_trid_3.trsvcid) == 0);
	CU_ASSERT(disc_log->entries[0].trtype == tcp_trid_3.trtype);

	subsystem->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
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
	CU_ADD_TEST(suite, test_discovery_log_with_filters);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
