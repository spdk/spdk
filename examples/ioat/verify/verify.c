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

#include "spdk/stdinc.h"

#include <rte_config.h>
#include <rte_lcore.h>

#include "spdk/ioat.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/string.h"

#define SRC_BUFFER_SIZE (512*1024)

enum ioat_task_type {
	IOAT_COPY_TYPE,
	IOAT_FILL_TYPE,
};

struct user_config {
	int queue_depth;
	int time_in_sec;
	char *core_mask;
};

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	TAILQ_ENTRY(ioat_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices;
static struct ioat_device *g_next_device;

static struct user_config g_user_config;

struct thread_entry {
	struct spdk_ioat_chan *chan;
	uint64_t xfer_completed;
	uint64_t xfer_failed;
	uint64_t fill_completed;
	uint64_t fill_failed;
	uint64_t current_queue_depth;
	unsigned lcore_id;
	bool is_draining;
	struct spdk_mempool *data_pool;
	struct spdk_mempool *task_pool;
};

struct ioat_task {
	enum ioat_task_type type;
	struct thread_entry *thread_entry;
	void *buffer;
	int len;
	uint64_t fill_pattern;
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
			spdk_ioat_detach(dev->ioat);
		}
		free(dev);
	}
}
static void prepare_ioat_task(struct thread_entry *thread_entry, struct ioat_task *ioat_task)
{
	int len;
	int src_offset;
	int dst_offset;
	int num_ddwords;
	uint64_t fill_pattern;

	if (ioat_task->type == IOAT_FILL_TYPE) {
		fill_pattern = rand_r(&seed);
		fill_pattern = fill_pattern << 32 | rand_r(&seed);

		/* ensure that the length of memset block is 8 Bytes aligned */
		num_ddwords = (rand_r(&seed) % SRC_BUFFER_SIZE) / 8;
		len = num_ddwords * 8;
		if (len < 8)
			len = 8;
		dst_offset = rand_r(&seed) % (SRC_BUFFER_SIZE - len);
		ioat_task->fill_pattern = fill_pattern;
	} else {
		src_offset = rand_r(&seed) % SRC_BUFFER_SIZE;
		len = rand_r(&seed) % (SRC_BUFFER_SIZE - src_offset);
		dst_offset = rand_r(&seed) % (SRC_BUFFER_SIZE - len);

		memset(ioat_task->buffer, 0, SRC_BUFFER_SIZE);
		ioat_task->src =  g_src + src_offset;
	}
	ioat_task->len = len;
	ioat_task->dst = ioat_task->buffer + dst_offset;
	ioat_task->thread_entry = thread_entry;
}

