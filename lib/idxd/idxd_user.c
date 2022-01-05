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
	struct idxd_registers	registers;
	void			*reg_base;
	uint32_t		wqcfg_offset;
	uint32_t		grpcfg_offset;
	uint32_t		ims_offset;
	uint32_t		msix_perm_offset;
	uint32_t		perfmon_offset;
};

typedef bool (*spdk_idxd_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

#define __user_idxd(idxd) (struct spdk_user_idxd_device *)idxd

pthread_mutex_t	g_driver_lock = PTHREAD_MUTEX_INITIALIZER;
static struct device_config g_user_dev_cfg = {};

static struct spdk_idxd_device *idxd_attach(struct spdk_pci_device *device);

static uint32_t
_idxd_read_4(struct spdk_idxd_device *idxd, uint32_t offset)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	return spdk_mmio_read_4((uint32_t *)(user_idxd->reg_base + offset));
}

static void
_idxd_write_4(struct spdk_idxd_device *idxd, uint32_t offset, uint32_t value)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	spdk_mmio_write_4((uint32_t *)(user_idxd->reg_base + offset), value);
}

static uint64_t
_idxd_read_8(struct spdk_idxd_device *idxd, uint32_t offset)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	return spdk_mmio_read_8((uint64_t *)(user_idxd->reg_base + offset));
}

static void
_idxd_write_8(struct spdk_idxd_device *idxd, uint32_t offset, uint64_t value)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	spdk_mmio_write_8((uint64_t *)(user_idxd->reg_base + offset), value);
}

static void
user_idxd_set_config(struct device_config *dev_cfg, uint32_t config_num)
{
	g_user_dev_cfg = *dev_cfg;
}

/* Used for control commands, not for descriptor submission. */
static int
idxd_wait_cmd(struct spdk_idxd_device *idxd, int _timeout)
{
	uint32_t timeout = _timeout;
	union idxd_cmdsts_reg cmd_status = {};

	cmd_status.raw = _idxd_read_4(idxd, IDXD_CMDSTS_OFFSET);
	while (cmd_status.active && --timeout) {
		usleep(1);
		cmd_status.raw = _idxd_read_4(idxd, IDXD_CMDSTS_OFFSET);
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
		addr = (void *)user_idxd->reg_base;
	} else if (bar == IDXD_WQ_BAR) {
		addr = (void *)idxd->portals;
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
	user_idxd->reg_base = addr;

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_WQ_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		rc = idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);
		if (rc) {
			SPDK_ERRLOG("unable to unmap MMIO bar\n");
		}
		return -EINVAL;
	}
	idxd->portals = addr;

	return 0;
}

static void
idxd_disable_dev(struct spdk_idxd_device *idxd)
{
	int rc;

	_idxd_write_4(idxd, IDXD_CMD_OFFSET, IDXD_DISABLE_DEV << IDXD_CMD_SHIFT);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error disabling device %u\n", rc);
	}
}

