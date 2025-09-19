/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/nvme_util.h"

#include <getopt.h>
#include <inttypes.h>

#define NUM_TEST_QPAIRS 9

enum io_mode {
	IO_MODE_READ = 0,
	IO_MODE_WRITE = 1,
};

struct app_config {
	uint32_t cmds_per_queue;
	uint32_t lba_count;
	uint64_t start_lba;
	uint32_t queue_size;
	uint32_t queue_requests;
	uint16_t hpw;
	uint16_t mpw;
	uint16_t lpw;
	uint8_t arbitration_burst;
	enum io_mode mode;
	const char *log_path;
};

struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	TAILQ_ENTRY(ctrlr_entry) link;
	char name[128];
};

struct ns_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	TAILQ_ENTRY(ns_entry) link;
};

struct qpair_ctx;

struct cmd_entry {
	struct qpair_ctx *qpair_ctx;
	void *buffer;
	uint64_t slba;
	uint32_t nlb;
	uint8_t opc;
	uint16_t cid;
	uint64_t submit_tick;
	uint64_t completion_tick;
	struct spdk_nvme_status status;
	bool success;
};

struct qpair_ctx {
	struct spdk_nvme_qpair *qpair;
	enum spdk_nvme_qprio qprio;
	uint16_t qid;
	uint32_t submitted;
	uint32_t completed;
	uint64_t base_lba;
	struct cmd_entry *entries;
	void *data_pool;
	size_t payload_size;
	uint32_t index;
};

struct completion_vector {
	struct cmd_entry **items;
	size_t count;
	size_t capacity;
};

static struct app_config g_cfg = {
	.cmds_per_queue = 255,
	.lba_count = 8,
	.start_lba = 0,
	.queue_size = 512,
	.queue_requests = 512,
	.hpw = 32,
	.mpw = 16,
	.lpw = 4,
	.arbitration_burst = 7,
	.mode = IO_MODE_READ,
	.log_path = "wrr_burst_log.csv",
};

static struct completion_vector g_completion_log = {};
static uint64_t g_total_completed;
static uint64_t g_total_errors;
static int g_log_error;

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

static const char *
qprio_to_string(enum spdk_nvme_qprio qprio)
{
	switch (qprio) {
	case SPDK_NVME_QPRIO_URGENT:
		return "urgent";
	case SPDK_NVME_QPRIO_HIGH:
		return "high";
	case SPDK_NVME_QPRIO_MEDIUM:
		return "medium";
	case SPDK_NVME_QPRIO_LOW:
		return "low";
	default:
		return "unknown";
	}
}

static uint8_t
weight_to_field(uint16_t weight)
{
	if (weight == 0) {
		return 0;
	}

	if (weight > 256) {
		weight = 256;
	}

	return (uint8_t)(weight - 1);
}

static int
completion_vector_reserve(size_t entries)
{
	struct cmd_entry **tmp;
	size_t new_cap;

	if (g_log_error != 0) {
		return g_log_error;
	}

	if (g_completion_log.count + entries <= g_completion_log.capacity) {
		return 0;
	}

	new_cap = g_completion_log.capacity ? g_completion_log.capacity : 256;
	while (new_cap < g_completion_log.count + entries) {
		new_cap *= 2;
	}

	tmp = realloc(g_completion_log.items, new_cap * sizeof(*tmp));
	if (tmp == NULL) {
		g_log_error = -ENOMEM;
		return g_log_error;
	}

	g_completion_log.items = tmp;
	g_completion_log.capacity = new_cap;

	return 0;
}

static void
completion_vector_append(struct cmd_entry *entry)
{
	if (completion_vector_reserve(1) != 0) {
		return;
	}

	g_completion_log.items[g_completion_log.count++] = entry;
}

