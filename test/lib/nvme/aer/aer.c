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

#include <stdbool.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_device				*pci_dev;
	struct spdk_nvme_ctrlr				*ctrlr;
	struct spdk_nvme_health_information_page	*health_page;
	uint32_t					orig_temp_threshold;
	char 						name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

static int aer_done = 0;


#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)


static int temperature_done = 0;
static int failed = 0;

static void set_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: set feature (temp threshold) failed\n", dev->name);
		failed = 1;
		return;
	}

	/* Admin command completions are synchronized by the NVMe driver,
	 * so we don't need to do any special locking here. */
	temperature_done++;
}

static int
set_temp_threshold(struct dev *dev, uint32_t temp)
{
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;
	cmd.cdw11 = temp;

	return spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, set_feature_completion, dev);
}

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get feature (temp threshold) failed\n", dev->name);
		failed = 1;
		return;
	}

	dev->orig_temp_threshold = cpl->cdw0;
	printf("%s: original temperature threshold: %u Kelvin (%d Celsius)\n",
	       dev->name, dev->orig_temp_threshold, dev->orig_temp_threshold - 273);

	/* Set temperature threshold to a low value so the AER will trigger. */
	set_temp_threshold(dev, 200);
}

static int
get_temp_threshold(struct dev *dev)
{
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	return spdk_nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, get_feature_completion, dev);
}

static void
print_health_page(struct dev *dev, struct spdk_nvme_health_information_page *hip)
{
	printf("%s: Current Temperature:         %u Kelvin (%d Celsius)\n",
	       dev->name, hip->temperature, hip->temperature - 273);
}

static void
get_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct dev *dev = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: get log page failed\n", dev->name);
		failed = 1;
		return;
	}

	print_health_page(dev, dev->health_page);
	aer_done++;
}

static int
get_health_log_page(struct dev *dev)
{
	return spdk_nvme_ctrlr_cmd_get_log_page(dev->ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
						SPDK_NVME_GLOBAL_NS_TAG, dev->health_page, sizeof(*dev->health_page),
						get_log_page_completion, dev);
}

static void
cleanup(void)
{
	struct dev *dev;

	foreach_dev(dev) {
		if (dev->health_page) {
			rte_free(dev->health_page);
		}
	}
}

static void aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;
	struct dev *dev = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("%s: AER failed\n", dev->name);
		failed = 1;
		return;
	}

	printf("%s: aer_cb for log page %d\n", dev->name, log_page_id);

	/* Set the temperature threshold back to the original value
	 * so the AER doesn't trigger again.
	 */
	set_temp_threshold(dev, dev->orig_temp_threshold);

	get_health_log_page(dev);
}


static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	printf("Attaching to %04x:%02x:%02x.%02x\n",
	       spdk_pci_device_get_domain(dev),
	       spdk_pci_device_get_bus(dev),
	       spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];

	dev->ctrlr = ctrlr;
	dev->pci_dev = pci_dev;

	snprintf(dev->name, sizeof(dev->name), "%04x:%02x:%02x.%02x",
		 spdk_pci_device_get_domain(pci_dev), spdk_pci_device_get_bus(pci_dev),
		 spdk_pci_device_get_dev(pci_dev), spdk_pci_device_get_func(pci_dev));

	printf("Attached to %s\n", dev->name);

	dev->health_page = rte_zmalloc("nvme health", sizeof(*dev->health_page), 4096);
	if (dev->health_page == NULL) {
		printf("Allocation error (health page)\n");
		failed = 1;
	}
}

static const char *ealargs[] = {
	"aer",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct dev			*dev;
	int				rc, i;

	printf("Asynchronous Event Request test\n");

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (failed) {
		goto done;
	}

	printf("Registering asynchronous event callbacks...\n");
	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_aer_callback(dev->ctrlr, aer_cb, dev);
	}

	printf("Setting temperature thresholds...\n");
	foreach_dev(dev) {
		/* Get the original temperature threshold and set it to a low value */
		get_temp_threshold(dev);
	}

	while (!failed && temperature_done < num_devs) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	if (failed) {
		goto done;
	}

	printf("Waiting for all controllers to trigger AER...\n");

	while (!failed && aer_done < num_devs) {
		foreach_dev(dev) {
			spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		spdk_nvme_detach(dev->ctrlr);
	}

done:
	cleanup();

	return failed;
}
