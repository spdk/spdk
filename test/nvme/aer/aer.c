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

#define MAX_DEVS 64

struct dev {
	struct spdk_nvme_ctrlr				*ctrlr;
	struct spdk_nvme_health_information_page	*health_page;
	struct spdk_nvme_ns_list			*changed_ns_list;
	uint32_t					orig_temp_threshold;
	char						name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static void get_feature_test(struct dev *dev);

static struct dev g_devs[MAX_DEVS];
static int g_num_devs = 0;

#define foreach_dev(iter) \
	for (iter = g_devs; iter - g_devs < g_num_devs; iter++)

static int g_outstanding_commands = 0;
static int g_aer_done = 0;
static int g_temperature_done = 0;
static int g_failed = 0;
static struct spdk_nvme_transport_id g_trid;
static char *g_touch_file;

/* Enable AER temperature test */
static int g_enable_temp_test = 0;
/* Enable AER namespace attribute notice test, this variable holds
 * the NSID that is expected to be in the Changed NS List.
 */
static uint32_t g_expected_ns_test = 0;

static void
set_temp_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: set feature (temp threshold) failed\n", dev->name);
		g_failed = 1;
		return;
	}

	/* Admin command completions are synchronized by the NVMe driver,
	 * so we don't need to do any special locking here. */
	g_temperature_done++;
}

static int
set_temp_threshold(struct dev *dev, uint32_t temp)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;
	cmd.cdw11_bits.feat_temp_threshold.bits.tmpth = temp;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, set_temp_completion, dev);
	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static void
get_temp_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get feature (temp threshold) failed\n", dev->name);
		g_failed = 1;
		return;
	}

	dev->orig_temp_threshold = cpl->cdw0;
	printf("%s: original temperature threshold: %u Kelvin (%d Celsius)\n",
	       dev->name, dev->orig_temp_threshold, dev->orig_temp_threshold - 273);

	g_temperature_done++;
}

static int
get_temp_threshold(struct dev *dev)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, get_temp_completion, dev);
	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static void
print_health_page(struct dev *dev, struct spdk_nvme_health_information_page *hip)
{
	printf("%s: Current Temperature:         %u Kelvin (%d Celsius)\n",
	       dev->name, hip->temperature, hip->temperature - 273);
}

static void
get_health_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands --;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get log page failed\n", dev->name);
		g_failed = 1;
		return;
	}

	print_health_page(dev, dev->health_page);
	g_aer_done++;
}

static void
get_changed_ns_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;
	bool found = false;
	uint32_t i;

	g_outstanding_commands --;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get log page failed\n", dev->name);
		g_failed = 1;
		return;
	}

	/* Let's compare the expected namespce ID is
	 * in changed namespace list
	 */
	if (dev->changed_ns_list->ns_list[0] != 0xffffffffu) {
		for (i = 0; i < sizeof(*dev->changed_ns_list) / sizeof(uint32_t); i++) {
			if (g_expected_ns_test == dev->changed_ns_list->ns_list[i]) {
				printf("%s: changed NS list contains expected NSID: %u\n",
				       dev->name, g_expected_ns_test);
				found = true;
				break;
			}
		}
	}

	if (!found) {
		printf("%s: Error: Can't find expected NSID %u\n", dev->name, g_expected_ns_test);
		g_failed = 1;
	}

	g_aer_done++;
}

