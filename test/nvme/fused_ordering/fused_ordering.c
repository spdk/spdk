/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#define WRITE_BLOCKS 128
#define FUSED_BLOCKS 1
#define NO_WRITE_CMDS 8

struct worker_thread {
	TAILQ_ENTRY(worker_thread) link;
	unsigned lcore;
	void *cw_buf;
	void *large_buf;
	struct spdk_nvme_qpair *qpair;
	uint32_t poll_count;
	uint32_t outstanding;
	int status;
};

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static struct spdk_nvme_transport_id g_trid = {};
static uint32_t g_num_workers = 0;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);


static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct worker_thread *worker = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		spdk_nvme_print_completion(spdk_nvme_qpair_get_id(worker->qpair),
					   (struct spdk_nvme_cpl *)cpl);
		exit(1);
	}

	worker->outstanding--;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		worker->lcore = i;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;

	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);
		free(worker);
	}
}

static unsigned
init_workers(void)
{
	void *cw_buf = NULL, *large_buf = NULL;
	struct worker_thread *worker;
	int rc = 0;

	assert(g_num_workers);

	cw_buf = spdk_zmalloc(FUSED_BLOCKS * 4096, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
			      SPDK_MALLOC_DMA);
	if (cw_buf == NULL) {
		printf("ERROR: buffer allocation failed.\n");
		rc = -1;
		goto error;
	}

	large_buf = spdk_zmalloc(WRITE_BLOCKS * 4096, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (large_buf == NULL) {
		printf("ERROR: buffer allocation failed.\n");
		rc = -1;
		goto error;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		worker->qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
		if (worker->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed.\n");
			rc = -1;
			goto error;
		}
		worker->cw_buf = cw_buf;
		worker->large_buf = large_buf;
	}
	goto exit;

error:
	TAILQ_FOREACH(worker, &g_workers, link) {
		spdk_nvme_ctrlr_free_io_qpair(worker->qpair);
	}
	spdk_free(large_buf);
	spdk_free(cw_buf);
exit:
	return rc;
}

static void
fini_workers(void)
{
	void *cw_buf = NULL, *large_buf = NULL;
	struct worker_thread *worker;

	TAILQ_FOREACH(worker, &g_workers, link) {
		spdk_nvme_ctrlr_free_io_qpair(worker->qpair);
		cw_buf = worker->cw_buf;
		large_buf = worker->large_buf;
	}

	spdk_free(large_buf);
	spdk_free(cw_buf);
}

static int
fused_ordering(void *arg)
{
	struct worker_thread *worker = (struct worker_thread *)arg;
	uint32_t i;
	uint32_t rc = 0;

	/* Issue relatively large writes - big enough that the data will not fit
	 * in-capsule - followed by the compare command. Then poll the completion queue a number of
	 * times matching the poll_count variable. This adds a variable amount of delay between
	 * the compare and the subsequent fused write submission.
	 *
	 * GitHub issue #2428 showed a problem where once the non-in-capsule data had been fetched from
	 * the host, that request could get sent to the target layer between the two fused commands. This
	 * variable delay would eventually induce this condition before the fix.
	 */
	/* Submit 8 write commands per queue */
	for (i = 0; i < NO_WRITE_CMDS; i++) {
		rc = spdk_nvme_ns_cmd_write(g_ns, worker->qpair, worker->large_buf,
					    0,
					    WRITE_BLOCKS, io_complete,
					    worker,
					    0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			goto out;
		}

		worker->outstanding++;
	}

	/* Submit first fuse command, per queue */
	rc = spdk_nvme_ns_cmd_compare(g_ns, worker->qpair, worker->cw_buf,
				      0,
				      FUSED_BLOCKS, io_complete,
				      worker,
				      SPDK_NVME_IO_FLAGS_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		goto out;
	}

	worker->outstanding++;

	/* Process completions */
	while (worker->poll_count-- > 0) {
		spdk_nvme_qpair_process_completions(worker->qpair, 0);
	}

	/* Submit second fuse command, one per queue */
	rc = spdk_nvme_ns_cmd_write(g_ns, worker->qpair, worker->cw_buf, 0,
				    FUSED_BLOCKS, io_complete,
				    worker,
				    SPDK_NVME_IO_FLAGS_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		goto out;
	}

	worker->outstanding++;

	/* Process completions */
	while (worker->outstanding > 0) {
		spdk_nvme_qpair_process_completions(worker->qpair, 0);
	}

out:
	worker->status = rc;
	return rc;
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-r remote NVMe over Fabrics target address]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)]\n");
	printf("\t[-c core mask]\n");
