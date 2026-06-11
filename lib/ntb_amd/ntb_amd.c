/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "ntb_amd_internal.h"

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/dma.h"
#include "spdk/log.h"

/* PCI Express capability ID. */
#define SPDK_PCI_CAP_ID_EXP 0x10

struct ntb_driver {
	pthread_mutex_t	lock;
	TAILQ_HEAD(, spdk_ntb_amd_dev) attached_chans;
};

static struct ntb_driver g_ntb_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.attached_chans = TAILQ_HEAD_INITIALIZER(g_ntb_driver.attached_chans),
};

static struct spdk_pci_device *ntb_pci_find_upstream_bridge(struct spdk_pci_device *pdev);

/**
 * DMA engine capability flags
 */
enum spdk_ntb_dma_capability_flags {
	SPDK_NTB_ENGINE_COPY_SUPPORTED  = 0x1,
};

static const enum spdk_ntb_bar ntb_amd_bar[] = {
	NTB_BAR23,
	NTB_BAR45,
};

/* Mapping the PCI BAR */
static int
ntb_map_pci_bar(struct spdk_ntb_amd_dev *ntb)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(ntb->device, NTB_PCIE_BAR, &addr, &phys_addr, &size);
	if (rc != 0 || addr == NULL) {
		SPDK_ERRLOG("pci_device_map_range failed with error code %d\n", rc);
		return -1;
	}

	ntb->io_regs = addr;

	return 0;
}

static int
ntb_unmap_pci_bar(struct spdk_ntb_amd_dev *ntb)
{
	int rc = 0;
	void *addr = (void *)ntb->io_regs;

	if (addr) {
		rc = spdk_pci_device_unmap_bar(ntb->device, 0, addr);
		ntb->io_regs = NULL;
	}

	return rc;
}

/*
 * Release every per-memory-window BAR mapping that ntb_channel_start()
 * acquired. Safe to call on partial initialisation: only entries that were
 * actually mapped are non-NULL. Each spdk_pci_device_map_bar() takes a
 * refcount that must be balanced or the BAR stays pinned for the lifetime
 * of the process.
 */
static void
ntb_unmap_mw_bars(struct spdk_ntb_amd_dev *ntb)
{
	uint32_t i;

	if (ntb == NULL) {
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(ntb->mw_vaddr); i++) {
		if (ntb->mw_vaddr[i] != NULL) {
			(void)spdk_pci_device_unmap_bar(ntb->device,
							ntb_amd_bar[i],
							ntb->mw_vaddr[i]);
			ntb->mw_vaddr[i] = NULL;
			ntb->mw_paddr[i] = 0;
		}
	}
}

/*
 * Header offsets used by the upstream-bridge walker. Defined locally so we
 * don't pull in an external "PCI header offsets" header just for these.
 */
#define NTB_PCI_CFG_CLASS_PROG_IF       0x09 /* class-code byte 0 (prog if) */
#define NTB_PCI_CFG_CLASS_SUBCLASS      0x0a /* class-code byte 1 (subclass) */
#define NTB_PCI_CFG_CLASS_BASE          0x0b /* class-code byte 2 (base class) */
#define NTB_PCI_CFG_HEADER_TYPE         0x0e
#define NTB_PCI_CFG_BRIDGE_SECONDARY_BUS    0x19
#define NTB_PCI_CFG_BRIDGE_SUBORDINATE_BUS  0x1a

#define NTB_PCI_CLASS_BRIDGE_BASE       0x06
#define NTB_PCI_CLASS_BRIDGE_SUBCLASS   0x04 /* PCI-PCI bridge */
#define NTB_PCI_HDR_TYPE_BRIDGE         0x01

struct ntb_find_bridge_ctx {
	struct spdk_pci_device *found;
	struct spdk_pci_addr tgt_addr;
	uint8_t found_secondary;
};

static int
ntb_pci_read_class_subclass(struct spdk_pci_device *dev,
			    uint8_t *base_class, uint8_t *subclass)
{
	int rc;

	rc = spdk_pci_device_cfg_read8(dev, base_class, NTB_PCI_CFG_CLASS_BASE);
	if (rc != 0) {
		return rc;
	}
	return spdk_pci_device_cfg_read8(dev, subclass, NTB_PCI_CFG_CLASS_SUBCLASS);
}

