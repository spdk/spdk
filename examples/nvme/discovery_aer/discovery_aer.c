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

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/endian.h"

/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_transport_id g_trid;
static const char *g_hostnqn;
static bool g_discovery_in_progress;
static bool g_pending_discovery;

static void get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr);

static void
print_discovery_log(struct spdk_nvmf_discovery_log_page *log_page)
{
	uint64_t numrec;
	char str[512];
	uint32_t i;

	printf("Discovery Log Page\n");
	printf("==================\n");

	numrec = from_le64(&log_page->numrec);

	printf("Generation Counter: %" PRIu64 "\n", from_le64(&log_page->genctr));
	printf("Number of Records:  %" PRIu64 "\n", numrec);
	printf("Record Format:      %" PRIu16 "\n", from_le16(&log_page->recfmt));
	printf("\n");

	for (i = 0; i < numrec; i++) {
		struct spdk_nvmf_discovery_log_page_entry *entry = &log_page->entries[i];

		printf("Discovery Log Entry %u\n", i);
		printf("----------------------\n");
		printf("Transport Type:                        %u (%s)\n",
		       entry->trtype, spdk_nvme_transport_id_trtype_str(entry->trtype));
		printf("Address Family:                        %u (%s)\n",
		       entry->adrfam, spdk_nvme_transport_id_adrfam_str(entry->adrfam));
		printf("Subsystem Type:                        %u (%s)\n",
		       entry->subtype,
		       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY ? "Discovery Service" :
		       entry->subtype == SPDK_NVMF_SUBTYPE_NVME ? "NVM Subsystem" :
		       "Unknown");
		printf("Port ID:                               %" PRIu16 " (0x%04" PRIx16 ")\n",
		       from_le16(&entry->portid), from_le16(&entry->portid));
		printf("Controller ID:                         %" PRIu16 " (0x%04" PRIx16 ")\n",
		       from_le16(&entry->cntlid), from_le16(&entry->cntlid));
		snprintf(str, sizeof(entry->trsvcid) + 1, "%s", entry->trsvcid);
		printf("Transport Service Identifier:          %s\n", str);
		snprintf(str, sizeof(entry->subnqn) + 1, "%s", entry->subnqn);
		printf("NVM Subsystem Qualified Name:          %s\n", str);
		snprintf(str, sizeof(entry->traddr) + 1, "%s", entry->traddr);
		printf("Transport Address:                     %s\n", str);
	}
}

static void
get_log_page_completion(void *cb_arg, int rc, const struct spdk_nvme_cpl *cpl,
			struct spdk_nvmf_discovery_log_page *log_page)
{
	if (rc || spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "get discovery log page failed\n");
		exit(1);
	}

	print_discovery_log(log_page);
	free(log_page);

	g_discovery_in_progress = false;
	if (g_pending_discovery) {
		get_discovery_log_page(g_ctrlr);
		g_pending_discovery = false;
	}
}

static void
get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (g_discovery_in_progress) {
		g_pending_discovery = true;
	}

	g_discovery_in_progress = true;

	if (spdk_nvme_ctrlr_get_discovery_log_page(ctrlr, get_log_page_completion, NULL)) {
		fprintf(stderr, "spdk_nvme_ctrlr_get_discovery_log_page() failed\n");
		exit(1);
	}
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-r, --transport <fmt> Transport ID for NVMeoF discovery subsystem]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (e.g. TCP, RDMA)\n");
	printf("\t  adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("\t  traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("\t  trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("\t Example: -r 'trtype:TCP adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
#ifdef DEBUG
	printf("\t[-G, --enable-debug enable debug logging]\n");
#else
	printf("\t[-G, --enable-debug enable debug logging (flag disabled, must reconfigure with --enable-debug)]\n");
#endif
	printf("\t[-H, --hostnqn Host NQN]\n");
}

static void
set_trid(const char *trid_str)
{
	struct spdk_nvme_transport_id *trid;

	trid = &g_trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		exit(1);
	}

	spdk_nvme_transport_id_populate_trstring(trid,
			spdk_nvme_transport_id_trtype_str(trid->trtype));
}

#define AER_GETOPT_SHORT "r:GH:T:"

static const struct option g_aer_cmdline_opts[] = {
#define AER_TRANSPORT		'r'
	{"transport",		required_argument,	NULL, AER_TRANSPORT},
#define AER_ENABLE_DEBUG	'G'
	{"enable-debug",	no_argument,		NULL, AER_ENABLE_DEBUG},
#define AER_HOSTNQN		'H'
	{"hostnqn",		required_argument,	NULL, AER_HOSTNQN},
#define AER_LOG_FLAG		'T'
	{"logflag",		required_argument,	NULL, AER_LOG_FLAG},
	/* Should be the last element */
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, long_idx;
	int rc;

	while ((op = getopt_long(argc, argv, AER_GETOPT_SHORT, g_aer_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case AER_TRANSPORT:
			set_trid(optarg);
			break;
		case AER_ENABLE_DEBUG:
#ifndef DEBUG
			fprintf(stderr, "%s must be configured with --enable-debug for -G flag\n",
				argv[0]);
			usage(argv[0]);
			return 1;
#else
			spdk_log_set_flag("nvme");
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			break;
#endif
		case AER_HOSTNQN:
			g_hostnqn = optarg;
			break;
		case AER_LOG_FLAG:
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
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "aer failed\n");
		exit(1);
	}

	if (log_page_id != SPDK_NVME_LOG_DISCOVERY) {
		fprintf(stderr, "unexpected log page 0x%x\n", log_page_id);
		exit(1);
	}

	get_discovery_log_page(g_ctrlr);
}

static void
sig_handler(int signo)
{
	g_exit = true;
}

static void
setup_sig_handlers(void)
{
	struct sigaction sigact = {};
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sig_handler;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGINT) failed, errno %d (%s)\n", errno, strerror(errno));
		exit(1);
	}

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed, errno %d (%s)\n", errno, strerror(errno));
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	opts.name = "discovery_aer";
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		exit(1);
	}

	if (g_trid.subnqn[0] == '\0') {
		fprintf(stderr, "Discovery subsystem transport ID not specified\n");
		usage(argv[0]);
		exit(1);
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		exit(1);
	}

	setup_sig_handlers();

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
	if (g_hostnqn) {
		snprintf(ctrlr_opts.hostnqn, sizeof(ctrlr_opts.hostnqn), "%s", g_hostnqn);
	}

	g_ctrlr = spdk_nvme_connect(&g_trid, &ctrlr_opts, sizeof(ctrlr_opts));
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed for transport address '%s'\n", g_trid.traddr);
		exit(1);
	}

	spdk_nvme_ctrlr_register_aer_callback(g_ctrlr, aer_cb, NULL);

	get_discovery_log_page(g_ctrlr);

	while (spdk_likely(!g_exit)) {
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}

	spdk_nvme_detach_async(g_ctrlr, &detach_ctx);

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	spdk_env_fini();

	return 0;
}
