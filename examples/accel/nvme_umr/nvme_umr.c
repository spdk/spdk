/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Dell Inc, or its subsidiaries.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/module/accel/mlx5.h"

#define DATA_PATTERN "Hello Accel+NVMe!"

struct io_ctx {
	int		is_completed;
	int		status;
	char		*buf;
	char		*expected;
	struct io_ctx	*next;
};

static struct spdk_thread		*g_thread;
static __thread struct spdk_io_channel	*g_accel_ch;
static struct spdk_nvme_ctrlr		*g_ctrlr;
static struct spdk_nvme_ns		*g_ns;
static struct spdk_nvme_qpair		*g_qpair;
static struct spdk_nvme_detach_ctx	*g_detach_ctx;
static char				*g_write_buf;
static char				*g_read_buf;

static bool g_env_initialized = false;
static bool g_thread_lib_initialized = false;
static bool g_iobuf_initialized = false;
static bool g_accel_initialized = false;

static char g_traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1] = "127.0.0.1";
static char g_trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1] = "4420";
static char *g_allowed_devs = NULL;

/* Call counters -- proof that the transport-level accel fn_table is
 * actually being invoked by lib/nvme/nvme_rdma.c during UMR I/O,
 * rather than the transport silently falling back to the normal
 * (non-accel) data path. */
static uint64_t g_append_copy_calls;
static uint64_t g_finish_sequence_calls;
static uint64_t g_reverse_sequence_calls;
static uint64_t g_abort_sequence_calls;

static int
nvme_accel_append_copy(void *ctx, void **seq,
		       struct iovec *dst_iovs, uint32_t dst_iovcnt,
		       struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
		       struct iovec *src_iovs, uint32_t src_iovcnt,
		       struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		       spdk_nvme_accel_step_cb cb_fn, void *cb_arg)
{
	g_append_copy_calls++;

	if (!g_accel_ch) {
		g_accel_ch = spdk_accel_get_io_channel();
		if (!g_accel_ch) {
			return -ENOMEM;
		}
	}

	return spdk_accel_append_copy((struct spdk_accel_sequence **)seq, g_accel_ch,
				      dst_iovs, dst_iovcnt, dst_domain, dst_domain_ctx,
				      src_iovs, src_iovcnt, src_domain, src_domain_ctx,
				      (spdk_accel_step_cb)cb_fn, cb_arg);
}

static void
nvme_accel_finish_sequence(void *seq, spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	g_finish_sequence_calls++;

	spdk_accel_sequence_finish((struct spdk_accel_sequence *)seq,
				   (spdk_accel_completion_cb)cb_fn, cb_arg);
}

static void
nvme_accel_reverse_sequence(void *seq)
{
	g_reverse_sequence_calls++;

	spdk_accel_sequence_reverse((struct spdk_accel_sequence *)seq);
}

static void
nvme_accel_abort_sequence(void *seq)
{
	g_abort_sequence_calls++;

	spdk_accel_sequence_abort((struct spdk_accel_sequence *)seq);
}

/* See struct spdk_nvme_accel_fn_table.poll: without this, spdk_nvme_ctrlr_free_io_qpair()
 * could hang below if a UMR request is still in flight when it's called. */
static void
nvme_accel_poll(void *ctx)
{
	spdk_thread_poll(g_thread, 0, 0);
}

static struct spdk_nvme_accel_fn_table g_accel_fn_table = {
	.table_size	  = sizeof(struct spdk_nvme_accel_fn_table),
	.append_copy	  = nvme_accel_append_copy,
	.finish_sequence  = nvme_accel_finish_sequence,
	.reverse_sequence = nvme_accel_reverse_sequence,
	.abort_sequence   = nvme_accel_abort_sequence,
	.poll		  = nvme_accel_poll,
};

