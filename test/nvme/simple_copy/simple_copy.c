/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Samsung Electronics Co., Ltd.
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
 *     * Neither the name of Samsung Electronics Co., Ltd. nor the names of its
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
#include "spdk/nvme.h"
#include "spdk/env.h"

#define NUM_LBAS 64
#define DEST_LBA 256

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
	struct spdk_nvme_qpair	*qpair;
};

struct simple_copy_context {
	struct ns_entry	*ns_entry;
	char		**write_bufs;
	char		**read_bufs;
	int		writes_completed;
	int		reads_completed;
	int		simple_copy_completed;
	int		matches_written_data;
	int		error;
};

static struct ns_entry *g_namespaces = NULL;

static void cleanup(struct simple_copy_context *context);

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
write_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct simple_copy_context	*context = arg;

	context->writes_completed++;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("write cpl error. SC 0x%x SCT 0x%x\n", cpl->status.sc, cpl->status.sct);
		context->error++;
		return;
	}
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct simple_copy_context	*context = arg;
	struct ns_entry			*ns_entry = context->ns_entry;
	int				rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("read cpl error. SC 0x%x SCT 0x%x\n", cpl->status.sc, cpl->status.sct);
		context->reads_completed++;
		context->error++;
		return;
	}

	rc = memcmp(context->write_bufs[context->reads_completed],
		    context->read_bufs[context->reads_completed], spdk_nvme_ns_get_sector_size(ns_entry->ns));
	if (rc == 0) {
		context->matches_written_data++;
	}

	context->reads_completed++;
}

static void
simple_copy_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct simple_copy_context	*context = arg;

	context->simple_copy_completed = 1;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("scc cpl error. SC 0x%x SCT 0x%x\n", cpl->status.sc, cpl->status.sct);
		context->error++;
		return;
	}

	printf("Copied LBAs from 0 - %d to the Destination LBA %d\n", NUM_LBAS - 1, DEST_LBA);
	context->reads_completed = 0;
	context->matches_written_data = 0;
}

