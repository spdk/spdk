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

static struct dev g_devs[MAX_DEVS];
static int g_num_devs = 0;

#define foreach_dev(iter) \
	for (iter = g_devs; iter - g_devs < g_num_devs; iter++)

static int g_outstanding_commands;
static int g_reserve_command_result;
static bool g_feat_host_id_successful;

#define HOST_ID		0xABABABABCDCDCDCD
#define EXT_HOST_ID	((uint8_t[]){0x0f, 0x97, 0xcd, 0x74, 0x8c, 0x80, 0x41, 0x42, \
				     0x99, 0x0f, 0x65, 0xc4, 0xf0, 0x39, 0x24, 0x20})

#define CR_KEY		0xDEADBEAF5A5A5A5B

static void
feat_host_id_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stdout, "Get/Set Features - Host Identifier failed\n");
		g_feat_host_id_successful = false;
	} else {
		g_feat_host_id_successful = true;
	}
	g_outstanding_commands--;
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

	g_outstanding_commands = 0;
	ret = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_HOST_IDENTIFIER, cdw11, host_id,
					      host_id_size,
					      feat_host_id_completion, NULL);
	if (ret) {
		fprintf(stdout, "Get Feature: Failed\n");
		return -1;
	}

	g_outstanding_commands++;
	g_feat_host_id_successful = false;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_feat_host_id_successful) {
		spdk_log_dump(stdout, "Get Feature: Host Identifier:", host_id, host_id_size);
		return 0;
	}

	return -1;
}

static int
set_host_identifier(struct spdk_nvme_ctrlr *ctrlr)
{
	int ret;
	uint8_t host_id[16] = {};
	uint32_t host_id_size;
	uint32_t cdw11;

	if (spdk_nvme_ctrlr_get_data(ctrlr)->ctratt.host_id_exhid_supported) {
		host_id_size = 16;
		cdw11 = 1;
		printf("Using 128-bit extended host identifier\n");
		memcpy(host_id, EXT_HOST_ID, host_id_size);
	} else {
		host_id_size = 8;
		cdw11 = 0;
		to_be64(host_id, HOST_ID);
		printf("Using 64-bit host identifier\n");
	}

	g_outstanding_commands = 0;
	ret = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_HOST_IDENTIFIER, cdw11, 0, host_id,
					      host_id_size, feat_host_id_completion, NULL);
	if (ret) {
		fprintf(stdout, "Set Feature: Failed\n");
		return -1;
	}

	g_outstanding_commands++;
	g_feat_host_id_successful = false;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_feat_host_id_successful) {
		spdk_log_dump(stdout, "Set Feature: Host Identifier:", host_id, host_id_size);
		return 0;
	}

	fprintf(stderr, "Set Feature: Host Identifier Failed\n");
	return -1;
}

static void
reservation_ns_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_reserve_command_result = -1;
	} else {
		g_reserve_command_result = 0;
	}

	g_outstanding_commands--;
}

static int
reservation_ns_register(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			uint32_t ns_id, bool reg)
{
	int ret;
	struct spdk_nvme_reservation_register_data rr_data;
	enum spdk_nvme_reservation_register_action action;
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);

	if (reg) {
		rr_data.crkey = 0;
		rr_data.nrkey = CR_KEY;
		action = SPDK_NVME_RESERVE_REGISTER_KEY;
	} else {
		rr_data.crkey = CR_KEY;
		rr_data.nrkey = 0;
		action = SPDK_NVME_RESERVE_UNREGISTER_KEY;
	}

	g_outstanding_commands = 0;
	g_reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_register(ns, qpair, &rr_data, true,
			action,
			SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Register Failed\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_reserve_command_result) {
		fprintf(stderr, "Reservation Register Failed\n");
		return -1;
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

	g_outstanding_commands = 0;
	g_reserve_command_result = -1;

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

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_reserve_command_result) {
		fprintf(stderr, "Reservation Report Failed\n");
		spdk_dma_free(payload);
		return -1;
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

	g_outstanding_commands = 0;
	g_reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_acquire(ns, qpair, &cdata,
			false,
			SPDK_NVME_RESERVE_ACQUIRE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Acquire Failed\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_reserve_command_result) {
		fprintf(stderr, "Reservation Acquire Failed\n");
		return -1;
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

	g_outstanding_commands = 0;
	g_reserve_command_result = -1;

	ret = spdk_nvme_ns_cmd_reservation_release(ns, qpair, &cdata,
			false,
			SPDK_NVME_RESERVE_RELEASE,
			SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
			reservation_ns_completion, NULL);
	if (ret) {
		fprintf(stderr, "Reservation Release Failed\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_reserve_command_result) {
		fprintf(stderr, "Reservation Release Failed\n");
		return -1;
	}

	return 0;
}

static int
reserve_controller(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_qpair			*qpair;
	int ret;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller at PCI bus %d, device %d, function %d\n",
	       pci_addr->bus, pci_addr->dev, pci_addr->func);
	printf("=====================================================\n");

	printf("Reservations:                %s\n",
	       cdata->oncs.reservations ? "Supported" : "Not Supported");

	if (!cdata->oncs.reservations) {
		return 0;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!qpair) {
		fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -EIO;
	}

	ret = set_host_identifier(ctrlr);
	if (ret) {
		goto out;
	}

	ret = get_host_identifier(ctrlr);
	if (ret) {
		goto out;
	}

	/* tested 1 namespace */
	ret += reservation_ns_register(ctrlr, qpair, 1, 1);
	ret += reservation_ns_acquire(ctrlr, qpair, 1);
	ret += reservation_ns_release(ctrlr, qpair, 1);
	ret += reservation_ns_register(ctrlr, qpair, 1, 0);
	ret += reservation_ns_report(ctrlr, qpair, 1);

out:
	spdk_nvme_ctrlr_free_io_qpair(qpair);
	return ret;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &g_devs[g_num_devs++];
	spdk_pci_addr_parse(&dev->pci_addr, trid->traddr);
	dev->ctrlr = ctrlr;
}

int main(int argc, char **argv)
{
	struct dev		*iter;
	struct spdk_env_opts	opts;
	int			ret = 0;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

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

	foreach_dev(iter) {
		ret = reserve_controller(iter->ctrlr, &iter->pci_addr);
		if (ret) {
			break;
		}
	}

	printf("Reservation test %s\n", ret ? "failed" : "passed");

	foreach_dev(iter) {
		spdk_nvme_detach_async(iter->ctrlr, &detach_ctx);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	return ret;
}
