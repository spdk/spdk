/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "util/fd_group.c"

static int
fd_group_cb_fn(void *ctx)
{
	return 0;
}

static void
test_fd_group_basic(void)
{
	struct spdk_fd_group *fgrp;
	struct event_handler *ehdlr = NULL;
	int fd;
	int rc;
	int cb_arg;

	rc = spdk_fd_group_create(&fgrp);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	fd = epoll_create1(0);
	SPDK_CU_ASSERT_FATAL(fd >= 0);

	rc = SPDK_FD_GROUP_ADD(fgrp, fd, fd_group_cb_fn, &cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(fgrp->num_fds == 1);

	/* Verify that event handler is initialized correctly */
	ehdlr = TAILQ_FIRST(&fgrp->event_handlers);
	SPDK_CU_ASSERT_FATAL(ehdlr != NULL);
	CU_ASSERT(ehdlr->fd == fd);
	CU_ASSERT(ehdlr->state == EVENT_HANDLER_STATE_WAITING);
	CU_ASSERT(ehdlr->events == EPOLLIN);

	/* Modify event type and see if event handler is updated correctly */
	rc = spdk_fd_group_event_modify(fgrp, fd, EPOLLIN | EPOLLERR);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ehdlr = TAILQ_FIRST(&fgrp->event_handlers);
	SPDK_CU_ASSERT_FATAL(ehdlr != NULL);
	CU_ASSERT(ehdlr->events == (EPOLLIN | EPOLLERR));

	spdk_fd_group_remove(fgrp, fd);
	SPDK_CU_ASSERT_FATAL(fgrp->num_fds == 0);

	rc = close(fd);
	CU_ASSERT(rc == 0);

	spdk_fd_group_destroy(fgrp);
}

static void
test_fd_group_nest_unnest(void)
{
	struct spdk_fd_group *parent, *child, *not_parent;
	int fd_parent, fd_child, fd_child_2;
	int rc;
	int cb_arg;

	rc = spdk_fd_group_create(&parent);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_fd_group_create(&child);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_fd_group_create(&not_parent);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	fd_parent = epoll_create1(0);
	SPDK_CU_ASSERT_FATAL(fd_parent >= 0);

	fd_child = epoll_create1(0);
	SPDK_CU_ASSERT_FATAL(fd_child >= 0);

	fd_child_2 = epoll_create1(0);
	SPDK_CU_ASSERT_FATAL(fd_child_2 >= 0);

	rc = SPDK_FD_GROUP_ADD(parent, fd_parent, fd_group_cb_fn, &cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 1);

	rc = SPDK_FD_GROUP_ADD(child, fd_child, fd_group_cb_fn, &cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 1);

	/* Nest child fd group to a parent fd group and verify their relation */
	rc = spdk_fd_group_nest(parent, child);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(child->parent == parent);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 2);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 0);

	/* Register second child fd to the child fd group and verify that the parent fd group
	 * has the correct number of fds.
	 */
	rc = SPDK_FD_GROUP_ADD(child, fd_child_2, fd_group_cb_fn, &cb_arg);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 0);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 3);

	/* Unnest child fd group from wrong parent fd group and verify that it fails. */
	rc = spdk_fd_group_unnest(not_parent, child);
	SPDK_CU_ASSERT_FATAL(rc == -EINVAL);

	/* Unnest child fd group from its parent fd group and verify it. */
	rc = spdk_fd_group_unnest(parent, child);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(child->parent == NULL);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 1);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 2);

	spdk_fd_group_remove(child, fd_child);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 1);

	spdk_fd_group_remove(child, fd_child_2);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 0);

	spdk_fd_group_remove(parent, fd_parent);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 0);

	rc = close(fd_child);
	CU_ASSERT(rc == 0);

	rc = close(fd_child_2);
	CU_ASSERT(rc == 0);

	rc = close(fd_parent);
	CU_ASSERT(rc == 0);

	spdk_fd_group_destroy(child);
	spdk_fd_group_destroy(parent);
	spdk_fd_group_destroy(not_parent);
}

struct ut_fgrp {
	struct spdk_fd_group	*fgrp;
	size_t			num_fds;
#define UT_MAX_FDS 4
	int			fd[UT_MAX_FDS];
};

