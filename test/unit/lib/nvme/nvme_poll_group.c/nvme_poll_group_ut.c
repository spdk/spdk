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

#include "spdk_cunit.h"

#include "nvme/nvme_poll_group.c"
#include "common/lib/test_env.c"

struct spdk_nvme_transport {
	const char				name[32];
	TAILQ_ENTRY(spdk_nvme_transport)	link;
};

struct spdk_nvme_transport t1 = {
	.name = "transport1",
};

struct spdk_nvme_transport t2 = {
	.name = "transport2",
};

struct spdk_nvme_transport t3 = {
	.name = "transport3",
};

struct spdk_nvme_transport t4 = {
	.name = "transport4",
};

int64_t g_process_completions_return_value = 0;
int g_destroy_return_value = 0;

TAILQ_HEAD(nvme_transport_list, spdk_nvme_transport) g_spdk_nvme_transports =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvme_transports);

static void
unit_test_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{

}

const struct spdk_nvme_transport *
nvme_get_first_transport(void)
{
	return TAILQ_FIRST(&g_spdk_nvme_transports);
}

const struct spdk_nvme_transport *
nvme_get_next_transport(const struct spdk_nvme_transport *transport)
{
	return TAILQ_NEXT(transport, link);
}

int
nvme_transport_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_qpair *iter_qp, *tmp_iter_qp;

	tgroup = qpair->poll_group;

	STAILQ_FOREACH_SAFE(iter_qp, &tgroup->connected_qpairs, poll_group_stailq, tmp_iter_qp) {
		if (qpair == iter_qp) {
			STAILQ_REMOVE(&tgroup->connected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
			STAILQ_INSERT_TAIL(&tgroup->disconnected_qpairs, qpair, poll_group_stailq);
			return 0;
		}
	}

	STAILQ_FOREACH(iter_qp, &tgroup->disconnected_qpairs, poll_group_stailq) {
		if (qpair == iter_qp) {
			return 0;
		}
	}

	return -EINVAL;
}

int
nvme_transport_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_qpair *iter_qp, *tmp_iter_qp;

	tgroup = qpair->poll_group;

	STAILQ_FOREACH_SAFE(iter_qp, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_iter_qp) {
		if (qpair == iter_qp) {
			STAILQ_REMOVE(&tgroup->disconnected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
			STAILQ_INSERT_TAIL(&tgroup->connected_qpairs, qpair, poll_group_stailq);
			return 0;
		}
	}

	STAILQ_FOREACH(iter_qp, &tgroup->connected_qpairs, poll_group_stailq) {
		if (qpair == iter_qp) {
			return 0;
		}
	}

	return -EINVAL;
}

struct spdk_nvme_transport_poll_group *
nvme_transport_poll_group_create(const struct spdk_nvme_transport *transport)
{
	struct spdk_nvme_transport_poll_group *group = NULL;

	/* TODO: separate this transport function table from the transport specific one. */
	group = calloc(1, sizeof(*group));
	if (group) {
		group->transport = transport;
		STAILQ_INIT(&group->connected_qpairs);
		STAILQ_INIT(&group->disconnected_qpairs);
	}

	return group;
}

int
nvme_transport_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	return g_destroy_return_value;
}

int
nvme_transport_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_qpair *qpair)
{
	STAILQ_INSERT_TAIL(&tgroup->connected_qpairs, qpair, poll_group_stailq);
	qpair->poll_group = tgroup;

	return 0;
}

int
nvme_transport_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				 struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_qpair *iter_qp, *tmp_iter_qp;

	STAILQ_FOREACH_SAFE(iter_qp, &tgroup->connected_qpairs, poll_group_stailq, tmp_iter_qp) {
		if (qpair == iter_qp) {
			STAILQ_REMOVE(&tgroup->connected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
			return 0;
		}
	}

	STAILQ_FOREACH_SAFE(iter_qp, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_iter_qp) {
		if (qpair == iter_qp) {
			STAILQ_REMOVE(&tgroup->disconnected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
			return 0;
		}
	}

	return -ENODEV;
}

int64_t
nvme_transport_poll_group_process_completions(struct spdk_nvme_transport_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	return g_process_completions_return_value;
}

static void
test_spdk_nvme_poll_group_create(void)
{
	struct spdk_nvme_poll_group *group;

	/* basic case - create a poll group with no internal transport poll groups. */
	group = spdk_nvme_poll_group_create(NULL);

	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->tgroups));
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t1, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t2, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t3, link);

	/* advanced case - create a poll group with three internal poll groups. */
	group = spdk_nvme_poll_group_create(NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->tgroups));
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	/* Failing case - failed to allocate a poll group. */
	MOCK_SET(calloc, NULL);
	group = spdk_nvme_poll_group_create(NULL);
	CU_ASSERT(group == NULL);
	MOCK_CLEAR(calloc);

	TAILQ_REMOVE(&g_spdk_nvme_transports, &t1, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t2, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t3, link);
}

