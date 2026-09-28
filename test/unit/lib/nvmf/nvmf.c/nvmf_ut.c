/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/ut_multithread.c"
#include "nvmf/nvmf.c"
#include "spdk/bdev_module.h"

DEFINE_STUB(spdk_bdev_get_md_size, uint32_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_desc_hide_metadata, bool, (struct spdk_bdev_desc *desc), false);

static bool g_add_transport_test;
static unsigned g_tgroup_live, g_tgroup_attempts, g_tgroup_fail_at;

void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	if (g_add_transport_test) {
		g_tgroup_live--;
		free(group);
	}
}
DEFINE_STUB_V(nvmf_ctrlr_destruct, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB_V(nvmf_transport_qpair_fini, (struct spdk_nvmf_qpair *qpair,
		spdk_nvmf_transport_qpair_fini_cb cb_fn,
		void *cb_arg));
DEFINE_STUB_V(nvmf_qpair_free_aer, (struct spdk_nvmf_qpair *qpair));
DEFINE_STUB_V(nvmf_qpair_abort_pending_zcopy_reqs, (struct spdk_nvmf_qpair *qpair));
DEFINE_RETURN_MOCK(nvmf_transport_poll_group_create, struct spdk_nvmf_transport_poll_group *);
struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	HANDLE_RETURN_MOCK(nvmf_transport_poll_group_create);
	if (!g_add_transport_test || ++g_tgroup_attempts == g_tgroup_fail_at) {
		return NULL;
	}
	tgroup = calloc(1, sizeof(*tgroup));
	SPDK_CU_ASSERT_FATAL(tgroup != NULL);
	tgroup->transport = transport;
	g_tgroup_live++;
	return tgroup;
}
DEFINE_STUB(nvmf_transport_poll_group_remove, int, (struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB_V(nvmf_transport_req_free, (struct spdk_nvmf_request *req));
DEFINE_STUB(nvmf_transport_poll_group_poll, int, (struct spdk_nvmf_transport_poll_group *group), 0);
DEFINE_STUB_V(nvmf_subsystem_remove_all_listeners, (struct spdk_nvmf_subsystem *subsystem,
		bool stop));
DEFINE_STUB_V(nvmf_subsystem_poll_group_update_ns_reservation, (const struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_subsystem_pg_ns_info *pg_ns));
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
DEFINE_STUB(spdk_nvmf_subsystem_get_min_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_host_get_nqn, const char *, (const struct spdk_nvmf_host *host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *prev_host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
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
DEFINE_STUB(spdk_nvmf_subsystem_get_first, struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB_V(nvmf_transport_dump_opts, (struct spdk_nvmf_transport *transport,
		struct spdk_json_write_ctx *w, bool named));
DEFINE_STUB_V(nvmf_transport_listen_dump_trid, (const struct spdk_nvme_transport_id *trid,
		struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(spdk_nvmf_send_discovery_log_notice, (struct spdk_nvmf_tgt *tgt,
		const char *hostnqn));
DEFINE_STUB(nvmf_nqn_is_valid, bool, (const char *nqn), true);
DEFINE_STUB(nvmf_nqn_is_discovery, bool, (const char *nqn), true);
DEFINE_STUB(spdk_key_get_name, const char *, (struct spdk_key *k), NULL);
DEFINE_STUB(nvmf_qpair_auth_init, int, (struct spdk_nvmf_qpair *q), 0);
DEFINE_STUB_V(nvmf_qpair_auth_destroy, (struct spdk_nvmf_qpair *q));
DEFINE_STUB_V(nvmf_tgt_stop_mdns_prr, (struct spdk_nvmf_tgt *tgt));

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

/* Namespace channels come from a real io_device so that the gets and
 * puts in the code under test are real and balance each other.
 */
static int g_ut_io_device;

static int
ut_io_device_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
ut_io_device_destroy_cb(void *io_device, void *ctx_buf)
{
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return spdk_get_io_channel(&g_ut_io_device);
}

static uint32_t g_ns_notice_count;
static struct spdk_nvmf_ctrlr *g_ns_notice_ctrlr;
static uint32_t g_ana_notice_count;

int
nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	g_ns_notice_count++;
	g_ns_notice_ctrlr = ctrlr;
	return 0;
}

int
nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	g_ana_notice_count++;
	return 0;
}

static void
test_nvmf_tgt_ns_metadata(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_bdev bdev = {};
	const struct spdk_nvmf_transport_ops ops = { .name = "TCP" };
	struct spdk_nvmf_transport transport = { .ops = &ops };
	uint32_t hidden, dif;

	RB_INIT(&tgt.subsystems);
	subsystem.max_nsid = 1;
	subsystem.ns = (struct spdk_nvmf_ns * []) { &ns };
	ns.bdev = &bdev;
	ns.desc = (void *)&bdev;
	MOCK_SET(spdk_nvmf_subsystem_get_first, &subsystem);
	for (hidden = 0; hidden < 2; hidden++) {
		MOCK_SET(spdk_bdev_desc_hide_metadata, hidden);
		for (dif = 0; dif < 2; dif++) {
			transport.opts.dif_insert_or_strip = dif;
			MOCK_SET(spdk_bdev_get_md_size, 8);
			CU_ASSERT(nvmf_tgt_check_ns_metadata(&tgt, &transport) ==
				  (hidden == dif ? 0 : -EINVAL));
			MOCK_SET(spdk_bdev_get_md_size, 0);
			CU_ASSERT(nvmf_tgt_check_ns_metadata(&tgt, &transport) == 0);
		}
	}
	MOCK_SET(spdk_bdev_get_md_size, 0);
	MOCK_SET(spdk_bdev_desc_hide_metadata, false);
	MOCK_SET(spdk_nvmf_subsystem_get_first, NULL);
}

static int
ut_add_transport_pg_create(void *io_device, void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;

	TAILQ_INIT(&group->tgroups);
	return 0;
}

static void
ut_add_transport_pg_destroy(void *io_device, void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;
	struct spdk_nvmf_transport_poll_group *tgroup;

	while ((tgroup = TAILQ_FIRST(&group->tgroups)) != NULL) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		nvmf_transport_poll_group_destroy(tgroup);
	}
}

struct ut_add_transport_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_poll_group *groups[2];
	struct spdk_thread *thread;
	unsigned calls;
	int status;
};

static void
ut_add_transport_done(void *arg, int status)
{
	struct ut_add_transport_ctx *ctx = arg;
	unsigned i;

	CU_ASSERT(spdk_get_thread() == ctx->thread);
	CU_ASSERT(++ctx->calls == 1);
	ctx->status = status;
	if (status != 0) {
		for (i = 0; i < SPDK_COUNTOF(ctx->groups); i++) {
			CU_ASSERT(nvmf_get_transport_poll_group(ctx->groups[i], ctx->transport) == NULL);
		}
		/* Error completion returns ownership only after every channel unwinds. */
		free(ctx->transport);
		ctx->transport = NULL;
	}
}

static void
test_nvmf_tgt_add_transport_overlap(void)
{
	const struct spdk_nvmf_transport_ops tcp = { .name = "TCP" };
	const struct spdk_nvmf_transport_ops vfio = { .name = "VFIOUSER" };
	struct spdk_io_channel *channels[2];
	struct spdk_nvmf_tgt tgt;
	struct spdk_nvmf_subsystem subsystem = { .max_nsid = 1 };
	struct spdk_bdev bdev = {};
	struct spdk_nvmf_ns ns = { .bdev = &bdev, .desc = (void *) &bdev };
	struct ut_add_transport_ctx first, second, retry;
	struct spdk_nvmf_ns *namespaces[] = { &ns };
	unsigned scenario, i;

	/* Conflicting policies, duplicate type, intervening namespace, partial setup. */
	for (scenario = 0; scenario < 4; scenario++) {
		allocate_threads(2);
		set_thread(0);
		memset(&tgt, 0, sizeof(tgt));
		memset(&first, 0, sizeof(first));
		memset(&second, 0, sizeof(second));
		memset(&retry, 0, sizeof(retry));
		TAILQ_INIT(&tgt.transports);
		RB_INIT(&tgt.subsystems);
		spdk_io_device_register(&tgt, ut_add_transport_pg_create, ut_add_transport_pg_destroy,
					sizeof(struct spdk_nvmf_poll_group), "add_transport");
		for (i = 0; i < SPDK_COUNTOF(channels); i++) {
			set_thread(i);
			channels[i] = spdk_get_io_channel(&tgt);
			SPDK_CU_ASSERT_FATAL(channels[i] != NULL);
			first.groups[i] = second.groups[i] = retry.groups[i] =
					spdk_io_channel_get_ctx(channels[i]);
		}
		set_thread(0);
		first.thread = second.thread = retry.thread = spdk_get_thread();
		MOCK_CLEAR(nvmf_transport_poll_group_create);
		MOCK_SET(spdk_nvmf_subsystem_get_first, NULL);
		g_add_transport_test = true;
		g_tgroup_attempts = 0;
		g_tgroup_fail_at = scenario == 3 ? 2 : 0;
		first.transport = calloc(1, sizeof(*first.transport));
		SPDK_CU_ASSERT_FATAL(first.transport != NULL);
		first.transport->ops = &tcp;
		spdk_nvmf_tgt_add_transport(&tgt, first.transport, ut_add_transport_done, &first);
		CU_ASSERT(first.calls == 0);
		if (scenario < 2) {
			second.transport = calloc(1, sizeof(*second.transport));
			SPDK_CU_ASSERT_FATAL(second.transport != NULL);
			second.transport->ops = scenario == 0 ? &vfio : &tcp;
			second.transport->opts.dif_insert_or_strip = scenario == 0;
			spdk_nvmf_tgt_add_transport(&tgt, second.transport, ut_add_transport_done, &second);
			CU_ASSERT(second.calls == 0);
		} else if (scenario == 2) {
			subsystem.ns = namespaces;
			MOCK_SET(spdk_nvmf_subsystem_get_first, &subsystem);
			MOCK_SET(spdk_bdev_get_md_size, 8);
			MOCK_SET(spdk_bdev_desc_hide_metadata, true);
		}
		poll_threads();
		CU_ASSERT(first.calls == 1);
		if (scenario < 2) {
			CU_ASSERT(first.status == 0);
			CU_ASSERT(second.calls == 1);
			CU_ASSERT(second.status == (scenario == 0 ? -EINVAL : -EEXIST));
			CU_ASSERT(TAILQ_FIRST(&tgt.transports) == first.transport);
			CU_ASSERT(TAILQ_NEXT(first.transport, link) == NULL);
			CU_ASSERT(g_tgroup_live == 2);
		} else {
			CU_ASSERT(first.status == (scenario == 2 ? -EINVAL : -1));
			CU_ASSERT(TAILQ_EMPTY(&tgt.transports));
			CU_ASSERT(g_tgroup_live == 0);
		}
		if (scenario == 2) {
			MOCK_SET(spdk_nvmf_subsystem_get_first, NULL);
			MOCK_SET(spdk_bdev_get_md_size, 0);
			MOCK_SET(spdk_bdev_desc_hide_metadata, false);
		}
		/* A compatible retry must succeed after the failed operation's callback. */
		set_thread(0);
		g_tgroup_fail_at = 0;
		retry.transport = calloc(1, sizeof(*retry.transport));
		SPDK_CU_ASSERT_FATAL(retry.transport != NULL);
		retry.transport->ops = &vfio;
		spdk_nvmf_tgt_add_transport(&tgt, retry.transport, ut_add_transport_done, &retry);
		poll_threads();
		CU_ASSERT(retry.calls == 1 && retry.status == 0);
		for (i = 0; i < SPDK_COUNTOF(channels); i++) {
			set_thread(i);
			spdk_put_io_channel(channels[i]);
		}
		poll_threads();
		CU_ASSERT(g_tgroup_live == 0);
		set_thread(0);
		spdk_io_device_unregister(&tgt, NULL);
		poll_threads();
		free(first.transport);
		free(retry.transport);
		g_add_transport_test = false;
		free_threads();
	}
}

static void
test_nvmf_tgt_options(void)
{
	struct spdk_nvmf_target_opts dflt_opts = {
		.size = SPDK_SIZEOF(&dflt_opts, dhchap_dhgroups),
		.name = "nvmf_test_tgt",
	};
	struct spdk_nvmf_target_opts dup_enable_opts = {
		.size = SPDK_SIZEOF(&dflt_opts, dup_host_policy),
		.name = "nvmf_test_tgt",
		.dup_host_policy = SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_RESTRICT_PER_LISTENER,
	};
	struct spdk_nvmf_tgt *tgt;

	allocate_threads(1);
	set_thread(0);

	/* Ensure dup_host_policy is set to ALLOW when not specified */
	tgt = spdk_nvmf_tgt_create(&dflt_opts);
	SPDK_CU_ASSERT_FATAL(tgt);
	SPDK_CU_ASSERT_FATAL(tgt->dup_host_policy == SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_ALLOW);
	spdk_nvmf_tgt_destroy(tgt, NULL, NULL);
	poll_threads();

	tgt = spdk_nvmf_tgt_create(&dup_enable_opts);
	SPDK_CU_ASSERT_FATAL(tgt);
	SPDK_CU_ASSERT_FATAL(
		tgt->dup_host_policy == SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_RESTRICT_PER_LISTENER);
	spdk_nvmf_tgt_destroy(tgt, NULL, NULL);
	poll_threads();
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
	struct spdk_io_channel		*ch;
	struct spdk_nvmf_transport_poll_group transport_pg = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	spdk_io_device_register(&g_ut_io_device, ut_io_device_create_cb,
				ut_io_device_destroy_cb, 0, NULL);
	ch = spdk_get_io_channel(&g_ut_io_device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	/* Create group with single subsystem */

	tgt.max_subsystems = 1;
	RB_INIT(&tgt.subsystems);

	/* Make sure subsystem has enough in subnqn so it can be
	 * inserted into RB-tree.
	 */
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "abc");
	RB_INSERT(subsystem_tree, &tgt.subsystems, &subsystem);
	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsystem.ns = calloc(1, sizeof(struct spdk_nvmf_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	MOCK_SET(spdk_nvmf_subsystem_get_first, &subsystem);

	subsystem.ns[0] = &ns;
	TAILQ_INIT(&ns.registrants);
	ns.bdev = &bdev;
	spdk_uuid_generate(&bdev.uuid);
	bdev.blockcnt = 512;

	TAILQ_INIT(&tgt.transports);
	TAILQ_INIT(&tgt.poll_groups);
	tgt.num_poll_groups = 0;
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
	CU_ASSERT(group.sgroups[0].ns_info[0].channel == ch);
	CU_ASSERT(!memcmp(&group.sgroups[0].ns_info[0].uuid, &bdev.uuid, 16));
	CU_ASSERT(group.sgroups[0].ns_info[0].num_blocks == 512);
	CU_ASSERT(TAILQ_FIRST(&tgt.poll_groups) == &group);
	CU_ASSERT(tgt.num_poll_groups == 1);
	CU_ASSERT(group.thread == thread);

	nvmf_tgt_destroy_poll_group((void *)&tgt, (void *)&group);
	CU_ASSERT(TAILQ_EMPTY(&tgt.poll_groups));
	CU_ASSERT(tgt.num_poll_groups == 0);
	free(subsystem.ns);
	MOCK_CLEAR(spdk_nvmf_subsystem_get_first);

	spdk_put_io_channel(ch);
	spdk_io_device_unregister(&g_ut_io_device, NULL);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
}

static void
test_nvmf_poll_group_update_subsystem_ns_aen(void)
{
	int rc;
	struct spdk_thread		*thread = NULL;
	struct spdk_thread		*other_thread = NULL;
	struct spdk_nvmf_poll_group	group = {};
	struct spdk_nvmf_subsystem_poll_group sgroup = {};
	struct spdk_nvmf_subsystem	subsystem = {};
	struct spdk_nvmf_ns		ns = {};
	struct spdk_nvmf_ns		ns2 = {};
	struct spdk_nvmf_ns		*ns_array[2] = { &ns, NULL };
	struct spdk_bdev		bdev = {};
	struct spdk_bdev		other_bdev = {};
	struct spdk_bdev		bdev2 = {};
	struct spdk_io_channel		*ch;
	struct spdk_nvmf_poll_group	other_group = {};
	struct spdk_nvmf_qpair		hidden_admin_qpair = {};
	struct spdk_nvmf_qpair		visible_admin_qpair = {};
	struct spdk_nvmf_qpair		other_admin_qpair = {};
	struct spdk_nvmf_qpair		other_thread_admin_qpair = {};
	struct spdk_nvmf_ctrlr		hidden_ctrlr = {};
	struct spdk_nvmf_ctrlr		visible_ctrlr = {};
	struct spdk_nvmf_ctrlr		other_group_ctrlr = {};
	struct spdk_nvmf_ctrlr		no_admin_qpair_ctrlr = {};
	struct spdk_nvmf_ctrlr		other_thread_ctrlr = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	other_thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(other_thread != NULL);
	spdk_set_thread(thread);

	/* The test holds one reference of its own to know the channel. */
	spdk_io_device_register(&g_ut_io_device, ut_io_device_create_cb,
				ut_io_device_destroy_cb, 0, NULL);
	ch = spdk_get_io_channel(&g_ut_io_device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_uuid_generate(&bdev.uuid);
	bdev.blockcnt = 512;
	ns.bdev = &bdev;
	ns.nsid = 1;
	ns.anagrpid = 1;
	TAILQ_INIT(&ns.registrants);

	subsystem.id = 0;
	subsystem.max_nsid = 2;
	subsystem.ns = ns_array;
	TAILQ_INIT(&subsystem.ctrlrs);

	group.num_sgroups = 1;
	group.sgroups = &sgroup;
	hidden_admin_qpair.group = &group;
	visible_admin_qpair.group = &group;

	/* Two controllers on this poll group. Namespace 1 is visible only to
	 * visible_ctrlr. In production nvmf_subsystem_ns_changed() records
	 * the nsid in the Changed Namespace List of every controller that
	 * can see it before the poll groups are updated; that producer lives
	 * in ctrlr.c/subsystem.c, outside this unit, so the test sets the
	 * list state directly: nsid 1 queued for visible_ctrlr, nothing for
	 * hidden_ctrlr.
	 */
	hidden_ctrlr.thread = thread;
	hidden_ctrlr.admin_qpair = &hidden_admin_qpair;
	hidden_ctrlr.changed_ns_list_count = 0;
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &hidden_ctrlr, link);

	visible_ctrlr.thread = thread;
	visible_ctrlr.admin_qpair = &visible_admin_qpair;
	visible_ctrlr.changed_ns_list_count = 1;
	visible_ctrlr.changed_ns_list.ns_list[0] = 1;
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &visible_ctrlr, link);

	/* Three more controllers that can see the namespace but must never
	 * be notified from this poll group's update: one whose admin queue
	 * pair belongs to another poll group (its own group's update notifies
	 * it), one whose admin queue pair is not assigned yet, and one that
	 * lives on another thread (its own thread's update notifies it).
	 */
	other_admin_qpair.group = &other_group;
	other_group_ctrlr.thread = thread;
	other_group_ctrlr.admin_qpair = &other_admin_qpair;
	other_group_ctrlr.changed_ns_list_count = 1;
	other_group_ctrlr.changed_ns_list.ns_list[0] = 1;
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &other_group_ctrlr, link);

	no_admin_qpair_ctrlr.thread = thread;
	no_admin_qpair_ctrlr.admin_qpair = NULL;
	no_admin_qpair_ctrlr.changed_ns_list_count = 1;
	no_admin_qpair_ctrlr.changed_ns_list.ns_list[0] = 1;
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &no_admin_qpair_ctrlr, link);

	other_thread_admin_qpair.group = &group;
	other_thread_ctrlr.thread = other_thread;
	other_thread_ctrlr.admin_qpair = &other_thread_admin_qpair;
	other_thread_ctrlr.changed_ns_list_count = 1;
	other_thread_ctrlr.changed_ns_list.ns_list[0] = 1;
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &other_thread_ctrlr, link);

	g_ns_notice_count = 0;
	g_ns_notice_ctrlr = NULL;
	g_ana_notice_count = 0;

	/* Namespace 1 appears: only the controller that can see it gets the
	 * Namespace Attribute Changed AEN.
	 */
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.num_ns == 2);
	SPDK_CU_ASSERT_FATAL(sgroup.ns_info != NULL);
	CU_ASSERT(sgroup.ns_info[1].channel == NULL);
	CU_ASSERT(sgroup.ns_info[0].channel == ch);
	CU_ASSERT(g_ns_notice_count == 1);
	CU_ASSERT(g_ns_notice_ctrlr == &visible_ctrlr);
	CU_ASSERT(g_ana_notice_count == 0);

	/* Nothing changed: no AEN, even though visible_ctrlr still has an
	 * unread Changed Namespace List.
	 */
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ns_notice_count == 1);
	CU_ASSERT(g_ana_notice_count == 0);

	/* Namespace 1 is resized: again only visible_ctrlr is notified. */
	bdev.blockcnt = 1024;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[0].num_blocks == 1024);
	CU_ASSERT(g_ns_notice_count == 2);
	CU_ASSERT(g_ns_notice_ctrlr == &visible_ctrlr);
	CU_ASSERT(g_ana_notice_count == 0);

	/* Namespace 1 is backed by a different bdev (new UUID): the channel
	 * is put and re-acquired, and only visible_ctrlr is notified.
	 */
	spdk_uuid_generate(&other_bdev.uuid);
	other_bdev.blockcnt = 1024;
	ns.bdev = &other_bdev;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[0].channel == ch);
	CU_ASSERT(spdk_uuid_compare(&sgroup.ns_info[0].uuid, &other_bdev.uuid) == 0);
	CU_ASSERT(g_ns_notice_count == 3);
	CU_ASSERT(g_ns_notice_ctrlr == &visible_ctrlr);
	CU_ASSERT(g_ana_notice_count == 0);

	/* Namespace 1 is removed: its channel is put and, as it was listed
	 * for visible_ctrlr, only visible_ctrlr is notified.
	 */
	ns_array[0] = NULL;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[0].channel == NULL);
	CU_ASSERT(g_ns_notice_count == 4);
	CU_ASSERT(g_ns_notice_ctrlr == &visible_ctrlr);
	CU_ASSERT(g_ana_notice_count == 0);

	/* A namespace appears that no controller can see (e.g. added with
	 * no_auto_visible): every Changed Namespace List is empty, so no
	 * controller is notified.
	 */
	visible_ctrlr.changed_ns_list_count = 0;
	ns_array[0] = &ns;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[0].channel == ch);
	CU_ASSERT(g_ns_notice_count == 4);
	CU_ASSERT(g_ana_notice_count == 0);

	/* An ANA group change is not a namespace change: both controllers on
	 * this poll group get the ANA change AEN and neither gets a namespace
	 * AEN, even though visible_ctrlr has nsid 1 queued in its Changed
	 * Namespace List.
	 */
	visible_ctrlr.changed_ns_list_count = 1;
	ns.anagrpid = 2;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[0].anagrpid == 2);
	CU_ASSERT(g_ns_notice_count == 4);
	CU_ASSERT(g_ana_notice_count == 2);

	/* A namespace change and an ANA group change in the same update:
	 * namespace 2 appears while namespace 1 moves to another ANA group.
	 * The namespace AEN goes to visible_ctrlr only, the ANA change AEN
	 * to both controllers on this poll group. (The two changes must be
	 * on different namespaces: per namespace the checks are an if/else
	 * chain in which any namespace change takes precedence over an ANA
	 * group change.)
	 */
	spdk_uuid_generate(&bdev2.uuid);
	bdev2.blockcnt = 512;
	ns2.bdev = &bdev2;
	ns2.nsid = 2;
	ns2.anagrpid = 1;
	TAILQ_INIT(&ns2.registrants);
	ns_array[1] = &ns2;
	ns.anagrpid = 3;
	rc = poll_group_update_subsystem(&group, &subsystem);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sgroup.ns_info[1].channel == ch);
	CU_ASSERT(sgroup.ns_info[0].anagrpid == 3);
	CU_ASSERT(g_ns_notice_count == 5);
	CU_ASSERT(g_ns_notice_ctrlr == &visible_ctrlr);
	CU_ASSERT(g_ana_notice_count == 4);

	/* Release the references held by ns_info, then the test's own. */
	spdk_put_io_channel(sgroup.ns_info[0].channel);
	spdk_put_io_channel(sgroup.ns_info[1].channel);
	free(sgroup.ns_info);
	spdk_put_io_channel(ch);
	spdk_io_device_unregister(&g_ut_io_device, NULL);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	spdk_set_thread(other_thread);
	spdk_thread_exit(other_thread);
	while (!spdk_thread_is_exited(other_thread)) {
		spdk_thread_poll(other_thread, 0, 0);
	}
	spdk_thread_destroy(other_thread);
}

