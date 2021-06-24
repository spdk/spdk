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
 *     * Neither the name of Samsung Electronics Co., Ltd.,  nor the names of its
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

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

struct nvme_io {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ns *ns;
	unsigned nsid;
	unsigned rlba;
	unsigned nlbas;
	unsigned wlba;
	uint32_t lba_size;
	unsigned done;
};

struct config {
	struct nvme_io pmr_dev;
	size_t         copy_size;
};

static struct config g_config;

/* Namespaces index from 1. Return 0 to invoke an error */
static unsigned
get_nsid(const struct spdk_nvme_transport_id *trid)
{
	if (!strcmp(trid->traddr, g_config.pmr_dev.trid.traddr)) {
		return g_config.pmr_dev.nsid;
	}
	return 0;
}

static void
check_io(void *arg, const struct spdk_nvme_cpl *completion)
{
	g_config.pmr_dev.done = 1;
}

static int
pmr_persistence(void)
{
	int rc = 0;
	void *pmr_buf, *buf;
	size_t sz;
	struct spdk_nvme_qpair	*qpair;

	/* Allocate Queue Pair for the Controller with PMR */
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_config.pmr_dev.ctrlr, NULL, 0);
	if (qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -ENOMEM;
	}

	/* Enable the PMR */
	rc = spdk_nvme_ctrlr_enable_pmr(g_config.pmr_dev.ctrlr);
	if (rc) {
		printf("ERROR: Enabling PMR failed\n");
		printf("Are you sure %s has a valid PMR?\n",
		       g_config.pmr_dev.trid.traddr);
		goto free_qpair;
	}

	/* Allocate buffer from PMR */
	pmr_buf = spdk_nvme_ctrlr_map_pmr(g_config.pmr_dev.ctrlr, &sz);
	if (pmr_buf == NULL || sz < g_config.copy_size) {
		printf("ERROR: PMR buffer allocation failed\n");
		rc = -ENOMEM;
		goto disable_pmr;
	}

	/* Clear the done flag */
	g_config.pmr_dev.done = 0;

	/* Do the write to the PMR IO buffer, reading from rlba */
	rc = spdk_nvme_ns_cmd_read(g_config.pmr_dev.ns, qpair, pmr_buf,
				   g_config.pmr_dev.rlba, g_config.pmr_dev.nlbas,
				   check_io, NULL, 0);
	if (rc != 0) {
		fprintf(stderr, "Read I/O to PMR failed\n");
		rc = -EIO;
		goto unmap_pmr;
	}
	while (!g_config.pmr_dev.done) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	/* Clear the done flag */
	g_config.pmr_dev.done = 0;

	pmr_buf = NULL;

	/* Free PMR buffer */
	rc = spdk_nvme_ctrlr_unmap_pmr(g_config.pmr_dev.ctrlr);
	if (rc) {
		printf("ERROR: Unmapping PMR failed\n");
		goto disable_pmr;
	}

	/* Disable the PMR */
	rc = spdk_nvme_ctrlr_disable_pmr(g_config.pmr_dev.ctrlr);
	if (rc) {
		printf("ERROR: Disabling PMR failed\n");
		goto free_qpair;
	}

	/* Free the queue */
	spdk_nvme_ctrlr_free_io_qpair(qpair);

	rc = spdk_nvme_ctrlr_reset(g_config.pmr_dev.ctrlr);
	if (rc) {
		printf("ERROR: Resetting Controller failed\n");
		return rc;
	}

	/* Allocate Queue Pair for the Controller with PMR */
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_config.pmr_dev.ctrlr, NULL, 0);
	if (qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -ENOMEM;
	}

	/* Enable the PMR */
	rc = spdk_nvme_ctrlr_enable_pmr(g_config.pmr_dev.ctrlr);
	if (rc) {
		printf("ERROR: Enabling PMR failed\n");
		goto free_qpair;
	}

	/* Allocate buffer from PMR */
	pmr_buf = spdk_nvme_ctrlr_map_pmr(g_config.pmr_dev.ctrlr, &sz);
	if (pmr_buf == NULL || sz < g_config.copy_size) {
		printf("ERROR: PMR buffer allocation failed\n");
		rc = -ENOMEM;
		goto disable_pmr;
	}

	/* Do the read from the PMR IO buffer, write to wlba */
	rc = spdk_nvme_ns_cmd_write(g_config.pmr_dev.ns, qpair, pmr_buf,
				    g_config.pmr_dev.wlba, g_config.pmr_dev.nlbas,
				    check_io, NULL, 0);
	if (rc != 0) {
		fprintf(stderr, "Read I/O from PMR failed\n");
		rc = -EIO;
		goto unmap_pmr;
	}
	while (!g_config.pmr_dev.done) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	/* Clear the done flag */
	g_config.pmr_dev.done = 0;

	buf = spdk_zmalloc(g_config.copy_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		printf("ERROR: Buffer allocation failed\n");
		rc = -ENOMEM;
		goto unmap_pmr;
	}

	/* Do the read from wlba to a buffer */
	rc = spdk_nvme_ns_cmd_read(g_config.pmr_dev.ns, qpair, buf,
				   g_config.pmr_dev.wlba, g_config.pmr_dev.nlbas,
				   check_io, NULL, 0);
	if (rc != 0) {
		fprintf(stderr, "Read I/O from WLBA failed\n");
		rc = -EIO;
		goto free_buf;
	}
	while (!g_config.pmr_dev.done) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	/* Clear the done flag */
	g_config.pmr_dev.done = 0;

	/* Compare the data in the read buffer to the PMR buffer */
	if (memcmp(buf, pmr_buf, g_config.copy_size)) {
		printf("PMR Data Not Persistent, after Controller Reset\n");
		rc = -EIO;
	} else {
		printf("PMR Data is Persistent across Controller Reset\n");
	}

