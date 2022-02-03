/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
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
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd.h"

struct spdk_user_idxd_device {
	struct spdk_idxd_device	idxd;
	struct spdk_pci_device	*device;
	int			sock_id;
	struct idxd_registers	*registers;
};

typedef bool (*spdk_idxd_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

#define __user_idxd(idxd) (struct spdk_user_idxd_device *)idxd

pthread_mutex_t	g_driver_lock = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_idxd_device *idxd_attach(struct spdk_pci_device *device);

/* Used for control commands, not for descriptor submission. */
static int
idxd_wait_cmd(struct spdk_idxd_device *idxd, int _timeout)
{
	uint32_t timeout = _timeout;
	union idxd_cmdsts_register cmd_status = {};
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	cmd_status.raw = spdk_mmio_read_4(&user_idxd->registers->cmdsts.raw);
	while (cmd_status.active && --timeout) {
		usleep(1);
		cmd_status.raw = spdk_mmio_read_4(&user_idxd->registers->cmdsts.raw);
	}

	/* Check for timeout */
	if (timeout == 0 && cmd_status.active) {
		SPDK_ERRLOG("Command timeout, waited %u\n", _timeout);
		return -EBUSY;
	}

	/* Check for error */
	if (cmd_status.err) {
		SPDK_ERRLOG("Command status reg reports error 0x%x\n", cmd_status.err);
		return -EINVAL;
	}

	return 0;
}

static int
idxd_unmap_pci_bar(struct spdk_idxd_device *idxd, int bar)
{
	int rc = 0;
	void *addr = NULL;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	if (bar == IDXD_MMIO_BAR) {
		addr = (void *)user_idxd->registers;
	} else if (bar == IDXD_WQ_BAR) {
		addr = (void *)idxd->portal;
	}

	if (addr) {
		rc = spdk_pci_device_unmap_bar(user_idxd->device, 0, addr);
	}
	return rc;
}

static int
idxd_map_pci_bars(struct spdk_idxd_device *idxd)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_MMIO_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}
	user_idxd->registers = (struct idxd_registers *)addr;

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_WQ_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		rc = idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);
		if (rc) {
			SPDK_ERRLOG("unable to unmap MMIO bar\n");
		}
		return -EINVAL;
	}
	idxd->portal = addr;

	return 0;
}

static void
idxd_disable_dev(struct spdk_idxd_device *idxd)
{
	int rc;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);
	union idxd_cmd_register cmd = {};

	cmd.command_code = IDXD_DISABLE_DEV;

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error disabling device %u\n", rc);
	}
}

static int
idxd_reset_dev(struct spdk_idxd_device *idxd)
{
	int rc;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);
	union idxd_cmd_register cmd = {};

	cmd.command_code = IDXD_RESET_DEVICE;

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error resetting device %u\n", rc);
	}

	return rc;
}

/*
 * Build group config based on getting info from the device combined
 * with the defined configuration. Once built, it is written to the
 * device.
 */
static int
idxd_group_config(struct spdk_idxd_device *idxd)
{
	int i;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);
	union idxd_groupcap_register groupcap;
	union idxd_enginecap_register enginecap;
	union idxd_wqcap_register wqcap;
	union idxd_offsets_register table_offsets;
	struct idxd_grptbl *grptbl;

	groupcap.raw = spdk_mmio_read_8(&user_idxd->registers->groupcap.raw);
	enginecap.raw = spdk_mmio_read_8(&user_idxd->registers->enginecap.raw);
	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	if (wqcap.num_wqs < 1) {
		return -ENOTSUP;
	}

	assert(groupcap.num_groups >= 1);
	idxd->groups = calloc(1, sizeof(struct idxd_group));
	if (idxd->groups == NULL) {
		SPDK_ERRLOG("Failed to allocate group memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < enginecap.num_engines; i++) {
		idxd->groups->grpcfg.engines |= (1 << i);
	}

	idxd->groups->grpcfg.wqs[0] = 0x1;
	idxd->groups->grpcfg.flags.read_buffers_allowed = groupcap.read_bufs;

	idxd->groups->idxd = idxd;
	idxd->groups->id = 0;

	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	grptbl = (struct idxd_grptbl *)((uint8_t *)user_idxd->registers + (table_offsets.grpcfg *
					IDXD_TABLE_OFFSET_MULT));

	/* GRPWQCFG, work queues config */
	spdk_mmio_write_8((uint64_t *)&grptbl->group[0].wqs[0], idxd->groups->grpcfg.wqs[0]);

	/* GRPENGCFG, engine config */
	spdk_mmio_write_8((uint64_t *)&grptbl->group[0].engines, idxd->groups->grpcfg.engines);

	/* GRPFLAGS, flags config */
	spdk_mmio_write_8((uint64_t *)&grptbl->group[0].flags, idxd->groups->grpcfg.flags.raw);

	/*
	 * Now write the other groups to zero them out
	 */
	for (i = 1 ; i < groupcap.num_groups; i++) {
		/* GRPWQCFG, work queues config */
		spdk_mmio_write_8((uint64_t *)&grptbl->group[i].wqs[0], 0UL);

		/* GRPENGCFG, engine config */
		spdk_mmio_write_8((uint64_t *)&grptbl->group[i].engines, 0UL);

		/* GRPFLAGS, flags config */
		spdk_mmio_write_8((uint64_t *)&grptbl->group[i].flags, 0UL);
	}

	return 0;
}

