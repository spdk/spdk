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

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}
	chan->idxd = idxd;

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

	chan->ring_ctrl.data_desc = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_hw_desc),
				    0x40, NULL,
				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.data_desc == NULL) {
		SPDK_ERRLOG("Failed to allocate descriptor memory\n");
		spdk_bit_array_free(&chan->ring_ctrl.ring_slots);
		return -ENOMEM;
	}

	chan->ring_ctrl.completions = spdk_zmalloc(num_ring_slots * sizeof(struct idxd_comp),
				      0x40, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ring_ctrl.completions == NULL) {
		SPDK_ERRLOG("Failed to allocate completion memory\n");
		spdk_bit_array_free(&chan->ring_ctrl.ring_slots);
		spdk_free(chan->ring_ctrl.data_desc);
		return -ENOMEM;
	}

	chan->ring_ctrl.portal = (char *)chan->idxd->portals + chan->idxd->wq_id * PORTAL_SIZE;

	return 0;
}

static void
_idxd_drain(struct spdk_idxd_io_channel *chan)
{
	uint32_t index;
	int set = 0;

	/*
	 * TODO this is a temp solution to drain until getting the drain cmd to work, this
	 * provides equivalent functionality but just doesn't use the device to do it.
	 */
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

	_idxd_drain(chan);

	assert(spdk_bit_array_count_set(chan->ring_ctrl.ring_slots) == 0);

	if (num_channels == 0) {
		spdk_free(chan->ring_ctrl.completions);
		spdk_free(chan->ring_ctrl.data_desc);
		spdk_bit_array_free(&chan->ring_ctrl.ring_slots);
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

int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
		      uint64_t nbytes,
		      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	uint32_t index;
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;

	index = spdk_bit_array_find_first_clear(chan->ring_ctrl.ring_slots, 0);
	if (index == UINT32_MAX) {
		/* ran out of ring slots */
		return -EBUSY;
	}

	spdk_bit_array_set(chan->ring_ctrl.ring_slots, index);

	desc = &chan->ring_ctrl.data_desc[index];
	comp = &chan->ring_ctrl.completions[index];

	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = (uintptr_t)&comp->hw;
	desc->src_addr = (uintptr_t)src;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;
	comp->cb_arg = (uint64_t)cb_arg;
	comp->cb_fn = cb_fn;

	movdir64b((uint64_t *)chan->ring_ctrl.portal, desc);

	return 0;
}

int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan, void *dst, uint64_t fill_pattern,
		      uint64_t nbytes,
		      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	uint32_t index;
	struct idxd_hw_desc *desc;
	struct idxd_comp *comp;

	index = spdk_bit_array_find_first_clear(chan->ring_ctrl.ring_slots, 0);
	if (index == UINT32_MAX) {
		/* ran out of ring slots */
		return -EBUSY;
	}

	spdk_bit_array_set(chan->ring_ctrl.ring_slots, index);

	desc = &chan->ring_ctrl.data_desc[index];
	comp = &chan->ring_ctrl.completions[index];

	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	desc->completion_addr = (uintptr_t)&comp->hw;
	desc->pattern = fill_pattern;
	desc->dst_addr = (uintptr_t)dst;
	desc->xfer_size = nbytes;
	comp->cb_arg = (uint64_t)cb_arg;
	comp->cb_fn = cb_fn;

	movdir64b((uint64_t *)chan->ring_ctrl.portal, desc);

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

	for (index = 0; index < chan->ring_ctrl.max_ring_slots; index++) {
		if (spdk_bit_array_get(chan->ring_ctrl.ring_slots, index)) {
			comp = &chan->ring_ctrl.completions[index];
			if (comp->hw.status == 1) {
				sw_error_0 = _idxd_read_8(chan->idxd, IDXD_SWERR_OFFSET);
				if (sw_error_0 & 0x1) {
					_dump_error_reg(chan);
					status = -EINVAL;
				}

				comp->cb_fn((void *)comp->cb_arg, status);
				comp->hw.status = status = 0;
				spdk_bit_array_clear(chan->ring_ctrl.ring_slots, index);
			}
		}
	}
}

SPDK_LOG_REGISTER_COMPONENT("idxd", SPDK_LOG_IDXD)
