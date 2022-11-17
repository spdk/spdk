/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation. All rights reserved.
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
#include "spdk/util.h"

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
static struct fuzz_type				*g_fuzzer;

struct io_thread {
	int					lba_num;
	char					*write_buf;
	char					*read_buf;
	size_t					buf_size;
	struct spdk_poller			*run_poller;
	struct spdk_thread			*thread;
	struct spdk_nvme_ctrlr			*io_ctrlr;
	pthread_t				io_td;
	struct spdk_nvme_ns			*io_ns;
	struct spdk_nvme_qpair			*io_qpair;
	char					*io_ctrlr_path;
	bool					io_processing;
	bool					terminate;
} g_io_thread;

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

static int
fuzz_vfio_user_region_rw(const uint8_t *data, size_t size, struct vfio_device *dev)
{
	uint8_t buf[4];
	uint64_t offset = 0;

	offset = ((uint64_t)data[0] << 8) + (uint64_t)data[1];
	offset = (SPDK_ALIGN_FLOOR(offset, 4)) % 4096;
	memcpy(buf, &data[2], sizeof(buf));

	/* writes to BAR0 depending on the register, therefore the return value is never checked */
	spdk_vfio_user_pci_bar_access(dev, VFIO_PCI_BAR0_REGION_INDEX, offset, sizeof(buf),
				      &buf, true);
	return spdk_vfio_user_pci_bar_access(dev, VFIO_PCI_BAR0_REGION_INDEX, offset, sizeof(buf),
					     &buf, false);
}

