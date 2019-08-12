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

#include "spdk/nvme.h"
#include "spdk/env.h"

static struct spdk_nvme_ctrlr *g_controller;
static int g_outstanding_commands;

struct io_context {
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	char *buf;
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s...\n", trid->traddr);
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	if (g_controller) {
		printf("This application handles just a single NVMe controller, ignoring %s\n", trid->traddr);
		return;
	}
	printf("Attached to %s !\n", trid->traddr);
	g_controller = ctrlr;
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_context *ctx = arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(ctx->qpair, (struct spdk_nvme_cpl *)completion);
		printf("I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		printf("Read I/O failed, aborting run\n");
		spdk_free(ctx->buf);
		return;
	}

	/* Read buffer is not filled with data from device. Display it, then free. */
	printf("%s", ctx->buf);
	spdk_free(ctx->buf);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_context *ctx = arg;
	int rc;

	g_outstanding_commands--;

	/* Free buffer used for write */
	spdk_free(ctx->buf);

	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(ctx->qpair, (struct spdk_nvme_cpl *)completion);
		printf("I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		printf("Write I/O failed, aborting run\n");
		return;
	}

	/* Allocate new buffer for read and send it to the device */
	ctx->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (ctx->buf == NULL) {
		printf("ERROR: read buffer allocation failed\n");
		return;
	}

	rc = spdk_nvme_ns_cmd_read(ctx->ns, ctx->qpair, ctx->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_complete, (void *)ctx, 0);
	if (rc != 0) {
		printf("starting read I/O failed\n");
		spdk_free(ctx->buf);
		return;
	}

	g_outstanding_commands++;
}

static void
exercise_3(void)
{
	int num_ns, nsid, rc;
	struct io_context ctx;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	char *buf;

	/* Check number of namespaces on the controller */
	num_ns = spdk_nvme_ctrlr_get_num_ns(g_controller);
	printf("Using controller with %d namespaces.\n", num_ns);

	/* Using only first namespace */
	nsid = 1;
	ns = spdk_nvme_ctrlr_get_ns(g_controller, nsid);
	if (ns == NULL) {
		printf("Namespace %d is not present on the controller.\n", nsid);
		return;
	}
	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Namespace %d is not active on the controller.\n", nsid);
		return;
	}
	ctx.ns = ns;

	/* Allocate buffer for write cmd */
	buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}
	snprintf(buf, 0x1000, "%s", "NVMe Lab\n");
	ctx.buf = buf;

	/* Allocate I/O qpair */
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_controller, NULL, 0);
	if (qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		free(ctx.buf);
		return;
	}
	ctx.qpair = qpair;

	/* Send allocated buffer to first namespace on allocated qpair */
	rc = spdk_nvme_ns_cmd_write(ctx.ns, ctx.qpair, ctx.buf,
				    0, /* LBA start */
				    1, /* number of LBAs */
				    write_complete, &ctx, 0);
	if (rc != 0) {
		printf("starting write I/O failed\n");
		free(ctx.buf);
		return;
	}
	g_outstanding_commands++;

	/* Process qpair completions */
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	/* Clean up resources */
	spdk_nvme_ctrlr_free_io_qpair(ctx.qpair);
	spdk_nvme_detach(g_controller);
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 *
	 */
	spdk_env_opts_init(&opts);
	opts.name = "exercise_3";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");
	/*
	 * Start the SPDK NVMe enumeration process.  probe_cb will be called
	 *  for each NVMe controller found, giving our application a choice on
	 *  whether to attach to each controller.  attach_cb will then be
	 *  called for each controller after the SPDK NVMe driver has completed
	 *  initializing the controller we chose to attach.
	 */
	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_controller == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}

	printf("Initialization complete.\n");
	exercise_3();

	return 0;
}
