/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "unit/lib/json_mock.c"
#include "notify/notify.c"

static int
event_cb(uint64_t idx, const struct spdk_notify_event *event, void *ctx)
{
	const struct spdk_notify_event **_event = ctx;

	*_event = event;
	return 0;
}

static void
notify(void)
{
	struct spdk_notify_type *n1, *n2;
	const struct spdk_notify_event *event;
	const char *name;
	uint64_t cnt;

	n1 = spdk_notify_type_register("one");
	n2 = spdk_notify_type_register("two");

	name = spdk_notify_type_get_name(n1);
	CU_ASSERT(strcmp(name, "one") == 0);

	name = spdk_notify_type_get_name(n2);
	CU_ASSERT(strcmp(name, "two") == 0);


	spdk_notify_send("one", "one_context");
	spdk_notify_send("two", "two_context");

	event = NULL;
	cnt = spdk_notify_foreach_event(0, 1, event_cb, &event);
	SPDK_CU_ASSERT_FATAL(cnt == 1);
	SPDK_CU_ASSERT_FATAL(event != NULL);
	CU_ASSERT(strcmp(event->type, "one") == 0);
	CU_ASSERT(strcmp(event->ctx, "one_context") == 0);

	event = NULL;
	cnt = spdk_notify_foreach_event(1, 1, event_cb, &event);
	SPDK_CU_ASSERT_FATAL(cnt == 1);
	SPDK_CU_ASSERT_FATAL(event != NULL);
	CU_ASSERT(strcmp(event->type, "two") == 0);
	CU_ASSERT(strcmp(event->ctx, "two_context") == 0);

	/* This event should not exist yet */
	event = NULL;
	cnt = spdk_notify_foreach_event(2, 1, event_cb, &event);
	CU_ASSERT(cnt == 0);
	CU_ASSERT(event == NULL);

	SPDK_CU_ASSERT_FATAL(event == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("app_suite", NULL, NULL);
	CU_ADD_TEST(suite, notify);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
