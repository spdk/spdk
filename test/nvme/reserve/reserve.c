/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/endian.h"
#include "spdk/nvme.h"
#include "spdk_internal/nvme_util.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"

static int g_outstanding_commands;
static int g_reserve_command_result;
static bool g_feat_host_id_successful;

#define HOST_ID		0xABABABABCDCDCDCD
#define EXT_HOST_ID	((uint8_t[]){0x0f, 0x97, 0xcd, 0x74, 0x8c, 0x80, 0x41, 0x42, \
				     0x99, 0x0f, 0x65, 0xc4, 0xf0, 0x39, 0x24, 0x20})

#define CR_KEY		0xDEADBEAF5A5A5A5B

#define HELP_RETURN_CODE UINT16_MAX

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	enum spdk_nvme_transport_type		trtype;

	TAILQ_ENTRY(ctrlr_entry)		link;
	char					name[1024];
};

struct _trid_entry {
	struct spdk_nvme_trid_entry entry;
	TAILQ_ENTRY(_trid_entry) tailq;
};

#define MAX_TRID_ENTRY 256
static struct _trid_entry g_trids[MAX_TRID_ENTRY];
static TAILQ_HEAD(, _trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	spdk_nvme_transport_id_usage(stdout,
				     SPDK_NVME_TRID_USAGE_OPT_LONGOPT | SPDK_NVME_TRID_USAGE_OPT_MULTI |
				     SPDK_NVME_TRID_USAGE_OPT_HOSTNQN);
	printf("\n");
}

#define PERF_GETOPT_SHORT "hr:"

static const struct option g_cmdline_opts[] = {
#define PERF_HELP 'h'
	{"help", no_argument, NULL, PERF_HELP},
#define PERF_TRANSPORT 'r'
	{"transport", required_argument, NULL, PERF_TRANSPORT},
	/* Should be the last element */
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, long_idx, rc;
	uint32_t trid_count = 0;

	while ((op = getopt_long(argc, argv, PERF_GETOPT_SHORT, g_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case PERF_TRANSPORT:
			if (trid_count == MAX_TRID_ENTRY) {
				fprintf(stderr, "Number of Transport ID specified with -r is limited to %u\n", MAX_TRID_ENTRY);
				return 1;
			}

			rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, optarg);
			if (rc < 0) {
				usage(argv[0]);
				return 1;
			}

			TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
			++trid_count;
			break;
		case PERF_HELP:
			usage(argv[0]);
			return HELP_RETURN_CODE;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, "trtype:PCIe");
		if (rc < 0) {
			return 1;
		}

		TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
	} else {
		struct _trid_entry *trid_entry;

		env_opts->no_pci = true;
		/* check whether there is local PCIe type */
		TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
			if (trid_entry->entry.trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
				env_opts->no_pci = false;
				break;
			}
		}
	}

	return 0;
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_trid_entry *trid_entry)
{
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	spdk_nvme_build_name(entry->name, sizeof(entry->name), ctrlr, NULL);
	printf("Attached to NVMe%s Controller at %s\n",
	       trid_entry->trid.trtype != SPDK_NVME_TRANSPORT_PCIE ? "oF" : "", entry->name);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_trid_entry *trid_entry = cb_ctx;

	if (trid->trtype != trid_entry->trid.trtype &&
	    strcasecmp(trid->trstring, trid_entry->trid.trstring)) {
		return false;
	}

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		memcpy(opts->extended_host_id, EXT_HOST_ID, sizeof(opts->extended_host_id));
	}

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	register_ctrlr(ctrlr, cb_ctx);
}

static int
register_controllers(void)
{
	struct _trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->entry.trid, &trid_entry->entry, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->entry.trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);
		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

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

	if (spdk_nvme_ctrlr_get_data(ctrlr)->ctratt.bits.host_id_exhid_supported) {
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

	if (spdk_nvme_ctrlr_get_data(ctrlr)->ctratt.bits.host_id_exhid_supported) {
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

	if (spdk_nvme_ctrlr_get_data(ctrlr)->ctratt.bits.host_id_exhid_supported) {
		struct spdk_nvme_reservation_status_extended_data *ext_status;

		ext_status = (struct spdk_nvme_reservation_status_extended_data *)payload;
		for (i = 0; i < ext_status->data.regctl; i++) {
			struct spdk_nvme_registered_ctrlr_extended_data *ext_cdata;

			ext_cdata = (struct spdk_nvme_registered_ctrlr_extended_data *)(payload +
					sizeof(struct spdk_nvme_reservation_status_extended_data) +
					sizeof(struct spdk_nvme_registered_ctrlr_extended_data) * i);
			fprintf(stdout, "Controller ID                           %u\n",
				ext_cdata->cntlid);
			fprintf(stdout, "Controller Reservation Status           %u\n",
				ext_cdata->rcsts.status);
			fprintf(stdout, "Controller Reservation Key              0x%"PRIx64"\n",
				ext_cdata->rkey);
			spdk_log_dump(stdout, "Controller Host ID                     ",
				      ext_cdata->hostid, 16);
		}
	} else {
		for (i = 0; i < status->regctl; i++) {
			cdata = (struct spdk_nvme_registered_ctrlr_data *)(payload +
					sizeof(struct spdk_nvme_reservation_status_data) +
					sizeof(struct spdk_nvme_registered_ctrlr_data) * i);
			fprintf(stdout, "Controller ID                           %u\n",
				cdata->cntlid);
			fprintf(stdout, "Controller Reservation Status           %u\n",
				cdata->rcsts.status);
			fprintf(stdout, "Controller Host ID                      0x%"PRIx64"\n",
				cdata->hostid);
			fprintf(stdout, "Controller Reservation Key              0x%"PRIx64"\n",
				cdata->rkey);
		}
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
reserve_controller(struct ctrlr_entry *entry)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_qpair			*qpair;
	int ret;

	ctrlr = entry->ctrlr;
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("=====================================================\n");
	printf("NVMe Controller %s\n", entry->name);
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

	if (entry->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		ret = set_host_identifier(ctrlr);
		if (ret) {
			goto out;
		}
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

int
main(int argc, char **argv)
{
	struct ctrlr_entry *entry;
	struct spdk_env_opts opts;
	int rc;

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "reserve";
	opts.core_mask = "0x1";
	opts.shm_id = 0;

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc == HELP_RETURN_CODE ? 0 : rc;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	TAILQ_FOREACH(entry, &g_controllers, link) {
		if (reserve_controller(entry) < 0) {
			rc = -1;
			goto cleanup;
		};
	}

cleanup:
	fflush(stdout);

	unregister_controllers();
	spdk_env_fini();
	if (rc != 0) {
		fprintf(stderr, "%s: errors occurred\n", argv[0]);
	}

	return rc;
}
