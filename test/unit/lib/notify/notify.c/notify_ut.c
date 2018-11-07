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

#include "unit/lib/json_mock.c"
#include "notify/notify.c"

struct spdk_notify *g_notify;

static void notify_get_type_info(struct spdk_json_write_ctx *w,
				 struct spdk_notify_type *type, void *ctx)
{

}

static void notify_get_info(struct spdk_json_write_ctx *w,
			    struct spdk_notify *notify, void *ctx)
{

}

static void notify_handler_cb(struct spdk_notify *notify,
			      void *ctx)
{
	SPDK_CU_ASSERT_FATAL(notify != NULL);
	spdk_notify_get(notify);
	g_notify = notify;
	SPDK_CU_ASSERT_FATAL(notify->refcnt == 2);
}

static void
notify(void)
{
	struct spdk_notify_type ntype1 = {
		.name = "type1",
		.write_type_cb = notify_get_type_info,
		.write_info_cb = notify_get_info,
	};

	struct spdk_notify_type ntype2 = {
		.name = "type1",
		.write_type_cb = notify_get_type_info,
		.write_info_cb = notify_get_info,
	};

	struct spdk_notify_type *ntype;
	struct spdk_notify *notify;
	int rc;

	/* Register new notify types */
	spdk_notify_type_register(&ntype1);
	spdk_notify_type_register(&ntype2);

	ntype = spdk_notify_type_first();
	SPDK_CU_ASSERT_FATAL(ntype != NULL);
	CU_ASSERT(!strcmp(ntype->name, "type1"));

	ntype = spdk_notify_type_next(ntype);
	SPDK_CU_ASSERT_FATAL(ntype != NULL);
	CU_ASSERT(!strcmp(ntype->name, "type2"));

	/* Register for notifications. */
	rc = spdk_notify_listen(notify_handler_cb, NULL);
	CU_ASSERT(rc == 0);

	/* Send notify */
	notify = spdk_notify_alloc(&ntype1);
	SPDK_CU_ASSERT_FATAL(notify != NULL);
	/* refcnt must be set to 1 in spdk_notify_alloc() so it can be put later. */
	SPDK_CU_ASSERT_FATAL(notify->refcnt == 1);

	spdk_notify_send(notify);

	SPDK_CU_ASSERT_FATAL(g_notify == notify);

	/* refcnt must be decremented before spdk_notify_send returns. */
	SPDK_CU_ASSERT_FATAL(notify->refcnt == 1);

	spdk_notify_put(notify);
	g_notify = NULL;

	/* Stop listening for notifications. */
	rc = spdk_notify_unlisten(notify_handler_cb, NULL);
	CU_ASSERT(rc == 0);

	/* No one is listening, but send notification to see if listener was unregistered. */
	notify = spdk_notify_alloc(&ntype2);
	SPDK_CU_ASSERT_FATAL(notify != NULL);

	g_notify = NULL;
	spdk_notify_send(notify);

	CU_ASSERT(g_notify == NULL);
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
