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

#define FUSE_USE_VERSION 31

#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

#include "nvme_internal.h"
#include "nvme_io_msg.h"

static bool g_cuse_initialized = false;

struct spdk_nvme_io_msg;

static struct spdk_ring *g_nvme_io_msgs;
pthread_mutex_t g_cuse_io_requests_lock;

#define SPDK_CUSE_REQUESTS_PROCESS_SIZE 8

/**
 * Send message to IO queue.
 */
int
spdk_nvme_io_msg_send(struct spdk_nvme_io_msg *io, spdk_nvme_io_msg_fn fn, void *arg)
{
	int rc;

	io->fn = fn;
	io->arg = arg;

	/* Protect requests ring against preemptive producers */
	pthread_mutex_lock(&g_cuse_io_requests_lock);

	rc = spdk_ring_enqueue(g_nvme_io_msgs, (void **)&io, 1, NULL);
	if (rc != 1) {
		assert(false);
		/* FIXIT! Do something with request here */
		return -ENOMEM;
	}

	pthread_mutex_unlock(&g_cuse_io_requests_lock);

	return 0;
}

/**
 * Get next IO message and process on the current SPDK thread.
 */
struct nvme_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;

	bool			collect_spin_stat;
	uint64_t		spin_ticks;
	uint64_t		start_ticks;
	uint64_t		end_ticks;
};

int
spdk_nvme_io_msg_process(void)
{
	int i;
	void *requests[SPDK_CUSE_REQUESTS_PROCESS_SIZE];
	int count;
	struct nvme_io_channel *ch;

	if (!g_cuse_initialized) {
		return 0;
	}

	count = spdk_ring_dequeue(g_nvme_io_msgs, requests, SPDK_CUSE_REQUESTS_PROCESS_SIZE);
	if (count == 0) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct spdk_nvme_io_msg *io = requests[i];

		assert(io != NULL);

		if (io->nsid != 0) {
			io->io_channel = spdk_get_io_channel(io->ctrlr);
			ch = spdk_io_channel_get_ctx(io->io_channel);
			io->qpair = ch->qpair;
		}

		io->fn(io);
	}

	return 0;
}

void
cuse_nvme_io_msg_free(struct spdk_nvme_io_msg *io)
{
	if (io->io_channel) {
		spdk_put_io_channel(io->io_channel);
	}
	spdk_free(io->data);
	free(io);
}

struct spdk_nvme_io_msg *
cuse_nvme_io_msg_alloc(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *ctx)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)calloc(1, sizeof(struct spdk_nvme_io_msg));

	io->ctrlr = ctrlr;
	io->nsid = nsid;
	io->ctx = ctx;
	return io;
}

static STAILQ_HEAD(, spdk_nvme_io_msg_producer) g_io_producers =
	STAILQ_HEAD_INITIALIZER(g_io_producers);

int
spdk_nvme_io_msg_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr) {
	struct spdk_nvme_io_msg_producer *io_msg_producer = NULL;

	STAILQ_FOREACH(io_msg_producer, &g_io_producers, link) {
		if (io_msg_producer->ctrlr_start) {
			io_msg_producer->ctrlr_start(ctrlr);
		}
	}
	return 0;
}

int
spdk_nvme_io_msg_ctrlr_stop(struct spdk_nvme_ctrlr *ctrlr) {
	struct spdk_nvme_io_msg_producer *io_msg_producer = NULL;

	STAILQ_FOREACH(io_msg_producer, &g_io_producers, link) {
		if (io_msg_producer->ctrlr_stop) {
			io_msg_producer->ctrlr_stop(ctrlr);
		}
	}
	return 0;
}

void
spdk_nvme_io_msg_register(struct spdk_nvme_io_msg_producer *io_msg_producer)
{
	STAILQ_INSERT_TAIL(&g_io_producers, io_msg_producer, link);
}
