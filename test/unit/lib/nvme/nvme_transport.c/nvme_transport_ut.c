/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "nvme/nvme_transport.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(nvme_qpair_state_string, const char *, (enum nvme_qpair_state state), "test");
DEFINE_STUB(nvme_poll_group_connect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB_V(nvme_qpair_abort_all_queued_reqs, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(nvme_poll_group_disconnect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(spdk_nvme_ctrlr_free_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(spdk_nvme_transport_id_trtype_str, const char *,
	    (enum spdk_nvme_transport_type trtype), NULL);
DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t, (struct spdk_nvme_qpair *qpair,
		uint32_t max_completions), 0);
DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t, (struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0);
DEFINE_STUB(nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);
DEFINE_STUB(spdk_nvme_ctrlr_is_fabrics, bool, (struct spdk_nvme_ctrlr *ctrlr), false);
DEFINE_STUB(ut_poll_group_add, int, (struct spdk_nvme_transport_poll_group *group,
				     struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(ut_poll_group_remove, int, (struct spdk_nvme_transport_poll_group *group,
					struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(ut_poll_group_connect, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(ut_poll_group_disconnect, int, (struct spdk_nvme_qpair *qpair), 0);

static void
test_nvme_transport_poll_group_hooks(void)
{
	struct spdk_nvme_transport transport = {
		.ops = {
			.poll_group_add = ut_poll_group_add,
			.poll_group_remove = ut_poll_group_remove,
			.poll_group_connect_qpair = ut_poll_group_connect,
			.poll_group_disconnect_qpair = ut_poll_group_disconnect,
		},
	};
	struct spdk_nvme_transport_poll_group group = {.transport = &transport};
	struct spdk_nvme_qpair qpair = {};

	STAILQ_INIT(&group.connected_qpairs);
	STAILQ_INIT(&group.disconnected_qpairs);
	nvme_qpair_set_state(&qpair, NVME_QPAIR_DISCONNECTED);
	MOCK_SET(ut_poll_group_add, -EINVAL);
	CU_ASSERT(nvme_transport_poll_group_add(&group, &qpair) == -EINVAL);
	CU_ASSERT(qpair.poll_group == NULL && qpair.poll_group_tailq_head == NULL);
	CU_ASSERT(STAILQ_EMPTY(&group.disconnected_qpairs));
	MOCK_SET(ut_poll_group_add, 0);
	CU_ASSERT(nvme_transport_poll_group_add(&group, &qpair) == 0);
	MOCK_SET(ut_poll_group_connect, -EINVAL);
	CU_ASSERT(nvme_transport_poll_group_connect_qpair(&qpair) == -EINVAL);
	CU_ASSERT(qpair.poll_group_tailq_head == &group.disconnected_qpairs);
	CU_ASSERT(group.num_connected_qpairs == 0);
	MOCK_SET(ut_poll_group_connect, -EINPROGRESS);
	CU_ASSERT(nvme_transport_poll_group_connect_qpair(&qpair) == 0);
	CU_ASSERT(qpair.poll_group_tailq_head == &group.disconnected_qpairs);
	MOCK_SET(ut_poll_group_connect, 0);
	CU_ASSERT(nvme_transport_poll_group_connect_qpair(&qpair) == 0);
	CU_ASSERT(group.num_connected_qpairs == 1);
	MOCK_SET(ut_poll_group_disconnect, -EIO);
	CU_ASSERT(nvme_transport_poll_group_disconnect_qpair(&qpair) == -EIO);
	CU_ASSERT(qpair.poll_group_tailq_head == &group.connected_qpairs);
	CU_ASSERT(group.num_connected_qpairs == 1);
	MOCK_SET(ut_poll_group_disconnect, 0);
	CU_ASSERT(nvme_transport_poll_group_disconnect_qpair(&qpair) == 0);
	MOCK_SET(ut_poll_group_remove, -EBUSY);
	CU_ASSERT(nvme_transport_poll_group_remove(&group, &qpair) == -EBUSY);
	CU_ASSERT(qpair.poll_group == &group);
	CU_ASSERT(qpair.poll_group_tailq_head == &group.disconnected_qpairs);
	MOCK_SET(ut_poll_group_remove, 0);
	CU_ASSERT(nvme_transport_poll_group_remove(&group, &qpair) == 0);
	CU_ASSERT(qpair.poll_group == NULL && qpair.poll_group_tailq_head == NULL);
	CU_ASSERT(STAILQ_EMPTY(&group.connected_qpairs));
	CU_ASSERT(STAILQ_EMPTY(&group.disconnected_qpairs));
}

static void
test_nvme_managed_queue_unsupported(void)
{
	struct spdk_nvme_transport transport = {};
	struct spdk_nvme_ctrlr ctrlr = {};

	memcpy(ctrlr.trid.trstring, "ut_unsupported", sizeof("ut_unsupported"));
	memcpy(transport.ops.name, ctrlr.trid.trstring, sizeof(transport.ops.name));
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &transport, link);
	CU_ASSERT(nvme_transport_ctrlr_create_io_cq(&ctrlr, 1, NULL) == NULL);
	CU_ASSERT(nvme_transport_ctrlr_delete_io_cq(&ctrlr, NULL) == -ENOTSUP);
	CU_ASSERT(nvme_transport_ctrlr_delete_io_cq_force(&ctrlr, NULL) == -ENOTSUP);
	CU_ASSERT(nvme_transport_ctrlr_connect_io_cq(&ctrlr, NULL) == -ENOTSUP);
	CU_ASSERT(nvme_transport_ctrlr_create_io_sq(&ctrlr, NULL, 1, NULL) == NULL);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &transport, link);
}

static int
ut_connect_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_cq *cq)
{
	return -EAGAIN;
}

static int
ut_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_cq *cq)
{
	return -EIO;
}

static struct spdk_nvme_cq *
ut_create_io_cq(struct spdk_nvme_ctrlr *ctrlr, uint16_t cqid,
		const struct spdk_nvme_io_cq_opts *opts)
{
	CU_ASSERT(cqid == 7);
	return NULL;
}

static struct spdk_nvme_qpair g_managed_sq;

static struct spdk_nvme_qpair *
ut_create_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_cq *cq,
		uint16_t sqid, const struct spdk_nvme_io_qpair_opts *opts)
{
	CU_ASSERT(sqid == 9);
	g_managed_sq.id = sqid;
	return &g_managed_sq;
}

static void
test_nvme_managed_queue_dispatch(void)
{
	struct spdk_nvme_transport transport = {
		.ops = {
			.name = "ut_managed",
			.ctrlr_create_io_cq = ut_create_io_cq,
			.ctrlr_delete_io_cq = ut_delete_io_cq,
			.ctrlr_delete_io_cq_force = ut_connect_io_cq,
			.ctrlr_connect_io_cq = ut_connect_io_cq,
			.ctrlr_create_io_sq = ut_create_io_sq,
		},
	};
	struct spdk_nvme_ctrlr ctrlr = {};

	memcpy(ctrlr.trid.trstring, transport.ops.name, sizeof(transport.ops.name));
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &transport, link);
	CU_ASSERT(nvme_transport_ctrlr_create_io_cq(&ctrlr, 7, NULL) == NULL);
	CU_ASSERT(nvme_transport_ctrlr_delete_io_cq(&ctrlr, NULL) == -EIO);
	CU_ASSERT(nvme_transport_ctrlr_delete_io_cq_force(&ctrlr, NULL) == -EAGAIN);
	CU_ASSERT(nvme_transport_ctrlr_connect_io_cq(&ctrlr, NULL) == -EAGAIN);
	memset(&g_managed_sq, 0, sizeof(g_managed_sq));
	CU_ASSERT(nvme_transport_ctrlr_create_io_sq(&ctrlr, NULL, 9, NULL) == &g_managed_sq);
	CU_ASSERT(g_managed_sq.transport == &transport);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &transport, link);
}

static void
ut_reset_transport_registry(void)
{
	TAILQ_INIT(&g_spdk_nvme_transports);
	memset(g_transports, 0, sizeof(g_transports));
	g_current_transport_index = 0;
}

static int
ut_registration_ready(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static int
ut_registration_events(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static void
test_nvme_transport_register_ext(void)
{
	struct spdk_nvme_transport_ops ops = {
		.ctrlr_ready = ut_registration_ready,
		.ctrlr_process_transport_events = ut_registration_events,
		.ctrlr_create_io_cq = ut_create_io_cq,
		.ctrlr_connect_io_cq = ut_connect_io_cq,
		.poll_group_add = ut_poll_group_add,
		.poll_group_remove = ut_poll_group_remove,
		.poll_group_connect_qpair = ut_poll_group_connect,
		.poll_group_disconnect_qpair = ut_poll_group_disconnect,
	};
	const struct spdk_nvme_transport *t;

	ut_reset_transport_registry();
	snprintf(ops.name, sizeof(ops.name), "ut_ext_exact");
	spdk_nvme_transport_register_ext(&ops, sizeof(ops));
	t = nvme_get_transport(ops.name);
	SPDK_CU_ASSERT_FATAL(t != NULL);
	CU_ASSERT(t->ops.ctrlr_ready == ut_registration_ready);
	CU_ASSERT(t->ops.ctrlr_process_transport_events == ut_registration_events);
	CU_ASSERT(t->ops.ctrlr_create_io_cq == ut_create_io_cq);
	CU_ASSERT(t->ops.ctrlr_connect_io_cq == ut_connect_io_cq);
	CU_ASSERT(t->ops.poll_group_add == ut_poll_group_add);
	CU_ASSERT(t->ops.poll_group_remove == ut_poll_group_remove);
	CU_ASSERT(t->ops.poll_group_connect_qpair == ut_poll_group_connect);
	CU_ASSERT(t->ops.poll_group_disconnect_qpair == ut_poll_group_disconnect);

	snprintf(ops.name, sizeof(ops.name), "ut_ext_small");
	spdk_nvme_transport_register_ext(&ops,
					 offsetof(struct spdk_nvme_transport_ops, ctrlr_get_registers));
	t = nvme_get_transport(ops.name);
	SPDK_CU_ASSERT_FATAL(t != NULL);
	CU_ASSERT(t->ops.ctrlr_ready == ut_registration_ready);
	CU_ASSERT(t->ops.ctrlr_process_transport_events == NULL);
	CU_ASSERT(t->ops.ctrlr_create_io_cq == NULL);
	CU_ASSERT(t->ops.ctrlr_connect_io_cq == NULL);

	snprintf(ops.name, sizeof(ops.name), "ut_ext_big");
	spdk_nvme_transport_register_ext(&ops, sizeof(ops) + 4096);
	t = nvme_get_transport(ops.name);
	SPDK_CU_ASSERT_FATAL(t != NULL);
	CU_ASSERT(t->ops.ctrlr_process_transport_events == ut_registration_events);
	ut_reset_transport_registry();
}

static void
test_nvme_transport_register_legacy(void)
{
	struct spdk_nvme_transport_ops ops = {
		.ctrlr_ready = ut_registration_ready,
		.ctrlr_process_transport_events = ut_registration_events,
		.ctrlr_create_io_cq = ut_create_io_cq,
		.ctrlr_connect_io_cq = ut_connect_io_cq,
		.poll_group_add = ut_poll_group_add,
		.poll_group_remove = ut_poll_group_remove,
		.poll_group_connect_qpair = ut_poll_group_connect,
		.poll_group_disconnect_qpair = ut_poll_group_disconnect,
	};
	const struct spdk_nvme_transport *t;

	ut_reset_transport_registry();
	snprintf(ops.name, sizeof(ops.name), "ut_legacy");
	spdk_nvme_transport_register(&ops);
	t = nvme_get_transport(ops.name);
	SPDK_CU_ASSERT_FATAL(t != NULL);
	CU_ASSERT(t->ops.ctrlr_ready == ut_registration_ready);
	CU_ASSERT(t->ops.ctrlr_process_transport_events == ut_registration_events);
	CU_ASSERT(t->ops.ctrlr_create_io_cq == NULL);
	CU_ASSERT(t->ops.ctrlr_connect_io_cq == NULL);
	CU_ASSERT(t->ops.poll_group_add == NULL);
	CU_ASSERT(t->ops.poll_group_remove == NULL);
	CU_ASSERT(t->ops.poll_group_connect_qpair == NULL);
	CU_ASSERT(t->ops.poll_group_disconnect_qpair == NULL);
	ut_reset_transport_registry();
}

static void
test_nvme_transport_register_invalid_size(void)
{
	struct spdk_nvme_transport_ops ops = {};

	ut_reset_transport_registry();
	snprintf(ops.name, sizeof(ops.name), "ut_invalid");
	spdk_nvme_transport_register_ext(NULL, sizeof(ops));
	CU_ASSERT(g_current_transport_index == 0);
	spdk_nvme_transport_register_ext(&ops, sizeof(ops.name) - 1);
	CU_ASSERT(g_current_transport_index == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_transports));
	ut_reset_transport_registry();
}

static void
ut_construct_transport(struct spdk_nvme_transport *transport, const char name[])
{
	memcpy(transport->ops.name, name, strlen(name));
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, transport, link);
}

static void
test_nvme_get_transport(void)
{
	const struct spdk_nvme_transport *nvme_transport = NULL;
	struct spdk_nvme_transport new_transport = {};

	ut_construct_transport(&new_transport, "new_transport");

	nvme_transport = nvme_get_transport("new_transport");
	CU_ASSERT(nvme_transport == &new_transport);
	TAILQ_REMOVE(&g_spdk_nvme_transports, nvme_transport, link);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_nvme_transports));

	/* Unavailable transport entry */
	nvme_transport = nvme_get_transport("new_transport");
	SPDK_CU_ASSERT_FATAL(nvme_transport == NULL);
}