static void
ntb_find_bridge_fn(void *ctx, struct spdk_pci_device *dev)
{
	struct ntb_find_bridge_ctx *search = ctx;
	struct spdk_pci_addr daddr = spdk_pci_device_get_addr(dev);
	uint8_t base_class = 0, subclass = 0, hdr_type = 0;
	uint8_t sec_bus = 0, sub_bus = 0;

	/* Same domain as the target. */
	if (daddr.domain != search->tgt_addr.domain) {
		return;
	}

	/* Must be a PCI-PCI bridge (class 0x06, subclass 0x04). */
	if (ntb_pci_read_class_subclass(dev, &base_class, &subclass) != 0) {
		return;
	}
	if (base_class != NTB_PCI_CLASS_BRIDGE_BASE ||
	    subclass != NTB_PCI_CLASS_BRIDGE_SUBCLASS) {
		return;
	}

	/* Must be a Type-1 (bridge) header so the secondary/subordinate
	 * bus-number fields are valid.
	 */
	if (spdk_pci_device_cfg_read8(dev, &hdr_type, NTB_PCI_CFG_HEADER_TYPE) != 0) {
		return;
	}
	if ((hdr_type & 0x7f) != NTB_PCI_HDR_TYPE_BRIDGE) {
		return;
	}

	if (spdk_pci_device_cfg_read8(dev, &sec_bus,
				      NTB_PCI_CFG_BRIDGE_SECONDARY_BUS) != 0 ||
	    spdk_pci_device_cfg_read8(dev, &sub_bus,
				      NTB_PCI_CFG_BRIDGE_SUBORDINATE_BUS) != 0) {
		return;
	}

	/*
	 * The upstream bridge of the target is the one whose secondary-bus..
	 * subordinate-bus range covers the target's bus. Among the bridges
	 * that match, pick the one with the largest secondary bus -- that's
	 * the immediate upstream port (deepest in the hierarchy). This works
	 * across multiple PCI domains, hot-plugged buses, and topologies
	 * where the upstream bridge isn't on bus N-1.
	 */
	if (search->tgt_addr.bus < sec_bus || search->tgt_addr.bus > sub_bus) {
		return;
	}

	if (search->found == NULL || sec_bus > search->found_secondary) {
		search->found = dev;
		search->found_secondary = sec_bus;
	}
}

static struct spdk_pci_device *
ntb_pci_find_upstream_bridge(struct spdk_pci_device *pdev)
{
	struct ntb_find_bridge_ctx ctx = {0};

	if (pdev == NULL) {
		return NULL;
	}

	ctx.tgt_addr = spdk_pci_device_get_addr(pdev);
	spdk_pci_for_each_device(&ctx, ntb_find_bridge_fn);

	return ctx.found;
}

/* Walk the PCI capability list looking for the PCI Express capability. */
static int
find_pcie_capability(struct spdk_pci_device *dev, uint8_t *cap_offset)
{
	uint16_t status;
	uint8_t pos;
	int rc;
	int hops;

	if (dev == NULL || cap_offset == NULL) {
		return -EINVAL;
	}

	rc = spdk_pci_device_cfg_read16(dev, &status, 0x06);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read PCI status register: %d\n", rc);
		return rc;
	}

	if (!(status & 0x10)) {
		SPDK_ERRLOG("Capabilities list not supported by device\n");
		return -ENOTSUP;
	}

	rc = spdk_pci_device_cfg_read8(dev, &pos, 0x34);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read PCI capabilities pointer: %d\n", rc);
		return rc;
	}

	/*
	 * Cap a maximum number of capability-list hops to defend against a
	 * malformed device that returns a cyclic capability chain.
	 */
	for (hops = 0; pos && hops < 64; hops++) {
		uint8_t cap_id, next;

		if (spdk_pci_device_cfg_read8(dev, &cap_id, pos) != 0 ||
		    spdk_pci_device_cfg_read8(dev, &next, pos + 1) != 0) {
			return -EIO;
		}
		if (cap_id == SPDK_PCI_CAP_ID_EXP) {
			*cap_offset = pos;
			SPDK_INFOLOG(ntb, "PCIe capability found at 0x%02x\n", pos);
			return 0;
		}
		pos = next;
	}

	SPDK_ERRLOG("PCIe capability not found\n");
	return -ENOENT;
}

int
spdk_ntb_amd_read_link_status(struct spdk_pci_device *pci_swus, uint16_t *lnk_word)
{
	uint8_t pcie_cap;
	uint16_t linkctl, linksta;
	int rc;

	if (pci_swus == NULL || lnk_word == NULL) {
		return -EINVAL;
	}

	rc = find_pcie_capability(pci_swus, &pcie_cap);
	if (rc != 0) {
		return rc;
	}

	rc = spdk_pci_device_cfg_read16(pci_swus, &linkctl, pcie_cap + 0x10);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read PCIe Link Control register: %d\n", rc);
		return rc;
	}
	rc = spdk_pci_device_cfg_read16(pci_swus, &linksta, pcie_cap + 0x12);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read PCIe Link Status register: %d\n", rc);
		return rc;
	}

	*lnk_word = linksta;

	SPDK_INFOLOG(ntb, "ASPM %s\n", (linkctl & 0x03) ? "Enabled" : "Disabled");
	SPDK_NOTICELOG("Link Speed: Gen%u Link Width: x%u\n",
		       linksta & 0xF, (linksta >> 4) & 0x3F);
	return 0;
}

