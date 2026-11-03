/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

#define NUM_SQS 3
#define READ_DEPTH 8
#define TIMEOUT_SECONDS 30

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s failed\n", __func__, __LINE__, #condition); \
		return -EIO; \
	} \
} while (0)

#define EXPECT_RC(call, expected) do { \
	int64_t actual_rc = (call); \
	if (actual_rc != (expected)) { \
		fprintf(stderr, "%s:%d: %s returned %" PRId64 ", expected %d\n", \
			__func__, __LINE__, #call, actual_rc, (expected)); \
		return -EIO; \
	} \
} while (0)

enum poll_path {
	POLL_CQ,
	POLL_SQ,
	POLL_GROUP,
	POLL_LEGACY,
};

struct read_ctx {
	void *buffer;
	unsigned done;
	uint16_t sqid;
	bool submitted;
	bool allow_abort;
	bool error;
};

struct test_ctx {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_cq *cq;
	struct spdk_nvme_qpair *sq[NUM_SQS];
	struct spdk_nvme_qpair *legacy;
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_poll_group *other;
	struct read_ctx io[NUM_SQS][READ_DEPTH];
	uint16_t sqid[NUM_SQS];
	bool connecting[NUM_SQS];
	uint32_t nsid;
	uint32_t block_size;
	int poll_error;
};

static uint64_t
deadline(void)
{
	return spdk_get_ticks() + TIMEOUT_SECONDS * spdk_get_ticks_hz();
}

static int
set_mode(struct test_ctx *ctx, enum spdk_nvme_queue_mgmt_mode mode)
{
	struct spdk_nvme_queue_mgmt_opts opts;

	spdk_nvme_ctrlr_get_default_queue_mgmt_opts(&opts, sizeof(opts));
	opts.mode = mode;
	return spdk_nvme_ctrlr_set_queue_mgmt_opts(ctx->ctrlr, &opts, sizeof(opts));
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct read_ctx *io = arg;
	bool failed = spdk_nvme_cpl_is_error(cpl);
	bool aborted = cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
		       (cpl->status.sc == SPDK_NVME_SC_ABORTED_BY_REQUEST ||
			cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION);

	io->done++;
	if (io->done != 1 || (failed && !(io->allow_abort && aborted)) ||
	    (!failed && io->sqid != 0 && cpl->sqid != io->sqid)) {
		fprintf(stderr, "READ callback: expected SQID %u, got %u, count %u, status %u/%u\n",
			io->sqid, cpl->sqid, io->done, cpl->status.sct, cpl->status.sc);
		io->error = true;
	}
}

static void
disconnected(struct spdk_nvme_qpair *qpair, void *arg)
{
	struct test_ctx *ctx = arg;
	unsigned i;

	for (i = 0; i < NUM_SQS; i++) {
		if (ctx->sq[i] == qpair && !ctx->connecting[i]) {
			return;
		}
	}
	fprintf(stderr, "Unexpected poll-group disconnect\n");
	ctx->poll_error = -ENXIO;
}

static int
group_stats(struct test_ctx *ctx, struct spdk_nvme_pcie_stat *out)
{
	struct spdk_nvme_poll_group_stat *stats;
	uint32_t i;
	bool found = false;
	int rc;

	rc = spdk_nvme_poll_group_get_stats(ctx->group, &stats);
	if (rc != 0) {
		fprintf(stderr, "Cannot read poll-group statistics: %d\n", rc);
		return rc;
	}
	for (i = 0; i < stats->num_transports; i++) {
		if (stats->transport_stat[i]->trtype == SPDK_NVME_TRANSPORT_PCIE) {
			*out = stats->transport_stat[i]->pcie;
			found = true;
			break;
		}
	}
	spdk_nvme_poll_group_free_stats(ctx->group, stats);
	if (!found) {
		fprintf(stderr, "Missing PCIe poll-group statistics\n");
		return -ENOENT;
	}
	return 0;
}

