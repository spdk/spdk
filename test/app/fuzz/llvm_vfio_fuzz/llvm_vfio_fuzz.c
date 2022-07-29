/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "spdk/file.h"

#include "spdk/vfio_user_pci.h"
#include <linux/vfio.h>
#include "spdk/vfio_user_spec.h"

#define VFIO_MAXIMUM_SPARSE_MMAP_REGIONS	8

typedef int (*fuzzer_fn)(const uint8_t *data, size_t size, struct vfio_device *dev);
struct fuzz_type {
	fuzzer_fn				fn;
	uint32_t				bytes_per_cmd;
};

#define VFIO_USER_MAX_PAYLOAD_SIZE		(4096)
static uint8_t					payload[VFIO_USER_MAX_PAYLOAD_SIZE];

static char					*g_ctrlr_path;
static int32_t					g_time_in_sec = 10;
static char					*g_corpus_dir;
static uint8_t					*g_repro_data;
static size_t					g_repro_size;
static pthread_t				g_fuzz_td;
static pthread_t				g_reactor_td;
static bool					g_in_fuzzer;
static struct fuzz_type				*g_fuzzer;

static int
fuzz_vfio_user_version(const uint8_t *data, size_t size, struct vfio_device *dev)
{
	struct vfio_user_version *version = (struct vfio_user_version *)payload;

	version->major = ((uint16_t)data[0] << 8) + (uint16_t)data[1];
	version->minor = ((uint16_t)data[2] << 8) + (uint16_t)data[3];

	return spdk_vfio_user_dev_send_request(dev, VFIO_USER_VERSION, payload,
					       sizeof(struct vfio_user_version),
					       sizeof(payload), NULL, 0);
}

static struct fuzz_type g_fuzzers[] = {
	{ .fn = fuzz_vfio_user_version,				.bytes_per_cmd = 4},
	{ .fn = NULL,						.bytes_per_cmd = 0}
};

#define NUM_FUZZERS (SPDK_COUNTOF(g_fuzzers) - 1)

static int
TestOneInput(const uint8_t *data, size_t size)
{
	struct vfio_device *dev = NULL;
	char ctrlr_path[PATH_MAX];
	int ret = 0;

	snprintf(ctrlr_path, sizeof(ctrlr_path), "%s/cntrl", g_ctrlr_path);
	ret = access(ctrlr_path, F_OK);
	if (ret != 0) {
		fprintf(stderr, "Access path %s failed\n", ctrlr_path);
		spdk_app_stop(-1);
		return -1;
	}

	dev = spdk_vfio_user_setup(ctrlr_path);
	if (dev == NULL) {
		fprintf(stderr, "spdk_vfio_user_setup() failed for controller path '%s'\n",
			ctrlr_path);
		spdk_app_stop(-1);
		return -1;
	}

	/* run cmds here */
	if (g_fuzzer->fn != NULL) {
		g_fuzzer->fn(data, size, dev);
	}

	spdk_vfio_user_release(dev);
	return 0;
}

int LLVMFuzzerRunDriver(int *argc, char ***argv, int (*UserCb)(const uint8_t *Data, size_t Size));

static void
exit_handler(void)
{
	if (g_in_fuzzer) {
		spdk_app_stop(0);
		pthread_join(g_reactor_td, NULL);
	}
}

static void *
start_fuzzer(void *ctx)
{
	char *_argv[] = {
		"spdk",
		"-len_control=0",
		"-detect_leaks=1",
		NULL,
		NULL,
		NULL
	};
	char time_str[128];
	char len_str[128];
	char **argv = _argv;
	int argc = SPDK_COUNTOF(_argv);
	uint32_t len = 0;

	spdk_unaffinitize_thread();
	len = 10 * g_fuzzer->bytes_per_cmd;
	snprintf(len_str, sizeof(len_str), "-max_len=%d", len);
	argv[argc - 3] = len_str;
	snprintf(time_str, sizeof(time_str), "-max_total_time=%d", g_time_in_sec);
	argv[argc - 2] = time_str;
	argv[argc - 1] = g_corpus_dir;

	g_in_fuzzer = true;
	atexit(exit_handler);

	if (g_repro_data) {
		printf("Running single test based on reproduction data file.\n");
		TestOneInput(g_repro_data, g_repro_size);
		printf("Done.\n");
	} else {
		LLVMFuzzerRunDriver(&argc, &argv, TestOneInput);
		/* TODO: in the normal case, LLVMFuzzerRunDriver never returns - it calls exit()
		 * directly and we never get here.  But this behavior isn't really documented
		 * anywhere by LLVM, so call spdk_app_stop(0) if it does return, which will
		 * result in the app exiting like a normal SPDK application (spdk_app_start()
		 * returns to main().
		 */
	}

	g_in_fuzzer = false;
	spdk_app_stop(0);

	return NULL;
}

