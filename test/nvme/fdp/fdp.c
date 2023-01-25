/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#define FDP_LOG_PAGE_SIZE		4096
#define FDP_NR_RUHS_DESC		256
#define MAX_FDP_EVENTS			0xFF

#define SET_EVENT_TYPES	((uint8_t[]){0x0, 0x1, 0x2, 0x3, 0x80, 0x81})

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
};

static struct ns_entry *g_namespaces = NULL;
static struct spdk_nvme_transport_id g_trid;
static bool g_use_trid = false;

static int g_outstanding_commands;
static int g_fdp_command_result;
static uint32_t g_feat_result;
static uint16_t ph_for_fdp_event;
static uint8_t rgif;
static uint8_t fdpci;
static uint16_t pid_for_ruhu;
static uint32_t g_spdk_sge_size = 4096;

static union spdk_nvme_feat_fdp_cdw12 fdp_res;
static uint8_t g_fdp_cfg_log_page_buf[FDP_LOG_PAGE_SIZE];
static uint8_t g_fdp_ruhu_log_page_buf[FDP_LOG_PAGE_SIZE];
static uint8_t g_fdp_events_log_page_buf[FDP_LOG_PAGE_SIZE];

static struct spdk_nvme_fdp_stats_log_page g_fdp_stats_log_page;
static struct spdk_nvme_fdp_cfg_log_page *g_fdp_cfg_log_page = (void *)g_fdp_cfg_log_page_buf;
static struct spdk_nvme_fdp_ruhu_log_page *g_fdp_ruhu_log_page = (void *)g_fdp_ruhu_log_page_buf;
static struct spdk_nvme_fdp_events_log_page *g_fdp_events_log_page = (void *)
		g_fdp_events_log_page_buf;

struct io_request {
	void *contig;
	uint32_t sgl_offset;
	uint32_t buf_size;
};

static void
nvme_req_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	struct io_request *req = (struct io_request *)cb_arg;

	req->sgl_offset = sgl_offset;
}

static int
nvme_req_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	uint32_t iov_len;

	*address = req->contig;

	if (req->sgl_offset) {
		*address += req->sgl_offset;
	}

	iov_len = req->buf_size - req->sgl_offset;
	if (iov_len > g_spdk_sge_size) {
		iov_len = g_spdk_sge_size;
	}

	req->sgl_offset += iov_len;
	*length = iov_len;

	return 0;
}

static void
get_feat_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_fdp_command_result = -1;
	} else {
		g_fdp_command_result = 0;
		g_feat_result = cpl->cdw0;
	}

	g_outstanding_commands--;
}

static void
cmd_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_fdp_command_result = -1;
	} else {
		g_fdp_command_result = 0;
	}

	g_outstanding_commands--;
}

static void
print_uint128_hex(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		printf("0x%llX%016llX", hi, lo);
	} else {
		printf("0x%llX", lo);
	}
}

static void
print_uint128_dec(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		/* can't handle large (>64-bit) decimal values for now, so fall back to hex */
		print_uint128_hex(v);
	} else {
		printf("%llu", (unsigned long long)lo);
	}
}

static int
set_fdp_events(struct spdk_nvme_ns *ns)
{
	int ret;
	uint8_t fdp_event_type_list[6] = {};
	uint32_t nfdp_events = 6;
	uint32_t cdw11, cdw12;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	int nsid = spdk_nvme_ns_get_id(ns);

	memcpy(fdp_event_type_list, SET_EVENT_TYPES, nfdp_events);
	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	cdw11 = (nfdp_events << 16) | ph_for_fdp_event;
	/* Enable FDP event */
	cdw12 = 1;

	ret = spdk_nvme_ctrlr_cmd_set_feature_ns(ctrlr, SPDK_NVME_FEAT_FDP_EVENTS, cdw11, cdw12,
			fdp_event_type_list, nfdp_events,
			get_feat_completion, NULL, nsid);
	if (ret) {
		fprintf(stderr, "Set Feature (fdp events) failed\n\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Set Feature (fdp events) failed\n\n");
		return -1;
	}

	fprintf(stdout, "Set Feature: Enabling FDP events on Placement handle: #%u Success\n\n",
		ph_for_fdp_event);
	return 0;
}

