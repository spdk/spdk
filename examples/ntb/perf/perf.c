/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/ae4dma.h"
#include "spdk/ntb_amd.h"
#include "spdk/ntb_amd_spec.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/log.h"


#define MAX_TOTAL_ORDER 40
#define MIN_TOTAL_ORDER 20
#define MIN_CHUNK_ORDER 12
#define NTB_LINK_POLL_TIMEOUT 100000

/*
 * Doorbells used by ntb_perf. Each macro pair groups the bit index and the
 * mask so the poller cannot test one bit and clear another by mistake.
 */
#define NTB_DB_PEER_READY_BIT      0
#define NTB_DB_PEER_DOWN_BIT       1
#define NTB_DB_PEER_ACK_DOWN_BIT   2
#define NTB_DB_COPY_DONE_BIT       4

#define NTB_DB_PEER_READY_MASK     (1u << NTB_DB_PEER_READY_BIT)
#define NTB_DB_PEER_DOWN_MASK      (1u << NTB_DB_PEER_DOWN_BIT)
#define NTB_DB_PEER_ACK_DOWN_MASK  (1u << NTB_DB_PEER_ACK_DOWN_BIT)
#define NTB_DB_COPY_DONE_MASK      (1u << NTB_DB_COPY_DONE_BIT)

/* AE4DMA definitions */
#define AE4DMA_MAX_HWQUEUES_PERDEVICE 1
#define AE4DMA_MAX_ENGINES 1
#define AE4DMA_QUEUE_DEPTH 28
/*
 * Hardware maximum number of command queues per AE4DMA device. This mirrors
 * the driver's AE4DMA_MAX_HW_QUEUES: the library clamps requests above it, but
 * this app keeps using the requested -l value to index cmd_q[], so it must
 * reject anything larger here to avoid out-of-bounds hardware queue IDs.
 */
#define AE4DMA_HW_QUEUES_MAX 16

/* NTB definitions */
#define ALIGN_4K 0x1000 /* 4K */
#define TOTAL_ORDER (1ULL << MIN_TOTAL_ORDER) /* 1MB */
#define NUM_TOTAL_BUFFERS (TOTAL_ORDER / CHUNK_ORDER)
#define CHUNK_ORDER (1ULL << MIN_CHUNK_ORDER) /* 4KB */

#define BYTES_TO_KB(bytes)	(bytes / 1024)
#define BYTES_TO_MB(bytes)	((BYTES_TO_KB(bytes)) / 1024)

/* AE4DMA structures */
struct pci_device {
	struct spdk_pci_device *pci_dev;
	TAILQ_ENTRY(pci_device) tailq;
};

struct user_dma_config {
	uint32_t xfer_size_bytes;
	uint32_t queue_depth;
	uint32_t time_in_sec;
	bool verify;
	char *core_mask;
	uint32_t ae4dma_chan_num;
	uint32_t ae4dma_hw_queues;
};

struct ae4dma_device {
	struct spdk_ae4dma_chan *ae4dma;
	TAILQ_ENTRY(ae4dma_device) tailq;
};

typedef struct ae4dma_datapool {
	struct spdk_mempool *src_mp;
	uint64_t  *dst;
} ae4dma_datapool_t;

struct ae4dma_chan_entry {
	struct spdk_ae4dma_chan *chan;
	int ae4dma_chan_id;
	int ae4dma_hwq_id;
	uint64_t xfer_completed;
	uint64_t xfer_failed;

	uint64_t next_job_index;
	uint64_t bytes_transferred;
	bool reported_done;
	/* timing */
	uint64_t tsc_start;
	uint64_t tsc_end;

	uint64_t current_queue_depth;
	uint64_t waiting_for_flush;
	uint64_t flush_threshold;
	bool is_draining;
	ae4dma_datapool_t *data_pool;
	struct spdk_mempool *task_pool;
	struct ae4dma_chan_entry *next;
};

struct dma_worker_thread {
	struct ae4dma_chan_entry        *ctx;
	struct dma_worker_thread    *next;
	unsigned                core;
};

struct ae4dma_task {
	struct ae4dma_chan_entry *ae4dma_chan_entry;
	void *src;
	void *dst;
};

