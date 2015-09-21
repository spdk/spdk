/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include <pciaccess.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

struct dev {
	struct pci_device			*pci_dev;
	struct nvme_controller 			*ctrlr;
	struct nvme_health_information_page	*health_page;
	uint32_t				orig_temp_threshold;
	char 					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

static int aer_done = 0;


#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)


static int temperature_done = 0;

static void set_feature_completion(void *arg, const struct nvme_completion *cpl)
{
	/* Admin command completions are synchronized by the NVMe driver,
	 * so we don't need to do any special locking here. */
	temperature_done++;
}

static int
set_temp_threshold(struct dev *dev, uint32_t temp)
{
	struct nvme_command cmd = {0};

	cmd.opc = NVME_OPC_SET_FEATURES;
	cmd.cdw10 = NVME_FEAT_TEMPERATURE_THRESHOLD;
	cmd.cdw11 = temp;

	return nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, set_feature_completion, dev);
}

static void
get_feature_completion(void *cb_arg, const struct nvme_completion *cpl)
{
	struct dev *dev = cb_arg;

	if (nvme_completion_is_error(cpl)) {
		printf("%s: get feature (temp threshold) failed\n", dev->name);
	} else {
		dev->orig_temp_threshold = cpl->cdw0;
		printf("%s: original temperature threshold: %u Kelvin (%d Celsius)\n",
		       dev->name, dev->orig_temp_threshold, dev->orig_temp_threshold - 273);
	}

	/* Set temperature threshold to a low value so the AER will trigger. */
	set_temp_threshold(dev, 200);
}

static int
get_temp_threshold(struct dev *dev)
{
	struct nvme_command cmd = {0};

	cmd.opc = NVME_OPC_GET_FEATURES;
	cmd.cdw10 = NVME_FEAT_TEMPERATURE_THRESHOLD;

	return nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, NULL, 0, get_feature_completion, dev);
}

static void
print_health_page(struct dev *dev, struct nvme_health_information_page *hip)
{
	printf("%s: Current Temperature:         %u Kelvin (%d Celsius)\n",
	       dev->name, hip->temperature, hip->temperature - 273);
}

static void
get_log_page_completion(void *cb_arg, const struct nvme_completion *cpl)
{
	struct dev *dev = cb_arg;

	if (nvme_completion_is_error(cpl)) {
		printf("%s: get log page failed\n", dev->name);
	} else {
		print_health_page(dev, dev->health_page);
	}

	aer_done++;
}

static int
get_health_log_page(struct dev *dev)
{
	struct nvme_command cmd = {0};

	cmd.opc = NVME_OPC_GET_LOG_PAGE;
	cmd.cdw10 = NVME_LOG_HEALTH_INFORMATION;
	cmd.cdw10 |= (sizeof(*(dev->health_page)) / 4) << 16; // number of dwords
	cmd.nsid = NVME_GLOBAL_NAMESPACE_TAG;

	return nvme_ctrlr_cmd_admin_raw(dev->ctrlr, &cmd, dev->health_page, sizeof(*dev->health_page),
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

static void aer_cb(void *arg, const struct nvme_completion *cpl)
{
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;
	struct dev *dev = arg;

	printf("%s: aer_cb for log page %d\n", dev->name, log_page_id);

	/* Set the temperature threshold back to the original value
	 * so the AER doesn't trigger again.
	 */
	set_temp_threshold(dev, dev->orig_temp_threshold);

	get_health_log_page(dev);
}

static const char *ealargs[] = {
	"aer",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct pci_device_iterator	*pci_dev_iter;
	struct pci_device		*pci_dev;
	struct dev			*dev;
	struct pci_id_match		match;
	int				rc, i;

	printf("Asynchronous Event Request test\n");

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	pci_system_init();

	match.vendor_id =	PCI_MATCH_ANY;
	match.subvendor_id =	PCI_MATCH_ANY;
	match.subdevice_id =	PCI_MATCH_ANY;
	match.device_id =	PCI_MATCH_ANY;
	match.device_class =	NVME_CLASS_CODE;
	match.device_class_mask = 0xFFFFFF;

	pci_dev_iter = pci_id_match_iterator_create(&match);

	rc = 0;
	while ((pci_dev = pci_device_next(pci_dev_iter))) {
		struct dev *dev;

		if (pci_device_has_kernel_driver(pci_dev) &&
		    !pci_device_has_uio_driver(pci_dev)) {
			fprintf(stderr, "non-uio kernel driver attached to nvme\n");
			fprintf(stderr, " controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			fprintf(stderr, " skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		/* add to dev list */
		dev = &devs[num_devs++];

		dev->pci_dev = pci_dev;

		snprintf(dev->name, sizeof(dev->name), "%04X:%02X:%02X.%02X",
			 pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func);

		printf("%s: attaching NVMe driver...\n", dev->name);

		dev->health_page = rte_zmalloc("nvme health", sizeof(*dev->health_page), 4096);
		if (dev->health_page == NULL) {
			printf("Allocation error (health page)\n");
			rc = 1;
			continue; /* TODO: just abort */
		}

		dev->ctrlr = nvme_attach(pci_dev);
		if (dev->ctrlr == NULL) {
			fprintf(stderr, "failed to attach to NVMe controller %s\n", dev->name);
			rc = 1;
			continue; /* TODO: just abort */
		}
	}

	printf("Registering asynchronous event callbacks...\n");
	foreach_dev(dev) {
		nvme_ctrlr_register_aer_callback(dev->ctrlr, aer_cb, dev);
	}

	printf("Setting temperature thresholds...\n");
	foreach_dev(dev) {
		/* Get the original temperature threshold and set it to a low value */
		get_temp_threshold(dev);
	}

	while (temperature_done < num_devs) {
		foreach_dev(dev) {
			nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	printf("Waiting for all controllers to trigger AER...\n");

	while (aer_done < num_devs) {
		foreach_dev(dev) {
			nvme_ctrlr_process_admin_completions(dev->ctrlr);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		nvme_detach(dev->ctrlr);
	}

	cleanup();

	pci_iterator_destroy(pci_dev_iter);
	return rc;
}