static void
begin_fuzz(void *ctx)
{
	g_reactor_td = pthread_self();

	pthread_create(&g_fuzz_td, NULL, start_fuzzer, NULL);
}

static void
vfio_fuzz_usage(void)
{
	fprintf(stderr, " -D                        Path of corpus directory.\n");
	fprintf(stderr, " -F                        Path for ctrlr that should be fuzzed.\n");
	fprintf(stderr, " -N                        Name of reproduction data file.\n");
	fprintf(stderr, " -t                        Time to run fuzz tests (in seconds). Default: 10\n");
	fprintf(stderr, " -Z                        Fuzzer to run (0 to %lu)\n", NUM_FUZZERS - 1);
}

static int
vfio_fuzz_parse(int ch, char *arg)
{
	long long tmp = 0;
	FILE *repro_file = NULL;

	switch (ch) {
	case 'D':
		g_corpus_dir = strdup(optarg);
		if (!g_corpus_dir) {
			fprintf(stderr, "cannot strdup: %s\n", optarg);
			return -ENOMEM;
		}
		break;
	case 'F':
		g_ctrlr_path = strdup(optarg);
		if (!g_ctrlr_path) {
			fprintf(stderr, "cannot strdup: %s\n", optarg);
			return -ENOMEM;
		}
		break;
	case 'N':
		repro_file = fopen(optarg, "r");
		if (repro_file == NULL) {
			fprintf(stderr, "could not open %s: %s\n", optarg, spdk_strerror(errno));
			return -1;
		}
		g_repro_data = spdk_posix_file_load(repro_file, &g_repro_size);
		if (g_repro_data == NULL) {
			fprintf(stderr, "could not load data for file %s\n", optarg);
			return -1;
		}
		break;
	case 't':
	case 'Z':
		tmp = spdk_strtoll(optarg, 10);
		if (tmp < 0 || tmp >= INT_MAX) {
			fprintf(stderr, "Invalid value '%s' for option -%c.\n", optarg, ch);
			return -EINVAL;
		}
		switch (ch) {
		case 't':
			g_time_in_sec = tmp;
			break;
		case 'Z':
			if ((unsigned long)tmp >= NUM_FUZZERS) {
				fprintf(stderr, "Invalid fuzz type %lld (max %lu)\n", tmp, NUM_FUZZERS - 1);
				return -EINVAL;
			}
			g_fuzzer = &g_fuzzers[tmp];
			break;
		}
		break;
	case '?':
	default:
		return -EINVAL;
	}
	return 0;
}

static void
fuzz_shutdown(void)
{
	/* If the user terminates the fuzzer prematurely, it is likely due
	 * to an input hang.  So raise a SIGSEGV signal which will cause the
	 * fuzzer to generate a crash file for the last input.
	 *
	 * Note that the fuzzer will always generate a crash file, even if
	 * we get our TestOneInput() function (which is called by the fuzzer)
	 * to pthread_exit().  So just doing the SIGSEGV here in all cases is
	 * simpler than trying to differentiate between hung inputs and
	 * an impatient user.
	 */
	pthread_kill(g_fuzz_td, SIGSEGV);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "vfio_fuzz";
	opts.shutdown_cb = fuzz_shutdown;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "D:F:N:t:Z:", NULL, vfio_fuzz_parse,
				      vfio_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (!g_corpus_dir) {
		fprintf(stderr, "Must specify corpus dir with -D option\n");
		return -1;
	}

	if (!g_ctrlr_path) {
		fprintf(stderr, "Must specify ctrlr path with -F option\n");
		return -1;
	}

	if (!g_fuzzer) {
		fprintf(stderr, "Must specify fuzzer with -Z option\n");
		return -1;
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	spdk_app_fini();
	return rc;
}