static void
test_nvmf_target_opts_copy_bounds_size(void)
{
	struct {
		struct spdk_nvmf_target_opts opts;
		uint32_t canary;
	} dst = {};
	struct {
		struct spdk_nvmf_target_opts opts;
		uint8_t extra[32];
	} src = {};

	dst.canary = 0xdeadbeef;
	src.opts.size = sizeof(src);
	snprintf(src.opts.name, sizeof(src.opts.name), "%s", "nvmf_target");
	src.opts.max_subsystems = 17;
	src.opts.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_FILTER_ANY;
	memset(src.extra, 0x5a, sizeof(src.extra));

	nvmf_target_opts_copy(&dst.opts, &src.opts, src.opts.size);

	CU_ASSERT(dst.canary == 0xdeadbeef);
	CU_ASSERT(dst.opts.size == sizeof(src));
	CU_ASSERT_STRING_EQUAL(dst.opts.name, "nvmf_target");
	CU_ASSERT(dst.opts.max_subsystems == 17);
	CU_ASSERT(dst.opts.discovery_filter == SPDK_NVMF_TGT_DISCOVERY_FILTER_ANY);
}

static void
ut_pause_done(void *cb_arg, int status)
{
	bool *done = cb_arg;

	*done = true;
}

static void
test_nvmf_pause_drains_targeted_ns(void)
{
	/* NVMe namespace IDs are 1-based, but the poll group's ns_info[] array
	 * is 0-based: namespace N is tracked at ns_info[N - 1].
	 */
	const uint32_t ns1 = 1;
	const uint32_t ns2 = 2;
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroup = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info[2] = {};
	bool done;

	subsystem.id = 0;
	group.num_sgroups = 1;
	group.sgroups = &sgroup;
	sgroup.num_ns = 2;
	sgroup.ns_info = ns_info;

	/* Two active namespaces: namespace 1 still has I/O in flight, namespace
	 * 2 is idle.
	 */
	sgroup.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info[ns1 - 1].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info[ns1 - 1].io_outstanding = 1;
	ns_info[ns2 - 1].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info[ns2 - 1].io_outstanding = 0;

	/*
	 * Pause namespace 2. It has no outstanding I/O, so the pause completes
	 * immediately and leaves the poll group PAUSED -- but namespace 1 was
	 * not its target, so namespace 1 stays ACTIVE and keeps its outstanding
	 * I/O. This is the precondition behind the io_outstanding underflow
	 * assert in _nvmf_request_complete() (issue #3738): a poll group that is
	 * already PAUSED while a namespace is still ACTIVE and accumulating I/O.
	 */
	done = false;
	nvmf_poll_group_pause_subsystem(&group, &subsystem, ns2, ut_pause_done, &done);
	CU_ASSERT(done == true);
	CU_ASSERT(sgroup.state == SPDK_NVMF_SUBSYSTEM_PAUSED);
	CU_ASSERT(ns_info[ns2 - 1].state == SPDK_NVMF_SUBSYSTEM_PAUSING);
	CU_ASSERT(ns_info[ns1 - 1].state == SPDK_NVMF_SUBSYSTEM_ACTIVE);

	/*
	 * Now pause namespace 1 while the poll group is already PAUSED. I/O
	 * admission is gated per-namespace, so namespace 1 can still be ACTIVE
	 * with I/O in flight even though the poll group is paused. Pausing it
	 * must quiesce namespace 1 (mark it PAUSING so it stops admitting new
	 * I/O) and, because the namespace still has I/O outstanding, defer
	 * completion until the namespace drains -- the callback must not fire
	 * yet. If pausing an already-PAUSED poll group instead short-circuited
	 * and reported completion immediately, namespace 1 would stay ACTIVE and
	 * its io_outstanding could later be cleared while requests were still in
	 * flight, underflowing the counter.
	 */
	done = false;
	nvmf_poll_group_pause_subsystem(&group, &subsystem, ns1, ut_pause_done, &done);
	CU_ASSERT(ns_info[ns1 - 1].state == SPDK_NVMF_SUBSYSTEM_PAUSING);
	CU_ASSERT(done == false);
	CU_ASSERT(sgroup.state == SPDK_NVMF_SUBSYSTEM_PAUSING);
	CU_ASSERT(sgroup.cb_arg == &done);
	SPDK_CU_ASSERT_FATAL(sgroup.cb_fn == ut_pause_done);

	/* Quiescing namespace 1 must not disturb the other namespace. */
	CU_ASSERT(ns_info[ns2 - 1].state == SPDK_NVMF_SUBSYSTEM_PAUSING);

	/*
	 * Drain namespace 1. In the running target the namespace's outstanding
	 * I/O completes in _nvmf_request_complete() (lib/nvmf/ctrlr.c), which
	 * decrements io_outstanding and, once no PAUSING namespace has I/O left,
	 * flips the poll group to PAUSED and invokes the stored callback. That
	 * file is not built into this unit test, so reproduce its effect here to
	 * show that the deferred pause finally completes once the namespace has
	 * drained.
	 */
	ns_info[ns1 - 1].io_outstanding--;
	CU_ASSERT(ns_info[ns1 - 1].io_outstanding == 0);
	sgroup.state = SPDK_NVMF_SUBSYSTEM_PAUSED;
	sgroup.cb_fn(sgroup.cb_arg, 0);
	sgroup.cb_fn = NULL;
	sgroup.cb_arg = NULL;
	CU_ASSERT(done == true);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_tgt_create_poll_group);
	CU_ADD_TEST(suite, test_nvmf_target_opts_copy_bounds_size);
	CU_ADD_TEST(suite, test_nvmf_tgt_add_transport_overlap);
	CU_ADD_TEST(suite, test_nvmf_tgt_options);
	CU_ADD_TEST(suite, test_nvmf_tgt_ns_metadata);
	CU_ADD_TEST(suite, test_nvmf_pause_drains_targeted_ns);
	CU_ADD_TEST(suite, test_nvmf_poll_group_update_subsystem_ns_aen);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
