/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <pciaccess.h>
#include "spdk/ioat.h"
#include "spdk/pci.h"
#include "spdk/string.h"

#define SRC_BUFFER_SIZE (512*1024)

struct user_config {
	int queue_depth;
	int time_in_sec;
	char *core_mask;
};

struct ioat_device {
	struct ioat_channel *ioat;
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
	void *buffer;
	int len;
	void *src;
	void *dst;
};

static __thread unsigned int seed = 0;

static unsigned char *g_src;

static void submit_single_xfer(struct ioat_task *ioat_task);

static void
construct_user_config(struct user_config *self)
{
	self->queue_depth = 32;
	self->time_in_sec = 10;
	self->core_mask = "0x1";
}

static void
dump_user_config(struct user_config *self)
{
	printf("User configuration:\n");
	printf("Run time:       %u seconds\n", self->time_in_sec);
	printf("Core mask:      %s\n", self->core_mask);
	printf("Queue depth:    %u\n", self->queue_depth);
}

static void
ioat_exit(void)
{
	struct ioat_device *dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		if (dev->ioat) {
			ioat_detach(dev->ioat);
		}
		free(dev);
	}
}
static void prepare_ioat_task(struct thread_entry *thread_entry, struct ioat_task *ioat_task)
{
	int len;
	int src_offset;
	int dst_offset;

	src_offset = rand_r(&seed) % SRC_BUFFER_SIZE;
	len = rand_r(&seed) % (SRC_BUFFER_SIZE - src_offset);
	dst_offset = rand_r(&seed) % (SRC_BUFFER_SIZE - len);

	memset(ioat_task->buffer, 0, SRC_BUFFER_SIZE);
	ioat_task->len = len;
	ioat_task->src =  g_src + src_offset;
	ioat_task->dst = ioat_task->buffer + dst_offset;
	ioat_task->thread_entry = thread_entry;
}

static void
ioat_done(void *cb_arg)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct thread_entry *thread_entry = ioat_task->thread_entry;

	if (memcmp(ioat_task->src, ioat_task->dst, ioat_task->len)) {
		thread_entry->xfer_failed++;
	} else {
		thread_entry->xfer_completed++;
	}
	thread_entry->current_queue_depth--;
	if (thread_entry->is_draining) {
		rte_mempool_put(thread_entry->data_pool, ioat_task->buffer);
		rte_mempool_put(thread_entry->task_pool, ioat_task);
	} else {
		prepare_ioat_task(thread_entry, ioat_task);
		submit_single_xfer(ioat_task);
	}
}