static TAILQ_HEAD(, pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);
static TAILQ_HEAD(, ae4dma_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static struct ae4dma_device *g_next_device;
static struct user_dma_config g_user_dma_config;
static uint32_t g_ae4dma_chan_num = 0;
static struct dma_worker_thread *g_dma_workers = NULL;
static int g_num_workers = 0;
static volatile uint32_t g_dma_done = 0;
static volatile uint32_t g_cpu_done = 0;
static uint64_t g_total_buffers;

ae4dma_datapool_t *ae4dma_datapool_create(const char *name, uint64_t xfer_size, uint8_t count);

static int submit_single_xfer(struct ae4dma_chan_entry *ae4dma_chan_entry,
			      struct ae4dma_task *ae4dma_task,
			      void *dst, void *src);
static void drain_io(struct ae4dma_chan_entry *ae4dma_chan_entry);

/* NTB structures and variables */

static uint64_t g_total_order;
static uint64_t g_chunk_order;
/* Raw bit counts captured during -O/-o parsing; cross-validated after
 * spdk_app_parse_args() finishes so the order of -O / -o on the command
 * line doesn't matter. */
static uint32_t g_total_order_bits;
static uint32_t g_chunk_order_bits;
static uint64_t *g_output_buffer;
static uint64_t *g_output_buffer_phys;
/* NTB memory domain for the active peer window; describes g_output_buffer_phys
 * as an already-resolved device (bus) address to spdk_ae4dma_build_copy(). */
static struct spdk_memory_domain *g_mw_domain;
static uint64_t gmw_size;
static uint8_t gmw_cnt;
static uint8_t g_start_copy;
static uint8_t g_use_dmacopy;
static uint8_t g_mw_index = 0;
/*
 * Total bytes allocated for the handshake/staging buffer.
 * Equal to sum(mw_size[i]); needed by spdk_ntb_amd_handshake_work() so the
 * lib can validate the buffer is big enough to back every memory window
 * with a non-overlapping slice.
 */
static size_t g_handshake_buffer_size = 0;
static int64_t ntb_link_status;
static int8_t gntb_topo;
/*
 * Set from the doorbell poller and observed by the read-mode loop on the
 * main thread. Use an atomic uint32_t so that the compiler does not hoist
 * the load out of the wait loop and so the store/load form a happens-before
 * pair across threads.
 */
static volatile uint32_t g_copy_done = 0;

/*
 * Set from a SIGINT/SIGTERM handler and polled by the peer-dependent wait
 * loops on the main thread. Those loops (the link-status wait and the
 * read-mode wait) can otherwise spin forever if the peer never rings the
 * expected doorbell; because they run directly on the reactor thread, the
 * default SPDK signal-driven shutdown poller never gets a chance to run,
 * so Ctrl-C would not stop the app. Checking this flag lets the loops
 * unwind to the normal cleanup path (which calls spdk_app_stop()).
 */
static volatile sig_atomic_t g_shutdown = 0;

static void
ntb_perf_sig_handler(int signum)
{
	(void)signum;
	g_shutdown = 1;
}

/*
 * Per-application NTB configuration / scratch space.
 *
 * The driver does not look at the contents of this struct; it lives here in
 * the example purely to keep the per-MW handshake buffer alongside its
 * mempool.
 */
struct spdk_ntb_amd_dev_config {
	uint8_t mw_cnt;
	uint64_t *mw_size;
	struct spdk_mempool *data_pool;
	void *buffer;
};

static struct spdk_ntb_amd_dev_config *conf;

struct display_info {
	int core;
	int thread;
};

struct cpu_worker_thread {
	int thread_idx;
	uint8_t core_id;
	uint64_t *dst_buffer;
	uint64_t *start_buffer_offset;
	uint64_t num_buffers;
	/*
	 * Bytes available to this worker starting at dst_buffer. Each worker
	 * writes only into [dst_buffer, dst_buffer + dst_slice_size) so that
	 * concurrent workers don't race on the same destination bytes
	 */
	uint64_t dst_slice_size;
	struct cpu_worker_thread *next;
};

struct ntb_device_entry {
	struct spdk_ntb_amd_dev *ntb;
	void  *ntb_mbuf;
};

static struct cpu_worker_thread *g_cpu_workers = NULL;
static struct ntb_device_entry *ntb_dev;
/*
 * Raw NTB handle captured in attach_cb() for deferred cleanup. attach_cb()
 * runs while spdk_ntb_amd_probe() holds the driver lock, and
 * spdk_ntb_amd_detach() takes that same non-recursive lock, so detaching
 * from inside the callback would deadlock. We stash the handle here and
 * detach it later from ntb_exit(), after probe has returned.
 */
static struct spdk_ntb_amd_dev *g_probed_ntb;
static struct spdk_app_opts g_opts = {};

static void ntb_perf_usage(void);
static int ntb_get_attr(struct spdk_ntb_amd_dev *ntb, const char *attr_name, uint64_t *attr_value);
static int ntb_dma_copy(void);
static int ntb_cpu_copy(void);
static void ntb_main_thread_cb(void *ctx);
static void ntb_dma_perf_fn(void *arg);
static void ntb_cpu_perf_fn(void *arg);

/* AE4DMA functions */
ae4dma_datapool_t *
ae4dma_datapool_create(const char *mempool_name, uint64_t xfer_size, uint8_t count)
{
	ae4dma_datapool_t *data_pool = (ae4dma_datapool_t *) malloc(sizeof(ae4dma_datapool_t));
	if (data_pool == NULL) {
		printf("failed to alloc mem for data_pool\n");
		return NULL;
	}
	errno = 0;
	data_pool->src_mp = spdk_mempool_create(mempool_name, (g_total_buffers + AE4DMA_QUEUE_DEPTH),
						g_chunk_order, SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, SPDK_ENV_NUMA_ID_ANY);
	if (data_pool->src_mp == NULL) {
		int saved_errno = errno;
		SPDK_ERRLOG("Failed to create dma src mempool '%s' "
			    "count=%" PRIu64 " elt_size=%" PRIu64
			    " (errno=%d/%s)\n",
			    mempool_name,
			    (uint64_t)(g_total_buffers + AE4DMA_QUEUE_DEPTH),
			    (uint64_t)g_chunk_order,
			    saved_errno, strerror(saved_errno));
		free(data_pool);
		return NULL;
	}

	data_pool->dst = g_output_buffer_phys;

	if (data_pool->src_mp == NULL || data_pool->dst == NULL) {
		printf("failed to alloc mem for data_pool src&dst\n");
		free(data_pool);
		return NULL;
	}
	return data_pool;
}

static void
construct_user_dma_config(struct user_dma_config *self)
{
	self->xfer_size_bytes = 4096; /* chunk_size */
	self->ae4dma_chan_num = AE4DMA_MAX_ENGINES; /* engine count - n */
	self->ae4dma_hw_queues = AE4DMA_MAX_HWQUEUES_PERDEVICE ; /* channels l */
	self->queue_depth = AE4DMA_QUEUE_DEPTH; /* using fixed 28 QD for NTB */
	self->verify = false;
	self->core_mask = "0x1"; /* SPDK reactor core mask - m */
}

static void
dump_user_dma_config(struct user_dma_config *self)
{
	printf("\nUser ae4dma configuration:\n");
	printf(" Number of AE4DMA engines:    %u\n", self->ae4dma_chan_num);
	printf(" Number of hw queues per device:    %u\n", self->ae4dma_hw_queues);
	printf(" Transfer size:  %u bytes\n", self->xfer_size_bytes);
	printf(" Queue depth:    %u\n\n", self->queue_depth);
}

static void
ae4dma_exit(void)
{
	struct ae4dma_device *dev;
	struct pci_device *pci_dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		if (dev->ae4dma) {
			spdk_ae4dma_detach(dev->ae4dma);
		}
		free(dev);
	}
	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
	}
}

static bool
ae4dma_probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct pci_device *pdev;
	printf("Found matching device at %04x:%02x:%02x.%x "
	       "vendor:0x%04x device:0x%04x\n",
	       spdk_pci_device_get_domain(pci_dev),
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev),
	       spdk_pci_device_get_vendor_id(pci_dev), spdk_pci_device_get_device_id(pci_dev));

	pdev = calloc(1, sizeof(*pdev));
	if (pdev == NULL) {
		return false;
	}
	pdev->pci_dev = pci_dev;

	/*
	 * Claim BEFORE inserting into the tracking list. If we did it the
	 * other way around, a claim failure would leave a "tracked but not
	 * owned" entry that ae4dma_exit() would later detach, double-freeing
	 * a device that another process may now legitimately own.
	 */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		SPDK_ERRLOG("Could not claim %04x:%02x:%02x.%x; another process is using it?\n",
			    spdk_pci_device_get_domain(pci_dev),
			    spdk_pci_device_get_bus(pci_dev),
			    spdk_pci_device_get_dev(pci_dev),
			    spdk_pci_device_get_func(pci_dev));
		free(pdev);
		return false;
	}

	TAILQ_INSERT_TAIL(&g_pci_devices, pdev, tailq);
	return true;
}

static void
ae4dma_attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ae4dma_chan *ae4dma)
{
	struct ae4dma_device *dev;

	if (g_ae4dma_chan_num >= g_user_dma_config.ae4dma_chan_num) {
		return;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ae4dma = ae4dma;
	g_ae4dma_chan_num++;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

static int
register_workers(void)
{
	uint32_t i;
	struct dma_worker_thread *worker;

	g_dma_workers = NULL;
	g_num_workers = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		/* Skip the main thread's core */
		if (i == spdk_env_get_current_core()) {
			continue;
		}
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			SPDK_ERRLOG("Unable to allocate worker\n");
			return 1;
		}

		worker->core = i;
		worker->next = g_dma_workers;
		g_dma_workers = worker;
		g_num_workers++;
	}

	return 0;
}

static void
unregister_dma_workers(void)
{
	struct dma_worker_thread *worker = g_dma_workers;
	struct ae4dma_chan_entry *entry, *entry1;

	/* Free ae4dma_chan_entry and worker thread */
	while (worker) {
		struct dma_worker_thread *next_worker = worker->next;
		entry = worker->ctx;
		while (entry) {
			entry1 = entry->next;
			/*
			 * Make sure no descriptor is still in flight before
			 * tearing down the per-channel pools. The worker
			 * thread normally drains via drain_io() but if we
			 * arrive here through an early-error path it may
			 * not have.
			 */
			if (entry->chan) {
				entry->is_draining = true;
				drain_io(entry);
			}
			if (entry->data_pool) {
				if (entry->data_pool->src_mp) {
					spdk_mempool_free(entry->data_pool->src_mp);
				}
				free(entry->data_pool);
			}
			if (entry->task_pool) {
				spdk_mempool_free(entry->task_pool);
			}
			free(entry);
			entry = entry1;
		}
		free(worker);
		worker = next_worker;
	}
	g_dma_workers = NULL;
}