static void
usage(const char *program)
{
	printf("Usage: %s [options]\n", program);
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help\t\tShow this message.\n");
	printf("  -r <trid>\t\tNVMe transport ID (default: local PCIe).\n");
	printf("  -d <MB>\t\tDPDK hugepage memory size.\n");
	printf("  -i <id>\t\tShared memory group ID.\n");
	printf("  -g\t\t\tUse a single file descriptor for hugepages.\n");
	printf("  -L <flag>\t\tEnable SPDK log flag.\n");
	printf("  -W, --write\t\tSend write commands instead of reads.\n");
	printf("  -C <num>\t\tCommands per qpair (default %u).\n", g_cfg.cmds_per_queue);
	printf("  -N <num>\t\tLogical blocks per command (default %u).\n", g_cfg.lba_count);
	printf("  -S <lba>\t\tStarting LBA (default %" PRIu64 ").\n", g_cfg.start_lba);
	printf("  -Q <entries>\tIO queue size per qpair (default %u).\n", g_cfg.queue_size);
	printf("  -O <path>\t\tWrite completion log to path (default %s).\n", g_cfg.log_path);
	printf("      --hpw <w>\tHigh priority weight (1-256, default %u).\n", g_cfg.hpw);
	printf("      --mpw <w>\tMedium priority weight (1-256, default %u).\n", g_cfg.mpw);
	printf("      --lpw <w>\tLow priority weight (1-256, default %u).\n", g_cfg.lpw);
	printf("      --burst <v>\tArbitration burst (0-7, default %u).\n", g_cfg.arbitration_burst);
	printf("\n");
	printf("Example:\n");
	printf("  %s -r \"trtype:PCIe\" --hpw 64 --mpw 16 --lpw 4\n", program);
}

static int
parse_positive_u32(const char *arg, uint32_t *value)
{
	char *endptr = NULL;
	uint64_t tmp;

	tmp = strtoull(arg, &endptr, 10);
	if (endptr == NULL || *endptr != '\0' || tmp > UINT32_MAX) {
		return -EINVAL;
	}

	*value = (uint32_t)tmp;
	if (*value == 0) {
		return -EINVAL;
	}

	return 0;
}

static int
parse_positive_u64(const char *arg, uint64_t *value)
{
	char *endptr = NULL;
	uint64_t tmp;

	tmp = strtoull(arg, &endptr, 10);
	if (endptr == NULL || *endptr != '\0') {
		return -EINVAL;
	}

	*value = tmp;
	return 0;
}