static int
ioat_init(void)
{
	struct pci_device_iterator *iter;
	struct pci_device *pci_dev;
	int err = 0;
	struct pci_id_match match;
	struct ioat_device *dev;

	pci_system_init();
	TAILQ_INIT(&g_devices);

	match.vendor_id		= PCI_MATCH_ANY;
	match.subvendor_id	= PCI_MATCH_ANY;
	match.subdevice_id	= PCI_MATCH_ANY;
	match.device_id		= PCI_MATCH_ANY;
	match.device_class	= 0x088000;
	match.device_class_mask	= 0xFFFFFF;

	iter = pci_id_match_iterator_create(&match);

	while ((pci_dev = pci_device_next(iter)) != NULL) {
		/* Check if the PCI devices is a supported IOAT channel. */
		if (!(ioat_pci_device_match_id(pci_dev->vendor_id,
					       pci_dev->device_id))) {
			continue;
		}

		printf(" Found matching device at %d:%d:%d "
		       "vendor:0x%04x device:0x%04x\n   name:%s\n",
		       pci_dev->bus, pci_dev->dev, pci_dev->func,
		       pci_dev->vendor_id, pci_dev->device_id,
		       pci_device_get_device_name(pci_dev));

		if (pci_device_has_non_null_driver(pci_dev)) {
			printf("Device has kernel driver, skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		dev = malloc(sizeof(*dev));
		if (dev == NULL) {
			printf("Failed to allocate device struct\n");
			err = -1;
			goto cleanup;
		}
		memset(dev, 0, sizeof(*dev));

		dev->ioat = ioat_attach(pci_dev);
		if (dev->ioat == NULL) {
			free(dev);
			/* Likely no device found. */
			err = -1;
			goto cleanup;
		}
		TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
	}

cleanup:
	pci_iterator_destroy(iter);
	if (err != 0) {
		ioat_exit();
	}
	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-h help message]\n");
	printf("\t[-c core mask for distributing I/O submission/completion work]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-q queue depth]\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	construct_user_config(&g_user_config);
	while ((op = getopt(argc, argv, "c:ht:q:")) != -1) {
		switch (op) {
		case 't':
			g_user_config.time_in_sec = atoi(optarg);
			break;
		case 'c':
			g_user_config.core_mask = optarg;
			break;
		case 'q':
			g_user_config.queue_depth = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (!g_user_config.time_in_sec || !g_user_config.core_mask || !g_user_config.queue_depth) {
		usage(argv[0]);
		return 1;
	}
	optind = 1;
	return 0;
}

static void
drain_xfers(struct thread_entry *thread_entry)
{
	while (thread_entry->current_queue_depth > 0) {
		ioat_process_events();
	}
}

static void
submit_single_xfer(struct ioat_task *ioat_task)
{
	ioat_submit_copy(ioat_task, ioat_done, ioat_task->dst, ioat_task->src, ioat_task->len);
	ioat_task->thread_entry->current_queue_depth++;
}

static void
submit_xfers(struct thread_entry *thread_entry, uint64_t queue_depth)
{
	while (queue_depth-- > 0) {
		struct ioat_task *ioat_task = NULL;
		rte_mempool_get(thread_entry->task_pool, (void **)&ioat_task);
		rte_mempool_get(thread_entry->data_pool, &(ioat_task->buffer));

		prepare_ioat_task(thread_entry, ioat_task);
		submit_single_xfer(ioat_task);
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	char buf_pool_name[20], task_pool_name[20];
	struct thread_entry *t = (struct thread_entry *)arg;

	t->lcore_id = rte_lcore_id();

	snprintf(buf_pool_name, sizeof(buf_pool_name), "buf_pool_%d", rte_lcore_id());
	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", rte_lcore_id());
	t->data_pool = rte_mempool_create(buf_pool_name, g_user_config.queue_depth, SRC_BUFFER_SIZE, 0, 0,
					  NULL, NULL,
					  NULL, NULL, SOCKET_ID_ANY, 0);
	t->task_pool = rte_mempool_create(task_pool_name, g_user_config.queue_depth,
					  sizeof(struct ioat_task), 0, 0, NULL, NULL,
					  NULL, NULL, SOCKET_ID_ANY, 0);
	if (!t->data_pool || !t->task_pool) {
		fprintf(stderr, "Could not allocate buffer pool.\n");
		return 1;
	}

	if (ioat_register_thread() != 0) {
		fprintf(stderr, "lcore %u: No ioat channels found. Check that ioatdma driver is unloaded.\n",
			rte_lcore_id());
		return 0;
	}

	tsc_end = rte_get_timer_cycles() + g_user_config.time_in_sec * rte_get_timer_hz();

	submit_xfers(t, g_user_config.queue_depth);
	while (rte_get_timer_cycles() < tsc_end) {
		ioat_process_events();
	}

	t->is_draining = true;
	drain_xfers(t);

	ioat_unregister_thread();

	return 0;
}

static int
init_src_buffer(void)
{
	int i;

	g_src = rte_malloc(NULL, SRC_BUFFER_SIZE, 512);
	if (g_src == NULL) {
		fprintf(stderr, "Allocate src buffer failed\n");
		return -1;
	}

	for (i = 0; i < SRC_BUFFER_SIZE / 4; i++) {
		memset((g_src + (4 * i)), i, 4);
	}

	return 0;
}

static int
init(void)
{
	char *core_mask_conf;

	core_mask_conf = sprintf_alloc("-c %s", g_user_config.core_mask);
	if (!core_mask_conf) {
		return 1;
	}

	char *ealargs[] = {"verify", core_mask_conf, "-n 4", "--no-pci"};
	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		free(core_mask_conf);
		fprintf(stderr, "Could not init eal\n");
		return 1;
	}

	free(core_mask_conf);

	if (init_src_buffer() != 0) {
		fprintf(stderr, "Could not init src buffer\n");
		return 1;
	}
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

	for (i = 0; i < len; i++) {
		struct thread_entry *t = &threads[i];
		total_completed += t->xfer_completed;
		total_failed += t->xfer_failed;
		if (t->xfer_completed || t->xfer_failed)
			printf("lcore = %d, success = %ld, failed = %ld \n",
			       t->lcore_id, t->xfer_completed, t->xfer_failed);
	}
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
	rte_free(g_src);
	ioat_exit();

	return rc;
}