static int64_t
poll_io(struct test_ctx *ctx, enum poll_path path, uint32_t budget)
{
	struct spdk_nvme_pcie_stat before, after;
	unsigned i, ready = 0;
	int64_t rc;

	switch (path) {
	case POLL_CQ:
		return spdk_nvme_pcie_cq_process_completions(ctx->cq, budget);
	case POLL_SQ:
		return spdk_nvme_qpair_process_completions(ctx->sq[0], budget);
	case POLL_LEGACY:
		return spdk_nvme_qpair_process_completions(ctx->legacy, budget);
	case POLL_GROUP:
		EXPECT_RC(group_stats(ctx, &before), 0);
		rc = spdk_nvme_poll_group_process_completions(ctx->group, budget, disconnected);
		if (rc < 0 || ctx->poll_error != 0) {
			fprintf(stderr, "Poll group failed: rc=%" PRId64 ", callback=%d\n", rc, ctx->poll_error);
			return rc < 0 ? rc : ctx->poll_error;
		}
		EXPECT_RC(group_stats(ctx, &after), 0);
		CHECK(after.polls == before.polls + 1);
		CHECK(after.completions - before.completions == (uint64_t)rc);
		for (i = 0; i < NUM_SQS; i++) {
			if (ctx->sq[i] && spdk_nvme_qpair_is_connected(ctx->sq[i])) {
				ready++;
			}
		}
		CHECK(budget == 0 || (uint64_t)rc <= (uint64_t)budget * ready);
		return rc;
	}
	fprintf(stderr, "Invalid I/O polling path\n");
	return -EINVAL;
}

static int
wait_connected(struct test_ctx *ctx)
{
	uint64_t end = deadline();
	unsigned i;
	bool ready;
	int64_t rc;

	do {
		ready = true;
		rc = spdk_nvme_poll_group_process_completions(ctx->group, 0, disconnected);
		CHECK(rc >= 0 && ctx->poll_error == 0);
		for (i = 0; i < NUM_SQS; i++) {
			if (ctx->sq[i] && ctx->connecting[i] && !spdk_nvme_qpair_is_connected(ctx->sq[i])) {
				ready = false;
			}
		}
		if (ready) {
			return 0;
		}
	} while (spdk_get_ticks() < end);
	fprintf(stderr, "Asynchronous SQ connection timed out\n");
	return -ETIMEDOUT;
}

static int
submit_reads(struct test_ctx *ctx, unsigned mask, unsigned depth, unsigned abort_mask,
	     bool legacy)
{
	unsigned i, j;
	struct read_ctx *io;
	int rc;

	for (i = 0; i < NUM_SQS; i++) {
		for (j = 0; j < READ_DEPTH; j++) {
			io = &ctx->io[i][j];
			CHECK(!io->submitted || io->done == 1);
			io->submitted = false;
		}
	}
	for (i = 0; i < NUM_SQS; i++) {
		if (!(mask & (1U << i))) {
			continue;
		}
		for (j = 0; j < depth; j++) {
			io = &ctx->io[i][j];
			io->done = 0;
			io->error = false;
			io->allow_abort = (abort_mask & (1U << i)) != 0;
			io->sqid = legacy ? 0 : ctx->sqid[i];
			rc = spdk_nvme_ns_cmd_read(ctx->ns, legacy ? ctx->legacy : ctx->sq[i],
						   io->buffer, 0, 1, read_complete, io, 0);
			if (rc != 0) {
				fprintf(stderr, "READ submit SQ %u slot %u: %d\n", io->sqid, j, rc);
				return rc;
			}
			io->submitted = true;
		}
	}
	return 0;
}