static int
get_fdp_events(struct spdk_nvme_ns *ns)
{
	int ret;
	uint32_t i, cdw11;
	struct spdk_nvme_fdp_event_desc events[MAX_FDP_EVENTS];
	struct spdk_nvme_fdp_event_desc *event_desc;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	int nsid = spdk_nvme_ns_get_id(ns);

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;
	g_feat_result = 0;

	cdw11 = (MAX_FDP_EVENTS << 16) | ph_for_fdp_event;

	ret = spdk_nvme_ctrlr_cmd_get_feature_ns(ctrlr, SPDK_NVME_FEAT_FDP_EVENTS, cdw11,
			events, MAX_FDP_EVENTS * sizeof(struct spdk_nvme_fdp_event_desc),
			get_feat_completion, NULL, nsid);
	if (ret) {
		fprintf(stderr, "Get Feature (fdp events) failed\n\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Get Feature (fdp events) failed\n\n");
		return -1;
	}

	fprintf(stdout, "Get Feature: FDP Events for Placement handle: #%u\n", ph_for_fdp_event);
	fprintf(stdout, "========================\n");
	fprintf(stdout, "Number of FDP Events: %u\n", g_feat_result);

	event_desc = events;
	for (i = 0; i < g_feat_result; i++) {
		fprintf(stdout, "FDP Event: #%u  Type: %s", i,
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_RU_NOT_WRITTEN_CAPACITY ?
			"RU Not Written to Capacity   " :
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_RU_TIME_LIMIT_EXCEEDED ?
			"RU Time Limit Exceeded       " :
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_CTRLR_RESET_MODIFY_RUH ?
			"Ctrlr Reset Modified RUH's   " :
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_INVALID_PLACEMENT_ID ?
			"Invalid Placement Identifier " :
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED ? "Media Reallocated            " :
			event_desc->fdp_etype == SPDK_NVME_FDP_EVENT_IMPLICIT_MODIFIED_RUH ?
			"Implicitly modified RUH      " :
			"Reserved");
		fprintf(stdout, "  Enabled: %s\n",
			event_desc->fdpeta.bits.fdp_ee ? "Yes" : "No");
		event_desc++;
	}

	fprintf(stdout, "\n");
	return 0;
}

static int
get_fdp(struct spdk_nvme_ns *ns)
{
	int ret;
	uint32_t cdw11;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;
	g_feat_result = 0;

	cdw11 = nsdata->endgid;

	ret = spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_FDP, cdw11, NULL, 0,
					      get_feat_completion, NULL);
	if (ret) {
		fprintf(stderr, "Get Feature (fdp) failed\n\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Get Feature (fdp) failed\n\n");
		return -1;
	}

	fdp_res.raw = g_feat_result;

	fprintf(stdout, "Get Feature: FDP:\n");
	fprintf(stdout, "=================\n");
	fprintf(stdout, "  Enabled:                 %s\n",
		fdp_res.bits.fdpe ? "Yes" : "No");
	fprintf(stdout, "  FDP configuration Index: %u\n\n", fdp_res.bits.fdpci);

	return 0;
}

static int
check_fdp_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	int ret;
	uint32_t sector_size, lba_count;
	uint64_t lba;
	struct io_request *req;
	struct spdk_nvme_ns_cmd_ext_io_opts ext_opts;

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	ext_opts.size = sizeof(struct spdk_nvme_ns_cmd_ext_io_opts);
	ext_opts.io_flags = SPDK_NVME_IO_FLAGS_DATA_PLACEMENT_DIRECTIVE;
	ext_opts.metadata = NULL;
	ext_opts.cdw13 = (pid_for_ruhu << 16);

	sector_size = spdk_nvme_ns_get_sector_size(ns);

	req = spdk_zmalloc(sizeof(*req), 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	assert(req);

	lba = 0;
	lba_count = 8;
	req->buf_size = sector_size * lba_count;
	req->contig = spdk_zmalloc(req->buf_size, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				   SPDK_MALLOC_DMA);
	assert(req->contig);

	ret = spdk_nvme_ns_cmd_writev_ext(ns, qpair, lba, lba_count, cmd_completion, req,
					  nvme_req_reset_sgl, nvme_req_next_sge, &ext_opts);

	if (ret) {
		fprintf(stderr, "spdk_nvme_ns_cmd_writev_ext failed\n\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "FDP write on placement id: %u failed\n\n", pid_for_ruhu);
	} else {
		fprintf(stdout, "FDP write on placement id: %u success\n\n", pid_for_ruhu);
	}

	spdk_free(req->contig);
	spdk_free(req);
	return g_fdp_command_result;
}

static int
reclaim_unit_handle_update(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	int ret;
	uint32_t npids = 1;
	uint16_t pid_list[1] = {};

	memcpy(pid_list, &pid_for_ruhu, sizeof(pid_list));
	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	ret = spdk_nvme_ns_cmd_io_mgmt_send(ns, qpair, pid_list, npids * sizeof(uint16_t),
					    SPDK_NVME_FDP_IO_MGMT_SEND_RUHU, npids - 1, cmd_completion, NULL);
	if (ret) {
		fprintf(stderr, "IO management send: RUH update failed\n\n");
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "IO management send: RUH update failed\n\n");
		return -1;
	}

	fprintf(stdout, "IO mgmt send: RUH update for Placement ID: #%u Success\n\n",
		pid_for_ruhu);
	return 0;
}

