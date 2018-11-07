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
#include "notify/notify.c"

struct spdk_notify *g_notify = NULL;
const char *g_name = "name0";

static const char *notify_get_object(struct spdk_notify_type *notify,
				     void *ctx)
{
	return g_name;
}

static const char *notify_get_uuid(struct spdk_notify_type *notify,
				   void *ctx)
{
	return g_name;
}

static void notify_handler_cb(struct spdk_notify *notify,
			      void *ctx)
{
	g_notify = notify;
}

static void
notify(void)
{
	struct spdk_notify_type *ntype;
	struct spdk_notify *notify;
	int rc;

	/* Register new notify types */
	spdk_notify_register_type("test_first", notify_get_object, notify_get_uuid);
	spdk_notify_register_type("test_second", notify_get_object, notify_get_uuid);

	ntype = spdk_notify_first();

	CU_ASSERT(!strcmp(ntype->name, "test_first"));

	ntype = spdk_notify_next(ntype);

	CU_ASSERT(!strcmp(ntype->name, "test_second"));

	/* Register for notify "new" */
	rc = spdk_notify_listen(notify_handler_cb, NULL);
	CU_ASSERT(rc == 0);

	/* Send notify */

	notify = calloc(1, sizeof(*notify));

	SPDK_CU_ASSERT_FATAL(notify != NULL);

	notify->type = ntype;
	notify->ctx = NULL;

	spdk_notify_send(notify, NULL);

	SPDK_CU_ASSERT_FATAL(g_notify != NULL);

	CU_ASSERT(!strcmp(g_notify->type->name, "test_second"));

	/* no more subscribers, notification send to void */
	g_notify = NULL;
	spdk_notify_send(notify, NULL);

	CU_ASSERT(g_notify == NULL);

	free(notify);
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
		CU_add_test(suite, "notify",
			    notify) == NULL
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