static int
wait_reads(struct test_ctx *ctx, enum poll_path path, uint32_t budget, bool count_cqes)
{
	uint64_t end = deadline(), cqes = 0;
	unsigned i, j, submitted, done;
	int64_t rc;

	do {
		rc = poll_io(ctx, path, budget);
		CHECK(rc >= 0);
		cqes += rc;
		submitted = done = 0;
		for (i = 0; i < NUM_SQS; i++) {
			for (j = 0; j < READ_DEPTH; j++) {
				struct read_ctx *io = &ctx->io[i][j];

				if (io->submitted) {
					submitted++;
					done += io->done;
					CHECK(!io->error && io->done <= 1);
				}
			}
		}
		if (done == submitted) {
			CHECK(!count_cqes || cqes == submitted);
			EXPECT_RC(poll_io(ctx, path, budget), 0);
			return 0;
		}
	} while (spdk_get_ticks() < end);
	fprintf(stderr, "READ drain timed out: %u/%u callbacks, %" PRIu64 " CQEs\n",
		done, submitted, cqes);
	return -ETIMEDOUT;
}

static int
read_rounds(struct test_ctx *ctx, enum poll_path path, unsigned mask, unsigned depth,
	    unsigned rounds)
{
	unsigned i;

	for (i = 0; i < rounds; i++) {
		EXPECT_RC(submit_reads(ctx, mask, depth, 0, path == POLL_LEGACY), 0);
		EXPECT_RC(wait_reads(ctx, path, 1, true), 0);
	}
	return 0;
}

static int
free_queues(struct test_ctx *ctx)
{
	unsigned i;
	int rc, result = 0;

	if (ctx->legacy) {
		rc = spdk_nvme_ctrlr_free_io_qpair(ctx->legacy);
		if (rc == 0) {
			ctx->legacy = NULL;
		} else {
			fprintf(stderr, "Legacy qpair cleanup failed: %d\n", rc);
			result = rc;
		}
	}
	for (i = 0; i < NUM_SQS; i++) {
		if (!ctx->sq[i]) {
			continue;
		}
		rc = spdk_nvme_ctrlr_free_io_sq(ctx->sq[i]);
		if (rc == 0) {
			ctx->sq[i] = NULL;
			ctx->connecting[i] = false;
		} else {
			fprintf(stderr, "SQ %u cleanup failed: %d\n", ctx->sqid[i], rc);
			result = rc;
		}
	}
	if (ctx->cq) {
		rc = spdk_nvme_ctrlr_free_io_cq(ctx->cq);
		if (rc == 0) {
			ctx->cq = NULL;
		} else {
			fprintf(stderr, "CQ cleanup failed: %d\n", rc);
			result = rc;
		}
	}
	return result;
}

static int
test_legacy(struct test_ctx *ctx, bool expect_disabled)
{
	struct spdk_nvme_queue_mgmt_opts opts;

	spdk_nvme_ctrlr_get_default_queue_mgmt_opts(&opts, sizeof(opts));
	CHECK(opts.mode == SPDK_NVME_QUEUE_MGMT_MODE_LEGACY);
	EXPECT_RC(spdk_nvme_ctrlr_get_queue_mgmt_opts(ctx->ctrlr, &opts, sizeof(opts)), 0);
	CHECK(opts.mode == SPDK_NVME_QUEUE_MGMT_MODE_LEGACY);
	ctx->legacy = spdk_nvme_ctrlr_alloc_io_qpair(ctx->ctrlr, NULL, 0);
	CHECK(ctx->legacy != NULL);
	EXPECT_RC(read_rounds(ctx, POLL_LEGACY, 1, 1, 2), 0);
	EXPECT_RC(free_queues(ctx), 0);
	EXPECT_RC(set_mode(ctx, SPDK_NVME_QUEUE_MGMT_MODE_SHARED_CQ),
		  expect_disabled ? -ENOTSUP : 0);
	EXPECT_RC(spdk_nvme_ctrlr_get_queue_mgmt_opts(ctx->ctrlr, &opts, sizeof(opts)), 0);
	CHECK(opts.mode == (expect_disabled ? SPDK_NVME_QUEUE_MGMT_MODE_LEGACY :
			    SPDK_NVME_QUEUE_MGMT_MODE_SHARED_CQ));
	printf("PASS: legacy defaults/I/O and %s build gate\n", expect_disabled ? "disabled" : "enabled");
	return 0;
}