static void
accel_fini_cb(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
iobuf_fini_cb(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
cleanup_all(void)
{
	bool done;

	free(g_allowed_devs);
	g_allowed_devs = NULL;

	if (g_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
		g_qpair = NULL;
	}
	if (g_ctrlr) {
		spdk_nvme_detach_async(g_ctrlr, &g_detach_ctx);
		g_ctrlr = NULL;
	}

	if (g_detach_ctx) {
		spdk_nvme_detach_poll(g_detach_ctx);
		g_detach_ctx = NULL;
	}

	spdk_free(g_read_buf);
	g_read_buf = NULL;
	spdk_free(g_write_buf);
	g_write_buf = NULL;

	if (g_accel_ch) {
		spdk_put_io_channel(g_accel_ch);
		g_accel_ch = NULL;
	}

	if (g_thread) {
		spdk_set_thread(g_thread);

		if (g_accel_initialized) {
			done = false;
			spdk_accel_finish(accel_fini_cb, &done);
			while (!done) {
				spdk_thread_poll(g_thread, 0, 0);
			}
			g_accel_initialized = false;
		}

		if (g_iobuf_initialized) {
			done = false;
			spdk_iobuf_finish(iobuf_fini_cb, &done);
			while (!done) {
				spdk_thread_poll(g_thread, 0, 0);
			}
			g_iobuf_initialized = false;
		}

		spdk_thread_exit(g_thread);
		while (!spdk_thread_is_exited(g_thread)) {
			spdk_thread_poll(g_thread, 0, 0);
		}
		spdk_set_thread(NULL);
		spdk_thread_destroy(g_thread);
		g_thread = NULL;
	}

	if (g_thread_lib_initialized) {
		spdk_thread_lib_fini();
		g_thread_lib_initialized = false;
	}
	if (g_env_initialized) {
		spdk_env_fini();
		g_env_initialized = false;
	}
}

static void read_cb(void *arg, const struct spdk_nvme_cpl *completion);

static void
write_cb(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_ctx *ctx = arg;
	struct io_ctx *read_ctx = ctx->next;

	if (spdk_nvme_cpl_is_error(completion)) {
		fprintf(stderr, "write failed\n");
		read_ctx->status = -EIO;
		read_ctx->is_completed = 1;
		return;
	}

	if (spdk_nvme_ns_cmd_read(g_ns, g_qpair, read_ctx->buf, 0, 1,
				  read_cb, read_ctx, 0) != 0) {
		fprintf(stderr, "failed to submit read\n");
		read_ctx->status = -EIO;
		read_ctx->is_completed = 1;
	}
}

static void
read_cb(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_ctx *ctx = arg;

	if (spdk_nvme_cpl_is_error(completion)) {
		fprintf(stderr, "read failed\n");
		ctx->status = -EIO;
	} else if (strcmp(ctx->buf, ctx->expected) != 0) {
		fprintf(stderr, "data mismatch: expected '%s', got '%s'\n",
			ctx->expected, ctx->buf);
		ctx->status = -1;
	} else {
		printf("verified: '%s'\n", ctx->buf);
	}

	ctx->is_completed = 1;
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
	int nsid;
	struct spdk_nvme_ns *ns;

	if (g_ctrlr) {
		return;
	}

	g_ctrlr = ctrlr;
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns) {
			g_ns = ns;
			break;
		}
	}
}

static void
usage(const char *prog)
{
	printf("usage: %s [options]\n", prog);
	printf("nvme_umr options:\n");
	printf("\t[-r RDMA target address]\n");
	printf("\t[-p RDMA target service port]\n");
	printf("\t[-d allowed mlx5 devices (default: pick the 'best' one)]\n");
	printf("\t[-h, --help show this help message]\n");
	printf("For multi-port/Multi-NIC setups pass all relevant mlx5 devices to -d,\n");
	printf("e.g. -d mlx5_0,mlx5_1 so the accel_mlx5 channel can match the RDMA PD.\n");
}

