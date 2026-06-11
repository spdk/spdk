/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#ifndef __NTB_INTERNAL_H__
#define __NTB_INTERNAL_H__

#include "spdk/stdinc.h"
#include "spdk/ntb_amd.h"
#include "spdk/ntb_amd_spec.h"
#include "spdk/queue.h"
#include "spdk/mmio.h"

struct spdk_memory_domain;
/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 */
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))

/* Reserved to app to use. */
#define NTB_SPAD_USER               "spad_user_"
#define NTB_SPAD_USER_LEN           (sizeof(NTB_SPAD_USER) - 1)
#define NTB_SPAD_USER_MAX_NUM       4
#define NTB_ATTR_NAME_LEN           30


/*
 * enum spdk_ntb_topo and enum spdk_ntb_link live in
 * include/spdk/ntb_amd_spec.h because they are part of the on-the-wire
 * handshake (returned to apps via
 * spdk_ntb_amd_attr_get(SPDK_NTB_TOPO_NAME / SPDK_NTB_LINK_STATUS_NAME)).
 */

enum ntb_speed {
	NTB_SPEED_NONE = 0,
	NTB_SPEED_GEN1 = 1,
	NTB_SPEED_GEN2 = 2,
	NTB_SPEED_GEN3 = 3,
	NTB_SPEED_GEN4 = 4,
};

enum ntb_width {
	NTB_WIDTH_NONE = 0,
	NTB_WIDTH_1 = 1,
	NTB_WIDTH_2 = 2,
	NTB_WIDTH_4 = 4,
	NTB_WIDTH_8 = 8,
	NTB_WIDTH_12 = 12,
	NTB_WIDTH_16 = 16,
	NTB_WIDTH_32 = 32,
};

/*
 * enum spdk_ntb_spad_idx (the spad-register layout used for the peer
 * handshake) lives in include/spdk/ntb_amd_spec.h so that applications
 * exchanging the same scratchpad protocol can include it directly.
 */

struct ntb_desc {
	uint64_t addr; /* buffer addr */
	uint16_t len;  /* buffer length */
	uint16_t rsv1;
	uint32_t rsv2;
};

/*
 * Driver-internal helpers (not part of the public ABI). These are kept here
 * so that the .c files in lib/ntb_amd/ and unit tests that include the .c
 * file can use them, but they are not exposed to consumers via the SO map.
 */
int spdk_ntb_amd_set_link(struct spdk_ntb_amd_dev *ntb, bool up);
int spdk_ntb_amd_spad_write(const struct spdk_ntb_amd_dev *ntb, int spad, bool peer,
			    uint32_t spad_v);
int spdk_ntb_amd_db_set_mask(struct spdk_ntb_amd_dev *ntb, uint64_t db_mask);
int spdk_ntb_amd_mw_set_trans(struct spdk_ntb_amd_dev *ntb, int mw_idx, uint64_t addr,
			      uint64_t size);
int spdk_ntb_amd_get_peer_mw_addr(struct spdk_ntb_amd_dev *ntb, int mw_idx, uint64_t *vaddr_out);
int spdk_ntb_amd_read_link_status(struct spdk_pci_device *pci_swus, uint16_t *lnk_word);

struct spdk_ntb_amd_dev {
	/* Opaque handle to upper layer */
	struct spdk_pci_device *device;

	uint8_t mw_cnt;
	uint8_t active_mw_idx;
	uint8_t db_cnt;
	uint8_t spad_cnt;

	uint64_t db_valid_mask;
	uint64_t db_mask;

	void  *self_mmio;
	void  *peer_mmio;
	uint32_t int_mask;
	unsigned int self_spad;
	unsigned int peer_spad;

	uint32_t ctl_status;

	enum spdk_ntb_topo topo;

	enum spdk_ntb_link link_status;
	enum ntb_speed link_speed;
	enum ntb_width link_width;

	char *hw_addr;

	uint8_t peer_dev_up;
	uint64_t *mw_size;

	/*
	 * Cached BAR mappings for each memory window (indexed by mw_idx).
	 * Populated by ntb_channel_start() so that callers like
	 * spdk_ntb_amd_mw_set_trans() don't have to keep re-mapping (and leaking the
	 * refcount in spdk_pci_device_map_bar()) on every register access.
	 * Sized for AMD_MW_COUNT memory windows.
	 */
	void     *mw_vaddr[AMD_MW_COUNT];
	uint64_t  mw_paddr[AMD_MW_COUNT];

	/*
	 * Memory domain describing the peer memory windows exposed by this
	 * device. Handed to callers via spdk_ntb_amd_get_peer_mw() so a DMA
	 * engine (e.g. AE4DMA) knows the window addresses are already-resolved
	 * device (IO) bus addresses rather than host virtual addresses.
	 */
	struct spdk_memory_domain *mw_domain;

	/* Reserve several spad for app to use. */
	int spad_user_list[NTB_SPAD_USER_MAX_NUM];

	/* I/O area used for device communication */
	void *io_regs;

	/* tailq entry for attached_chans */
	TAILQ_ENTRY(spdk_ntb_amd_dev) tailq;
};

#endif /* __NTB_INTERNAL_H__ */
