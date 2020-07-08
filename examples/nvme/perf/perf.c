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
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/sock.h"

#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif

#if HAVE_LIBAIO
#include <libaio.h>
#endif

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	enum spdk_nvme_transport_type		trtype;
	struct spdk_nvme_intel_rw_latency_page	*latency_page;

	struct spdk_nvme_qpair			**unused_qpairs;

	TAILQ_ENTRY(ctrlr_entry)		link;
	char					name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
	ENTRY_TYPE_URING_FILE,
};

struct ns_fn_table;

struct ns_entry {
	enum entry_type		type;
	const struct ns_fn_table	*fn_table;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns	*ns;
		} nvme;
#ifdef SPDK_CONFIG_URING
		struct {
			int			fd;
		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			int                     fd;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_entry)	link;
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

static const double g_latency_cutoffs[] = {
	0.01,
	0.10,
	0.25,
	0.50,
	0.75,
	0.90,
	0.95,
	0.98,
	0.99,
	0.995,
	0.999,
	0.9999,
	0.99999,
	0.999999,
	0.9999999,
	-1,
};

struct ns_worker_stats {
	uint64_t		io_completed;
	uint64_t		last_io_completed;
	uint64_t		total_tsc;
	uint64_t		min_tsc;
	uint64_t		max_tsc;
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	struct ns_worker_stats	stats;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

	union {
		struct {
			int				num_active_qpairs;
			int				num_all_qpairs;
			struct spdk_nvme_qpair		**qpair;
			struct spdk_nvme_poll_group	*group;
			int				last_qpair;
		} nvme;

#ifdef SPDK_CONFIG_URING
		struct {
			struct io_uring		ring;
			uint64_t		io_inflight;
			uint64_t		io_pending;
			struct io_uring_cqe	**cqes;

		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_worker_ctx)	link;

	struct spdk_histogram_data	*histogram;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	struct iovec		iov;
	struct iovec		md_iov;
	uint64_t		submit_tsc;
	bool			is_read;
	struct spdk_dif_ctx	dif_ctx;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)	ns_ctx;
	TAILQ_ENTRY(worker_thread)	link;
	unsigned			lcore;
};

struct ns_fn_table {
	void	(*setup_payload)(struct perf_task *task, uint8_t pattern);

	int	(*submit_io)(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
			     struct ns_entry *entry, uint64_t offset_in_ios);

	void	(*check_io)(struct ns_worker_ctx *ns_ctx);

	void	(*verify_io)(struct perf_task *task, struct ns_entry *entry);

	int	(*init_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);