static void
test_spdk_nvme_poll_group_add_remove(void)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_transport_poll_group *tgroup = NULL, *tmp_tgroup, *tgroup_1 = NULL,
						       *tgroup_2 = NULL,
							*tgroup_4 = NULL;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_qpair qpair1_1 = {0};
	struct spdk_nvme_qpair qpair1_2 = {0};
	struct spdk_nvme_qpair qpair2_1 = {0};
	struct spdk_nvme_qpair qpair2_2 = {0};
	struct spdk_nvme_qpair qpair4_1 = {0};
	struct spdk_nvme_qpair qpair4_2 = {0};
	int i = 0;

	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t1, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t2, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t3, link);

	group = spdk_nvme_poll_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->tgroups));

	/* Add qpairs to a single transport. */
	qpair1_1.transport = &t1;
	qpair1_1.state = NVME_QPAIR_DISCONNECTED;
	qpair1_2.transport = &t1;
	qpair1_2.state = NVME_QPAIR_ENABLED;
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair1_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair1_2) == -EINVAL);
	STAILQ_FOREACH(tmp_tgroup, &group->tgroups, link) {
		if (tmp_tgroup->transport == &t1) {
			tgroup = tmp_tgroup;
		} else {
			CU_ASSERT(STAILQ_EMPTY(&tmp_tgroup->connected_qpairs));
		}
		i++;
	}
	CU_ASSERT(i == 1);
	SPDK_CU_ASSERT_FATAL(tgroup != NULL);
	qpair = STAILQ_FIRST(&tgroup->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair1_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);

	/* Add qpairs to a second transport. */
	qpair2_1.transport = &t2;
	qpair2_2.transport = &t2;
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair2_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair2_2) == 0);
	qpair4_1.transport = &t4;
	qpair4_2.transport = &t4;
	/* Add qpairs for a transport that doesn't exist. */
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair4_1) == -ENODEV);
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair4_2) == -ENODEV);
	i = 0;
	STAILQ_FOREACH(tmp_tgroup, &group->tgroups, link) {
		if (tmp_tgroup->transport == &t1) {
			tgroup_1 = tmp_tgroup;
		} else if (tmp_tgroup->transport == &t2) {
			tgroup_2 = tmp_tgroup;
		} else {
			CU_ASSERT(STAILQ_EMPTY(&tmp_tgroup->connected_qpairs));
		}
		i++;
	}
	CU_ASSERT(i == 2);
	SPDK_CU_ASSERT_FATAL(tgroup_1 != NULL);
	qpair = STAILQ_FIRST(&tgroup_1->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair1_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);
	SPDK_CU_ASSERT_FATAL(tgroup_2 != NULL);
	qpair = STAILQ_FIRST(&tgroup_2->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair2_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair2_2);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);

	/* Try removing a qpair that belongs to a transport not in our poll group. */
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair4_1) == -ENODEV);

	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t4, link);
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair4_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair4_2) == 0);
	STAILQ_FOREACH(tmp_tgroup, &group->tgroups, link) {
		if (tmp_tgroup->transport == &t1) {
			tgroup_1 = tmp_tgroup;
		} else if (tmp_tgroup->transport == &t2) {
			tgroup_2 = tmp_tgroup;
		} else if (tmp_tgroup->transport == &t4) {
			tgroup_4 = tmp_tgroup;
		} else {
			CU_ASSERT(STAILQ_EMPTY(&tmp_tgroup->connected_qpairs));
		}
	}
	SPDK_CU_ASSERT_FATAL(tgroup_1 != NULL);
	qpair = STAILQ_FIRST(&tgroup_1->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair1_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);
	SPDK_CU_ASSERT_FATAL(tgroup_2 != NULL);
	qpair = STAILQ_FIRST(&tgroup_2->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair2_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair2_2);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);
	SPDK_CU_ASSERT_FATAL(tgroup_4 != NULL);
	qpair = STAILQ_FIRST(&tgroup_4->connected_qpairs);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair4_1);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	SPDK_CU_ASSERT_FATAL(qpair == &qpair4_2);
	qpair = STAILQ_NEXT(qpair, poll_group_stailq);
	CU_ASSERT(qpair == NULL);

	/* remove all qpairs */
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair1_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair2_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair2_2) == 0);
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair4_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair4_2) == 0);
	/* Confirm the fourth transport group was created. */
	i = 0;
	STAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp_tgroup) {
		CU_ASSERT(STAILQ_EMPTY(&tgroup->connected_qpairs));
		STAILQ_REMOVE(&group->tgroups, tgroup, spdk_nvme_transport_poll_group, link);
		free(tgroup);
		i++;
	}
	CU_ASSERT(i == 3);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	TAILQ_REMOVE(&g_spdk_nvme_transports, &t1, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t2, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t3, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t4, link);
}

