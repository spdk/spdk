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

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s...\n", trid->traddr);
	if (strcmp(trid->traddr, "0000:82:00.0")) {
		printf("Failed attaching to %s, this is not the expected traddr\n", trid->traddr);
		return false;
	}

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
exercise_1(void)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	char serial[128];
	union spdk_nvme_csts_register csts;

	/* Get the identify controller data as defined by the NVMe specification. */
	cdata = spdk_nvme_ctrlr_get_data(g_controller);
	snprintf(serial, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("Serial number: %s\n", serial);

	/* Perform a full hardware reset of the NVMe controller. */
	if (spdk_nvme_ctrlr_reset(g_controller)) {
		printf("Resetting the controller did not succeed\n");
		return;
	}

	/* Get the NVMe controller CSTS (Status) register. */
	csts = spdk_nvme_ctrlr_get_regs_csts(g_controller);
	if (csts.bits.rdy != 1) {
		printf("Controller not ready after reset\n");
		return;
	}

	/* Detach NVMe controller, g_controller is no longer valid */
	spdk_nvme_detach(g_controller);

	printf("Great success !\n");
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
	opts.name = "exercise_1";
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
	exercise_1();

	return 0;
}
