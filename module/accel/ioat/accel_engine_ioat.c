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

#include "accel_engine_ioat.h"

#include "spdk/stdinc.h"

#include "spdk_internal/accel_engine.h"
#include "spdk/log.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/ioat.h"

static bool g_ioat_enable = false;
static bool g_ioat_initialized = false;

struct ioat_probe_ctx {
	int num_whitelist_devices;
	struct spdk_pci_addr whitelist[IOAT_MAX_CHANNELS];
};

static struct ioat_probe_ctx g_probe_ctx;

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	bool is_allocated;
	/** linked list pointer for device list */
	TAILQ_ENTRY(ioat_device) tailq;
};

struct pci_device {
	struct spdk_pci_device *pci_dev;
	TAILQ_ENTRY(pci_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static pthread_mutex_t g_ioat_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);

struct ioat_io_channel {
	struct spdk_ioat_chan		*ioat_ch;
	struct ioat_device		*ioat_dev;
	struct spdk_poller		*poller;
};

static int
ioat_find_dev_by_whitelist_bdf(const struct spdk_pci_addr *pci_addr,
			       const struct spdk_pci_addr *whitelist,
			       int num_whitelist_devices)
{
	int i;

	for (i = 0; i < num_whitelist_devices; i++) {
		if (spdk_pci_addr_compare(pci_addr, &whitelist[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static struct ioat_device *
ioat_allocate_device(void)
{
	struct ioat_device *dev;

	pthread_mutex_lock(&g_ioat_mutex);
	TAILQ_FOREACH(dev, &g_devices, tailq) {
		if (!dev->is_allocated) {
			dev->is_allocated = true;
			pthread_mutex_unlock(&g_ioat_mutex);
			return dev;
		}
	}
	pthread_mutex_unlock(&g_ioat_mutex);

	return NULL;
}

static void
ioat_free_device(struct ioat_device *dev)
{
	pthread_mutex_lock(&g_ioat_mutex);
	dev->is_allocated = false;
	pthread_mutex_unlock(&g_ioat_mutex);
}

static int accel_engine_ioat_init(void);
static void accel_engine_ioat_exit(void *ctx);

static size_t
accel_engine_ioat_get_ctx_size(void)
{
	return 0;
}

SPDK_ACCEL_MODULE_REGISTER(accel_engine_ioat_init, accel_engine_ioat_exit,
			   NULL, accel_engine_ioat_get_ctx_size)

static void
ioat_done(void *cb_arg)
{
	struct spdk_accel_task *accel_task = cb_arg;

	spdk_accel_task_complete(accel_task, 0);
}

static int
ioat_poll(void *arg)
{
	struct spdk_ioat_chan *chan = arg;

	return spdk_ioat_process_events(chan) != 0 ? SPDK_POLLER_BUSY :
	       SPDK_POLLER_IDLE;
}

static struct spdk_io_channel *ioat_get_io_channel(void);

static uint64_t
ioat_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL;
}

static uint32_t
ioat_batch_get_max(struct spdk_io_channel *ch)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);

	return spdk_ioat_get_max_descriptors(ioat_ch->ioat_dev->ioat);
}

static int
ioat_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	do {
		switch (accel_task->op_code) {
		case ACCEL_OPCODE_MEMFILL:
			rc = spdk_ioat_build_fill(ioat_ch->ioat_ch, accel_task, ioat_done,
						  accel_task->dst, accel_task->fill_pattern, accel_task->nbytes);
			break;
		case ACCEL_OPCODE_MEMMOVE:
			rc = spdk_ioat_build_copy(ioat_ch->ioat_ch, accel_task, ioat_done,
						  accel_task->dst, accel_task->src, accel_task->nbytes);
			break;
		default:
			assert(false);
			break;
		}

		tmp = TAILQ_NEXT(accel_task, link);

		/* Report any build errors via the callback now. */
		if (rc) {
			spdk_accel_task_complete(accel_task, rc);
		}

		accel_task = tmp;
	} while (accel_task);

	spdk_ioat_flush(ioat_ch->ioat_ch);

	return 0;
}

static struct spdk_accel_engine ioat_accel_engine = {
	.get_capabilities	= ioat_get_capabilities,
	.get_io_channel		= ioat_get_io_channel,
	.batch_get_max		= ioat_batch_get_max,
	.submit_tasks		= ioat_submit_tasks,
};

static int
ioat_create_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;
	struct ioat_device *ioat_dev;

	ioat_dev = ioat_allocate_device();
	if (ioat_dev == NULL) {
		return -1;
	}

	ch->ioat_dev = ioat_dev;
	ch->ioat_ch = ioat_dev->ioat;
	ch->poller = SPDK_POLLER_REGISTER(ioat_poll, ch->ioat_ch, 0);

	return 0;
}

static void
ioat_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ioat_io_channel *ch = ctx_buf;

