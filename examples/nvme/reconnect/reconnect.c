/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	enum spdk_nvme_transport_type		trtype;

	struct spdk_nvme_qpair			**unused_qpairs;

	struct ctrlr_entry			*next;
	char					name[1024];
	int					num_resets;
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;

	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	uint32_t		num_io_requests;
	uint64_t		size_in_ios;
	uint32_t		block_size;
	uint32_t		md_size;
	bool			md_interleave;
	bool			pi_loc;
	enum spdk_nvme_pi_type	pi_type;
	uint32_t		io_flags;
	char			name[1024];
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	uint64_t		io_completed;
	uint64_t		total_tsc;
	uint64_t		min_tsc;
	uint64_t		max_tsc;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

	int			num_qpairs;
	struct spdk_nvme_qpair	**qpair;
	bool			*failed_qpair;
	int			last_qpair;

	struct ns_worker_ctx	*next;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	struct iovec		iov;
	struct iovec		md_iov;
	uint64_t		submit_tsc;
	bool			is_read;
	struct spdk_dif_ctx	dif_ctx;
};

struct worker_thread {
	struct ns_worker_ctx	*ns_ctx;
	struct worker_thread	*next;
	unsigned		lcore;
};

/* For basic reset handling. */
static int g_max_ctrlr_resets = 15;

static bool g_vmd = false;

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;

static uint64_t g_tsc_rate;

static uint32_t g_io_align = 0x200;
static uint32_t g_io_size_bytes;
static uint32_t g_max_io_md_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_metacfg_pract_flag;
static uint32_t g_metacfg_prchk_flags;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_nr_io_queues_per_ns = 1;
static int g_nr_unused_io_queues = 0;
static int g_time_in_sec;
static uint32_t g_max_completions;
static int g_dpdk_mem;
static int g_shm_id = -1;
static uint32_t g_disable_sq_cmb;
static bool g_no_pci;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification = false;
static uint32_t g_keep_alive_timeout_in_ms = 0;

static const char *g_core_mask;

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	uint16_t			nsid;
	TAILQ_ENTRY(trid_entry)		tailq;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static inline void
task_complete(struct perf_task *task);
static void submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth);

static void io_complete(void *ctx, const struct spdk_nvme_cpl *cpl);

static void
nvme_setup_payload(struct perf_task *task, uint8_t pattern)
{
	uint32_t max_io_size_bytes, max_io_md_size;

	/* maximum extended lba format size from all active namespace,
	 * it's same with g_io_size_bytes for namespace without metadata.
	 */
	max_io_size_bytes = g_io_size_bytes + g_max_io_md_size * g_max_io_size_blocks;
	task->iov.iov_base = spdk_dma_zmalloc(max_io_size_bytes, g_io_align, NULL);
	task->iov.iov_len = max_io_size_bytes;
	if (task->iov.iov_base == NULL) {
		fprintf(stderr, "task->buf spdk_dma_zmalloc failed\n");
		exit(1);
	}
	memset(task->iov.iov_base, pattern, task->iov.iov_len);

	max_io_md_size = g_max_io_md_size * g_max_io_size_blocks;
	if (max_io_md_size != 0) {
		task->md_iov.iov_base = spdk_dma_zmalloc(max_io_md_size, g_io_align, NULL);
		task->md_iov.iov_len = max_io_md_size;
		if (task->md_iov.iov_base == NULL) {
			fprintf(stderr, "task->md_buf spdk_dma_zmalloc failed\n");
			spdk_dma_free(task->iov.iov_base);
			exit(1);
		}
	}
}