static struct fuzz_type g_fuzzers[] = {
	{ .fn = fuzz_vfio_user_region_rw,			.bytes_per_cmd = 6},
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
io_terminate(void *ctx)
{
	((struct io_thread *)ctx)->terminate = true;
}

static void
exit_handler(void)
{
	if (g_io_thread.io_ctrlr_path) {
		spdk_thread_send_msg(g_io_thread.thread, io_terminate, &g_io_thread);

	} else {
		spdk_app_stop(0);
	}

	pthread_join(g_reactor_td, NULL);
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

	atexit(exit_handler);

	if (g_repro_data) {
		printf("Running single test based on reproduction data file.\n");
		TestOneInput(g_repro_data, g_repro_size);
		printf("Done.\n");
	} else {
		LLVMFuzzerRunDriver(&argc, &argv, TestOneInput);
		/* TODO: in the normal case, LLVMFuzzerRunDriver never returns - it calls exit()
		 * directly and we never get here.  But this behavior isn't really documented
		 * anywhere by LLVM.
		 */
	}

	return NULL;
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	int sectors_num = 0;
	struct io_thread *io = (struct io_thread *)arg;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(io->io_qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O read error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		io->io_processing = false;
		pthread_kill(g_fuzz_td, SIGSEGV);
		return;
	}

	if (memcmp(io->read_buf, io->write_buf, io->buf_size)) {
		fprintf(stderr, "I/O corrupt, value not the same\n");
		pthread_kill(g_fuzz_td, SIGSEGV);
	}

	sectors_num =  spdk_nvme_ns_get_num_sectors(io->io_ns);
	io->lba_num = (io->lba_num + 1) % sectors_num;

	io->io_processing = false;
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	int rc = 0;
	struct io_thread *io = (struct io_thread *)arg;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(io->io_qpair,
						 (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O write error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		io->io_processing = false;
		pthread_kill(g_fuzz_td, SIGSEGV);
		return;
	}
	rc = spdk_nvme_ns_cmd_read(io->io_ns, io->io_qpair,
				   io->read_buf, io->lba_num, 1,
				   read_complete, io, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		io->io_processing = false;
		pthread_kill(g_fuzz_td, SIGSEGV);
	}
}

static int
io_poller(void *ctx)
{
	int ret = 0;
	struct io_thread *io = (struct io_thread *)ctx;
	size_t i;
	unsigned int seed = 0;
	int *write_buf = (int *)io->write_buf;

	if (io->io_processing) {
		spdk_nvme_qpair_process_completions(io->io_qpair, 0);
		return SPDK_POLLER_BUSY;
	}

	if (io->terminate) {
		/* detaching controller here cause deadlock */
		spdk_poller_unregister(&(io->run_poller));
		spdk_free(io->write_buf);
		spdk_free(io->read_buf);
		spdk_thread_exit(spdk_get_thread());

		spdk_app_stop(0);

		return SPDK_POLLER_IDLE;
	}

	/* Compiler should optimize the "/ sizeof(int)" into a right shift. */
	for (i = 0; i < io->buf_size / sizeof(int); i++) {
		write_buf[i] = rand_r(&seed);
	}

	io->io_processing = true;

	ret = spdk_nvme_ns_cmd_write(io->io_ns, io->io_qpair,
				     io->write_buf, io->lba_num, 1,
				     write_complete, io, 0);
	if (ret < 0) {
		fprintf(stderr, "starting write I/O failed\n");
		pthread_kill(g_fuzz_td, SIGSEGV);
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_IDLE;
}

static void
start_io_poller(void *ctx)
{
	struct io_thread *io = (struct io_thread *)ctx;

	io->run_poller = SPDK_POLLER_REGISTER(io_poller, ctx, 0);
	if (io->run_poller == NULL) {
		fprintf(stderr, "Failed to register a poller for IO.\n");
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
	}
}

static void *
init_io(void *ctx)
{
	struct spdk_nvme_transport_id trid = {};
	int nsid = 0;

	snprintf(trid.traddr, sizeof(trid.traddr), "%s", g_io_thread.io_ctrlr_path);

	trid.trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	g_io_thread.io_ctrlr = spdk_nvme_connect(&trid, NULL, 0);
	if (g_io_thread.io_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed for transport address '%s'\n",
			trid.traddr);
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
		return NULL;
	}

	g_io_thread.io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_io_thread.io_ctrlr, NULL, 0);
	if (g_io_thread.io_qpair == NULL) {
		spdk_nvme_detach(g_io_thread.io_ctrlr);
		fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair failed\n");
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
		return NULL;
	}

	if (spdk_nvme_ctrlr_get_num_ns(g_io_thread.io_ctrlr) == 0) {
		fprintf(stderr, "no namespaces for IO\n");
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
		return NULL;
	}

	nsid = spdk_nvme_ctrlr_get_first_active_ns(g_io_thread.io_ctrlr);
	g_io_thread.io_ns = spdk_nvme_ctrlr_get_ns(g_io_thread.io_ctrlr, nsid);
	if (!g_io_thread.io_ns) {
		fprintf(stderr, "no io_ns for IO\n");
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
		return NULL;
	}

	g_io_thread.buf_size = spdk_nvme_ns_get_sector_size(g_io_thread.io_ns);

	g_io_thread.read_buf = spdk_zmalloc(g_io_thread.buf_size, 0x1000, NULL,
					    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	g_io_thread.write_buf = spdk_zmalloc(g_io_thread.buf_size, 0x1000, NULL,
					     SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	if (!g_io_thread.write_buf || !g_io_thread.read_buf) {
		fprintf(stderr, "cannot allocated memory for io buffers\n");
		spdk_app_stop(-1);
		pthread_kill(g_fuzz_td, SIGSEGV);
		return NULL;
	}

	g_io_thread.thread = spdk_thread_create("io_thread", NULL);
	spdk_thread_send_msg(g_io_thread.thread, start_io_poller, &g_io_thread);

	return NULL;
}

static void
begin_fuzz(void *ctx)
{
	g_reactor_td = pthread_self();

	pthread_create(&g_fuzz_td, NULL, start_fuzzer, NULL);

	/* posix thread is use to avoid deadlock during spdk_nvme_connect
	 * vfio-user version negotiation may block when waiting for response
	 */
	if (g_io_thread.io_ctrlr_path) {
		pthread_create(&g_io_thread.io_td, NULL, init_io, NULL);
	}
}

static void
vfio_fuzz_usage(void)
{
	fprintf(stderr, " -D                        Path of corpus directory.\n");
	fprintf(stderr, " -F                        Path for ctrlr that should be fuzzed.\n");
	fprintf(stderr, " -N                        Name of reproduction data file.\n");
	fprintf(stderr, " -t                        Time to run fuzz tests (in seconds). Default: 10\n");
	fprintf(stderr, " -Y                        Path of addition controller to perform io.\n");
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
	case 'Y':
		g_io_thread.io_ctrlr_path = strdup(optarg);
		if (!g_io_thread.io_ctrlr_path) {
			fprintf(stderr, "cannot strdup: %s\n", optarg);
			return -ENOMEM;
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

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "D:F:N:t:Y:Z:", NULL, vfio_fuzz_parse,
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
