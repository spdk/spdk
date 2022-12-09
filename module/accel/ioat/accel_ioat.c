/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_ioat.h"

#include "spdk/stdinc.h"

#include "spdk_internal/accel_module.h"
#include "spdk/log.h"
#include "spdk/likely.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/ioat.h"

static bool g_ioat_enable = false;
static bool g_ioat_initialized = false;

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

static int accel_ioat_init(void);
static void accel_ioat_exit(void *ctx);
static bool ioat_supports_opcode(enum accel_opcode opc);
static struct spdk_io_channel *ioat_get_io_channel(void);
static int ioat_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);

static size_t
accel_ioat_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static struct spdk_accel_module_if g_ioat_module = {
	.module_init = accel_ioat_init,
	.module_fini = accel_ioat_exit,
	.write_config_json = NULL,
	.get_ctx_size = accel_ioat_get_ctx_size,
	.name			= "ioat",
	.supports_opcode	= ioat_supports_opcode,
	.get_io_channel		= ioat_get_io_channel,
	.submit_tasks		= ioat_submit_tasks
};

SPDK_ACCEL_MODULE_REGISTER(ioat, &g_ioat_module)

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

static bool
ioat_supports_opcode(enum accel_opcode opc)
{
	if (!g_ioat_initialized) {
		return false;
	}

	switch (opc) {
	case ACCEL_OPC_COPY:
	case ACCEL_OPC_FILL:
		return true;
	default:
		return false;
	}

}

static int
ioat_submit_fill(struct ioat_io_channel *ioat_ch, struct spdk_accel_task *task)
{
	if (spdk_unlikely(task->d.iovcnt != 1)) {
		return -EINVAL;
	}

	return spdk_ioat_build_fill(ioat_ch->ioat_ch, task, ioat_done,
				    task->d.iovs[0].iov_base, task->fill_pattern,
				    task->d.iovs[0].iov_len);
}

static int
ioat_submit_copy(struct ioat_io_channel *ioat_ch, struct spdk_accel_task *task)
{
	if (spdk_unlikely(task->d.iovcnt != 1 || task->s.iovcnt != 1)) {
		return -EINVAL;
	}

	if (spdk_unlikely(task->d.iovs[0].iov_len != task->s.iovs[0].iov_len)) {
		return -EINVAL;
	}

	return spdk_ioat_build_copy(ioat_ch->ioat_ch, task, ioat_done,
				    task->d.iovs[0].iov_base, task->s.iovs[0].iov_base,
				    task->d.iovs[0].iov_len);
}

static int
ioat_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct ioat_io_channel *ioat_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	if (accel_task->flags == ACCEL_FLAG_PERSISTENT) {
		SPDK_ERRLOG("IOAT does not support durable destinations.\n");
		return -EINVAL;
	}

	do {
		switch (accel_task->op_code) {
		case ACCEL_OPC_FILL:
			rc = ioat_submit_fill(ioat_ch,  accel_task);
			break;
		case ACCEL_OPC_COPY:
			rc = ioat_submit_copy(ioat_ch, accel_task);
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
	return spdk_get_io_channel(&g_ioat_module);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
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
accel_ioat_enable_probe(void)
{
	g_ioat_enable = true;
}

static int
accel_ioat_init(void)
{
	if (!g_ioat_enable) {
		return 0;
	}

	if (spdk_ioat_probe(NULL, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_ioat_probe() failed\n");
		return -1;
	}

	if (TAILQ_EMPTY(&g_devices)) {
		SPDK_NOTICELOG("No available ioat devices\n");
		return -1;
	}

	g_ioat_initialized = true;
	SPDK_NOTICELOG("Accel framework IOAT module initialized.\n");
	spdk_io_device_register(&g_ioat_module, ioat_create_cb, ioat_destroy_cb,
				sizeof(struct ioat_io_channel), "ioat_accel_module");
	return 0;
}

static void
_device_unregister_cb(void *io_device)
{
	struct ioat_device *dev = io_device;
	struct pci_device *pci_dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		spdk_ioat_detach(dev->ioat);
		free(dev);
	}

	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
	}

	g_ioat_initialized = false;

	spdk_accel_module_finish();
}

static void
accel_ioat_exit(void *ctx)
{
	if (g_ioat_initialized) {
		spdk_io_device_unregister(&g_ioat_module, _device_unregister_cb);
	} else {
		spdk_accel_module_finish();
	}
}

SPDK_LOG_REGISTER_COMPONENT(accel_ioat)