static int
idxd_reset_dev(struct spdk_idxd_device *idxd)
{
	int rc;

	_idxd_write_4(idxd, IDXD_CMD_OFFSET, IDXD_RESET_DEVICE << IDXD_CMD_SHIFT);
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
	uint64_t base_offset;
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	assert(g_user_dev_cfg.num_groups <= user_idxd->registers.groupcap.num_groups);
	idxd->groups = calloc(user_idxd->registers.groupcap.num_groups, sizeof(struct idxd_group));
	if (idxd->groups == NULL) {
		SPDK_ERRLOG("Failed to allocate group memory\n");
		return -ENOMEM;
	}

	assert(g_user_dev_cfg.total_engines <= user_idxd->registers.enginecap.num_engines);
	for (i = 0; i < g_user_dev_cfg.total_engines; i++) {
		idxd->groups[i % g_user_dev_cfg.num_groups].grpcfg.engines |= (1 << i);
	}

	assert(g_user_dev_cfg.total_wqs <= user_idxd->registers.wqcap.num_wqs);
	for (i = 0; i < g_user_dev_cfg.total_wqs; i++) {
		idxd->groups[i % g_user_dev_cfg.num_groups].grpcfg.wqs[0] |= (1 << i);
	}

	for (i = 0; i < g_user_dev_cfg.num_groups; i++) {
		idxd->groups[i].idxd = idxd;
		idxd->groups[i].id = i;

		/* Divide BW tokens evenly */
		idxd->groups[i].grpcfg.flags.tokens_allowed =
			user_idxd->registers.groupcap.read_bufs / g_user_dev_cfg.num_groups;
	}

	/*
	 * Now write the group config to the device for all groups. We write
	 * to the max number of groups in order to 0 out the ones we didn't
	 * configure.
	 */
	for (i = 0 ; i < user_idxd->registers.groupcap.num_groups; i++) {

		base_offset = user_idxd->grpcfg_offset + i * 64;

		/* GRPWQCFG, work queues config */
		_idxd_write_8(idxd, base_offset, idxd->groups[i].grpcfg.wqs[0]);

		/* GRPENGCFG, engine config */
		_idxd_write_8(idxd, base_offset + CFG_ENGINE_OFFSET, idxd->groups[i].grpcfg.engines);

		/* GRPFLAGS, flags config */
		_idxd_write_8(idxd, base_offset + CFG_FLAG_OFFSET, idxd->groups[i].grpcfg.flags.raw);
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
	uint32_t i, j;
	struct idxd_wq *queue;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;
	uint32_t wq_size = user_idxd->registers.wqcap.total_wq_size / g_user_dev_cfg.total_wqs;
	uint32_t wqcap_size = 1 << (WQCFG_SHIFT + user_idxd->registers.wqcap.wqcfg_size);

	SPDK_DEBUGLOG(idxd, "Total ring slots available space 0x%x, so per work queue is 0x%x\n",
		      user_idxd->registers.wqcap.total_wq_size, wq_size);
	assert(g_user_dev_cfg.total_wqs <= IDXD_MAX_QUEUES);
	assert(g_user_dev_cfg.total_wqs <= user_idxd->registers.wqcap.num_wqs);
	assert(LOG2_WQ_MAX_BATCH <= user_idxd->registers.gencap.max_batch_shift);
	assert(LOG2_WQ_MAX_XFER <= user_idxd->registers.gencap.max_xfer_shift);

	idxd->total_wq_size = user_idxd->registers.wqcap.total_wq_size;
	/* Spread the channels we allow per device based on the total number of WQE to try
	 * and achieve optimal performance for common cases.
	 */
	idxd->chan_per_device = (idxd->total_wq_size >= 128) ? 8 : 4;
	idxd->queues = calloc(1, g_user_dev_cfg.total_wqs * sizeof(struct idxd_wq));
	if (idxd->queues == NULL) {
		SPDK_ERRLOG("Failed to allocate queue memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_user_dev_cfg.total_wqs; i++) {
		queue = &idxd->queues[i];
		/* Per spec we need to read in existing values first so we don't zero out something we
		 * didn't touch when we write the cfg register out below.
		 */
		for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
			queue->wqcfg.raw[j] = _idxd_read_4(idxd,
							   user_idxd->wqcfg_offset + i * wqcap_size + j * sizeof(uint32_t));
		}
		queue->wqcfg.wq_size = wq_size;
		queue->wqcfg.mode = WQ_MODE_DEDICATED;
		queue->wqcfg.max_batch_shift = LOG2_WQ_MAX_BATCH;
		queue->wqcfg.max_xfer_shift = LOG2_WQ_MAX_XFER;
		queue->wqcfg.wq_state = WQ_ENABLED;
		queue->wqcfg.priority = WQ_PRIORITY_1;

		/* Not part of the config struct */
		queue->idxd = idxd;
		queue->group = &idxd->groups[i % g_user_dev_cfg.num_groups];
	}

	/*
	 * Now write the work queue config to the device for configured queues
	 */
	for (i = 0 ; i < g_user_dev_cfg.total_wqs; i++) {
		queue = &idxd->queues[i];
		for (j = 0 ; j < (sizeof(union idxd_wqcfg) / sizeof(uint32_t)); j++) {
			_idxd_write_4(idxd, user_idxd->wqcfg_offset + i * wqcap_size + j * sizeof(uint32_t),
				      queue->wqcfg.raw[j]);
		}
	}

	return 0;
}

static int
idxd_device_configure(struct spdk_user_idxd_device *user_idxd)
{
	int i, rc = 0;
	union idxd_offsets_register offsets_reg;
	union idxd_genstatus_register genstatus_reg;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;

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
	 * Read in config registers
	 */
	user_idxd->registers.version = _idxd_read_4(idxd, IDXD_VERSION_OFFSET);
	user_idxd->registers.gencap.raw = _idxd_read_8(idxd, IDXD_GENCAP_OFFSET);
	user_idxd->registers.wqcap.raw = _idxd_read_8(idxd, IDXD_WQCAP_OFFSET);
	user_idxd->registers.groupcap.raw = _idxd_read_8(idxd, IDXD_GRPCAP_OFFSET);
	user_idxd->registers.enginecap.raw = _idxd_read_8(idxd, IDXD_ENGCAP_OFFSET);
	for (i = 0; i < IDXD_OPCAP_WORDS; i++) {
		user_idxd->registers.opcap.raw[i] =
			_idxd_read_8(idxd, i * sizeof(uint64_t) + IDXD_OPCAP_OFFSET);
	}
	offsets_reg.raw[0] = _idxd_read_8(idxd, IDXD_TABLE_OFFSET);
	offsets_reg.raw[1] = _idxd_read_8(idxd, IDXD_TABLE_OFFSET + sizeof(uint64_t));
	user_idxd->grpcfg_offset = offsets_reg.grpcfg * IDXD_TABLE_OFFSET_MULT;
	user_idxd->wqcfg_offset = offsets_reg.wqcfg * IDXD_TABLE_OFFSET_MULT;
	user_idxd->ims_offset = offsets_reg.ims * IDXD_TABLE_OFFSET_MULT;
	user_idxd->msix_perm_offset = offsets_reg.msix_perm  * IDXD_TABLE_OFFSET_MULT;
	user_idxd->perfmon_offset = offsets_reg.perfmon * IDXD_TABLE_OFFSET_MULT;

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
	genstatus_reg.raw = _idxd_read_4(idxd, IDXD_GENSTATUS_OFFSET);
	assert(genstatus_reg.state == IDXD_DEVICE_STATE_DISABLED);

	_idxd_write_4(idxd, IDXD_CMD_OFFSET, IDXD_ENABLE_DEV << IDXD_CMD_SHIFT);
	rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
	genstatus_reg.raw = _idxd_read_4(idxd, IDXD_GENSTATUS_OFFSET);
	if ((rc < 0) || (genstatus_reg.state != IDXD_DEVICE_STATE_ENABLED)) {
		rc = -EINVAL;
		SPDK_ERRLOG("Error enabling device %u\n", rc);
		goto err_device_enable;
	}

	genstatus_reg.raw = spdk_mmio_read_4((uint32_t *)(user_idxd->reg_base + IDXD_GENSTATUS_OFFSET));
	assert(genstatus_reg.state == IDXD_DEVICE_STATE_ENABLED);

	/*
	 * Enable the work queues that we've configured
	 */
	for (i = 0; i < g_user_dev_cfg.total_wqs; i++) {
		_idxd_write_4(idxd, IDXD_CMD_OFFSET,
			      (IDXD_ENABLE_WQ << IDXD_CMD_SHIFT) | i);
		rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
		if (rc < 0) {
			SPDK_ERRLOG("Error enabling work queues 0x%x\n", rc);
			goto err_wq_enable;
		}
	}

	if ((rc == 0) && (genstatus_reg.state == IDXD_DEVICE_STATE_ENABLED)) {
		SPDK_DEBUGLOG(idxd, "Device enabled, version 0x%x gencap: 0x%lx\n",
			      user_idxd->registers.version,
			      user_idxd->registers.gencap.raw);

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
	uint64_t sw_error_0;
	uint16_t i;

	sw_error_0 = _idxd_read_8(idxd, IDXD_SWERR_OFFSET);

	SPDK_NOTICELOG("SW Error bits set:");
	for (i = 0; i < CHAR_BIT; i++) {
		if ((1ULL << i) & sw_error_0) {
			SPDK_NOTICELOG("    %d\n", i);
		}
	}
	SPDK_NOTICELOG("SW Error error code: %#x\n", (uint8_t)(sw_error_0 >> 8));
	SPDK_NOTICELOG("SW Error WQ index: %u\n", (uint8_t)(sw_error_0 >> 16));
	SPDK_NOTICELOG("SW Error Operation: %u\n", (uint8_t)(sw_error_0 >> 32));
}

static char *
user_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	return (char *)idxd->portals + idxd->wq_id * WQ_TOTAL_PORTAL_SIZE;
}

static bool
user_idxd_nop_check(struct spdk_idxd_device *idxd)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	/* TODO: temp workaround for simulator.  Remove this function when fixed or w/silicon. */
	if (user_idxd->registers.gencap.raw == 0x1833f011f) {
		return true;
	}

	return false;
}

static struct spdk_idxd_impl g_user_idxd_impl = {
	.name			= "user",
	.set_config		= user_idxd_set_config,
	.probe			= user_idxd_probe,
	.destruct		= user_idxd_device_destruct,
	.dump_sw_error		= user_idxd_dump_sw_err,
	.portal_get_addr	= user_idxd_portal_get_addr,
	.nop_check		= user_idxd_nop_check,
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
