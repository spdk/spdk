/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk_internal/nvme_util.h"
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
static bool g_extended;

static void get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr);

static void
print_discovery_entry(uint64_t idx, struct spdk_nvmf_discovery_log_page_entry *entry)
{
	char str[512];

	printf("Discovery Log Entry %" PRIu64 "\n", idx);
	printf("----------------------\n");
	printf("Transport Type:                        %u (%s)\n",
	       entry->trtype, spdk_nvme_transport_id_trtype_str(entry->trtype));
	printf("Address Family:                        %u (%s)\n",
	       entry->adrfam, spdk_nvme_transport_id_adrfam_str(entry->adrfam));
	printf("Subsystem Type:                        %u (%s)\n",
	       entry->subtype,
	       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY ? "Referral to a discovery service" :
	       entry->subtype == SPDK_NVMF_SUBTYPE_NVME ? "NVM Subsystem" :
	       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ? "Current Discovery Subsystem" :
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

static const char *
exattype_str(uint16_t exattype)
{
	switch (exattype) {
	case SPDK_NVMF_EXTAT_HOST_ID:
		return "Host Identifier";
	case SPDK_NVMF_EXTAT_ADMIN_LABEL:
		return "Admin Label";
	case SPDK_NVMF_EXTAT_ADMIN_LABEL_UTF8:
		return "Admin Label (UTF-8)";
	default:
		return "Unknown";
	}
}

static int
print_extended_attributes(struct spdk_nvmf_discovery_log_page_entry_extended *ext_hdr,
			  uint8_t *entry_base, uint8_t *entry_end)
{
	uint8_t *attr_ptr;
	size_t remaining;
	uint16_t exattype, exatlen;
	uint16_t numexat;
	uint16_t i;
	int rc = 0;

	numexat = from_le16(&ext_hdr->numexat);
	if (numexat == 0) {
		return 0;
	}

	printf("Extended Attributes:                   %" PRIu16 "\n", numexat);

	attr_ptr = entry_base + SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE;

	for (i = 0; i < numexat; i++) {
		remaining = entry_end - attr_ptr;
		if (remaining < SPDK_NVMF_EXATTYPE_SIZE + SPDK_NVMF_EXATLEN_SIZE) {
			fprintf(stderr, "Error: extended attribute %" PRIu16
				" header exceeds entry boundary\n", i);
			return -EINVAL;
		}

		exattype = from_le16(attr_ptr);
		exatlen = from_le16(attr_ptr + SPDK_NVMF_EXATTYPE_SIZE);
		attr_ptr += SPDK_NVMF_EXATTYPE_SIZE + SPDK_NVMF_EXATLEN_SIZE;
		remaining = entry_end - attr_ptr;
		if (exatlen > remaining) {
			fprintf(stderr, "Error: extended attribute %" PRIu16 " length %" PRIu16
				" exceeds remaining entry size %zu\n", i, exatlen, remaining);
			exatlen = (uint16_t)remaining;
			rc = -EINVAL;
		}

		printf("  Attribute[%u] Type:                   0x%04x (%s)\n",
		       i, exattype, exattype_str(exattype));
		printf("  Attribute[%u] Length:                 %" PRIu16 "\n", i, exatlen);

		if (exattype == SPDK_NVMF_EXTAT_ADMIN_LABEL ||
		    exattype == SPDK_NVMF_EXTAT_ADMIN_LABEL_UTF8) {
			printf("  Attribute[%u] Value:                  \"%.*s\"\n",
			       i, exatlen, attr_ptr);
		}

		attr_ptr += exatlen;
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
print_extended_discovery_log(struct spdk_nvmf_discovery_log_page *log_page)
{
	uint64_t numrec = from_le64(&log_page->numrec);
	uint32_t tdlpl = from_le32(&log_page->tdlpl);
	struct spdk_nvmf_discovery_log_page_entry *entry;
	struct spdk_nvmf_discovery_log_page_entry_extended *ext_hdr;
	uint8_t *ptr, *end, *entry_end;
	size_t remaining;
	uint32_t tel;
	uint64_t i;
	int rc;

	printf("Extended Discovery Log Page\n");
	printf("===========================\n");
	printf("Generation Counter:                    %" PRIu64 "\n",
	       from_le64(&log_page->genctr));
	printf("Number of Records:                     %" PRIu64 "\n", numrec);
	printf("Record Format:                         %" PRIu16 "\n",
	       from_le16(&log_page->recfmt));
	printf("DLPF.extend:                           %u\n", log_page->dlpf.extend);
	printf("Total Discovery Log Page Length:       %" PRIu32 " bytes\n", tdlpl);
	printf("\n");

	if (!log_page->dlpf.extend) {
		fprintf(stderr, "Warning: dlpf.extend not set; controller may not support "
			"extended discovery.\nFalling back to standard entry parsing.\n\n");
		for (i = 0; i < numrec; i++) {
			print_discovery_entry(i, &log_page->entries[i]);
		}
		return 0;
	}

	ptr = (uint8_t *)log_page + SPDK_NVMF_DISC_LOG_PAGE_HEADER_SIZE;
	end = (uint8_t *)log_page + tdlpl;

	for (i = 0; i < numrec; i++) {
		remaining = end - ptr;
		if (remaining < SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE) {
			fprintf(stderr, "Error: extended discovery entry %" PRIu64 " is truncated\n", i);
			return -EINVAL;
		}

		entry = (struct spdk_nvmf_discovery_log_page_entry *)ptr;
		ext_hdr = (struct spdk_nvmf_discovery_log_page_entry_extended *)
			  (ptr + SPDK_NVMF_DISC_ENTRY_SIZE);
		tel = from_le32(&ext_hdr->tel);
		if (tel < SPDK_NVMF_DISC_EXT_ENTRY_BASE_SIZE || tel > remaining) {
			fprintf(stderr, "Error: invalid TEL %" PRIu32 " for entry %" PRIu64
				" (remaining page size %zu)\n", tel, i, remaining);
			return -EINVAL;
		}
		entry_end = ptr + tel;

		print_discovery_entry(i, entry);
		printf("Total Entry Length:                    %" PRIu32 " bytes\n", tel);
		rc = print_extended_attributes(ext_hdr, ptr, entry_end);
		if (rc != 0) {
			return rc;
		}
		printf("\n");

		ptr = entry_end;
	}

	return 0;
}

static void
print_discovery_log(struct spdk_nvmf_discovery_log_page *log_page)
{
	uint64_t numrec;
	uint64_t i;

	printf("Discovery Log Page\n");
	printf("==================\n");

	numrec = from_le64(&log_page->numrec);

	printf("Generation Counter: %" PRIu64 "\n", from_le64(&log_page->genctr));
	printf("Number of Records:  %" PRIu64 "\n", numrec);
	printf("Record Format:      %" PRIu16 "\n", from_le16(&log_page->recfmt));
	printf("\n");

	for (i = 0; i < numrec; i++) {
		print_discovery_entry(i, &log_page->entries[i]);
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

	if (log_page->dlpf.extend) {
		rc = print_extended_discovery_log(log_page);
		if (rc != 0) {
			fprintf(stderr, "Extended discovery log page parsing failed\n");
		}
	} else {
		print_discovery_log(log_page);
	}
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
	union spdk_nvmf_discovery_log_lsp lsp = {};
	int rc;

	if (g_discovery_in_progress) {
		g_pending_discovery = true;
		return;
	}

	g_discovery_in_progress = true;

	if (g_extended) {
		lsp.bits.extdlpe = 1;
	}

	rc = spdk_nvme_ctrlr_get_discovery_log_page_ext(ctrlr, lsp, get_log_page_completion, NULL);
	if (rc) {
		fprintf(stderr, "spdk_nvme_ctrlr_get_discovery_log_page_ext() failed\n");
		exit(1);
	}
}

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	spdk_nvme_transport_id_usage(stdout,
				     SPDK_NVME_TRID_USAGE_OPT_MANDATORY | SPDK_NVME_TRID_USAGE_OPT_LONGOPT |
				     SPDK_NVME_TRID_USAGE_OPT_NO_PCIE);
	printf("\t\tNote: Transport ID can be specified for NVMeoF discovery subsystem only.\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
#ifdef DEBUG
	printf("\t[-G, --enable-debug enable debug logging]\n");
#else
	printf("\t[-G, --enable-debug enable debug logging (flag disabled, must reconfigure with --enable-debug)]\n");
#endif
	printf("\t[-H, --hostnqn Host NQN]\n");
	printf("\t[-E, --extended Request Extended Discovery Log Page]\n");
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

#define AER_GETOPT_SHORT "r:GH:T:E"

static const struct option g_aer_cmdline_opts[] = {
#define AER_TRANSPORT		'r'
	{"transport",		required_argument,	NULL, AER_TRANSPORT},
#define AER_ENABLE_DEBUG	'G'
	{"enable-debug",	no_argument,		NULL, AER_ENABLE_DEBUG},
#define AER_HOSTNQN		'H'
	{"hostnqn",		required_argument,	NULL, AER_HOSTNQN},
#define AER_LOG_FLAG		'T'
	{"logflag",		required_argument,	NULL, AER_LOG_FLAG},
#define AER_EXTENDED		'E'
	{"extended",		no_argument,		NULL, AER_EXTENDED},
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
		case AER_EXTENDED:
			g_extended = true;
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

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	opts.opts_size = sizeof(opts);
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