/*
 * Build work queue (WQ) config based on getting info from the device combined
 * with the defined configuration. Once built, it is written to the device.
 */
static int
idxd_wq_config(struct spdk_user_idxd_device *user_idxd)
{
	uint32_t j;
	struct idxd_wq *queue;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;
	uint32_t wq_size;
	union idxd_wqcap_register wqcap;
	union idxd_offsets_register table_offsets;
	struct idxd_wqtbl *wqtbl;
	union idxd_wqcfg wqcfg;

	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	wq_size = wqcap.total_wq_size;

	assert(sizeof(wqtbl->wq[0]) == 1 << (WQCFG_SHIFT + wqcap.wqcfg_size));

	SPDK_DEBUGLOG(idxd, "Total ring slots available space 0x%x, so per work queue is 0x%x\n",
		      wqcap.total_wq_size, wq_size);

	idxd->total_wq_size = wqcap.total_wq_size;
	/* Spread the channels we allow per device based on the total number of WQE to try
	 * and achieve optimal performance for common cases.
	 */
	idxd->chan_per_device = (idxd->total_wq_size >= 128) ? 8 : 4;
	idxd->queues = calloc(1, sizeof(struct idxd_wq));
	if (idxd->queues == NULL) {
		SPDK_ERRLOG("Failed to allocate queue memory\n");
		return -ENOMEM;
	}

	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	queue = idxd->queues;
	wqtbl = (struct idxd_wqtbl *)((uint8_t *)user_idxd->registers + (table_offsets.wqcfg *
				      IDXD_TABLE_OFFSET_MULT));

	/* Per spec we need to read in existing values first so we don't zero out something we
	 * didn't touch when we write the cfg register out below.
	 */
	for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
		wqcfg.raw[j] = spdk_mmio_read_4(&wqtbl->wq[0].raw[j]);
	}

	wqcfg.wq_size = wq_size;
	wqcfg.mode = WQ_MODE_DEDICATED;
	wqcfg.max_batch_shift = LOG2_WQ_MAX_BATCH;
	wqcfg.max_xfer_shift = LOG2_WQ_MAX_XFER;
	wqcfg.wq_state = WQ_ENABLED;
	wqcfg.priority = WQ_PRIORITY_1;

	/* Not part of the config struct */
	queue->idxd = idxd;
	queue->group = idxd->groups;

	/*
	 * Now write the work queue config to the device for configured queues
	 */
	for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
		spdk_mmio_write_4(&wqtbl->wq[0].raw[j], wqcfg.raw[j]);
	}

	return 0;
}

static int
idxd_device_configure(struct spdk_user_idxd_device *user_idxd)
{
	int rc = 0;
	union idxd_gensts_register gensts_reg;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;
	union idxd_cmd_register cmd = {};

	/*
	 * Map BAR0 and BAR2
	 */
	rc = idxd_map_pci_bars(idxd);
	if (rc) {
		return rc;
	}

	/*
	 * Reset the device
	 */
	rc = idxd_reset_dev(idxd);
	if (rc) {
		goto err_reset;
	}

	/*
	 * Configure groups and work queues.
	 */
	rc = idxd_group_config(idxd);
	if (rc) {
		goto err_group_cfg;
	}

	rc = idxd_wq_config(user_idxd);
	if (rc) {
		goto err_wq_cfg;
	}

	/*
	 * Enable the device
	 */
	gensts_reg.raw = spdk_mmio_read_4(&user_idxd->registers->gensts.raw);
	assert(gensts_reg.state == IDXD_DEVICE_STATE_DISABLED);

	cmd.command_code = IDXD_ENABLE_DEV;

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	gensts_reg.raw = spdk_mmio_read_4(&user_idxd->registers->gensts.raw);
	if ((rc < 0) || (gensts_reg.state != IDXD_DEVICE_STATE_ENABLED)) {
		rc = -EINVAL;
		SPDK_ERRLOG("Error enabling device %u\n", rc);
		goto err_device_enable;
	}

	/*
	 * Enable the work queue that we've configured
	 */
	cmd.command_code = IDXD_ENABLE_WQ;
	cmd.operand = 0;

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error enabling work queues 0x%x\n", rc);
		goto err_wq_enable;
	}

	if ((rc == 0) && (gensts_reg.state == IDXD_DEVICE_STATE_ENABLED)) {
		SPDK_DEBUGLOG(idxd, "Device enabled\n");
	}

	return rc;