static int
nvme_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	       struct ns_entry *entry, uint64_t offset_in_ios)
{
	uint64_t lba;
	int rc;
	int qp_num;

	enum dif_mode {
		DIF_MODE_NONE = 0,
		DIF_MODE_DIF = 1,
		DIF_MODE_DIX = 2,
	}  mode = DIF_MODE_NONE;

	lba = offset_in_ios * entry->io_size_blocks;

	if (entry->md_size != 0 && !(entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		if (entry->md_interleave) {
			mode = DIF_MODE_DIF;
		} else {
			mode = DIF_MODE_DIX;
		}
	}

	qp_num = ns_ctx->last_qpair;
	ns_ctx->last_qpair++;
	if (ns_ctx->last_qpair == ns_ctx->num_qpairs) {
		ns_ctx->last_qpair = 0;
	}

	if (mode != DIF_MODE_NONE) {
		rc = spdk_dif_ctx_init(&task->dif_ctx, entry->block_size, entry->md_size,
				       entry->md_interleave, entry->pi_loc,
				       (enum spdk_dif_type)entry->pi_type, entry->io_flags,
				       lba, 0xFFFF, (uint16_t)entry->io_size_blocks, 0, 0);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed\n");
			exit(1);
		}
	}

	if (task->is_read) {
		return spdk_nvme_ns_cmd_read_with_md(entry->ns, ns_ctx->qpair[qp_num],
						     task->iov.iov_base, task->md_iov.iov_base,
						     lba,
						     entry->io_size_blocks, io_complete,
						     task, entry->io_flags,
						     task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
	} else {
		switch (mode) {
		case DIF_MODE_DIF:
			rc = spdk_dif_generate(&task->iov, 1, entry->io_size_blocks, &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIF failed\n");
				return rc;
			}
			break;
		case DIF_MODE_DIX:
			rc = spdk_dix_generate(&task->iov, 1, &task->md_iov, entry->io_size_blocks,
					       &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIX failed\n");
				return rc;
			}
			break;
		default:
			break;
		}

		return spdk_nvme_ns_cmd_write_with_md(entry->ns, ns_ctx->qpair[qp_num],
						      task->iov.iov_base, task->md_iov.iov_base,
						      lba,
						      entry->io_size_blocks, io_complete,
						      task, entry->io_flags,
						      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
	}
}

static void
nvme_check_io(struct ns_worker_ctx *ns_ctx)
{
	int i, rc;

	for (i = 0; i < ns_ctx->num_qpairs; i++) {
		rc = spdk_nvme_qpair_process_completions(ns_ctx->qpair[i], g_max_completions);
		if (spdk_unlikely(rc < 0)) {
			/* This means the qpair failed in the driver and must be reconnected or destroyed. */
			if (rc == -ENXIO) {
				ns_ctx->failed_qpair[i] = true;
			} else {
				fprintf(stderr, "Received an unknown error processing completions.\n");
				exit(1);
			}
		}

		/* This qpair failed at some point in the past. We need to recover it. */
		if (spdk_unlikely(ns_ctx->failed_qpair[i])) {
			rc = spdk_nvme_ctrlr_reconnect_io_qpair(ns_ctx->qpair[i]);
			/* successful reconnect */
			if (rc == 0) {
				ns_ctx->failed_qpair[i] = false;
			} else if (rc == -ENXIO) {
				/* This means the controller is failed. Defer to it to restore the qpair. */
				continue;
			} else {
				/*
				 * We were unable to restore the qpair on this attempt. We don't
				 * really know why. For naive handling, just keep trying.
				 * TODO: add a retry limit, and destroy the qpair after x iterations.
				 */
				fprintf(stderr, "qpair failed and we were unable to recover it.\n");
			}
		}
	}
}

static void
nvme_verify_io(struct perf_task *task, struct ns_entry *entry)
{
	struct spdk_dif_error err_blk = {};
	int rc;

	if (!task->is_read || (entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		return;
	}

	if (entry->md_interleave) {
		rc = spdk_dif_verify(&task->iov, 1, entry->io_size_blocks, &task->dif_ctx,
				     &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIF error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	} else {
		rc = spdk_dix_verify(&task->iov, 1, &task->md_iov, entry->io_size_blocks,
				     &task->dif_ctx, &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIX error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	}
}

/*
 * TODO: If a controller has multiple namespaces, they could all use the same queue.
 *  For now, give each namespace/thread combination its own queue.
 */
static int
nvme_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_entry *entry = ns_ctx->entry;
	int i;

	ns_ctx->num_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->qpair = calloc(ns_ctx->num_qpairs, sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->qpair) {
		return -1;
	}
	ns_ctx->failed_qpair = calloc(ns_ctx->num_qpairs, sizeof(bool));
	if (!ns_ctx->failed_qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->ctrlr, &opts, sizeof(opts));
	if (opts.io_queue_requests < entry->num_io_requests) {
		opts.io_queue_requests = entry->num_io_requests;
	}
	opts.delay_pcie_doorbell = true;

	for (i = 0; i < ns_ctx->num_qpairs; i++) {
		ns_ctx->qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(entry->ctrlr, &opts,
				   sizeof(opts));
		if (!ns_ctx->qpair[i]) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			return -1;
		}
	}

	return 0;
}

static void
nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->num_qpairs; i++) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->qpair[i]);
	}

	free(ns_ctx->qpair);
	free(ns_ctx->failed_qpair);
}

