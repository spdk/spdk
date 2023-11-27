/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/accel.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/xor.h"
#include "spdk/dif.h"

#define DATA_PATTERN 0x5a
#define ALIGN_4K 0x1000
#define COMP_BUF_PAD_PERCENTAGE 1.1L

static uint64_t	g_tsc_rate;
static uint64_t g_tsc_end;
static int g_rc;
static int g_xfer_size_bytes = 4096;
static int g_block_size_bytes = 512;
static int g_md_size_bytes = 8;
static int g_queue_depth = 32;
/* g_allocate_depth indicates how many tasks we allocate per worker. It will
 * be at least as much as the queue depth.
 */
static int g_allocate_depth = 0;
static int g_threads_per_core = 1;
static int g_time_in_sec = 5;
static uint32_t g_crc32c_seed = 0;
static uint32_t g_chained_count = 1;
static int g_fail_percent_goal = 0;
static uint8_t g_fill_pattern = 255;
static uint32_t g_xor_src_count = 2;
static bool g_verify = false;
static const char *g_workload_type = NULL;
static enum spdk_accel_opcode g_workload_selection = SPDK_ACCEL_OPC_LAST;
static const char *g_module_name = NULL;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;
static char *g_cd_file_in_name = NULL;
static pthread_mutex_t g_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct spdk_app_opts g_opts = {};

struct ap_compress_seg {
	void		*uncompressed_data;
	uint32_t	uncompressed_len;
	struct iovec	*uncompressed_iovs;
	uint32_t	uncompressed_iovcnt;

	void		*compressed_data;
	uint32_t	compressed_len;
	uint32_t	compressed_len_padded;
	struct iovec	*compressed_iovs;
	uint32_t	compressed_iovcnt;

	STAILQ_ENTRY(ap_compress_seg)	link;
};

static STAILQ_HEAD(, ap_compress_seg) g_compress_segs = STAILQ_HEAD_INITIALIZER(g_compress_segs);

struct worker_thread;
static void accel_done(void *ref, int status);

struct display_info {
	int core;
	int thread;
};

struct ap_task {
	void			*src;
	struct iovec		*src_iovs;
	uint32_t		src_iovcnt;
	void			**sources;
	struct iovec		*dst_iovs;
	uint32_t		dst_iovcnt;
	void			*dst;
	void			*dst2;
	uint32_t		crc_dst;
	uint32_t		compressed_sz;
	struct ap_compress_seg *cur_seg;
	struct worker_thread	*worker;
	int			expected_status; /* used for the compare operation */
	uint32_t		num_blocks; /* used for the DIF related operations */
	struct spdk_dif_ctx	dif_ctx;
	struct spdk_dif_error	dif_err;
	TAILQ_ENTRY(ap_task)	link;
};

struct worker_thread {
	struct spdk_io_channel		*ch;
	struct spdk_accel_opcode_stats	stats;
	uint64_t			xfer_failed;
	uint64_t			injected_miscompares;
	uint64_t			current_queue_depth;
	TAILQ_HEAD(, ap_task)		tasks_pool;
	struct worker_thread		*next;
	unsigned			core;
	struct spdk_thread		*thread;
	bool				is_draining;
	struct spdk_poller		*is_draining_poller;
	struct spdk_poller		*stop_poller;
	void				*task_base;
	struct display_info		display;
	enum spdk_accel_opcode		workload;
};

static void
dump_user_config(void)
{
	const char *module_name = NULL;
	int rc;

	rc = spdk_accel_get_opc_module_name(g_workload_selection, &module_name);
	if (rc) {
		printf("error getting module name (%d)\n", rc);
	}

	printf("\nSPDK Configuration:\n");
	printf("Core mask:      %s\n\n", g_opts.reactor_mask);
	printf("Accel Perf Configuration:\n");
	printf("Workload Type:  %s\n", g_workload_type);
	if (g_workload_selection == SPDK_ACCEL_OPC_CRC32C ||
	    g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C) {
		printf("CRC-32C seed:   %u\n", g_crc32c_seed);
	} else if (g_workload_selection == SPDK_ACCEL_OPC_FILL) {
		printf("Fill pattern:   0x%x\n", g_fill_pattern);
	} else if ((g_workload_selection == SPDK_ACCEL_OPC_COMPARE) && g_fail_percent_goal > 0) {
		printf("Failure inject: %u percent\n", g_fail_percent_goal);
	} else if (g_workload_selection == SPDK_ACCEL_OPC_XOR) {
		printf("Source buffers: %u\n", g_xor_src_count);
	}
	if (g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		printf("Vector size:    %u bytes\n", g_xfer_size_bytes);
		printf("Transfer size:  %u bytes\n", g_xfer_size_bytes * g_chained_count);
	} else {
		printf("Transfer size:  %u bytes\n", g_xfer_size_bytes);
	}
	if (g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY) {
		printf("Block size:     %u bytes\n", g_block_size_bytes);
		printf("Metadata size:  %u bytes\n", g_md_size_bytes);
	}
	printf("Vector count    %u\n", g_chained_count);
	printf("Module:         %s\n", module_name);
	if (g_workload_selection == SPDK_ACCEL_OPC_COMPRESS ||
	    g_workload_selection == SPDK_ACCEL_OPC_DECOMPRESS) {
		printf("File Name:      %s\n", g_cd_file_in_name);
	}
	printf("Queue depth:    %u\n", g_queue_depth);
	printf("Allocate depth: %u\n", g_allocate_depth);
	printf("# threads/core: %u\n", g_threads_per_core);
	printf("Run time:       %u seconds\n", g_time_in_sec);
	printf("Verify:         %s\n\n", g_verify ? "Yes" : "No");
}

