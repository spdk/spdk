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

#include "spdk/endian.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/log.h"

#define MAX_DEVS 64

struct dev {
	struct spdk_pci_addr			pci_addr;
	struct spdk_nvme_ctrlr			*ctrlr;
	char					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int outstanding_commands;
static int reserve_command_result;
static bool get_host_id_successful;

#define HOST_ID		0xABABABABCDCDCDCD
#define EXT_HOST_ID	((uint8_t[]){0x0f, 0x97, 0xcd, 0x74, 0x8c, 0x80, 0x41, 0x42, \
				     0x99, 0x0f, 0x65, 0xc4, 0xf0, 0x39, 0x24, 0x20})

#define CR_KEY		0xDEADBEAF5A5A5A5B

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stdout, "Get Features - Host Identifier failed\n");
		get_host_id_successful = false;
	} else {
		get_host_id_successful = true;
	}
	outstanding_commands--;
}

static int
get_host_identifier(struct spdk_nvme_ctrlr *ctrlr)
{
	int ret;
	uint8_t host_id[16];
	uint32_t host_id_size;
	uint32_t cdw11;

	if (spdk_nvme_ctrlr_get_data(ctrlr)->ctratt.host_id_exhid_supported) {
		host_id_size = 16;
		cdw11 = 1;
		printf("Using 128-bit extended host identifier\n");
	} else {
		host_id_size = 8;
		cdw11 = 0;
		printf("Using 64-bit host identifier\n");
	}

	outstanding_commands = 0;
	ret = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_HOST_IDENTIFIER, cdw11, host_id,
					      host_id_size,
					      get_feature_completion, NULL);
	if (ret) {
		fprintf(stdout, "Get Feature: Failed\n");
		return -1;
	}

	outstanding_commands++;
	get_host_id_successful = false;

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (get_host_id_successful) {
		spdk_log_dump(stdout, "Get Feature: Host Identifier:", host_id, host_id_size);
	}

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
reservation_ns_register(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			uint32_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_register_data rr_data;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);

	rr_data.crkey = CR_KEY;
	rr_data.nrkey = CR_KEY;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_register(ns, qpair, &rr_data, true,
			SPDK_NVME_RESERVE_REGISTER_KEY,
			SPDK_NVME_RESERVE_PTPL_NO_CHANGES,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Register Failed\n");
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (reserve_command_result) {
		fprintf(stderr, "Reservation Register Failed\n");
	}

	return 0;
}

static int
reservation_ns_report(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint32_t ns_id)
{
	int ret, i;
	uint8_t *payload;
	struct spdk_nvme_reservation_status_data *status;
	struct spdk_nvme_registered_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);

	outstanding_commands = 0;
	reserve_command_result = -1;

	payload = spdk_dma_zmalloc(0x1000, 0x1000, NULL);
	if (!payload) {
		fprintf(stderr, "DMA Buffer Allocation Failed\n");
		return -1;
	}

	ret = spdk_nvme_ns_cmd_reservation_report(ns, qpair, payload, 0x1000,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Report Failed\n");
		spdk_dma_free(payload);
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (reserve_command_result) {
		fprintf(stderr, "Reservation Report Failed\n");
		spdk_dma_free(payload);
		return 0;
	}

	status = (struct spdk_nvme_reservation_status_data *)payload;
	fprintf(stdout, "Reservation Generation Counter                  %u\n", status->gen);
	fprintf(stdout, "Reservation type                                %u\n", status->rtype);
	fprintf(stdout, "Reservation Number of Registered Controllers    %u\n", status->regctl);
	fprintf(stdout, "Reservation Persist Through Power Loss State    %u\n", status->ptpls);
	for (i = 0; i < status->regctl; i++) {
		cdata = (struct spdk_nvme_registered_ctrlr_data *)(payload +
				sizeof(struct spdk_nvme_reservation_status_data) +
				sizeof(struct spdk_nvme_registered_ctrlr_data) * i);
		fprintf(stdout, "Controller ID                           %u\n", cdata->cntlid);
		fprintf(stdout, "Controller Reservation Status           %u\n", cdata->rcsts.status);
		fprintf(stdout, "Controller Host ID                      0x%"PRIx64"\n", cdata->hostid);
		fprintf(stdout, "Controller Reservation Key              0x%"PRIx64"\n", cdata->rkey);
	}

	spdk_dma_free(payload);
	return 0;
}

static int
reservation_ns_acquire(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint32_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_acquire_data cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	cdata.crkey = CR_KEY;
	cdata.prkey = 0;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_acquire(ns, qpair, &cdata,
			false,
			SPDK_NVME_RESERVE_ACQUIRE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Acquire Failed\n");
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (reserve_command_result) {
		fprintf(stderr, "Reservation Acquire Failed\n");
	}

	return 0;
}

static int
reservation_ns_release(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint32_t ns_id)
{
	int ret;
	struct spdk_nvme_reservation_key_data cdata;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	cdata.crkey = CR_KEY;

	outstanding_commands = 0;
	reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_release(ns, qpair, &cdata,
			false,
			SPDK_NVME_RESERVE_RELEASE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Release Failed\n");
		return -1;
	}

	outstanding_commands++;
	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (reserve_command_result) {
		fprintf(stderr, "Reservation Release Failed\n");
	}

	return 0;
}

static void
reserve_controller(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
		   const struct spdk_pci_addr *pci_addr)
{
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller at PCI bus %d, device %d, function %d\n",
	       pci_addr->bus, pci_addr->dev, pci_addr->func);
	printf("=====================================================\n");

	printf("Reservations:                %s\n",
	       cdata->oncs.reservations ? "Supported" : "Not Supported");

	if (!cdata->oncs.reservations) {
		return;
	}

	get_host_identifier(ctrlr);

	/* tested 1 namespace */
	reservation_ns_register(ctrlr, qpair, 1);
	reservation_ns_acquire(ctrlr, qpair, 1);
	reservation_ns_report(ctrlr, qpair, 1);
	reservation_ns_release(ctrlr, qpair, 1);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	/*
	 * Provide both 64-bit and 128-bit host identifiers.
	 *
	 * The NVMe library will choose which one to use based on whether the controller
	 * supports extended host identifiers.
	 */
	to_le64(opts->host_id, HOST_ID);
	memcpy(opts->extended_host_id, EXT_HOST_ID, sizeof(opts->extended_host_id));

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];
	spdk_pci_addr_parse(&dev->pci_addr, trid->traddr);
	dev->ctrlr = ctrlr;
}

int main(int argc, char **argv)
{
	struct dev		*iter;
	int			rc, i;
	struct spdk_env_opts	opts;

	spdk_env_opts_init(&opts);
	opts.name = "reserve";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	rc = 0;

	foreach_dev(iter) {
		struct spdk_nvme_qpair *qpair;

		qpair = spdk_nvme_ctrlr_alloc_io_qpair(iter->ctrlr, NULL, 0);
		if (!qpair) {
			fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			rc = 1;
		} else {
			reserve_controller(iter->ctrlr, qpair, &iter->pci_addr);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];
		spdk_nvme_detach(dev->ctrlr);
	}

	return rc;
}