static void
build_nvme_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport_id *trid;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		snprintf(name, length, "PCIE (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_RDMA:
		snprintf(name, length, "RDMA (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		snprintf(name, length, "TCP  (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		break;
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < g_io_size_bytes || sector_size > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       ns_size, spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (g_io_size_bytes - 1) / max_xfer_size + 2;
	if ((g_queue_depth * entries) > opts.io_queue_size) {
		printf("controller IO queue size %u less than required\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or small IO size because "
		       "IO requests may be queued at the NVMe driver.\n");
		g_warn = true;
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->num_io_requests = g_queue_depth * entries;

	entry->size_in_ios = ns_size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / sector_size;

	entry->block_size = spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size = spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave = spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc = spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type = spdk_nvme_ns_get_pi_type(ns);

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		entry->io_flags = g_metacfg_pract_flag | g_metacfg_prchk_flags;
	}

	/* If metadata size = 8 bytes, PI is stripped (read) or inserted (write),
	 *  and so reduce metadata size from block size.  (If metadata size > 8 bytes,
	 *  PI is passed (read) or replaced (write).  So block size is not necessary
	 *  to change.)
	 */
	if ((entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT) && (entry->md_size == 8)) {
		entry->block_size = spdk_nvme_ns_get_sector_size(ns);
	}

	if (g_max_io_md_size < entry->md_size) {
		g_max_io_md_size = entry->md_size;
	}

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry = g_namespaces;

	while (entry) {
		struct ns_entry *next = entry->next;
		free(entry);
		entry = next;
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = calloc(1, sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	entry->next = g_controllers;
	g_controllers = entry;

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			register_ns(ctrlr, ns);
		}
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		if (!ns) {
			perror("Namespace does not exist.");
			exit(1);
		}

		register_ns(ctrlr, ns);
	}

	if (g_nr_unused_io_queues) {
		int i;

		printf("Creating %u unused qpairs for controller %s\n", g_nr_unused_io_queues, entry->name);

		entry->unused_qpairs = calloc(g_nr_unused_io_queues, sizeof(struct spdk_nvme_qpair *));
		if (!entry->unused_qpairs) {
			fprintf(stderr, "Unable to allocate memory for qpair array\n");
			exit(1);
		}

		for (i = 0; i < g_nr_unused_io_queues; i++) {
			entry->unused_qpairs[i] = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
			if (!entry->unused_qpairs[i]) {
				fprintf(stderr, "Unable to allocate unused qpair. Did you request too many?\n");
				exit(1);
			}
		}
	}

}

static __thread unsigned int seed = 0;

static inline void
submit_single_io(struct perf_task *task)
{
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ns_entry		*entry = ns_ctx->entry;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	task->submit_tsc = spdk_get_ticks();

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		task->is_read = true;
	} else {
		task->is_read = false;
	}

	rc = nvme_submit_io(task, ns_ctx, entry, offset_in_ios);

	if (spdk_unlikely(rc != 0)) {
		fprintf(stderr, "starting I/O failed\n");
	} else {
		ns_ctx->current_queue_depth++;
	}
}

static inline void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;
	uint64_t		tsc_diff;
	struct ns_entry		*entry;

	ns_ctx = task->ns_ctx;
	entry = ns_ctx->entry;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;
	tsc_diff = spdk_get_ticks() - task->submit_tsc;
	ns_ctx->total_tsc += tsc_diff;
	if (spdk_unlikely(ns_ctx->min_tsc > tsc_diff)) {
		ns_ctx->min_tsc = tsc_diff;
	}
	if (spdk_unlikely(ns_ctx->max_tsc < tsc_diff)) {
		ns_ctx->max_tsc = tsc_diff;
	}

	if (spdk_unlikely(entry->md_size > 0)) {
		/* add application level verification for end-to-end data protection */
		nvme_verify_io(task, entry);
	}

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (spdk_unlikely(ns_ctx->is_draining)) {
		spdk_dma_free(task->iov.iov_base);
		spdk_dma_free(task->md_iov.iov_base);
		free(task);
	} else {
		submit_single_io(task);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		fprintf(stderr, "%s completed with error (sct=%d, sc=%d)\n",
			task->is_read ? "Read" : "Write",
			cpl->status.sct, cpl->status.sc);
	}

	task_complete(task);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
	nvme_check_io(ns_ctx);
}

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	nvme_setup_payload(task, queue_depth % 8 + 1);

	task->ns_ctx = ns_ctx;

	return task;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = allocate_task(ns_ctx, queue_depth);
		submit_single_io(task);
	}
}

