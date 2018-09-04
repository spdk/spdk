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
#include "event/app.c"

#define test_argc 6

DEFINE_STUB_V(spdk_rpc_initialize, (const char *listen_addr));
DEFINE_STUB_V(spdk_rpc_finish, (void));
DEFINE_STUB_V(spdk_event_call, (struct spdk_event *event));
DEFINE_STUB_V(spdk_reactors_start, (void));
DEFINE_STUB_V(spdk_reactors_stop, (void *arg1, void *arg2));
DEFINE_STUB(spdk_reactors_init, int, (unsigned int max_delay_us), 0);
DEFINE_STUB_V(spdk_reactors_fini, (void));
DEFINE_STUB(spdk_event_allocate, struct spdk_event *, (uint32_t core, spdk_event_fn fn, void *arg1,
		void *arg2), NULL);
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);
DEFINE_STUB(spdk_app_get_core_mask, struct spdk_cpuset *, (void), NULL);
DEFINE_STUB_V(spdk_subsystem_config, (FILE *fp));
DEFINE_STUB_V(spdk_subsystem_init, (struct spdk_event *app_start_event));
DEFINE_STUB_V(spdk_subsystem_fini, (struct spdk_event *app_stop_event));
DEFINE_STUB(spdk_env_init, int, (const struct spdk_env_opts *opts), 0);
DEFINE_STUB_V(spdk_env_opts_init, (struct spdk_env_opts *opts));
DEFINE_STUB(spdk_env_get_core_count, uint32_t, (void), 1);
DEFINE_STUB_V(spdk_rpc_register_method, (const char *method, spdk_rpc_method_handler func,
		uint32_t state_mask));
DEFINE_STUB_V(spdk_rpc_set_state, (uint32_t state));


static void
unittest_usage(void)
{
}

static void
unittest_parse_args(int ch, char *arg)
{
}

static void
test_spdk_app_parse_args(void)
{
	spdk_app_parse_args_rvals_t rc;
	struct spdk_app_opts opts = {};
	struct option my_options[2] = {};
	char *valid_argv[test_argc] = {"app_ut",
				       "--wait-for-rpc",
				       "-d",
				       "-p0",
				       "-B",
				       "0000:81:00.0"
				      };
	char *invalid_argv_BW[test_argc] = {"app_ut",
					    "-B",
					    "0000:81:00.0",
					    "-W",
					    "0000:82:00.0",
					    "-cspdk.conf"
					   };
	/* currently use -z as our new option */
	char *argv_added_short_opt[test_argc] = {"app_ut",
						 "-z",
						 "-d",
						 "--wait-for-rpc",
						 "-p0",
						 "-cspdk.conf"
						};
	char *argv_added_long_opt[test_argc] = {"app_ut",
						"-cspdk.conf",
						"-d",
						"-r/var/tmp/spdk.sock",
						"--test-long-opt",
						"--wait-for-rpc"
					       };
	char *invalid_argv_missing_option[test_argc] = {"app_ut",
							"-d",
							"-p",
							"--wait-for-rpc",
							"--silence-noticelog"
							"-R"
						       };

	/* Test valid arguments. Expected result: PASS */
	rc = spdk_app_parse_args(test_argc, valid_argv, &opts, "", NULL, unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;

	/* Test invalid short option Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, argv_added_short_opt, &opts, "", NULL, unittest_parse_args,
				 NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;

	/* Test valid global and local options. Expected result: PASS */
	rc = spdk_app_parse_args(test_argc, argv_added_short_opt, &opts, "z", NULL, unittest_parse_args,
				 unittest_usage);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;

	/* Test invalid long option Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, argv_added_long_opt, &opts, "", NULL, unittest_parse_args,
				 NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;

	/* Test valid global and local options. Expected result: PASS */
	my_options[0].name = "test-long-opt";
	rc = spdk_app_parse_args(test_argc, argv_added_long_opt, &opts, "", my_options, unittest_parse_args,
				 unittest_usage);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;

	/* Test overlapping global and local options. Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, valid_argv, &opts, SPDK_APP_GETOPT_STRING, NULL,
				 unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;

	/* Specify -B and -W options at the same time. Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, invalid_argv_BW, &opts, "", NULL, unittest_parse_args, NULL);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;

	/* Omit necessary argument to option */
	rc = spdk_app_parse_args(test_argc, invalid_argv_missing_option, &opts, "", NULL,
				 unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
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
		CU_add_test(suite, "test_spdk_app_parse_args",
			    test_spdk_app_parse_args) == NULL
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