static int
ntb_link_status(struct spdk_ntb_amd_dev *ntb)
{
	struct spdk_pci_device *pdev = ntb->device;
	struct spdk_pci_device *pci_swus = NULL;
	struct spdk_pci_device *pci_swds = NULL;
	off_t pcie_cap_offset = 0;
	uint16_t lnk_word = 0;

	if (ntb == NULL) {
		SPDK_ERRLOG("Invalid device.\n");
		return -EINVAL;
	}
	if (ntb->topo == NTB_TOPO_PRI) {
		pcie_cap_offset = spdk_ntb_amd_read_link_status(pdev, &lnk_word);

		if (pcie_cap_offset) {
			SPDK_ERRLOG("[%s()] failed to find the pcie capability", __func__);
			return pcie_cap_offset;
		}
	} else if (ntb->topo == NTB_TOPO_SEC) {
		pci_swds = ntb_pci_find_upstream_bridge(pdev);

		if (pci_swds) {
			pci_swus = ntb_pci_find_upstream_bridge(pci_swds);
		}

		if (pci_swus) {
			pcie_cap_offset = spdk_ntb_amd_read_link_status(pci_swus, &lnk_word);
		}

		if (pcie_cap_offset) {
			SPDK_INFOLOG(ntb, "[%s()] failed to find the pcie capability", __func__);
			return pcie_cap_offset;
		}
	}

	ntb->link_status = lnk_word;
	return 0;
}

static int
ntb_poll_link(struct spdk_ntb_amd_dev *ntb)
{
	struct spdk_ntb_amd_dev *ntb_hw;
	void *mmio;
	uint32_t reg;

	ntb_hw = ntb;
	mmio = ntb_hw->peer_mmio;

	reg = spdk_mmio_read_4(mmio + AMD_SIDEINFO_OFFSET);
	reg &= AMD_SIDE_READY;

	ntb_hw->ctl_status = reg;

	ntb_link_status(ntb);

	return ntb_hw->ctl_status;
}

static int
ntb_link_is_up(struct spdk_ntb_amd_dev *ntb)
{
	/*
	 * A non-zero ntb_poll_link() result means our side observes the
	 * peer's SIDE_READY bit. The AMD NTB does not surface a separate
	 * pollable peer link-down event here, so link readiness is
	 * determined purely by the side-info register for both topologies.
	 */
	return ntb_poll_link(ntb);
}

int
spdk_ntb_amd_get_link_status(struct spdk_ntb_amd_dev *ntb)
{
	struct spdk_ntb_amd_dev *ntb_hw;
	int ret;

	ntb_hw = ntb;

	ret = ntb_link_is_up(ntb_hw);
	if (ret) {
		if (ntb->link_status) {
			ntb_hw->link_speed = AMD_NTB_LNK_STA_SPEED(ntb->link_status);
			ntb_hw->link_width = AMD_NTB_LNK_STA_WIDTH(ntb->link_status);
		} else {
			ntb_hw->link_speed = NTB_SPEED_NONE;
			ntb_hw->link_width = NTB_WIDTH_NONE;
		}
	}
	return 0;
}

int
spdk_ntb_amd_set_link(struct spdk_ntb_amd_dev *ntb, bool up)
{
	uint32_t ntb_ctl, ntb_side_ready;
	void *mmio;

	if (ntb == NULL) {
		return -EINVAL;
	}

	mmio = ntb->hw_addr;
	ntb->self_mmio = mmio;

	/*
	 * Set or clear AMD_SIDE_READY in the side-info register: it is the
	 * NTB-level "I am ready to talk" advertisement that the peer polls.
	 * Set or clear PMM/SMM control to enable or disable register-side
	 * memory translation. The caller controls direction via 'up'.
	 */
	ntb_side_ready = spdk_mmio_read_4(mmio + AMD_SIDEINFO_OFFSET);
	ntb_ctl = spdk_mmio_read_4(mmio + AMD_CNTL_OFFSET);

	if (up) {
		ntb_side_ready |= AMD_SIDE_READY;
		ntb_ctl |= (PMM_REG_CTL | SMM_REG_CTL);
	} else {
		ntb_side_ready &= ~AMD_SIDE_READY;
		ntb_ctl &= ~(PMM_REG_CTL | SMM_REG_CTL);
	}

	spdk_mmio_write_4(mmio + AMD_SIDEINFO_OFFSET, ntb_side_ready);
	spdk_mmio_write_4(mmio + AMD_CNTL_OFFSET, ntb_ctl);

	return 0;
}

