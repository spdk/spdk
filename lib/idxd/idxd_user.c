/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd_internal.h"

struct spdk_user_idxd_device {
	struct spdk_idxd_device	idxd;
	struct spdk_pci_device	*device;
	int			sock_id;
	struct idxd_registers	*registers;
};

#define __user_idxd(idxd) (struct spdk_user_idxd_device *)idxd

pthread_mutex_t	g_driver_lock = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_idxd_device *idxd_attach(struct spdk_pci_device *device);

/* Used for control commands, not for descriptor submission. */
static int
idxd_wait_cmd(struct spdk_user_idxd_device *user_idxd, int _timeout)
{
	uint32_t timeout = _timeout;
	union idxd_cmdsts_register cmd_status = {};

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
idxd_unmap_pci_bar(struct spdk_user_idxd_device *user_idxd, int bar)
{
	int rc = 0;
	void *addr = NULL;

	if (bar == IDXD_MMIO_BAR) {
		addr = (void *)user_idxd->registers;
	} else if (bar == IDXD_WQ_BAR) {
		addr = (void *)user_idxd->idxd.portal;
	}

	if (addr) {
		rc = spdk_pci_device_unmap_bar(user_idxd->device, 0, addr);
	}
	return rc;
}

static int
idxd_map_pci_bars(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_MMIO_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}
	user_idxd->registers = (struct idxd_registers *)addr;

	rc = spdk_pci_device_map_bar(user_idxd->device, IDXD_WQ_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		rc = idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);
		if (rc) {
			SPDK_ERRLOG("unable to unmap MMIO bar\n");
		}
		return -EINVAL;
	}
	user_idxd->idxd.portal = addr;

	return 0;
}

static void
idxd_disable_dev(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	union idxd_cmd_register cmd = {};

	cmd.command_code = IDXD_DISABLE_DEV;

	assert(&user_idxd->registers->cmd.raw); /* scan-build */
	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error disabling device %u\n", rc);
	}
}

static int
idxd_reset_dev(struct spdk_user_idxd_device *user_idxd)
{
	int rc;
	union idxd_cmd_register cmd = {};

	cmd.command_code = IDXD_RESET_DEVICE;

	spdk_mmio_write_4(&user_idxd->registers->cmd.raw, cmd.raw);
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error resetting device %u\n", rc);
	}

	return rc;
}

static int
idxd_group_config(struct spdk_user_idxd_device *user_idxd)
{
	int i;
	union idxd_groupcap_register groupcap;
	union idxd_enginecap_register enginecap;
	union idxd_wqcap_register wqcap;
	union idxd_offsets_register table_offsets;

	struct idxd_grptbl *grptbl;
	struct idxd_grpcfg grpcfg = {};

	groupcap.raw = spdk_mmio_read_8(&user_idxd->registers->groupcap.raw);
	enginecap.raw = spdk_mmio_read_8(&user_idxd->registers->enginecap.raw);
	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	if (wqcap.num_wqs < 1) {
		return -ENOTSUP;
	}

	/* Build one group with all of the engines and a single work queue. */
	grpcfg.wqs[0] = 1;
	grpcfg.flags.read_buffers_allowed = groupcap.read_bufs;
	for (i = 0; i < enginecap.num_engines; i++) {
		grpcfg.engines |= (1 << i);
	}

	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	grptbl = (struct idxd_grptbl *)((uint8_t *)user_idxd->registers + (table_offsets.grpcfg *
					IDXD_TABLE_OFFSET_MULT));

	/* Write the group we've configured */
	spdk_mmio_write_8(&grptbl->group[0].wqs[0], grpcfg.wqs[0]);
	spdk_mmio_write_8(&grptbl->group[0].wqs[1], 0);
	spdk_mmio_write_8(&grptbl->group[0].wqs[2], 0);
	spdk_mmio_write_8(&grptbl->group[0].wqs[3], 0);
	spdk_mmio_write_8(&grptbl->group[0].engines, grpcfg.engines);
	spdk_mmio_write_4(&grptbl->group[0].flags.raw, grpcfg.flags.raw);

	/* Write zeroes to the rest of the groups */
	for (i = 1 ; i < groupcap.num_groups; i++) {
		spdk_mmio_write_8(&grptbl->group[i].wqs[0], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[1], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[2], 0L);
		spdk_mmio_write_8(&grptbl->group[i].wqs[3], 0L);
		spdk_mmio_write_8(&grptbl->group[i].engines, 0L);
		spdk_mmio_write_4(&grptbl->group[i].flags.raw, 0L);
	}

	return 0;
}