static void
usage(void)
{
	printf("accel_perf options:\n");
	printf("\t[-h help message]\n");
	printf("\t[-q queue depth per core]\n");
	printf("\t[-C for supported workloads, use this value to configure the io vector size to test (default 1)\n");
	printf("\t[-T number of threads per core\n");
	printf("\t[-o transfer size in bytes (default: 4KiB. For compress/decompress, 0 means the input file size)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-w workload type must be one of these: copy, fill, crc32c, copy_crc32c, compare, compress, decompress, dualcast, xor,\n");
	printf("\t[                                       dif_verify, , dif_generate, dif_generate_copy\n");
	printf("\t[-M assign module to the operation, not compatible with accel_assign_opc RPC\n");
	printf("\t[-l for compress/decompress workloads, name of uncompressed input file\n");
	printf("\t[-S for crc32c workload, use this seed value (default 0)\n");
	printf("\t[-P for compare workload, percentage of operations that should miscompare (percent, default 0)\n");
	printf("\t[-f for fill workload, use this BYTE value (default 255)\n");
	printf("\t[-x for xor workload, use this number of source buffers (default, minimum: 2)]\n");
	printf("\t[-y verify result if this switch is on]\n");
	printf("\t[-a tasks to allocate per core (default: same value as -q)]\n");
	printf("\t\tCan be used to spread operations across a wider range of memory.\n");
}

static int
parse_args(int ch, char *arg)
{
	int argval = 0;

	switch (ch) {
	case 'a':
	case 'C':
	case 'f':
	case 'T':
	case 'o':
	case 'P':
	case 'q':
	case 'S':
	case 't':
	case 'x':
		argval = spdk_strtol(optarg, 10);
		if (argval < 0) {
			fprintf(stderr, "-%c option must be non-negative.\n", ch);
			usage();
			return 1;
		}
		break;
	default:
		break;
	};

	switch (ch) {
	case 'a':
		g_allocate_depth = argval;
		break;
	case 'C':
		g_chained_count = argval;
		break;
	case 'l':
		g_cd_file_in_name = optarg;
		break;
	case 'f':
		g_fill_pattern = (uint8_t)argval;
		break;
	case 'T':
		g_threads_per_core = argval;
		break;
	case 'o':
		g_xfer_size_bytes = argval;
		break;
	case 'P':
		g_fail_percent_goal = argval;
		break;
	case 'q':
		g_queue_depth = argval;
		break;
	case 'S':
		g_crc32c_seed = argval;
		break;
	case 't':
		g_time_in_sec = argval;
		break;
	case 'x':
		g_xor_src_count = argval;
		break;
	case 'y':
		g_verify = true;
		break;
	case 'w':
		g_workload_type = optarg;
		if (!strcmp(g_workload_type, "copy")) {
			g_workload_selection = SPDK_ACCEL_OPC_COPY;
		} else if (!strcmp(g_workload_type, "fill")) {
			g_workload_selection = SPDK_ACCEL_OPC_FILL;
		} else if (!strcmp(g_workload_type, "crc32c")) {
			g_workload_selection = SPDK_ACCEL_OPC_CRC32C;
		} else if (!strcmp(g_workload_type, "copy_crc32c")) {
			g_workload_selection = SPDK_ACCEL_OPC_COPY_CRC32C;
		} else if (!strcmp(g_workload_type, "compare")) {
			g_workload_selection = SPDK_ACCEL_OPC_COMPARE;
		} else if (!strcmp(g_workload_type, "dualcast")) {
			g_workload_selection = SPDK_ACCEL_OPC_DUALCAST;
		} else if (!strcmp(g_workload_type, "compress")) {
			g_workload_selection = SPDK_ACCEL_OPC_COMPRESS;
		} else if (!strcmp(g_workload_type, "decompress")) {
			g_workload_selection = SPDK_ACCEL_OPC_DECOMPRESS;
		} else if (!strcmp(g_workload_type, "xor")) {
			g_workload_selection = SPDK_ACCEL_OPC_XOR;
		} else if (!strcmp(g_workload_type, "dif_verify")) {
			g_workload_selection = SPDK_ACCEL_OPC_DIF_VERIFY;
		} else if (!strcmp(g_workload_type, "dif_generate")) {
			g_workload_selection = SPDK_ACCEL_OPC_DIF_GENERATE;
		} else if (!strcmp(g_workload_type, "dif_generate_copy")) {
			g_workload_selection = SPDK_ACCEL_OPC_DIF_GENERATE_COPY;
		} else {
			fprintf(stderr, "Unsupported workload type: %s\n", optarg);
			usage();
			return 1;
		}
		break;
	case 'M':
		g_module_name = optarg;
		break;

	default:
		usage();
		return 1;
	}

	return 0;
}

static int dump_result(void);
static void
unregister_worker(void *arg1)
{
	struct worker_thread *worker = arg1;

	if (worker->ch) {
		spdk_accel_get_opcode_stats(worker->ch, worker->workload,
					    &worker->stats, sizeof(worker->stats));
		spdk_put_io_channel(worker->ch);
		worker->ch = NULL;
	}
	free(worker->task_base);
	spdk_thread_exit(spdk_get_thread());
	pthread_mutex_lock(&g_workers_lock);
	assert(g_num_workers >= 1);
	if (--g_num_workers == 0) {
		pthread_mutex_unlock(&g_workers_lock);
		/* Only dump results on successful runs */
		if (g_rc == 0) {
			g_rc = dump_result();
		}
		spdk_app_stop(g_rc);
	} else {
		pthread_mutex_unlock(&g_workers_lock);
	}
}