static void
ioat_done(void *cb_arg)
{
	char *value;
	int i, failed = 0;
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct thread_entry *thread_entry = ioat_task->thread_entry;

	if (ioat_task->type == IOAT_FILL_TYPE) {
		value = ioat_task->dst;
		for (i = 0; i < ioat_task->len / 8; i++) {
			if (memcmp(value, &ioat_task->fill_pattern, 8) != 0) {
				thread_entry->fill_failed++;
				failed = 1;
				break;
			}
			value += 8;
		}
		if (!failed)
			thread_entry->fill_completed++;
	} else {
		if (memcmp(ioat_task->src, ioat_task->dst, ioat_task->len)) {
			thread_entry->xfer_failed++;
		} else {
			thread_entry->xfer_completed++;
		}
	}

	thread_entry->current_queue_depth--;
	if (thread_entry->is_draining) {
		spdk_mempool_put(thread_entry->data_pool, ioat_task->buffer);
		spdk_mempool_put(thread_entry->task_pool, ioat_task);
	} else {
		prepare_ioat_task(thread_entry, ioat_task);
		submit_single_xfer(ioat_task);
	}
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	printf(" Found matching device at %04x:%02x:%02x.%x "
	       "vendor:0x%04x device:0x%04x\n",
	       spdk_pci_device_get_domain(pci_dev),
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev),
	       spdk_pci_device_get_vendor_id(pci_dev), spdk_pci_device_get_device_id(pci_dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	dev = malloc(sizeof(*dev));
	if (dev == NULL) {
		printf("Failed to allocate device struct\n");
		return;
	}
	memset(dev, 0, sizeof(*dev));

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

	return 0;
}

static void
drain_xfers(struct thread_entry *thread_entry)
{
	while (thread_entry->current_queue_depth > 0) {
		spdk_ioat_process_events(thread_entry->chan);
	}
}

static void
submit_single_xfer(struct ioat_task *ioat_task)
{
	if (ioat_task->type == IOAT_FILL_TYPE)
		spdk_ioat_submit_fill(ioat_task->thread_entry->chan, ioat_task, ioat_done,
				      ioat_task->dst, ioat_task->fill_pattern, ioat_task->len);
	else
		spdk_ioat_submit_copy(ioat_task->thread_entry->chan, ioat_task, ioat_done,
				      ioat_task->dst, ioat_task->src, ioat_task->len);
	ioat_task->thread_entry->current_queue_depth++;
}

static void
submit_xfers(struct thread_entry *thread_entry, uint64_t queue_depth)
{
	while (queue_depth-- > 0) {
		struct ioat_task *ioat_task = NULL;
		ioat_task = spdk_mempool_get(thread_entry->task_pool);
		ioat_task->buffer = spdk_mempool_get(thread_entry->data_pool);

		ioat_task->type = IOAT_COPY_TYPE;
		if (spdk_ioat_get_dma_capabilities(thread_entry->chan) & SPDK_IOAT_ENGINE_FILL_SUPPORTED) {
			if (queue_depth % 2)
				ioat_task->type = IOAT_FILL_TYPE;
		}
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

	if (!t->chan) {
		return 0;
	}

	t->lcore_id = rte_lcore_id();

	snprintf(buf_pool_name, sizeof(buf_pool_name), "buf_pool_%d", rte_lcore_id());
	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", rte_lcore_id());
	t->data_pool = spdk_mempool_create(buf_pool_name, g_user_config.queue_depth, SRC_BUFFER_SIZE,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	t->task_pool = spdk_mempool_create(task_pool_name, g_user_config.queue_depth,
					   sizeof(struct ioat_task),
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);
	if (!t->data_pool || !t->task_pool) {
		fprintf(stderr, "Could not allocate buffer pool.\n");
		return 1;
	}

	tsc_end = spdk_get_ticks() + g_user_config.time_in_sec * spdk_get_ticks_hz();

	submit_xfers(t, g_user_config.queue_depth);
	while (spdk_get_ticks() < tsc_end) {
		spdk_ioat_process_events(t->chan);
	}

	t->is_draining = true;
	drain_xfers(t);

	return 0;
}

static int
init_src_buffer(void)
{
	int i;

	g_src = spdk_dma_zmalloc(SRC_BUFFER_SIZE, 512, NULL);
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
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	opts.name = "verify";
	opts.core_mask = g_user_config.core_mask;
	spdk_env_init(&opts);

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
		total_completed += t->fill_completed;
		total_failed += t->xfer_failed;
		total_failed += t->fill_failed;
		if (t->xfer_completed || t->xfer_failed)
			printf("lcore = %d, copy success = %ld, copy failed = %ld, fill success = %ld, fill failed = %ld \n",
			       t->lcore_id, t->xfer_completed, t->xfer_failed, t->fill_completed, t->fill_failed);
	}
	return total_failed ? 1 : 0;
}

static struct spdk_ioat_chan *
get_next_chan(void)
{
	struct spdk_ioat_chan *chan;

	if (g_next_device == NULL) {
		fprintf(stderr, "Not enough ioat channels found. Check that ioatdma driver is unloaded.\n");
		return NULL;
	}

	chan = g_next_device->ioat;

	g_next_device = TAILQ_NEXT(g_next_device, tailq);

	return chan;
}

int
main(int argc, char **argv)
{
	uint32_t i, current_core;
	struct thread_entry threads[RTE_MAX_LCORE] = {};
	int rc;

	if (parse_args(argc, argv) != 0) {
		return 1;
	}

	if (init() != 0) {
		return 1;
	}

	dump_user_config(&g_user_config);

	g_next_device = TAILQ_FIRST(&g_devices);

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			threads[i].chan = get_next_chan();
			rte_eal_remote_launch(work_fn, &threads[i], i);
		}
	}

	threads[current_core].chan = get_next_chan();
	if (work_fn(&threads[current_core]) != 0) {
		rc = 1;
		goto cleanup;
	}

	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			if (rte_eal_wait_lcore(i) != 0) {
				rc = 1;
				goto cleanup;
			}
		}
	}

	rc = dump_result(threads, RTE_MAX_LCORE);

cleanup:
	spdk_dma_free(g_src);
	ioat_exit();

	return rc;
}
