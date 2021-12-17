/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
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
#include "common/lib/test_env.c"
#include "nvmf/nvmf.c"
#include "spdk/bdev_module.h"

DEFINE_STUB_V(nvmf_transport_poll_group_destroy, (struct spdk_nvmf_transport_poll_group *group));
DEFINE_STUB_V(nvmf_ctrlr_destruct, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB_V(nvmf_transport_qpair_fini, (struct spdk_nvmf_qpair *qpair,
		spdk_nvmf_transport_qpair_fini_cb cb_fn,
		void *cb_arg));
DEFINE_STUB_V(nvmf_qpair_free_aer, (struct spdk_nvmf_qpair *qpair));
DEFINE_STUB_V(nvmf_qpair_abort_pending_zcopy_reqs, (struct spdk_nvmf_qpair *qpair));
DEFINE_STUB(nvmf_transport_poll_group_create, struct spdk_nvmf_transport_poll_group *,
	    (struct spdk_nvmf_transport *transport), NULL);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc),
	    NULL);
DEFINE_STUB(nvmf_ctrlr_async_event_ns_notice, int, (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice, int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB(nvmf_transport_poll_group_remove, int, (struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB(nvmf_transport_req_free, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(nvmf_transport_poll_group_poll, int, (struct spdk_nvmf_transport_poll_group *group), 0);
DEFINE_STUB(nvmf_transport_accept, uint32_t, (struct spdk_nvmf_transport *transport), 0);
DEFINE_STUB_V(nvmf_subsystem_remove_all_listeners, (struct spdk_nvmf_subsystem *subsystem,
		bool stop));
DEFINE_STUB(spdk_nvmf_subsystem_destroy, int, (struct spdk_nvmf_subsystem *subsystem,
		nvmf_subsystem_destroy_cb cpl_cb, void *cpl_cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_listener, struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_listener, struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem,
	     struct spdk_nvmf_subsystem_listener *prev_listener), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next, struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_allow_any_host, bool,
	    (const struct spdk_nvmf_subsystem *subsystem), true);
DEFINE_STUB(spdk_nvmf_subsystem_get_sn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_mn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_namespaces, uint32_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_min_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_listener_get_trid, const struct spdk_nvme_transport_id *,
	    (struct spdk_nvmf_subsystem_listener *listener), NULL);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_host_get_nqn, const char *, (const struct spdk_nvmf_host *host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *prev_host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(nvmf_subsystem_get_ana_reporting, bool, (struct spdk_nvmf_subsystem *subsystem), false);
DEFINE_STUB_V(spdk_nvmf_ns_get_opts, (const struct spdk_nvmf_ns *ns,
				      struct spdk_nvmf_ns_opts *opts, size_t opts_size));
DEFINE_STUB(spdk_nvmf_ns_get_id, uint32_t, (const struct spdk_nvmf_ns *ns), 0);
DEFINE_STUB(spdk_nvmf_ns_get_bdev, struct spdk_bdev *, (struct spdk_nvmf_ns *ns), NULL);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns), NULL);
DEFINE_STUB(spdk_nvmf_transport_listen, int, (struct spdk_nvmf_transport *transport,
		const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_listen_opts *opts), 0);
DEFINE_STUB(spdk_nvmf_transport_stop_listen, int,
	    (struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid), 0)
DEFINE_STUB(nvmf_transport_get_optimal_poll_group, struct spdk_nvmf_transport_poll_group *,
	    (struct spdk_nvmf_transport *transport, struct spdk_nvmf_qpair *qpair), NULL);
DEFINE_STUB(nvmf_transport_poll_group_add, int,
	    (struct spdk_nvmf_transport_poll_group *group,
	     struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB(nvmf_transport_qpair_get_peer_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(nvmf_transport_qpair_get_local_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(nvmf_transport_qpair_get_listen_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB_V(spdk_nvmf_request_zcopy_start, (struct spdk_nvmf_request *req));
DEFINE_STUB(spdk_nvmf_get_transport_name, const char *,
	    (struct spdk_nvmf_transport *transport), NULL);
DEFINE_STUB(spdk_nvmf_transport_destroy, int, (struct spdk_nvmf_transport *transport,
		spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_type, enum spdk_nvmf_subtype,
	    (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_first, struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB_V(nvmf_transport_dump_opts, (struct spdk_nvmf_transport *transport,
		struct spdk_json_write_ctx *w, bool named));
DEFINE_STUB_V(nvmf_transport_listen_dump_opts, (struct spdk_nvmf_transport *transport,
		const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w));

struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct io_device		*dev;
	uint32_t			ref;
	uint32_t			destroy_ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;
	spdk_io_channel_destroy_cb	destroy_cb;

	uint8_t				_padding[48];
};

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

static void
test_nvmf_tgt_create_poll_group(void)
{
	int rc;
	struct spdk_thread		*thread = NULL;
	struct spdk_nvmf_tgt		tgt = {};
	struct spdk_nvmf_poll_group	group = {};
	struct spdk_nvmf_transport	transport = {};
	struct spdk_nvmf_subsystem	subsystem = {};
	struct spdk_nvmf_ns		ns = {};
	struct spdk_bdev		bdev = {};
	struct spdk_io_channel		ch = {};
	struct spdk_nvmf_transport_poll_group transport_pg = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	/* Create group with single subsystem */
	ch.thread = thread;
	MOCK_SET(spdk_bdev_get_io_channel, &ch);

	tgt.max_subsystems = 1;
	tgt.subsystems = calloc(tgt.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems != NULL);

	tgt.subsystems[0] = &subsystem;
	tgt.subsystems[0]->id = 0;
	tgt.subsystems[0]->max_nsid = 1;
	tgt.subsystems[0]->ns = calloc(1, sizeof(struct spdk_nvmf_ns *));
	SPDK_CU_ASSERT_FATAL(tgt.subsystems[0]->ns != NULL);

	tgt.subsystems[0]->ns[0] = &ns;
	ns.crkey = 0xaa;
	ns.rtype = 0xbb;
	TAILQ_INIT(&ns.registrants);
	ns.bdev = &bdev;
	spdk_uuid_generate(&bdev.uuid);
	bdev.blockcnt = 512;

	TAILQ_INIT(&tgt.transports);
	TAILQ_INIT(&tgt.poll_groups);
	pthread_mutex_init(&tgt.mutex, NULL);
	transport.tgt = &tgt;
	TAILQ_INSERT_TAIL(&tgt.transports, &transport, link);

	MOCK_SET(nvmf_transport_poll_group_create, &transport_pg);
	rc = nvmf_tgt_create_poll_group((void *)&tgt, (void *)&group);
	MOCK_SET(nvmf_transport_poll_group_create, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(group.num_sgroups == 1);
	CU_ASSERT(group.sgroups != NULL);
	CU_ASSERT(group.sgroups[0].state == SPDK_NVMF_SUBSYSTEM_ACTIVE);
	CU_ASSERT(group.sgroups[0].ns_info[0].channel == &ch);
	CU_ASSERT(!memcmp(&group.sgroups[0].ns_info[0].uuid, &bdev.uuid, 16));
	CU_ASSERT(group.sgroups[0].ns_info[0].num_blocks == 512);
	CU_ASSERT(group.sgroups[0].ns_info[0].crkey == 0xaa);
	CU_ASSERT(group.sgroups[0].ns_info[0].rtype == 0xbb);
	CU_ASSERT(TAILQ_FIRST(&tgt.poll_groups) == &group);
	CU_ASSERT(group.thread == thread);
	CU_ASSERT(group.poller != NULL);

	nvmf_tgt_destroy_poll_group((void *)&tgt, (void *)&group);
	CU_ASSERT(TAILQ_EMPTY(&tgt.poll_groups));
	free(tgt.subsystems[0]->ns);
	free(tgt.subsystems);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
	MOCK_CLEAR(spdk_bdev_get_io_channel);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_tgt_create_poll_group);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