static void
simple_copy_test(void)
{
	struct ns_entry				*ns_entry;
	struct spdk_nvme_ctrlr			*ctrlr;
	const struct spdk_nvme_ctrlr_data	*data;
	struct simple_copy_context		context;
	struct spdk_nvme_scc_source_range	range;
	uint32_t				max_block_size;
	int					rc, i;

	memset(&context, 0, sizeof(struct simple_copy_context));
	max_block_size = get_max_block_size();
	ns_entry = g_namespaces;

	context.write_bufs = calloc(NUM_LBAS, sizeof(char *));
	if (context.write_bufs == NULL) {
		printf("could not allocate write buffer pointers for test\n");
		cleanup(&context);
		return;
	}

	context.read_bufs = calloc(NUM_LBAS, sizeof(char *));
	if (context.read_bufs == NULL) {
		printf("could not allocate read buffer pointers for test\n");
		cleanup(&context);
		return;
	}

	for (i = 0; i < NUM_LBAS; i++) {
		context.write_bufs[i] = spdk_zmalloc(0x1000, max_block_size, NULL, SPDK_ENV_LCORE_ID_ANY,
						     SPDK_MALLOC_DMA);
		if (context.write_bufs[i] == NULL) {
			printf("could not allocate write buffer %d for test\n", i);
			cleanup(&context);
			return;
		}

		fill_random(context.write_bufs[i], 0x1000);
		context.read_bufs[i] = spdk_zmalloc(0x1000, max_block_size, NULL, SPDK_ENV_LCORE_ID_ANY,
						    SPDK_MALLOC_DMA);
		if (context.read_bufs[i] == NULL) {
			printf("could not allocate read buffer %d for test\n", i);
			cleanup(&context);
			return;
		}
	}

	while (ns_entry != NULL) {

		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			cleanup(&context);
			return;
		}

		ctrlr = spdk_nvme_ns_get_ctrlr(ns_entry->ns);
		data = spdk_nvme_ctrlr_get_data(ctrlr);

		printf("\nController %-20.20s (%-20.20s)\n", data->mn, data->sn);
		printf("Controller PCI vendor:%u PCI subsystem vendor:%u\n", data->vid, data->ssvid);
		printf("Namespace Block Size:%u\n", spdk_nvme_ns_get_sector_size(ns_entry->ns));
		printf("Writing LBAs 0 to %d with Random Data\n", NUM_LBAS - 1);

		context.ns_entry = ns_entry;

		for (i = 0; i < NUM_LBAS; i++) {
			rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, context.write_bufs[i],
						    i,
						    1,
						    write_complete, &context, 0);
			if (rc) {
				printf("submission of write I/O failed\n");
			}
		}
		while (context.writes_completed < NUM_LBAS) {
			rc = spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			if (rc < 0) {
				printf("Error processing write completions, rc: %d\n", rc);
				break;
			}
		}

		if (context.error) {
			printf("Error : %d Write completions failed\n",
			       context.error);
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			cleanup(&context);
			exit(1);
		}

		range.nlb = NUM_LBAS - 1;
		range.slba = 0;

		rc = spdk_nvme_ns_cmd_copy(ns_entry->ns, ns_entry->qpair,
					   &range, 1, DEST_LBA, simple_copy_complete, &context);

		if (rc) {
			printf("submission of copy I/O failed\n");
		}

		while (!context.simple_copy_completed) {
			rc = spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
			if (rc < 0) {
				printf("Error processing copy completions, rc: %d\n", rc);
				break;
			}
		}

		if (context.error) {
			printf("Error : Copy completion failed\n");
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			cleanup(&context);
			exit(1);
		}

		for (i = 0; i < NUM_LBAS; i++) {
			rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, context.read_bufs[i],
						   DEST_LBA + i, /* LBA start */
						   1, /* number of LBAs */
						   read_complete, &context, 0);
			if (rc) {
				printf("submission of read I/O failed\n");
			}
			/* block after each read command so that we can match the block to the write buffer. */
			while (context.reads_completed <= i) {
				rc = spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
				if (rc < 0) {
					printf("Error processing read completions, rc: %d\n", rc);
					break;
				}
			}
		}

		if (context.error) {
			printf("Error : %d Read completions failed\n",
			       context.error);
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			cleanup(&context);
			exit(1);
		}

		printf("LBAs matching Written Data: %d\n", context.matches_written_data);

		if (context.matches_written_data != NUM_LBAS) {
			printf("Error : %d LBAs are copied correctly out of %d LBAs\n",
			       context.matches_written_data, NUM_LBAS);
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			cleanup(&context);
			exit(1);
		}

		/* reset counters in between each namespace. */
		context.matches_written_data = 0;
		context.writes_completed = 0;
		context.reads_completed = 0;
		context.simple_copy_completed = 0;

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
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (cdata->oncs.copy) {
		printf("Controller supports SCC. Attached to %s\n", trid->traddr);
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
	} else {
		printf("Controller doesn't support SCC. Not Attached to %s\n", trid->traddr);
	}
}

static void
cleanup(struct simple_copy_context *context)
{
	struct ns_entry	*ns_entry = g_namespaces;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;
	int		i;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;

		spdk_nvme_detach_async(ns_entry->ctrlr, &detach_ctx);

		free(ns_entry);
		ns_entry = next;
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	for (i = 0; i < NUM_LBAS; i++) {
		if (context->write_bufs && context->write_bufs[i]) {
			spdk_free(context->write_bufs[i]);
		} else {
			break;
		}
		if (context->read_bufs && context->read_bufs[i]) {
			spdk_free(context->read_bufs[i]);
		} else {
			break;
		}
	}

	free(context->write_bufs);
	free(context->read_bufs);
}

int main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	spdk_env_opts_init(&opts);
	opts.name = "simple_copy";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_namespaces == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}

	printf("Initialization complete.\n");
	simple_copy_test();
	return 0;
}
