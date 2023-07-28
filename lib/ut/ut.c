/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk_internal/cunit.h"

enum ut_action {
	UT_ACTION_RUN_TESTS,
	UT_ACTION_PRINT_HELP,
	UT_ACTION_LIST_TESTS,
};

struct ut_config {
	const char			*app;
	const char			*test;
	const char			*suite;
	enum ut_action			action;
	const struct spdk_ut_opts	*opts;
};

#define OPTION_STRING "hls:t:"

static const struct option g_ut_options[] = {
#define OPTION_TEST_CASE 't'
	{"test", required_argument, NULL, OPTION_TEST_CASE},
#define OPTION_TEST_SUITE 's'
	{"suite", required_argument, NULL, OPTION_TEST_SUITE},
#define OPTION_LIST 'l'
	{"list", no_argument, NULL, OPTION_LIST},
#define OPTION_HELP 'h'
	{"help", no_argument, NULL, OPTION_HELP},
	{},
};

static void
usage(struct ut_config *config)
{
	const struct spdk_ut_opts *opts = config->opts;

	printf("Usage: %s [OPTIONS]\n", config->app);
	printf("  -t, --test                       run single test case\n");
	printf("  -s, --suite                      run all tests in a given suite\n");
	printf("  -l, --list                       list registered test suites and test cases\n");
	printf("  -h, --help                       print this help\n");

	if (opts != NULL && opts->usage_cb_fn != NULL) {
		opts->usage_cb_fn(opts->cb_arg);
	}
}

static int
parse_args(int argc, char **argv, struct ut_config *config)
{
	const struct spdk_ut_opts *opts = config->opts;
#define MAX_OPTSTRING_LEN 4096
	char optstring[MAX_OPTSTRING_LEN] = {};
#define MAX_OPT_COUNT 128
	struct option options[MAX_OPT_COUNT] = {};
	size_t optlen;
	int op, rc;

	/* Run the tests by default */
	config->action = UT_ACTION_RUN_TESTS;
	config->app = argv[0];

	if (opts != NULL && opts->opts != NULL) {
		optlen = SPDK_COUNTOF(g_ut_options) + opts->optlen;
		if (optlen > MAX_OPT_COUNT) {
			fprintf(stderr, "%s: unsupported number of options: %zu\n",
				config->app, optlen);
			return -EINVAL;
		}

		memcpy(&options[0], opts->opts, sizeof(*opts->opts) * opts->optlen);
		memcpy(&options[opts->optlen], g_ut_options, sizeof(g_ut_options));

		rc = snprintf(optstring, MAX_OPTSTRING_LEN, "%s%s", OPTION_STRING,
			      opts->optstring);
		if (rc < 0 || rc >= MAX_OPTSTRING_LEN) {
			fprintf(stderr, "%s: bad optstring\n", config->app);
			return -EINVAL;
		}
	} else {
		snprintf(optstring, sizeof(optstring), "%s", OPTION_STRING);
		memcpy(options, g_ut_options, sizeof(g_ut_options));
	}

	while ((op = getopt_long(argc, argv, optstring, options, NULL)) != -1) {
		switch (op) {
		case OPTION_TEST_CASE:
			config->test = optarg;
			break;
		case OPTION_TEST_SUITE:
			config->suite = optarg;
			break;
		case OPTION_HELP:
			config->action = UT_ACTION_PRINT_HELP;
			break;
		case OPTION_LIST:
			config->action = UT_ACTION_LIST_TESTS;
			break;
		case '?':
			return -EINVAL;
		default:
			if (opts != NULL && opts->option_cb_fn != NULL) {
				rc = opts->option_cb_fn(op, optarg, opts->cb_arg);
				if (rc != 0) {
					return rc;
				}
			} else {
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int
run_tests(const struct ut_config *config)
{
	CU_pSuite suite = NULL;
	CU_pTest test = NULL;

	if (config->suite != NULL) {
		suite = CU_get_suite(config->suite);
		if (suite == NULL) {
			fprintf(stderr, "%s: invalid test suite: '%s'\n",
				config->app, config->suite);
			return 1;
		}
	}

	if (config->test != NULL) {
		if (suite == NULL) {
			/* Allow users to skip test suite if there's only a single test suite
			 * registered (CUnit starts indexing from 1). */
			if (CU_get_suite_at_pos(2) != NULL) {
				fprintf(stderr, "%s: there are multiple test suites registered, "
					"select one using the -s option\n", config->app);
				return 1;
			}

			suite = CU_get_suite_at_pos(1);
			if (suite == NULL) {
				fprintf(stderr, "%s: there are no tests registered\n", config->app);
				return 1;
			}
		}

		test = CU_get_test(suite, config->test);
		if (test == NULL) {
			fprintf(stderr, "%s: invalid test case: '%s'\n", config->app, config->test);
			return 1;
		}
	}

	CU_set_error_action(CUEA_ABORT);
	CU_basic_set_mode(CU_BRM_VERBOSE);

	/* Either run a single test, all tests in a given test suite, or all registered tests */
	if (test != NULL) {
		CU_basic_run_test(suite, test);
	} else if (suite != NULL) {
		CU_basic_run_suite(suite);
	} else {
		CU_basic_run_tests();
	}

	return CU_get_number_of_failures();
}

static void
list_tests(void)
{
	CU_pSuite suite;
	CU_pTest test;
	int sid, tid;

	for (sid = 1;; ++sid) {
		suite = CU_get_suite_at_pos(sid);
		if (suite == NULL) {
			break;
		}

		printf("%s:\n", suite->pName);
		for (tid = 1;; ++tid) {
			test = CU_get_test_at_pos(suite, tid);
			if (test == NULL) {
				break;
			}

			printf("  %s\n", test->pName);
		}
	}
}

int
spdk_ut_run_tests(int argc, char **argv, const struct spdk_ut_opts *opts)
{
	struct ut_config config = {.opts = opts};
	int rc;

	rc = parse_args(argc, argv, &config);
	if (rc != 0) {
		usage(&config);
		return 1;
	}

	switch (config.action) {
	case UT_ACTION_PRINT_HELP:
		usage(&config);
		break;
	case UT_ACTION_RUN_TESTS:
		if (opts != NULL && opts->init_cb_fn != NULL) {
			rc = opts->init_cb_fn(opts->cb_arg);
			if (rc != 0) {
				usage(&config);
				return 1;
			}
		}

		rc = run_tests(&config);
		break;
	case UT_ACTION_LIST_TESTS:
		list_tests();
		break;
	default:
		assert(0);
	}

	return rc;
}