static int
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	return nvme_init_ns_worker_ctx(ns_ctx);
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	nvme_cleanup_ns_worker_ctx(ns_ctx);
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	uint32_t unfinished_ns_ctx;

	printf("Starting thread on core %u\n", worker->lcore);

	/* Allocate queue pairs for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			return 1;
		}
		ns_ctx = ns_ctx->next;
	}

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	/* Submit initial I/O for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		submit_io(ns_ctx, g_queue_depth);
		ns_ctx = ns_ctx->next;
	}

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			check_io(ns_ctx);
			ns_ctx = ns_ctx->next;
		}

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	/* drain the io of each ns_ctx in round robin to make the fairness */
	do {
		unfinished_ns_ctx = 0;
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			/* first time will enter into this if case */
			if (!ns_ctx->is_draining) {
				ns_ctx->is_draining = true;
			}

			if (ns_ctx->current_queue_depth > 0) {
				check_io(ns_ctx);
				if (ns_ctx->current_queue_depth == 0) {
					cleanup_ns_worker_ctx(ns_ctx);
				} else {
					unfinished_ns_ctx++;
				}
			}
			ns_ctx = ns_ctx->next;
		}
	} while (unfinished_ns_ctx > 0);

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-o io size in bytes]\n");
	printf("\t[-n number of io queues per namespace. default: 1]\n");
	printf("\t[-U number of unused io queues per controller. default: 0]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)\n");
	printf("\t[-D disable submission queue in controller memory buffer, default: enabled]\n");
	printf("\t[-H enable header digest for TCP transport, default: disabled]\n");
	printf("\t[-I enable data digest for TCP transport, default: disabled]\n");
	printf("\t[-N no shutdown notification process for controllers, default: disabled]\n");
	printf("\t[-r Transport ID for local PCIe NVMe or NVMeoF]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (e.g. PCIe, RDMA)\n");
	printf("\t  adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("\t  traddr      Transport address (e.g. 0000:04:00.0 for PCIe or 192.168.100.8 for RDMA)\n");
	printf("\t  trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("\t  subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("\t Example: -r 'trtype:PCIe traddr:0000:04:00.0' for PCIe or\n");
	printf("\t          -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420' for NVMeoF\n");
	printf("\t[-e metadata configuration]\n");
	printf("\t Keys:\n");
	printf("\t  PRACT      Protection Information Action bit (PRACT=1 or PRACT=0)\n");
	printf("\t  PRCHK      Control of Protection Information Checking (PRCHK=GUARD|REFTAG|APPTAG)\n");
	printf("\t Example: -e 'PRACT=0,PRCHK=GUARD|REFTAG|APPTAG'\n");
	printf("\t          -e 'PRACT=1,PRCHK=GUARD'\n");
	printf("\t[-k keep alive timeout period in millisecond]\n");
	printf("\t[-s DPDK huge memory size in MB.]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
	printf("\t[-V enable VMD enumeration]\n");
#ifdef DEBUG
	printf("\t[-G enable debug logging]\n");
#else
	printf("\t[-G enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
}

static void
unregister_trids(void)
{
	struct trid_entry *trid_entry, *tmp;

	TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid_entry, tailq);
		free(trid_entry);
	}
}

static int
add_trid(const char *trid_str)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;
	char *ns;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
		return -1;
	}

	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}

	ns = strcasestr(trid_str, "ns:");
	if (ns) {
		char nsid_str[6]; /* 5 digits maximum in an nsid */
		int len;
		int nsid;

		ns += 3;

		len = strcspn(ns, " \t\n");
		if (len > 5) {
			fprintf(stderr, "NVMe namespace IDs must be 5 digits or less\n");
			free(trid_entry);
			return 1;
		}

		memcpy(nsid_str, ns, len);
		nsid_str[len] = '\0';

		nsid = spdk_strtol(nsid_str, 10);
		if (nsid <= 0 || nsid > 65535) {
			fprintf(stderr, "NVMe namespace IDs must be less than 65536 and greater than 0\n");
			free(trid_entry);
			return 1;
		}

		trid_entry->nsid = (uint16_t)nsid;
	}

	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
	return 0;
}