static int
ntb_channel_start(struct spdk_ntb_amd_dev *ntb)
{
	uint32_t i, j, bar_index;
	void *ntb_mmio_base_addr;
	uint32_t ntb_topo;
	int ret;

	if (ntb_map_pci_bar(ntb) != 0) {
		SPDK_ERRLOG("ntb_map_pci_bar() failed\n");
		return -1;
	}

	ntb_mmio_base_addr = (uint8_t *)ntb->io_regs;

	ntb->peer_dev_up = 0;
	ntb->hw_addr = (uint8_t *)ntb->io_regs;
	ntb->link_status = NTB_LINK_DOWN;
	ntb->link_speed = NTB_SPEED_NONE;
	ntb->link_width = NTB_WIDTH_NONE;

	ntb->self_mmio = ntb->hw_addr;
	ntb->peer_mmio = ntb->self_mmio + AMD_PEER_OFFSET;
	ntb->int_mask = AMD_EVENT_INTMASK;

	/* Bit 0 of link status register */
	ntb_topo = spdk_mmio_read_4((ntb_mmio_base_addr + AMD_SIDEINFO_OFFSET));

	if (ntb_topo & AMD_SIDE_MASK) {
		ntb->topo = NTB_TOPO_SEC;
	} else {
		ntb->topo = NTB_TOPO_PRI;
	}

	/* memory window, doorbell and scratchpad counts */
	ntb->mw_cnt = AMD_MW_COUNT;
	ntb->active_mw_idx = 0;
	ntb->db_cnt = AMD_DB_COUNT;
	ntb->spad_cnt = AMD_SPADS_COUNT;

	/*
	 * mw_size[] is a pure CPU-side bookkeeping array — it stores the
	 * captured BAR sizes per memory window and is never used as a DMA
	 * source/sink. Plain calloc() is sufficient and avoids burning a
	 * hugepage / IOVA mapping on a few uint64_t.
	 */
	ntb->mw_size = calloc(ntb->mw_cnt, sizeof(uint64_t));
	if (ntb->mw_size == NULL) {
		SPDK_ERRLOG("Cannot allocate memory for mw size.\n");
		return -ENOMEM;
	}
	for (i = 0; i < ntb->mw_cnt; i++) {
		void *mapped_addr;
		uint64_t phys_addr;
		uint64_t bar_size;
		int rc;

		bar_index = ntb_amd_bar[i];

		rc = spdk_pci_device_map_bar(ntb->device, bar_index, &mapped_addr, &phys_addr, &bar_size);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to map PCI bar %d\n", bar_index);
			/*
			 * Unwind any previously-mapped MW BARs so we don't
			 * leak refcounts; the caller of ntb_channel_start()
			 * also calls ntb_unmap_mw_bars() but that's a
			 * defence-in-depth, the primary unwind happens here
			 * close to the failing site.
			 */
			ntb_unmap_mw_bars(ntb);
			return rc;
		}
		ntb->mw_size[i] = bar_size;
		ntb->mw_vaddr[i] = mapped_addr;
		ntb->mw_paddr[i] = phys_addr;
	}

	/* splitting the scratchpad registers to half and assign it to each side */

	ntb->spad_cnt >>= 1;
	if (ntb->topo == NTB_TOPO_PRI) {
		ntb->self_spad = 0;
		ntb->peer_spad = 0x20;
	} else {
		ntb->self_spad = 0x20;
		ntb->peer_spad = 0;
	}

	/*
	 * Reserve the last NTB_SPAD_USER_MAX_NUM scratchpad registers (per
	 * side) for application use. Every entry in spad_user_list[] must
	 * be a valid scratchpad index after this loop, otherwise a later
	 * spad_read/write of e.g. spad_user_list[2] would access a register
	 * past the end of the scratchpad block.
	 */
	if (ntb->spad_cnt < NTB_SPAD_USER_MAX_NUM) {
		SPDK_ERRLOG("Only %u scratchpads available per side, need at least %u\n",
			    ntb->spad_cnt, NTB_SPAD_USER_MAX_NUM);
		return -EINVAL;
	}
	for (j = 0; j < NTB_SPAD_USER_MAX_NUM; j++) {
		ntb->spad_user_list[j] = ntb->spad_cnt - NTB_SPAD_USER_MAX_NUM + j;
	}

	ret = spdk_ntb_amd_set_link(ntb, 1);
	if (ret) {
		return ret;
	}

	/* Init doorbell */
	ntb->db_valid_mask = (1ULL << ntb->db_cnt) - 1;
	/* Clear all valid doorbell bits before registering intr handler */
	spdk_ntb_amd_db_clear(ntb, ntb->db_valid_mask);

	/* Register callback func to eal lib */
	/* TO ADD the poller code here */

	/*
	 * In the AMD NTB doorbell-mask register, a bit *set* disables the
	 * corresponding doorbell-interrupt source. We want all doorbells to
	 * be enabled at startup, so write the mask register directly to 0
	 * and keep the cached copy in sync. We can't reuse spdk_ntb_amd_db_set_mask()
	 * here because it has OR-in semantics (intended for adding bits to
	 * an already-built mask), so calling it with 0 would be a no-op and
	 * silently leave any previously-set bits in place.
	 */
	ntb->db_mask = 0;
	spdk_mmio_write_2(ntb->self_mmio + AMD_DBMASK_OFFSET, 0);

	return 0;
}