static int
reclaim_unit_handle_status(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	int ret;
	uint32_t i;
	size_t fdp_ruhs_size;
	struct spdk_nvme_fdp_ruhs *fdp_ruhs;
	struct spdk_nvme_fdp_ruhs_desc *ruhs_desc;

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	fdp_ruhs_size = sizeof(struct spdk_nvme_fdp_ruhs) +
			FDP_NR_RUHS_DESC * sizeof(struct spdk_nvme_fdp_ruhs_desc);
	fdp_ruhs = calloc(1, fdp_ruhs_size);
	if (fdp_ruhs == NULL) {
		fprintf(stderr, "FDP reclaim unit handle status allocation failed!\n\n");
		return -1;
	}

	ret = spdk_nvme_ns_cmd_io_mgmt_recv(ns, qpair, fdp_ruhs, fdp_ruhs_size,
					    SPDK_NVME_FDP_IO_MGMT_RECV_RUHS, 0, cmd_completion, NULL);
	if (ret) {
		fprintf(stderr, "IO management receive: RUH status failed\n\n");
		free(fdp_ruhs);
		return -1;
	}

	g_outstanding_commands++;
	while (g_outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 100);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "IO management receive: RUH status failed\n\n");
		free(fdp_ruhs);
		return -1;
	}

	fprintf(stdout, "FDP Reclaim unit handle status\n");
	fprintf(stdout, "==============================\n");

	fprintf(stdout, "Number of RUHS descriptors:   %u\n", fdp_ruhs->nruhsd);
	for (i = 0; i < fdp_ruhs->nruhsd; i++) {
		ruhs_desc = &fdp_ruhs->ruhs_desc[i];

		fprintf(stdout,
			"RUHS Desc: #%04u  PID: 0x%04x  RUHID: 0x%04x  ERUT: 0x%08x  RUAMW: 0x%016"PRIx64"\n",
			i, ruhs_desc->pid, ruhs_desc->ruhid, ruhs_desc->earutr, ruhs_desc->ruamw);
	}
	fprintf(stdout, "\n");

	/* Use this Placement Identifier for Reclaim unit handle Update */
	pid_for_ruhu = (&fdp_ruhs->ruhs_desc[0])->pid;

	/* Use this Placement Handle to enable FDP events */
	ph_for_fdp_event = pid_for_ruhu & ((1 << (16 - rgif)) - 1);

	free(fdp_ruhs);
	return 0;
}