static void
test_spdk_nvme_poll_group_process_completions(void)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_transport_poll_group *tgroup, *tmp_tgroup;
	struct spdk_nvme_qpair qpair1_1 = {0};

	group = spdk_nvme_poll_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);

	/* If we don't have any transport poll groups, we shouldn't get any completions. */
	g_process_completions_return_value = 32;
	CU_ASSERT(spdk_nvme_poll_group_process_completions(group, 128,
			unit_test_disconnected_qpair_cb) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t1, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t2, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t3, link);

	/* try it with three transport poll groups. */
	group = spdk_nvme_poll_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);
	qpair1_1.state = NVME_QPAIR_DISCONNECTED;
	qpair1_1.transport = &t1;
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair1_1) == 0);
	qpair1_1.state = NVME_QPAIR_ENABLED;
	CU_ASSERT(nvme_poll_group_connect_qpair(&qpair1_1) == 0);
	CU_ASSERT(spdk_nvme_poll_group_process_completions(group, 128,
			unit_test_disconnected_qpair_cb) == 32);
	CU_ASSERT(spdk_nvme_poll_group_remove(group, &qpair1_1) == 0);
	STAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp_tgroup) {
		CU_ASSERT(STAILQ_EMPTY(&tgroup->connected_qpairs));
		STAILQ_REMOVE(&group->tgroups, tgroup, spdk_nvme_transport_poll_group, link);
		free(tgroup);
	}
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	TAILQ_REMOVE(&g_spdk_nvme_transports, &t1, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t2, link);
	TAILQ_REMOVE(&g_spdk_nvme_transports, &t3, link);
}

static void
test_spdk_nvme_poll_group_destroy(void)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_transport_poll_group *tgroup, *tgroup_1, *tgroup_2;
	struct spdk_nvme_qpair qpair1_1 = {0};
	int num_tgroups = 0;

	/* Simple destruction of empty poll group. */
	group = spdk_nvme_poll_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);

	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t1, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t2, link);
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, &t3, link);
	group = spdk_nvme_poll_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);

	qpair1_1.transport = &t1;
	CU_ASSERT(spdk_nvme_poll_group_add(group, &qpair1_1) == 0);

	/* Don't remove busy poll groups. */
	g_destroy_return_value = -EBUSY;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == -EBUSY);
	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		num_tgroups++;
	}
	CU_ASSERT(num_tgroups == 1);

	/* destroy poll group with internal poll groups. */
	g_destroy_return_value = 0;
	tgroup_1 = STAILQ_FIRST(&group->tgroups);
	tgroup_2 = STAILQ_NEXT(tgroup_1, link);
	CU_ASSERT(tgroup_2 == NULL)
	SPDK_CU_ASSERT_FATAL(spdk_nvme_poll_group_destroy(group) == 0);
	free(tgroup_1);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ns_cmd", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "nvme_poll_group_create_test", test_spdk_nvme_poll_group_create) == NULL ||
		CU_add_test(suite, "nvme_poll_group_add_remove_test",
			    test_spdk_nvme_poll_group_add_remove) == NULL ||
		CU_add_test(suite, "nvme_poll_group_process_completions",
			    test_spdk_nvme_poll_group_process_completions) == NULL ||
		CU_add_test(suite, "nvme_poll_group_destroy_test", test_spdk_nvme_poll_group_destroy) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
