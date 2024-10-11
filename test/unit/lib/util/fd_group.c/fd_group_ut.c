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
	int fd_parent, fd_child;
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

	/* Unnest child fd group from wrong parent fd group and verify that it fails. */
	rc = spdk_fd_group_unnest(not_parent, child);
	SPDK_CU_ASSERT_FATAL(rc == -EINVAL);

	/* Unnest child fd group from its parent fd group and verify it. */
	rc = spdk_fd_group_unnest(parent, child);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(child->parent == NULL);

	spdk_fd_group_remove(child, fd_child);
	SPDK_CU_ASSERT_FATAL(child->num_fds == 0);

	spdk_fd_group_remove(parent, fd_parent);
	SPDK_CU_ASSERT_FATAL(parent->num_fds == 0);

	rc = close(fd_child);
	CU_ASSERT(rc == 0);

	rc = close(fd_parent);
	CU_ASSERT(rc == 0);

	spdk_fd_group_destroy(child);
	spdk_fd_group_destroy(parent);
	spdk_fd_group_destroy(not_parent);
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

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
