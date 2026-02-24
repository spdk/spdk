/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Alibaba Cloud and its affiliates.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/log.h"
#include "spdk_internal/nvme_util.h"
#include "spdk_internal/rdma_utils.h"

#define MAX_DEVS 64

struct dev {
	struct spdk_nvme_ctrlr				*ctrlr;
	struct spdk_nvme_ns				*ns;
	struct spdk_nvme_qpair				*qpair;
	size_t						block_size;
	void						*data;
	char						name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static bool g_use_trid = false;
static struct spdk_nvme_transport_id g_trid;

static uint32_t g_queue_depth = 128;
static uint32_t g_error_rate_num = 10;
static uint32_t g_error_rate_den = 100;

static int g_num_devs = 0;
static struct dev g_devs[MAX_DEVS];

#define foreach_dev(iter) \
	for (iter = g_devs; iter - g_devs < g_num_devs; iter++)

static uint64_t g_num_cmd_sent = 0;
static uint64_t g_num_cmd_fail = 0;
static int g_failure_ec = 0;

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
	struct dev *dev;
	uint32_t nsid;

	/* add to dev list */
	dev = &g_devs[g_num_devs++];
	if (g_num_devs >= MAX_DEVS) {
		return;
	}

	dev->ctrlr = ctrlr;
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	dev->block_size = spdk_nvme_ns_get_sector_size(dev->ns);

	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (dev->qpair == NULL) {
		g_failure_ec = -ENOMEM;
		return;
	}

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

static void
rw_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t *num_outstanding_commands = cb_arg;

	(*num_outstanding_commands)--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("Error: command failed, sct=%u, sc=%u\n", cpl->status.sct, cpl->status.sc);
		g_num_cmd_fail++;
	}
}

static void
rw_test(void)
{
	uint32_t num_outstanding_commands = 0;
	struct dev *dev;
	uint32_t block_cnt, i;
	int rc;

	rc = spdk_rdma_utils_inject_wc_error(IBV_WC_GENERAL_ERR, g_error_rate_num, g_error_rate_den);
	if (rc != 0) {
		printf("Error: failed to inject WC error, rc=%d\n", rc);
		g_failure_ec = rc;
		return;
	}

	/* Alloc memory buffer */
	foreach_dev(dev) {
		dev->data = spdk_zmalloc(0x1000 * g_queue_depth, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					 SPDK_MALLOC_DMA);
		if (!dev->data) {
			printf("Error: failed to allocate memory for dev=%p\n", dev);
			g_failure_ec = -ENOMEM;
			goto cleanup;
		}
		memset(dev->data, 0xae, 0x1000 * g_queue_depth);
	}

	/* Do write. */
	foreach_dev(dev) {
		if (dev->ns == NULL) {
			printf("Error: failed to get active namespace for dev=%p\n", dev);
			continue;
		}

		block_cnt = 0x1000 / dev->block_size;
		for (i = 0; i < g_queue_depth; i++) {
			rc = spdk_nvme_ns_cmd_write(dev->ns, dev->qpair, dev->data,
						    i * block_cnt, block_cnt, rw_test_cb, &num_outstanding_commands, 0);
			if (rc != 0) {
				printf("Error: failed to send Write command for dev=%p\n", dev);
				g_failure_ec = rc;
				goto cleanup;
			}
			num_outstanding_commands++;
			g_num_cmd_sent++;
		}
	}

	while (num_outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_qpair_process_completions(dev->qpair, 0);
		}
	}

	/* Do read. */
	foreach_dev(dev) {
		if (dev->ns == NULL) {
			printf("Error: failed to get active namespace for dev=%p\n", dev);
			continue;
		}

		block_cnt = 0x1000 / dev->block_size;
		for (i = 0; i < g_queue_depth; i++) {
			rc = spdk_nvme_ns_cmd_read(dev->ns, dev->qpair, dev->data,
						   i * block_cnt, block_cnt, rw_test_cb, &num_outstanding_commands, 0);
			if (rc != 0) {
				printf("Error: failed to send Read command for dev=%p\n", dev);
				g_failure_ec = rc;
				goto cleanup;
			}
			num_outstanding_commands++;
			g_num_cmd_sent++;
		}
	}

cleanup:
	while (num_outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_qpair_process_completions(dev->qpair, 0);
		}
	}

	foreach_dev(dev) {
		spdk_free(dev->data);
		dev->data = NULL;
	}

	spdk_rdma_utils_cancel_wc_error();
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	spdk_nvme_transport_id_usage(stdout, 0);
	printf("\t-n         specify the numerator of error rate (default: 10)");
	printf("\t-d         specify the denominator of error rate (default: 100)");
	printf("\t-h         show this usage\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op;
	char *end_ptr = NULL;
	unsigned long val = 0;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:q:n:d:h")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}

			g_use_trid = true;
			break;
		case 'q':
			val = strtoul(optarg, &end_ptr, 10);
			if ((end_ptr != NULL && *end_ptr != '\0') || val > UINT32_MAX) {
				fprintf(stderr, "Invalid queue depth %s\n", optarg);
				return 1;
			}
			g_queue_depth = (uint32_t)val;
			break;
		case 'n':
			val = strtoul(optarg, &end_ptr, 10);
			if ((end_ptr != NULL && *end_ptr != '\0') || val > UINT32_MAX) {
				fprintf(stderr, "Invalid numerator %s of error rate\n", optarg);
				return 1;
			}
			g_error_rate_num = (uint32_t)val;
			break;
		case 'd':
			val = strtoul(optarg, &end_ptr, 10);
			if ((end_ptr != NULL && *end_ptr != '\0') || val > UINT32_MAX) {
				fprintf(stderr, "Invalid denominator %s of error rate\n", optarg);
				return 1;
			}
			g_error_rate_den = (uint32_t)val;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
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
	struct dev		*dev;
	struct spdk_env_opts	opts;
	int			rc;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "rdma_err_injection";
	opts.shm_id = 0;
	rc = spdk_env_init(&opts);
	if (rc < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return rc;
	}

	printf("NVMe RDMA Error Injection test\n");

	rc = spdk_nvme_probe(g_use_trid ? &g_trid : NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return rc;
	}

	if (g_failure_ec != 0 || g_num_devs == 0) {
		printf("Failed to probe NVMe controller, %s exiting\n", argv[0]);
		return g_failure_ec != 0 ? g_failure_ec : 1;
	}

	printf("Running rw tests...\n");

	/* Run rw tests. */
	rw_test();

	printf("Test finished, %lu commands sent, %lu commands failed\n", g_num_cmd_sent, g_num_cmd_fail);

	printf("Cleaning up...\n");

	foreach_dev(dev) {
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}
	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	return g_failure_ec;
}