	void	(*cleanup_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
};

static int g_outstanding_commands;

static bool g_latency_ssd_tracking_enable;
static int g_latency_sw_tracking_level;

static bool g_vmd;
static const char *g_workload_type;
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static int g_num_namespaces;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static int g_num_workers = 0;
static uint32_t g_master_core;

static uint64_t g_tsc_rate;

static uint32_t g_io_align = 0x200;
static bool g_io_align_specified;
static uint32_t g_io_size_bytes;
static uint32_t g_max_io_md_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_metacfg_pract_flag;
static uint32_t g_metacfg_prchk_flags;
static int g_rw_percentage = -1;
static int g_is_random;
static int g_queue_depth;
static int g_nr_io_queues_per_ns = 1;
static int g_nr_unused_io_queues;
static int g_time_in_sec;
static int g_warmup_time_in_sec;
static uint32_t g_max_completions;
static int g_dpdk_mem;
static bool g_dpdk_mem_single_seg = false;
static int g_shm_id = -1;
static uint32_t g_disable_sq_cmb;
static bool g_use_uring;
static bool g_no_pci;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification;
static bool g_mix_specified;
/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
/* Default to 10 seconds for the keep alive value. This value is arbitrary. */
static uint32_t g_keep_alive_timeout_in_ms = 10000;

static const char *g_core_mask;

#define MAX_ALLOWED_PCI_DEVICE_NUM 128
static struct spdk_pci_addr g_allowed_pci_addr[MAX_ALLOWED_PCI_DEVICE_NUM];
static uint32_t g_allowed_pci_addr_num;

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	uint16_t			nsid;
	TAILQ_ENTRY(trid_entry)		tailq;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static int g_file_optind; /* Index of first filename in argv */

static inline void
task_complete(struct perf_task *task);

static void
perf_set_sock_zcopy(const char *impl_name, bool enable)
{
	struct spdk_sock_impl_opts sock_opts = {};
	size_t opts_size = sizeof(sock_opts);
	int rc;

	rc = spdk_sock_impl_get_opts(impl_name, &sock_opts, &opts_size);
	if (rc != 0) {
		if (errno == EINVAL) {
			fprintf(stderr, "Unknown sock impl %s\n", impl_name);
		} else {
			fprintf(stderr, "Failed to get opts for sock impl %s: error %d (%s)\n", impl_name, errno,
				strerror(errno));
		}
		return;
	}

	if (opts_size != sizeof(sock_opts)) {
		fprintf(stderr, "Warning: sock_opts size mismatch. Expected %zu, received %zu\n",
			sizeof(sock_opts), opts_size);
		opts_size = sizeof(sock_opts);
	}

	sock_opts.enable_zerocopy_send = enable;

	if (spdk_sock_impl_set_opts(impl_name, &sock_opts, opts_size)) {
		fprintf(stderr, "Failed to %s zcopy send for sock impl %s: error %d (%s)\n",
			enable ? "enable" : "disable", impl_name, errno, strerror(errno));
	}
}

#ifdef SPDK_CONFIG_URING

static void
uring_setup_payload(struct perf_task *task, uint8_t pattern)
{
	task->iov.iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	task->iov.iov_len = g_io_size_bytes;
	if (task->iov.iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->iov.iov_base failed\n");
		exit(1);
	}
	memset(task->iov.iov_base, pattern, task->iov.iov_len);
}

static int
uring_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
		struct ns_entry *entry, uint64_t offset_in_ios)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ns_ctx->u.uring.ring);
	if (!sqe) {
		fprintf(stderr, "Cannot get sqe\n");
		return -1;
	}

	if (task->is_read) {
		io_uring_prep_readv(sqe, entry->u.uring.fd, &task->iov, 1, offset_in_ios * task->iov.iov_len);
	} else {
		io_uring_prep_writev(sqe, entry->u.uring.fd, &task->iov, 1, offset_in_ios * task->iov.iov_len);
	}

	io_uring_sqe_set_data(sqe, task);
	ns_ctx->u.uring.io_pending++;

	return 0;
}

static void
uring_check_io(struct ns_worker_ctx *ns_ctx)
{
	int i, count, to_complete, to_submit, ret = 0;
	struct perf_task *task;

	to_submit = ns_ctx->u.uring.io_pending;

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&ns_ctx->u.uring.ring);
		if (ret < 0) {
			return;
		}
		ns_ctx->u.uring.io_pending = 0;
		ns_ctx->u.uring.io_inflight += to_submit;
	}

	to_complete = ns_ctx->u.uring.io_inflight;
	if (to_complete > 0) {
		count = io_uring_peek_batch_cqe(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes, to_complete);
		ns_ctx->u.uring.io_inflight -= count;
		for (i = 0; i < count; i++) {
			assert(ns_ctx->u.uring.cqes[i] != NULL);
			task = (struct perf_task *)ns_ctx->u.uring.cqes[i]->user_data;
			if (ns_ctx->u.uring.cqes[i]->res != (int)task->iov.iov_len) {
				fprintf(stderr, "cqe[i]->status=%d\n", ns_ctx->u.uring.cqes[i]->res);
				exit(0);
			}
			io_uring_cqe_seen(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes[i]);
			task_complete(task);
		}
	}
}

static void
uring_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
uring_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (io_uring_queue_init(g_queue_depth, &ns_ctx->u.uring.ring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ns_ctx->u.uring.cqes = calloc(g_queue_depth, sizeof(struct io_uring_cqe *));
	if (!ns_ctx->u.uring.cqes) {
		io_uring_queue_exit(&ns_ctx->u.uring.ring);
		return -1;
	}

	return 0;
}

static void
uring_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_uring_queue_exit(&ns_ctx->u.uring.ring);
	free(ns_ctx->u.uring.cqes);
}