int
spdk_ntb_amd_handshake_work(struct spdk_ntb_amd_dev *ntb, void *buffer,
			    size_t buffer_size)
{
	uint64_t buffer_pa;
	uint64_t total_mw_size = 0;
	uint64_t contig_size;
	uint64_t offset;
	int ret, i;

	if (ntb == NULL || buffer == NULL) {
		return -EINVAL;
	}

	/*
	 * Each memory window must be backed by its own non-overlapping slice
	 * of the staging buffer.
	 */
	for (i = 0; i < ntb->mw_cnt; i++) {
		total_mw_size += ntb->mw_size[i];
	}

	if (buffer_size < total_mw_size) {
		SPDK_ERRLOG("Handshake buffer is too small: have %zu bytes, "
			    "need %" PRIu64 " bytes (sum of %u MW sizes)\n",
			    buffer_size, total_mw_size, (unsigned)ntb->mw_cnt);
		return -EINVAL;
	}

	/*
	 * The memory windows are programmed as contiguous slices of one
	 * staging buffer (window i at buffer_pa + sum(mw_size[0..i-1])), so
	 * the whole region must be physically contiguous. Ask spdk_vtophys()
	 * for the length of the contiguous mapping starting at buffer and
	 * reject a buffer that straddles a discontinuity (e.g. spans more than
	 * one hugepage) -- otherwise later windows would be programmed to
	 * unrelated physical memory that the peer would then write into.
	 */
	contig_size = total_mw_size;
	buffer_pa = spdk_vtophys(buffer, &contig_size);
	if (buffer_pa == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Failed to translate handshake buffer to physical address\n");
		return -EINVAL;
	}
	if (contig_size < total_mw_size) {
		SPDK_ERRLOG("Handshake buffer is not physically contiguous: only %" PRIu64
			    " of %" PRIu64 " bytes are contiguous\n", contig_size, total_mw_size);
		return -EINVAL;
	}

	/* Tell the peer how many memory windows we expose. */
	ret = spdk_ntb_amd_spad_write(ntb, SPAD_NUM_MWS, 1, ntb->mw_cnt);
	if (ret < 0) {
		return ret;
	}

	offset = 0;
	for (i = 0; i < ntb->mw_cnt; i++) {
		uint64_t mw_pa = buffer_pa + offset;

		ret = spdk_ntb_amd_mw_set_trans(ntb, i, mw_pa, ntb->mw_size[i]);
		if (ret < 0) {
			return ret;
		}

		offset += ntb->mw_size[i];
	}

	/* Ring doorbell 0 to tell peer the device is ready. */
	ret = spdk_ntb_amd_peer_db_set(ntb, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

/*
 * Memory window address (a particular BAR) -- virtual.
 *
 * Returns 0 on success, negative errno on failure. The mapped virtual
 * address is written to *vaddr_out. We use an out-parameter (rather than
 * stuffing -EINVAL into a uint64_t return) because there is no value in the
 * uint64_t range that is a reliably-invalid mapping address.
 *
 * BARs are mapped once during ntb_channel_start(); this helper just looks
 * up the cached address so callers don't accumulate map_bar refcount leaks.
 */
int
spdk_ntb_amd_get_peer_mw_addr(struct spdk_ntb_amd_dev *ntb, int mw_idx, uint64_t *vaddr_out)
{
	if (ntb == NULL || vaddr_out == NULL) {
		return -EINVAL;
	}
	if (mw_idx < 0 || mw_idx >= ntb->mw_cnt) {
		return -EINVAL;
	}
	if (ntb->mw_vaddr[mw_idx] == NULL) {
		SPDK_ERRLOG("mw %d not mapped\n", mw_idx);
		return -ENODEV;
	}

	*vaddr_out = (uint64_t)(uintptr_t)ntb->mw_vaddr[mw_idx];
	return 0;
}

int
spdk_ntb_amd_get_peer_mw(struct spdk_ntb_amd_dev *ntb, uint8_t mw_idx,
			 struct iovec *iov, struct spdk_memory_domain **domain,
			 void **vaddr)
{
	if (ntb == NULL || iov == NULL) {
		return -EINVAL;
	}
	if (mw_idx >= ntb->mw_cnt) {
		return -EINVAL;
	}
	if (ntb->mw_vaddr[mw_idx] == NULL) {
		SPDK_ERRLOG("mw %u not mapped\n", mw_idx);
		return -ENODEV;
	}

	/*
	 * The engine-facing address of a peer window is its device (bus)
	 * address; the accompanying memory domain tells the DMA engine to use
	 * it directly rather than translating it as host memory.
	 */
	iov->iov_base = (void *)(uintptr_t)ntb->mw_paddr[mw_idx];
	iov->iov_len = ntb->mw_size[mw_idx];

	if (domain != NULL) {
		*domain = ntb->mw_domain;
	}
	if (vaddr != NULL) {
		*vaddr = ntb->mw_vaddr[mw_idx];
	}

	return 0;
}

int
spdk_ntb_amd_dev_start(struct spdk_ntb_amd_dev *ntb)
{
	if (ntb == NULL) {
		return -EINVAL;
	}

	/*
	 * Validate the configured active memory window. The window's mapped
	 * address, bus address and size are looked up on demand through
	 * spdk_ntb_amd_get_peer_mw(); nothing needs to be cached here.
	 */
	if (ntb->active_mw_idx >= ntb->mw_cnt) {
		return -EINVAL;
	}

	return 0;
}

int
spdk_ntb_amd_set_active_mw(struct spdk_ntb_amd_dev *ntb, uint8_t mw_idx)
{
	if (ntb == NULL) {
		return -EINVAL;
	}

	if (mw_idx >= ntb->mw_cnt) {
		return -EINVAL;
	}

	ntb->active_mw_idx = mw_idx;
	return 0;
}

bool
spdk_ntb_amd_peer_ready_get(const struct spdk_ntb_amd_dev *ntb)
{
	if (ntb == NULL) {
		return false;
	}
	return ntb->peer_dev_up != 0;
}

int
spdk_ntb_amd_peer_ready_set(struct spdk_ntb_amd_dev *ntb, bool ready)
{
	if (ntb == NULL) {
		return -EINVAL;
	}
	ntb->peer_dev_up = ready ? 1 : 0;
	return 0;
}

int
spdk_ntb_amd_attr_get(struct spdk_ntb_amd_dev *ntb, const char *attr_name,
		      uint64_t *attr_value)
{
	if (ntb == NULL || attr_name == NULL || attr_value == NULL) {
		SPDK_ERRLOG("Invalid arguments for getting attributes.\n");
		return -EINVAL;
	}

	if (!strncmp(attr_name, SPDK_NTB_TOPO_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->topo;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")\n",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_LINK_STATUS_NAME, NTB_ATTR_NAME_LEN)) {
		/* hw->link_status only indicates hw link status. */
		*attr_value = ntb->link_status || ntb->peer_dev_up;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_SPEED_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->link_speed;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_WIDTH_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->link_width;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_MW_CNT_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->mw_cnt;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_DB_CNT_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->db_cnt;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	if (!strncmp(attr_name, SPDK_NTB_SPAD_CNT_NAME, NTB_ATTR_NAME_LEN)) {
		*attr_value = ntb->spad_cnt;
		SPDK_INFOLOG(ntb, "Attribute (%s) Value (%" PRIu64 ")",
			     attr_name, *attr_value);
		return 0;
	}

	/* Attribute not found. */
	SPDK_ERRLOG("Attribute not found.\n");
	return -EINVAL;
}

/*
 * Scratchpad register read.
 *
 * Returns 0 on success and writes the register value to *value. Uses an
 * out-parameter (rather than returning the value directly) so the error
 * path is unambiguous: a negative errno can never be mistaken for a
 * legitimately-read 0xFFFFFFEA-style scratchpad value.
 */
int
spdk_ntb_amd_spad_read(const struct spdk_ntb_amd_dev *ntb, int spad, bool peer,
		       uint32_t *value)
{
	uint32_t offset;

	if (ntb == NULL || value == NULL) {
		return -EINVAL;
	}
	if (spad < 0 || spad >= ntb->spad_cnt) {
		SPDK_ERRLOG("Invalid scratchpad count.\n");
		return -EINVAL;
	}

	if (peer) {
		offset = ntb->peer_spad;
	} else {
		offset = ntb->self_spad;
	}

	*value = spdk_mmio_read_4((const volatile uint32_t *)(ntb->hw_addr + AMD_SPAD_OFFSET + offset +
				  (spad << 2)));
	return 0;
}

/* scratchpad register write */
int
spdk_ntb_amd_spad_write(const struct spdk_ntb_amd_dev *ntb, int spad,
			bool peer, uint32_t spad_v)
{
	uint32_t offset;

	if (spad < 0 || spad >= ntb->spad_cnt) {
		SPDK_ERRLOG("Invalid scratchpad count. spad %d ntb->spad_cnt %d\n", spad, ntb->spad_cnt);
		return -EINVAL;
	}

	if (peer) {
		offset = ntb->peer_spad + (spad << 2);
	} else {
		offset = ntb->self_spad + (spad << 2);
	}

	spdk_mmio_write_4((volatile uint32_t *)(ntb->hw_addr + AMD_SPAD_OFFSET + offset), spad_v);

	return 0;
}

/*
 * The AMD NTB doorbell, doorbell-mask and doorbell-status registers are all
 * 16 bits wide (one bit per doorbell, AMD_DB_COUNT == 16). The public API
 * uses uint64_t for forward compatibility, but every read/write is masked to
 * the 16 valid bits to avoid touching adjacent register space when callers
 * pass a wider mask by mistake.
 */
#define AMD_NTB_DB_REG_MASK 0xFFFFu

uint64_t
spdk_ntb_amd_db_read(const struct spdk_ntb_amd_dev *ntb)
{
	if (ntb == NULL || ntb->self_mmio == NULL) {
		return 0;
	}

	return (uint64_t)spdk_mmio_read_2(ntb->self_mmio + AMD_DBSTAT_OFFSET);
}

int
spdk_ntb_amd_db_clear(struct spdk_ntb_amd_dev *ntb, uint64_t db_bits)
{
	if (ntb == NULL || ntb->self_mmio == NULL) {
		return -EINVAL;
	}
	if (db_bits & ~(uint64_t)AMD_NTB_DB_REG_MASK) {
		SPDK_ERRLOG("spdk_ntb_amd_db_clear: bits 0x%" PRIx64 " outside 16-bit range\n", db_bits);
		return -EINVAL;
	}

	spdk_mmio_write_2(ntb->self_mmio + AMD_DBSTAT_OFFSET,
			  (uint16_t)(db_bits & AMD_NTB_DB_REG_MASK));

	return 0;
}

int
spdk_ntb_amd_db_set_mask(struct spdk_ntb_amd_dev *ntb, uint64_t db_mask)
{
	if (ntb == NULL || ntb->self_mmio == NULL) {
		return -EINVAL;
	}
	if (db_mask & (~ntb->db_valid_mask)) {
		return -EINVAL;
	}
	ntb->db_mask |= db_mask;

	spdk_mmio_write_2(ntb->self_mmio + AMD_DBMASK_OFFSET,
			  (uint16_t)(ntb->db_mask & AMD_NTB_DB_REG_MASK));
	return 0;
}

int
spdk_ntb_amd_peer_db_set(struct spdk_ntb_amd_dev *ntb, uint8_t db_idx)
{
	if (ntb == NULL || ntb->hw_addr == NULL) {
		return -EINVAL;
	}
	if (db_idx >= ntb->db_cnt) {
		SPDK_ERRLOG("Invalid doorbell index %u (db_cnt %u)\n", db_idx, ntb->db_cnt);
		return -EINVAL;
	}
	if (((uint64_t)1 << db_idx) & ~ntb->db_valid_mask) {
		SPDK_ERRLOG("Invalid doorbell.\n");
		return -EINVAL;
	}

	spdk_mmio_write_2((volatile uint16_t *)(ntb->hw_addr + AMD_DBREQ_OFFSET),
			  (uint16_t)((1u << db_idx) & AMD_NTB_DB_REG_MASK));
	return 0;
}

/* Setting the registers with memory window details (start addresses and sizes) */
int
spdk_ntb_amd_mw_set_trans(struct spdk_ntb_amd_dev *ntb, int mw_idx,
			  uint64_t addr, uint64_t size)
{
	struct spdk_ntb_amd_dev *ntb_hw;
	void *peer_mmio;
	uint32_t bar_index;
	uint64_t reg_val, limit;
	uint64_t mw_size;

	if (ntb == NULL || mw_idx < 0 || mw_idx >= ntb->mw_cnt) {
		return -EINVAL;
	}
	if (ntb->mw_vaddr[mw_idx] == NULL) {
		SPDK_ERRLOG("mw %d not mapped\n", mw_idx);
		return -ENODEV;
	}

	ntb_hw = ntb;

	peer_mmio = ntb_hw->peer_mmio;

	/* Set the limit if supported */
	limit = size;

	bar_index = AMD_MW_IDX << mw_idx;
	mw_size = ntb_hw->mw_size[mw_idx];

	if (size > mw_size) {
		return -EINVAL;
	}

	/* bar = 2 */
	if (bar_index == ntb_amd_bar[0]) {
		spdk_mmio_write_8(peer_mmio + AMD_BAR23_XLAT_OFFSET, addr);
		reg_val = spdk_mmio_read_8(peer_mmio + AMD_BAR23_XLAT_OFFSET);
		if (reg_val != addr) {
			spdk_mmio_write_8(peer_mmio + AMD_BAR23_XLAT_OFFSET, 0);
			return -EIO;
		}

		spdk_mmio_write_8(peer_mmio + AMD_BAR23_LIMIT_OFFSET, limit);
		reg_val = spdk_mmio_read_8(peer_mmio + AMD_BAR23_LIMIT_OFFSET);
		if (reg_val != limit) {
			/* Roll back the translation we just programmed. */
			spdk_mmio_write_8(peer_mmio + AMD_BAR23_XLAT_OFFSET, 0);
			spdk_mmio_write_8(peer_mmio + AMD_BAR23_LIMIT_OFFSET, 0);
			return -EIO;
		}
	} /* bar = 4 */
	else if (bar_index == ntb_amd_bar[1]) {
		spdk_mmio_write_8(peer_mmio + AMD_BAR45_XLAT_OFFSET, addr);
		reg_val = spdk_mmio_read_8(peer_mmio + AMD_BAR45_XLAT_OFFSET);
		if (reg_val != addr) {
			spdk_mmio_write_8(peer_mmio + AMD_BAR45_XLAT_OFFSET, 0);
			return -EIO;
		}

		spdk_mmio_write_8(peer_mmio + AMD_BAR45_LIMIT_OFFSET, limit);
		reg_val = spdk_mmio_read_8(peer_mmio + AMD_BAR45_LIMIT_OFFSET);
		if (reg_val != limit) {
			/* Roll back the translation we just programmed. */
			spdk_mmio_write_8(peer_mmio + AMD_BAR45_XLAT_OFFSET, 0);
			spdk_mmio_write_8(peer_mmio + AMD_BAR45_LIMIT_OFFSET, 0);
			return -EIO;
		}
	}

	return 0;
}

static struct spdk_ntb_amd_dev *
ntb_attach(struct spdk_pci_device *device)
{
	struct spdk_ntb_amd_dev *ntb;
	uint32_t cmd_reg;
	int rc;

	ntb = calloc(1, sizeof(struct spdk_ntb_amd_dev));
	if (ntb == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for ntb device.\n");
		return NULL;
	}

	ntb->device = device;

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(device, &cmd_reg, 4);
	cmd_reg |= 0x4;
	spdk_pci_device_cfg_write32(device, cmd_reg, 4);

	if (ntb_channel_start(ntb) != 0) {
		SPDK_INFOLOG(ntb, "ntb_channel_start() failed\n");
		/*
		 * ntb_channel_start() may have already mapped some of the MW
		 * BARs and/or calloc()'d mw_size[] before failing. Walk the
		 * cached mappings (NULL slots are skipped) and drop the bare
		 * pointers; free(NULL) is a no-op, so unconditional free is
		 * safe.
		 */
		ntb_unmap_mw_bars(ntb);
		ntb_unmap_pci_bar(ntb);
		free(ntb->mw_size);
		free(ntb);
		return NULL;
	}

	/*
	 * Memory domain describing the peer windows. Its mere presence (and
	 * device type) tells a DMA engine that window addresses handed out via
	 * spdk_ntb_amd_get_peer_mw() are already-resolved bus addresses. It is
	 * a descriptor/tag only -- no translation callback is installed, since
	 * the addresses need no further resolution.
	 */
	rc = spdk_memory_domain_create(&ntb->mw_domain, SPDK_DMA_DEVICE_TYPE_DMA,
				       NULL, "ntb_amd");
	if (rc != 0) {
		SPDK_ERRLOG("spdk_memory_domain_create() failed: %d\n", rc);
		ntb_unmap_mw_bars(ntb);
		ntb_unmap_pci_bar(ntb);
		free(ntb->mw_size);
		free(ntb);
		return NULL;
	}

	return ntb;
}

struct ntb_enum_ctx {
	spdk_ntb_amd_probe_cb probe_cb;
	spdk_ntb_amd_attach_cb attach_cb;
	void *cb_ctx;
};

static int
ntb_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct ntb_enum_ctx *enum_ctx = ctx;
	struct spdk_ntb_amd_dev *ntb;

	/* Verify that this device is not already attached */
	TAILQ_FOREACH(ntb, &g_ntb_driver.attached_chans, tailq) {
		/*
		 * NOTE: This assumes that the PCI abstraction layer will use the same device handle
		 *  across enumerations; we could compare by BDF instead if this is not true.
		 */
		if (pci_dev == ntb->device) {
			return 0;
		}
	}

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		/*
		 * Since NTB init is relatively quick, just perform the full init during probing.
		 *  If this turns out to be a bottleneck later, this can be changed to work like
		 *  NVMe with a list of devices to initialize in parallel.
		 */
		ntb = ntb_attach(pci_dev);
		if (ntb == NULL) {
			SPDK_ERRLOG("ntb_attach() failed\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&g_ntb_driver.attached_chans, ntb, tailq);

		enum_ctx->attach_cb(enum_ctx->cb_ctx, pci_dev, ntb);
	}

	return 0;
}

int
spdk_ntb_amd_probe(void *cb_ctx, spdk_ntb_amd_probe_cb probe_cb, spdk_ntb_amd_attach_cb attach_cb)
{
	int rc;
	struct ntb_enum_ctx enum_ctx;

	pthread_mutex_lock(&g_ntb_driver.lock);

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	rc = spdk_pci_enumerate(spdk_pci_ntb_amd_get_driver(), ntb_enum_cb, &enum_ctx);

	pthread_mutex_unlock(&g_ntb_driver.lock);

	return rc;
}

void
spdk_ntb_amd_detach(struct spdk_ntb_amd_dev *ntb)
{
	struct ntb_driver *driver = &g_ntb_driver;

	if (ntb == NULL) {
		return;
	}

	/* ntb should be in the free list (not registered to a thread)
	 * when calling ntb_detach().
	 */
	pthread_mutex_lock(&driver->lock);
	TAILQ_REMOVE(&driver->attached_chans, ntb, tailq);
	pthread_mutex_unlock(&driver->lock);

	/*
	 * Retract what we advertised to the peer before releasing the device.
	 * Clearing AMD_SIDE_READY and the PMM/SMM translation-control bits
	 * disables register-side memory translation, so the peer stops seeing
	 * this endpoint as ready and can no longer reach our host memory.
	 * This must happen while the register BAR is still mapped, i.e. before
	 * ntb_unmap_pci_bar(); merely unmapping BARs only drops this process's
	 * mappings and would leave the inbound translations live on the peer.
	 */
	spdk_ntb_amd_set_link(ntb, false);

	ntb_unmap_mw_bars(ntb);
	ntb_unmap_pci_bar(ntb);
	spdk_pci_device_detach(ntb->device);

	spdk_memory_domain_destroy(ntb->mw_domain);
	free(ntb->mw_size);
	free(ntb);
}

SPDK_LOG_REGISTER_COMPONENT(ntb)
