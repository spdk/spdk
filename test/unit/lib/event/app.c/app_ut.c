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

DEFINE_STUB_V(spdk_event_call, (struct spdk_event *event));
DEFINE_STUB(spdk_event_allocate, struct spdk_event *, (uint32_t core, spdk_event_fn fn, void *arg1,
		void *arg2), NULL);
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);
DEFINE_STUB_V(spdk_subsystem_init, (spdk_msg_fn cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_rpc_register_method, (const char *method, spdk_rpc_method_handler func,
		uint32_t state_mask));
DEFINE_STUB_V(spdk_rpc_set_state, (uint32_t state));
DEFINE_STUB(spdk_rpc_get_state, uint32_t, (void), SPDK_RPC_RUNTIME);
DEFINE_STUB_V(spdk_app_json_config_load, (const char *json_config_file, const char *rpc_addr,
		spdk_msg_fn cb_fn, void *cb_arg));

static void
unittest_usage(void)
{
}

static int
unittest_parse_args(int ch, char *arg)
{
	return 0;
}

static void
clean_opts(struct spdk_app_opts *opts)
{
	free(opts->pci_whitelist);
	opts->pci_whitelist = NULL;
	free(opts->pci_blacklist);
	opts->pci_blacklist = NULL;
	memset(opts, 0, sizeof(struct spdk_app_opts));
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
	clean_opts(&opts);

	/* Test invalid short option Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, argv_added_short_opt, &opts, "", NULL, unittest_parse_args,
				 NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
	clean_opts(&opts);

	/* Test valid global and local options. Expected result: PASS */
	rc = spdk_app_parse_args(test_argc, argv_added_short_opt, &opts, "z", NULL, unittest_parse_args,
				 unittest_usage);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;
	clean_opts(&opts);

	/* Test invalid long option Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, argv_added_long_opt, &opts, "", NULL, unittest_parse_args,
				 NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
	clean_opts(&opts);

	/* Test valid global and local options. Expected result: PASS */
	my_options[0].name = "test-long-opt";
	rc = spdk_app_parse_args(test_argc, argv_added_long_opt, &opts, "", my_options, unittest_parse_args,
				 unittest_usage);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;
	clean_opts(&opts);

	/* Test overlapping global and local options. Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, valid_argv, &opts, SPDK_APP_GETOPT_STRING, NULL,
				 unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
	clean_opts(&opts);

	/* Specify -B and -W options at the same time. Expected result: FAIL */
	rc = spdk_app_parse_args(test_argc, invalid_argv_BW, &opts, "", NULL, unittest_parse_args, NULL);
	SPDK_CU_ASSERT_FATAL(rc == SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
	clean_opts(&opts);

	/* Omit necessary argument to option */
	rc = spdk_app_parse_args(test_argc, invalid_argv_missing_option, &opts, "", NULL,
				 unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_FAIL);
	optind = 1;
	clean_opts(&opts);
}

static int
cpuset_check_range(struct spdk_cpuset *core_mask, uint32_t min, uint32_t max, bool isset)
{
	uint32_t core;
	for (core = min; core <= max; core++) {
		if (isset != spdk_cpuset_get_cpu(core_mask, core)) {
			return -1;
		}
	}
	return 0;
}

static void
test_spdk_app_affinity_groups(void)
{
	struct spdk_cpuset *group;
	spdk_app_parse_args_rvals_t rc;
	struct spdk_app_opts opts = {};
	char *valid_argv[2] = {"app_ut",
			       "--cpu-affinity-groups=group1@1;group2@[1-3];group3@0xF0"
			      };

	/* Test valid arguments. Expected result: PASS */
	rc = spdk_app_parse_args(2, valid_argv, &opts, "", NULL, unittest_parse_args, NULL);
	CU_ASSERT_EQUAL(rc, SPDK_APP_PARSE_ARGS_SUCCESS);
	optind = 1;

	/* group1 - core 0 set */
	group = spdk_app_get_affinity_group("group1");
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(cpuset_check_range(group, 0, 0, true) == 0);
	CU_ASSERT(cpuset_check_range(group, 1, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* group2 - core 1-3 set */
	group = spdk_app_get_affinity_group("group2");
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(cpuset_check_range(group, 0, 0, false) == 0);
	CU_ASSERT(cpuset_check_range(group, 1, 3, true) == 0);
	CU_ASSERT(cpuset_check_range(group, 4, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* group3 - core 4-7 set */
	group = spdk_app_get_affinity_group("group3");
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(cpuset_check_range(group, 0, 3, false) == 0);
	CU_ASSERT(cpuset_check_range(group, 4, 7, true) == 0);
	CU_ASSERT(cpuset_check_range(group, 8, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* group_not_defined */
	group = spdk_app_get_affinity_group("group_not_defined");
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(cpuset_check_range(group, 0, 7, false) == 0);
	CU_ASSERT(cpuset_check_range(group, 8, SPDK_CPUSET_SIZE - 1, true) == 0);

	/* get free cores */
	group = spdk_app_get_free_core_mask();
	SPDK_CU_ASSERT_FATAL(group != NULL);
	CU_ASSERT(cpuset_check_range(group, 0, 7, false) == 0);
	CU_ASSERT(cpuset_check_range(group, 8, SPDK_CPUSET_SIZE - 1, true) == 0);

	clean_opts(&opts);
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
			    test_spdk_app_parse_args) == NULL ||
		CU_add_test(suite, "test_spdk_app_affinity_groups",
			    test_spdk_app_affinity_groups) == NULL
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