#endif
	printf("\t[-s memory size in MB for DPDK (default: 0MB)]\n");
	printf("\t[--no-huge SPDK is run without hugepages]\n");
}

#define FUSED_GETOPT_STRING "r:L:q:c:s:"
static const struct option g_fused_cmdline_opts[] = {
#define FUSED_NO_HUGE        257
	{"no-huge", no_argument, NULL, FUSED_NO_HUGE},
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc, opt_index;
	long int value;

	while ((op = getopt_long(argc, argv, FUSED_GETOPT_STRING, g_fused_cmdline_opts,
				 &opt_index)) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'c':
			env_opts->core_mask = optarg;
			break;
		case 's':
			value = spdk_strtol(optarg, 10);
			if (value < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			env_opts->mem_size = value;
			break;
		case FUSED_NO_HUGE:
			env_opts->no_huge = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc, i;
	struct spdk_env_opts opts;
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	int nsid;
	const struct spdk_nvme_ctrlr_opts *ctrlr_opts_actual;
	uint32_t ctrlr_io_queues;
	uint32_t main_core;
	struct worker_thread *main_worker = NULL, *worker = NULL;

	spdk_env_opts_init(&opts);
	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "fused_ordering";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (register_workers() != 0) {
		rc = -1;
		goto exit;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
	ctrlr_opts.keep_alive_timeout_ms = 60 * 1000;
	g_ctrlr = spdk_nvme_connect(&g_trid, &ctrlr_opts, sizeof(ctrlr_opts));
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed\n");
		rc = 1;
		goto exit;
	}

	printf("Attached to %s\n", g_trid.subnqn);

	nsid = spdk_nvme_ctrlr_get_first_active_ns(g_ctrlr);
	if (nsid == 0) {
		perror("No active namespaces");
		exit(1);
	}
	g_ns = spdk_nvme_ctrlr_get_ns(g_ctrlr, nsid);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(g_ns),
	       spdk_nvme_ns_get_size(g_ns) / 1000000000);

	ctrlr_opts_actual = spdk_nvme_ctrlr_get_opts(g_ctrlr);
	ctrlr_io_queues = ctrlr_opts_actual->num_io_queues;

	/* One qpair per core */
	if (g_num_workers > ctrlr_io_queues) {
		printf("ERROR: Number of IO queues requested %d more then ctrlr caps %d.\n", g_num_workers,
		       ctrlr_io_queues);
		rc = -1;
		goto exit;
	}

	rc = init_workers();
	if (rc) {
		printf("ERROR: Workers initialization failed.\n");
		goto exit;
	}

	for (i = 0; i < 1024; i++) {
		printf("fused_ordering(%d)\n", i);
		main_core = spdk_env_get_current_core();
		TAILQ_FOREACH(worker, &g_workers, link) {
			worker->poll_count = i;
			if (worker->lcore != main_core) {
				spdk_env_thread_launch_pinned(worker->lcore, fused_ordering, worker);
			} else {
				main_worker = worker;
			}
		}

		if (main_worker != NULL) {
			fused_ordering(main_worker);
		}

		spdk_env_thread_wait_all();

		TAILQ_FOREACH(worker, &g_workers, link) {
			if (spdk_unlikely(worker->status != 0)) {
				SPDK_ERRLOG("Iteration of fused ordering(%d) failed.\n", i - 1);
				rc = -1;
				goto exit;
			}
		}
	}

exit:
	fini_workers();
	unregister_workers();
	spdk_nvme_detach(g_ctrlr);
	spdk_env_fini();
	return rc;
}
