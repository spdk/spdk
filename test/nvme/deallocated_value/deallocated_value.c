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

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#define NUM_BLOCKS 100

/*
 * The purpose of this sample app is to determine the read value of deallocated logical blocks
 * from a given NVMe Controller. The NVMe 1.3 spec requires the controller to list this value,
 * but controllers adhering to the NVMe 1.2 spec may not report this value. According to the spec,
 * "The values read from a deallocated logical block and its metadata (excluding protection information) shall
 * be all bytes set to 00h, all bytes set to FFh, or the last data written to the associated logical block".
 */

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
	struct spdk_nvme_qpair	*qpair;
};

struct deallocate_context {
	struct ns_entry	*ns_entry;
	char		**write_buf;
	char		**read_buf;
	char		*zero_buf;
	char		*FFh_buf;
	int		writes_completed;
	int		reads_completed;
	int		deallocate_completed;
	int		flush_complete;
	int		matches_zeroes;
	int		matches_previous_data;
	int		matches_FFh;
};

static struct ns_entry *g_namespaces = NULL;
static struct spdk_nvme_transport_id g_trid = {};

static void cleanup(struct deallocate_context *context);

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-d DPDK huge memory size in MB]\n");
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-r remote NVMe over Fabrics target address]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
	spdk_log_usage(stdout, "\t\t-L");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:gi:r:L:")) != -1) {
		switch (op) {
		case 'd':
			env_opts->mem_size = spdk_strtol(optarg, 10);
			if (env_opts->mem_size < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return env_opts->mem_size;
			}
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
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

static void
fill_random(char *buf, size_t num_bytes)
{
	size_t	i;

	srand((unsigned) time(NULL));
	for (i = 0; i < num_bytes; i++) {
		buf[i] = rand() % 0x100;
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry				*entry;
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = g_namespaces;
	g_namespaces = entry;

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static uint32_t
get_max_block_size(void)
{
	struct ns_entry	*ns;
	uint32_t	max_block_size, temp_block_size;

	ns = g_namespaces;
	max_block_size = 0;

	while (ns != NULL) {
		temp_block_size = spdk_nvme_ns_get_sector_size(ns->ns);
		max_block_size = temp_block_size > max_block_size ? temp_block_size : max_block_size;
		ns = ns->next;
	}

	return max_block_size;
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct deallocate_context	*context = arg;

	context->writes_completed++;
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct deallocate_context	*context = arg;
	struct ns_entry			*ns_entry = context->ns_entry;
	int				rc;

	rc = memcmp(context->write_buf[context->reads_completed],
		    context->read_buf[context->reads_completed], spdk_nvme_ns_get_sector_size(ns_entry->ns));
	if (rc == 0) {
		context->matches_previous_data++;
	}

	rc = memcmp(context->zero_buf, context->read_buf[context->reads_completed],
		    spdk_nvme_ns_get_sector_size(ns_entry->ns));
	if (rc == 0) {
		context->matches_zeroes++;
	}

	rc = memcmp(context->FFh_buf, context->read_buf[context->reads_completed],
		    spdk_nvme_ns_get_sector_size(ns_entry->ns));
	if (rc == 0) {
		context->matches_FFh++;
	}
	context->reads_completed++;
}

static void
deallocate_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct deallocate_context	*context = arg;

	printf("blocks matching previous data: %d\n", context->matches_previous_data);
	printf("blocks matching zeroes: %d\n", context->matches_zeroes);
	printf("blocks matching 0xFF: %d\n", context->matches_FFh);
	printf("Deallocating Blocks 0 to %d with random data.\n", NUM_BLOCKS - 1);
	printf("On next read, read value will match deallocated block read value.\n");
	context->deallocate_completed = 1;
	context->reads_completed = 0;
	context->matches_previous_data = 0;
	context->matches_zeroes = 0;
	context->matches_FFh = 0;
}

static void
flush_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct deallocate_context	*context = arg;

	context->flush_complete = 1;
}

static void
deallocate_test(void)
{
	struct ns_entry				*ns_entry;
	struct spdk_nvme_ctrlr			*ctrlr;
	const struct spdk_nvme_ctrlr_data	*data;
	struct deallocate_context		context;
	struct spdk_nvme_dsm_range		range;
	uint32_t				max_block_size;
	int					rc, i;

	memset(&context, 0, sizeof(struct deallocate_context));
	max_block_size = get_max_block_size();
	ns_entry = g_namespaces;

	if (max_block_size > 0) {
		context.zero_buf = malloc(max_block_size);
	} else {
		printf("Unable to determine max block size.\n");
		return;
	}

	if (context.zero_buf == NULL) {
		printf("could not allocate buffer for test.\n");
		return;
	}

	context.FFh_buf = malloc(max_block_size);
	if (context.FFh_buf == NULL) {
		cleanup(&context);
		printf("could not allocate buffer for test.\n");
		return;
	}

	context.write_buf = calloc(NUM_BLOCKS, sizeof(char *));
	if (context.write_buf == NULL) {
		cleanup(&context);
		return;
	}

	context.read_buf = calloc(NUM_BLOCKS, sizeof(char *));
	if (context.read_buf == NULL) {
		printf("could not allocate buffer for test.\n");
		cleanup(&context);
		return;
	}

	memset(context.zero_buf, 0x00, max_block_size);
	memset(context.FFh_buf, 0xFF, max_block_size);

	for (i = 0; i < NUM_BLOCKS; i++) {
		context.write_buf[i] = spdk_zmalloc(0x1000, max_block_size, NULL, SPDK_ENV_LCORE_ID_ANY,
						    SPDK_MALLOC_DMA);
		if (context.write_buf[i] == NULL) {
			printf("could not allocate buffer for test.\n");
			cleanup(&context);
			return;
		}

		fill_random(context.write_buf[i], 0x1000);
		context.read_buf[i] = spdk_zmalloc(0x1000, max_block_size, NULL, SPDK_ENV_LCORE_ID_ANY,
						   SPDK_MALLOC_DMA);
		if (context.read_buf[i] == NULL) {
			printf("could not allocate buffer for test.\n");
			cleanup(&context);
			return;
		}
	}

	while (ns_entry != NULL) {

		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed.\n");
			cleanup(&context);
			return;
		}

		ctrlr = spdk_nvme_ns_get_ctrlr(ns_entry->ns);
		data = spdk_nvme_ctrlr_get_data(ctrlr);

		printf("\nController %-20.20s (%-20.20s)\n", data->mn, data->sn);
		printf("Controller PCI vendor:%u PCI subsystem vendor:%u\n", data->vid, data->ssvid);
		printf("Namespace Block Size:%u\n", spdk_nvme_ns_get_sector_size(ns_entry->ns));
		printf("Writing Blocks 0 to %d with random data.\n", NUM_BLOCKS - 1);
		printf("On next read, read value will match random data.\n");

		context.ns_entry = ns_entry;

		for (i = 0; i < NUM_BLOCKS; i++) {
			rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, context.write_buf[i],
						    i,
						    1,
						    write_complete, &context, 0);
			if (rc) {
				printf("Error in nvme command completion, values may be inaccurate.\n");
			}
		}
		while (context.writes_completed < NUM_BLOCKS) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		spdk_nvme_ns_cmd_flush(ns_entry->ns, ns_entry->qpair, flush_complete, &context);
		while (!context.flush_complete) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		for (i = 0; i < NUM_BLOCKS; i++) {
			rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, context.read_buf[i],
						   i, /* LBA start */
						   1, /* number of LBAs */
						   read_complete, &context, 0);
			if (rc) {
				printf("Error in nvme command completion, values may be inaccurate.\n");
			}

			/* block after each read command so that we can match the block to the write buffer. */
			while (context.reads_completed <= i) {
				spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			}
		}

		context.flush_complete = 0;
		range.length = NUM_BLOCKS;
		range.starting_lba = 0;
		rc = spdk_nvme_ns_cmd_dataset_management(ns_entry->ns, ns_entry->qpair,
				SPDK_NVME_DSM_ATTR_DEALLOCATE, &range, 1, deallocate_complete, &context);
		if (rc) {
			printf("Error in nvme command completion, values may be inaccurate.\n");
		}

		while (!context.deallocate_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		for (i = 0; i < NUM_BLOCKS; i++) {
			rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, context.read_buf[i],
						   i, /* LBA start */
						   1, /* number of LBAs */
						   read_complete, &context, 0);
			if (rc) {
				printf("Error in nvme command completion, values may be inaccurate.\n");
			}
			while (context.reads_completed <= i) {
				spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			}
		}

		printf("blocks matching previous data: %d\n", context.matches_previous_data);
		printf("blocks matching zeroes: %d\n", context.matches_zeroes);
		printf("blocks matching FFh: %d\n", context.matches_FFh);

		/* reset counters in between each namespace. */
		context.matches_previous_data = 0;
		context.matches_zeroes = 0;
		context.matches_FFh = 0;
		context.writes_completed = 0;
		context.reads_completed = 0;
		context.deallocate_completed = 0;

		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
		ns_entry = ns_entry->next;
	}
	cleanup(&context);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int			num_ns;
	struct spdk_nvme_ns	*ns;

	printf("Attached to %s\n", trid->traddr);
	/*
	 * Use only the first namespace from each controller since we are testing controller level functionality.
	 */
	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	if (num_ns < 1) {
		printf("No valid namespaces in controller\n");
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
		register_ns(ctrlr, ns);
	}
}

static void
cleanup(struct deallocate_context *context)
{
	struct ns_entry	*ns_entry = g_namespaces;
	int		i;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;
		free(ns_entry);
		ns_entry = next;
	}
	for (i = 0; i < NUM_BLOCKS; i++) {
		if (context->write_buf && context->write_buf[i]) {
			spdk_free(context->write_buf[i]);
		} else {
			break;
		}
		if (context->read_buf && context->read_buf[i]) {
			spdk_free(context->read_buf[i]);
		} else {
			break;
		}
	}

	free(context->write_buf);
	free(context->read_buf);
	free(context->zero_buf);
	free(context->FFh_buf);
}

int main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "deallocate_test";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_namespaces == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}

	printf("Initialization complete.\n");
	deallocate_test();
	return 0;
}