static int
get_fdp_cfg_log_page(struct spdk_nvme_ns *ns)
{
	uint32_t i, j;
	struct spdk_nvme_fdp_cfg_descriptor *cfg_desc;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	void *log;

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	/* Fetch the FDP configurations log page for only 4096 bytes */
	if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_CONFIGURATIONS, 0,
			g_fdp_cfg_log_page, FDP_LOG_PAGE_SIZE, 0, 0, (nsdata->endgid << 16),
			0, cmd_completion, NULL) == 0) {
		g_outstanding_commands++;
	} else {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP config) failed\n\n");
		return -1;
	}

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Failed to get FDP configuration log page\n\n");
		return -1;
	}

	fprintf(stdout, "FDP configurations log page\n");
	fprintf(stdout, "===========================\n");

	fprintf(stdout, "Number of FDP configurations:         %u\n", g_fdp_cfg_log_page->ncfg + 1);
	fprintf(stdout, "Version:                              %u\n", g_fdp_cfg_log_page->version);
	fprintf(stdout, "Size:                                 %u\n", g_fdp_cfg_log_page->size);

	log = g_fdp_cfg_log_page->cfg_desc;
	for (i = 0; i <= g_fdp_cfg_log_page->ncfg; i++) {
		cfg_desc = log;
		fprintf(stdout, "FDP Configuration Descriptor:         %u\n", i);
		fprintf(stdout, "  Descriptor Size:                    %u\n", cfg_desc->ds);
		fprintf(stdout, "  Reclaim Group Identifier format:    %u\n",
			cfg_desc->fdpa.bits.rgif);
		fprintf(stdout, "  FDP Volatile Write Cache:           %s\n",
			cfg_desc->fdpa.bits.fdpvwc ? "Present" : "Not Present");
		fprintf(stdout, "  FDP Configuration:                  %s\n",
			cfg_desc->fdpa.bits.fdpcv ? "Valid" : "Invalid");
		fprintf(stdout, "  Vendor Specific Size:               %u\n", cfg_desc->vss);
		fprintf(stdout, "  Number of Reclaim Groups:           %u\n", cfg_desc->nrg);
		fprintf(stdout, "  Number of Recalim Unit Handles:     %u\n", cfg_desc->nruh);
		fprintf(stdout, "  Max Placement Identifiers:          %u\n", cfg_desc->maxpids + 1);
		fprintf(stdout, "  Number of Namespaces Suppprted:     %u\n", cfg_desc->nns);
		fprintf(stdout, "  Reclaim unit Nominal Size:          %" PRIx64 " bytes\n", cfg_desc->runs);
		fprintf(stdout, "  Estimated Reclaim Unit Time Limit:  ");
		if (cfg_desc->erutl) {
			fprintf(stdout, "%u seconds\n", cfg_desc->erutl);
		} else {
			fprintf(stdout, "Not Reported\n");
		}
		for (j = 0; j < cfg_desc->nruh; j++) {
			fprintf(stdout, "    RUH Desc #%03d:          RUH Type: %s\n", j,
				cfg_desc->ruh_desc[j].ruht == SPDK_NVME_FDP_RUHT_INITIALLY_ISOLATED ? "Initially Isolated" :
				cfg_desc->ruh_desc[j].ruht == SPDK_NVME_FDP_RUHT_PERSISTENTLY_ISOLATED ? "Persistently Isolated" :
				"Reserved");
		}
		if (i == fdpci) {
			rgif = cfg_desc->fdpa.bits.rgif;
		}
		log += cfg_desc->ds;
	}

	fprintf(stdout, "\n");
	return 0;
}

static int
get_fdp_ruhu_log_page(struct spdk_nvme_ns *ns)
{
	uint32_t i;
	struct spdk_nvme_fdp_ruhu_descriptor *ruhu_desc;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_RECLAIM_UNIT_HANDLE_USAGE, 0,
			g_fdp_ruhu_log_page, FDP_LOG_PAGE_SIZE, 0, 0, (nsdata->endgid << 16),
			0, cmd_completion, NULL) == 0) {
		g_outstanding_commands++;
	} else {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page_ext(RUH usage) failed\n\n");
		return -1;
	}

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Failed to get Reclaim Unit Handle usage log page\n\n");
		return -1;
	}

	fprintf(stdout, "FDP reclaim unit handle usage log page\n");
	fprintf(stdout, "======================================\n");

	fprintf(stdout, "Number of Reclaim Unit Handles:       %u\n", g_fdp_ruhu_log_page->nruh);

	for (i = 0; i < g_fdp_ruhu_log_page->nruh; i++) {
		ruhu_desc = &g_fdp_ruhu_log_page->ruhu_desc[i];

		fprintf(stdout, "  RUH Usage Desc #%03d:   RUH Attributes: %s\n", i,
			ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_UNUSED ? "Unused" :
			ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_HOST_SPECIFIED ? "Host Specified" :
			ruhu_desc->ruha == SPDK_NVME_FDP_RUHA_CTRLR_SPECIFIED ? "Controller Specified" :
			"Reserved");
	}

	fprintf(stdout, "\n");
	return 0;
}