static void
drain_io(struct ae4dma_chan_entry *ae4dma_chan_entry)
{
	spdk_ae4dma_flush(ae4dma_chan_entry->chan, ae4dma_chan_entry->ae4dma_hwq_id);
	while (ae4dma_chan_entry->current_queue_depth > 0) {
		spdk_ae4dma_process_events(ae4dma_chan_entry->chan, ae4dma_chan_entry->ae4dma_hwq_id);
	}
}
static int
submit_xfer_oncb_done(struct ae4dma_chan_entry *e, struct ae4dma_task *ae4dma_task)
{
	void *src = NULL;
	void *dst = NULL;
	uint64_t mw_size = gmw_size;
	void *mw_base = (void *)e->data_pool->dst;
	uint64_t chunk_size = g_user_dma_config.xfer_size_bytes;
	uint64_t job = e->next_job_index++;

	/*
	 * Sequential write: copying to entire mw - dst,
	 * using fixed qd number of task's and src's
	* Wrapping dst: if total_order exceeds mw_size
	 */

	uint64_t dst_offset = (job * chunk_size) % mw_size;
	dst = (char *)mw_base + dst_offset;
	src = spdk_mempool_get(e->data_pool->src_mp);
	if (src == NULL) {
		SPDK_ERRLOG("Failed to acquire src buffer for resubmit; returning task to pool\n");
		spdk_mempool_put(e->task_pool, ae4dma_task);
		return -ENOMEM;
	}
	memset(src, 'A' + (job % 26), g_user_dma_config.xfer_size_bytes);

	if (submit_single_xfer(e, ae4dma_task, dst, src) != 0) {
		spdk_mempool_put(e->data_pool->src_mp, src);
		spdk_mempool_put(e->task_pool, ae4dma_task);
		return -EIO;
	}

	return 0;
}

static void
ae4dma_done(void *cb_arg, int status)
{
	struct ae4dma_task *ae4dma_task = (struct ae4dma_task *)cb_arg;
	struct ae4dma_chan_entry *ae4dma_chan_entry = ae4dma_task->ae4dma_chan_entry;
	void *completed_src = ae4dma_task->src;

	/*
	 * The DMA destination here is a peer NTB BAR IOVA (device memory),
	 * not a CPU-dereferenceable pointer, so src/dst cannot be compared
	 * with memcmp(). Byte-for-byte verification of a device-memory copy
	 * would require reading back through the local CPU mapping of the
	 * window, which this example does not wire up; verification is
	 * therefore not supported on the DMA path.
	 *
	 * A non-zero status is the hardware descriptor error code: the copy
	 * did not land in the peer window, so it must be recorded as a failure
	 * rather than counted as a successful completion.
	 */
	if (status != 0) {
		SPDK_ERRLOG("DMA transfer failed on channel %d hwq %d: status %d\n",
			    ae4dma_chan_entry->ae4dma_chan_id, ae4dma_chan_entry->ae4dma_hwq_id,
			    status);
		ae4dma_chan_entry->xfer_failed++;
	} else if (!(ae4dma_chan_entry->is_draining)) {
		ae4dma_chan_entry->xfer_completed++;
	}

	if (ae4dma_chan_entry->current_queue_depth) {
		ae4dma_chan_entry->current_queue_depth--;
	}

	/*
	 * The src buffer associated with this task is always returned to the
	 * source mempool here. On the resubmit path submit_xfer_oncb_done()
	 * obtains a fresh src buffer for the next transfer.
	 */
	if (completed_src != NULL) {
		spdk_mempool_put(ae4dma_chan_entry->data_pool->src_mp, completed_src);
		ae4dma_task->src = NULL;
	}

	if (ae4dma_chan_entry->is_draining) {
		spdk_mempool_put(ae4dma_chan_entry->task_pool, ae4dma_task);
	} else {
		submit_xfer_oncb_done(ae4dma_chan_entry, ae4dma_task);
	}
}

static int
submit_single_xfer(struct ae4dma_chan_entry *ae4dma_chan_entry, struct ae4dma_task *ae4dma_task,
		   void *dst,
		   void *src)
{
	int ret;
	struct iovec siov, diov;

	ae4dma_task->ae4dma_chan_entry = ae4dma_chan_entry;
	ae4dma_task->src = src;
	ae4dma_task->dst = dst;

	/* Single-segment iovecs for source and destination. */
	siov.iov_base = src;
	siov.iov_len  = g_user_dma_config.xfer_size_bytes;
	diov.iov_base = dst;
	diov.iov_len  = g_user_dma_config.xfer_size_bytes;

	/*
	 * Destination is the peer NTB window (device memory), so it is tagged
	 * with g_mw_domain; the host source buffer has no domain. spdk_ae4dma_
	 * build_copy() returns 1 when the descriptor ring is full. In that case
	 * we must reap completions before retrying; otherwise we'd spin forever
	 * holding the only thread that can free descriptor slots.
	 */
	for (;;) {
		ret = spdk_ae4dma_build_copy(ae4dma_chan_entry->chan,
					     ae4dma_chan_entry->ae4dma_hwq_id,
					     ae4dma_task, ae4dma_done,
					     &diov, 1, g_mw_domain,
					     &siov, 1, NULL);
		if (ret == 0) {
			break;
		}
		if (ret == 1) {
			spdk_ae4dma_flush(ae4dma_chan_entry->chan,
					  ae4dma_chan_entry->ae4dma_hwq_id);
			spdk_ae4dma_process_events(ae4dma_chan_entry->chan,
						   ae4dma_chan_entry->ae4dma_hwq_id);
			continue;
		}
		SPDK_ERRLOG("spdk_ae4dma_build_copy() failed: %d\n", ret);
		return ret;
	}

	spdk_ae4dma_flush(ae4dma_chan_entry->chan, ae4dma_chan_entry->ae4dma_hwq_id);

	ae4dma_chan_entry->current_queue_depth++;

	return 0;
}

static int
submit_xfers(struct ae4dma_chan_entry *ae4dma_chan_entry, uint64_t queue_depth,
	     uint64_t total_chunks)
{
	uint64_t i = 0;
	uint64_t idx;

	while (queue_depth-- > 0) {
		void *src = NULL, *dst = NULL;
		struct ae4dma_task *ae4dma_task = NULL;
		int rc;

		src = spdk_mempool_get(ae4dma_chan_entry->data_pool->src_mp);
		ae4dma_task = spdk_mempool_get(ae4dma_chan_entry->task_pool);
		if (!ae4dma_task || !src) {
			SPDK_ERRLOG("ae4dma_task or src buffer allocation failed\n");
			if (src) {
				spdk_mempool_put(ae4dma_chan_entry->data_pool->src_mp, src);
			}
			if (ae4dma_task) {
				spdk_mempool_put(ae4dma_chan_entry->task_pool, ae4dma_task);
			}
			return 1;
		}

		dst = (char *)(ae4dma_chan_entry->data_pool->dst) + (i * g_user_dma_config.xfer_size_bytes);
		memset(src, 'a' + (i % 26), g_user_dma_config.xfer_size_bytes);

		rc = submit_single_xfer(ae4dma_chan_entry, ae4dma_task, dst, src);
		if (rc != 0) {
			spdk_mempool_put(ae4dma_chan_entry->data_pool->src_mp, src);
			spdk_mempool_put(ae4dma_chan_entry->task_pool, ae4dma_task);
			return 1;
		}

		i++;

		idx = ae4dma_chan_entry->next_job_index++;
		if (idx >= (total_chunks - 1)) {
			/* If total_chunks is less than qd, do not fill remaining slots. */
			break;
		}
	}
	return 0;
}