static int
idxd_wq_config(struct spdk_user_idxd_device *user_idxd)
{
	uint32_t i;
	struct spdk_idxd_device *idxd = &user_idxd->idxd;
	union idxd_wqcap_register wqcap;
	union idxd_offsets_register table_offsets;
	union idxd_wqcfg *wqcfg;

	wqcap.raw = spdk_mmio_read_8(&user_idxd->registers->wqcap.raw);

	SPDK_DEBUGLOG(idxd, "Total ring slots available 0x%x\n", wqcap.total_wq_size);

	idxd->total_wq_size = wqcap.total_wq_size;
	/* Spread the channels we allow per device based on the total number of WQE to try
	 * and achieve optimal performance for common cases.
	 */
	idxd->chan_per_device = (idxd->total_wq_size >= 128) ? 8 : 4;

	table_offsets.raw[0] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[0]);
	table_offsets.raw[1] = spdk_mmio_read_8(&user_idxd->registers->offsets.raw[1]);

	wqcfg = (union idxd_wqcfg *)((uint8_t *)user_idxd->registers + (table_offsets.wqcfg *
				     IDXD_TABLE_OFFSET_MULT));

	for (i = 0 ; i < SPDK_COUNTOF(wqcfg->raw); i++) {
		wqcfg->raw[i] = spdk_mmio_read_4(&wqcfg->raw[i]);
	}

	wqcfg->wq_size = wqcap.total_wq_size;
	wqcfg->mode = WQ_MODE_DEDICATED;
	wqcfg->max_batch_shift = LOG2_WQ_MAX_BATCH;
	wqcfg->max_xfer_shift = LOG2_WQ_MAX_XFER;
	wqcfg->wq_state = WQ_ENABLED;
	wqcfg->priority = WQ_PRIORITY_1;

	for (i = 0; i < SPDK_COUNTOF(wqcfg->raw); i++) {
		spdk_mmio_write_4(&wqcfg->raw[i], wqcfg->raw[i]);
	}

	return 0;
}

static int
idxd_device_configure(struct spdk_user_idxd_device *user_idxd)
{
	int rc = 0;
	union idxd_gensts_register gensts_reg;
	union idxd_cmd_register cmd = {};

	/*
	 * Map BAR0 and BAR2
	 */
	rc = idxd_map_pci_bars(user_idxd);
	if (rc) {
		return rc;
	}

	/*
	 * Reset the device
	 */
	rc = idxd_reset_dev(user_idxd);
	if (rc) {
		goto err_reset;
	}

	/*
	 * Save the device version for use in the common library code.
	 */
	user_idxd->idxd.version = user_idxd->registers->version;

	/*
	 * Configure groups and work queues.
	 */
	rc = idxd_group_config(user_idxd);
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
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
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
	rc = idxd_wait_cmd(user_idxd, IDXD_REGISTER_TIMEOUT_US);
	if (rc < 0) {
		SPDK_ERRLOG("Error enabling work queues 0x%x\n", rc);
		goto err_wq_enable;
	}

	if ((rc == 0) && (gensts_reg.state == IDXD_DEVICE_STATE_ENABLED)) {
		SPDK_DEBUGLOG(idxd, "Device enabled VID 0x%x DID 0x%x\n",
			      user_idxd->device->id.vendor_id, user_idxd->device->id.device_id);
	}

	return rc;
err_wq_enable:
err_device_enable:
err_wq_cfg:
err_group_cfg:
err_reset:
	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);

	return rc;
}

static void
user_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	struct spdk_user_idxd_device *user_idxd = __user_idxd(idxd);

	idxd_disable_dev(user_idxd);

	idxd_unmap_pci_bar(user_idxd, IDXD_MMIO_BAR);
	idxd_unmap_pci_bar(user_idxd, IDXD_WQ_BAR);

	spdk_pci_device_detach(user_idxd->device);
	if (idxd->type == IDXD_DEV_TYPE_IAA) {
		spdk_free(idxd->aecs);
	}
	free(user_idxd);
}

struct idxd_enum_ctx {
	spdk_idxd_probe_cb probe_cb;
	spdk_idxd_attach_cb attach_cb;
	void *cb_ctx;
};

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