static const struct ns_fn_table uring_fn_table = {
	.setup_payload          = uring_setup_payload,
	.submit_io              = uring_submit_io,
	.check_io               = uring_check_io,
	.verify_io              = uring_verify_io,
	.init_ns_worker_ctx     = uring_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx  = uring_cleanup_ns_worker_ctx,
};

#endif

#ifdef HAVE_LIBAIO
static void
aio_setup_payload(struct perf_task *task, uint8_t pattern)
{
	task->iov.iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	task->iov.iov_len = g_io_size_bytes;
	if (task->iov.iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->buf failed\n");
		exit(1);
	}
	memset(task->iov.iov_base, pattern, task->iov.iov_len);
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd,
	   struct iovec *iov, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = iov->iov_base;
	iocb->u.c.nbytes = iov->iov_len;
	iocb->u.c.offset = offset * iov->iov_len;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		printf("io_submit");
		return -1;
	}

	return 0;
}

static int
aio_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	      struct ns_entry *entry, uint64_t offset_in_ios)
{
	if (task->is_read) {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD,
				  &task->iov, offset_in_ios, task);
	} else {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE,
				  &task->iov, offset_in_ios, task);
	}
}

static void
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->u.aio.ctx, 1, g_queue_depth, ns_ctx->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		exit(1);
	}

	for (i = 0; i < count; i++) {
		task_complete(ns_ctx->u.aio.events[i].data);
	}
}

static void
aio_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
aio_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->u.aio.events = calloc(g_queue_depth, sizeof(struct io_event));
	if (!ns_ctx->u.aio.events) {
		return -1;
	}
	ns_ctx->u.aio.ctx = 0;
	if (io_setup(g_queue_depth, &ns_ctx->u.aio.ctx) < 0) {
		free(ns_ctx->u.aio.events);
		perror("io_setup");
		return -1;
	}
	return 0;
}

static void
aio_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_destroy(ns_ctx->u.aio.ctx);
	free(ns_ctx->u.aio.events);
}

static const struct ns_fn_table aio_fn_table = {
	.setup_payload		= aio_setup_payload,
	.submit_io		= aio_submit_io,
	.check_io		= aio_check_io,
	.verify_io		= aio_verify_io,
	.init_ns_worker_ctx	= aio_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= aio_cleanup_ns_worker_ctx,
};

#endif /* HAVE_LIBAIO */

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)

static int
register_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else if (g_rw_percentage == 0) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = spdk_fd_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = spdk_fd_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of device %s\n", path);
		close(fd);
		return -1;
	}

	/*
	 * TODO: This should really calculate the LCM of the current g_io_align and blklen.
	 * For now, it's fairly safe to just assume all block sizes are powers of 2.
	 */
	if (g_io_align < blklen) {
		if (g_io_align_specified) {
			fprintf(stderr, "Wrong IO alignment (%u). aio requires block-sized alignment (%u)\n", g_io_align,
				blklen);
			close(fd);
			return -1;
		}

		g_io_align = blklen;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("ns_entry malloc");
		return -1;
	}

	if (g_use_uring) {
#ifdef SPDK_CONFIG_URING
		entry->type = ENTRY_TYPE_URING_FILE;
		entry->fn_table = &uring_fn_table;
		entry->u.uring.fd = fd;
#endif
	} else {
#if HAVE_LIBAIO
		entry->type = ENTRY_TYPE_AIO_FILE;
		entry->fn_table = &aio_fn_table;
		entry->u.aio.fd = fd;
#endif
	}
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	return 0;
}

static int
register_files(int argc, char **argv)
{
	int i;

	/* Treat everything after the options as files for AIO/URING */
	for (i = g_file_optind; i < argc; i++) {
		if (register_file(argv[i]) != 0) {
			return 1;
		}
	}

	return 0;
}
#endif

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

	qp_num = ns_ctx->u.nvme.last_qpair;
	ns_ctx->u.nvme.last_qpair++;
	if (ns_ctx->u.nvme.last_qpair == ns_ctx->u.nvme.num_active_qpairs) {
		ns_ctx->u.nvme.last_qpair = 0;
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
		return spdk_nvme_ns_cmd_read_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
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

		return spdk_nvme_ns_cmd_write_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
						      task->iov.iov_base, task->md_iov.iov_base,
						      lba,
						      entry->io_size_blocks, io_complete,
						      task, entry->io_flags,
						      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
	}
}