static int
parse_weight(const char *arg, uint16_t *weight)
{
	uint32_t parsed;
	int rc;

	rc = parse_positive_u32(arg, &parsed);
	if (rc != 0) {
		return rc;
	}

	if (parsed == 0 || parsed > 256U) {
		return -ERANGE;
	}

	*weight = (uint16_t)parsed;
	return 0;
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int opt, rc;
	uint32_t u32;
	uint64_t u64;
	int option_index;

	static const struct option long_options[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "write", no_argument, NULL, 'W' },
		{ "hpw", required_argument, NULL, 0x100 },
		{ "mpw", required_argument, NULL, 0x101 },
		{ "lpw", required_argument, NULL, 0x102 },
		{ "burst", required_argument, NULL, 0x103 },
		{ NULL, 0, NULL, 0 }
	};

	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((opt = getopt_long(argc, argv, "hd:gi:r:L:WC:N:S:Q:O:", long_options, &option_index)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return 1;
		case 'd':
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0) {
				fprintf(stderr, "Invalid memory size '%s'\n", optarg);
				return rc;
			}
			env_opts->mem_size = u32;
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'i':
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0) {
				fprintf(stderr, "Invalid shared memory ID '%s'\n", optarg);
				return rc;
			}
			env_opts->shm_id = u32;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Failed to parse transport ID '%s'\n", optarg);
				return -EINVAL;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "Unknown log flag '%s'\n", optarg);
				return rc;
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'W':
			g_cfg.mode = IO_MODE_WRITE;
			break;
		case 'C':
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0) {
				fprintf(stderr, "Invalid command count '%s'\n", optarg);
				return rc;
			}
			g_cfg.cmds_per_queue = u32;
			break;
		case 'N':
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0) {
				fprintf(stderr, "Invalid block count '%s'\n", optarg);
				return rc;
			}
			g_cfg.lba_count = u32;
			break;
		case 'S':
			rc = parse_positive_u64(optarg, &u64);
			if (rc != 0) {
				fprintf(stderr, "Invalid start LBA '%s'\n", optarg);
				return rc;
			}
			g_cfg.start_lba = u64;
			break;
		case 'Q':
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0) {
				fprintf(stderr, "Invalid queue size '%s'\n", optarg);
				return rc;
			}
			g_cfg.queue_size = u32;
			g_cfg.queue_requests = u32;
			break;
		case 'O':
			g_cfg.log_path = optarg;
			break;
		case 0x100:
			rc = parse_weight(optarg, &g_cfg.hpw);
			if (rc != 0) {
				fprintf(stderr, "Invalid high priority weight '%s'\n", optarg);
				return rc;
			}
			break;
		case 0x101:
			rc = parse_weight(optarg, &g_cfg.mpw);
			if (rc != 0) {
				fprintf(stderr, "Invalid medium priority weight '%s'\n", optarg);
				return rc;
			}
			break;
		case 0x102:
			rc = parse_weight(optarg, &g_cfg.lpw);
			if (rc != 0) {
				fprintf(stderr, "Invalid low priority weight '%s'\n", optarg);
				return rc;
			}
			break;
		case 0x103:
			rc = parse_positive_u32(optarg, &u32);
			if (rc != 0 || u32 > 7U) {
				fprintf(stderr, "Invalid arbitration burst '%s'\n", optarg);
				return -ERANGE;
			}
			g_cfg.arbitration_burst = (uint8_t)u32;
			break;
		default:
			usage(argv[0]);
			return -EINVAL;
		}
	}

	if (g_cfg.queue_size < g_cfg.cmds_per_queue) {
		g_cfg.queue_size = g_cfg.cmds_per_queue;
	}
	if (g_cfg.queue_requests < g_cfg.queue_size) {
		g_cfg.queue_requests = g_cfg.queue_size;
	}

	return 0;
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		perror("ns_entry calloc");
		exit(EXIT_FAILURE);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace %d: size %ju GiB\n", spdk_nvme_ns_get_id(ns),
	       (uintmax_t)(spdk_nvme_ns_get_size(ns) / (uint64_t)1024 / 1024 / 1024));
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		perror("ctrlr_entry calloc");
		exit(EXIT_FAILURE);
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	printf("Controller: %s\n", entry->name);
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns != NULL) {
			register_ns(ctrlr, ns);
		}
	}
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		/* wait */
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	(void)cb_ctx;

	opts->arb_mechanism = SPDK_NVME_CC_AMS_WRR;
	opts->arbitration_burst = g_cfg.arbitration_burst;
	opts->high_priority_weight = weight_to_field(g_cfg.hpw);
	opts->medium_priority_weight = weight_to_field(g_cfg.mpw);
	opts->low_priority_weight = weight_to_field(g_cfg.lpw);
	if (g_cfg.queue_size > opts->io_queue_size) {
		opts->io_queue_size = g_cfg.queue_size;
	}
	if (g_cfg.queue_requests > opts->io_queue_requests) {
		opts->io_queue_requests = g_cfg.queue_requests;
	}

	printf("Probing %s\n", trid->traddr[0] ? trid->traddr : "(local PCIe)");
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	(void)cb_ctx;
	(void)opts;

	printf("Attached to %s\n", trid->traddr[0] ? trid->traddr : "(local PCIe)");
	register_ctrlr(ctrlr);
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct cmd_entry *entry = arg;
	struct qpair_ctx *ctx = entry->qpair_ctx;

	entry->completion_tick = spdk_get_ticks();
	entry->cid = cpl->cid;
	entry->status = cpl->status;
	entry->success = !spdk_nvme_cpl_is_error(cpl);

	completion_vector_append(entry);

	ctx->completed++;
	g_total_completed++;

	if (!entry->success) {
		g_total_errors++;
	}
}