free_buf:
	spdk_free(buf);

unmap_pmr:
	/* Free PMR buffer */
	spdk_nvme_ctrlr_unmap_pmr(g_config.pmr_dev.ctrlr);

disable_pmr:
	/* Disable the PMR */
	spdk_nvme_ctrlr_disable_pmr(g_config.pmr_dev.ctrlr);

free_qpair:
	/* Free the queue */
	spdk_nvme_ctrlr_free_io_qpair(qpair);

	return rc;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	/* We will only attach to the Controller specified by the user */
	if (spdk_nvme_transport_id_compare(trid, &g_config.pmr_dev.trid)) {
		printf("%s - not probed %s!\n", __func__, trid->traddr);
		return 0;
	}

	printf("%s - probed %s!\n", __func__, trid->traddr);
	return 1;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, get_nsid(trid));
	if (ns == NULL) {
		fprintf(stderr, "Could not locate namespace %d on controller %s.\n",
			get_nsid(trid), trid->traddr);
		exit(-1);
	}

	g_config.pmr_dev.ctrlr    = ctrlr;
	g_config.pmr_dev.ns       = ns;
	g_config.pmr_dev.lba_size = spdk_nvme_ns_get_sector_size(ns);

	printf("%s - attached %s!\n", __func__, trid->traddr);
}

static void
usage(char *program_name)
{
	printf("%s options (all mandatory)", program_name);
	printf("\n");
	printf("\t[-p PCIe address of the NVMe Device with PMR support]\n");
	printf("\t[-n Namespace ID]\n");
	printf("\t[-r Read LBA]\n");
	printf("\t[-l Number of LBAs to read]\n");
	printf("\t[-w Write LBA]\n");
	printf("\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;
	unsigned num_args = 0;
	long int val;

	while ((op = getopt(argc, argv, "p:n:r:l:w:")) != -1) {
		switch (op) {
		case 'p':
			snprintf(&g_config.pmr_dev.trid.traddr[0], SPDK_NVMF_TRADDR_MAX_LEN + 1,
				 "%s", optarg);

			g_config.pmr_dev.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

			spdk_nvme_transport_id_populate_trstring(&g_config.pmr_dev.trid,
					spdk_nvme_transport_id_trtype_str(g_config.pmr_dev.trid.trtype));

			num_args++;
			break;
		case 'n':
		case 'r':
		case 'l':
		case 'w':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case 'n':
				g_config.pmr_dev.nsid = (unsigned)val;
				num_args++;
				break;
			case 'r':
				g_config.pmr_dev.rlba = (unsigned)val;
				num_args++;
				break;
			case 'l':
				g_config.pmr_dev.nlbas = (unsigned)val;
				num_args++;
				break;
			case 'w':
				g_config.pmr_dev.wlba = (unsigned)val;
				num_args++;
				break;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (num_args != 5) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}

static void
cleanup(void)
{
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_nvme_detach_async(g_config.pmr_dev.ctrlr, &detach_ctx);

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

int main(int argc, char **argv)
{
	int rc = 0;
	struct spdk_env_opts opts;

	/*
	 * Parse the input arguments. For now we use the following
	 * format list:
	 *
	 * -p <pci id> -n <namespace> -r <Read LBA> -l <number of LBAs> -w <Write LBA>
	 *
	 */
	rc = parse_args(argc, argv);
	if (rc) {
		fprintf(stderr, "Error in parse_args(): %d\n", rc);
		return rc;
	}

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 *
	 */
	spdk_env_opts_init(&opts);
	opts.name = "pmr_persistence";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	/*
	 * PMRs only apply to PCIe attached NVMe controllers so we
	 * only probe the PCIe bus. This is the default when we pass
	 * in NULL for the first argument.
	 */

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc) {
		fprintf(stderr, "Error in spdk_nvme_probe(): %d\n", rc);
		cleanup();
		return rc;
	}

	g_config.copy_size = g_config.pmr_dev.nlbas * g_config.pmr_dev.lba_size;

	/*
	 * Call the pmr_persistence() function which performs the data copy
	 * to PMR region, resets the Controller and verifies the data persistence
	 * or returns an error code if it fails.
	 */
	rc = pmr_persistence();
	if (rc) {
		fprintf(stderr, "Error in pmr_persistence(): %d\n", rc);
	}

	cleanup();

	return rc;
}