static int
ntb_dma_perf(void *arg)
{
	bool all_done;
	struct dma_worker_thread *worker = (struct dma_worker_thread *)arg;
	struct ae4dma_chan_entry *t = NULL;
	uint64_t hz = spdk_get_ticks_hz();
	uint64_t target_completions = g_total_buffers;

	t = worker->ctx;

	while (t != NULL) {
		/* begin to submit transfers */
		t->waiting_for_flush = 0;
		t->flush_threshold = g_user_dma_config.queue_depth / 2;

		if (submit_xfers(t, g_user_dma_config.queue_depth, target_completions) != 0) {
			/*
			 * Signal completion on this error path too, otherwise
			 * wait_for_workers_done(&g_dma_done, ...) would never
			 * reach the worker count and the app would hang.
			 */
			__sync_add_and_fetch(&g_dma_done, 1);
			spdk_thread_exit(spdk_get_thread());
			return 1;
		}
		t = t->next;
	}

	while (1) {
		all_done = true;  /* assume all channels finished */

		t = worker->ctx;
		while (t != NULL) {
			if (t->xfer_completed < target_completions) {
				/* first time we touch this channel, record start */
				if (t->xfer_completed == 0 && t->tsc_start == 0) {
					t->tsc_start = spdk_get_ticks();
				}

				spdk_ae4dma_process_events(t->chan, t->ae4dma_hwq_id);
				all_done = false;

			} else if (!t->reported_done) {
				/* mark stop time */
				t->tsc_end = spdk_get_ticks();

				double sec = (double)(t->tsc_end - t->tsc_start) / hz;
				double mb = (t->xfer_completed * g_user_dma_config.xfer_size_bytes) / (1024.0 * 1024.0);
				double mbps = mb / sec;

				printf("Engine %d on core %d with channel %d finished: %" PRIu64
				       " completions, %" PRIu64 " failed, %.2f MB copied in %.3f s (%.2f MB/s)\n",
				       t->ae4dma_chan_id,
				       worker->core,
				       t->ae4dma_hwq_id,
				       t->xfer_completed,
				       t->xfer_failed,
				       mb, sec, mbps);

				t->reported_done = true;
			}
			t = t->next;
		}

		if (all_done) {
			/* printf("All channels finished (each >= %" PRIu64 " completions)\n",target_completions); */
			break;
		}
	}
	/*
	 * Processing left-over descriptors,to clear all the submitted once.
	 * not accounting for calculations.
	 */
	t = worker->ctx;
	while (t != NULL) {
		/* begin to drain io */
		t->is_draining = true;
		drain_io(t);
		t = t->next;
	}
	__sync_add_and_fetch(&g_dma_done, 1);
	spdk_thread_exit(spdk_get_thread());
	return 0;
}

static void
ntb_dma_perf_fn(void *arg)
{
	(void)ntb_dma_perf(arg);
}

static struct spdk_ae4dma_chan *
get_next_chan(void)
{
	struct spdk_ae4dma_chan *chan;

	if (g_next_device == NULL) {
		return NULL;
	}

	chan = g_next_device->ae4dma;
	g_next_device = TAILQ_NEXT(g_next_device, tailq);

	return chan;
}

static int
associate_workers_with_chan(void)
{
	struct spdk_ae4dma_chan *chan = get_next_chan();
	struct dma_worker_thread    *worker = g_dma_workers;
	struct ae4dma_chan_entry        *t;
	char buf_pool_name[40], task_pool_name[40];
	int i = 0, j = 0;

	while (chan != NULL) {

		for (j = (g_user_dma_config.ae4dma_hw_queues - 1); j >=  0; j--) {
			t = calloc(1, sizeof(struct ae4dma_chan_entry));
			if (!t) {
				return 1;
			}
			t->ae4dma_chan_id = i;
			t->ae4dma_hwq_id = j;

			snprintf(buf_pool_name, sizeof(buf_pool_name),
				 "ae4dma_src_%d_%d", i, j);
			snprintf(task_pool_name, sizeof(task_pool_name),
				 "ae4dma_task_%d_%d", i, j);

			t->data_pool = ae4dma_datapool_create(buf_pool_name, g_user_dma_config.xfer_size_bytes,
							      g_user_dma_config.queue_depth);

			t->task_pool = spdk_mempool_create(task_pool_name,
							   g_user_dma_config.queue_depth,
							   sizeof(struct ae4dma_task),
							   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
							   SPDK_ENV_SOCKET_ID_ANY);
			if (!t->task_pool || !t->data_pool) {
				SPDK_ERRLOG("Could not allocate buffer pool.\n");
				if (t->data_pool) {
					/*
					 * free() on the wrapper alone would leak
					 * the contained src mempool; release it
					 * first.
					 */
					if (t->data_pool->src_mp) {
						spdk_mempool_free(t->data_pool->src_mp);
					}
					free(t->data_pool);
				}
				if (t->task_pool) {
					spdk_mempool_free(t->task_pool);
				}
				free(t);
				return 1;
			}

			t->next_job_index = 0;
			t->chan = chan;
			t->next = worker->ctx;
			worker->ctx = t;
			worker = worker->next;
			if (worker == NULL) {
				worker = g_dma_workers;
			}
		}

		chan = get_next_chan();
		i++;
	}
	return 0;
}

/* NTB functions */