static int
submit_burst(struct qpair_ctx *ctx, struct spdk_nvme_ns *ns)
{
	uint32_t i;
	int rc = 0;

	for (i = 0; i < g_cfg.cmds_per_queue; ++i) {
		struct cmd_entry *entry = &ctx->entries[i];
		uint64_t lba = ctx->base_lba + (uint64_t)i * g_cfg.lba_count;
		void *buffer = (uint8_t *)ctx->data_pool + (size_t)i * ctx->payload_size;

		entry->qpair_ctx = ctx;
		entry->buffer = buffer;
		entry->slba = lba;
		entry->nlb = g_cfg.lba_count;
		entry->opc = (g_cfg.mode == IO_MODE_WRITE) ? SPDK_NVME_OPC_WRITE : SPDK_NVME_OPC_READ;
		entry->submit_tick = spdk_get_ticks();
		entry->completion_tick = 0;
		entry->cid = UINT16_MAX;
		memset(&entry->status, 0, sizeof(entry->status));
		entry->success = false;

		if (g_cfg.mode == IO_MODE_WRITE) {
			rc = spdk_nvme_ns_cmd_write(ns, ctx->qpair, buffer, lba, g_cfg.lba_count,
					     io_complete, entry, 0);
		} else {
			rc = spdk_nvme_ns_cmd_read(ns, ctx->qpair, buffer, lba, g_cfg.lba_count,
					    io_complete, entry, 0);
		}

		if (rc != 0) {
			SPDK_ERRLOG("Failed to submit I/O for qpair %u (rc=%d)\n", ctx->qid, rc);
			return rc;
		}

		ctx->submitted++;
	}

	return 0;
}

static void
flush_submissions(struct qpair_ctx *qpairs, uint32_t num_qpairs)
{
	uint32_t i;

	for (i = 0; i < num_qpairs; ++i) {
		struct qpair_ctx *ctx = &qpairs[i];

		if (ctx->qpair == NULL) {
			continue;
		}

		/* Ring the SQ doorbell for all queued commands without reaping completions yet. */
		(void)spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}
}

static int
reserve_completion_capacity(void)
{
	uint64_t total = (uint64_t)g_cfg.cmds_per_queue * NUM_TEST_QPAIRS;

	if (total > SIZE_MAX) {
		return -ENOMEM;
	}

	return completion_vector_reserve((size_t)total);
}

static int
dump_completion_log(struct qpair_ctx *qpairs, uint32_t num_qpairs)
{
	FILE *out;
	bool to_stdout = (g_cfg.log_path != NULL && strcmp(g_cfg.log_path, "-") == 0);
	double ticks_to_us = 1e6 / (double)spdk_get_ticks_hz();
	uint64_t total_cmds = (uint64_t)g_cfg.cmds_per_queue * NUM_TEST_QPAIRS;
	uint32_t i;
	int rc = 0;

	if (g_cfg.log_path == NULL || to_stdout) {
		out = stdout;
	} else {
		out = fopen(g_cfg.log_path, "w");
		if (out == NULL) {
			perror("fopen");
			return -errno;
		}
	}

	fprintf(out, "sequence,qpair_id,priority,cmd_index,cid,opc,slba,nlb,submit_us,complete_us,latency_us,status\n");

	for (size_t idx = 0; idx < g_completion_log.count; ++idx) {
		struct cmd_entry *entry = g_completion_log.items[idx];
		struct qpair_ctx *ctx = entry->qpair_ctx;
		size_t cmd_index = (size_t)(entry - ctx->entries);
		double submit_us = entry->submit_tick * ticks_to_us;
		double complete_us = entry->completion_tick * ticks_to_us;
		double latency_us = (entry->completion_tick - entry->submit_tick) * ticks_to_us;
		const char *status_str = spdk_nvme_cpl_get_status_string(&entry->status);

		fprintf(out,
			"%zu,%u,%s,%zu,%u,%s,%" PRIu64 ",%u,%.3f,%.3f,%.3f,%s\n",
			idx,
			ctx->qid,
			qprio_to_string(ctx->qprio),
			cmd_index,
			entry->cid,
			(entry->opc == SPDK_NVME_OPC_WRITE) ? "write" : "read",
			entry->slba,
			entry->nlb,
			submit_us,
			complete_us,
			latency_us,
			status_str);
	}

	if (!to_stdout) {
		fclose(out);
	}

	printf("\nPer-qpair completion summary:\n");
	for (i = 0; i < num_qpairs; ++i) {
		struct qpair_ctx *ctx = &qpairs[i];
		double share = total_cmds ? ((double)ctx->completed * 100.0 / (double)total_cmds) : 0.0;

		printf("  QID %u (%s) -> %u completions (%.2f%%)\n",
		       ctx->qid, qprio_to_string(ctx->qprio), ctx->completed, share);
	}

	if (g_total_errors > 0) {
		printf("\nWARNING: observed %" PRIu64 " command errors.\n", g_total_errors);
	}

	if (g_log_error != 0) {
		fprintf(stderr, "Completion log truncated: %s\n", spdk_strerror(-g_log_error));
		rc = g_log_error;
	}

	return rc;
}