static int
test_independent_queues(struct test_ctx *ctx)
{
	struct spdk_nvme_io_cq_opts cq_opts = {.io_queue_size = 8, .cqid = 3};
	struct spdk_nvme_io_qpair_opts opts;

	ctx->cq = spdk_nvme_ctrlr_alloc_io_cq(ctx->ctrlr, &cq_opts, sizeof(cq_opts));
	CHECK(ctx->cq != NULL);
	CHECK(spdk_nvme_ctrlr_alloc_io_cq(ctx->ctrlr, &cq_opts, sizeof(cq_opts)) == NULL);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctx->ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = 16;
	opts.io_queue_requests = 32;
	ctx->sqid[0] = 3;
	ctx->sqid[1] = 1;
	ctx->sq[0] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, 3, &opts, sizeof(opts));
	ctx->sq[1] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, 1, &opts, sizeof(opts));
	CHECK(ctx->sq[0] && ctx->sq[1]);
	CHECK(spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, 3, &opts, sizeof(opts)) == NULL);
	EXPECT_RC(spdk_nvme_ctrlr_free_io_cq(ctx->cq), -EBUSY);
	EXPECT_RC(set_mode(ctx, SPDK_NVME_QUEUE_MGMT_MODE_LEGACY), -EBUSY);
	EXPECT_RC(read_rounds(ctx, POLL_CQ, 3, 1, 24), 0);
	EXPECT_RC(read_rounds(ctx, POLL_SQ, 3, 1, 8), 0);
	EXPECT_RC(spdk_nvme_ctrlr_free_io_sq(ctx->sq[0]), 0);
	ctx->sq[0] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, 3, &opts, sizeof(opts));
	CHECK(ctx->sq[0] != NULL);
	EXPECT_RC(read_rounds(ctx, POLL_CQ, 3, 1, 2), 0);
	ctx->legacy = spdk_nvme_ctrlr_alloc_io_qpair(ctx->ctrlr, NULL, 0);
	CHECK(ctx->legacy != NULL);
	EXPECT_RC(read_rounds(ctx, POLL_LEGACY, 4, 1, 2), 0);
	EXPECT_RC(free_queues(ctx), 0);
	EXPECT_RC(set_mode(ctx, SPDK_NVME_QUEUE_MGMT_MODE_LEGACY), 0);
	EXPECT_RC(set_mode(ctx, SPDK_NVME_QUEUE_MGMT_MODE_SHARED_CQ), 0);
	printf("PASS: independent IDs/depths, busy ordering, routing, wraps, reuse and coexistence\n");
	return 0;
}

static int
connect_members(struct test_ctx *ctx, unsigned mask)
{
	unsigned i;

	for (i = 0; i < NUM_SQS; i++) {
		if ((mask & (1U << i)) && ctx->sq[i]) {
			ctx->connecting[i] = true;
			EXPECT_RC(spdk_nvme_ctrlr_connect_io_qpair(ctx->ctrlr, ctx->sq[i]), 0);
		}
	}
	return wait_connected(ctx);
}