/* This function must only be called while holding g_driver_lock */
static int
idxd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct idxd_enum_ctx *enum_ctx = ctx;
	struct spdk_idxd_device *idxd;

	/* Call the user probe_cb to see if they want this device or not, if not
	 * skip it with a positive return code.
	 */
	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev) == false) {
		return 1;
	}

	if (probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		idxd = idxd_attach(pci_dev);
		if (idxd == NULL) {
			SPDK_ERRLOG("idxd_attach() failed\n");
			return -EINVAL;
		}

		enum_ctx->attach_cb(enum_ctx->cb_ctx, idxd);
	}

	return 0;
}

/* The IDXD driver supports 2 distinct HW units, DSA and IAA. */
static int
user_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		spdk_idxd_probe_cb probe_cb)
{
	int rc;
	struct idxd_enum_ctx enum_ctx;

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	pthread_mutex_lock(&g_driver_lock);
	rc = spdk_pci_enumerate(spdk_pci_idxd_get_driver(), idxd_enum_cb, &enum_ctx);
	pthread_mutex_unlock(&g_driver_lock);
	assert(rc == 0);

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

/*
 * Fixed Huffman tables the IAA hardware requires to implement RFC-1951.
 */
const uint32_t fixed_ll_sym[286] = {
	0x40030, 0x40031, 0x40032, 0x40033, 0x40034, 0x40035, 0x40036, 0x40037,
	0x40038, 0x40039, 0x4003A, 0x4003B, 0x4003C, 0x4003D, 0x4003E, 0x4003F,
	0x40040, 0x40041, 0x40042, 0x40043, 0x40044, 0x40045, 0x40046, 0x40047,
	0x40048, 0x40049, 0x4004A, 0x4004B, 0x4004C, 0x4004D, 0x4004E, 0x4004F,
	0x40050, 0x40051, 0x40052, 0x40053, 0x40054, 0x40055, 0x40056, 0x40057,
	0x40058, 0x40059, 0x4005A, 0x4005B, 0x4005C, 0x4005D, 0x4005E, 0x4005F,
	0x40060, 0x40061, 0x40062, 0x40063, 0x40064, 0x40065, 0x40066, 0x40067,
	0x40068, 0x40069, 0x4006A, 0x4006B, 0x4006C, 0x4006D, 0x4006E, 0x4006F,
	0x40070, 0x40071, 0x40072, 0x40073, 0x40074, 0x40075, 0x40076, 0x40077,
	0x40078, 0x40079, 0x4007A, 0x4007B, 0x4007C, 0x4007D, 0x4007E, 0x4007F,
	0x40080, 0x40081, 0x40082, 0x40083, 0x40084, 0x40085, 0x40086, 0x40087,
	0x40088, 0x40089, 0x4008A, 0x4008B, 0x4008C, 0x4008D, 0x4008E, 0x4008F,
	0x40090, 0x40091, 0x40092, 0x40093, 0x40094, 0x40095, 0x40096, 0x40097,
	0x40098, 0x40099, 0x4009A, 0x4009B, 0x4009C, 0x4009D, 0x4009E, 0x4009F,
	0x400A0, 0x400A1, 0x400A2, 0x400A3, 0x400A4, 0x400A5, 0x400A6, 0x400A7,
	0x400A8, 0x400A9, 0x400AA, 0x400AB, 0x400AC, 0x400AD, 0x400AE, 0x400AF,
	0x400B0, 0x400B1, 0x400B2, 0x400B3, 0x400B4, 0x400B5, 0x400B6, 0x400B7,
	0x400B8, 0x400B9, 0x400BA, 0x400BB, 0x400BC, 0x400BD, 0x400BE, 0x400BF,
	0x48190, 0x48191, 0x48192, 0x48193, 0x48194, 0x48195, 0x48196, 0x48197,
	0x48198, 0x48199, 0x4819A, 0x4819B, 0x4819C, 0x4819D, 0x4819E, 0x4819F,
	0x481A0, 0x481A1, 0x481A2, 0x481A3, 0x481A4, 0x481A5, 0x481A6, 0x481A7,
	0x481A8, 0x481A9, 0x481AA, 0x481AB, 0x481AC, 0x481AD, 0x481AE, 0x481AF,
	0x481B0, 0x481B1, 0x481B2, 0x481B3, 0x481B4, 0x481B5, 0x481B6, 0x481B7,
	0x481B8, 0x481B9, 0x481BA, 0x481BB, 0x481BC, 0x481BD, 0x481BE, 0x481BF,
	0x481C0, 0x481C1, 0x481C2, 0x481C3, 0x481C4, 0x481C5, 0x481C6, 0x481C7,
	0x481C8, 0x481C9, 0x481CA, 0x481CB, 0x481CC, 0x481CD, 0x481CE, 0x481CF,
	0x481D0, 0x481D1, 0x481D2, 0x481D3, 0x481D4, 0x481D5, 0x481D6, 0x481D7,
	0x481D8, 0x481D9, 0x481DA, 0x481DB, 0x481DC, 0x481DD, 0x481DE, 0x481DF,
	0x481E0, 0x481E1, 0x481E2, 0x481E3, 0x481E4, 0x481E5, 0x481E6, 0x481E7,
	0x481E8, 0x481E9, 0x481EA, 0x481EB, 0x481EC, 0x481ED, 0x481EE, 0x481EF,
	0x481F0, 0x481F1, 0x481F2, 0x481F3, 0x481F4, 0x481F5, 0x481F6, 0x481F7,
	0x481F8, 0x481F9, 0x481FA, 0x481FB, 0x481FC, 0x481FD, 0x481FE, 0x481FF,
	0x38000, 0x38001, 0x38002, 0x38003, 0x38004, 0x38005, 0x38006, 0x38007,
	0x38008, 0x38009, 0x3800A, 0x3800B, 0x3800C, 0x3800D, 0x3800E, 0x3800F,
	0x38010, 0x38011, 0x38012, 0x38013, 0x38014, 0x38015, 0x38016, 0x38017,
	0x400C0, 0x400C1, 0x400C2, 0x400C3, 0x400C4, 0x400C5
};

const uint32_t fixed_d_sym[30] = {
	0x28000, 0x28001, 0x28002, 0x28003, 0x28004, 0x28005, 0x28006, 0x28007,
	0x28008, 0x28009, 0x2800A, 0x2800B, 0x2800C, 0x2800D, 0x2800E, 0x2800F,
	0x28010, 0x28011, 0x28012, 0x28013, 0x28014, 0x28015, 0x28016, 0x28017,
	0x28018, 0x28019, 0x2801A, 0x2801B, 0x2801C, 0x2801D
};
#define DYNAMIC_HDR			0x2
#define DYNAMIC_HDR_SIZE		3

/* Caller must hold g_driver_lock */
static struct spdk_idxd_device *
idxd_attach(struct spdk_pci_device *device)
{
	struct spdk_user_idxd_device *user_idxd;
	struct spdk_idxd_device *idxd;
	uint16_t did = device->id.device_id;
	uint32_t cmd_reg;
	uint64_t updated = sizeof(struct iaa_aecs);
	int rc;

	user_idxd = calloc(1, sizeof(struct spdk_user_idxd_device));
	if (user_idxd == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for user_idxd device.\n");
		return NULL;
	}

	idxd = &user_idxd->idxd;
	if (did == PCI_DEVICE_ID_INTEL_DSA) {
		idxd->type = IDXD_DEV_TYPE_DSA;
	} else if (did == PCI_DEVICE_ID_INTEL_IAA) {
		idxd->type = IDXD_DEV_TYPE_IAA;
		idxd->aecs = spdk_zmalloc(sizeof(struct iaa_aecs),
					  0x20, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (idxd->aecs == NULL) {
			SPDK_ERRLOG("Failed to allocate iaa aecs\n");
			goto err;
		}

		idxd->aecs_addr = spdk_vtophys((void *)idxd->aecs, &updated);
		if (idxd->aecs_addr == SPDK_VTOPHYS_ERROR || updated < sizeof(struct iaa_aecs)) {
			SPDK_ERRLOG("Failed to translate iaa aecs\n");
			spdk_free(idxd->aecs);
			goto err;
		}

		/* Configure aecs table using fixed Huffman table */
		idxd->aecs->output_accum[0] = DYNAMIC_HDR | 1;
		idxd->aecs->num_output_accum_bits = DYNAMIC_HDR_SIZE;

		/* Add Huffman table to aecs */
		memcpy(idxd->aecs->ll_sym, fixed_ll_sym, sizeof(fixed_ll_sym));
		memcpy(idxd->aecs->d_sym, fixed_d_sym, sizeof(fixed_d_sym));
	}

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