static int
run_wrr_burst_test(struct ns_entry *target)
{
	struct qpair_ctx qpairs[NUM_TEST_QPAIRS] = {};
	const enum spdk_nvme_qprio priorities[NUM_TEST_QPAIRS] = {
		SPDK_NVME_QPRIO_HIGH, SPDK_NVME_QPRIO_HIGH, SPDK_NVME_QPRIO_HIGH,
		SPDK_NVME_QPRIO_MEDIUM, SPDK_NVME_QPRIO_MEDIUM, SPDK_NVME_QPRIO_MEDIUM,
		SPDK_NVME_QPRIO_LOW, SPDK_NVME_QPRIO_LOW, SPDK_NVME_QPRIO_LOW,
	};
	uint32_t block_size = spdk_nvme_ns_get_sector_size(target->ns);
	uint64_t ns_size = spdk_nvme_ns_get_num_sectors(target->ns);
	uint64_t lbas_per_qpair = (uint64_t)g_cfg.cmds_per_queue * g_cfg.lba_count;
	uint64_t total_lbas = lbas_per_qpair * NUM_TEST_QPAIRS;
	uint64_t max_lba = g_cfg.start_lba + total_lbas;
	uint32_t i;
	int rc = 0;

	if (max_lba > ns_size) {
		fprintf(stderr,
			"Requested range exceeds namespace capacity (need %" PRIu64 ", have %" PRIu64 ").\n",
			max_lba, ns_size);
		return -EINVAL;
	}

	g_total_completed = 0;
	g_total_errors = 0;
	g_log_error = 0;
	g_completion_log.count = 0;

	rc = reserve_completion_capacity();
	if (rc != 0) {
		fprintf(stderr, "Failed to reserve completion log capacity (%s)\n", spdk_strerror(-rc));
		return rc;
	}

	printf("\nConfig:\n");
	printf("  Commands/qpair      : %u\n", g_cfg.cmds_per_queue);
	printf("  LBAs/command        : %u\n", g_cfg.lba_count);
	printf("  Arbitration weights : HPW=%u MPW=%u LPW=%u\n",
	       g_cfg.hpw, g_cfg.mpw, g_cfg.lpw);
	printf("  Arbitration burst   : %u\n", g_cfg.arbitration_burst);
	printf("  Queue depth         : %u\n", g_cfg.queue_size);
	printf("  Mode                : %s\n",
	       (g_cfg.mode == IO_MODE_WRITE) ? "write" : "read");

	for (i = 0; i < NUM_TEST_QPAIRS; ++i) {
		struct qpair_ctx *ctx = &qpairs[i];
		struct spdk_nvme_io_qpair_opts qopts;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(target->ctrlr, &qopts, sizeof(qopts));
		qopts.qprio = priorities[i];
		qopts.io_queue_size = g_cfg.queue_size;
		qopts.io_queue_requests = g_cfg.queue_requests;
		qopts.delay_cmd_submit = true;

		ctx->payload_size = (size_t)g_cfg.lba_count * block_size;
		ctx->data_pool = spdk_zmalloc(ctx->payload_size * g_cfg.cmds_per_queue,
					block_size, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (ctx->data_pool == NULL) {
			fprintf(stderr, "Unable to allocate I/O buffer for qpair %u\n", i);
			rc = -ENOMEM;
			goto cleanup;
		}

		memset(ctx->data_pool, (int)(i + 1), ctx->payload_size * g_cfg.cmds_per_queue);

		ctx->entries = calloc(g_cfg.cmds_per_queue, sizeof(struct cmd_entry));
		if (ctx->entries == NULL) {
			fprintf(stderr, "Unable to allocate completion records for qpair %u\n", i);
			rc = -ENOMEM;
			goto cleanup;
		}

		ctx->qpair = spdk_nvme_ctrlr_alloc_io_qpair(target->ctrlr, &qopts, sizeof(qopts));
		if (ctx->qpair == NULL) {
			fprintf(stderr, "Failed to allocate IO qpair for priority '%s'\n",
				qprio_to_string(priorities[i]));
			rc = -ENOMEM;
			goto cleanup;
		}

		ctx->qprio = priorities[i];
		ctx->qid = spdk_nvme_qpair_get_id(ctx->qpair);
		ctx->index = i;
		ctx->base_lba = g_cfg.start_lba + lbas_per_qpair * i;

		printf("  Qpair %u mapped to priority %s (QID %u, base LBA %" PRIu64 ")\n",
		       i, qprio_to_string(ctx->qprio), ctx->qid, ctx->base_lba);

		rc = submit_burst(ctx, target->ns);
		if (rc != 0) {
			goto cleanup;
		}
	}

	/* Ring the doorbells for every qpair only after all bursts are prepared. */
	flush_submissions(qpairs, NUM_TEST_QPAIRS);

	while (g_total_completed < (uint64_t)g_cfg.cmds_per_queue * NUM_TEST_QPAIRS) {
		for (i = 0; i < NUM_TEST_QPAIRS; ++i) {
			int32_t completions = spdk_nvme_qpair_process_completions(qpairs[i].qpair, 0);
			if (completions < 0) {
				SPDK_ERRLOG("Completion polling failed for qpair %u (rc=%d)\n", i, completions);
				rc = completions;
				goto cleanup;
			}
		}
	}

	rc = dump_completion_log(qpairs, NUM_TEST_QPAIRS);

cleanup:
	for (i = 0; i < NUM_TEST_QPAIRS; ++i) {
		struct qpair_ctx *ctx = &qpairs[i];
		if (ctx->qpair != NULL) {
			spdk_nvme_ctrlr_free_io_qpair(ctx->qpair);
			ctx->qpair = NULL;
		}
		if (ctx->data_pool != NULL) {
			spdk_free(ctx->data_pool);
			ctx->data_pool = NULL;
		}
		free(ctx->entries);
	}

	return rc;
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct ns_entry *ns_entry;

	spdk_env_opts_init(&opts);
	opts.name = "wrr_burst_test";
	opts.shm_id = -1;

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		if (rc > 0) {
			return EXIT_SUCCESS;
		}
		return EXIT_FAILURE;
	}

	if (g_trid.trtype == SPDK_NVME_TRANSPORT_CUSTOM) {
		snprintf(g_trid.trstring, sizeof(g_trid.trstring), "%d", SPDK_NVME_TRANSPORT_PCIE);
	}

	if (spdk_env_init(&opts) != 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return EXIT_FAILURE;
	}

	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed (%s)\n", spdk_strerror(-rc));
		cleanup();
		spdk_env_fini();
		return EXIT_FAILURE;
	}

	if (TAILQ_EMPTY(&g_namespaces)) {
		fprintf(stderr, "No active namespaces found.\n");
		cleanup();
		spdk_env_fini();
		return EXIT_FAILURE;
	}

	ns_entry = TAILQ_FIRST(&g_namespaces);
	printf("\nRunning WRR burst test on namespace %d\n", spdk_nvme_ns_get_id(ns_entry->ns));
	rc = run_wrr_burst_test(ns_entry);

	cleanup();
	spdk_env_fini();

	free(g_completion_log.items);
	g_completion_log.items = NULL;
	g_completion_log.count = 0;
	g_completion_log.capacity = 0;

	if (rc != 0) {
		fprintf(stderr, "Test failed: %s\n", spdk_strerror(-rc));
		return EXIT_FAILURE;
	}

	printf("\nWRR burst test completed successfully.\n");
	printf("Enable the 'nvme_pcie' trace group when running the test to capture doorbell activity per queue.\n");

	return EXIT_SUCCESS;
}
