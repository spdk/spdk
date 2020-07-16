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

#include "spdk_internal/log.h"
#include "spdk_internal/idxd.h"

#include "idxd.h"

#define ALIGN_4K 0x1000

pthread_mutex_t	g_driver_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * g_dev_cfg gives us 2 pre-set configurations of DSA to choose from
 * via RPC.
 */
struct device_config *g_dev_cfg = NULL;

/*
 * Pre-built configurations. Variations depend on various factors
 * including how many different types of target latency profiles there
 * are, how many different QOS requirements there might be, etc.
 */
struct device_config g_dev_cfg0 = {
	.config_num = 0,
	.num_groups = 4,
	.num_wqs_per_group = 1,
	.num_engines_per_group = 1,
	.total_wqs = 4,
	.total_engines = 4,
};

struct device_config g_dev_cfg1 = {
	.config_num = 1,
	.num_groups = 2,
	.num_wqs_per_group = 2,
	.num_engines_per_group = 2,
	.total_wqs = 4,
	.total_engines = 4,
};

static uint32_t
_idxd_read_4(struct spdk_idxd_device *idxd, uint32_t offset)
{
	return spdk_mmio_read_4((uint32_t *)(idxd->reg_base + offset));
}

static void
_idxd_write_4(struct spdk_idxd_device *idxd, uint32_t offset, uint32_t value)
{
	spdk_mmio_write_4((uint32_t *)(idxd->reg_base + offset), value);
}

static uint64_t
_idxd_read_8(struct spdk_idxd_device *idxd, uint32_t offset)
{
	return spdk_mmio_read_8((uint64_t *)(idxd->reg_base + offset));
}

static void
_idxd_write_8(struct spdk_idxd_device *idxd, uint32_t offset, uint64_t value)
{
	spdk_mmio_write_8((uint64_t *)(idxd->reg_base + offset), value);
}

struct spdk_idxd_io_channel *
spdk_idxd_get_channel(struct spdk_idxd_device *idxd)
{
	struct spdk_idxd_io_channel *chan;
	struct idxd_batch *batch;
	int i;

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}
	chan->idxd = idxd;

	TAILQ_INIT(&chan->batches);

	TAILQ_INIT(&chan->batch_pool);
	for (i = 0 ; i < NUM_BATCHES ; i++) {
		batch = calloc(1, sizeof(struct idxd_batch));
		if (batch == NULL) {
			SPDK_ERRLOG("Failed to allocate batch\n");
			while ((batch = TAILQ_FIRST(&chan->batch_pool))) {
				TAILQ_REMOVE(&chan->batch_pool, batch, link);
				free(batch);
			}
			return NULL;
		}
		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
	}

	return chan;
}

void
spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan)
{
	free(chan);
}

int
spdk_idxd_configure_chan(struct spdk_idxd_io_channel *chan)
{
	uint32_t num_ring_slots;
	int rc;

	/* Round robin the WQ selection for the chan on this IDXD device. */
	chan->idxd->wq_id++;
	if (chan->idxd->wq_id == g_dev_cfg->total_wqs) {
		chan->idxd->wq_id = 0;
	}

	num_ring_slots = chan->idxd->queues[chan->idxd->wq_id].wqcfg.wq_size;

	chan->ring_ctrl.ring_slots = spdk_bit_array_create(num_ring_slots);
	if (chan->ring_ctrl.ring_slots == NULL) {
		SPDK_ERRLOG("Failed to allocate bit array for ring\n");
		return -ENOMEM;
	}

	/*
	 * max ring slots can change as channels come and go but we
	 * start off getting all of the slots for this work queue.
	 */
	chan->ring_ctrl.max_ring_slots = num_ring_slots;

	/* Store the original size of the ring. */
	chan->ring_ctrl.ring_size = num_ring_slots;

	chan->ring_ctrl.desc = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_hw_desc),
					    0x40, NULL,
					    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.desc == NULL) {
		SPDK_ERRLOG("Failed to allocate descriptor memory\n");
		rc = -ENOMEM;
		goto err_desc;
	}

	chan->ring_ctrl.completions = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_comp),
				      0x40, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.completions == NULL) {
		SPDK_ERRLOG("Failed to allocate completion memory\n");
		rc = -ENOMEM;
		goto err_comp;
	}

	chan->ring_ctrl.user_desc = spdk_zmalloc(TOTAL_USER_DESC * sizeof(struct idxd_hw_desc),
				    0x40, NULL,
				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.user_desc == NULL) {
		SPDK_ERRLOG("Failed to allocate batch descriptor memory\n");
		rc = -ENOMEM;
		goto err_user_desc;
	}

	/* Each slot on the ring reserves DESC_PER_BATCH elemnts in user_desc. */
	chan->ring_ctrl.user_ring_slots = spdk_bit_array_create(NUM_BATCHES);
	if (chan->ring_ctrl.user_ring_slots == NULL) {
		SPDK_ERRLOG("Failed to allocate bit array for user ring\n");
		rc = -ENOMEM;
		goto err_user_ring;
	}

	chan->ring_ctrl.user_completions = spdk_zmalloc(TOTAL_USER_DESC * sizeof(struct idxd_comp),
					   0x40, NULL,
					   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.user_completions == NULL) {
		SPDK_ERRLOG("Failed to allocate user completion memory\n");
		rc = -ENOMEM;
		goto err_user_comp;
	}

	chan->ring_ctrl.portal = (char *)chan->idxd->portals + chan->idxd->wq_id * PORTAL_SIZE;

	return 0;