static int
parse_args(int argc, char **argv, struct spdk_nvme_transport_id *trid)
{
	static const struct option cmdline_opts[] = {
		{"traddr", required_argument, NULL, 'r'},
		{"trsvcid", required_argument, NULL, 'p'},
		{"allowed-devs", required_argument, NULL, 'd'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int op;

	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_RDMA);
	trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	snprintf(trid->traddr, sizeof(trid->traddr), "%s", g_traddr);
	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", g_trsvcid);

	if (argc == 1) {
		usage(argv[0]);
		return -1;
	}

	opterr = 0;
	while ((op = getopt_long(argc, argv, "r:p:d:h", cmdline_opts, NULL)) != -1) {
		switch (op) {
		case 'r':
			snprintf(g_traddr, sizeof(g_traddr), "%s", optarg);
			snprintf(trid->traddr, sizeof(trid->traddr), "%s", g_traddr);
			break;
		case 'p':
			snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg);
			snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%s", g_trsvcid);
			break;
		case 'd':
			free(g_allowed_devs);
			g_allowed_devs = strdup(optarg);
			if (!g_allowed_devs) {
				fprintf(stderr, "Failed to allocate allowed_devs string\n");
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return -1;
		default:
			fprintf(stderr, "Invalid arguments.\n");
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts			opts = {};
	struct spdk_nvme_transport_id		trid = {};
	struct spdk_nvme_transport_opts		topts = {};
	struct io_ctx				write_ctx = {};
	struct io_ctx				read_ctx = {};
	struct spdk_accel_mlx5_attr		mlx5_attr;
	int					rc = 0;

	rc = parse_args(argc, argv, &trid);
	if (rc < 0) {
		return 0;
	}
	if (rc != 0) {
		return rc;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nvme_umr";
	if (spdk_env_init(&opts) != 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}
	g_env_initialized = true;

	if (spdk_thread_lib_init(NULL, 0) != 0) {
		fprintf(stderr, "Unable to initialize thread library\n");
		rc = 1;
		goto cleanup;
	}
	g_thread_lib_initialized = true;

	g_thread = spdk_thread_create("nvme_umr", NULL);
	if (!g_thread) {
		fprintf(stderr, "Unable to create SPDK thread\n");
		rc = 1;
		goto cleanup;
	}
	spdk_set_thread(g_thread);

	spdk_accel_mlx5_get_default_attr(&mlx5_attr, sizeof(mlx5_attr));
	mlx5_attr.enable_driver = true;
	if (g_allowed_devs) {
		mlx5_attr.allowed_devs = g_allowed_devs;
	}
	rc = spdk_accel_mlx5_enable(&mlx5_attr);
	if (rc != 0 && rc != -EEXIST) {
		fprintf(stderr, "Failed to enable accel_mlx5: %s\n", spdk_strerror(-rc));
		rc = 1;
		goto cleanup;
	}

	if (spdk_iobuf_initialize() != 0) {
		fprintf(stderr, "Unable to initialize iobuf\n");
		rc = 1;
		goto cleanup;
	}
	g_iobuf_initialized = true;

	if (spdk_accel_initialize() != 0) {
		fprintf(stderr, "Unable to initialize accel framework\n");
		rc = 1;
		goto cleanup;
	}
	g_accel_initialized = true;

	spdk_thread_poll(g_thread, 0, 0);

	spdk_nvme_transport_get_opts(&topts, sizeof(topts));
	topts.rdma_umr_per_io = true;
	rc = spdk_nvme_transport_set_opts(&topts, sizeof(topts));
	if (rc != 0) {
		fprintf(stderr, "Failed to set transport options: %s\n", spdk_strerror(-rc));
		rc = 1;
		goto cleanup;
	}

	rc = spdk_nvme_transport_set_accel_fn_table(&g_accel_fn_table, NULL);
	if (rc != 0) {
		fprintf(stderr, "Failed to set accel fn_table: %s\n", spdk_strerror(-rc));
		rc = 1;
		goto cleanup;
	}

	rc = spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0 || g_ctrlr == NULL || g_ns == NULL) {
		fprintf(stderr, "Unable to find NVMe controller/namespace\n");
		rc = 1;
		goto cleanup;
	}

	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (!g_qpair) {
		fprintf(stderr, "Unable to allocate I/O qpair\n");
		rc = 1;
		goto cleanup;
	}

	g_write_buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	g_read_buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_write_buf || !g_read_buf) {
		fprintf(stderr, "Failed to allocate I/O buffers\n");
		rc = 1;
		goto cleanup;
	}

	snprintf(g_write_buf, 0x1000, "%s", DATA_PATTERN);

	write_ctx.buf = g_write_buf;
	write_ctx.next = &read_ctx;
	read_ctx.buf = g_read_buf;
	read_ctx.expected = g_write_buf;

	rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, g_write_buf, 0, 1, write_cb, &write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "Failed to submit write: %s\n", spdk_strerror(-rc));
		rc = 1;
		goto cleanup;
	}

	while (!read_ctx.is_completed) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
		spdk_thread_poll(g_thread, 0, 0);
	}

	if (read_ctx.status != 0) {
		fprintf(stderr, "I/O failed or data mismatch\n");
		rc = 1;
	}

	printf("accel fn_table calls: append_copy=%"PRIu64" finish_sequence=%"PRIu64
	       " reverse_sequence=%"PRIu64" abort_sequence=%"PRIu64"\n",
	       g_append_copy_calls, g_finish_sequence_calls,
	       g_reverse_sequence_calls, g_abort_sequence_calls);
	if (g_append_copy_calls == 0 || g_finish_sequence_calls == 0) {
		fprintf(stderr, "accel fn_table was never invoked -- UMR path was not exercised\n");
		rc = 1;
	}

cleanup:
	cleanup_all();
	return rc;
}