static void
accel_perf_construct_iovs(void *buf, uint64_t sz, struct iovec *iovs, uint32_t iovcnt)
{
	uint64_t ele_size;
	uint8_t *data;
	uint32_t i;

	ele_size = spdk_divide_round_up(sz, iovcnt);

	data = buf;
	for (i = 0; i < iovcnt; i++) {
		ele_size = spdk_min(ele_size, sz);
		assert(ele_size > 0);

		iovs[i].iov_base = data;
		iovs[i].iov_len = ele_size;

		data += ele_size;
		sz -= ele_size;
	}
	assert(sz == 0);
}

static int
_get_task_data_bufs(struct ap_task *task)
{
	uint32_t align = 0;
	uint32_t i = 0;
	int src_buff_len = g_xfer_size_bytes;
	int dst_buff_len = g_xfer_size_bytes;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	uint32_t num_blocks, transfer_size_with_md;
	int rc;

	/* For dualcast, the DSA HW requires 4K alignment on destination addresses but
	 * we do this for all modules to keep it simple.
	 */
	if (g_workload_selection == SPDK_ACCEL_OPC_DUALCAST) {
		align = ALIGN_4K;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_COMPRESS ||
	    g_workload_selection == SPDK_ACCEL_OPC_DECOMPRESS) {
		task->cur_seg = STAILQ_FIRST(&g_compress_segs);

		if (g_workload_selection == SPDK_ACCEL_OPC_COMPRESS) {
			dst_buff_len = task->cur_seg->compressed_len_padded;
		}

		task->dst = spdk_dma_zmalloc(dst_buff_len, align, NULL);
		if (task->dst == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}

		task->dst_iovs = calloc(g_chained_count, sizeof(struct iovec));
		if (!task->dst_iovs) {
			fprintf(stderr, "cannot allocate task->dst_iovs for task=%p\n", task);
			return -ENOMEM;
		}
		task->dst_iovcnt = g_chained_count;
		accel_perf_construct_iovs(task->dst, dst_buff_len, task->dst_iovs, task->dst_iovcnt);

		return 0;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		task->dst_iovcnt = g_chained_count;
		task->dst_iovs = calloc(task->dst_iovcnt, sizeof(struct iovec));
		if (!task->dst_iovs) {
			fprintf(stderr, "cannot allocate task->dst_iovs for task=%p\n", task);
			return -ENOMEM;
		}

		num_blocks = g_xfer_size_bytes / g_block_size_bytes;
		/* Add bytes for each block for metadata */
		transfer_size_with_md = g_xfer_size_bytes + (num_blocks * g_md_size_bytes);
		task->num_blocks = num_blocks;

		for (i = 0; i < task->dst_iovcnt; i++) {
			task->dst_iovs[i].iov_base = spdk_dma_zmalloc(transfer_size_with_md, 0, NULL);
			if (task->dst_iovs[i].iov_base == NULL) {
				return -ENOMEM;
			}
			task->dst_iovs[i].iov_len = transfer_size_with_md;
		}

		dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
		dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

		rc = spdk_dif_ctx_init(&task->dif_ctx,
				       g_block_size_bytes + g_md_size_bytes,
				       g_md_size_bytes, true, true,
				       SPDK_DIF_TYPE1,
				       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
				       0x123, 0xFFFF, 0x234, 0, 0, &dif_opts);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed\n");
			return rc;
		}
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_CRC32C ||
	    g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		assert(g_chained_count > 0);
		task->src_iovcnt = g_chained_count;
		task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
		if (!task->src_iovs) {
			fprintf(stderr, "cannot allocated task->src_iovs fot task=%p\n", task);
			return -ENOMEM;
		}

		if (g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C) {
			dst_buff_len = g_xfer_size_bytes * g_chained_count;
		}

		if (g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
		    g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY) {
			src_buff_len += (g_xfer_size_bytes / g_block_size_bytes) * g_md_size_bytes;
		}

		for (i = 0; i < task->src_iovcnt; i++) {
			task->src_iovs[i].iov_base = spdk_dma_zmalloc(src_buff_len, 0, NULL);
			if (task->src_iovs[i].iov_base == NULL) {
				return -ENOMEM;
			}
			memset(task->src_iovs[i].iov_base, DATA_PATTERN, src_buff_len);
			task->src_iovs[i].iov_len = src_buff_len;
		}
	} else if (g_workload_selection == SPDK_ACCEL_OPC_XOR) {
		assert(g_xor_src_count > 1);
		task->sources = calloc(g_xor_src_count, sizeof(*task->sources));
		if (!task->sources) {
			return -ENOMEM;
		}

		for (i = 0; i < g_xor_src_count; i++) {
			task->sources[i] = spdk_dma_zmalloc(g_xfer_size_bytes, 0, NULL);
			if (!task->sources[i]) {
				return -ENOMEM;
			}
			memset(task->sources[i], DATA_PATTERN, g_xfer_size_bytes);
		}
	} else {
		task->src = spdk_dma_zmalloc(g_xfer_size_bytes, 0, NULL);
		if (task->src == NULL) {
			fprintf(stderr, "Unable to alloc src buffer\n");
			return -ENOMEM;
		}

		/* For fill, set the entire src buffer so we can check if verify is enabled. */
		if (g_workload_selection == SPDK_ACCEL_OPC_FILL) {
			memset(task->src, g_fill_pattern, g_xfer_size_bytes);
		} else {
			memset(task->src, DATA_PATTERN, g_xfer_size_bytes);
		}
	}

	if (g_workload_selection != SPDK_ACCEL_OPC_CRC32C &&
	    g_workload_selection != SPDK_ACCEL_OPC_DIF_VERIFY &&
	    g_workload_selection != SPDK_ACCEL_OPC_DIF_GENERATE &&
	    g_workload_selection != SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		task->dst = spdk_dma_zmalloc(dst_buff_len, align, NULL);
		if (task->dst == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}

		/* For compare we want the buffers to match, otherwise not. */
		if (g_workload_selection == SPDK_ACCEL_OPC_COMPARE) {
			memset(task->dst, DATA_PATTERN, dst_buff_len);
		} else {
			memset(task->dst, ~DATA_PATTERN, dst_buff_len);
		}
	}

	/* For dualcast 2 buffers are needed for the operation.  */
	if (g_workload_selection == SPDK_ACCEL_OPC_DUALCAST ||
	    (g_workload_selection == SPDK_ACCEL_OPC_XOR && g_verify)) {
		task->dst2 = spdk_dma_zmalloc(g_xfer_size_bytes, align, NULL);
		if (task->dst2 == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}
		memset(task->dst2, ~DATA_PATTERN, g_xfer_size_bytes);
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
	    g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY) {
		dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
		dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

		task->num_blocks = (g_xfer_size_bytes * g_chained_count) / g_block_size_bytes;

		rc = spdk_dif_ctx_init(&task->dif_ctx,
				       g_block_size_bytes + g_md_size_bytes,
				       g_md_size_bytes, true, true,
				       SPDK_DIF_TYPE1,
				       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
				       16, 0xFFFF, 10, 0, 0, &dif_opts);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed, error (%d)\n", rc);
			return rc;
		}

		if (g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY) {
			rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIF failed, error (%d)\n", rc);
				return rc;
			}
		}
	}

	return 0;
}

