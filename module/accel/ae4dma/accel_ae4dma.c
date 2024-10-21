/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */


#include "accel_ae4dma.h"

#include "spdk/stdinc.h"
#include "spdk/accel_module.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/ae4dma.h"
#include "spdk/json.h"

#define AE4DMA_MAX_CHANNELS     2

static bool g_ae4dma_enable = false;
static bool g_ae4dma_initialized = false;

struct ae4dma_device {
	struct spdk_ae4dma_chan *ae4dma;
	uint8_t hwq_avail;
	/** linked list pointer for device list */
	TAILQ_ENTRY(ae4dma_device) tailq;
};

struct pci_device {
	struct spdk_pci_device *pci_dev;

	TAILQ_ENTRY(pci_device) tailq;
};

static TAILQ_HEAD(, ae4dma_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static pthread_mutex_t g_ae4dma_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);

struct ae4dma_io_channel {
	struct spdk_ae4dma_chan		*ae4dma_ch;
	struct ae4dma_device		*ae4dma_dev;
	struct spdk_poller		*poller;
	uint8_t ae4dma_chan_id;
};

static struct ae4dma_device *
ae4dma_alloc_dev_channel(void)
{
	struct ae4dma_device *dev;

	pthread_mutex_lock(&g_ae4dma_mutex);

	TAILQ_FOREACH(dev, &g_devices, tailq) {
		if (dev->hwq_avail) {
			dev->hwq_avail--;
			pthread_mutex_unlock(&g_ae4dma_mutex);
			return dev;
		}
	}

	pthread_mutex_unlock(&g_ae4dma_mutex);
	SPDK_ERRLOG("Available ae4dma channels per device are %d\n", AE4DMA_MAX_CHANNELS);

	return NULL;
}

static int accel_ae4dma_init(void);
static void accel_ae4dma_exit(void *ctx);
static void accel_ae4dma_write_config_json(struct spdk_json_write_ctx *w);
static bool ae4dma_supports_opcode(enum spdk_accel_opcode opc);
static struct spdk_io_channel *ae4dma_get_io_channel(void);
static int ae4dma_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);

static size_t
accel_ae4dma_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static struct spdk_accel_module_if g_ae4dma_module = {
	.module_init = accel_ae4dma_init,
	.module_fini = accel_ae4dma_exit,
	.write_config_json = accel_ae4dma_write_config_json,
	.get_ctx_size = accel_ae4dma_get_ctx_size,
	.name			= "ae4dma",
	.supports_opcode	= ae4dma_supports_opcode,
	.get_io_channel		= ae4dma_get_io_channel,
	.submit_tasks		= ae4dma_submit_tasks
};

static void
ae4dma_done(void *cb_arg, int status)
{
	struct spdk_accel_task *accel_task = cb_arg;
	if (status) {
		SPDK_ERRLOG("AE4DMA Desc error code : %d\n", status);
	}
	spdk_accel_task_complete(accel_task, status);
}

static int
ae4dma_poll(void *arg)
{
	struct ae4dma_io_channel *chan = arg;

	if ((spdk_ae4dma_process_events(chan->ae4dma_ch, chan->ae4dma_chan_id)) != 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static struct spdk_io_channel *ae4dma_get_io_channel(void);

static bool
ae4dma_supports_opcode(enum spdk_accel_opcode opc)
{
	if (!g_ae4dma_initialized) {
		assert(0);
		return false;
	}

	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
		return true;

	default:
		return false;
	}
}


static int
ae4dma_submit_copy(struct ae4dma_io_channel *ae4dma_ch, struct spdk_accel_task *task)
{

	return spdk_ae4dma_build_copy(ae4dma_ch->ae4dma_ch, ae4dma_ch->ae4dma_chan_id, task, ae4dma_done,
				      task->d.iovs, task->d.iovcnt, task->s.iovs, task->s.iovcnt
				     );
}

static int
ae4dma_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct ae4dma_io_channel *ae4dma_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	do {
		switch (accel_task->op_code) {
		case SPDK_ACCEL_OPC_COPY:
			rc = ae4dma_submit_copy(ae4dma_ch, accel_task);
			break;

		default:
			assert(false);
			break;
		}

		tmp = STAILQ_NEXT(accel_task, link);

		/* Report any build errors via the callback now. */
		if (rc) {
			SPDK_ERRLOG("AE4DMA build_copy_error : %d\n", rc);
			spdk_accel_task_complete(accel_task, rc);
		}
		accel_task = tmp;
	} while (accel_task);

	spdk_ae4dma_flush(ae4dma_ch->ae4dma_ch, ae4dma_ch->ae4dma_chan_id);

	return 0;
}

static int
ae4dma_create_cb(void *io_device, void *ctx_buf)
{
	struct ae4dma_io_channel *ch = ctx_buf;
	struct ae4dma_device *ae4dma_dev;

	ae4dma_dev = ae4dma_alloc_dev_channel();
	if (ae4dma_dev == NULL) {
		return -ENODEV;
	}
	ch->ae4dma_dev = ae4dma_dev;
	ch->ae4dma_ch = ae4dma_dev->ae4dma;
	ch->ae4dma_chan_id = (AE4DMA_MAX_CHANNELS - 1) - ae4dma_dev->hwq_avail;

	ch->poller = SPDK_POLLER_REGISTER(ae4dma_poll, ch, 0);
	return 0;
}

static void
ae4dma_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ae4dma_io_channel *ch = ctx_buf;

	assert(ch->ae4dma_dev->hwq_avail < AE4DMA_MAX_CHANNELS);
	ch->ae4dma_dev->hwq_avail++;
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
ae4dma_get_io_channel(void)
{
	return spdk_get_io_channel(&g_ae4dma_module);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(pci_dev);
	struct pci_device *pdev;

	SPDK_INFOLOG(accel_ae4dma,
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

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;
	}
	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ae4dma_chan *ae4dma)
{
	struct ae4dma_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ae4dma = ae4dma;
	dev->hwq_avail = AE4DMA_MAX_CHANNELS;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

void
accel_ae4dma_enable_probe(void)
{
	g_ae4dma_enable = true;

	spdk_accel_module_list_add(&g_ae4dma_module);
}

static int
accel_ae4dma_init(void)
{
	if (!g_ae4dma_enable) {
		assert(0);
		return 0;
	}

	if (spdk_ae4dma_probe(NULL, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ae4dma_probe() failed\n");
		return -1;
	}

	if (TAILQ_EMPTY(&g_devices)) {
		return -ENODEV;
	}
	g_ae4dma_initialized = true;
	spdk_io_device_register(&g_ae4dma_module, ae4dma_create_cb, ae4dma_destroy_cb,
				sizeof(struct ae4dma_io_channel), "ae4dma_accel_module");
	return 0;
}

static void
_device_unregister_cb(void *io_device)
{
	struct ae4dma_device *dev = io_device;
	struct pci_device *pci_dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ae4dma_detach(dev->ae4dma);
		free(dev);
	}

	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
	}

	g_ae4dma_initialized = false;

	spdk_accel_module_finish();
}

static void
accel_ae4dma_exit(void *ctx)
{
	if (g_ae4dma_initialized) {
		spdk_io_device_unregister(&g_ae4dma_module, _device_unregister_cb);
	} else {
		spdk_accel_module_finish();
	}
}

static void
accel_ae4dma_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_ae4dma_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "ae4dma_scan_accel_module");
		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(accel_ae4dma)