static int
get_health_log_page(struct dev *dev)
{
	int rc;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(dev->ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					      SPDK_NVME_GLOBAL_NS_TAG, dev->health_page, sizeof(*dev->health_page), 0,
					      get_health_log_page_completion, dev);

	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static int
get_changed_ns_log_page(struct dev *dev)
{
	int rc;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(dev->ctrlr, SPDK_NVME_LOG_CHANGED_NS_LIST,
					      SPDK_NVME_GLOBAL_NS_TAG, dev->changed_ns_list,
					      sizeof(*dev->changed_ns_list), 0,
					      get_changed_ns_log_page_completion, dev);

	if (rc == 0) {
		g_outstanding_commands++;
	}

	return rc;
}

static void
cleanup(void)
{
	struct dev *dev;

	foreach_dev(dev) {
		if (dev->health_page) {
			spdk_free(dev->health_page);
		}
		if (dev->changed_ns_list) {
			spdk_free(dev->changed_ns_list);
		}
	}
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;
	struct dev *dev = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: AER failed\n", dev->name);
		g_failed = 1;
		return;
	}

	printf("%s: aer_cb for log page %d\n", dev->name, log_page_id);

	if (log_page_id == SPDK_NVME_LOG_HEALTH_INFORMATION) {
		/* Set the temperature threshold back to the original value
		 * so the AER doesn't trigger again.
		 */
		set_temp_threshold(dev, dev->orig_temp_threshold);
		get_health_log_page(dev);
	} else if (log_page_id == SPDK_NVME_LOG_CHANGED_NS_LIST) {
		get_changed_ns_log_page(dev);
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -T         enable temperature tests\n");
	printf(" -n         expected Namespace attribute notice ID\n");
	printf(" -t <file>  touch specified file when ready to receive AER\n");
	printf(" -r trid    remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");

	spdk_log_usage(stdout, "-L");

	printf(" -v         verbose (enable warnings)\n");
	printf(" -H         show this usage\n");
}

static int
parse_args(int argc, char **argv)
{
	int op, rc;
	long int val;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "n:r:t:HL:T")) != -1) {
		switch (op) {
		case 'n':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Invalid NS attribute notice ID\n");
				return val;
			}
			g_expected_ns_test = (uint32_t)val;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 't':
			g_touch_file = optarg;
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
		case 'T':
			g_enable_temp_test = 1;
			break;
		case 'H':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
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
	struct dev *dev;

	/* add to dev list */
	dev = &g_devs[g_num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);

	dev->health_page = spdk_zmalloc(sizeof(*dev->health_page), 4096, NULL, SPDK_ENV_LCORE_ID_ANY,
					SPDK_MALLOC_DMA);
	if (dev->health_page == NULL) {
		printf("Allocation error (health page)\n");
		g_failed = 1;
	}
	dev->changed_ns_list = spdk_zmalloc(sizeof(*dev->changed_ns_list), 4096, NULL,
					    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (dev->changed_ns_list == NULL) {
		printf("Allocation error (changed namespace list page)\n");
		g_failed = 1;
	}
}

static void
get_feature_test_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	g_outstanding_commands--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get number of queues failed\n", dev->name);
		g_failed = 1;
		return;
	}

	if (g_aer_done < g_num_devs) {
		/*
		 * Resubmit Get Features command to continue filling admin queue
		 * while the test is running.
		 */
		get_feature_test(dev);
	}
}

static void
get_feature_test(struct dev *dev)
{
	struct spdk_nvme_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;
	if (spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0,
					  get_feature_test_cb, dev) != 0) {
		printf("Failed to send Get Features command for dev=%p\n", dev);
		g_failed = 1;
		return;
	}

	g_outstanding_commands++;
}

static int
spdk_aer_temperature_test(void)
{
	struct dev *dev;

	printf("Getting temperature thresholds of all controllers...\n");
	foreach_dev(dev) {
		/* Get the original temperature threshold */
		get_temp_threshold(dev);
	}

	while (!g_failed && g_temperature_done < g_num_devs) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}
	g_temperature_done = 0;
	g_aer_done = 0;

	/* Send admin commands to test admin queue wraparound while waiting for the AER */
	foreach_dev(dev) {
		get_feature_test(dev);
	}

	if (g_failed) {
		return g_failed;
	}

	printf("Waiting for all controllers to trigger AER...\n");
	foreach_dev(dev) {
		/* Set the temperature threshold to a low value */
		set_temp_threshold(dev, 200);
	}

	if (g_failed) {
		return g_failed;
	}

	while (!g_failed && (g_aer_done < g_num_devs || g_temperature_done < g_num_devs)) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}

	return 0;
}

static int
spdk_aer_changed_ns_test(void)
{
	struct dev *dev;

	g_aer_done = 0;

	printf("Starting namespce attribute notice tests for all controllers...\n");

	foreach_dev(dev) {
		get_feature_test(dev);
	}

	if (g_failed) {
		return g_failed;
	}

	while (!g_failed && (g_aer_done < g_num_devs)) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (g_failed) {
		return g_failed;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct dev		*dev;
	struct spdk_env_opts	opts;
	int			rc;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "aer";
	opts.core_mask = "0x1";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Asynchronous Event Request test\n");

	if (spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_failed) {
		goto done;
	}

	printf("Registering asynchronous event callbacks...\n");
	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_aer_callback(dev->ctrlr, aer_cb, dev);
	}

	if (g_touch_file) {
		int fd;

		fd = open(g_touch_file, O_CREAT | O_EXCL | O_RDWR, S_IFREG);
		if (fd == -1) {
			fprintf(stderr, "Could not touch %s (%s).\n", g_touch_file, strerror(errno));
			g_failed = true;
			goto done;
		}
		close(fd);
	}

	/* AER temperature test */
	if (g_enable_temp_test) {
		if (spdk_aer_temperature_test()) {
			goto done;
		}
	}

	/* AER changed namespace list test */
	if (g_expected_ns_test) {
		if (spdk_aer_changed_ns_test()) {
			goto done;
		}
	}

	printf("Cleaning up...\n");

	while (g_outstanding_commands) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	/* unregister AER callback so we don't fail on aborted AERs when we close out qpairs. */
	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_aer_callback(dev->ctrlr, NULL, NULL);
	}

	foreach_dev(dev) {
		spdk_nvme_detach_async(dev->ctrlr, &detach_ctx);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

done:
	cleanup();

	return g_failed;
}