static int
test_poll_groups(struct test_ctx *ctx)
{
	struct spdk_nvme_io_cq_opts cq_opts = {.io_queue_size = 64, .cqid = 3};
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_pcie_stat before, after;
	struct spdk_nvme_poll_group *old;
	unsigned i;

	ctx->cq = spdk_nvme_ctrlr_alloc_io_cq(ctx->ctrlr, &cq_opts, sizeof(cq_opts));
	CHECK(ctx->cq != NULL);
	ctx->group = spdk_nvme_poll_group_create(ctx, NULL);
	ctx->other = spdk_nvme_poll_group_create(ctx, NULL);
	CHECK(ctx->group && ctx->other);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctx->ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = 16;
	opts.io_queue_requests = 32;
	opts.create_only = true;
	opts.async_mode = true;
	opts.delay_cmd_submit = true;
	for (i = 0; i < NUM_SQS; i++) {
		ctx->sqid[i] = i + 1;
		ctx->sq[i] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, i + 1, &opts, sizeof(opts));
		CHECK(ctx->sq[i] != NULL);
	}
	EXPECT_RC(spdk_nvme_pcie_cq_process_completions(ctx->cq, 0), 0);
	EXPECT_RC(spdk_nvme_poll_group_add(ctx->group, ctx->sq[0]), 0);
	EXPECT_RC(spdk_nvme_poll_group_add(ctx->other, ctx->sq[1]), -EINVAL);
	EXPECT_RC(spdk_nvme_poll_group_add(ctx->group, ctx->sq[1]), 0);
	EXPECT_RC(spdk_nvme_poll_group_add(ctx->group, ctx->sq[2]), 0);
	EXPECT_RC(spdk_nvme_pcie_cq_process_completions(ctx->cq, 0), -EBUSY);
	EXPECT_RC(connect_members(ctx, 3), 0);
	EXPECT_RC(spdk_nvme_qpair_process_completions(ctx->sq[0], 0), -EBUSY);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 3, 4, 4), 0);
	EXPECT_RC(connect_members(ctx, 4), 0);
	EXPECT_RC(group_stats(ctx, &before), 0);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 7, READ_DEPTH, 32), 0);
	EXPECT_RC(group_stats(ctx, &after), 0);
	CHECK(after.sq_mmio_doorbell_updates + after.sq_shadow_doorbell_updates >
	      before.sq_mmio_doorbell_updates + before.sq_shadow_doorbell_updates);
	for (i = 0; i < NUM_SQS; i++) {
		ctx->connecting[i] = false;
		spdk_nvme_ctrlr_disconnect_io_qpair(ctx->sq[i]);
		EXPECT_RC(spdk_nvme_poll_group_remove(ctx->group, ctx->sq[i]), 0);
		if (i == 0) {
			EXPECT_RC(spdk_nvme_poll_group_add(ctx->other, ctx->sq[0]), -EINVAL);
		}
	}
	old = ctx->group;
	ctx->group = ctx->other;
	ctx->other = old;
	for (i = 0; i < NUM_SQS; i++) {
		EXPECT_RC(spdk_nvme_ctrlr_free_io_sq(ctx->sq[i]), 0);
		ctx->sq[i] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, i + 1, &opts, sizeof(opts));
		CHECK(ctx->sq[i] != NULL);
		EXPECT_RC(spdk_nvme_poll_group_add(ctx->group, ctx->sq[i]), 0);
	}
	EXPECT_RC(spdk_nvme_poll_group_destroy(ctx->other), 0);
	ctx->other = NULL;
	EXPECT_RC(connect_members(ctx, 7), 0);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 7, 4, 4), 0);
	EXPECT_RC(spdk_nvme_ctrlr_free_io_sq(ctx->sq[2]), 0);
	ctx->sq[2] = NULL;
	ctx->connecting[2] = false;
	EXPECT_RC(spdk_nvme_ctrlr_free_io_cq(ctx->cq), -EBUSY);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 3, 4, 2), 0);
	printf("PASS: async/create-only progress, drain-once counters, budgets, delayed doorbells and migration\n");
	return 0;
}

static int
test_reset(struct test_ctx *ctx)
{
	EXPECT_RC(submit_reads(ctx, 3, 4, 3, false), 0);
	EXPECT_RC(poll_io(ctx, POLL_GROUP, 1), 0);
	EXPECT_RC(spdk_nvme_ctrlr_reset(ctx->ctrlr), 0);
	ctx->ns = spdk_nvme_ctrlr_get_ns(ctx->ctrlr, ctx->nsid);
	CHECK(ctx->ns && spdk_nvme_ns_is_active(ctx->ns));
	CHECK(spdk_nvme_ns_get_sector_size(ctx->ns) == ctx->block_size);
	EXPECT_RC(wait_connected(ctx), 0);
	EXPECT_RC(wait_reads(ctx, POLL_GROUP, 1, false), 0);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 3, 4, 4), 0);
	printf("PASS: reset with outstanding reads and post-reset poll-group I/O\n");
	return 0;
}