err_user_comp:
	spdk_bit_array_free(&chan->ring_ctrl.user_ring_slots);
err_user_ring:
	spdk_free(chan->ring_ctrl.user_desc);
err_user_desc:
	spdk_free(chan->ring_ctrl.completions);
err_comp:
	spdk_free(chan->ring_ctrl.desc);
err_desc:
	spdk_bit_array_free(&chan->ring_ctrl.ring_slots);

	return rc;
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

static void
_idxd_drain(struct spdk_idxd_io_channel *chan)
{
	uint32_t index;
	int set = 0;

	do {
		spdk_idxd_process_events(chan);
		set = 0;
		for (index = 0; index < chan->ring_ctrl.max_ring_slots; index++) {
			set |= spdk_bit_array_get(chan->ring_ctrl.ring_slots, index);
		}
	} while (set);
}

int
spdk_idxd_reconfigure_chan(struct spdk_idxd_io_channel *chan, uint32_t num_channels)
{
	uint32_t num_ring_slots;
	int rc;
	struct idxd_batch *batch;

	_idxd_drain(chan);

	assert(spdk_bit_array_count_set(chan->ring_ctrl.ring_slots) == 0);

	if (num_channels == 0) {
		spdk_free(chan->ring_ctrl.completions);
		spdk_free(chan->ring_ctrl.desc);
		spdk_bit_array_free(&chan->ring_ctrl.ring_slots);
		spdk_free(chan->ring_ctrl.user_completions);
		spdk_free(chan->ring_ctrl.user_desc);
		spdk_bit_array_free(&chan->ring_ctrl.user_ring_slots);
		while ((batch = TAILQ_FIRST(&chan->batch_pool))) {
			TAILQ_REMOVE(&chan->batch_pool, batch, link);
			free(batch);
		}
		return 0;
	}

	num_ring_slots = chan->ring_ctrl.ring_size / num_channels;

	/* re-allocate our descriptor ring for hw flow control. */
	rc = spdk_bit_array_resize(&chan->ring_ctrl.ring_slots, num_ring_slots);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to resize channel bit array\n");
		return -ENOMEM;
	}

	chan->ring_ctrl.max_ring_slots = num_ring_slots;

	/*
	 * Note: The batch descriptor ring does not change with the
	 * number of channels as descriptors on this ring do not
	 * "count" for flow control.
	 */

	return rc;
}

/* Called via RPC to select a pre-defined configuration. */
void
spdk_idxd_set_config(uint32_t config_num)
{
	switch (config_num) {
	case 0:
		g_dev_cfg = &g_dev_cfg0;
		break;
	case 1:
		g_dev_cfg = &g_dev_cfg1;
		break;
	default:
		g_dev_cfg = &g_dev_cfg0;
		SPDK_ERRLOG("Invalid config, using default\n");
		break;
	}
}

