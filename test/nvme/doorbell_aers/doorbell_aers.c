/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/mmio.h"

#define IO_QUEUE_SIZE 32

static struct spdk_nvme_transport_id g_trid;
static struct spdk_nvme_ctrlr *g_ctrlr;
struct spdk_nvme_qpair *g_io_qpair;

uint32_t g_qpair_id;
uint32_t *g_doorbell_base;
uint32_t g_doorbell_stride_u32;

static union spdk_nvme_async_event_completion g_expected_event;
static bool g_test_done;

static struct spdk_nvme_error_information_entry g_error_entries[256];

static bool g_exit;

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-r <fmt> Transport ID for PCIe NVMe device]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (PCIe)\n");
	printf("\t  traddr      Transport address (e.g. 0000:db:00.0)\n");
	printf("\t Example: -r 'trtype:PCIe traddr:0000:db:00.0'\n");
	printf("\t");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op;

	while ((op = getopt(argc, argv, "r:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Invalid transport ID format '%s'\n", optarg);
				exit(1);
			}

			if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
				fprintf(stderr, "Invalid transport type, expected PCIe");
				exit(1);
			}

			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
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

static void
get_error_log_page_completion(void *arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "get error log page failed\n");
		exit(1);
	}

	/* TODO: do handling (print?) of error log page */
	printf("Error Informaton Log Page received.\n");
	g_test_done = true;
}

static void
get_error_log_page(void)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(g_ctrlr);

	if (spdk_nvme_ctrlr_cmd_get_log_page(g_ctrlr, SPDK_NVME_LOG_ERROR,
					     SPDK_NVME_GLOBAL_NS_TAG, g_error_entries,
					     sizeof(*g_error_entries) * (cdata->elpe + 1),
					     0,
					     get_error_log_page_completion, NULL)) {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
		exit(1);
	}
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	union spdk_nvme_async_event_completion event;

	event.raw = cpl->cdw0;

	printf("Asynchronous Event received.\n");

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "aer failed\n");
		exit(1);
	}

	if (event.bits.async_event_type != g_expected_event.bits.async_event_type) {
		fprintf(stderr, "unexpected async event type 0x%x\n", event.bits.async_event_type);
		exit(1);
	}

	if (event.bits.async_event_info != g_expected_event.bits.async_event_info) {
		fprintf(stderr, "unexpected async event info 0x%x\n", event.bits.async_event_info);
		exit(1);
	}

	if (event.bits.log_page_identifier != g_expected_event.bits.log_page_identifier) {
		fprintf(stderr, "unexpected async event log page 0x%x\n", event.bits.log_page_identifier);
		exit(1);
	}

	get_error_log_page();
}

static void
wait_for_aer_and_log_page_cpl(void)
{
	while (!g_exit && !g_test_done) {
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}
}

static void
create_ctrlr(void)
{
	g_ctrlr = spdk_nvme_connect(&g_trid, NULL, 0);
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect() failed for transport address '%s'\n", g_trid.traddr);
		exit(1);
	}
}

static void
create_io_qpair(void)
{
	struct spdk_nvme_io_qpair_opts opts;

	/* Override io_queue_size here, instead of doing it at connect time with
	 * the ctrlr_opts.  This is because stub app could be running, meaning
	 * that ctrlr opts were already set.
	 */
	spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = IO_QUEUE_SIZE;
	opts.io_queue_requests = IO_QUEUE_SIZE;

	g_io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &opts, sizeof(opts));
	if (!g_io_qpair) {
		fprintf(stderr, "failed to spdk_nvme_ctrlr_alloc_io_qpair()");
		exit(1);
	}

	g_qpair_id = spdk_nvme_qpair_get_id(g_io_qpair);
}

static void
set_doorbell_vars(void)
{
	volatile struct spdk_nvme_registers *regs = spdk_nvme_ctrlr_get_registers(g_ctrlr);

	g_doorbell_stride_u32 = 1 << regs->cap.bits.dstrd;
	g_doorbell_base = (uint32_t *)&regs->doorbell[0].sq_tdbl;
}


static void
pre_test(const char *test_name, enum spdk_nvme_async_event_info_error aec_info)
{
	printf("Executing: %s\n", test_name);

	g_test_done = false;

	g_expected_event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR;
	g_expected_event.bits.log_page_identifier = SPDK_NVME_LOG_ERROR;
	g_expected_event.bits.async_event_info = aec_info;
}

static void
post_test(const char *test_name)
{
	printf("Waiting for AER completion...\n");
	wait_for_aer_and_log_page_cpl();
	printf("%s: %s\n\n", g_test_done ? "Success" : "Failure", test_name);
}

static void
test_write_invalid_db(void)
{
	volatile uint32_t *wrong_db;

	pre_test(__func__, SPDK_NVME_ASYNC_EVENT_WRITE_INVALID_DB);

	spdk_wmb();
	/* Write to invalid register (note g_qpair_id + 1). */
	wrong_db = g_doorbell_base + (2 * (g_qpair_id + 1) + 0) * g_doorbell_stride_u32;
	spdk_mmio_write_4(wrong_db, 0);

	post_test(__func__);
}

static void
test_invalid_db_write_overflow_sq(void)
{
	volatile uint32_t *good_db;

	pre_test(__func__, SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE);

	spdk_wmb();
	good_db = g_doorbell_base + (2 * g_qpair_id + 0) * g_doorbell_stride_u32;
	/* Overflow SQ doorbell over queue size. */
	spdk_mmio_write_4(good_db, IO_QUEUE_SIZE + 1);

	post_test(__func__);
}

static void
test_invalid_db_write_overflow_cq(void)
{
	volatile uint32_t *good_db;

	pre_test(__func__, SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE);

	good_db = g_doorbell_base + (2 * g_qpair_id + 1) * g_doorbell_stride_u32;
	spdk_wmb();
	/* Overflow CQ doorbell over queue size. */
	spdk_mmio_write_4(good_db, IO_QUEUE_SIZE + 1);

	post_test(__func__);
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	opts.name = "doorbell_aers";
	opts.shm_id = 0;

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		exit(1);
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		exit(1);
	}

	setup_sig_handlers();

	create_ctrlr();
	create_io_qpair();

	set_doorbell_vars();

	spdk_nvme_ctrlr_register_aer_callback(g_ctrlr, aer_cb, NULL);

	test_write_invalid_db();
	test_invalid_db_write_overflow_sq();
	test_invalid_db_write_overflow_cq();

	spdk_nvme_detach_async(g_ctrlr, &detach_ctx);

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	spdk_env_fini();

	return 0;
}
