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
#include "common/lib/test_env.c"
#include "notifications/notifications.c"

struct spdk_notification *g_notification = NULL;



static struct spdk_notification_type test_first = {
	.name = "test_first",
	.subsystem = NULL,
	.details_cb = NULL,
};

static struct spdk_notification_type test_second = {
	.name = "test_second",
	.subsystem = NULL,
	.details_cb = NULL,
};

static void notification_handler_cb(struct spdk_notification *notification,
				    void *ctx)
{
	g_notification = notification;
}

static void
notifications(void)
{
	const char **notification_names;
	size_t types_cnt;
	int rc;

	/* Register new notification types */
	spdk_register_notification_type(&test_first);

	rc = spdk_get_notificiation_types(NULL, &types_cnt);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(types_cnt == 1);

	spdk_register_notification_type(&test_second);

	rc = spdk_get_notificiation_types(NULL, &types_cnt);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(types_cnt == 2);

	notification_names = calloc(types_cnt, sizeof(char *));
	rc = spdk_get_notificiation_types(notification_names, &types_cnt);
	CU_ASSERT(rc == 0);
	CU_ASSERT(types_cnt == 2);

	SPDK_CU_ASSERT_FATAL(notification_names != NULL);
	SPDK_CU_ASSERT_FATAL(notification_names[0] != NULL);
	SPDK_CU_ASSERT_FATAL(notification_names[1] != NULL);

	CU_ASSERT(!strcmp(notification_names[0], "test_first"));
	CU_ASSERT(!strcmp(notification_names[1], "test_second"));

	/* Register for notification "new" */
	rc = spdk_notification_listen("not_existing", notification_handler_cb, NULL);
	CU_ASSERT(rc == -ENOENT);

	rc = spdk_notification_listen("test_first", notification_handler_cb, NULL);
	CU_ASSERT(rc == 0);

	/* Send notification */
	spdk_send_notification(&test_first, NULL);

	SPDK_CU_ASSERT_FATAL(g_notification != NULL);

	CU_ASSERT(!strcmp(g_notification->type->name, "test_first"));

	free(notification_names);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("app_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "notifications",
			    notifications) == NULL
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