static int
idxd_unmap_pci_bar(struct spdk_idxd_device *idxd, int bar)
{
	int rc = 0;
	void *addr = NULL;

	if (bar == IDXD_MMIO_BAR) {
		addr = (void *)idxd->reg_base;
	} else if (bar == IDXD_WQ_BAR) {
		addr = (void *)idxd->portals;
	}

	if (addr) {
		rc = spdk_pci_device_unmap_bar(idxd->device, 0, addr);
	}
	return rc;
}

static int
idxd_map_pci_bars(struct spdk_idxd_device *idxd)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(idxd->device, IDXD_MMIO_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}
	idxd->reg_base = addr;

	rc = spdk_pci_device_map_bar(idxd->device, IDXD_WQ_BAR, &addr, &phys_addr, &size);
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

	assert(g_dev_cfg->num_groups <= idxd->registers.groupcap.num_groups);
	idxd->groups = calloc(idxd->registers.groupcap.num_groups, sizeof(struct idxd_group));
	if (idxd->groups == NULL) {
		SPDK_ERRLOG("Failed to allocate group memory\n");
		return -ENOMEM;
	}

	assert(g_dev_cfg->total_engines <= idxd->registers.enginecap.num_engines);
	for (i = 0; i < g_dev_cfg->total_engines; i++) {
		idxd->groups[i % g_dev_cfg->num_groups].grpcfg.engines |= (1 << i);
	}

	assert(g_dev_cfg->total_wqs <= idxd->registers.wqcap.num_wqs);
	for (i = 0; i < g_dev_cfg->total_wqs; i++) {
		idxd->groups[i % g_dev_cfg->num_groups].grpcfg.wqs[0] |= (1 << i);
	}

	for (i = 0; i < g_dev_cfg->num_groups; i++) {
		idxd->groups[i].idxd = idxd;
		idxd->groups[i].id = i;

		/* Divide BW tokens evenly */
		idxd->groups[i].grpcfg.flags.tokens_allowed =
			idxd->registers.groupcap.total_tokens / g_dev_cfg->num_groups;
	}

	/*
	 * Now write the group config to the device for all groups. We write
	 * to the max number of groups in order to 0 out the ones we didn't
	 * configure.
	 */
	for (i = 0 ; i < idxd->registers.groupcap.num_groups; i++) {

		base_offset = idxd->grpcfg_offset + i * 64;

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
idxd_wq_config(struct spdk_idxd_device *idxd)
{
	int i, j;
	struct idxd_wq *queue;
	u_int32_t wq_size = idxd->registers.wqcap.total_wq_size / g_dev_cfg->total_wqs;

	SPDK_NOTICELOG("Total ring slots available space 0x%x, so per work queue is 0x%x\n",
		       idxd->registers.wqcap.total_wq_size, wq_size);
	assert(g_dev_cfg->total_wqs <= IDXD_MAX_QUEUES);
	assert(g_dev_cfg->total_wqs <= idxd->registers.wqcap.num_wqs);
	assert(LOG2_WQ_MAX_BATCH <= idxd->registers.gencap.max_batch_shift);
	assert(LOG2_WQ_MAX_XFER <= idxd->registers.gencap.max_xfer_shift);

	idxd->queues = calloc(1, idxd->registers.wqcap.num_wqs * sizeof(struct idxd_wq));
	if (idxd->queues == NULL) {
		SPDK_ERRLOG("Failed to allocate queue memory\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_dev_cfg->total_wqs; i++) {
		queue = &idxd->queues[i];
		queue->wqcfg.wq_size = wq_size;
		queue->wqcfg.mode = WQ_MODE_DEDICATED;
		queue->wqcfg.max_batch_shift = LOG2_WQ_MAX_BATCH;
		queue->wqcfg.max_xfer_shift = LOG2_WQ_MAX_XFER;
		queue->wqcfg.wq_state = WQ_ENABLED;
		queue->wqcfg.priority = WQ_PRIORITY_1;

		/* Not part of the config struct */
		queue->idxd = idxd;
		queue->group = &idxd->groups[i % g_dev_cfg->num_groups];
	}

	/*
	 * Now write the work queue config to the device for all wq space
	 */
	for (i = 0 ; i < idxd->registers.wqcap.num_wqs; i++) {
		queue = &idxd->queues[i];
		for (j = 0 ; j < WQCFG_NUM_DWORDS; j++) {
			_idxd_write_4(idxd, idxd->wqcfg_offset + i * 32 + j * 4,
				      queue->wqcfg.raw[j]);
		}
	}

	return 0;
}

static int
idxd_device_configure(struct spdk_idxd_device *idxd)
{
	int i, rc = 0;
	union idxd_offsets_register offsets_reg;
	union idxd_genstatus_register genstatus_reg;

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
	idxd->registers.version = _idxd_read_4(idxd, IDXD_VERSION_OFFSET);
	idxd->registers.gencap.raw = _idxd_read_8(idxd, IDXD_GENCAP_OFFSET);
	idxd->registers.wqcap.raw = _idxd_read_8(idxd, IDXD_WQCAP_OFFSET);
	idxd->registers.groupcap.raw = _idxd_read_8(idxd, IDXD_GRPCAP_OFFSET);
	idxd->registers.enginecap.raw = _idxd_read_8(idxd, IDXD_ENGCAP_OFFSET);
	for (i = 0; i < IDXD_OPCAP_WORDS; i++) {
		idxd->registers.opcap.raw[i] =
			_idxd_read_8(idxd, i * sizeof(uint64_t) + IDXD_OPCAP_OFFSET);
	}
	offsets_reg.raw[0] = _idxd_read_8(idxd, IDXD_TABLE_OFFSET);
	offsets_reg.raw[1] = _idxd_read_8(idxd, IDXD_TABLE_OFFSET + sizeof(uint64_t));
	idxd->grpcfg_offset = offsets_reg.grpcfg * IDXD_TABLE_OFFSET_MULT;
	idxd->wqcfg_offset = offsets_reg.wqcfg * IDXD_TABLE_OFFSET_MULT;
	idxd->ims_offset = offsets_reg.ims * IDXD_TABLE_OFFSET_MULT;
	idxd->msix_perm_offset = offsets_reg.msix_perm  * IDXD_TABLE_OFFSET_MULT;
	idxd->perfmon_offset = offsets_reg.perfmon * IDXD_TABLE_OFFSET_MULT;

	/*
	 * Configure groups and work queues.
	 */
	rc = idxd_group_config(idxd);
	if (rc) {
		goto err_group_cfg;
	}

	rc = idxd_wq_config(idxd);
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

	genstatus_reg.raw = spdk_mmio_read_4((uint32_t *)(idxd->reg_base + IDXD_GENSTATUS_OFFSET));
	assert(genstatus_reg.state == IDXD_DEVICE_STATE_ENABLED);

	/*
	 * Enable the work queues that we've configured
	 */
	for (i = 0; i < g_dev_cfg->total_wqs; i++) {
		_idxd_write_4(idxd, IDXD_CMD_OFFSET,
			      (IDXD_ENABLE_WQ << IDXD_CMD_SHIFT) | i);
		rc = idxd_wait_cmd(idxd, IDXD_REGISTER_TIMEOUT_US);
		if (rc < 0) {
			SPDK_ERRLOG("Error enabling work queues 0x%x\n", rc);
			goto err_wq_enable;
		}
	}

	if ((rc == 0) && (genstatus_reg.state == IDXD_DEVICE_STATE_ENABLED)) {
		SPDK_NOTICELOG("Device enabled, version 0x%x gencap: 0x%lx\n",
			       idxd->registers.version,
			       idxd->registers.gencap.raw);

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
idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	idxd_unmap_pci_bar(idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(idxd, IDXD_WQ_BAR);
	free(idxd->groups);
	free(idxd->queues);
	free(idxd);
}

/* Caller must hold g_driver_lock */
static struct spdk_idxd_device *
idxd_attach(struct spdk_pci_device *device)
{
	struct spdk_idxd_device *idxd;
	uint32_t cmd_reg;
	int rc;

	idxd = calloc(1, sizeof(struct spdk_idxd_device));
	if (idxd == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for idxd device.\n");
		return NULL;
	}

	idxd->device = device;

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);
	cmd_reg |= 0x4;
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);

	rc = idxd_device_configure(idxd);
	if (rc) {
		goto err;
	}

	return idxd;
err:
	idxd_device_destruct(idxd);
	return NULL;
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

		enum_ctx->attach_cb(enum_ctx->cb_ctx, pci_dev, idxd);
	}

	return 0;
}

int
spdk_idxd_probe(void *cb_ctx, spdk_idxd_probe_cb probe_cb, spdk_idxd_attach_cb attach_cb)
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

void
spdk_idxd_detach(struct spdk_idxd_device *idxd)
{
	idxd_device_destruct(idxd);
}

static struct idxd_hw_desc *
_idxd_prep_command(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		   void *cb_arg, struct idxd_batch *batch)
{
	uint32_t index;
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;

	index = spdk_bit_array_find_first_clear(chan->ring_ctrl.ring_slots, 0);
	if (index == UINT32_MAX) {
		/* ran out of ring slots */
		return NULL;
	}

	spdk_bit_array_set(chan->ring_ctrl.ring_slots, index);

	desc = &chan->ring_ctrl.desc[index];
	comp = &chan->ring_ctrl.completions[index];

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = (uintptr_t)&comp->hw;
	comp->cb_arg = cb_arg;
	comp->cb_fn = cb_fn;
	if (batch) {
		comp->batch = batch;
		batch->batch_desc_index = index;
	}

	return desc;
}

int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
		      uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, NULL);
	if (desc == NULL) {
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->src_addr = (uintptr_t)src;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

/* Dual-cast copies the same source to two separate destination buffers. */
int
spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan, void *dst1, void *dst2,
			  const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, NULL);
	if (desc == NULL) {
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_DUALCAST;
	desc->src_addr = (uintptr_t)src;
	desc->dst_addr = (uintptr_t)dst1;
	desc->dest2 = (uintptr_t)dst2;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

int
spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan, void *src1, const void *src2,
			 uint64_t nbytes,
			 spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, NULL);
	if (desc == NULL) {
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPARE;
	desc->src_addr = (uintptr_t)src1;
	desc->src2_addr = (uintptr_t)src2;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan, void *dst, uint64_t fill_pattern,
		      uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, NULL);
	if (desc == NULL) {
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->pattern = fill_pattern;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

int
spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan, uint32_t *dst, void *src,
			uint32_t seed, uint64_t nbytes,
			spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, NULL);
	if (desc == NULL) {
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_CRC32C_GEN;
	desc->dst_addr = (uintptr_t)dst;
	desc->src_addr = (uintptr_t)src;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

uint32_t
spdk_idxd_batch_get_max(void)
{
	return DESC_PER_BATCH; /* TODO maybe add startup RPC to set this */
}

struct idxd_batch *
spdk_idxd_batch_create(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch = NULL;

	if (!TAILQ_EMPTY(&chan->batch_pool)) {
		batch = TAILQ_FIRST(&chan->batch_pool);
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
	} else {
		/* The application needs to handle this. */
		return NULL;
	}

	batch->batch_num = spdk_bit_array_find_first_clear(chan->ring_ctrl.user_ring_slots, 0);
	if (batch->batch_num == UINT32_MAX) {
		/* ran out of ring slots, the application needs to handle this. */
		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
		return NULL;
	}

	spdk_bit_array_set(chan->ring_ctrl.user_ring_slots, batch->batch_num);

	/*
	 * Find the first descriptor address for the given batch. The
	 * descriptor ring used for user desctipors is allocated in
	 * units of DESC_PER_BATCH.  The actual index is in units of
	 * one descriptor.
	 */
	batch->start_index = batch->cur_index = batch->batch_num * DESC_PER_BATCH;

	TAILQ_INSERT_TAIL(&chan->batches, batch, link);
	SPDK_DEBUGLOG(SPDK_LOG_IDXD, "New batch %p num %u\n", batch, batch->batch_num);

	return batch;
}

static bool
_does_batch_exist(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	bool found = false;
	struct idxd_batch *cur_batch;

	TAILQ_FOREACH(cur_batch, &chan->batches, link) {
		if (cur_batch == batch) {
			found = true;
			break;
		}
	}

	return found;
}

int
spdk_idxd_batch_cancel(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch)
{
	if (_does_batch_exist(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to cancel a batch that doesn't exist\n.");
		return -EINVAL;
	}

	if (batch->remaining > 0) {
		SPDK_ERRLOG("Cannot cancel batch, already submitted to HW\n.");
		return -EINVAL;
	}

	TAILQ_REMOVE(&chan->batches, batch, link);
	spdk_bit_array_clear(chan->ring_ctrl.user_ring_slots, batch->batch_num);
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);

	return 0;
}

int
spdk_idxd_batch_submit(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
		       spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	if (_does_batch_exist(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to submit a batch that doesn't exist\n.");
		return -EINVAL;
	}

	/* Common prep. */
	desc = _idxd_prep_command(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_IDXD, "Can't submit batch %p busy batch num %u\n", batch, batch->batch_num);
		return -EBUSY;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_BATCH;
	desc->desc_list_addr = (uintptr_t)&chan->ring_ctrl.user_desc[batch->start_index];
	desc->desc_count = batch->cur_index - batch->start_index;
	assert(desc->desc_count <= DESC_PER_BATCH);

	if (desc->desc_count < MIN_USER_DESC_COUNT) {
		SPDK_ERRLOG("Attempt to submit a batch without at least %u operations.\n",
			    MIN_USER_DESC_COUNT);
		return -EINVAL;
	}

	/* Total completions for the batch = num desc plus 1 for the batch desc itself. */
	batch->remaining = desc->desc_count + 1;

	/* Submit operation. */
	movdir64b(chan->ring_ctrl.portal, desc);

	return 0;
}

static struct idxd_hw_desc *
_idxd_prep_batch_cmd(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		     void *cb_arg, struct idxd_batch *batch)
{
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;

	if (_does_batch_exist(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to add to a batch that doesn't exist\n.");
		return NULL;
	}

	if ((batch->cur_index - batch->start_index) == DESC_PER_BATCH) {
		SPDK_ERRLOG("Attempt to add to a batch that is already full\n.");
		return NULL;
	}

	desc = &chan->ring_ctrl.user_desc[batch->cur_index];
	comp = &chan->ring_ctrl.user_completions[batch->cur_index];
	SPDK_DEBUGLOG(SPDK_LOG_IDXD, "Prep batch %p index %u\n", batch, batch->cur_index);

	batch->cur_index++;
	assert(batch->cur_index > batch->start_index);

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = (uintptr_t)&comp->hw;
	comp->cb_arg = cb_arg;
	comp->cb_fn = cb_fn;
	comp->batch = batch;

	return desc;
}

int
spdk_idxd_batch_prep_copy(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			  void *dst, const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->src_addr = (uintptr_t)src;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_fill(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			  void *dst, uint64_t fill_pattern, uint64_t nbytes,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->pattern = fill_pattern;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_dualcast(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			      void *dst1, void *dst2, const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* Common prep. */
	desc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		return -EINVAL;
	}
	desc->opcode = IDXD_OPCODE_DUALCAST;
	desc->src_addr = (uintptr_t)src;
	desc->dst_addr = (uintptr_t)dst1;
	desc->dest2 = (uintptr_t)dst2;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_crc32c(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			    uint32_t *dst, void *src, uint32_t seed, uint64_t nbytes,
			    spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_CRC32C_GEN;
	desc->dst_addr = (uintptr_t)dst;
	desc->src_addr = (uintptr_t)src;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;

	return 0;
}

int
spdk_idxd_batch_prep_compare(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			     void *src1, void *src2, uint64_t nbytes, spdk_idxd_req_cb cb_fn,
			     void *cb_arg)
{
	struct idxd_hw_desc *desc;

	/* Common prep. */
	desc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, batch);
	if (desc == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPARE;
	desc->src_addr = (uintptr_t)src1;
	desc->src2_addr = (uintptr_t)src2;
	desc->xfer_size = nbytes;

	return 0;
}

static void
_dump_error_reg(struct spdk_idxd_io_channel *chan)
{
	uint64_t sw_error_0;
	uint16_t i;

	sw_error_0 = _idxd_read_8(chan->idxd, IDXD_SWERR_OFFSET);

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

static void
_free_batch(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan,
	    struct idxd_comp *comp)
{
	TAILQ_REMOVE(&chan->batches, batch, link);
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
	comp->batch = NULL;
	spdk_bit_array_clear(chan->ring_ctrl.user_ring_slots, batch->batch_num);
	spdk_bit_array_clear(chan->ring_ctrl.ring_slots, batch->batch_desc_index);
}

static void
_spdk_idxd_process_batch_events(struct spdk_idxd_io_channel *chan)
{
	uint16_t index;
	struct idxd_comp *comp;
	uint64_t sw_error_0;
	int status = 0;
	struct idxd_batch *batch;

	/*
	 * We don't check the bit array for user completions as there's only
	 * one bit per per batch.
	 */
	for (index = 0; index < TOTAL_USER_DESC; index++) {
		comp = &chan->ring_ctrl.user_completions[index];
		if (comp->hw.status == 1) {
			struct idxd_hw_desc *desc;

			sw_error_0 = _idxd_read_8(chan->idxd, IDXD_SWERR_OFFSET);
			if (sw_error_0 & 0x1) {
				_dump_error_reg(chan);
				status = -EINVAL;
			}

			desc = &chan->ring_ctrl.user_desc[index];
			switch (desc->opcode) {
			case IDXD_OPCODE_CRC32C_GEN:
				*(uint32_t *)desc->dst_addr = comp->hw.crc32c_val;
				*(uint32_t *)desc->dst_addr ^= ~0;
				break;
			case IDXD_OPCODE_COMPARE:
				if (status == 0) {
					status = comp->hw.result;
				}
				break;
			case IDXD_OPCODE_MEMFILL:
			case IDXD_OPCODE_DUALCAST:
			case IDXD_OPCODE_MEMMOVE:
				break;
			default:
				assert(false);
				break;
			}

			/* The hw will complete all user desc first before the batch
			 * desc (see spec for configuration exceptions) however
			 * because of the order that we check for comps in the poller
			 * we may "see" them in a different order than they actually
			 * completed in.
			 */
			batch = comp->batch;
			assert(batch->remaining > 0);
			if (--batch->remaining == 0) {
				_free_batch(batch, chan, comp);
			}

			comp->cb_fn((void *)comp->cb_arg, status);
			comp->hw.status = status = 0;
		}
	}
}

/*
 * TODO: Experiment with different methods of reaping completions for performance
 * once we have real silicon.
 */
void
spdk_idxd_process_events(struct spdk_idxd_io_channel *chan)
{
	uint16_t index;
	struct idxd_comp *comp;
	uint64_t sw_error_0;
	int status = 0;
	struct idxd_batch *batch;

	if (!TAILQ_EMPTY(&chan->batches)) {
		_spdk_idxd_process_batch_events(chan);
	}

	for (index = 0; index < chan->ring_ctrl.max_ring_slots; index++) {
		if (spdk_bit_array_get(chan->ring_ctrl.ring_slots, index)) {
			comp = &chan->ring_ctrl.completions[index];
			if (comp->hw.status == 1) {
				struct idxd_hw_desc *desc;

				sw_error_0 = _idxd_read_8(chan->idxd, IDXD_SWERR_OFFSET);
				if (sw_error_0 & 0x1) {
					_dump_error_reg(chan);
					status = -EINVAL;
				}

				desc = &chan->ring_ctrl.desc[index];
				switch (desc->opcode) {
				case IDXD_OPCODE_BATCH:
					/* The hw will complete all user desc first before the batch
					 * desc (see spec for configuration exceptions) however
					 * because of the order that we check for comps in the poller
					 * we may "see" them in a different order than they actually
					 * completed in.
					 */
					batch = comp->batch;
					assert(batch->remaining > 0);
					if (--batch->remaining == 0) {
						_free_batch(batch, chan, comp);
					}
					break;
				case IDXD_OPCODE_CRC32C_GEN:
					*(uint32_t *)desc->dst_addr = comp->hw.crc32c_val;
					*(uint32_t *)desc->dst_addr ^= ~0;
					break;
				case IDXD_OPCODE_COMPARE:
					if (status == 0) {
						status = comp->hw.result;
					}
					break;
				}

				comp->cb_fn(comp->cb_arg, status);
				comp->hw.status = status = 0;
				if (desc->opcode != IDXD_OPCODE_BATCH) {
					spdk_bit_array_clear(chan->ring_ctrl.ring_slots, index);
				}
			}
		}
	}
}

SPDK_LOG_REGISTER_COMPONENT("idxd", SPDK_LOG_IDXD)
