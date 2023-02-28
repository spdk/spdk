/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static struct spdk_nvme_qpair *g_qpair;
static struct spdk_nvme_transport_id g_trid = {};
static uint32_t g_outstanding;

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		spdk_nvme_print_completion(spdk_nvme_qpair_get_id(g_qpair),
					   (struct spdk_nvme_cpl *)cpl);
		exit(1);
	}

	g_outstanding--;
}

#define WRITE_BLOCKS 128
#define FUSED_BLOCKS 1

static void
fused_ordering(uint32_t poll_count)
{
	void *cw_buf, *large_buf;
	int rc;
	int i;

	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (g_qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		exit(1);
	}

	cw_buf = spdk_zmalloc(FUSED_BLOCKS * 4096, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (cw_buf == NULL) {
		printf("ERROR: buffer allocation failed\n");
		return;
	}

	large_buf = spdk_zmalloc(WRITE_BLOCKS * 4096, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (large_buf == NULL) {
		printf("ERROR: buffer allocation failed\n");
		return;
	}

	/* Issue a bunch of relatively large writes - big enough that the data will not fit
	 * in-capsule - followed by the compare command.  Then poll the completion queue a number of
	 * times matching the poll_count variable.  This adds a variable amount of delay between
	 * the compare and the subsequent fused write submission.
	 *
	 * GitHub issue #2428 showed a problem where once the non-in-capsule data had been fetched from
	 * the host, that request could get sent to the target layer between the two fused commands.  This
	 * variable delay would eventually induce this condition before the fix.
	 */
	for (i = 0; i < 8; i++) {
		rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, large_buf, 0, WRITE_BLOCKS, io_complete, NULL, 0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}
		g_outstanding++;
	}

	rc = spdk_nvme_ns_cmd_compare(g_ns, g_qpair, cw_buf, 0, FUSED_BLOCKS, io_complete, NULL,
				      SPDK_NVME_IO_FLAGS_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting compare I/O failed\n");
		exit(1);
	}
	g_outstanding++;
	while (poll_count--) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}

	rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, cw_buf, 0, FUSED_BLOCKS, io_complete, NULL,
				    SPDK_NVME_IO_FLAGS_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	g_outstanding++;

	while (g_outstanding) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_free(cw_buf);
	spdk_free(large_buf);
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
#endif
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	while ((op = getopt(argc, argv, "r:L:")) != -1) {
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

	for (i = 0; i < 1024; i++) {
		printf("fused_ordering(%d)\n", i);
		fused_ordering(i);
	}

exit:
	spdk_nvme_detach(g_ctrlr);
	spdk_env_fini();
	return rc;
}