static void
perf_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{

}

static void
nvme_check_io(struct ns_worker_ctx *ns_ctx)
{
	int64_t rc;

	rc = spdk_nvme_poll_group_process_completions(ns_ctx->u.nvme.group, 0, perf_disconnect_cb);
	if (rc < 0) {
		fprintf(stderr, "NVMe io qpair process completion error\n");
		exit(1);
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
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_qpair *qpair;
	int i;

	ns_ctx->u.nvme.num_active_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.num_all_qpairs = g_nr_io_queues_per_ns + g_nr_unused_io_queues;
	ns_ctx->u.nvme.qpair = calloc(ns_ctx->u.nvme.num_all_qpairs, sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->u.nvme.qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->u.nvme.ctrlr, &opts, sizeof(opts));
	if (opts.io_queue_requests < entry->num_io_requests) {
		opts.io_queue_requests = entry->num_io_requests;
	}
	opts.delay_cmd_submit = true;
	opts.create_only = true;

	ns_ctx->u.nvme.group = spdk_nvme_poll_group_create(NULL);
	if (ns_ctx->u.nvme.group == NULL) {
		goto poll_group_failed;
	}

	group = ns_ctx->u.nvme.group;
	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		ns_ctx->u.nvme.qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(entry->u.nvme.ctrlr, &opts,
					  sizeof(opts));
		qpair = ns_ctx->u.nvme.qpair[i];
		if (!qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto qpair_failed;
		}

		if (spdk_nvme_poll_group_add(group, qpair)) {
			printf("ERROR: unable to add I/O qpair to poll group.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}

		if (spdk_nvme_ctrlr_connect_io_qpair(entry->u.nvme.ctrlr, qpair)) {
			printf("ERROR: unable to connect I/O qpair.\n");
			spdk_nvme_poll_group_remove(group, qpair);
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}
	}

	return 0;

qpair_failed:
	for (; i > 0; --i) {
		spdk_nvme_poll_group_remove(ns_ctx->u.nvme.group, ns_ctx->u.nvme.qpair[i - 1]);
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i - 1]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
poll_group_failed:
	free(ns_ctx->u.nvme.qpair);
	return -1;
}

static void
nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		spdk_nvme_poll_group_remove(ns_ctx->u.nvme.group, ns_ctx->u.nvme.qpair[i]);
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	free(ns_ctx->u.nvme.qpair);
}

static const struct ns_fn_table nvme_fn_table = {
	.setup_payload		= nvme_setup_payload,
	.submit_io		= nvme_submit_io,
	.check_io		= nvme_check_io,
	.verify_io		= nvme_verify_io,
	.init_ns_worker_ctx	= nvme_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= nvme_cleanup_ns_worker_ctx,
};

static int
build_nvme_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport_id *trid;
	int res = 0;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		res = snprintf(name, length, "PCIE (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_RDMA:
		res = snprintf(name, length, "RDMA (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		res = snprintf(name, length, "TCP (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		res = snprintf(name, length, "CUSTOM (%s)", trid->traddr);
		break;

	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		break;
	}
	return res;
}

static void
build_nvme_ns_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	int res = 0;

	res = build_nvme_name(name, length, ctrlr);
	if (res > 0) {
		snprintf(name + res, length - res, " NSID %u", nsid);
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

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->fn_table = &nvme_fn_table;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
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

	build_nvme_ns_name(entry->name, sizeof(entry->name), ctrlr, spdk_nvme_ns_get_id(ns));

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		free(entry);
	}
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_outstanding_commands--;
}

static void
set_latency_tracking_feature(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	int res;
	union spdk_nvme_intel_feat_latency_tracking latency_tracking;

	if (enable) {
		latency_tracking.bits.enable = 0x01;
	} else {
		latency_tracking.bits.enable = 0x00;
	}

	res = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING,
					      latency_tracking.raw, 0, NULL, 0, enable_latency_tracking_complete, NULL);
	if (res) {
		printf("fail to allocate nvme request.\n");
		return;
	}
	g_outstanding_commands++;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->latency_page = spdk_dma_zmalloc(sizeof(struct spdk_nvme_intel_rw_latency_page),
					       4096, NULL);
	if (entry->latency_page == NULL) {
		printf("Allocation error (latency page)\n");
		exit(1);
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (g_latency_ssd_tracking_enable &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
		set_latency_tracking_feature(ctrlr, true);
	}

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

	rc = entry->fn_table->submit_io(task, ns_ctx, entry, offset_in_ios);

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
	ns_ctx->stats.io_completed++;
	tsc_diff = spdk_get_ticks() - task->submit_tsc;
	ns_ctx->stats.total_tsc += tsc_diff;
	if (spdk_unlikely(ns_ctx->stats.min_tsc > tsc_diff)) {
		ns_ctx->stats.min_tsc = tsc_diff;
	}
	if (spdk_unlikely(ns_ctx->stats.max_tsc < tsc_diff)) {
		ns_ctx->stats.max_tsc = tsc_diff;
	}
	if (spdk_unlikely(g_latency_sw_tracking_level > 0)) {
		spdk_histogram_data_tally(ns_ctx->histogram, tsc_diff);
	}

	if (spdk_unlikely(entry->md_size > 0)) {
		/* add application level verification for end-to-end data protection */
		entry->fn_table->verify_io(task, entry);
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

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	ns_ctx->entry->fn_table->setup_payload(task, queue_depth % 8 + 1);

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
	return ns_ctx->entry->fn_table->init_ns_worker_ctx(ns_ctx);
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

static void
print_periodic_performance(bool warmup)
{
	uint64_t io_this_second;
	double mb_this_second;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;

	if (!isatty(STDOUT_FILENO)) {
		/* Don't print periodic stats if output is not going
		 * to a terminal.
		 */
		return;
	}

	io_this_second = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			io_this_second += ns_ctx->stats.io_completed - ns_ctx->stats.last_io_completed;
			ns_ctx->stats.last_io_completed = ns_ctx->stats.io_completed;
		}
	}

	mb_this_second = (double)io_this_second * g_io_size_bytes / (1024 * 1024);
	printf("%s%9ju IOPS, %8.2f MiB/s\r", warmup ? "[warmup] " : "", io_this_second, mb_this_second);
	fflush(stdout);
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end, tsc_current, tsc_next_print;
	struct worker_thread *worker = (struct worker_thread *) arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	uint32_t unfinished_ns_ctx;
	bool warmup = false;

	/* Allocate queue pairs for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			return 1;
		}
	}

	tsc_current = spdk_get_ticks();
	tsc_next_print = tsc_current + g_tsc_rate;

	if (g_warmup_time_in_sec) {
		warmup = true;
		tsc_end = tsc_current + g_warmup_time_in_sec * g_tsc_rate;
	} else {
		tsc_end = tsc_current + g_time_in_sec * g_tsc_rate;
	}

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		submit_io(ns_ctx, g_queue_depth);
	}

	while (spdk_likely(!g_exit)) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			ns_ctx->entry->fn_table->check_io(ns_ctx);
		}

		tsc_current = spdk_get_ticks();

		if (worker->lcore == g_master_core && tsc_current > tsc_next_print) {
			tsc_next_print += g_tsc_rate;
			print_periodic_performance(warmup);
		}

		if (tsc_current > tsc_end) {
			if (warmup) {
				/* Update test end time, clear statistics */
				tsc_end = tsc_current + g_time_in_sec * g_tsc_rate;

				TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
					memset(&ns_ctx->stats, 0, sizeof(ns_ctx->stats));
					ns_ctx->stats.min_tsc = UINT64_MAX;
				}

				if (worker->lcore == g_master_core && isatty(STDOUT_FILENO)) {
					/* warmup stage prints a longer string to stdout, need to erase it */
					printf("%c[2K", 27);
				}

				warmup = false;
			} else {
				break;
			}
		}
	}

	/* drain the io of each ns_ctx in round robin to make the fairness */
	do {
		unfinished_ns_ctx = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			/* first time will enter into this if case */
			if (!ns_ctx->is_draining) {
				ns_ctx->is_draining = true;
			}

			if (ns_ctx->current_queue_depth > 0) {
				ns_ctx->entry->fn_table->check_io(ns_ctx);
				if (ns_ctx->current_queue_depth == 0) {
					cleanup_ns_worker_ctx(ns_ctx);
				} else {
					unfinished_ns_ctx++;
				}
			}
		}
	} while (unfinished_ns_ctx > 0);

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
#if defined(SPDK_CONFIG_URING) || defined(HAVE_LIBAIO)
	printf(" [Kernel device(s)]...");