static int
get_fdp_stats_log_page(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_STATISTICS, 0,
			&g_fdp_stats_log_page, 64, 0, 0, (nsdata->endgid << 16), 0,
			cmd_completion, NULL) == 0) {
		g_outstanding_commands++;
	} else {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP stats) failed\n\n");
		return -1;
	}

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Failed to get FDP statistics log page\n\n");
		return -1;
	}

	fprintf(stdout, "FDP statistics log page\n");
	fprintf(stdout, "=======================\n");

	fprintf(stdout, "Host bytes with metadata written:  ");
	print_uint128_dec(g_fdp_stats_log_page.hbmw);
	fprintf(stdout, "\n");
	fprintf(stdout, "Media bytes with metadata written: ");
	print_uint128_dec(g_fdp_stats_log_page.mbmw);
	fprintf(stdout, "\n");
	fprintf(stdout, "Media bytes erased:                ");
	print_uint128_dec(g_fdp_stats_log_page.mbe);
	fprintf(stdout, "\n\n");

	return 0;
}

static int
get_fdp_events_log_page(struct spdk_nvme_ns *ns)
{
	uint32_t i;
	struct spdk_nvme_fdp_event *event;
	struct spdk_nvme_fdp_event_media_reallocated *media_reallocated;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	g_outstanding_commands = 0;
	g_fdp_command_result = -1;

	/* Only fetch FDP host events here */
	if (spdk_nvme_ctrlr_cmd_get_log_page_ext(ctrlr, SPDK_NVME_LOG_FDP_EVENTS, 0,
			g_fdp_events_log_page, FDP_LOG_PAGE_SIZE, 0,
			(SPDK_NVME_FDP_REPORT_HOST_EVENTS << 8), (nsdata->endgid << 16),
			0, cmd_completion, NULL) == 0) {
		g_outstanding_commands++;
	} else {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page_ext(FDP events) failed\n\n");
		return -1;
	}

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (g_fdp_command_result) {
		fprintf(stderr, "Failed to get eventss log page\n\n");
		return -1;
	}

	fprintf(stdout, "FDP events log page\n");
	fprintf(stdout, "===================\n");
	fprintf(stdout, "Number of FDP events: %u\n", g_fdp_events_log_page->nevents);

	for (i = 0; i < g_fdp_events_log_page->nevents; i++) {
		event = &g_fdp_events_log_page->event[i];

		fprintf(stdout, "FDP Event #%u:\n", i);
		fprintf(stdout, "  Event Type:                      %s\n",
			event->etype == SPDK_NVME_FDP_EVENT_RU_NOT_WRITTEN_CAPACITY ? "RU Not Written to Capacity" :
			event->etype == SPDK_NVME_FDP_EVENT_RU_TIME_LIMIT_EXCEEDED ? "RU Time Limit Exceeded" :
			event->etype == SPDK_NVME_FDP_EVENT_CTRLR_RESET_MODIFY_RUH ? "Ctrlr Reset Modified RUH's" :
			event->etype == SPDK_NVME_FDP_EVENT_INVALID_PLACEMENT_ID ? "Invalid Placement Identifier" :
			event->etype == SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED ? "Media Reallocated" :
			event->etype == SPDK_NVME_FDP_EVENT_IMPLICIT_MODIFIED_RUH ? "Implicitly modified RUH" :
			"Reserved");
		fprintf(stdout, "  Placement Identifier:            %s\n",
			event->fdpef.bits.piv ? "Valid" : "Invalid");
		fprintf(stdout, "  NSID:                            %s\n",
			event->fdpef.bits.nsidv ? "Valid" : "Invalid");
		fprintf(stdout, "  Location:                        %s\n",
			event->fdpef.bits.lv ? "Valid" : "Invalid");
		if (event->fdpef.bits.piv) {
			fprintf(stdout, "  Placement Identifier:            %u\n", event->pid);
		} else {
			fprintf(stdout, "  Placement Identifier:            Reserved\n");
		}
		fprintf(stdout, "  Event Timestamp:                 %" PRIx64 "\n", event->timestamp);
		if (event->fdpef.bits.nsidv) {
			fprintf(stdout, "  Namespace Identifier:            %u\n", event->nsid);
		} else {
			fprintf(stdout, "  Namespace Identifier:            Ignore\n");
		}

		if (event->etype == SPDK_NVME_FDP_EVENT_MEDIA_REALLOCATED) {
			media_reallocated = (struct spdk_nvme_fdp_event_media_reallocated *)&event->event_type_specific;

			fprintf(stdout, "  LBA:                             %s\n",
				media_reallocated->sef.bits.lbav ? "Valid" : "Invalid");
			fprintf(stdout, "  Number of LBA's Moved:           %u\n", media_reallocated->nlbam);
			if (media_reallocated->sef.bits.lbav) {
				fprintf(stdout, "  Logical Block Address:           %u\n", event->nsid);
			} else {
				fprintf(stdout, "  Logical Block Address:           Ignore\n");
			}
		}

		if (event->fdpef.bits.lv) {
			fprintf(stdout, "  Reclaim Group Identifier:        %u\n", event->rgid);
		} else {
			fprintf(stdout, "  Reclaim Group Identifier:        Ignore\n");
		}
		if (event->fdpef.bits.lv) {
			fprintf(stdout, "  Reclaim Unit Handle Identifier:  %u\n", event->ruhid);
		} else {
			fprintf(stdout, "  Reclaim Unit Handle Identifier:  Ignore\n");
		}
	}

	fprintf(stdout, "\n");
	return 0;
}