static void
test_fd_group_multi_nest(void)
{
	struct ut_fgrp fgrp[] = {
		{ .num_fds = 1 },
		{ .num_fds = 2 },
		{ .num_fds = 2 },
		{ .num_fds = 3 },
	};
	size_t i, j;
	int fd, rc;

	/* Create four fd_groups with the folowing hierarchy:
	 *           fgrp[0]
	 *           (fd:0)
	 *              |
	 *  fgrp[1]-----+-----fgrp[2]
	 * (fd:1,2)          (fd:3,4)
	 *     |
	 *  fgrp[3]
	 * (fd:5,6,7)
	 */
	for (i = 0; i < SPDK_COUNTOF(fgrp); i++) {
		rc = spdk_fd_group_create(&fgrp[i].fgrp);
		SPDK_CU_ASSERT_FATAL(rc == 0);
		for (j = 0; j < fgrp[i].num_fds; j++) {
			fgrp[i].fd[j] = fd = eventfd(0, 0);
			CU_ASSERT(fd >= 0);
			rc = SPDK_FD_GROUP_ADD(fgrp[i].fgrp, fd, fd_group_cb_fn, NULL);
			CU_ASSERT_EQUAL(rc, 0);
		}
	}

	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds, fgrp[0].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, fgrp[1].num_fds);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, fgrp[2].num_fds);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, fgrp[3].num_fds);

	rc = spdk_fd_group_nest(fgrp[0].fgrp, fgrp[2].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_fd_group_nest(fgrp[1].fgrp, fgrp[3].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_fd_group_nest(fgrp[0].fgrp, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(rc, 0);

	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[1].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[3].fgrp->parent, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds,
			fgrp[0].num_fds + fgrp[1].num_fds +
			fgrp[2].num_fds + fgrp[3].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, 0);

	/* Unnest fgrp[1] and verify that it now owns its own fds along with fgrp[3] fds */
	rc = spdk_fd_group_unnest(fgrp[0].fgrp, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(rc, 0);

	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[1].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[3].fgrp->parent, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds, fgrp[0].num_fds + fgrp[2].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, fgrp[1].num_fds + fgrp[3].num_fds);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, 0);

	/* Nest it again, keeping the same configuration */
	rc = spdk_fd_group_nest(fgrp[0].fgrp, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[1].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[3].fgrp->parent, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds,
			fgrp[0].num_fds + fgrp[1].num_fds +
			fgrp[2].num_fds + fgrp[3].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, 0);

	/* Add a new fd to the fgrp at the bottom, fgrp[3] */
	fgrp[3].fd[fgrp[3].num_fds++] = fd = eventfd(0, 0);
	rc = SPDK_FD_GROUP_ADD(fgrp[3].fgrp, fd, fd_group_cb_fn, NULL);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds,
			fgrp[0].num_fds + fgrp[1].num_fds +
			fgrp[2].num_fds + fgrp[3].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, 0);

	/* Remove one of the fds from fgrp[2] */
	fd = fgrp[2].fd[--fgrp[2].num_fds];
	spdk_fd_group_remove(fgrp[2].fgrp, fd);
	close(fd);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds,
			fgrp[0].num_fds + fgrp[1].num_fds +
			fgrp[2].num_fds + fgrp[3].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, 0);

	/* Unnest the fgrp at the bottom, fgrp[3] */
	rc = spdk_fd_group_unnest(fgrp[1].fgrp, fgrp[3].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[1].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_PTR_EQUAL(fgrp[3].fgrp->parent, NULL);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds, fgrp[0].num_fds + fgrp[1].num_fds + fgrp[2].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);
	CU_ASSERT_EQUAL(fgrp[3].fgrp->num_fds, fgrp[3].num_fds);

	/* Unnest the remaining fgrps, fgrp[1] and fgrp[2] */
	rc = spdk_fd_group_unnest(fgrp[0].fgrp, fgrp[1].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[1].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, fgrp[0].fgrp);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds, fgrp[0].num_fds + fgrp[2].num_fds);
	CU_ASSERT_EQUAL(fgrp[1].fgrp->num_fds, fgrp[1].num_fds);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, 0);

	rc = spdk_fd_group_unnest(fgrp[0].fgrp, fgrp[2].fgrp);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_PTR_EQUAL(fgrp[0].fgrp->parent, NULL);
	CU_ASSERT_PTR_EQUAL(fgrp[2].fgrp->parent, NULL);
	CU_ASSERT_EQUAL(fgrp[0].fgrp->num_fds, fgrp[0].num_fds);
	CU_ASSERT_EQUAL(fgrp[2].fgrp->num_fds, fgrp[2].num_fds);

	for (i = 0; i < SPDK_COUNTOF(fgrp); i++) {
		for (j = 0; j < fgrp[i].num_fds; j++) {
			spdk_fd_group_remove(fgrp[i].fgrp, fgrp[i].fd[j]);
			close(fgrp[i].fd[j]);
		}
		spdk_fd_group_destroy(fgrp[i].fgrp);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("fd_group", NULL, NULL);

	CU_ADD_TEST(suite, test_fd_group_basic);
	CU_ADD_TEST(suite, test_fd_group_nest_unnest);
	CU_ADD_TEST(suite, test_fd_group_multi_nest);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