#endif
	printf("\n");
	printf("\t[-b allowed local PCIe device address]\n");
	printf("\t Example: -b 0000:d8:00.0 -b 0000:d9:00.0\n");
	printf("\t[-q io depth]\n");
	printf("\t[-o io size in bytes]\n");
	printf("\t[-P number of io queues per namespace. default: 1]\n");
	printf("\t[-U number of unused io queues per controller. default: 0]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-L enable latency tracking via sw, default: disabled]\n");
	printf("\t\t-L for latency summary, -LL for detailed histogram\n");
	printf("\t[-l enable latency tracking via ssd (if supported), default: disabled]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-a warmup time in seconds]\n");
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
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-C max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
	printf("\t[-V enable VMD enumeration]\n");
	printf("\t[-z disable zero copy send for the given sock implementation. Default for posix impl]\n");
	printf("\t[-Z enable zero copy send for the given sock implementation]\n");
	printf("\t[-A IO buffer alignment. Must be power of 2 and not less than cache line (%u)]\n",
	       SPDK_CACHE_LINE_SIZE);
	printf("\t[-S set the default sock impl, e.g. \"posix\"]\n");
#ifdef SPDK_CONFIG_URING
	printf("\t[-R enable using liburing to drive kernel devices (Default: libaio)]\n");