static size_t
parse_next_key(const char **str, char *key, char *val, size_t key_buf_size,
	       size_t val_buf_size)
{
	const char *sep;
	const char *separator = ", \t\n";
	size_t key_len, val_len;

	*str += strspn(*str, separator);

	sep = strchr(*str, '=');
	if (!sep) {
		fprintf(stderr, "Key without '=' separator\n");
		return 0;
	}

	key_len = sep - *str;
	if (key_len >= key_buf_size) {
		fprintf(stderr, "Key length %zu is greater than maximum allowed %zu\n",
			key_len, key_buf_size - 1);
		return 0;
	}

	memcpy(key, *str, key_len);
	key[key_len] = '\0';

	*str += key_len + 1;	/* Skip key */
	val_len = strcspn(*str, separator);
	if (val_len == 0) {
		fprintf(stderr, "Key without value\n");
		return 0;
	}

	if (val_len >= val_buf_size) {
		fprintf(stderr, "Value length %zu is greater than maximum allowed %zu\n",
			val_len, val_buf_size - 1);
		return 0;
	}

	memcpy(val, *str, val_len);
	val[val_len] = '\0';

	*str += val_len;

	return val_len;
}

static int
parse_metadata(const char *metacfg_str)
{
	const char *str;
	size_t val_len;
	char key[32];
	char val[1024];

	if (metacfg_str == NULL) {
		return -EINVAL;
	}

	str = metacfg_str;

	while (*str != '\0') {
		val_len = parse_next_key(&str, key, val, sizeof(key), sizeof(val));
		if (val_len == 0) {
			fprintf(stderr, "Failed to parse metadata\n");
			return -EINVAL;
		}

		if (strcmp(key, "PRACT") == 0) {
			if (*val == '1') {
				g_metacfg_pract_flag = SPDK_NVME_IO_FLAGS_PRACT;
			}
		} else if (strcmp(key, "PRCHK") == 0) {
			if (strstr(val, "GUARD") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
			}
			if (strstr(val, "REFTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
			}
			if (strstr(val, "APPTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;
			}
		} else {
			fprintf(stderr, "Unknown key '%s'\n", key);
		}
	}

	return 0;
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;
	long int val;
	int rc;

	/* default value */
	g_queue_depth = 0;
	g_io_size_bytes = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	g_rw_percentage = -1;
	g_core_mask = NULL;
	g_max_completions = 0;

	while ((op = getopt(argc, argv, "c:e:i:m:n:o:q:r:k:s:t:w:DGHIM:NT:U:V")) != -1) {
		switch (op) {
		case 'i':
		case 'm':
		case 'n':
		case 'o':
		case 'q':
		case 'k':
		case 's':
		case 't':
		case 'M':
		case 'U':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case 'i':
				g_shm_id = val;
				break;
			case 'm':
				g_max_completions = val;
				break;
			case 'n':
				g_nr_io_queues_per_ns = val;
				break;
			case 'o':
				g_io_size_bytes = val;
				break;
			case 'q':
				g_queue_depth = val;
				break;
			case 'k':
				g_keep_alive_timeout_in_ms = val;
				break;
			case 's':
				g_dpdk_mem = val;
				break;
			case 't':
				g_time_in_sec = val;
				break;
			case 'M':
				g_rw_percentage = val;
				mix_specified = true;
				break;
			case 'U':
				g_nr_unused_io_queues = val;
				break;
			}
			break;
		case 'c':
			g_core_mask = optarg;
			break;
		case 'e':
			if (parse_metadata(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'r':
			if (add_trid(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'D':
			g_disable_sq_cmb = 1;
			break;
		case 'G':
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
		case 'H':
			g_header_digest = 1;
			break;
		case 'I':
			g_data_digest = 1;
			break;
		case 'N':
			g_no_shn_notification = true;
			break;
		case 'T':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -T flag.\n",
				argv[0]);
			usage(argv[0]);
			return 0;
#endif
			break;
		case 'V':
			g_vmd = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_nr_io_queues_per_ns) {
		usage(argv[0]);
		return 1;
	}

	if (!g_queue_depth) {
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}
	if (!workload_type) {
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		if (mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		add_trid("trtype:PCIe");
	} else {
		struct trid_entry *trid_entry, *trid_entry_tmp;

		g_no_pci = true;
		/* check whether there is local PCIe type */
		TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, trid_entry_tmp) {
			if (trid_entry->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
				g_no_pci = false;
				break;
			}
		}
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	g_workers = NULL;
	g_num_workers = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		worker->lcore = i;
		worker->next = g_workers;
		g_workers = worker;
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker = g_workers;

	/* Free namespace context and worker thread */
	while (worker) {
		struct worker_thread *next_worker = worker->next;
		struct ns_worker_ctx *ns_ctx = worker->ns_ctx;

		while (ns_ctx) {
			struct ns_worker_ctx *next_ns_ctx = ns_ctx->next;
			free(ns_ctx);
			ns_ctx = next_ns_ctx;
		}

		free(worker);
		worker = next_worker;
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}

		printf("Attaching to NVMe Controller at %s\n",
		       trid->traddr);
	}

	/* Set io_queue_size to UINT16_MAX, NVMe driver
	 * will then reduce this to MQES to maximize
	 * the io_queue_size as much as possible.
	 */
	opts->io_queue_size = UINT16_MAX;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = spdk_max(opts->keep_alive_timeout_ms,
					       g_keep_alive_timeout_in_ms);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry	*trid_entry = cb_ctx;
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

	register_ctrlr(ctrlr, trid_entry);
}

static int
register_controllers(void)
{
	struct trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->trid, trid_entry, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;

	while (entry) {
		struct ctrlr_entry *next = entry->next;

		if (g_nr_unused_io_queues) {
			int i;

			for (i = 0; i < g_nr_unused_io_queues; i++) {
				spdk_nvme_ctrlr_free_io_qpair(entry->unused_qpairs[i]);
			}

			free(entry->unused_qpairs);
		}

		spdk_nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = g_namespaces;
	struct worker_thread	*worker = g_workers;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->min_tsc = UINT64_MAX;
		ns_ctx->entry = entry;
		ns_ctx->next = worker->ns_ctx;
		worker->ns_ctx = ns_ctx;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		entry = entry->next;
		if (entry == NULL) {
			entry = g_namespaces;
		}

	}

	return 0;
}

static void *
nvme_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	spdk_unaffinitize_thread();

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		entry = g_controllers;
		while (entry) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				/* This controller has encountered a failure at the transport level. reset it. */
				if (rc == -ENXIO) {
					fprintf(stderr, "A controller has encountered a failure and is being reset.\n");
					rc = spdk_nvme_ctrlr_reset(entry->ctrlr);
					if (rc != 0) {
						entry->num_resets++;
						fprintf(stderr, "Unable to reset the controller.\n");

						if (entry->num_resets > g_max_ctrlr_resets) {
							fprintf(stderr, "Controller cannot be recovered. Exiting.\n");
							exit(1);
						}
					} else {
						fprintf(stderr, "Controller properly reset.\n");
					}
				}
			}
			entry = entry->next;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *master_worker;
	unsigned master_core;
	struct spdk_env_opts opts;
	pthread_t thread_id = 0;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "reconnect";
	opts.shm_id = g_shm_id;
	if (g_core_mask) {
		opts.core_mask = g_core_mask;
	}

	if (g_dpdk_mem) {
		opts.mem_size = g_dpdk_mem;
	}
	if (g_no_pci) {
		opts.no_pci = g_no_pci;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		rc = -1;
		goto cleanup;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr, "No valid NVMe controllers found\n");
		goto cleanup;
	}

	rc = pthread_create(&thread_id, NULL, &nvme_poll_ctrlrs, NULL);
	if (rc != 0) {
		fprintf(stderr, "Unable to spawn a thread to poll admin queues.\n");
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	master_core = spdk_env_get_current_core();
	master_worker = NULL;
	worker = g_workers;
	while (worker != NULL) {
		if (worker->lcore != master_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
		worker = worker->next;
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);

	spdk_env_thread_wait_all();

cleanup:
	if (thread_id && pthread_cancel(thread_id) == 0) {
		pthread_join(thread_id, NULL);
	}
	unregister_trids();
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