static int
fdp_tests(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	int ret, err;

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!qpair) {
		fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -EIO;
	}

	ret = 0;

	fprintf(stdout, "==================================\n");
	fprintf(stdout, "== FDP tests for Namespace: #%02u ==\n", spdk_nvme_ns_get_id(ns));
	fprintf(stdout, "==================================\n\n");
	err = get_fdp(ns);
	if (err) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return err;
	}

	if (!fdp_res.bits.fdpe) {
		fprintf(stdout, "FDP support disabled\n");
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return 0;
	}

	fdpci = fdp_res.bits.fdpci;
	err = get_fdp_cfg_log_page(ns);
	if (err) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return err;
	}

	ret += get_fdp_ruhu_log_page(ns);
	ret += get_fdp_stats_log_page(ns);

	err = reclaim_unit_handle_status(ns, qpair);
	if (err) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return err;
	}

	err = check_fdp_write(ns, qpair);
	if (err) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return err;
	}

	ret += set_fdp_events(ns);
	ret += reclaim_unit_handle_update(ns, qpair);

	ret += get_fdp_events(ns);
	ret += get_fdp_events_log_page(ns);

	return ret;
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = g_namespaces;
	g_namespaces = entry;

	printf("Namespace ID: %d Endurance Group ID: %d\n", spdk_nvme_ns_get_id(ns),
	       nsdata->endgid);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	fprintf(stdout, "Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int num_ns, nsid;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (cdata->ctratt.fdps) {
		fprintf(stdout, "Controller supports FDP Attached to %s\n", trid->traddr);
		num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
		if (num_ns < 1) {
			printf("No valid namespaces in controller\n");
		} else {
			for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
			     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
				ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
				register_ns(ctrlr, ns);
			}
		}
	} else {
		fprintf(stdout, "Controller attached to: %s doesn't support FDP\n", trid->traddr);
	}
}

static void
cleanup(void)
{
	struct ns_entry	*ns_entry = g_namespaces;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;

		spdk_nvme_detach_async(ns_entry->ctrlr, &detach_ctx);

		free(ns_entry);
		ns_entry = next;
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r trid    remote NVMe over Fabrics target address\n");
	printf("    Format: 'key:value [key:value] ...'\n");
	printf("    Keys:\n");
	printf("     trtype      Transport type (e.g. RDMA)\n");
	printf("     adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("     traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("     trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("     subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("    Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");
	printf(" -h         show this usage\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}

			g_use_trid = true;
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
	int			rc;
	struct spdk_env_opts	opts;
	struct ns_entry	*ns_entry;

	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "fdp";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(g_use_trid ? &g_trid : NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_namespaces == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}

	printf("Initialization complete.\n\n");

	ns_entry = g_namespaces;
	while (ns_entry != NULL) {
		rc = fdp_tests(ns_entry->ns);
		if (rc) {
			break;
		}
		ns_entry = ns_entry->next;
	}

	printf("FDP test %s\n", rc ? "failed" : "passed");
	cleanup();

	return 0;
}