#endif
#ifdef DEBUG
	printf("\t[-G enable debug logging]\n");
#else
	printf("\t[-G enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
}

static void
check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = ctx;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, (double)end * 1000 * 1000 / g_tsc_rate);
		(*cutoff)++;
	}
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far * 100 / total;
	printf("%9.3f - %9.3f: %9.4f%%  (%9ju)\n",
	       (double)start * 1000 * 1000 / g_tsc_rate,
	       (double)end * 1000 * 1000 / g_tsc_rate,
	       so_far_pct, count);
}

static void
print_performance(void)
{
	uint64_t total_io_completed, total_io_tsc;
	double io_per_second, mb_per_second, average_latency, min_latency, max_latency;
	double sum_ave_latency, min_latency_so_far, max_latency_so_far;
	double total_io_per_second, total_mb_per_second;
	int ns_count;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;
	uint32_t max_strlen;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	total_io_completed = 0;
	total_io_tsc = 0;
	min_latency_so_far = (double)UINT64_MAX;
	max_latency_so_far = 0;
	ns_count = 0;

	max_strlen = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			max_strlen = spdk_max(strlen(ns_ctx->entry->name), max_strlen);
		}
	}

	printf("========================================================\n");
	printf("%*s\n", max_strlen + 60, "Latency(us)");
	printf("%-*s: %10s %10s %10s %10s %10s\n",
	       max_strlen + 13, "Device Information", "IOPS", "MiB/s", "Average", "min", "max");

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (ns_ctx->stats.io_completed != 0) {
				io_per_second = (double)ns_ctx->stats.io_completed / g_time_in_sec;
				mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
				average_latency = ((double)ns_ctx->stats.total_tsc / ns_ctx->stats.io_completed) * 1000 * 1000 /
						  g_tsc_rate;
				min_latency = (double)ns_ctx->stats.min_tsc * 1000 * 1000 / g_tsc_rate;
				if (min_latency < min_latency_so_far) {
					min_latency_so_far = min_latency;
				}

				max_latency = (double)ns_ctx->stats.max_tsc * 1000 * 1000 / g_tsc_rate;
				if (max_latency > max_latency_so_far) {
					max_latency_so_far = max_latency;
				}

				printf("%-*.*s from core %2u: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
				       max_strlen, max_strlen, ns_ctx->entry->name, worker->lcore,
				       io_per_second, mb_per_second,
				       average_latency, min_latency, max_latency);
				total_io_per_second += io_per_second;
				total_mb_per_second += mb_per_second;
				total_io_completed += ns_ctx->stats.io_completed;
				total_io_tsc += ns_ctx->stats.total_tsc;
				ns_count++;
			}
		}
	}

	if (ns_count != 0 && total_io_completed) {
		sum_ave_latency = ((double)total_io_tsc / total_io_completed) * 1000 * 1000 / g_tsc_rate;
		printf("========================================================\n");
		printf("%-*s: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
		       max_strlen + 13, "Total", total_io_per_second, total_mb_per_second,
		       sum_ave_latency, min_latency_so_far, max_latency_so_far);
		printf("\n");
	}

	if (g_latency_sw_tracking_level == 0 || total_io_completed == 0) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			const double *cutoff = g_latency_cutoffs;

			printf("Summary latency data for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("=================================================================================\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, check_cutoff, &cutoff);

			printf("\n");
		}
	}

	if (g_latency_sw_tracking_level == 1) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			printf("Latency histogram for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("==============================================================================\n");
			printf("       Range in us     Cumulative    IO count\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, print_bucket, NULL);
			printf("\n");
		}
	}

}