static void
test_nvme_transport_poll_group_connect_qpair(void)
{
	int rc;
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_transport_poll_group	tgroup = {};
	struct spdk_nvme_transport transport = {};

	qpair.poll_group = &tgroup;
	tgroup.transport = &transport;
	STAILQ_INIT(&tgroup.connected_qpairs);
	STAILQ_INIT(&tgroup.disconnected_qpairs);

	/* Connected qpairs */
	qpair.poll_group_tailq_head = &tgroup.connected_qpairs;

	rc = nvme_transport_poll_group_connect_qpair(&qpair);
	CU_ASSERT(rc == 0);

	/* Disconnected qpairs */

	qpair.poll_group_tailq_head = &tgroup.disconnected_qpairs;
	STAILQ_INSERT_TAIL(&tgroup.disconnected_qpairs, &qpair, poll_group_stailq);

	rc = nvme_transport_poll_group_connect_qpair(&qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(STAILQ_EMPTY(&tgroup.disconnected_qpairs));
	CU_ASSERT(!STAILQ_EMPTY(&tgroup.connected_qpairs));
	STAILQ_REMOVE(&tgroup.connected_qpairs, &qpair, spdk_nvme_qpair, poll_group_stailq);
	CU_ASSERT(STAILQ_EMPTY(&tgroup.connected_qpairs));

	/* None qpairs */
	qpair.poll_group_tailq_head = NULL;

	rc = nvme_transport_poll_group_connect_qpair(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == -EINVAL);
}

static void
test_nvme_transport_poll_group_disconnect_qpair(void)
{
	int rc;
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_transport_poll_group	tgroup = {};
	struct spdk_nvme_transport transport = {};

	qpair.poll_group = &tgroup;
	tgroup.transport = &transport;
	STAILQ_INIT(&tgroup.connected_qpairs);
	STAILQ_INIT(&tgroup.disconnected_qpairs);

	/* Disconnected qpairs */
	qpair.poll_group_tailq_head = &tgroup.disconnected_qpairs;

	rc = nvme_transport_poll_group_disconnect_qpair(&qpair);
	CU_ASSERT(rc == 0);

	/* Connected qpairs */
	qpair.poll_group_tailq_head = &tgroup.connected_qpairs;
	STAILQ_INSERT_TAIL(&tgroup.connected_qpairs, &qpair, poll_group_stailq);
	tgroup.num_connected_qpairs++;

	rc = nvme_transport_poll_group_disconnect_qpair(&qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(STAILQ_EMPTY(&tgroup.connected_qpairs));
	CU_ASSERT(tgroup.num_connected_qpairs == 0);
	CU_ASSERT(!STAILQ_EMPTY(&tgroup.disconnected_qpairs));
	STAILQ_REMOVE(&tgroup.disconnected_qpairs, &qpair, spdk_nvme_qpair, poll_group_stailq);
	CU_ASSERT(STAILQ_EMPTY(&tgroup.disconnected_qpairs));

	/* None qpairs */
	qpair.poll_group_tailq_head = NULL;

	rc = nvme_transport_poll_group_disconnect_qpair(&qpair);
	SPDK_CU_ASSERT_FATAL(rc == -EINVAL);
}

static void
test_nvme_transport_poll_group_add_remove(void)
{
	int rc;
	struct spdk_nvme_transport_poll_group tgroup = {};
	struct spdk_nvme_qpair qpair = {};
	const struct spdk_nvme_transport transport = {};

	tgroup.transport = &transport;
	qpair.poll_group = &tgroup;
	qpair.state = NVME_QPAIR_DISCONNECTED;
	STAILQ_INIT(&tgroup.connected_qpairs);
	STAILQ_INIT(&tgroup.disconnected_qpairs);

	/* Add qpair */
	rc = nvme_transport_poll_group_add(&tgroup, &qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(qpair.poll_group_tailq_head == &tgroup.disconnected_qpairs);
	CU_ASSERT(STAILQ_FIRST(&tgroup.disconnected_qpairs) == &qpair);

	/*  Remove disconnected_qpairs */
	SPDK_CU_ASSERT_FATAL(!STAILQ_EMPTY(&tgroup.disconnected_qpairs));

	rc = nvme_transport_poll_group_remove(&tgroup, &qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(STAILQ_EMPTY(&tgroup.disconnected_qpairs));
	CU_ASSERT(qpair.poll_group == NULL);
	CU_ASSERT(qpair.poll_group_tailq_head == NULL);

	/* Remove connected_qpairs */
	qpair.poll_group_tailq_head = &tgroup.connected_qpairs;
	STAILQ_INSERT_TAIL(&tgroup.connected_qpairs, &qpair, poll_group_stailq);

	rc = nvme_transport_poll_group_remove(&tgroup, &qpair);
	CU_ASSERT(rc == -EINVAL);

	STAILQ_REMOVE(&tgroup.connected_qpairs, &qpair, spdk_nvme_qpair, poll_group_stailq);

	/* Invalid qpair */
	qpair.poll_group_tailq_head = NULL;

	rc = nvme_transport_poll_group_remove(&tgroup, &qpair);
	CU_ASSERT(rc == -ENOENT);
}

static int
g_ut_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_memory_domain **domains, int array_size)
{
	return 1;
}

static void
test_ctrlr_get_memory_domains(void)
{
	struct spdk_nvme_ctrlr ctrlr = {
		.trid = {
			.trstring = "new_transport"
		}
	};
	struct spdk_nvme_transport new_transport = {
		.ops = { .ctrlr_get_memory_domains = g_ut_ctrlr_get_memory_domains }
	};

	ut_construct_transport(&new_transport, "new_transport");

	/* transport contains necessary op */
	CU_ASSERT(nvme_transport_ctrlr_get_memory_domains(&ctrlr, NULL, 0) == 1);

	/* transport doesn't contain necessary op */
	new_transport.ops.ctrlr_get_memory_domains = NULL;
	CU_ASSERT(nvme_transport_ctrlr_get_memory_domains(&ctrlr, NULL, 0) == 0);

	TAILQ_REMOVE(&g_spdk_nvme_transports, &new_transport, link);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_transport", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_get_transport);
	CU_ADD_TEST(suite, test_nvme_managed_queue_unsupported);
	CU_ADD_TEST(suite, test_nvme_managed_queue_dispatch);
	CU_ADD_TEST(suite, test_nvme_transport_register_ext);
	CU_ADD_TEST(suite, test_nvme_transport_register_legacy);
	CU_ADD_TEST(suite, test_nvme_transport_register_invalid_size);
	CU_ADD_TEST(suite, test_nvme_transport_poll_group_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_transport_poll_group_disconnect_qpair);
	CU_ADD_TEST(suite, test_nvme_transport_poll_group_add_remove);
	CU_ADD_TEST(suite, test_nvme_transport_poll_group_hooks);
	CU_ADD_TEST(suite, test_ctrlr_get_memory_domains);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