	ioat_free_device(ch->ioat_dev);
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
ioat_get_io_channel(void)
{
	return spdk_get_io_channel(&ioat_accel_engine);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_probe_ctx *ctx = cb_ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(pci_dev);
	struct pci_device *pdev;

	SPDK_INFOLOG(accel_ioat,
		     " Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		     pci_addr.domain,
		     pci_addr.bus,
		     pci_addr.dev,
		     pci_addr.func,
		     spdk_pci_device_get_vendor_id(pci_dev),
		     spdk_pci_device_get_device_id(pci_dev));

	pdev = calloc(1, sizeof(*pdev));
	if (pdev == NULL) {
		return false;
	}
	pdev->pci_dev = pci_dev;
	TAILQ_INSERT_TAIL(&g_pci_devices, pdev, tailq);

	if (ctx->num_whitelist_devices > 0 &&
	    !ioat_find_dev_by_whitelist_bdf(&pci_addr, ctx->whitelist, ctx->num_whitelist_devices)) {
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ioat = ioat;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

void
accel_engine_ioat_enable_probe(void)
{
	g_ioat_enable = true;
}

static int
accel_engine_ioat_add_whitelist_device(const char *pci_bdf)
{
	if (pci_bdf == NULL) {
		return -1;
	}

	if (g_probe_ctx.num_whitelist_devices >= IOAT_MAX_CHANNELS) {
		SPDK_ERRLOG("Ioat whitelist is full (max size is %d)\n",
			    IOAT_MAX_CHANNELS);
		return -1;
	}

	if (spdk_pci_addr_parse(&g_probe_ctx.whitelist[g_probe_ctx.num_whitelist_devices],
				pci_bdf) < 0) {
		SPDK_ERRLOG("Invalid address %s\n", pci_bdf);
		return -1;
	}

	g_probe_ctx.num_whitelist_devices++;

	return 0;
}

int
accel_engine_ioat_add_whitelist_devices(const char *pci_bdfs[], size_t num_pci_bdfs)
{
	size_t i;

	for (i = 0; i < num_pci_bdfs; i++) {
		if (accel_engine_ioat_add_whitelist_device(pci_bdfs[i]) < 0) {
			return -1;
		}
	}

	return 0;
}

static int
accel_engine_ioat_init(void)
{
	if (!g_ioat_enable) {
		return 0;
	}

	if (spdk_ioat_probe(&g_probe_ctx, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ioat_probe() failed\n");
		return -1;
	}

	g_ioat_initialized = true;
	SPDK_NOTICELOG("Accel engine updated to use IOAT engine.\n");
	spdk_accel_hw_engine_register(&ioat_accel_engine);
	spdk_io_device_register(&ioat_accel_engine, ioat_create_cb, ioat_destroy_cb,
				sizeof(struct ioat_io_channel), "ioat_accel_engine");
	return 0;
}

static void
accel_engine_ioat_exit(void *ctx)
{
	struct ioat_device *dev;
	struct pci_device *pci_dev;

	if (g_ioat_initialized) {
		spdk_io_device_unregister(&ioat_accel_engine, NULL);
	}

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ioat_detach(dev->ioat);
		ioat_free_device(dev);
		free(dev);
	}

	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
	}

	spdk_accel_engine_module_finish();
}

SPDK_LOG_REGISTER_COMPONENT(accel_ioat)
