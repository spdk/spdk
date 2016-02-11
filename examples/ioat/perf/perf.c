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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mempool.h>

#include "spdk/ioat.h"
#include "spdk/pci.h"
#include "spdk/string.h"

struct user_config {
	int xfer_size_bytes;
	int queue_depth;
	int time_in_sec;
	bool verify;
	char *core_mask;
};

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	TAILQ_ENTRY(ioat_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices;

static struct user_config g_user_config;

struct thread_entry {
	uint64_t xfer_completed;
	uint64_t xfer_failed;
	uint64_t current_queue_depth;
	unsigned lcore_id;
	bool is_draining;
	struct rte_mempool *data_pool;
	struct rte_mempool *task_pool;
};

struct ioat_task {
	struct thread_entry *thread_entry;
	void *src;
	void *dst;
};

static void submit_single_xfer(struct thread_entry *thread_entry, struct ioat_task *ioat_task,
			       void *dst, void *src);

static void
construct_user_config(struct user_config *self)
{
	self->xfer_size_bytes = 4096;
	self->queue_depth = 256;
	self->time_in_sec = 10;
	self->verify = false;
	self->core_mask = "0x1";
}

static void
dump_user_config(struct user_config *self)
{
	printf("User configuration:\n");
	printf("Transfer size:  %u bytes\n", self->xfer_size_bytes);
	printf("Queue depth:    %u\n", self->queue_depth);
	printf("Run time:       %u seconds\n", self->time_in_sec);
	printf("Core mask:      %s\n", self->core_mask);
	printf("Verify:         %s\n\n", self->verify ? "Yes" : "No");
}

static void
ioat_exit(void)
{
	struct ioat_device *dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		if (dev->ioat) {
			spdk_ioat_detach(dev->ioat);
		}
		rte_free(dev);
	}
}