static int
test_delete_under_load(struct test_ctx *ctx)
{
	struct spdk_nvme_io_qpair_opts opts;
	unsigned i;

	EXPECT_RC(submit_reads(ctx, 3, 4, 2, false), 0);
	EXPECT_RC(poll_io(ctx, POLL_GROUP, 1), 0);
	EXPECT_RC(spdk_nvme_ctrlr_free_io_sq(ctx->sq[1]), 0);
	ctx->sq[1] = NULL;
	ctx->connecting[1] = false;
	for (i = 0; i < 4; i++) {
		CHECK(ctx->io[1][i].done == 1 && !ctx->io[1][i].error);
	}
	EXPECT_RC(wait_reads(ctx, POLL_GROUP, 1, false), 0);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctx->ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = 16;
	opts.io_queue_requests = 32;
	opts.create_only = true;
	opts.async_mode = true;
	opts.delay_cmd_submit = true;
	ctx->sq[1] = spdk_nvme_ctrlr_alloc_io_sq(ctx->ctrlr, ctx->cq, 2, &opts, sizeof(opts));
	CHECK(ctx->sq[1] != NULL);
	EXPECT_RC(spdk_nvme_poll_group_add(ctx->group, ctx->sq[1]), 0);
	EXPECT_RC(connect_members(ctx, 2), 0);
	EXPECT_RC(read_rounds(ctx, POLL_GROUP, 3, 4, 4), 0);
	printf("PASS: deletion under load, sibling progress and SQID reuse in an owned CQ\n");
	return 0;
}

static void
usage(const char *program)
{
	printf("Usage: %s -r 'trtype:PCIe traddr:DDDD:BB:DD.F' [-n NSID] [--reset | --expect-disabled]\n",
	       program);
}

