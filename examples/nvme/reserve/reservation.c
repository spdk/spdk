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
#include <unistd.h>
#include <inttypes.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_device			*pci_dev;
	struct spdk_nvme_ctrlr 			*ctrlr;
	char 					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int outstanding_commands;
static int reserve_command_result;
static int set_feature_result;

struct feature {
	uint32_t result;
	bool valid;
};

static struct feature features[256];

#define HOST_ID		0xABABABABCDCDCDCD
#define CR_KEY		0xDEADBEAF5A5A5A5B

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stdout, "get_feature(0x%02X) failed\n", fid);
	} else {
		feature->result = cpl->cdw0;
		feature->valid = true;
	}
	outstanding_commands--;
}

static void
set_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stdout, "set_feature(0x%02X) failed\n", fid);
		set_feature_result = -1;
	} else {
		set_feature_result = 0;
	}
	outstanding_commands--;
}

static int
get_host_identifier(struct spdk_nvme_ctrlr *ctrlr)
{
	int ret;
	uint64_t *host_id;
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_HOST_IDENTIFIER;

	outstanding_commands = 0;

	host_id = rte_malloc(NULL, 8, 0);
	ret = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, host_id, 8,
					    get_feature_completion, &features[SPDK_NVME_FEAT_HOST_IDENTIFIER]);
	if (ret) {
		fprintf(stdout, "Get Feature: Failed\n");
		return -1;
	}

	outstanding_commands++;

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (features[SPDK_NVME_FEAT_HOST_IDENTIFIER].valid) {
		fprintf(stdout, "Get Feature: Host Identifier 0x%"PRIx64"\n", *host_id);
	}

	return 0;
}

static int
set_host_identifier(struct spdk_nvme_ctrlr *ctrlr)
{
	int ret;
	uint64_t *host_id;
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_HOST_IDENTIFIER;

	host_id = rte_malloc(NULL, 8, 0);
	*host_id = HOST_ID;

	outstanding_commands = 0;
	set_feature_result = -1;

	fprintf(stdout, "Set Feature: Host Identifier 0x%"PRIx64"\n", *host_id);
	ret = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, host_id, 8,
					    set_feature_completion, &features[SPDK_NVME_FEAT_HOST_IDENTIFIER]);
	if (ret) {
		fprintf(stdout, "Set Feature: Failed\n");
		rte_free(host_id);
		return -1;
	}

	outstanding_commands++;

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (set_feature_result)
		fprintf(stdout, "Set Feature: Host Identifier Failed\n");

	rte_free(host_id);
	return 0;
}

static void
reservation_ns_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		reserve_command_result = -1;
	} else {
		reserve_command_result = 0;
	}

	outstanding_commands--;
}

static int
reservation_ns_register(struct spdk_nvme_ctrlr *ctrlr, uint16_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_register_data *rr_data;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);

	rr_data = rte_zmalloc(NULL, sizeof(struct spdk_nvme_reservation_register_data), 0);
	rr_data->crkey = CR_KEY;
	rr_data->nrkey = CR_KEY;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_register(ns, rr_data, 1,
			SPDK_NVME_RESERVE_REGISTER_KEY,
			SPDK_NVME_RESERVE_PTPL_NO_CHANGES,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Register Failed\n");
		rte_free(rr_data);
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_io_completions(ctrlr, 100);
	}

	if (reserve_command_result)
		fprintf(stderr, "Reservation Register Failed\n");

	rte_free(rr_data);
	return 0;
}

static int
reservation_ns_report(struct spdk_nvme_ctrlr *ctrlr, uint16_t ns_id)
{
	int ret, i;
	uint8_t *payload;
	struct spdk_nvme_reservation_status_data *status;
	struct spdk_nvme_reservation_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	payload = rte_zmalloc(NULL, 0x1000, 0x1000);

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_report(ns, payload, 0x1000,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Report Failed\n");
		rte_free(payload);
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_io_completions(ctrlr, 100);
	}

	if (reserve_command_result) {
		fprintf(stderr, "Reservation Report Failed\n");
		rte_free(payload);
		return 0;
	}

	status = (struct spdk_nvme_reservation_status_data *)payload;
	fprintf(stdout, "Reservation Generation Counter                  %u\n", status->generation);
	fprintf(stdout, "Reservation type                                %u\n", status->type);
	fprintf(stdout, "Reservation Number of Registered Controllers    %u\n", status->nr_regctl);
	fprintf(stdout, "Reservation Persist Through Power Loss State    %u\n", status->ptpl_state);
	for (i = 0; i < status->nr_regctl; i++) {
		cdata = (struct spdk_nvme_reservation_ctrlr_data *)(payload + sizeof(struct
				spdk_nvme_reservation_status_data) * (i + 1));
		fprintf(stdout, "Controller ID                           %u\n", cdata->ctrlr_id);
		fprintf(stdout, "Controller Reservation Status           %u\n", cdata->rcsts.status);
		fprintf(stdout, "Controller Host ID                      0x%"PRIx64"\n", cdata->host_id);
		fprintf(stdout, "Controller Reservation Key              0x%"PRIx64"\n", cdata->key);
	}

	rte_free(payload);
	return 0;
}

static int
reservation_ns_acquire(struct spdk_nvme_ctrlr *ctrlr, uint16_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_acquire_data *cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	cdata = rte_zmalloc(NULL, sizeof(struct spdk_nvme_reservation_acquire_data), 0);
	cdata->crkey = CR_KEY;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_acquire(ns, cdata,
			0,
			SPDK_NVME_RESERVE_ACQUIRE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Acquire Failed\n");
		rte_free(cdata);
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_io_completions(ctrlr, 100);
	}

	if (reserve_command_result)
		fprintf(stderr, "Reservation Acquire Failed\n");

	rte_free(cdata);
	return 0;
}

static int
reservation_ns_release(struct spdk_nvme_ctrlr *ctrlr, uint16_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_key_data *cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	cdata = rte_zmalloc(NULL, sizeof(struct spdk_nvme_reservation_key_data), 0);
	cdata->crkey = CR_KEY;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_release(ns, cdata,
			0,
			SPDK_NVME_RESERVE_RELEASE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Release Failed\n");
		rte_free(cdata);
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_io_completions(ctrlr, 100);
	}

	if (reserve_command_result)
		fprintf(stderr, "Reservation Release Failed\n");

	rte_free(cdata);
	return 0;
}

static void
reserve_controller(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_device *pci_dev)
{
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller at PCI bus %d, device %d, function %d\n",
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev));
	printf("=====================================================\n");

	printf("Reservations:                %s\n",
	       cdata->oncs.reservations ? "Supported" : "Not Supported");

	if (!cdata->oncs.reservations)
		return;

	set_host_identifier(ctrlr);
	get_host_identifier(ctrlr);

	/* tested 1 namespace */
	reservation_ns_register(ctrlr, 1);
	reservation_ns_acquire(ctrlr, 1);
	reservation_ns_report(ctrlr, 1);
	reservation_ns_release(ctrlr, 1);
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

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];
	dev->pci_dev = pci_dev;
	dev->ctrlr = ctrlr;
}

static const char *ealargs[] = {
	"reserve",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct dev			*iter;
	int				rc, i;

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

	if (num_devs) {
		rc = spdk_nvme_register_io_thread();
		if (rc != 0)
			return rc;
	}

	foreach_dev(iter) {
		reserve_controller(iter->ctrlr, iter->pci_dev);
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];
		spdk_nvme_detach(dev->ctrlr);
	}

	if (num_devs)
		spdk_nvme_unregister_io_thread();

	return rc;
}