inline static struct ap_task *
_get_task(struct worker_thread *worker)
{
	struct ap_task *task;

	if (!TAILQ_EMPTY(&worker->tasks_pool)) {
		task = TAILQ_FIRST(&worker->tasks_pool);
		TAILQ_REMOVE(&worker->tasks_pool, task, link);
	} else {
		fprintf(stderr, "Unable to get ap_task\n");
		return NULL;
	}

	return task;
}

/* Submit one operation using the same ap task that just completed. */
static void
_submit_single(struct worker_thread *worker, struct ap_task *task)
{
	int random_num;
	int rc = 0;
	int flags = 0;

	assert(worker);

	switch (worker->workload) {
	case SPDK_ACCEL_OPC_COPY:
		rc = spdk_accel_submit_copy(worker->ch, task->dst, task->src,
					    g_xfer_size_bytes, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_FILL:
		/* For fill use the first byte of the task->dst buffer */
		rc = spdk_accel_submit_fill(worker->ch, task->dst, *(uint8_t *)task->src,
					    g_xfer_size_bytes, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		rc = spdk_accel_submit_crc32cv(worker->ch, &task->crc_dst,
					       task->src_iovs, task->src_iovcnt, g_crc32c_seed,
					       accel_done, task);
		break;
	case SPDK_ACCEL_OPC_COPY_CRC32C:
		rc = spdk_accel_submit_copy_crc32cv(worker->ch, task->dst, task->src_iovs, task->src_iovcnt,
						    &task->crc_dst, g_crc32c_seed, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_COMPARE:
		random_num = rand() % 100;
		if (random_num < g_fail_percent_goal) {
			task->expected_status = -EILSEQ;
			*(uint8_t *)task->dst = ~DATA_PATTERN;
		} else {
			task->expected_status = 0;
			*(uint8_t *)task->dst = DATA_PATTERN;
		}
		rc = spdk_accel_submit_compare(worker->ch, task->dst, task->src,
					       g_xfer_size_bytes, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_DUALCAST:
		rc = spdk_accel_submit_dualcast(worker->ch, task->dst, task->dst2,
						task->src, g_xfer_size_bytes, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_COMPRESS:
		task->src_iovs = task->cur_seg->uncompressed_iovs;
		task->src_iovcnt = task->cur_seg->uncompressed_iovcnt;
		rc = spdk_accel_submit_compress(worker->ch, task->dst, task->cur_seg->compressed_len_padded,
						task->src_iovs,
						task->src_iovcnt, &task->compressed_sz, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_DECOMPRESS:
		task->src_iovs = task->cur_seg->compressed_iovs;
		task->src_iovcnt = task->cur_seg->compressed_iovcnt;
		rc = spdk_accel_submit_decompress(worker->ch, task->dst_iovs, task->dst_iovcnt, task->src_iovs,
						  task->src_iovcnt, NULL, flags, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_XOR:
		rc = spdk_accel_submit_xor(worker->ch, task->dst, task->sources, g_xor_src_count,
					   g_xfer_size_bytes, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_DIF_VERIFY:
		rc = spdk_accel_submit_dif_verify(worker->ch, task->src_iovs, task->src_iovcnt, task->num_blocks,
						  &task->dif_ctx, &task->dif_err, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_DIF_GENERATE:
		rc = spdk_accel_submit_dif_generate(worker->ch, task->src_iovs, task->src_iovcnt, task->num_blocks,
						    &task->dif_ctx, accel_done, task);
		break;
	case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
		rc = spdk_accel_submit_dif_generate_copy(worker->ch, task->dst_iovs, task->dst_iovcnt,
				task->src_iovs, task->src_iovcnt,
				task->num_blocks, &task->dif_ctx, accel_done, task);
		break;
	default:
		assert(false);
		break;

	}

	worker->current_queue_depth++;
	if (rc) {
		accel_done(task, rc);
	}
}

static void
_free_task_buffers(struct ap_task *task)
{
	uint32_t i;

	if (g_workload_selection == SPDK_ACCEL_OPC_DECOMPRESS ||
	    g_workload_selection == SPDK_ACCEL_OPC_COMPRESS) {
		free(task->dst_iovs);
	} else if (g_workload_selection == SPDK_ACCEL_OPC_CRC32C ||
		   g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C ||
		   g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY ||
		   g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE ||
		   g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		if (task->src_iovs) {
			for (i = 0; i < task->src_iovcnt; i++) {
				if (task->src_iovs[i].iov_base) {
					spdk_dma_free(task->src_iovs[i].iov_base);
				}
			}
			free(task->src_iovs);
		}
	} else if (g_workload_selection == SPDK_ACCEL_OPC_XOR) {
		if (task->sources) {
			for (i = 0; i < g_xor_src_count; i++) {
				spdk_dma_free(task->sources[i]);
			}
			free(task->sources);
		}
	} else {
		spdk_dma_free(task->src);
	}

	spdk_dma_free(task->dst);
	if (g_workload_selection == SPDK_ACCEL_OPC_DUALCAST || g_workload_selection == SPDK_ACCEL_OPC_XOR) {
		spdk_dma_free(task->dst2);
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE_COPY) {
		if (task->dst_iovs) {
			for (i = 0; i < task->dst_iovcnt; i++) {
				if (task->dst_iovs[i].iov_base) {
					spdk_dma_free(task->dst_iovs[i].iov_base);
				}
			}
			free(task->dst_iovs);
		}
	}
}

static int
_vector_memcmp(void *_dst, struct iovec *src_src_iovs, uint32_t iovcnt)
{
	uint32_t i;
	uint32_t ttl_len = 0;
	uint8_t *dst = (uint8_t *)_dst;

	for (i = 0; i < iovcnt; i++) {
		if (memcmp(dst, src_src_iovs[i].iov_base, src_src_iovs[i].iov_len)) {
			return -1;
		}
		dst += src_src_iovs[i].iov_len;
		ttl_len += src_src_iovs[i].iov_len;
	}

	if (ttl_len != iovcnt * g_xfer_size_bytes) {
		return -1;
	}

	return 0;
}

static int _worker_stop(void *arg);

static void
accel_done(void *arg1, int status)
{
	struct ap_task *task = arg1;
	struct worker_thread *worker = task->worker;
	uint32_t sw_crc32c;
	struct spdk_dif_error err_blk;

	assert(worker);
	assert(worker->current_queue_depth > 0);

	if (g_verify && status == 0) {
		switch (worker->workload) {
		case SPDK_ACCEL_OPC_COPY_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->src_iovs, task->src_iovcnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				worker->xfer_failed++;
			}
			if (_vector_memcmp(task->dst, task->src_iovs, task->src_iovcnt)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->src_iovs, task->src_iovcnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_COPY:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_DUALCAST:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, first destination\n");
				worker->xfer_failed++;
			}
			if (memcmp(task->src, task->dst2, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, second destination\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_FILL:
			if (memcmp(task->dst, task->src, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_COMPARE:
			break;
		case SPDK_ACCEL_OPC_COMPRESS:
			break;
		case SPDK_ACCEL_OPC_DECOMPRESS:
			if (memcmp(task->dst, task->cur_seg->uncompressed_data, task->cur_seg->uncompressed_len)) {
				SPDK_NOTICELOG("Data miscompare on decompression\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_XOR:
			if (spdk_xor_gen(task->dst2, task->sources, g_xor_src_count,
					 g_xfer_size_bytes) != 0) {
				SPDK_ERRLOG("Failed to generate xor for verification\n");
			} else if (memcmp(task->dst, task->dst2, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_DIF_VERIFY:
			break;
		case SPDK_ACCEL_OPC_DIF_GENERATE:
			if (spdk_dif_verify(task->src_iovs, task->src_iovcnt, task->num_blocks,
					    &task->dif_ctx, &err_blk) != 0) {
				SPDK_NOTICELOG("Data miscompare, "
					       "err_type %u, expected %lu, actual %lu, err_offset %u\n",
					       err_blk.err_type, err_blk.expected,
					       err_blk.actual, err_blk.err_offset);
				worker->xfer_failed++;
			}
			break;
		case SPDK_ACCEL_OPC_DIF_GENERATE_COPY:
			if (spdk_dif_verify(task->dst_iovs, task->dst_iovcnt, task->num_blocks,
					    &task->dif_ctx, &err_blk) != 0) {
				SPDK_NOTICELOG("Data miscompare, "
					       "err_type %u, expected %lu, actual %lu, err_offset %u\n",
					       err_blk.err_type, err_blk.expected,
					       err_blk.actual, err_blk.err_offset);
				worker->xfer_failed++;
			}
			break;
		default:
			assert(false);
			break;
		}
	}

	if (worker->workload == SPDK_ACCEL_OPC_COMPRESS ||
	    g_workload_selection == SPDK_ACCEL_OPC_DECOMPRESS) {
		/* Advance the task to the next segment */
		task->cur_seg = STAILQ_NEXT(task->cur_seg, link);
		if (task->cur_seg == NULL) {
			task->cur_seg = STAILQ_FIRST(&g_compress_segs);
		}
	}

	if (task->expected_status == -EILSEQ) {
		assert(status != 0);
		worker->injected_miscompares++;
		status = 0;
	} else if (status) {
		/* Expected to pass but the accel module reported an error (ex: COMPARE operation). */
		worker->xfer_failed++;
	}

	worker->current_queue_depth--;

	if (!worker->is_draining && status == 0) {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
		task = _get_task(worker);
		_submit_single(worker, task);
	} else {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
	}
}

static int
dump_result(void)
{
	uint64_t total_completed = 0;
	uint64_t total_failed = 0;
	uint64_t total_miscompared = 0;
	uint64_t total_xfer_per_sec, total_bw_in_MiBps;
	struct worker_thread *worker = g_workers;
	char tmp[64];

	printf("\n%-12s %20s %16s %16s %16s\n",
	       "Core,Thread", "Transfers", "Bandwidth", "Failed", "Miscompares");
	printf("------------------------------------------------------------------------------------\n");
	while (worker != NULL) {

		uint64_t xfer_per_sec = worker->stats.executed / g_time_in_sec;
		uint64_t bw_in_MiBps = worker->stats.num_bytes /
				       (g_time_in_sec * 1024 * 1024);

		total_completed += worker->stats.executed;
		total_failed += worker->xfer_failed;
		total_miscompared += worker->injected_miscompares;

		snprintf(tmp, sizeof(tmp), "%u,%u", worker->display.core, worker->display.thread);
		if (xfer_per_sec) {
			printf("%-12s %18" PRIu64 "/s %10" PRIu64 " MiB/s %16"PRIu64 " %16" PRIu64 "\n",
			       tmp, xfer_per_sec, bw_in_MiBps, worker->xfer_failed,
			       worker->injected_miscompares);
		}

		worker = worker->next;
	}

	total_xfer_per_sec = total_completed / g_time_in_sec;
	total_bw_in_MiBps = (total_completed * g_xfer_size_bytes) /
			    (g_time_in_sec * 1024 * 1024);

	printf("====================================================================================\n");
	printf("%-12s %18" PRIu64 "/s %10" PRIu64 " MiB/s %16"PRIu64 " %16" PRIu64 "\n",
	       "Total", total_xfer_per_sec, total_bw_in_MiBps, total_failed, total_miscompared);

	return total_failed ? 1 : 0;
}

static inline void
_free_task_buffers_in_pool(struct worker_thread *worker)
{
	struct ap_task *task;

	assert(worker);
	while ((task = TAILQ_FIRST(&worker->tasks_pool))) {
		TAILQ_REMOVE(&worker->tasks_pool, task, link);
		_free_task_buffers(task);
	}
}

static int
_check_draining(void *arg)
{
	struct worker_thread *worker = arg;

	assert(worker);

	if (worker->current_queue_depth == 0) {
		_free_task_buffers_in_pool(worker);
		spdk_poller_unregister(&worker->is_draining_poller);
		unregister_worker(worker);
	}

	return SPDK_POLLER_BUSY;
}

static int
_worker_stop(void *arg)
{
	struct worker_thread *worker = arg;

	assert(worker);

	spdk_poller_unregister(&worker->stop_poller);

	/* now let the worker drain and check it's outstanding IO with a poller */
	worker->is_draining = true;
	worker->is_draining_poller = SPDK_POLLER_REGISTER(_check_draining, worker, 0);

	return SPDK_POLLER_BUSY;
}

static void shutdown_cb(void);

static void
_init_thread(void *arg1)
{
	struct worker_thread *worker;
	struct ap_task *task;
	int i, num_tasks = g_allocate_depth;
	struct display_info *display = arg1;

	worker = calloc(1, sizeof(*worker));
	if (worker == NULL) {
		fprintf(stderr, "Unable to allocate worker\n");
		free(display);
		spdk_thread_exit(spdk_get_thread());
		goto no_worker;
	}

	worker->workload = g_workload_selection;
	worker->display.core = display->core;
	worker->display.thread = display->thread;
	free(display);
	worker->core = spdk_env_get_current_core();
	worker->thread = spdk_get_thread();
	pthread_mutex_lock(&g_workers_lock);
	g_num_workers++;
	worker->next = g_workers;
	g_workers = worker;
	pthread_mutex_unlock(&g_workers_lock);
	worker->ch = spdk_accel_get_io_channel();
	if (worker->ch == NULL) {
		fprintf(stderr, "Unable to get an accel channel\n");
		goto error;
	}

	TAILQ_INIT(&worker->tasks_pool);

	worker->task_base = calloc(num_tasks, sizeof(struct ap_task));
	if (worker->task_base == NULL) {
		fprintf(stderr, "Could not allocate task base.\n");
		goto error;
	}

	task = worker->task_base;
	for (i = 0; i < num_tasks; i++) {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
		task->worker = worker;
		if (_get_task_data_bufs(task)) {
			fprintf(stderr, "Unable to get data bufs\n");
			goto error;
		}
		task++;
	}

	/* Register a poller that will stop the worker at time elapsed */
	worker->stop_poller = SPDK_POLLER_REGISTER(_worker_stop, worker,
			      g_time_in_sec * 1000000ULL);

	/* Load up queue depth worth of operations. */
	for (i = 0; i < g_queue_depth; i++) {
		task = _get_task(worker);
		if (task == NULL) {
			goto error;
		}

		_submit_single(worker, task);
	}
	return;
error:

	_free_task_buffers_in_pool(worker);
	free(worker->task_base);
no_worker:
	shutdown_cb();
	g_rc = -1;
}

static void
accel_perf_start(void *arg1)
{
	struct spdk_cpuset tmp_cpumask = {};
	char thread_name[32];
	uint32_t i;
	int j;
	struct spdk_thread *thread;
	struct display_info *display;

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	dump_user_config();

	printf("Running for %d seconds...\n", g_time_in_sec);
	fflush(stdout);

	/* Create worker threads for each core that was specified. */
	SPDK_ENV_FOREACH_CORE(i) {
		for (j = 0; j < g_threads_per_core; j++) {
			snprintf(thread_name, sizeof(thread_name), "ap_worker_%u_%u", i, j);
			spdk_cpuset_zero(&tmp_cpumask);
			spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
			thread = spdk_thread_create(thread_name, &tmp_cpumask);
			display = calloc(1, sizeof(*display));
			if (display == NULL) {
				fprintf(stderr, "Unable to allocate memory\n");
				spdk_app_stop(-1);
				return;
			}
			display->core = i;
			display->thread = j;
			spdk_thread_send_msg(thread, _init_thread, display);
		}
	}
}

static void
accel_perf_free_compress_segs(void)
{
	struct ap_compress_seg *seg, *tmp;

	STAILQ_FOREACH_SAFE(seg, &g_compress_segs, link, tmp) {
		free(seg->uncompressed_iovs);
		free(seg->compressed_iovs);
		spdk_dma_free(seg->compressed_data);
		spdk_dma_free(seg->uncompressed_data);
		STAILQ_REMOVE_HEAD(&g_compress_segs, link);
		free(seg);
	}
}

struct accel_perf_prep_ctx {
	FILE			*file;
	long			remaining;
	struct spdk_io_channel	*ch;
	struct ap_compress_seg	*cur_seg;
};

static void accel_perf_prep_process_seg(struct accel_perf_prep_ctx *ctx);

static void
accel_perf_prep_process_seg_cpl(void *ref, int status)
{
	struct accel_perf_prep_ctx *ctx = ref;
	struct ap_compress_seg *seg;

	if (status != 0) {
		fprintf(stderr, "error (%d) on initial compress completion\n", status);
		spdk_dma_free(ctx->cur_seg->compressed_data);
		spdk_dma_free(ctx->cur_seg->uncompressed_data);
		free(ctx->cur_seg);
		spdk_put_io_channel(ctx->ch);
		fclose(ctx->file);
		free(ctx);
		spdk_app_stop(-status);
		return;
	}

	seg = ctx->cur_seg;

	if (g_workload_selection == SPDK_ACCEL_OPC_DECOMPRESS) {
		seg->compressed_iovs = calloc(g_chained_count, sizeof(struct iovec));
		if (seg->compressed_iovs == NULL) {
			fprintf(stderr, "unable to allocate iovec\n");
			spdk_dma_free(seg->compressed_data);
			spdk_dma_free(seg->uncompressed_data);
			free(seg);
			spdk_put_io_channel(ctx->ch);
			fclose(ctx->file);
			free(ctx);
			spdk_app_stop(-ENOMEM);
			return;
		}
		seg->compressed_iovcnt = g_chained_count;

		accel_perf_construct_iovs(seg->compressed_data, seg->compressed_len, seg->compressed_iovs,
					  seg->compressed_iovcnt);
	}

	STAILQ_INSERT_TAIL(&g_compress_segs, seg, link);
	ctx->remaining -= seg->uncompressed_len;

	accel_perf_prep_process_seg(ctx);
}

static void
accel_perf_prep_process_seg(struct accel_perf_prep_ctx *ctx)
{
	struct ap_compress_seg *seg;
	int sz, sz_read, sz_padded;
	void *ubuf, *cbuf;
	struct iovec iov[1];
	int rc;

	if (ctx->remaining == 0) {
		spdk_put_io_channel(ctx->ch);
		fclose(ctx->file);
		free(ctx);
		accel_perf_start(NULL);
		return;
	}

	sz = spdk_min(ctx->remaining, g_xfer_size_bytes);
	/* Add 10% pad to the compress buffer for incompressible data. Note that a real app
	 * would likely either deal with the failure of not having a large enough buffer
	 * by submitting another operation with a larger one.  Or, like the vbdev module
	 * does, just accept the error and use the data uncompressed marking it as such in
	 * its own metadata so that in the future it doesn't try to decompress uncompressed
	 * data, etc.
	 */
	sz_padded = sz * COMP_BUF_PAD_PERCENTAGE;

	ubuf = spdk_dma_zmalloc(sz, ALIGN_4K, NULL);
	if (!ubuf) {
		fprintf(stderr, "unable to allocate uncompress buffer\n");
		rc = -ENOMEM;
		goto error;
	}

	cbuf = spdk_dma_malloc(sz_padded, ALIGN_4K, NULL);
	if (!cbuf) {
		fprintf(stderr, "unable to allocate compress buffer\n");
		rc = -ENOMEM;
		spdk_dma_free(ubuf);
		goto error;
	}

	seg = calloc(1, sizeof(*seg));
	if (!seg) {
		fprintf(stderr, "unable to allocate comp/decomp segment\n");
		spdk_dma_free(ubuf);
		spdk_dma_free(cbuf);
		rc = -ENOMEM;
		goto error;
	}

	sz_read = fread(ubuf, sizeof(uint8_t), sz, ctx->file);
	if (sz_read != sz) {
		fprintf(stderr, "unable to read input file\n");
		free(seg);
		spdk_dma_free(ubuf);
		spdk_dma_free(cbuf);
		rc = -errno;
		goto error;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_COMPRESS) {
		seg->uncompressed_iovs = calloc(g_chained_count, sizeof(struct iovec));
		if (seg->uncompressed_iovs == NULL) {
			fprintf(stderr, "unable to allocate iovec\n");
			free(seg);
			spdk_dma_free(ubuf);
			spdk_dma_free(cbuf);
			rc = -ENOMEM;
			goto error;
		}
		seg->uncompressed_iovcnt = g_chained_count;
		accel_perf_construct_iovs(ubuf, sz, seg->uncompressed_iovs, seg->uncompressed_iovcnt);
	}

	seg->uncompressed_data = ubuf;
	seg->uncompressed_len = sz;
	seg->compressed_data = cbuf;
	seg->compressed_len = sz;
	seg->compressed_len_padded = sz_padded;

	ctx->cur_seg = seg;
	iov[0].iov_base = seg->uncompressed_data;
	iov[0].iov_len = seg->uncompressed_len;
	/* Note that anytime a call is made to spdk_accel_submit_compress() there's a chance
	 * it will fail with -ENOMEM in the event that the destination buffer is not large enough
	 * to hold the compressed data.  This example app simply adds 10% buffer for compressed data
	 * but real applications may want to consider a more sophisticated method.
	 */
	rc = spdk_accel_submit_compress(ctx->ch, seg->compressed_data, seg->compressed_len_padded, iov, 1,
					&seg->compressed_len, 0, accel_perf_prep_process_seg_cpl, ctx);
	if (rc < 0) {
		fprintf(stderr, "error (%d) on initial compress submission\n", rc);
		goto error;
	}

	return;

error:
	spdk_put_io_channel(ctx->ch);
	fclose(ctx->file);
	free(ctx);
	spdk_app_stop(rc);
}

static void
accel_perf_prep(void *arg1)
{
	struct accel_perf_prep_ctx *ctx;
	const char *module_name = NULL;
	int rc = 0;

	if (g_module_name) {
		rc = spdk_accel_get_opc_module_name(g_workload_selection, &module_name);
		if (rc != 0 || strcmp(g_module_name, module_name) != 0) {
			fprintf(stderr, "Module '%s' was assigned via JSON config or RPC, instead of '%s'\n",
				module_name, g_module_name);
			fprintf(stderr, "-M option is not compatible with accel_assign_opc RPC\n");
			rc = -EINVAL;
			goto error_end;
		}
	}

	if (g_workload_selection != SPDK_ACCEL_OPC_COMPRESS &&
	    g_workload_selection != SPDK_ACCEL_OPC_DECOMPRESS) {
		accel_perf_start(arg1);
		return;
	}

	if (g_cd_file_in_name == NULL) {
		fprintf(stdout, "A filename is required.\n");
		rc = -EINVAL;
		goto error_end;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_COMPRESS && g_verify) {
		fprintf(stdout, "\nCompression does not support the verify option, aborting.\n");
		rc = -ENOTSUP;
		goto error_end;
	}

	printf("Preparing input file...\n");

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto error_end;
	}

	ctx->file = fopen(g_cd_file_in_name, "r");
	if (ctx->file == NULL) {
		fprintf(stderr, "Could not open file %s.\n", g_cd_file_in_name);
		rc = -errno;
		goto error_ctx;
	}

	fseek(ctx->file, 0L, SEEK_END);
	ctx->remaining = ftell(ctx->file);
	fseek(ctx->file, 0L, SEEK_SET);

	ctx->ch = spdk_accel_get_io_channel();
	if (ctx->ch == NULL) {
		rc = -EAGAIN;
		goto error_file;
	}

	if (g_xfer_size_bytes == 0) {
		/* size of 0 means "file at a time" */
		g_xfer_size_bytes = ctx->remaining;
	}

	accel_perf_prep_process_seg(ctx);
	return;

error_file:
	fclose(ctx->file);
error_ctx:
	free(ctx);
error_end:
	spdk_app_stop(rc);
}

static void
worker_shutdown(void *ctx)
{
	_worker_stop(ctx);
}

static void
shutdown_cb(void)
{
	struct worker_thread *worker;

	pthread_mutex_lock(&g_workers_lock);
	if (!g_workers) {
		spdk_app_stop(1);
		goto unlock;
	}

	worker = g_workers;
	while (worker) {
		spdk_thread_send_msg(worker->thread, worker_shutdown, worker);
		worker = worker->next;
	}
unlock:
	pthread_mutex_unlock(&g_workers_lock);
}

int
main(int argc, char **argv)
{
	struct worker_thread *worker, *tmp;
	int rc;

	pthread_mutex_init(&g_workers_lock, NULL);
	spdk_app_opts_init(&g_opts, sizeof(g_opts));
	g_opts.name = "accel_perf";
	g_opts.reactor_mask = "0x1";
	g_opts.shutdown_cb = shutdown_cb;

	rc = spdk_app_parse_args(argc, argv, &g_opts, "a:C:o:q:t:yw:M:P:f:T:l:S:x:", NULL,
				 parse_args, usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc == SPDK_APP_PARSE_ARGS_HELP ? 0 : 1;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_LAST) {
		fprintf(stderr, "Must provide a workload type\n");
		usage();
		return -1;
	}

	if (g_allocate_depth > 0 && g_queue_depth > g_allocate_depth) {
		fprintf(stdout, "allocate depth must be at least as big as queue depth\n");
		usage();
		return -1;
	}

	if (g_allocate_depth == 0) {
		g_allocate_depth = g_queue_depth;
	}

	if ((g_workload_selection == SPDK_ACCEL_OPC_CRC32C ||
	     g_workload_selection == SPDK_ACCEL_OPC_COPY_CRC32C ||
	     g_workload_selection == SPDK_ACCEL_OPC_DIF_VERIFY ||
	     g_workload_selection == SPDK_ACCEL_OPC_DIF_GENERATE) &&
	    g_chained_count == 0) {
		usage();
		return -1;
	}

	if (g_workload_selection == SPDK_ACCEL_OPC_XOR && g_xor_src_count < 2) {
		usage();
		return -1;
	}

	if (g_module_name && spdk_accel_assign_opc(g_workload_selection, g_module_name)) {
		fprintf(stderr, "Was not able to assign '%s' module to the workload\n", g_module_name);
		usage();
		return -1;
	}

	g_rc = spdk_app_start(&g_opts, accel_perf_prep, NULL);
	if (g_rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

	pthread_mutex_destroy(&g_workers_lock);

	worker = g_workers;
	while (worker) {
		tmp = worker->next;
		free(worker);
		worker = tmp;
	}
	accel_perf_free_compress_segs();
	spdk_app_fini();
	return g_rc;
}