static void
ioat_done(void *cb_arg)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct thread_entry *thread_entry = ioat_task->thread_entry;

	if (g_user_config.verify && memcmp(ioat_task->src, ioat_task->dst, g_user_config.xfer_size_bytes)) {
		thread_entry->xfer_failed++;
	} else {
		thread_entry->xfer_completed++;
	}

	thread_entry->current_queue_depth--;

	if (thread_entry->is_draining) {
		rte_mempool_put(thread_entry->data_pool, ioat_task->src);
		rte_mempool_put(thread_entry->data_pool, ioat_task->dst);
		rte_mempool_put(thread_entry->task_pool, ioat_task);
	} else {
		submit_single_xfer(thread_entry, ioat_task, ioat_task->dst, ioat_task->src);
	}
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	printf(" Found matching device at %d:%d:%d "
	       "vendor:0x%04x device:0x%04x\n   name:%s\n",
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev),
	       spdk_pci_device_get_vendor_id(pci_dev), spdk_pci_device_get_device_id(pci_dev),
	       spdk_pci_device_get_device_name(pci_dev));

	if (spdk_pci_device_has_non_uio_driver(pci_dev)) {
		printf("Device has non-uio kernel driver, skipping...\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	dev = rte_malloc(NULL, sizeof(*dev), 0);
	if (dev == NULL) {
		printf("Failed to allocate device struct\n");
		return;
	}

	dev->ioat = ioat;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

static int
ioat_init(void)
{
	TAILQ_INIT(&g_devices);

	if (spdk_ioat_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "ioat_probe() failed\n");
		return 1;
	}

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-h help message]\n");
	printf("\t[-c core mask for distributing I/O submission/completion work]\n");
	printf("\t[-q queue depth]\n");
	printf("\t[-s transfer size in bytes]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-v verify copy result if this switch is on]\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	construct_user_config(&g_user_config);
	while ((op = getopt(argc, argv, "c:hq:s:t:v")) != -1) {
		switch (op) {
		case 's':
			g_user_config.xfer_size_bytes = atoi(optarg);
			break;
		case 'q':
			g_user_config.queue_depth = atoi(optarg);
			break;
		case 't':
			g_user_config.time_in_sec = atoi(optarg);
			break;
		case 'c':
			g_user_config.core_mask = optarg;
			break;
		case 'v':
			g_user_config.verify = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (!g_user_config.xfer_size_bytes || !g_user_config.queue_depth ||
	    !g_user_config.time_in_sec || !g_user_config.core_mask) {
		usage(argv[0]);
		return 1;
	}
	optind = 1;
	return 0;
}

static void
drain_io(struct thread_entry *thread_entry)
{
	while (thread_entry->current_queue_depth > 0) {
		spdk_ioat_process_events();
	}
}

static void
submit_single_xfer(struct thread_entry *thread_entry, struct ioat_task *ioat_task, void *dst,
		   void *src)
{
	ioat_task->thread_entry = thread_entry;
	ioat_task->src = src;
	ioat_task->dst = dst;

	spdk_ioat_submit_copy(ioat_task, ioat_done, dst, src, g_user_config.xfer_size_bytes);

	thread_entry->current_queue_depth++;
}

static void
submit_xfers(struct thread_entry *thread_entry, uint64_t queue_depth)
{
	while (queue_depth-- > 0) {
		void *src = NULL, *dst = NULL;
		struct ioat_task *ioat_task = NULL;

		rte_mempool_get(thread_entry->data_pool, &src);
		rte_mempool_get(thread_entry->data_pool, &dst);
		rte_mempool_get(thread_entry->task_pool, (void **)&ioat_task);

		submit_single_xfer(thread_entry, ioat_task, dst, src);
	}
}

static int
work_fn(void *arg)
{
	char buf_pool_name[20], task_pool_name[20];
	uint64_t tsc_end;
	struct thread_entry *t = (struct thread_entry *)arg;

	t->lcore_id = rte_lcore_id();

	snprintf(buf_pool_name, sizeof(buf_pool_name), "buf_pool_%d", rte_lcore_id());
	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", rte_lcore_id());
	t->data_pool = rte_mempool_create(buf_pool_name, 512, g_user_config.xfer_size_bytes, 0, 0, NULL,
					  NULL,
					  NULL, NULL, SOCKET_ID_ANY, 0);
	t->task_pool = rte_mempool_create(task_pool_name, 512, sizeof(struct ioat_task), 0, 0, NULL, NULL,
					  NULL, NULL, SOCKET_ID_ANY, 0);
	if (!t->data_pool || !t->task_pool) {
		fprintf(stderr, "Could not allocate buffer pool.\n");
		return 1;
	}

	if (spdk_ioat_register_thread() != 0) {
		fprintf(stderr, "lcore %u: No ioat channels found. Check that ioatdma driver is unloaded.\n",
			rte_lcore_id());
		return 0;
	}

	tsc_end = rte_get_timer_cycles() + g_user_config.time_in_sec * rte_get_timer_hz();

	// begin to submit transfers
	submit_xfers(t, g_user_config.queue_depth);
	while (rte_get_timer_cycles() < tsc_end) {
		spdk_ioat_process_events();
	}

	// begin to drain io
	t->is_draining = true;
	drain_io(t);
	spdk_ioat_unregister_thread();

	return 0;
}

static int
init(void)
{
	char *core_mask_conf;

	core_mask_conf = spdk_sprintf_alloc("-c %s", g_user_config.core_mask);
	if (!core_mask_conf) {
		return 1;
	}

	char *ealargs[] = {"perf", core_mask_conf, "-n 4", "--no-pci"};

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		free(core_mask_conf);
		fprintf(stderr, "Could not init eal\n");
		return 1;
	}

	free(core_mask_conf);

	if (ioat_init() != 0) {
		fprintf(stderr, "Could not init ioat\n");
		return 1;
	}

	return 0;
}

static int
dump_result(struct thread_entry *threads, int len)
{
	int i;
	uint64_t total_completed = 0;
	uint64_t total_failed = 0;
	uint64_t total_xfer_per_sec, total_bw_in_MBps;

	printf("lcore     Transfers        Bandwidth  Failed\n");
	printf("--------------------------------------------\n");
	for (i = 0; i < len; i++) {
		struct thread_entry *t = &threads[i];

		uint64_t xfer_per_sec = t->xfer_completed / g_user_config.time_in_sec;
		uint64_t bw_in_MBps = (t->xfer_completed * g_user_config.xfer_size_bytes) /
				      (g_user_config.time_in_sec * 1024 * 1024);

		total_completed += t->xfer_completed;
		total_failed += t->xfer_failed;

		if (xfer_per_sec) {
			printf("%5d  %10" PRIu64 "/s  %10" PRIu64 " MB/s  %6" PRIu64 "\n",
			       t->lcore_id, xfer_per_sec, bw_in_MBps, t->xfer_failed);
		}
	}

	total_xfer_per_sec = total_completed / g_user_config.time_in_sec;
	total_bw_in_MBps = (total_completed * g_user_config.xfer_size_bytes) /
			   (g_user_config.time_in_sec * 1024 * 1024);

	printf("============================================\n");
	printf("Total: %10" PRIu64 "/s  %10" PRIu64 " MB/s  %6" PRIu64 "\n",
	       total_xfer_per_sec, total_bw_in_MBps, total_failed);
	return total_failed ? 1 : 0;
}

int
main(int argc, char **argv)
{
	unsigned lcore_id;
	struct thread_entry threads[RTE_MAX_LCORE] = {};
	int rc;

	if (parse_args(argc, argv) != 0) {
		return 1;
	}

	if (init() != 0) {
		return 1;
	}

	dump_user_config(&g_user_config);

	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(work_fn, &threads[lcore_id], lcore_id);
	}

	if (work_fn(&threads[rte_get_master_lcore()]) != 0) {
		rc = 1;
		goto cleanup;
	}

	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) != 0) {
			rc = 1;
			goto cleanup;
		}
	}

	rc = dump_result(threads, RTE_MAX_LCORE);

cleanup:
	ioat_exit();

	return rc;
}