static void
print_latency_page(struct ctrlr_entry *entry)
{
	int i;

	printf("\n");
	printf("%s\n", entry->name);
	printf("--------------------------------------------------------\n");

	for (i = 0; i < 32; i++) {
		if (entry->latency_page->buckets_32us[i]) {
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32, entry->latency_page->buckets_32us[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_1ms[i]) {
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2, entry->latency_page->buckets_1ms[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page->buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, log_page, SPDK_NVME_GLOBAL_NS_TAG,
							     ctrlr->latency_page, sizeof(struct spdk_nvme_intel_rw_latency_page), 0,
							     enable_latency_tracking_complete,
							     NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n", ctrlr->name, op_name);
		}
	}

	while (g_outstanding_commands) {
		TAILQ_FOREACH(ctrlr, &g_controllers, link) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
		}
	}

	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
	}
	printf("\n");
}

static void
print_stats(void)
{
	print_performance();
	if (g_latency_ssd_tracking_enable) {
		if (g_rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
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

	spdk_nvme_transport_id_populate_trstring(trid,
			spdk_nvme_transport_id_trtype_str(trid->trtype));

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

static int
add_allowed_pci_device(const char *bdf_str)
{
	int rc;

	if (g_allowed_pci_addr_num >= MAX_ALLOWED_PCI_DEVICE_NUM) {
		fprintf(stderr, "Currently we only support allowed PCI device num=%d\n",
			MAX_ALLOWED_PCI_DEVICE_NUM);
		return -1;
	}

	rc = spdk_pci_addr_parse(&g_allowed_pci_addr[g_allowed_pci_addr_num], bdf_str);
	if (rc < 0) {
		fprintf(stderr, "Failed to parse the given bdf_str=%s\n", bdf_str);
		return -1;
	}

	g_allowed_pci_addr_num++;
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
	int op;
	long int val;
	int rc;

	while ((op = getopt(argc, argv, "a:b:c:e:gi:lo:q:r:k:s:t:w:z:A:C:DGHILM:NP:RS:T:U:VZ:")) != -1) {
		switch (op) {
		case 'a':
		case 'A':
		case 'i':
		case 'C':
		case 'P':
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
			case 'a':
				g_warmup_time_in_sec = val;
				break;
			case 'i':
				g_shm_id = val;
				break;
			case 'C':
				g_max_completions = val;
				break;
			case 'P':
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
				g_mix_specified = true;
				break;
			case 'U':
				g_nr_unused_io_queues = val;
				break;
			case 'A':
				g_io_align = val;
				if (!spdk_u32_is_pow2(g_io_align) || g_io_align < SPDK_CACHE_LINE_SIZE) {
					fprintf(stderr, "Wrong alignment %u. Must be power of 2 and not less than cache lize (%u)\n",
						g_io_align, SPDK_CACHE_LINE_SIZE);
					usage(argv[0]);
					return 1;
				}
				g_io_align_specified = true;
				break;
			}
			break;
		case 'b':
			if (add_allowed_pci_device(optarg)) {
				usage(argv[0]);
				return 1;
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
		case 'g':
			g_dpdk_mem_single_seg = true;
			break;
		case 'l':
			g_latency_ssd_tracking_enable = true;
			break;
		case 'r':
			if (add_trid(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'w':
			g_workload_type = optarg;
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
		case 'L':
			g_latency_sw_tracking_level++;
			break;
		case 'N':
			g_no_shn_notification = true;
			break;
		case 'R':
#ifndef SPDK_CONFIG_URING
			fprintf(stderr, "%s must be rebuilt with CONFIG_URING=y for -R flag.\n",
				argv[0]);
			usage(argv[0]);
			return 0;
#endif
			g_use_uring = true;
			break;
		case 'T':
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
		case 'V':
			g_vmd = true;
			break;
		case 'z':
			perf_set_sock_zcopy(optarg, false);
			break;
		case 'Z':
			perf_set_sock_zcopy(optarg, true);
			break;
		case 'S':
			rc = spdk_sock_set_default_impl(optarg);
			if (rc) {
				fprintf(stderr, "Failed to set sock impl %s, err %d (%s)\n", optarg, errno, strerror(errno));
				return 1;
			}
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
		fprintf(stderr, "missing -q (queue size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		fprintf(stderr, "missing -o (block size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_workload_type) {
		fprintf(stderr, "missing -w (io pattern type) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		fprintf(stderr, "missing -t (test time in seconds) operand\n");
		usage(argv[0]);
		return 1;
	}

	if (strncmp(g_workload_type, "rand", 4) == 0) {
		g_is_random = 1;
		g_workload_type = &g_workload_type[4];
	}

	if (strcmp(g_workload_type, "read") == 0 || strcmp(g_workload_type, "write") == 0) {
		g_rw_percentage = strcmp(g_workload_type, "read") == 0 ? 100 : 0;
		if (g_mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	} else if (strcmp(g_workload_type, "rw") == 0) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	} else {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
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

	g_file_optind = optind;

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			spdk_histogram_data_free(ns_ctx->histogram);
			free(ns_ctx);
		}

		free(worker);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}
	}

	/* Set io_queue_size to UINT16_MAX, NVMe driver
	 * will then reduce this to MQES to maximize
	 * the io_queue_size as much as possible.
	 */
	opts->io_queue_size = UINT16_MAX;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = g_keep_alive_timeout_in_ms;

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
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);

		spdk_dma_free(entry->latency_page);
		if (g_latency_ssd_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
			set_latency_tracking_feature(entry->ctrlr, false);
		}

		if (g_nr_unused_io_queues) {
			int i;

			for (i = 0; i < g_nr_unused_io_queues; i++) {
				spdk_nvme_ctrlr_free_io_qpair(entry->unused_qpairs[i]);
			}

			free(entry->unused_qpairs);
		}

		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

	if (g_vmd) {
		spdk_vmd_fini();
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
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
		ns_ctx->stats.min_tsc = UINT64_MAX;
		ns_ctx->entry = entry;
		ns_ctx->histogram = spdk_histogram_data_alloc();
		TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
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

		TAILQ_FOREACH(entry, &g_controllers, link) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit)) {
					g_exit = true;
				}
			}
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
	struct spdk_env_opts opts;
	pthread_t thread_id = 0;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "perf";
	opts.shm_id = g_shm_id;
	if (g_core_mask) {
		opts.core_mask = g_core_mask;
	}

	if (g_dpdk_mem) {
		opts.mem_size = g_dpdk_mem;
	}
	opts.hugepage_single_segments = g_dpdk_mem_single_seg;
	if (g_no_pci) {
		opts.no_pci = g_no_pci;
	}

	if (g_allowed_pci_addr_num) {
		opts.pci_whitelist = g_allowed_pci_addr;
		opts.num_pci_addr = g_allowed_pci_addr_num;
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

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)
	if (register_files(argc, argv) != 0) {
		rc = -1;
		goto cleanup;
	}
#endif

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr, "No valid NVMe controllers or AIO or URING devices found\n");
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
	g_master_core = spdk_env_get_current_core();
	master_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != g_master_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);

	spdk_env_thread_wait_all();

	print_stats();

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