err_wq_enable:
err_device_enable:
	free(idxd->queues);
err_wq_cfg:
	free(idxd->groups);
err_group_cfg:
err_reset:
	idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);

	return rc;
}

static void
user_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	idxd_disable_dev(idxd);

	idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(idxd, IDXD_WQ_BAR);
	free(idxd->groups);
	free(idxd->queues);

	spdk_pci_device_detach(user_idxd->device);
	free(user_idxd);
}

struct idxd_enum_ctx {
	spdk_idxd_probe_cb probe_cb;
	spdk_idxd_attach_cb attach_cb;
	void *cb_ctx;
};

/* This function must only be called while holding g_driver_lock */
static int
idxd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct idxd_enum_ctx *enum_ctx = ctx;
	struct spdk_idxd_device *idxd;

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		idxd = idxd_attach(pci_dev);
		if (idxd == NULL) {
			SPDK_ERRLOG("idxd_attach() failed\n");
			return -EINVAL;
		}

		enum_ctx->attach_cb(enum_ctx->cb_ctx, idxd);
	}

	return 0;
}


static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr __attribute__((unused));

	pci_addr = spdk_pci_device_get_addr(pci_dev);

	SPDK_DEBUGLOG(idxd,
		      " Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		      pci_addr.domain,
		      pci_addr.bus,
		      pci_addr.dev,
		      pci_addr.func,
		      spdk_pci_device_get_vendor_id(pci_dev),
		      spdk_pci_device_get_device_id(pci_dev));

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;
	}

	return true;
}

static int
user_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb)
{
	int rc;
	struct idxd_enum_ctx enum_ctx;

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	pthread_mutex_lock(&g_driver_lock);
	rc = spdk_pci_enumerate(spdk_pci_idxd_get_driver(), idxd_enum_cb, &enum_ctx);
	pthread_mutex_unlock(&g_driver_lock);

	return rc;
}

static void
user_idxd_dump_sw_err(struct spdk_idxd_device *idxd, void *portal)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);
	union idxd_swerr_register sw_err;
	uint16_t i;

	SPDK_NOTICELOG("SW Error Raw:");
	for (i = 0; i < 4; i++) {
		sw_err.raw[i] = spdk_mmio_read_8(&user_idxd->registers->sw_err.raw[i]);
		SPDK_NOTICELOG("    0x%lx\n", sw_err.raw[i]);
	}

	SPDK_NOTICELOG("SW Error error code: %#x\n", (uint8_t)(sw_err.error));
	SPDK_NOTICELOG("SW Error WQ index: %u\n", (uint8_t)(sw_err.wq_idx));
	SPDK_NOTICELOG("SW Error Operation: %u\n", (uint8_t)(sw_err.operation));
}

static char *
user_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	return (char *)idxd->portal;
}

static struct spdk_idxd_impl g_user_idxd_impl = {
	.name			= "user",
	.probe			= user_idxd_probe,
	.destruct		= user_idxd_device_destruct,
	.dump_sw_error		= user_idxd_dump_sw_err,
	.portal_get_addr	= user_idxd_portal_get_addr
};

/* Caller must hold g_driver_lock */
static struct spdk_idxd_device *
idxd_attach(struct spdk_pci_device *device)
{
	struct spdk_user_idxd_device *user_idxd;
	struct spdk_idxd_device *idxd;
	uint32_t cmd_reg;
	int rc;

	user_idxd = calloc(1, sizeof(struct spdk_user_idxd_device));
	if (user_idxd == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for user_idxd device.\n");
		return NULL;
	}

	idxd = &user_idxd->idxd;
	user_idxd->device = device;
	idxd->impl = &g_user_idxd_impl;
	idxd->socket_id = device->socket_id;
	pthread_mutex_init(&idxd->num_channels_lock, NULL);

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);
	cmd_reg |= 0x4;
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);

	rc = idxd_device_configure(user_idxd);
	if (rc) {
		goto err;
	}

	return idxd;
err:
	user_idxd_device_destruct(idxd);
	return NULL;
}

SPDK_IDXD_IMPL_REGISTER(user, &g_user_idxd_impl);