int
main(int argc, char **argv)
{
	const struct option options[] = {
		{"reset", no_argument, NULL, 'R'},
		{"expect-disabled", no_argument, NULL, 'D'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	struct spdk_nvme_transport_id trid = {.trtype = SPDK_NVME_TRANSPORT_PCIE};
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	struct spdk_env_opts env_opts;
	struct spdk_pci_addr pci_addr;
	struct test_ctx ctx = {};
	bool reset = false, expect_disabled = false;
	unsigned i, j;
	long nsid;
	int ch, rc = 0, cleanup_rc;

	while ((ch = getopt_long(argc, argv, "r:n:h", options, NULL)) != -1) {
		switch (ch) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&trid, optarg) != 0) {
				fprintf(stderr, "Invalid transport ID\n");
				return 1;
			}
			break;
		case 'n':
			nsid = spdk_strtol(optarg, 10);
			if (nsid <= 0 || (uint64_t)nsid > UINT32_MAX) {
				fprintf(stderr, "Invalid namespace ID\n");
				return 1;
			}
			ctx.nsid = nsid;
			break;
		case 'R':
			reset = true;
			break;
		case 'D':
			expect_disabled = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (optind != argc || (reset && expect_disabled) || trid.trtype != SPDK_NVME_TRANSPORT_PCIE ||
	    spdk_pci_addr_parse(&pci_addr, trid.traddr) != 0) {
		usage(argv[0]);
		return 1;
	}
	spdk_env_opts_init(&env_opts);
	env_opts.name = "shared_cq_sit";
	env_opts.core_mask = "0x1";
	env_opts.mem_size = 256;
	env_opts.pci_allowed = &pci_addr;
	env_opts.num_pci_addr = 1;
	if (spdk_env_init(&env_opts) < 0) {
		fprintf(stderr, "Environment initialization failed\n");
		return 1;
	}
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
	ctrlr_opts.num_io_queues = 8;
	ctrlr_opts.io_queue_size = 64;
	ctrlr_opts.admin_timeout_ms = TIMEOUT_SECONDS * 1000;
	ctx.ctrlr = spdk_nvme_connect(&trid, &ctrlr_opts, sizeof(ctrlr_opts));
	if (!ctx.ctrlr) {
		fprintf(stderr, "Cannot attach controller %s\n", trid.traddr);
		rc = -ENODEV;
		goto finish;
	}
	if (ctx.nsid == 0) {
		ctx.nsid = spdk_nvme_ctrlr_get_first_active_ns(ctx.ctrlr);
	}
	ctx.ns = spdk_nvme_ctrlr_get_ns(ctx.ctrlr, ctx.nsid);
	if (!ctx.ns || !spdk_nvme_ns_is_active(ctx.ns) || spdk_nvme_ns_get_md_size(ctx.ns) != 0 ||
	    spdk_nvme_ns_get_num_sectors(ctx.ns) == 0 ||
	    spdk_nvme_ctrlr_get_opts(ctx.ctrlr)->num_io_queues < 4 ||
	    spdk_nvme_ctrlr_get_regs_cap(ctx.ctrlr).bits.mqes < 63) {
		fprintf(stderr, "Need four I/O queue IDs, depth 64 and an active namespace without metadata\n");
		rc = -ENOTSUP;
		goto cleanup;
	}
	ctx.block_size = spdk_nvme_ns_get_sector_size(ctx.ns);
	for (i = 0; i < NUM_SQS; i++) {
		for (j = 0; j < READ_DEPTH; j++) {
			ctx.io[i][j].buffer = spdk_dma_zmalloc(ctx.block_size, 4096, NULL);
			if (!ctx.io[i][j].buffer) {
				fprintf(stderr, "Cannot allocate READ DMA buffer\n");
				rc = -ENOMEM;
				goto cleanup;
			}
		}
	}
	rc = test_legacy(&ctx, expect_disabled);
	if (rc == 0 && !expect_disabled) {
		rc = test_independent_queues(&ctx);
		if (rc == 0) {
			rc = test_poll_groups(&ctx);
		}
		if (rc == 0 && reset) {
			rc = test_reset(&ctx);
		}
		if (rc == 0) {
			rc = test_delete_under_load(&ctx);
		}
	}
cleanup:
	cleanup_rc = free_queues(&ctx);
	if (cleanup_rc != 0) {
		rc = cleanup_rc;
	}
	cleanup_rc = set_mode(&ctx, SPDK_NVME_QUEUE_MGMT_MODE_LEGACY);
	if (cleanup_rc != 0) {
		fprintf(stderr, "Cannot restore legacy mode: %d\n", cleanup_rc);
		rc = cleanup_rc;
	}
	cleanup_rc = spdk_nvme_detach(ctx.ctrlr);
	if (cleanup_rc != 0) {
		fprintf(stderr, "Controller detach failed: %d; retaining DMA buffers until process exit\n",
			cleanup_rc);
		return 1;
	}
	if (ctx.group && (cleanup_rc = spdk_nvme_poll_group_destroy(ctx.group)) != 0) {
		fprintf(stderr, "Poll-group cleanup failed: %d\n", cleanup_rc);
		rc = cleanup_rc;
	}
	if (ctx.other && (cleanup_rc = spdk_nvme_poll_group_destroy(ctx.other)) != 0) {
		fprintf(stderr, "Secondary poll-group cleanup failed: %d\n", cleanup_rc);
		rc = cleanup_rc;
	}
finish:
	for (i = 0; i < NUM_SQS; i++) {
		for (j = 0; j < READ_DEPTH; j++) {
			spdk_dma_free(ctx.io[i][j].buffer);
		}
	}
	spdk_env_fini();
	printf("%s: shared-CQ SIT (disabled=%s, reset=%s)\n",
	       rc == 0 ? "PASS" : "FAIL", expect_disabled ? "yes" : "no", reset ? "yes" : "no");
	return rc == 0 ? 0 : 1;
}