static void
ntb_exit(void)
{
	/*
	 * Detach first. spdk_ntb_amd_detach() clears our advertised link/
	 * translation state, so the peer stops being able to reach conf->buffer.
	 * Only then is it safe to free that buffer -- freeing it while the
	 * windows are still live would let the peer write into freed memory.
	 */
	if (g_probed_ntb != NULL) {
		spdk_ntb_amd_detach(g_probed_ntb);
		g_probed_ntb = NULL;
	}
	free(ntb_dev);
	ntb_dev = NULL;

	if (conf != NULL) {
		if (conf->buffer != NULL) {
			spdk_dma_free(conf->buffer);
			conf->buffer = NULL;
		}
		free(conf);
		conf = NULL;
		g_handshake_buffer_size = 0;
	}
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	printf("Found matching device at %04x:%02x:%02x.%x "
	       "vendor:0x%04x device:0x%04x\n",
	       spdk_pci_device_get_domain(pci_dev),
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev),
	       spdk_pci_device_get_vendor_id(pci_dev), spdk_pci_device_get_device_id(pci_dev));

	/*
	 * This sample app only manages a single NTB endpoint. Reject any
	 * further candidates so they aren't claimed and silently leaked.
	 */
	if (ntb_dev != NULL) {
		printf("ntb_perf only supports one NTB device; ignoring additional endpoint\n");
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ntb_amd_dev *ntb)
{
	/*
	 * Record the handle for cleanup regardless of what happens below.
	 * We must not call spdk_ntb_amd_detach() here: this callback runs with
	 * the driver lock held by spdk_ntb_amd_probe(), and detach() takes the
	 * same non-recursive lock. Any teardown is deferred to ntb_exit().
	 */
	g_probed_ntb = ntb;

	if (ntb_dev != NULL) {
		/*
		 * The probe_cb above should have prevented this, but guard
		 * here as well so that a future change to that policy cannot
		 * leak the previous handle.
		 */
		SPDK_ERRLOG("attach_cb invoked with ntb_dev already populated; extra device deferred to cleanup\n");
		return;
	}

	ntb_dev = calloc(1, sizeof(*ntb_dev));
	if (ntb_dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	ntb_dev->ntb = ntb;
}

static int
ntb_init(void)
{
	if (spdk_ntb_amd_probe(NULL, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ntb_amd_probe() failed\n");
		return 1;
	}
	/*
	 * spdk_ntb_amd_probe() also returns success when it finds no matching
	 * device (attach_cb never ran) or when attach_cb hit an allocation
	 * failure. In both cases ntb_dev is NULL and dereferencing it later
	 * would crash, so treat "nothing attached" as an error here.
	 */
	if (ntb_dev == NULL) {
		SPDK_ERRLOG("No NTB device was attached\n");
		return 1;
	}
	SPDK_NOTICELOG("NTB device probed\n");
	return 0;
}

static int
ntb_dev_configure(struct spdk_ntb_amd_dev *ntb)
{
	(void)ntb;

	conf = calloc(1, sizeof(struct spdk_ntb_amd_dev_config));
	if (conf == NULL) {
		SPDK_ERRLOG("Out of memory allocating ntb config\n");
		return -ENOMEM;
	}

	/*
	 * Allocate one contiguous staging buffer that backs every exposed
	 * memory window with a non-overlapping slice. spdk_ntb_amd_handshake_work()
	 * lays MW i out at offset sum(mw_size[0..i-1]); on AMD NTB the per-MW
	 * sizes are uniform so the total is gmw_cnt * gmw_size.
	 */
	g_handshake_buffer_size = (size_t)gmw_cnt * (size_t)gmw_size;
	conf->buffer = spdk_dma_zmalloc(g_handshake_buffer_size, ALIGN_4K, NULL);
	if (conf->buffer == NULL) {
		SPDK_ERRLOG("Failed to allocate %zu B handshake buffer "
			    "(gmw_cnt=%u gmw_size=%" PRIu64 ")\n",
			    g_handshake_buffer_size, (unsigned)gmw_cnt,
			    (uint64_t)gmw_size);
		free(conf);
		conf = NULL;
		return -ENOMEM;
	}
	conf->data_pool = NULL;

	return 0;
}

static int
ntb_get_attr(struct spdk_ntb_amd_dev *ntb, const char *attr_name, uint64_t *attr_value)
{
	return spdk_ntb_amd_attr_get(ntb, attr_name, attr_value);
}

static inline int
wait_for_workers_done(volatile uint32_t *done_counter, uint32_t total_workers)
{
	/*
	 * Block until every worker has bumped *done_counter. This function
	 * has no failure mode of its own: once the counter is >= total it
	 * has succeeded, so always return 0.
	 */
	while (__atomic_load_n(done_counter, __ATOMIC_RELAXED) < total_workers) {
		spdk_thread_poll(spdk_get_thread(), 0, 0);
		spdk_delay_us(1000);
	}

	return 0;
}

static int
ntb_dma_copy(void)
{
	int rc, core_id;
	struct dma_worker_thread *worker;

	struct spdk_cpuset tmp_cpumask = {};
	char thread_name[32];
	struct spdk_thread *currentthread;

	/*
	 * Pass hw_queues_per_device by value.
	 * cb_ctx is left NULL because perf doesn't pass any
	 * additional opaque context to the probe/attach callbacks.
	 */
	if (spdk_ae4dma_probe((uint8_t)g_user_dma_config.ae4dma_hw_queues, NULL, ae4dma_probe_cb,
			      ae4dma_attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ae4dma_probe() failed\n");
		return 1;
	}

	if (g_total_order == 0 || g_chunk_order == 0) {
		SPDK_ERRLOG("Invalid parameters for copy.\n");
		return -EINVAL;
	}
	g_user_dma_config.xfer_size_bytes = g_chunk_order;

	if (register_workers() != 0) {
		rc = 1;
		goto cleanup;
	}

	/*
	 * register_workers() skips the main reactor's core, so a single-core
	 * reactor mask leaves no DMA workers. associate_workers_with_chan()
	 * would then dereference a NULL worker; bail out with a clear message
	 * (the CPU path performs the same check).
	 */
	if (g_num_workers == 0 || g_dma_workers == NULL) {
		SPDK_ERRLOG("No worker cores available (need at least 2 cores; pass -m <mask with >=2 cores>)\n");
		rc = 1;
		goto cleanup;
	}

	if (g_ae4dma_chan_num == 0) {
		printf("No channels found\n");
		rc = 1;
		goto cleanup;
	}
	if (g_user_dma_config.ae4dma_chan_num > g_ae4dma_chan_num) {
		printf("%u channels are requested, but only %u are found, "
		       "so only test %u channels\n", g_user_dma_config.ae4dma_chan_num,
		       g_ae4dma_chan_num, g_ae4dma_chan_num);
		g_user_dma_config.ae4dma_chan_num = g_ae4dma_chan_num;
	}

	dump_user_dma_config(&g_user_dma_config);

	g_next_device = TAILQ_FIRST(&g_devices);

	if (associate_workers_with_chan() != 0) {
		rc = 1;
		goto cleanup;
	}
	for (worker = g_dma_workers; worker; worker = worker->next) {

		core_id = worker->core;
		snprintf(thread_name, sizeof(thread_name), "ap_dma_worker_%u", core_id);
		spdk_cpuset_zero(&tmp_cpumask);
		spdk_cpuset_set_cpu(&tmp_cpumask, core_id, true);

		currentthread = spdk_thread_create(thread_name, &tmp_cpumask);
		if (currentthread) {
			spdk_thread_send_msg(currentthread, ntb_dma_perf_fn, worker);
		} else {
			printf("failed to create thread = %s", thread_name);
			rc = 1;
			goto cleanup;
		}

	}
	/* wait until all dma workers finished */
	rc = wait_for_workers_done(&g_dma_done, g_num_workers);

cleanup:
	unregister_dma_workers();
	ae4dma_exit();
	return rc;
}

static void
unregister_cpu_workers(void)
{
	struct cpu_worker_thread *worker = g_cpu_workers;
	while (worker) {
		struct cpu_worker_thread *next_worker = worker->next;
		free(worker);
		worker = next_worker;
	}
}

static int
ntb_cpu_perf(void *arg1)
{
	int rc;
	uint64_t start, end;
	uint64_t i;
	double cpu_time_seconds = 0.0;
	double cpu_time_ms = 0.0;
	double bandwidth_MBps = 0.0;
	uint64_t total_buffers;
	char mempool_name[32];
	uint8_t **src_buffers;
	struct spdk_mempool *src_mp = NULL;
	uint64_t slice_size;

	struct cpu_worker_thread *worker = (struct cpu_worker_thread *)arg1;
	if (worker == NULL) {
		SPDK_ERRLOG("Worker thread argument is NULL.\n");
		rc = 1;
		goto cleanup;
	}
	/*
	 * Each worker is given a non-overlapping slice of the destination MW
	 * by ntb_cpu_copy(); copy only within that slice and wrap with its
	 * size, not the whole MW.
	 */
	slice_size = worker->dst_slice_size;
	if (slice_size == 0) {
		SPDK_ERRLOG("Worker thread %d has zero-sized dst slice.\n",
			    worker->thread_idx);
		rc = 1;
		goto cleanup;
	}
	uint8_t *dst = (uint8_t *)worker->dst_buffer;

	if (g_total_order == 0 || g_chunk_order == 0) {
		printf("Error: Invalid parameters for copy.\n");
		rc = -EINVAL;
		goto cleanup;
	}
	total_buffers = g_total_buffers;

	snprintf(mempool_name, sizeof(mempool_name), "src_mempool_%d", worker->thread_idx);
	src_mp = spdk_mempool_create(mempool_name, total_buffers,
				     g_chunk_order, SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, SPDK_ENV_NUMA_ID_ANY);
	if (src_mp == NULL) {
		SPDK_ERRLOG("Worker thread %d failed to create mempool.\n", worker->thread_idx);
		rc = 1;
		goto cleanup;
	}

	src_buffers = calloc(total_buffers, sizeof(uint8_t *));
	if (src_buffers == NULL) {
		SPDK_ERRLOG("Thread %d failed to allocate src buffers.\n", worker->thread_idx);
		spdk_mempool_free(src_mp);
		rc = 1;
		goto cleanup;
	}

	for (i = 0; i < total_buffers; i++) {
		src_buffers[i] = spdk_mempool_get(src_mp);
		if (src_buffers[i] == NULL) {
			SPDK_ERRLOG("Error getting buffer %" PRIu64 " from mempool\n", i);
			for (; i > 0; i--) {
				spdk_mempool_put(src_mp, src_buffers[i - 1]);
			}
			free(src_buffers);
			spdk_mempool_free(src_mp);
			rc = 1;
			goto cleanup;
		}
		memset(src_buffers[i], 'A' + (i % 26), g_chunk_order);
	}

	start = spdk_get_ticks();
	for (i = 0; i < total_buffers; i++) {
		/* Wrap inside this worker's own slice so writes from
		 * different workers stay in disjoint destination ranges. */
		uint64_t current_dst_offset = (i * g_chunk_order) % slice_size;

		memcpy(dst + current_dst_offset, src_buffers[i], g_chunk_order);
	}

	end = spdk_get_ticks();

	if (end > start) {
		cpu_time_seconds = (double)(end - start) / spdk_get_ticks_hz();
		cpu_time_ms = cpu_time_seconds * 1000.0;
		bandwidth_MBps = (double)g_total_order / (cpu_time_seconds * 1024 * 1024);
	}

	printf("Data copied: %" PRIu64 " bytes in  %.3f ms. Bandwidth: %.2f MBytes/s on core %d\n",
	       g_total_order, cpu_time_ms, bandwidth_MBps, worker->core_id);

	for (i = 0; i < total_buffers; i++) {
		spdk_mempool_put(src_mp, src_buffers[i]);
	}

	spdk_mempool_free(src_mp);
	free(src_buffers);
	rc = 0;

cleanup:
	/*
	 * Signal completion on every exit path (success and all error/goto
	 * paths), otherwise wait_for_workers_done(&g_cpu_done, ...) would
	 * never reach the worker count and the app would hang.
	 */
	__sync_add_and_fetch(&g_cpu_done, 1);
	spdk_thread_exit(spdk_get_thread());

	return rc;
}

static void
ntb_cpu_perf_fn(void *arg)
{
	(void)ntb_cpu_perf(arg);
}

static int
ntb_cpu_copy(void)
{
	int rc = 0;
	uint32_t core_id;
	char thread_name[32];
	struct spdk_thread *currentthread;
	struct spdk_cpuset tmp_cpumask = {};
	uint32_t total_workers;
	uint64_t per_worker_slice;

	/*
	 * Partition the destination memory window across worker threads
	 * BEFORE dispatching any of them.
	 *
	 * The total core count - 1 (we skip the main reactor's core) gives us
	 * the worker count up front. We hand each worker a non-overlapping
	 * slice of the active MW; the per-worker copy loop then wraps inside
	 * its own slice, not the whole MW.
	 */
	total_workers = spdk_env_get_core_count();
	if (total_workers > 0) {
		total_workers -= 1;
	}
	if (total_workers == 0) {
		SPDK_ERRLOG("No worker cores available (need at least 2 cores)\n");
		return 1;
	}

	per_worker_slice = gmw_size / total_workers;
	/* Round each worker's slice down to a chunk boundary so the modulo
	 * arithmetic in ntb_cpu_perf() never tears a chunk across slices. */
	per_worker_slice -= per_worker_slice % g_chunk_order;
	if (per_worker_slice == 0) {
		SPDK_ERRLOG("MW too small (%" PRIu64 " B) to give each of "
			    "%u workers a chunk-aligned slice (chunk=%" PRIu64
			    " B)\n",
			    (uint64_t)gmw_size, total_workers,
			    (uint64_t)g_chunk_order);
		return 1;
	}

	SPDK_ENV_FOREACH_CORE(core_id) {
		/* Skip the main thread's core */
		if (core_id == spdk_env_get_current_core()) {
			continue;
		}

		struct cpu_worker_thread *worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			SPDK_ERRLOG("Failed to allocate worker args.\n");
			rc = 1;
			goto cleanup;
		}

		worker->thread_idx = g_num_workers;
		worker->dst_buffer = (uint64_t *)((uint8_t *)g_output_buffer +
						  ((uint64_t)g_num_workers *
						   per_worker_slice));
		worker->dst_slice_size = per_worker_slice;
		worker->core_id = core_id;

		worker->next = g_cpu_workers;
		g_cpu_workers = worker;

		snprintf(thread_name, sizeof(thread_name), "ap_worker_%u", core_id);
		spdk_cpuset_zero(&tmp_cpumask);
		spdk_cpuset_set_cpu(&tmp_cpumask, core_id, true);

		currentthread = spdk_thread_create(thread_name, &tmp_cpumask);
		if (currentthread) {
			spdk_thread_send_msg(currentthread, ntb_cpu_perf_fn, worker);
		} else {
			printf("failed to create thread = %s", thread_name);
			rc = 1;
			goto cleanup;
		}

		g_num_workers++;
	}
cleanup:
	/*
	 * Wait for every dispatched worker to finish before freeing the
	 * cpu_worker_thread structs. Workers dispatched with
	 * spdk_thread_send_msg() run on other reactor threads and read their
	 * argument struct until completion, so unregister_cpu_workers() must
	 * not free those structs while a worker is still running -- that would
	 * be a use-after-free. g_num_workers counts only successfully
	 * dispatched workers, each of which bumps g_cpu_done exactly once
	 * (on every exit path), so this also covers the mid-dispatch
	 * goto-cleanup case.
	 */
	wait_for_workers_done(&g_cpu_done, g_num_workers);
	unregister_cpu_workers();

	return rc;
}

static int
ntb_perf_parse_args(int ch, char *arg)
{
	int argval = 0;
	(void)arg;

	switch (ch) {
	case 'O':
	case 'o':
	case 'w':
	case 'a':
	case 'k':
	case 'l':
	case 'M':
		argval = spdk_strtol(optarg, 10);
		if (argval < 0) {
			SPDK_ERRLOG("-%c option must be non-negative.\n", ch);
			ntb_perf_usage();
			return 1;
		}
		break;
	default:
		break;
	}

	switch (ch) {
	case 'O':
		if (argval > MAX_TOTAL_ORDER || argval < MIN_TOTAL_ORDER) {
			SPDK_ERRLOG("Total Order should be within: (%d to %d)\n",
				    MIN_TOTAL_ORDER, MAX_TOTAL_ORDER);
			ntb_perf_usage();
			return 1;
		}
		g_total_order_bits = (uint32_t)argval;
		g_total_order = 1ULL << argval;
		break;

	case 'o':
		if (argval < MIN_CHUNK_ORDER) {
			SPDK_ERRLOG("Chunk Order (%d) should be at least (%d)\n",
				    argval, MIN_CHUNK_ORDER);
			ntb_perf_usage();
			return 1;
		}
		/*
		 * Cross-check (-o <= -O) deferred until after parsing finishes
		 * so the user can pass -o and -O in either order. See
		 * ntb_perf_validate_args().
		 */
		g_chunk_order_bits = (uint32_t)argval;
		g_chunk_order = 1ULL << argval;
		break;

	case 'a':
		g_use_dmacopy = argval;
		break;
	case 'w':
		g_start_copy = argval;
		break;
	case 'k':
		g_user_dma_config.ae4dma_chan_num = (uint32_t)argval;
		break;
	case 'l':
		if (argval <= 0 || argval > AE4DMA_HW_QUEUES_MAX) {
			fprintf(stderr,
				"-l: hw queues per device must be in [1, %u]\n",
				AE4DMA_HW_QUEUES_MAX);
			return 1;
		}
		g_user_dma_config.ae4dma_hw_queues = (uint32_t)argval;
		break;
	case 'M':
		g_mw_index = (uint8_t)argval;
		break;
	default:
		ntb_perf_usage();
		return 1;
	}

	return 0;
}

/*
 * Cross-argument validation that has to wait until both -O and -o have been
 * observed (which we can't guarantee inside the per-flag callback). Returns
 * 0 if the configuration is consistent, non-zero otherwise.
 */
static int
ntb_perf_validate_args(void)
{
	if (g_total_order_bits != 0 && g_chunk_order_bits != 0 &&
	    g_chunk_order_bits > g_total_order_bits) {
		SPDK_ERRLOG("Chunk Order (%u) cannot exceed Total Order (%u)\n",
			    g_chunk_order_bits, g_total_order_bits);
		ntb_perf_usage();
		return 1;
	}
	return 0;
}

static void
ntb_perf_usage(void)
{
	printf("ntb_perf options:\n");
	printf("[-h help message]\n");
	printf("[-O total order]\n");
	printf("[-o chunk order]\n");
	printf("[-w initiate writing - copying]\n"
	       "  Initiates copy from Primary.\n"
	       "  To initiate from Secondary rebuild with changes.\n");
	printf("[-a use dma]\n");
	printf("[-k no.of.ae4dma engines to use]\n");
	printf("[-l h/w queues per engine - channels]\n");
	printf("[-M NTB memory window index to use (0..N-1)]\n");

}
static void
dump_user_config(void)
{
	printf("\nSPDK NTB-Perf Configuration:\n");
	printf("Core mask:      %s\n", g_opts.reactor_mask);

	printf(" NTB topo :  %s\n", gntb_topo ? "Primary" : "Secondary");
	printf(" NTB Memory Window's count : %u\n", (unsigned)gmw_cnt);
	printf(" NTB Memory Window size in MB : %" PRIu64 "\n", BYTES_TO_MB(gmw_size));
	printf(" Total order in MB : %" PRIu64 "\n", BYTES_TO_MB(g_total_order));
	printf(" Chunk order in KB : %" PRIu64 "\n", BYTES_TO_KB(g_chunk_order));
	printf(" Mode : %s\n", g_start_copy ? "Write mode" : "Read mode");
	printf(" Using DMA to copy  : %s\n\n", g_use_dmacopy ? "Yes" : "No");

}

static struct spdk_poller *ntb_db_poller = NULL;

static int
ntb_db_poller_cb(void *arg)
{
	struct spdk_ntb_amd_dev *ntb_hw = (struct spdk_ntb_amd_dev *)arg;
	uint64_t db_bits;
	uint32_t peer_mw_cnt;

	db_bits = spdk_ntb_amd_db_read(ntb_hw);
	if (db_bits == 0) {
		return SPDK_POLLER_BUSY;
	}

	if (db_bits & NTB_DB_PEER_READY_MASK) {
		uint64_t local_mw_cnt = 0;

		printf("DB%u: peer device ready\n", NTB_DB_PEER_READY_BIT);
		spdk_ntb_amd_db_clear(ntb_hw, NTB_DB_PEER_READY_MASK);

		if (spdk_ntb_amd_peer_ready_get(ntb_hw)) {
			/* Peer already up. All mw settings have already been done. */
			return SPDK_POLLER_IDLE;
		}

		/*
		 * Check the peer's MW configuration matches ours. Use the
		 * public attribute API for the local MW count instead of
		 * reaching into spdk_ntb_amd_dev — that struct is intentionally
		 * opaque outside the driver.
		 */
		if (spdk_ntb_amd_attr_get(ntb_hw, SPDK_NTB_MW_CNT_NAME, &local_mw_cnt) != 0) {
			SPDK_ERRLOG("Failed to query local MW count.\n");
			return SPDK_POLLER_IDLE;
		}
		if (spdk_ntb_amd_spad_read(ntb_hw, SPAD_NUM_MWS, 1, &peer_mw_cnt) != 0) {
			SPDK_ERRLOG("Failed to read peer MW count.\n");
			return SPDK_POLLER_IDLE;
		}
		if ((uint64_t)peer_mw_cnt != local_mw_cnt) {
			SPDK_ERRLOG("MW count mismatch: peer %u, local %" PRIu64 "\n",
				    peer_mw_cnt, local_mw_cnt);
			return SPDK_POLLER_IDLE;
		}

		spdk_ntb_amd_peer_ready_set(ntb_hw, true);
		spdk_ntb_amd_get_link_status(ntb_hw);

		/*
		 * Handshake with peer. Spad_write & mw_set_trans only work
		 * once both devices are up, so re-run the handshake when the
		 * doorbell is received and ring the bell again so a peer that
		 * missed our first ring will see this one.
		 */
		if (spdk_ntb_amd_handshake_work(ntb_hw, conf->buffer,
						g_handshake_buffer_size) < 0) {
			SPDK_ERRLOG("Handshake work failed.\n");
			return SPDK_POLLER_IDLE;
		}
	} else if (db_bits & NTB_DB_PEER_DOWN_MASK) {
		printf("DB%u: peer device down\n", NTB_DB_PEER_DOWN_BIT);
		spdk_ntb_amd_db_clear(ntb_hw, NTB_DB_PEER_DOWN_MASK);
		spdk_ntb_amd_peer_ready_set(ntb_hw, false);
		/* Acknowledge peer's dev_stop request. */
		spdk_ntb_amd_peer_db_set(ntb_hw, NTB_DB_PEER_ACK_DOWN_BIT);
	} else if (db_bits & NTB_DB_PEER_ACK_DOWN_MASK) {
		printf("DB%u: peer agrees device should be down\n",
		       NTB_DB_PEER_ACK_DOWN_BIT);
		spdk_ntb_amd_db_clear(ntb_hw, NTB_DB_PEER_ACK_DOWN_MASK);
		spdk_ntb_amd_peer_ready_set(ntb_hw, false);
	} else if (db_bits & NTB_DB_COPY_DONE_MASK) {
		printf("DB%u: copy done\n", NTB_DB_COPY_DONE_BIT);
		spdk_ntb_amd_db_clear(ntb_hw, NTB_DB_COPY_DONE_MASK);
		__atomic_store_n(&g_copy_done, 1, __ATOMIC_RELEASE);
	} else {
		/*
		 * Unknown doorbell bit set: clear it so we don't busy-loop on
		 * it forever, then keep going.
		 */
		SPDK_WARNLOG("Unhandled doorbell bits 0x%" PRIx64 "\n", db_bits);
		spdk_ntb_amd_db_clear(ntb_hw, db_bits);
	}

	return SPDK_POLLER_IDLE;
}

static void
ntb_main_thread_cb(void *ctx)
{
	int rc = 0;
	uint64_t tmp = 0;

	(void)ctx;

	if (ntb_init() != 0) {
		spdk_app_stop(1);
		return;
	}


	if (ntb_get_attr(ntb_dev->ntb, SPDK_NTB_MW_CNT_NAME, &tmp) != 0) {
		printf("no memory windows\n");
	} else {
		gmw_cnt = (uint8_t)tmp;
		printf("NTB memory Window Count %u\n", gmw_cnt);
	}

	if (g_mw_index >= gmw_cnt) {
		printf("Invalid memory window index %u (available count %u)\n",
		       g_mw_index, gmw_cnt);
		rc = -EINVAL;
		goto cleanup;
	}

	if (spdk_ntb_amd_set_active_mw(ntb_dev->ntb, g_mw_index) != 0) {
		printf("Failed to set active NTB memory window index %u\n", g_mw_index);
		rc = -EINVAL;
		goto cleanup;
	}

	{
		struct iovec mw_iov;

		if (spdk_ntb_amd_get_peer_mw(ntb_dev->ntb, g_mw_index, &mw_iov,
					     NULL, NULL) != 0) {
			printf("Failed to get memory window size\n");
			rc = -EINVAL;
			goto cleanup;
		}
		gmw_size = mw_iov.iov_len;
		printf("NTB memory Window Size 0x%" PRIx64 "\n", gmw_size);
	}

	/*
	 * Each transfer copies a whole g_chunk_order-byte chunk into the
	 * window. The DMA path wraps the starting offset within the window but
	 * still copies the full chunk, so a chunk larger than the window would
	 * run past its end. Reject that here now that the size is known.
	 */
	if (g_chunk_order > gmw_size) {
		printf("Chunk size 0x%" PRIx64 " exceeds memory window size 0x%" PRIx64 "\n",
		       g_chunk_order, gmw_size);
		rc = -EINVAL;
		goto cleanup;
	}

	{
		uint64_t tmp = 0;

		if (ntb_get_attr(ntb_dev->ntb, SPDK_NTB_TOPO_NAME, &tmp) != 0) {
			printf("Failed to get topology\n");
		} else {
			gntb_topo = (int8_t)tmp;
			printf("NTB topo %d\n", gntb_topo);
		}
	}

	rc = ntb_dev_configure(ntb_dev->ntb);
	if (rc != 0) {
		SPDK_ERRLOG("ntb_dev_configure() failed: %d\n", rc);
		goto cleanup;
	}

	struct spdk_thread *thread = spdk_get_thread();
	if (thread == NULL) {
		SPDK_ERRLOG("spdk_get_thread() returned NULL.\n");
		rc = -1;
		goto cleanup;
	}

	SPDK_NOTICELOG("Main thread callback started. Registering poller with 1ms interval.\n");

	/* Register the poller */
	ntb_db_poller = spdk_poller_register(ntb_db_poller_cb, ntb_dev->ntb, 1000);

	if (ntb_db_poller == NULL) {
		SPDK_ERRLOG("Failed to register poller.\n");
		rc = -1;
		goto cleanup;
	}

	SPDK_NOTICELOG("Poller successfully registered.\n");

	/*
	 * Install our own SIGINT/SIGTERM handler now that the app is running.
	 * The bring-up below runs long, peer-dependent waits directly on the
	 * reactor thread, which blocks SPDK's default signal-shutdown poller;
	 * this handler lets those loops observe g_shutdown and unwind cleanly.
	 */
	signal(SIGINT, ntb_perf_sig_handler);
	signal(SIGTERM, ntb_perf_sig_handler);

	spdk_ntb_amd_handshake_work(ntb_dev->ntb, conf->buffer,
				    g_handshake_buffer_size);

	/* Waiting for peer dev up at most 100s */
	printf("Checking ntb link status...\n");

	for (int i = 0; i < 1000; i++) {
		uint64_t link_tmp = 0;

		if (g_shutdown) {
			printf("Interrupted while waiting for peer; shutting down.\n");
			rc = -1;
			goto cleanup;
		}

		spdk_thread_poll(thread, 0, 0);
		ntb_get_attr(ntb_dev->ntb, SPDK_NTB_LINK_STATUS_NAME, &link_tmp);
		ntb_link_status = (int64_t)link_tmp;
		if (ntb_link_status) {
			printf("Peer dev ready, ntb link up.\n");
			break;
		}
		usleep(NTB_LINK_POLL_TIMEOUT);
	}

	if (!ntb_link_status) {
		printf("link is down\n");
		rc = -1;
		goto cleanup;
	}

	spdk_ntb_amd_dev_start(ntb_dev->ntb);

	{
		struct iovec mw_iov;
		void *mw_vaddr = NULL;

		if (spdk_ntb_amd_get_peer_mw(ntb_dev->ntb, g_mw_index, &mw_iov,
					     &g_mw_domain, &mw_vaddr) != 0) {
			printf("Failed to get peer memory window.\n");
			rc = -1;
			goto cleanup;
		}
		/* Virtual address for CPU memcpy; bus address (+domain) for DMA. */
		g_output_buffer = (uint64_t *)mw_vaddr;
		g_output_buffer_phys = (uint64_t *)mw_iov.iov_base;
	}

	g_total_buffers = g_total_order / g_chunk_order;
	printf("total buffers = %" PRIu64 "; total_order = %" PRIu64 "; chunk_order = %" PRIu64 ";\n",
	       g_total_buffers, g_total_order, g_chunk_order);

	dump_user_config();

	if ((gntb_topo == NTB_TOPO_PRI) || (gntb_topo == NTB_TOPO_SEC)) {

		if (g_start_copy && (gntb_topo == NTB_TOPO_PRI)) { /* change to SEC for secondary write mode */
			/* write mode */
			if (g_use_dmacopy) {
				printf(" DMA copy mode\n");
				rc = ntb_dma_copy();
			} else {
				printf(" CPU copy mode\n");
				rc = ntb_cpu_copy();
			}
			/*
			 * If the copy failed, the peer's window may hold partial
			 * or no data. Don't ring the copy-done doorbell (which
			 * would tell the peer to consume it) and propagate the
			 * error to spdk_app_stop().
			 */
			if (rc != 0) {
				SPDK_ERRLOG("%s copy failed (%d); skipping copy-done doorbell\n",
					    g_use_dmacopy ? "DMA" : "CPU", rc);
				goto cleanup;
			}
			/* Tell peer the copy is done. */
			int ret = spdk_ntb_amd_peer_db_set(ntb_dev->ntb, NTB_DB_COPY_DONE_BIT);
			if (ret < 0) {
				printf("failed to set copy-done doorbell (%d)\n", ret);
				rc = ret;
				goto cleanup;
			} else {
				printf("copy-done doorbell triggered\n");
			}
		} else {
			/*
			 * Read mode.
			* conf->buffer holds the entire sum(mw_size) region with each MW
			 * laid out at offset (i * gmw_size). The peer wrote
			 * into the slice owned by the active MW, so anchor
			 * our scan at that offset (not the start of the
			 * whole staging buffer).
			 */
			char *data_ptr_base = (char *)conf->buffer +
					      ((size_t)g_mw_index * gmw_size);
			char *data_ptr = data_ptr_base;
			char *data_ptr_end = data_ptr_base + gmw_size;
			while (1) {
				if (g_shutdown) {
					printf("Interrupted while waiting for copy-done; shutting down.\n");
					rc = -1;
					goto cleanup;
				}
				spdk_thread_poll(thread, 0, 0);
				usleep(1000);
				if (__atomic_load_n(&g_copy_done, __ATOMIC_ACQUIRE)) {
					/*
					 * Bound the scan by the memory window size.
					 */
					while (data_ptr < data_ptr_end && *data_ptr) {
						data_ptr++;
					}
					printf("Data read size in bytes: %td\n", data_ptr - data_ptr_base);
					break;
				}
			}
		}
	}

cleanup:
	ntb_exit();
	spdk_poller_unregister(&ntb_db_poller);
	spdk_app_stop(rc);
}

int
main(int argc, char **argv)
{
	int rc;

	g_total_order = TOTAL_ORDER;
	g_chunk_order = CHUNK_ORDER;

	spdk_app_opts_init(&g_opts, sizeof(g_opts));
	g_opts.name = "ntb_perf";
	g_opts.reactor_mask = "0x1";

	construct_user_dma_config(&g_user_dma_config);

	rc = spdk_app_parse_args(argc, argv, &g_opts, "O:o:w:a:k:l:M:", NULL,
				 ntb_perf_parse_args, ntb_perf_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc == SPDK_APP_PARSE_ARGS_HELP ? 0 : 1;
	}

	if (ntb_perf_validate_args() != 0) {
		return 1;
	}

	rc = spdk_app_start(&g_opts, ntb_main_thread_cb, NULL);
	if (rc) {
		SPDK_ERRLOG("Error in starting SPDK application: %s\n", spdk_strerror(rc));
	}

	SPDK_NOTICELOG("Application shut down successfully.\n");

	spdk_app_fini();
	return rc;
}
